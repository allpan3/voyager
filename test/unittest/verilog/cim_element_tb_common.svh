// Common CIMIntElement unit-test harness code
// Include this inside a generated test module after defining the localparams below:
// CASE_NAME, CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH, BASE_C_WIDTH,
// WRITE_CH_IN, MAC_LATENCY, INST_MODE, INST_IMPL, SIGNED, A_WIDTH, B_WIDTH, NUM_ITERS,
// MCLK_PERIOD, WCLK_PERIOD, EXPECT_BUSY_MAC_ATTEMPT, and TEST_KIND

localparam int unsigned A_COLS = CH_IN;
localparam int unsigned SUM_GUARD_WIDTH = (A_COLS <= 1) ? 1 : $clog2(A_COLS);
localparam int unsigned NUM_B_SLICES = B_WIDTH / BASE_B_WIDTH;
localparam int unsigned B_COLS = CH_OUT / NUM_B_SLICES;
localparam int unsigned B_ROWS = WRITE_CH_IN;
localparam int unsigned C_WIDTH = A_WIDTH + B_WIDTH + SUM_GUARD_WIDTH;
localparam int unsigned BITS_A_COLS = (A_COLS <= 1) ? 1 : $clog2(A_COLS);
localparam int unsigned BITS_SET = (B_SETS <= 1) ? 1 : $clog2(B_SETS);
localparam int unsigned DEFAULT_RNG_SEED = 32'h1;

localparam int unsigned TEST_NORMAL = 0;
localparam int unsigned TEST_RESET_MID_OP = 1;
localparam int unsigned MAX_WAIT_CYCLES = 4096;

logic                       wclk;
logic                       mclk;
logic                       rstn;
logic [A_WIDTH-1:0]         a [A_COLS];
logic [B_WIDTH-1:0]         b [B_COLS][B_ROWS];
logic                       wen;
logic [BITS_A_COLS-1:0]     widx;
logic [BITS_SET-1:0]        wset;
logic                       mac_start;
logic [BITS_SET-1:0]        mset;
logic [C_WIDTH-1:0]         c [B_COLS];
logic                       c_valid;
logic                       mac_busy;

logic [B_WIDTH-1:0] model_weights [B_SETS][B_COLS][A_COLS];
logic [C_WIDTH-1:0] expected [NUM_ITERS][B_COLS];
int unsigned busy_mac_attempts;
int unsigned rng_state;
string waveform_path;

CIMIntElement #(
    .CH_IN(CH_IN),
    .CH_OUT(CH_OUT),
    .B_SETS(B_SETS),
    .BASE_A_WIDTH(BASE_A_WIDTH),
    .BASE_B_WIDTH(BASE_B_WIDTH),
    .BASE_C_WIDTH(BASE_C_WIDTH),
    .WRITE_CH_IN(WRITE_CH_IN),
    .MAC_LATENCY(MAC_LATENCY),
    .MODE(INST_MODE),
    .MACRO_IMPL(INST_IMPL),
    .A_WIDTH(A_WIDTH),
    .B_WIDTH(B_WIDTH),
    .SIGNED(SIGNED)
) dut (
    .wclk(wclk),
    .mclk(mclk),
    .rstn(rstn),
    .a(a),
    .b(b),
    .wen(wen),
    .widx(widx),
    .wset(wset),
    .mac_start(mac_start),
    .mset(mset),
    .c(c),
    .c_valid(c_valid),
    .mac_busy(mac_busy)
);

// Start VCD dumping when the runner supplies a waveform path
task automatic start_waveform_dump;
  begin
    if ($value$plusargs("waveform=%s", waveform_path)) begin
      $dumpfile(waveform_path);
      $dumpvars(0);
    end
  end
endtask

// Load the deterministic pseudo-random generator seed from plusargs
task automatic init_rng_from_plusarg;
  int unsigned plusarg_rng_seed;
  begin
    if ($value$plusargs("rng_seed=%h", plusarg_rng_seed)) begin
      rng_state = plusarg_rng_seed;
    end else begin
      rng_state = DEFAULT_RNG_SEED;
    end
  end
endtask

// Advance the deterministic pseudo-random generator used by test stimuli
task automatic rng_next(output int unsigned value);
  begin
    rng_state = (rng_state * 32'd1664525) + 32'd1013904223;
    value = rng_state;
  end
endtask

// Reject common generated parameters that would make scheduling ambiguous
task automatic check_common_test_params;
  begin
    if ((MCLK_PERIOD <= 1) || (WCLK_PERIOD <= 1)) begin
      $fatal(1, "%s: clock periods must be greater than one tick", CASE_NAME);
    end
    if (NUM_ITERS == 0) begin
      $fatal(1, "%s: NUM_ITERS must be positive", CASE_NAME);
    end
  end
endtask

// Generate one random activation vector on the a interface
task automatic randomize_activation;
  int unsigned value;
  begin
    for (int col = 0; col < A_COLS; col++) begin
      rng_next(value);
      a[col] = A_WIDTH'(value);
    end
  end
endtask

// Interpret an activation according to the generated element signedness
function automatic longint signed decode_a(input logic [A_WIDTH-1:0] value);
  logic signed [A_WIDTH-1:0] signed_value;
  begin
    signed_value = value;
    decode_a = SIGNED ? longint'(signed_value) : longint'(value);
  end
endfunction

// Interpret a stored weight according to the generated element signedness
function automatic longint signed decode_b(input logic [B_WIDTH-1:0] value);
  logic signed [B_WIDTH-1:0] signed_value;
  begin
    signed_value = value;
    decode_b = SIGNED ? longint'(signed_value) : longint'(value);
  end
endfunction

// Pulse the element mclk once
task automatic tick_mclk;
  begin
    #(MCLK_PERIOD) mclk = 1'b1;
    #1 mclk = 1'b0;
    #1;
  end
endtask

// Pulse the element wclk once
task automatic tick_wclk;
  begin
    #(WCLK_PERIOD) wclk = 1'b1;
    #1 wclk = 1'b0;
    #1;
  end
endtask

// Reset all driven signals and scoreboard state before applying DUT reset
task automatic drive_defaults;
  begin
    wclk = 1'b0;
    mclk = 1'b0;
    rstn = 1'b0;
    wen = 1'b0;
    widx = '0;
    wset = '0;
    mac_start = 1'b0;
    mset = '0;
    busy_mac_attempts = 0;

    init_rng_from_plusarg();

    for (int chi = 0; chi < A_COLS; chi++) begin
      a[chi] = '0;
    end
    for (int col = 0; col < B_COLS; col++) begin
      for (int lane = 0; lane < B_ROWS; lane++) begin
        b[col][lane] = '0;
      end
    end
    for (int row = 0; row < B_SETS; row++) begin
      for (int col = 0; col < B_COLS; col++) begin
        for (int chi = 0; chi < A_COLS; chi++) begin
          model_weights[row][col][chi] = '0;
        end
      end
    end
    for (int slot = 0; slot < NUM_ITERS; slot++) begin
      for (int col = 0; col < B_COLS; col++) begin
        expected[slot][col] = '0;
      end
    end
    #1;
  end
endtask

// Reject generated parameters that would make the element harness ambiguous
task automatic check_test_params;
  begin
    check_common_test_params();
    if ((A_COLS % WRITE_CH_IN) != 0) begin
      $fatal(1, "%s: A_COLS must be divisible by WRITE_CH_IN", CASE_NAME);
    end
    if ((B_WIDTH % BASE_B_WIDTH) != 0) begin
      $fatal(1, "%s: B_WIDTH must be divisible by BASE_B_WIDTH", CASE_NAME);
    end
    if ((CH_OUT % NUM_B_SLICES) != 0) begin
      $fatal(1, "%s: CH_OUT must be divisible by NUM_B_SLICES", CASE_NAME);
    end
    if (TEST_KIND > TEST_RESET_MID_OP) begin
      $fatal(1, "%s: unknown TEST_KIND=%0d", CASE_NAME, TEST_KIND);
    end
  end
endtask

// Apply element reset and check the reset-state handshake
task automatic apply_reset;
  begin
    rstn = 1'b0;
    mac_start = 1'b0;
    wen = 1'b0;
    tick_mclk();
    tick_mclk();
    if (mac_busy !== 1'b1) begin
      $fatal(1, "%s: mac_busy must be high during reset", CASE_NAME);
    end
    if (c_valid !== 1'b0) begin
      $fatal(1, "%s: c_valid must be low during reset", CASE_NAME);
    end

    rstn = 1'b1;
    #1;
    if (mac_busy !== 1'b0) begin
      $fatal(1, "%s: mac_busy must be low after reset release", CASE_NAME);
    end
    if (c_valid !== 1'b0) begin
      $fatal(1, "%s: c_valid must stay low after reset release", CASE_NAME);
    end
  end
endtask

// Drive one WRITE_CH_IN-wide logical B write group for a row and base channel
task automatic drive_random_weight_group(input int row, input int base);
  int unsigned value;
  begin
    wset = BITS_SET'(row);
    widx = BITS_A_COLS'(base);
    for (int col = 0; col < B_COLS; col++) begin
      for (int lane = 0; lane < B_ROWS; lane++) begin
        rng_next(value);
        b[col][lane] = B_WIDTH'(value);
      end
    end
    wen = 1'b1;
  end
endtask

// Mirror a completed DUT write group into the logical reference model
task automatic commit_weight_group(input int row, input int base);
  begin
    for (int col = 0; col < B_COLS; col++) begin
      for (int lane = 0; lane < B_ROWS; lane++) begin
        model_weights[row][col][base + lane] = b[col][lane];
      end
    end
  end
endtask

// Load every logical B row before MAC checks begin
task automatic load_all_weights;
  begin
    for (int row = 0; row < B_SETS; row++) begin
      for (int base = 0; base < A_COLS; base += WRITE_CH_IN) begin
        drive_random_weight_group(row, base);
        tick_wclk();
        commit_weight_group(row, base);
        wen = 1'b0;
      end
    end
  end
endtask

// Compute the expected logical output for the currently driven activation and row
task automatic record_expected(input int slot, input int row);
  longint signed acc;
  begin
    if (slot >= NUM_ITERS) begin
      $fatal(1, "%s: expected slot %0d is outside NUM_ITERS=%0d", CASE_NAME, slot, NUM_ITERS);
    end

    for (int col = 0; col < B_COLS; col++) begin
      acc = 0;
      for (int chi = 0; chi < A_COLS; chi++) begin
        acc += decode_a(a[chi]) * decode_b(model_weights[row][col][chi]);
      end
      expected[slot][col] = C_WIDTH'(acc);
    end
  end
endtask

// Start one logical element MAC operation while the element is not busy
task automatic start_element_op(input int slot, input int row);
  begin
    if (mac_busy !== 1'b0) begin
      $fatal(1, "%s: attempted to start op %0d while mac_busy is high", CASE_NAME, slot);
    end

    randomize_activation();
    mset = BITS_SET'(row);
    record_expected(slot, row);

    mac_start = 1'b1;
    tick_mclk();
    mac_start = 1'b0;

    if (c_valid !== 1'b0) begin
      $fatal(1, "%s: c_valid must clear on accepted op %0d", CASE_NAME, slot);
    end
  end
endtask

// Optionally assert ignored mac_start noise while the element is busy
task automatic drive_busy_mac_noise;
  begin
    if (EXPECT_BUSY_MAC_ATTEMPT && (mac_busy === 1'b1)) begin
      mac_start = 1'b1;
      busy_mac_attempts++;
    end else begin
      mac_start = 1'b0;
    end
  end
endtask

// Wait for one logical result and compare every output column
task automatic wait_for_valid_and_check(input int slot);
  int unsigned wait_cycles;
  begin
    wait_cycles = 0;
    while (c_valid !== 1'b1) begin
      if (wait_cycles >= MAX_WAIT_CYCLES) begin
        $fatal(1, "%s: timed out waiting for c_valid on op %0d", CASE_NAME, slot);
      end
      drive_busy_mac_noise();
      tick_mclk();
      mac_start = 1'b0;
      wait_cycles++;
    end

    for (int col = 0; col < B_COLS; col++) begin
      if (c[col] !== expected[slot][col]) begin
        $fatal(1,
               "%s: op %0d column %0d got 0x%0h expected 0x%0h",
               CASE_NAME, slot, col, c[col], expected[slot][col]);
      end
    end
    if (mac_busy !== 1'b0) begin
      $fatal(1, "%s: mac_busy must be low with completed op %0d", CASE_NAME, slot);
    end
  end
endtask

// Check that completed outputs stay c_valid and stable until the next accepted op
task automatic check_valid_hold(input int slot);
  logic [C_WIDTH-1:0] held_c [B_COLS];
  begin
    for (int col = 0; col < B_COLS; col++) begin
      held_c[col] = c[col];
    end
    mac_start = 1'b0;
    tick_mclk();
    if (c_valid !== 1'b1) begin
      $fatal(1, "%s: c_valid dropped without a new op after slot %0d", CASE_NAME, slot);
    end
    for (int col = 0; col < B_COLS; col++) begin
      if (c[col] !== held_c[col]) begin
        $fatal(1, "%s: c changed without a new op after slot %0d column %0d",
               CASE_NAME, slot, col);
      end
    end
  end
endtask

// Run one complete logical operation and stable-output check
task automatic run_one_op(input int slot, input int row);
  begin
    start_element_op(slot, row);
    wait_for_valid_and_check(slot);
    check_valid_hold(slot);
  end
endtask

// Run the normal legal-operation sequence for this case
task automatic run_normal_ops;
  begin
    busy_mac_attempts = 0;
    for (int op = 0; op < NUM_ITERS; op++) begin
      run_one_op(op, op % B_SETS);
    end
    if (EXPECT_BUSY_MAC_ATTEMPT && (busy_mac_attempts == 0)) begin
      $fatal(1, "%s: no busy mac_start noise was injected", CASE_NAME);
    end
  end
endtask

// Start an operation and reset before it can produce a valid result
task automatic reset_during_active_op;
  begin
    if (mac_busy !== 1'b0) begin
      $fatal(1, "%s: reset-mid-op precondition failed because mac_busy is high", CASE_NAME);
    end

    randomize_activation();
    mset = '0;
    mac_start = 1'b1;
    tick_mclk();
    mac_start = 1'b0;

    if (INST_MODE == CIM_MODE_BIT_SERIAL) begin
      // Let the resetless serial macro wrapper consume one full window before parent reset
      for (int cycle = 1; cycle < BASE_A_WIDTH; cycle++) begin
        drive_busy_mac_noise();
        tick_mclk();
        mac_start = 1'b0;
      end
    end else begin
      drive_busy_mac_noise();
      tick_mclk();
      mac_start = 1'b0;
    end

    rstn = 1'b0;
    tick_mclk();
    tick_mclk();
    if (mac_busy !== 1'b1) begin
      $fatal(1, "%s: mac_busy must be high while reset interrupts an op", CASE_NAME);
    end
    if (c_valid !== 1'b0) begin
      $fatal(1, "%s: c_valid must clear when reset interrupts an op", CASE_NAME);
    end

    rstn = 1'b1;
    #1;
    if (mac_busy !== 1'b0) begin
      $fatal(1, "%s: mac_busy did not recover after reset-mid-op", CASE_NAME);
    end
    if (c_valid !== 1'b0) begin
      $fatal(1, "%s: stale c_valid appeared after reset-mid-op", CASE_NAME);
    end

    tick_mclk();
    if (c_valid !== 1'b0) begin
      $fatal(1, "%s: stale result appeared after reset recovery idle cycle", CASE_NAME);
    end
  end
endtask

// Run reset interruption followed by a clean legal sequence
task automatic run_reset_mid_op;
  begin
    apply_reset();
    load_all_weights();
    reset_during_active_op();
    run_normal_ops();
  end
endtask

initial begin
  start_waveform_dump();
  drive_defaults();
  check_test_params();
  if (TEST_KIND == TEST_RESET_MID_OP) begin
    run_reset_mid_op();
  end else begin
    apply_reset();
    load_all_weights();
    run_normal_ops();
  end

  $display("[pass] %s", CASE_NAME);
  $finish;
end
