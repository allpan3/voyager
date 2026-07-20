// CIM element adapter around the CIM macro wrapper
//
// This module adds two pieces of behavior around the macro wrapper:
//   1. CIM element-level A_WIDTH can be wider than the macro wrapper's BASE_A_WIDTH.
//      The element sends A to the macro wrapper MSB slice first, then lower slices, and
//      combines the returned results in an external accumulator.
//   2. CIM element-level B_WIDTH can be wider than the macro wrapper's BASE_B_WIDTH.
//      The element splits each B value across multiple macro wrapper output channels, then
//      combines the partial results from those channels in the external accumulator.
//
// Workload geometry:
//
//                         N
//                   +-----------+
//               K   |  B[K][N]  |
//                   +-----------+
//       K                 N
//   +---------+       +-----------+
// M | A[M][K] |  x  = | C[M][N]  |
//   +---------+       +-----------+
//
// M is temporal: one issue consumes A[m][0:K] and produces C[m][0:N]
// BK is a fraction of K in a B block write, so b[bk][n] writes B[wchi + bk][n]
// The macro mapping is CH_IN=K, WRITE_CH_IN=BK, and CH_OUT=N*NUM_B_SLICES

`include "cim_typedefs.svh"

module CIMIntElement #(
    parameter int unsigned CH_IN = 64,
    parameter int unsigned CH_OUT = 8,
    parameter int unsigned B_SETS = 18,

    // BASE_* parameters describe the base shape of the macro wrapper
    // In bit-serial mode, the macro wrapper A width may be widened up to BASE_C_WIDTH capacity.
    parameter int unsigned BASE_A_WIDTH = 4,
    parameter int unsigned BASE_B_WIDTH = 4,
    parameter int unsigned BASE_C_WIDTH = 20,
    parameter int unsigned WRITE_CH_IN = 1,
    parameter int unsigned MAC_LATENCY = 1,
    parameter cim_mode_t MODE = CIM_MODE_BIT_SERIAL,
    parameter cim_macro_wrapper_impl_t MACRO_IMPL = CIM_MACRO_WRAPPER_IMPL_MODEL,

    // A_WIDTH and B_WIDTH are the logical operand widths implemented by this element
    // SIGNED applies to both logical A and logical B at design time
    parameter int unsigned A_WIDTH = 8,
    parameter int unsigned B_WIDTH = 8,
    parameter bit SIGNED = 1'b0,

    localparam int unsigned K = CH_IN,
    localparam int unsigned BK = WRITE_CH_IN,
    localparam int unsigned SUM_GUARD_WIDTH = (K <= 1) ? 1 : $clog2(K),

    // One B value is split across NUM_B_SLICES physical macro wrapper output channels
    localparam int unsigned NUM_B_SLICES = B_WIDTH / BASE_B_WIDTH,
    localparam int unsigned N = CH_OUT / NUM_B_SLICES,

    // C_WIDTH is the result width depending on the operand widths and the vector length
    localparam int unsigned C_WIDTH = A_WIDTH + B_WIDTH + SUM_GUARD_WIDTH,
    localparam int unsigned BITS_K = (K <= 1) ? 1 : $clog2(K),
    localparam int unsigned BITS_SET = (B_SETS <= 1) ? 1 : $clog2(B_SETS)
) (
    input  logic                       wclk,
    input  logic                       mclk,
    input  logic                       rstn,

    // Logical A operand is one temporal row with K values. a and mset must remain
    // stable from an accepted issue until mac_ready returns high (the issue window)
    input  logic [A_WIDTH-1:0]         a [K],

    // One B write provides BK consecutive rows for all N columns
    input  logic [B_WIDTH-1:0]         b [BK][N],
    input  logic                       wen,
    input  logic [BITS_K-1:0]          wchi,
    input  logic [BITS_SET-1:0]        wset,

    // mac_issue pulses to begin a MAC when mac_ready is high; issues while not
    // ready are ignored so the reservation must be made by the producer
    input  logic                       mac_issue,
    input  logic [BITS_SET-1:0]        mset,

    output logic [C_WIDTH-1:0]         c [N],  // registered result, stable until the next retire
    output logic                       c_retire,   // toggles once per retired result
    output logic                       mac_ready   // high when an issue presented this cycle is accepted
);

  // CIM element walks operand A slice by slice. The width of a slice depends on the selected macro wrapper mode:
  // For bit-parallel, the slice is the fixed BASE_A_WIDTH fed into the macro wrapper. For bit-serial, the internal
  // accumulator width may allow the macro wrapper to process a wider slice than BASE_A_WIDTH, so it performs an
  // additional "intra-slice" (or window) walking.
  // We refer the base width the macro wrapper processes at a time as a "window", and the max width the macro wrapper
  // can process a "slice". A slice contains one or more window.

  // ---------------------------------------------------------------------------
  // Slice Walking
  // ---------------------------------------------------------------------------

  logic start_mac;
  // The final A slice result reaches the accumulator on the retire edge
  logic retire_op;

  // Max operand A width supported by the macro wrapper in serial mode
  localparam int unsigned SERIAL_MAX_SLICE_WIDTH = BASE_C_WIDTH - BASE_B_WIDTH - SUM_GUARD_WIDTH;

  // The slice width may just be A_WIDTH if the A_WIDTH is smaller than what the macro wrapper can handle
  localparam int unsigned SERIAL_SLICE_WIDTH = (A_WIDTH < SERIAL_MAX_SLICE_WIDTH) ? A_WIDTH : SERIAL_MAX_SLICE_WIDTH;
  localparam int unsigned SLICE_WIDTH = (MODE == CIM_MODE_BIT_SERIAL) ? SERIAL_SLICE_WIDTH : BASE_A_WIDTH;
  // Number of A slices needed for A_WIDTH; + SLICE_WIDTH - 1 implements ceiling division to cover any partial final slice
  localparam int unsigned NUM_SLICES = (A_WIDTH + SLICE_WIDTH - 1) / SLICE_WIDTH;
  localparam int unsigned BITS_SLICE = (NUM_SLICES <= 1) ? 1 : $clog2(NUM_SLICES);

  // SLICE_LAUNCH_INTERVAL is the number of cycles between launching slices into the macro wrapper
  // Bit-parallel consumes a slice in one cycle; bit-serial consumes one bit per cycle, padded to whole windows
  localparam int unsigned SERIAL_SLICE_INTERVAL = (SERIAL_SLICE_WIDTH + BASE_A_WIDTH - 1) / BASE_A_WIDTH * BASE_A_WIDTH;
  localparam int unsigned SLICE_LAUNCH_INTERVAL = (MODE == CIM_MODE_BIT_SERIAL) ? SERIAL_SLICE_INTERVAL : 1;
  localparam int unsigned BITS_SLICE_LAUNCH_INTERVAL =
    (SLICE_LAUNCH_INTERVAL <= 1) ? 1 : $clog2(SLICE_LAUNCH_INTERVAL + 1);
  // The actual MAC latency for a slice; for bit-serial, including the tail latency for completing the last bit of the slice
  localparam int unsigned SLICE_MAC_CYCLES = SLICE_LAUNCH_INTERVAL + MAC_LATENCY - 1;

  // Slice driven into the window walker this cycle. Slice 0 contains the MSB bits of operand A.
  logic [BITS_SLICE-1:0] issue_a_slice_idx;
  // Next slice that can be issued once the current slice is retiring
  logic [BITS_SLICE-1:0] next_a_slice_idx;
  // All slices have entered the macro wrapper; results may still be retiring through the macro wrapper pipeline
  logic issued_all_slices;
  // Cycle counter for the current slice; it is set to 1 because the launch cycle already feeds the macro wrapper
  logic [BITS_SLICE_LAUNCH_INTERVAL-1:0] slice_cycle;
  // Registered slice walk state after the launch cycle
  logic slice_walk_active;
  assign slice_walk_active = (slice_cycle != '0);

  // The issue window closes once the final slice has fed its last window; the
  // retire pipeline may still be draining while a new issue is accepted
  logic issue_window_open;
  assign issue_window_open = slice_walk_active &&
    !(issued_all_slices && (slice_cycle == BITS_SLICE_LAUNCH_INTERVAL'(SLICE_LAUNCH_INTERVAL)));
  assign mac_ready = rstn && !issue_window_open;
  assign start_mac = mac_issue && mac_ready;

  // issue_slice marks the first cycle of a slice
  logic issue_slice;
  assign issue_slice = start_mac || (rstn && !issued_all_slices &&
    (slice_cycle == BITS_SLICE_LAUNCH_INTERVAL'(SLICE_LAUNCH_INTERVAL)));

  always_comb begin
    // eagerly update the issue slice index
    issue_a_slice_idx = next_a_slice_idx;

    // A new mac operation issues the first slice
    if (start_mac) begin
      issue_a_slice_idx = '0;
    end
  end

  always_ff @(posedge mclk or negedge rstn) begin
    if (!rstn) begin
      slice_cycle <= '0;
    end else if (issue_slice) begin
      slice_cycle <= BITS_SLICE_LAUNCH_INTERVAL'(1);
    end else if (slice_walk_active && (slice_cycle < BITS_SLICE_LAUNCH_INTERVAL'(SLICE_LAUNCH_INTERVAL))) begin
      slice_cycle <= slice_cycle + BITS_SLICE_LAUNCH_INTERVAL'(1);
    end
  end

  always_ff @(posedge mclk or negedge rstn) begin
    if (!rstn) begin
      next_a_slice_idx <= '0;
      issued_all_slices <= 1'b0;
    end else if (issue_slice) begin
      if (issue_a_slice_idx == BITS_SLICE'(NUM_SLICES - 1)) begin
        next_a_slice_idx <= '0;
        issued_all_slices <= 1'b1;
      end else begin
        next_a_slice_idx <= issue_a_slice_idx + BITS_SLICE'(1);
        issued_all_slices <= 1'b0;
      end
    end
  end

  // first marks the signed/MSB A slice, and last marks completion of the element op.
  logic slice_valid_pipe [SLICE_MAC_CYCLES];
  logic slice_is_first_pipe [SLICE_MAC_CYCLES];
  logic slice_is_last_pipe [SLICE_MAC_CYCLES];

  always_ff @(posedge mclk or negedge rstn) begin
    if (!rstn) begin
      for (int stage = 0; stage < SLICE_MAC_CYCLES; stage++) begin
        slice_valid_pipe[stage] <= 1'b0;
        slice_is_first_pipe[stage] <= 1'b0;
        slice_is_last_pipe[stage] <= 1'b0;
      end
    end else begin
      slice_valid_pipe[0] <= issue_slice;
      slice_is_first_pipe[0] <= issue_slice && (issue_a_slice_idx == '0);
      slice_is_last_pipe[0] <= issue_slice && (issue_a_slice_idx == BITS_SLICE'(NUM_SLICES - 1));
      for (int stage = 1; stage < SLICE_MAC_CYCLES; stage++) begin
        slice_valid_pipe[stage] <= slice_valid_pipe[stage-1];
        slice_is_first_pipe[stage] <= slice_is_first_pipe[stage-1];
        slice_is_last_pipe[stage] <= slice_is_last_pipe[stage-1];
      end
    end
  end

  // Indicates the current slice has reached the external accumulator.
  logic slice_result_ready;
  assign slice_result_ready = slice_valid_pipe[SLICE_MAC_CYCLES-1];

  // Indicates the current retiring slice is the first/final A slice.
  logic retiring_first_a_slice, retiring_last_a_slice;
  assign retiring_first_a_slice = slice_is_first_pipe[SLICE_MAC_CYCLES-1];
  assign retiring_last_a_slice = slice_is_last_pipe[SLICE_MAC_CYCLES-1];

  // The final slice result is written to the output stage on the retire edge
  assign retire_op = slice_result_ready && retiring_last_a_slice;

  // ---------------------------------------------------------------------------
  // Window Walking
  // ---------------------------------------------------------------------------

  // Number of windows in a slice
  localparam int unsigned A_WINDOWS_PER_SLICE = (SLICE_WIDTH + BASE_A_WIDTH - 1) / BASE_A_WIDTH;
  localparam int unsigned BITS_A_WINDOW = (A_WINDOWS_PER_SLICE <= 1) ? 1 : $clog2(A_WINDOWS_PER_SLICE);

  // Active slice stays registered after launch because the issue index immediately advances
  logic [BITS_SLICE-1:0] a_window_slice_idx;
  logic [BITS_SLICE-1:0] macro_wrapper_a_slice_idx;
  assign macro_wrapper_a_slice_idx = issue_slice ? issue_a_slice_idx : a_window_slice_idx;

  always_ff @(posedge mclk or negedge rstn) begin
    if (!rstn) begin
      a_window_slice_idx <= '0;
    end else if (issue_slice) begin
      a_window_slice_idx <= issue_a_slice_idx;
    end
  end

  // The mac signal of the macro wrapper is asserted for SLICE_LAUNCH_INTERVAL cycles so that all bits are fed to the wrapper
  // This does not include the tail latency for the MAC op to finish
  logic macro_wrapper_mac;
  assign macro_wrapper_mac = rstn && (issue_slice ||
    (slice_walk_active && (slice_cycle < BITS_SLICE_LAUNCH_INTERVAL'(SLICE_LAUNCH_INTERVAL))));

  // Each window remains stable while the serial macro wrapper consumes its BASE_A_WIDTH bits
  logic [BITS_A_WINDOW-1:0] issue_a_window_idx;

  always_comb begin
    issue_a_window_idx = '0;
    if (macro_wrapper_mac) begin
        if (issue_slice)
          issue_a_window_idx = '0;
        else
          issue_a_window_idx = BITS_A_WINDOW'(slice_cycle / BASE_A_WIDTH);
    end
  end

  logic macro_wrapper_a_signed;
  assign macro_wrapper_a_signed = SIGNED && (macro_wrapper_a_slice_idx == '0) && (issue_a_window_idx == '0);

  logic macro_wrapper_init;
  assign macro_wrapper_init = (MODE == CIM_MODE_BIT_SERIAL) ? issue_slice : macro_wrapper_mac;

  // ---------------------------------------------------------------------------
  // Macro-Wrapper-Facing Data Buses
  // ---------------------------------------------------------------------------

  // Repack A/B operands into the fixed macro wrapper shape. B signedness is also
  // expanded here because only the MSB physical B slice should be signed. These
  // are declared ahead of the functions/always_comb below because reduce_b_slices
  // reads macro_wrapper_c; VCS rejects referencing a module variable declared
  // later in the module (Verilator accepts the forward reference).
  logic [BASE_A_WIDTH-1:0] macro_wrapper_a [CH_IN];
  logic [BASE_B_WIDTH-1:0] macro_wrapper_b [WRITE_CH_IN][CH_OUT];
  logic macro_wrapper_b_signed [CH_OUT];
  logic [BASE_C_WIDTH-1:0] macro_wrapper_c [CH_OUT];

  // Select one macro-wrapper-width A window from a logical A slice
  function automatic logic [BASE_A_WIDTH-1:0] select_a_window(
      input logic [A_WIDTH-1:0] a,
      input logic [BITS_SLICE-1:0] slice_idx,
      input logic [BITS_A_WINDOW-1:0] window_idx
  );
    int unsigned slice_lower_bit;
    int unsigned window_bit_offset;
    int unsigned slice_bit_idx;
    int unsigned a_bit_idx;
    logic sign_bit;
    begin
      // Slice numbering is MSB first. The expression below finds the LSB of the slice in the A operand
      slice_lower_bit = (NUM_SLICES - 1 - int'(slice_idx)) * SLICE_WIDTH;

      // Similarly, find the LSB of the window in the slice
      window_bit_offset = (A_WINDOWS_PER_SLICE - 1 - int'(window_idx)) * BASE_A_WIDTH;

      // Pre-fill with sign bits so a narrow MSB slice is sign- or zero-extended
      sign_bit = SIGNED && (slice_idx == 0) && a[A_WIDTH - 1];
      select_a_window = {BASE_A_WIDTH{sign_bit}};

      // Copy only the real bits in this slice; any remaining high bits keep
      // the sign/zero fill from above.
      for (int bit_idx = 0; bit_idx < BASE_A_WIDTH; bit_idx++) begin
        slice_bit_idx = window_bit_offset + bit_idx;
        a_bit_idx = slice_lower_bit + slice_bit_idx;
        if ((slice_bit_idx < SLICE_WIDTH) && (a_bit_idx < A_WIDTH)) begin
          select_a_window[bit_idx] = a[a_bit_idx];
        end
      end
    end
  endfunction

  function automatic logic [BASE_B_WIDTH-1:0] select_b_slice(
      input logic [B_WIDTH-1:0] b,
      input int unsigned slice_idx
  );
    int unsigned lower_bit;
    begin
      lower_bit = (NUM_B_SLICES - 1 - slice_idx) * BASE_B_WIDTH;
      select_b_slice = b[lower_bit +: BASE_B_WIDTH];
    end
  endfunction

  function automatic logic [C_WIDTH-1:0] extend_macro_wrapper_c(
      input logic [BASE_C_WIDTH-1:0] value,
      input logic result_is_signed
  );
    logic signed [BASE_C_WIDTH-1:0] signed_value;
    begin
      signed_value = value;
      extend_macro_wrapper_c = result_is_signed ? C_WIDTH'(signed_value) : C_WIDTH'(value);
    end
  endfunction

  function automatic logic [C_WIDTH-1:0] reduce_b_slices(
      input int unsigned n
  );
    int unsigned phys_cho;
    int unsigned b_shift;
    logic result_is_signed;
    begin
      reduce_b_slices = '0;
      for (int b_slice = 0; b_slice < NUM_B_SLICES; b_slice++) begin
        phys_cho = n * NUM_B_SLICES + b_slice;
        b_shift = (NUM_B_SLICES - 1 - b_slice) * BASE_B_WIDTH;
        result_is_signed = SIGNED && (retiring_first_a_slice || (b_slice == 0));
        reduce_b_slices += extend_macro_wrapper_c(macro_wrapper_c[phys_cho], result_is_signed) << b_shift;
      end
    end
  endfunction

  always_comb begin
    for (int k = 0; k < K; k++) begin
      macro_wrapper_a[k] = select_a_window(a[k], macro_wrapper_a_slice_idx, issue_a_window_idx);
    end
  end

  // b (logical):
  //       n0           n1           n2           n3
  // bk0   b[0][0]      b[0][1]      b[0][2]      b[0][3]
  // bk1   b[1][0]      b[1][1]      b[1][2]      b[1][3]
  //
  // macro_wrapper_b (physical):
  //       cho0         cho1         cho2         cho3         cho4         cho5         cho6         cho7
  // chi0  b[0][0].sl0  b[0][0].sl1  b[0][1].sl0  b[0][1].sl1  b[0][2].sl0  b[0][2].sl1  b[0][3].sl0  b[0][3].sl1
  // chi1  b[1][0].sl0  b[1][0].sl1  b[1][1].sl0  b[1][1].sl1  b[1][2].sl0  b[1][2].sl1  b[1][3].sl0  b[1][3].sl1
  always_comb begin
    for (int cho = 0; cho < CH_OUT; cho++) begin
      int unsigned n;
      int unsigned slice_idx;

      n = cho / NUM_B_SLICES;
      slice_idx = cho % NUM_B_SLICES;
      // Only the MSB slice needs to be signed
      macro_wrapper_b_signed[cho] = SIGNED && (slice_idx == 0);

      // Position within the current BK-wide write block
      for (int bk = 0; bk < BK; bk++) begin
        macro_wrapper_b[bk][cho] = select_b_slice(b[bk][n], slice_idx);
      end
    end
  end


  // ---------------------------------------------------------------------------
  // CIM Macro Wrapper
  // ---------------------------------------------------------------------------

  CIMIntMacroWrapper #(
      .CH_IN(CH_IN),
      .CH_OUT(CH_OUT),
      .B_SETS(B_SETS),
      .A_WIDTH(BASE_A_WIDTH),
      .B_WIDTH(BASE_B_WIDTH),
      .C_WIDTH(BASE_C_WIDTH),
      .WRITE_CH_IN(WRITE_CH_IN),
      .MAC_LATENCY(MAC_LATENCY),
      .MODE(MODE),
      .IMPL(MACRO_IMPL)
  ) macro_wrapper (
      .wclk(wclk),
      .mclk(mclk),
      .a(macro_wrapper_a),
      .b(macro_wrapper_b),
      .wen(wen),
      .mac(macro_wrapper_mac),
      .init(macro_wrapper_init),
      .a_signed(macro_wrapper_a_signed),
      .b_signed(macro_wrapper_b_signed),
      .wchi(wchi),
      .wset(wset),
      .mset(mset),
      .c(macro_wrapper_c)
  );

  // ---------------------------------------------------------------------------
  // External Accumulator and Outputs
  // ---------------------------------------------------------------------------

  // Hold partial results while the element walks across multiple A slices
  logic [C_WIDTH-1:0] acc [N];
  // Next accumulator value shared by the accumulator and the retire capture
  logic [C_WIDTH-1:0] acc_next [N];
  // Registered output stage keeps a retired result stable while the next op accumulates
  logic [C_WIDTH-1:0] c_out [N];
  assign c = c_out;

  always_comb begin
    for (int n = 0; n < N; n++) begin
      // The first retiring slice restarts the accumulation, so back-to-back ops need no clear
      acc_next[n] = (retiring_first_a_slice ? {C_WIDTH{1'b0}} : (acc[n] << SLICE_WIDTH)) + reduce_b_slices(n);
    end
  end

  always_ff @(posedge mclk or negedge rstn) begin
    if (!rstn) begin
      c_retire <= 1'b0;
      for (int n = 0; n < N; n++) begin
        acc[n] <= '0;
        c_out[n] <= '0;
      end
    end else if (slice_result_ready) begin
      for (int n = 0; n < N; n++) begin
        acc[n] <= acc_next[n];
      end
      if (retire_op) begin
        c_retire <= !c_retire;
        for (int n = 0; n < N; n++) begin
          c_out[n] <= acc_next[n];
        end
      end
    end
  end

  // ---------------------------------------------------------------------------
  // Static Parameter Checks
  // ---------------------------------------------------------------------------
  // Check static element parameters during elaboration
  generate
    if (A_WIDTH == 0) begin : gen_invalid_a_width
      $fatal(1, "CIMIntElement: A_WIDTH must be positive");
    end
    if (B_WIDTH == 0) begin : gen_invalid_b_width
      $fatal(1, "CIMIntElement: B_WIDTH must be positive");
    end
    if (BASE_B_WIDTH != 0) begin : gen_check_base_b_width
      if ((B_WIDTH % BASE_B_WIDTH) != 0) begin : gen_invalid_b_width_base_b_width
        $fatal(1, "CIMIntElement: B_WIDTH must be a multiple of BASE_B_WIDTH");
      end
    end
    if (NUM_B_SLICES == 0) begin : gen_invalid_num_b_slices
      $fatal(1, "CIMIntElement: B_WIDTH must be at least BASE_B_WIDTH");
    end
    if (NUM_B_SLICES != 0) begin : gen_check_num_b_slices
      if ((CH_OUT % NUM_B_SLICES) != 0) begin : gen_invalid_ch_out_num_b_slices
        $fatal(1, "CIMIntElement: CH_OUT must be divisible by NUM_B_SLICES for wider B grouping");
      end
    end
    if (SLICE_WIDTH == 0) begin : gen_invalid_slice_width
      $fatal(1, "CIMIntElement: selected macro wrapper A slice width must be positive");
    end
  endgenerate

endmodule

// CIMIntElementPacked adapts Catapult-friendly packed buses to the native array RTL
module CIMIntElementPacked #(
    parameter int unsigned CH_IN = 64,
    parameter int unsigned CH_OUT = 8,
    parameter int unsigned B_SETS = 18,

    // BASE_* parameters describe the base shape of the macro wrapper
    parameter int unsigned BASE_A_WIDTH = 4,
    parameter int unsigned BASE_B_WIDTH = 4,
    parameter int unsigned BASE_C_WIDTH = 20,
    parameter int unsigned WRITE_CH_IN = 1,
    parameter int unsigned MAC_LATENCY = 1,
    parameter cim_mode_t MODE = CIM_MODE_BIT_SERIAL,
    parameter cim_macro_wrapper_impl_t MACRO_IMPL = CIM_MACRO_WRAPPER_IMPL_MODEL,

    // A_WIDTH and B_WIDTH are the logical operand widths implemented by this element
    parameter int unsigned A_WIDTH = 8,
    parameter int unsigned B_WIDTH = 8,
    parameter bit SIGNED = 1'b0,

    localparam int unsigned K = CH_IN,
    localparam int unsigned BK = WRITE_CH_IN,
    localparam int unsigned SUM_GUARD_WIDTH = (K <= 1) ? 1 : $clog2(K),
    localparam int unsigned NUM_B_SLICES = B_WIDTH / BASE_B_WIDTH,
    localparam int unsigned N = CH_OUT / NUM_B_SLICES,
    localparam int unsigned C_WIDTH = A_WIDTH + B_WIDTH + SUM_GUARD_WIDTH,
    localparam int unsigned BITS_K = (K <= 1) ? 1 : $clog2(K),
    localparam int unsigned BITS_SET = (B_SETS <= 1) ? 1 : $clog2(B_SETS),
    localparam int unsigned A_BUS_WIDTH = K * A_WIDTH,
    localparam int unsigned B_BUS_WIDTH = N * BK * B_WIDTH,
    localparam int unsigned C_BUS_WIDTH = N * C_WIDTH
) (
    input  logic                         wclk,
    input  logic                         mclk,
    input  logic                         rstn,

    input  logic [A_BUS_WIDTH-1:0]       a_bus,
    input  logic [B_BUS_WIDTH-1:0]       b_bus,
    input  logic                         wen,
    input  logic [BITS_K-1:0]            wchi,
    input  logic [BITS_SET-1:0]          wset,

    input  logic                         mac_issue,
    input  logic [BITS_SET-1:0]          mset,

    output logic [C_BUS_WIDTH-1:0]       c_bus,
    output logic                         c_retire,
    output logic                         mac_ready
);

  logic [A_WIDTH-1:0] a [K];
  logic [B_WIDTH-1:0] b [BK][N];
  logic [C_WIDTH-1:0] c [N];

  always_comb begin
    for (int k = 0; k < K; k++) begin
      a[k] = a_bus[k * A_WIDTH +: A_WIDTH];
    end

    for (int bk = 0; bk < BK; bk++) begin
      for (int n = 0; n < N; n++) begin
        b[bk][n] = b_bus[((bk * N) + n) * B_WIDTH +: B_WIDTH];
      end
    end

    for (int n = 0; n < N; n++) begin
      c_bus[n * C_WIDTH +: C_WIDTH] = c[n];
    end
  end

  CIMIntElement #(
      .CH_IN(CH_IN),
      .CH_OUT(CH_OUT),
      .B_SETS(B_SETS),
      .BASE_A_WIDTH(BASE_A_WIDTH),
      .BASE_B_WIDTH(BASE_B_WIDTH),
      .BASE_C_WIDTH(BASE_C_WIDTH),
      .WRITE_CH_IN(WRITE_CH_IN),
      .MAC_LATENCY(MAC_LATENCY),
      .MODE(MODE),
      .MACRO_IMPL(MACRO_IMPL),
      .A_WIDTH(A_WIDTH),
      .B_WIDTH(B_WIDTH),
      .SIGNED(SIGNED)
  ) core (
      .wclk(wclk),
      .mclk(mclk),
      .rstn(rstn),
      .a(a),
      .b(b),
      .wen(wen),
      .wchi(wchi),
      .wset(wset),
      .mac_issue(mac_issue),
      .mset(mset),
      .c(c),
      .c_retire(c_retire),
      .mac_ready(mac_ready)
  );

endmodule
