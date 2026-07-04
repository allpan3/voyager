// Behavioral CIM macro model for CIMIntMacroWrapper
`include "cim_typedefs.svh"

// CIMIntMacroModel stores B rows and computes bit-parallel or bit-serial MAC results against A
module CIMIntMacroModel #(
    parameter int unsigned CH_IN = 64,
    parameter int unsigned CH_OUT = 8,
    parameter int unsigned NUM_ROWS = 18,
    parameter int unsigned A_WIDTH = 4,              // Bit-width of A operand (the streaming operand)
    parameter int unsigned B_WIDTH = 4,              // Base bit-width of B operand (the stored operand)
    parameter int unsigned C_WIDTH = 20,             // Bit-width of the output result; This determines the max supported precision for A
    parameter int unsigned WRITE_CH_IN = 1,             // Number of input channels written per cycle when writing B
    parameter int unsigned MAC_LATENCY = 1,          // Latency to produce C; for bit-serial, this is the latency for a single bit's MAC, the total latency would be this plus A_WIDTH-1
    parameter cim_mode_t MODE = CIM_MODE_BIT_SERIAL, // Select bit-parallel or bit-serial MAC behavior
    localparam int unsigned BITS_CH_IN = (CH_IN <= 1) ? 1 : $clog2(CH_IN),  // Number of bits needed to index the B operand when writing; note when WRITE_CH_IN > 1, continguous addresses are written
    localparam int unsigned BITS_ROW  = (NUM_ROWS <= 1) ? 1 : $clog2(NUM_ROWS)               // Number of bits needed to address the rows
) (
    input  logic                  wclk,
    input  logic                  mclk,
    input  logic [A_WIDTH-1:0]    a [CH_IN],               // Activation operand sampled directly while mac is high
    input  logic [B_WIDTH-1:0]    b [CH_OUT][WRITE_CH_IN],    // Stored operand to be written
    input  logic                  wen,
    input  logic                  mac,                     // Signals the start of an MAC op, no need to stay high for the mac pipeline; for bit-serial, this performs one-bit mac; for bit-parallel, this performs an A_WIDTHxB_WIDTH mac
    input  logic                  init,                    // Serial: marks the first partial result when mac is high; ignored by bit-parallel
    input  logic                  a_signed,
    input  logic                  b_signed [CH_OUT],       // Each output channel can have different signedness, which is needed to support wider signed B computation
    input  logic [BITS_CH_IN-1:0] widx,                    // Address to select the slot (bank) for writing b
    input  logic [BITS_ROW-1:0]   wrow,                    // Address to select the row for writing b
    input  logic [BITS_ROW-1:0]   mrow,                    // Address to select the row for MAC computation
    output logic [C_WIDTH-1:0]    c [CH_OUT]
);
`ifndef SYNTHESIS
  import CIMExceptionPkg::*;
`endif

  // Stored B data
  logic [B_WIDTH-1:0] b_mem [NUM_ROWS][CH_OUT][CH_IN];

  // -- Write logic for B
  always_ff @(posedge wclk) begin
    if (wen) begin
      for (int cho = 0; cho < CH_OUT; cho++) begin : chan_out
        for (int lane = 0; lane < WRITE_CH_IN; lane++) begin : chan_in
          b_mem[wrow][cho][widx + BITS_CH_IN'(lane)] <= b[cho][lane];
        end
      end
    end
  end

  // Row of operand B selected for the current MAC operation
  logic [B_WIDTH-1:0] b_mac [CH_OUT][CH_IN];
  assign b_mac = b_mem[mrow];

`ifndef SYNTHESIS
  // Report model protocol violations as fatal unless a collection test asks to log and keep going
  task automatic report_model_violation(input string exception_type, input string message);
    begin
      if ($test$plusargs("collect_exception_types")) begin
        collect_exception(exception_type, message);
      end else begin
        $fatal(1, "%s", message);
      end
    end
  endtask

  // Simulation checker for row-level write/MAC exclusion in the behavioral model
  always @(posedge wclk) begin
    if (wen && mac && (wrow == mrow)) begin
      report_model_violation(EXCEPTION_TYPE_ROW_WRITE_MAC_COLLISION, $sformatf("CIMIntMacroModel row protocol violation: write and MAC target row %0d while both enables are high", wrow));
    end
  end
`endif

  // -- MAC logic
  generate
    if (MODE == CIM_MODE_BIT_PARALLEL) begin : gen_bit_parallel

      // Keep one extra bit so unsigned*unsigned and mixed signedness products
      // can share one signed internal representation without losing the top bit.
      localparam int unsigned MAC_RES_WIDTH = A_WIDTH + B_WIDTH + $clog2(CH_IN) + 1;

      function automatic logic [MAC_RES_WIDTH-1:0] mac_product(
          input logic [A_WIDTH-1:0] a_val,
          input logic [B_WIDTH-1:0] b_val,
          input logic b_is_signed
      );
        logic signed [A_WIDTH-1:0] a_signed_value;
        logic signed [B_WIDTH-1:0] b_signed_value;
        logic signed [MAC_RES_WIDTH-1:0] a_ext;
        logic signed [MAC_RES_WIDTH-1:0] b_ext;
        logic signed [MAC_RES_WIDTH-1:0] product;
        begin
          a_signed_value = a_val;
          b_signed_value = b_val;
          // Extend each operand according to its own signedness before the multiply.
          // In the mixed signed/unsigned case, the signed operand must keep
          // its sign bit while the unsigned operand must zero-extend so its
          // MSB is not interpreted as a sign.
          a_ext = a_signed ? MAC_RES_WIDTH'(a_signed_value) : MAC_RES_WIDTH'(a_val);
          b_ext = b_is_signed ? MAC_RES_WIDTH'(b_signed_value) : MAC_RES_WIDTH'(b_val);
          product = a_ext * b_ext;
          mac_product = product;
        end
      endfunction

      function automatic logic [C_WIDTH-1:0] extend_mac_result(
          input logic [MAC_RES_WIDTH-1:0] value,
          input logic result_is_signed
      );
        logic signed [MAC_RES_WIDTH-1:0] value_signed;
        begin
          value_signed = value;
          extend_mac_result = result_is_signed ? C_WIDTH'(value_signed) : C_WIDTH'(value);
        end
      endfunction

      logic [MAC_RES_WIDTH-1:0] mac_res [CH_OUT];
      // Compute MAC
      always_comb begin
        for (int cho = 0; cho < CH_OUT; cho++) begin : mac_ch_out
          mac_res[cho] = '0;
          for (int chi = 0; chi < CH_IN; chi++) begin : mac_ch_in
            mac_res[cho] += mac_product(a[chi], b_mac[cho][chi], b_signed[cho]);
          end
        end
      end

      logic [MAC_RES_WIDTH-1:0] mac_pipe [MAC_LATENCY][CH_OUT];
      // Signedness is sampled with each issued MAC and used when the delayed
      // result is extended, so it must be pipelined with mac_pipe.
      logic mac_signed_pipe [MAC_LATENCY][CH_OUT];

      always_ff @(posedge mclk) begin
        for (int cho = 0; cho < CH_OUT; cho++) begin
          mac_pipe[0][cho] <= mac_res[cho];
          mac_signed_pipe[0][cho] <= a_signed || b_signed[cho];
        end

        for (int stage = 1; stage < MAC_LATENCY; stage++) begin
          for (int cho = 0; cho < CH_OUT; cho++) begin
            mac_pipe[stage][cho] <= mac_pipe[stage-1][cho];
            mac_signed_pipe[stage][cho] <= mac_signed_pipe[stage-1][cho];
          end
        end
      end

      // Assign outputs
      for (genvar cho = 0; cho < CH_OUT; cho++) begin : assign_c
        // When either operand is signed, the MAC result is treated as signed
        assign c[cho] = extend_mac_result(mac_pipe[MAC_LATENCY-1][cho], mac_signed_pipe[MAC_LATENCY-1][cho]);
      end

    end else if (MODE == CIM_MODE_BIT_SERIAL) begin : gen_bit_serial

      // The number of pipeline stages between the start of MAC operation and when the result is ready for accumulation
      localparam int unsigned MAC_NUM_STAGES = MAC_LATENCY - 1;
      // In bit-serial mode, adder tree accumulates B_WIDTH bits of multiply result across all input channels.
      // Keep one extra bit so the signed MSB partial sum can be negated without overflowing.
      localparam int unsigned MAC_RES_WIDTH = B_WIDTH + $clog2(CH_IN) + 1;

      localparam int unsigned BITS_A_BIT_IDX = (A_WIDTH <= 1) ? 1 : $clog2(A_WIDTH);
      localparam logic [BITS_A_BIT_IDX-1:0] A_MSB_IDX = BITS_A_BIT_IDX'(A_WIDTH - 1);
      logic [BITS_A_BIT_IDX-1:0] bit_idx;
      logic [BITS_A_BIT_IDX-1:0] active_bit_idx;

      // init selects the MSB for the current cycle and marks this MAC input as
      // the first partial result of a new accumulation stream.
      assign active_bit_idx = (init && mac) ? A_MSB_IDX : bit_idx;

      // When init and mac overlap, the counter restarts for the accepted stream
      // and immediately advances to the next bit.
      always_ff @(posedge mclk) begin
        if (mac) begin
          if (init) begin
            if (A_MSB_IDX != '0) begin
              bit_idx <= A_MSB_IDX - BITS_A_BIT_IDX'(1);
            end else begin
              bit_idx <= A_MSB_IDX;
            end
          end else if (bit_idx == '0) begin
            bit_idx <= A_MSB_IDX;
          end else begin
            bit_idx <= bit_idx - BITS_A_BIT_IDX'(1);
          end
        end
      end

      function automatic logic [MAC_RES_WIDTH-1:0] extend_b_for_mac(
          input logic [B_WIDTH-1:0] b_val,
          input logic b_is_signed
      );
        logic signed [B_WIDTH-1:0] b_signed_value;
        begin
          b_signed_value = b_val;
          extend_b_for_mac = b_is_signed ? MAC_RES_WIDTH'(b_signed_value) : MAC_RES_WIDTH'(b_val);
        end
      endfunction

      // For signed A, the MSB has negative weight. Compute the same partial sum
      // as the other bits, then negate the whole sum before the shared adder path.
      function automatic logic [MAC_RES_WIDTH-1:0] apply_a_sign_bit(
          input logic [MAC_RES_WIDTH-1:0] partial_sum,
          input logic is_msb
      );
        logic signed [MAC_RES_WIDTH-1:0] partial_sum_signed;
        begin
          partial_sum_signed = partial_sum;
          apply_a_sign_bit = (a_signed && is_msb) ? -partial_sum_signed : partial_sum;
        end
      endfunction

      function automatic logic [C_WIDTH-1:0] extend_acc_in(
          input logic [MAC_RES_WIDTH-1:0] value,
          input logic result_is_signed
      );
        logic signed [MAC_RES_WIDTH-1:0] value_signed;
        begin
          value_signed = value;
          extend_acc_in = result_is_signed ? C_WIDTH'(value_signed) : C_WIDTH'(value);
        end
      endfunction

      // Partial MAC result for the current bit position of A
      logic [MAC_RES_WIDTH-1:0] mac_res [CH_OUT];
      // Signedness must travel with the partial result because a new stream can
      // enter before the previous stream has retired through the MAC pipeline.
      logic mac_res_signed [CH_OUT];

      always_comb begin
        for (int cho = 0; cho < CH_OUT; cho++) begin
          mac_res_signed[cho] = a_signed || b_signed[cho];
        end
      end

      // Compute MAC
      always_comb begin
        for (int cho = 0; cho < CH_OUT; cho++) begin : mac_ch_out
          mac_res[cho] = '0;
          for (int chi = 0; chi < CH_IN; chi++) begin : mac_ch_in
            if (a[chi][active_bit_idx]) begin
              mac_res[cho] += extend_b_for_mac(b_mac[cho][chi], b_signed[cho]);
            end
          end
          mac_res[cho] = apply_a_sign_bit(mac_res[cho], active_bit_idx == A_MSB_IDX);
        end
      end

      // Accumulator
      logic [C_WIDTH-1:0] acc [CH_OUT];

      // Case when the MAC result is accumulated in the same cycle
      if (MAC_NUM_STAGES == 0) begin : gen_no_mac_pipe

        always_ff @(posedge mclk) begin
          for (int cho = 0; cho < CH_OUT; cho++) begin
            if (mac) begin
              if (init) begin
                acc[cho] <= extend_acc_in(mac_res[cho], mac_res_signed[cho]);
              end else begin
                acc[cho] <= (acc[cho] << 1) + extend_acc_in(mac_res[cho], mac_res_signed[cho]);
              end
            end
          end
        end

      end

      // Case when multiply + adder tree has a >=1 cycle latency. The first
      // marker travels with the partial sum so the accumulator overwrites when
      // the first delayed partial result retires.
      else begin : gen_mac_pipe
        logic [MAC_RES_WIDTH-1:0] mac_pipe [MAC_NUM_STAGES][CH_OUT];
        // acc_en_pipe is the delayed MAC input-enable. It lets the accumulator
        // accept the pipeline tail after mac deasserts; callers must still avoid
        // bubbles inside the serial input stream.
        logic acc_en_pipe [MAC_NUM_STAGES];
        logic mac_init_pipe [MAC_NUM_STAGES];
        // Signedness belongs to the partial result, not the current interface
        // controls; it is pipelined so overlapping streams extend correctly.
        logic mac_signed_pipe [MAC_NUM_STAGES][CH_OUT];

        // Propagate the partial-result pipeline. init travels with the partial
        // result so it initializes the accumulator when that result retires.
        always_ff @(posedge mclk) begin
          acc_en_pipe[0] <= mac;
          mac_init_pipe[0] <= init && mac;
          for (int cho = 0; cho < CH_OUT; cho++) begin
            mac_pipe[0][cho] <= mac_res[cho];
            mac_signed_pipe[0][cho] <= mac_res_signed[cho];
          end

          for (int stage = 1; stage < MAC_NUM_STAGES; stage++) begin
            acc_en_pipe[stage] <= acc_en_pipe[stage-1];
            mac_init_pipe[stage] <= mac_init_pipe[stage-1];
            for (int cho = 0; cho < CH_OUT; cho++) begin
              mac_pipe[stage][cho] <= mac_pipe[stage-1][cho];
              mac_signed_pipe[stage][cho] <= mac_signed_pipe[stage-1][cho];
            end
          end
        end

        always_ff @(posedge mclk) begin
          for (int cho = 0; cho < CH_OUT; cho++) begin
            if (acc_en_pipe[MAC_NUM_STAGES-1]) begin
              if (mac_init_pipe[MAC_NUM_STAGES-1]) begin
                acc[cho] <= extend_acc_in(mac_pipe[MAC_NUM_STAGES-1][cho], mac_signed_pipe[MAC_NUM_STAGES-1][cho]);
              end else begin
                acc[cho] <= (acc[cho] << 1) + extend_acc_in(mac_pipe[MAC_NUM_STAGES-1][cho], mac_signed_pipe[MAC_NUM_STAGES-1][cho]);
              end
            end
          end
        end
      end

      // Assign outputs
      assign c = acc;
    end
  endgenerate

endmodule
