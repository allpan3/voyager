// Shared CIM testbench helpers for generated Verilator harnesses
// Include this inside a generated test module after defining CASE_NAME, CH_IN,
// NUM_ROWS, A_WIDTH, NUM_ITERS, MCLK_PERIOD, and WCLK_PERIOD

localparam int unsigned BITS_CH_IN = (CH_IN <= 1) ? 1 : $clog2(CH_IN);
localparam int unsigned BITS_ROW = (NUM_ROWS <= 1) ? 1 : $clog2(NUM_ROWS);
localparam int unsigned DEFAULT_RNG_SEED = 32'h1;

int unsigned rng_state;

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
    for (int chi = 0; chi < CH_IN; chi++) begin
      rng_next(value);
      a[chi] = A_WIDTH'(value);
    end
  end
endtask
