// SystemC tests for the strict INT8 CIMProcessor matrix-backend contract

#ifdef SCVERIFY
#include <mc_scverify.h>
#define CIMPROCESSOR_DUT_TYPE(T) CCS_DESIGN(T)
#else
#define CIMPROCESSOR_DUT_TYPE(T) T
#endif

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include <deque>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "CIMProcessor.h"

#ifndef CIM_PROCESSOR_LARGE_TEST
#define CIM_PROCESSOR_LARGE_TEST 0
#endif

#ifndef CIM_PROCESSOR_TEST_CH_OUT
#define CIM_PROCESSOR_TEST_CH_OUT 2
#endif

#ifndef CIM_PROCESSOR_TEST_BASE_A_WIDTH
#define CIM_PROCESSOR_TEST_BASE_A_WIDTH 4
#endif

#ifndef CIM_PROCESSOR_TEST_BASE_B_WIDTH
#define CIM_PROCESSOR_TEST_BASE_B_WIDTH 4
#endif

#ifndef CIM_PROCESSOR_TEST_BASE_C_WIDTH
#define CIM_PROCESSOR_TEST_BASE_C_WIDTH 12
#endif

#if CIM_PROCESSOR_LARGE_TEST
static constexpr int CH_IN = 64;
static constexpr int CH_OUT = 64;
static constexpr int B_SETS = 4;
static constexpr int BASE_A_WIDTH = 4;
static constexpr int BASE_B_WIDTH = 4;
static constexpr int BASE_C_WIDTH = 20;
static constexpr int TILE_INPUT_AXIS_ELEMENTS = 1;
static constexpr int TILE_OUTPUT_AXIS_ELEMENTS = 2;
static constexpr int INPUT_AXIS_TILES = 1;
static constexpr int OUTPUT_AXIS_TILES = 2;
#else
static constexpr int CH_IN = 2;
static constexpr int CH_OUT = CIM_PROCESSOR_TEST_CH_OUT;
static constexpr int B_SETS = 2;
static constexpr int BASE_A_WIDTH = CIM_PROCESSOR_TEST_BASE_A_WIDTH;
static constexpr int BASE_B_WIDTH = CIM_PROCESSOR_TEST_BASE_B_WIDTH;
static constexpr int BASE_C_WIDTH = CIM_PROCESSOR_TEST_BASE_C_WIDTH;
static constexpr int TILE_INPUT_AXIS_ELEMENTS = 2;
static constexpr int TILE_OUTPUT_AXIS_ELEMENTS = 1;
static constexpr int INPUT_AXIS_TILES = 2;
static constexpr int OUTPUT_AXIS_TILES = 3;
#endif

static constexpr int WRITE_CH_IN = 1;
static constexpr int MAC_LATENCY = 1;
static constexpr int MODE = 0;
static constexpr int A_WIDTH = 8;
static constexpr int B_WIDTH = 8;
static constexpr bool SIGNED = true;
static constexpr int A_PORT_TILES = INPUT_AXIS_TILES;
#ifndef CIM_TEST_B_PORT_TILES
#define CIM_TEST_B_PORT_TILES 1
#endif
static constexpr int B_PORT_TILES = CIM_TEST_B_PORT_TILES;
static constexpr int C_PORT_TILES = OUTPUT_AXIS_TILES;
static constexpr int K = CH_IN * TILE_INPUT_AXIS_ELEMENTS * INPUT_AXIS_TILES;
static constexpr int TILE_N =
    (CH_OUT / (B_WIDTH / BASE_B_WIDTH)) * TILE_OUTPUT_AXIS_ELEMENTS;
static constexpr int N = TILE_N * OUTPUT_AXIS_TILES;
static constexpr int BUFFER_DEPTH = 16;

using Processor =
    CIMProcessor<std::tuple<DataTypes::int8>, std::tuple<DataTypes::int8>,
                 DataTypes::int8, DataTypes::int8, DataTypes::int24,
                 DataTypes::int24, DataTypes::fp8_e8m0, K, N, BUFFER_DEPTH,
                 CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH,
                 BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY, MODE, SIGNED,
                 TILE_INPUT_AXIS_ELEMENTS, TILE_OUTPUT_AXIS_ELEMENTS,
                 INPUT_AXIS_TILES, OUTPUT_AXIS_TILES, A_PORT_TILES,
                 B_PORT_TILES, C_PORT_TILES, CIM_C_BEAT_OUTPUT_MAJOR>;

using Dut = CIMPROCESSOR_DUT_TYPE(Processor);
using Buffer = DataTypes::int24;
using BufferVector = Pack1D<Buffer, N>;
using WriteRequest = BufferWriteRequest<BufferVector>;

// CIMProcessorTb checks scheduling, resident-weight reuse, backpressure, and
// persistent accumulation
SC_MODULE(CIMProcessorTb) {
  // One queued result lets the consumer independently delay ready and
  // backpressure the processor
  struct ExpectedOutput {
    std::string label;
    BufferVector values;
    int stall_cycles;
  };

  Dut dut;
  sc_clock clk;
  sc_signal<bool> rstn;

  Connections::Combinational<ac_int<INPUT_BUFFER_WIDTH, false>> input_channel;
  Connections::Combinational<ac_int<Processor::WEIGHT_WRITE_WIDTH, false>>
      weight_channel;
  Connections::Combinational<BufferVector> bias_channel;
  Connections::Combinational<MatrixParams> params_channel;
  Connections::Combinational<BufferVector> output_channel;
  Connections::SyncChannel start_channel;
#if ENABLE_PERF_COUNTERS
  sc_signal<MatrixPerformance::CounterIndex> perf_counter_select;
  sc_signal<MatrixPerformance::Counter> perf_counter_value;
#endif

  Connections::Combinational<ac_int<16, false>> accumulation_read_address_0;
  Connections::Combinational<BufferVector> accumulation_read_data_0;
  Connections::Combinational<WriteRequest> accumulation_write_request_0;
#if DOUBLE_BUFFERED_ACCUM_BUFFER
  Connections::Combinational<ac_int<16, false>> accumulation_read_address_1;
  Connections::Combinational<BufferVector> accumulation_read_data_1;
  Connections::Combinational<WriteRequest> accumulation_write_request_1;
  Connections::SyncChannel accumulation_done_0;
  Connections::SyncChannel accumulation_done_1;
#endif

  BufferVector accumulation_memory[Processor::ACCUM_BUFFER_BANKS][BUFFER_DEPTH];
  bool pending_read[Processor::ACCUM_BUFFER_BANKS];
  ac_int<16, false> pending_read_address[Processor::ACCUM_BUFFER_BANKS];
  unsigned long pending_read_ready_cycle[Processor::ACCUM_BUFFER_BANKS];
  int read_count[Processor::ACCUM_BUFFER_BANKS];
  int write_count[Processor::ACCUM_BUFFER_BANKS];
  int done_count[Processor::ACCUM_BUFFER_BANKS];
  unsigned long buffer_cycle;

  std::deque<ExpectedOutput> expected_outputs;
  std::deque<BufferVector> pending_biases;
  std::vector<unsigned long> throughput_input_cycles;
  std::vector<unsigned long> throughput_output_cycles;
  sc_event expected_output_event;
  sc_event bias_event;
  int checked_outputs;
  bool test_failed;

  SC_HAS_PROCESS(CIMProcessorTb);

  // Construct the processor and independent ready/valid peers around it
  explicit CIMProcessorTb(sc_module_name name)
      : sc_module(name),
        dut("dut"),
        clk("clk", 10, SC_NS),
        start_channel("start_channel"),
#if DOUBLE_BUFFERED_ACCUM_BUFFER
        accumulation_done_0("accumulation_done_0"),
        accumulation_done_1("accumulation_done_1"),
#endif
        buffer_cycle(0),
        checked_outputs(0),
        test_failed(false) {
    dut.clk(clk);
    dut.rstn(rstn);
    dut.input_channel(input_channel);
    dut.weight_channel(weight_channel);
    dut.bias_channel(bias_channel);
    dut.params_in(params_channel);
    dut.output_channel(output_channel);
    dut.start(start_channel);
#if ENABLE_PERF_COUNTERS
    dut.perf_counter_select(perf_counter_select);
    dut.perf_counter_value(perf_counter_value);
#endif
    dut.accumulation_buffer_read_address[0](accumulation_read_address_0);
    dut.accumulation_buffer_read_data[0](accumulation_read_data_0);
    dut.accumulation_buffer_write_request[0](accumulation_write_request_0);
#if DOUBLE_BUFFERED_ACCUM_BUFFER
    dut.accumulation_buffer_read_address[1](accumulation_read_address_1);
    dut.accumulation_buffer_read_data[1](accumulation_read_data_1);
    dut.accumulation_buffer_write_request[1](accumulation_write_request_1);
    dut.accumulation_buffer_done[0](accumulation_done_0);
    dut.accumulation_buffer_done[1](accumulation_done_1);
#endif

    for (int bank = 0; bank < Processor::ACCUM_BUFFER_BANKS; bank++) {
      pending_read[bank] = false;
      pending_read_address[bank] = 0;
      pending_read_ready_cycle[bank] = 0;
      read_count[bank] = 0;
      write_count[bank] = 0;
      done_count[bank] = 0;
      for (int address = 0; address < BUFFER_DEPTH; address++) {
        accumulation_memory[bank][address] = BufferVector::zero();
      }
    }

    SC_THREAD(run);
    sensitive << clk.posedge_event();

    SC_THREAD(drive_bias);
    sensitive << clk.posedge_event();

    SC_THREAD(check_outputs);
    sensitive << clk.posedge_event();

    SC_THREAD(run_accumulation_buffer);
    sensitive << clk.posedge_event();

#if DOUBLE_BUFFERED_ACCUM_BUFFER
    SC_THREAD(consume_done_0);
    sensitive << clk.posedge_event();

    SC_THREAD(consume_done_1);
    sensitive << clk.posedge_event();
#endif

    SC_THREAD(watchdog);
  }

  // Advance one processor cycle
  void tick() {
    wait(clk.posedge_event());
    wait(SC_ZERO_TIME);
  }

  // Record a deterministic failure without abandoning channel cleanup
  void require(bool condition, const std::string &message) {
    if (condition) {
      return;
    }
    std::cerr << "[FAIL] " << message << std::endl;
    test_failed = true;
  }

  // Stop a ready/valid or scheduling deadlock with a bounded failure
  void watchdog() {
    wait(1, SC_MS);
    require(false, "timed out waiting for CIMProcessor completion");
    sc_stop();
  }

  // Initialize mapper loop indices shared by all scenarios
  MatrixParams make_base_params() const {
    MatrixParams params;
    for (int level = 0; level < 2; level++) {
      for (int loop = 0; loop < 6; loop++) {
        params.loops[level][loop] = 1;
      }
    }

    // Level 0 orders output Y, output X, weights, filter Y, then reduction
    params.y_loop_idx[0] = 0;
    params.x_loop_idx[0] = 1;
    params.weight_loop_idx[0] = 2;
    params.fy_loop_idx[0] = 3;
    params.reduction_loop_idx[0] = 4;

    // Level 1 orders filter Y/X, output Y, weights, output X, then reduction
    params.fy_loop_idx[1] = 0;
    params.fx_loop_idx = 1;
    params.y_loop_idx[1] = 2;
    params.weight_loop_idx[1] = 3;
    params.x_loop_idx[1] = 4;
    params.reduction_loop_idx[1] = 5;

    params.weight_reuse_idx[0] = 0;
    params.weight_reuse_idx[1] = 1;
    params.use_input_codebook = false;
    params.use_weight_codebook = false;
    return params;
  }

  // Create two output-X addresses with two temporal contributions each
  MatrixParams make_accumulation_params(bool has_bias) const {
    MatrixParams params = make_base_params();
    params.loops[1][params.x_loop_idx[1]] = 2;
    params.loops[1][params.reduction_loop_idx[1]] = 2;
    params.has_bias = has_bias;
    return params;
  }

  // Create output-X or output-Y traversal inside one resident-weight lifetime
  MatrixParams make_weight_reuse_params(bool traverse_x) const {
    MatrixParams params = make_base_params();
    params.weight_loop_idx[0] = 1;
    params.x_loop_idx[0] = traverse_x ? 2 : 0;
    params.y_loop_idx[0] = traverse_x ? 0 : 2;
    params.fy_loop_idx[0] = 3;
    params.reduction_loop_idx[0] = 4;
    params.loops[0][traverse_x ? params.x_loop_idx[0] : params.y_loop_idx[0]] =
        4;
    params.has_bias = false;
    return params;
  }

  // Create a long output-X traversal inside one resident-weight lifetime
  MatrixParams make_throughput_params(int operations) const {
    MatrixParams params = make_weight_reuse_params(true);
    params.loops[0][params.x_loop_idx[0]] = operations;
    return params;
  }

  // Create one output address with two contributions for the backpressure case
  MatrixParams make_backpressure_params() const {
    MatrixParams params = make_base_params();
    params.loops[1][params.reduction_loop_idx[1]] = 2;
    params.has_bias = true;
    return params;
  }

  // Create one result redirected into the selected accumulation-buffer bank
  MatrixParams make_double_buffer_params() const {
    MatrixParams params = make_base_params();
    params.has_bias = false;
    params.write_output_to_accum_buffer = true;
    return params;
  }

  // Return one signed activation pattern so heterogeneous jobs cannot alias
  int input_value(int input_pattern, int k) const {
    return k == 0 ? -(input_pattern + 2) : input_pattern + 1;
  }

  // Return one deterministic signed weight from a logical B row
  int weight_value(int weight_pattern, int k, int n) const {
    const int output_axis_idx = n / TILE_N;
    return weight_pattern + 1 + output_axis_idx + k;
  }

  // Pack one complete signed A vector
  ac_int<INPUT_BUFFER_WIDTH, false> make_inputs(int input_pattern) const {
    ac_int<INPUT_BUFFER_WIDTH, false> inputs = 0;
    for (int k = 0; k < K; k++) {
      const ac_int<A_WIDTH, true> value = input_value(input_pattern, k);
      inputs.set_slc(k * A_WIDTH, value.template slc<A_WIDTH>(0));
    }
    return inputs;
  }

  // Pack one output-axis span in the weight-channel lane order
  ac_int<Processor::WEIGHT_WRITE_WIDTH, false> make_weight_beat(
      int weight_pattern, int k, int span) const {
    ac_int<Processor::WEIGHT_WRITE_WIDTH, false> beat = 0;
    for (int port_tile = 0; port_tile < B_PORT_TILES; port_tile++) {
      for (int tile_n = 0; tile_n < TILE_N; tile_n++) {
        const int n = (span * B_PORT_TILES + port_tile) * TILE_N + tile_n;
        const int lane = port_tile * TILE_N + tile_n;
        const ac_int<B_WIDTH, true> value = weight_value(weight_pattern, k, n);
        beat.set_slc(lane * B_WIDTH, value.template slc<B_WIDTH>(0));
      }
    }
    return beat;
  }

  // Compute one complete golden MAC result
  BufferVector expected_partial(int input_pattern, int weight_pattern) const {
    BufferVector expected = BufferVector::zero();
    for (int n = 0; n < N; n++) {
      int value = 0;
      for (int k = 0; k < K; k++) {
        value +=
            input_value(input_pattern, k) * weight_value(weight_pattern, k, n);
      }
      expected[n] = Buffer(value);
    }
    return expected;
  }

  // Add one vector into another with the processor's Buffer arithmetic
  void add_vector(BufferVector & destination, const BufferVector &source)
      const {
    for (int n = 0; n < N; n++) {
      destination[n] += source[n];
    }
  }

  // Queue one bias vector for the independent bias producer
  BufferVector queue_bias(int base) {
    BufferVector bias = BufferVector::zero();
    for (int n = 0; n < N; n++) {
      bias[n] = Buffer(base + n);
    }
    pending_biases.push_back(bias);
    bias_event.notify(SC_ZERO_TIME);
    return bias;
  }

  // Queue one expected output and its deliberate ready stall
  void expect_output(const std::string &label, const BufferVector &values,
                     int stall_cycles) {
    expected_outputs.push_back(ExpectedOutput{label, values, stall_cycles});
    expected_output_event.notify(SC_ZERO_TIME);
  }

  // Send one mapper job with an exact resident-weight load schedule
  void send_job(const MatrixParams &params,
                const std::vector<int> &weight_patterns,
                const std::vector<bool> &load_weights, int input_pattern) {
    require(weight_patterns.size() == load_weights.size(),
            "test job vectors must have equal lengths");
    params_channel.Push(params);
    start_channel.SyncPop();

    for (std::size_t operation = 0; operation < weight_patterns.size();
         operation++) {
      if (load_weights[operation]) {
        for (int k = 0; k < K; k++) {
          for (int span = 0; span < Processor::WEIGHT_BEATS_PER_ROW; span++) {
            weight_channel.Push(
                make_weight_beat(weight_patterns[operation], k, span));
          }
        }
      }
      input_channel.Push(make_inputs(input_pattern));
    }
  }

  // Return the current clock index for ready/valid cadence measurements
  unsigned long current_cycle() const {
    return static_cast<unsigned long>(sc_time_stamp() / sc_time(10, SC_NS));
  }

  // Send a no-accumulation stream that reuses one resident weight set
  void send_throughput_job(int operations, int weight_pattern,
                           int input_pattern) {
    params_channel.Push(make_throughput_params(operations));
    start_channel.SyncPop();

    for (int k = 0; k < K; k++) {
      for (int span = 0; span < Processor::WEIGHT_BEATS_PER_ROW; span++) {
        weight_channel.Push(make_weight_beat(weight_pattern, k, span));
      }
    }

    for (int operation = 0; operation < operations; operation++) {
      input_channel.Push(make_inputs(input_pattern));
      throughput_input_cycles.push_back(current_cycle());
    }
  }

  // Print and validate consecutive acceptance intervals
  void report_throughput(const char *name,
                         const std::vector<unsigned long> &cycles,
                         int expected_count) {
    std::ostringstream count_message;
    count_message << name << " expected " << expected_count << " samples got "
                  << cycles.size();
    require(static_cast<int>(cycles.size()) == expected_count,
            count_message.str());

    std::cout << "RTL_CADENCE " << name << "_intervals=";
    for (std::size_t index = 1; index < cycles.size(); index++) {
      if (index > 1) {
        std::cout << ",";
      }
      std::cout << cycles[index] - cycles[index - 1];
    }
    std::cout << std::endl;
  }

  // Drive queued biases only when the processor requests them
  void drive_bias() {
    bias_channel.ResetWrite();
    wait();
    while (!rstn.read()) {
      wait();
    }

    while (true) {
      if (pending_biases.empty()) {
        wait(bias_event);
        continue;
      }
      const BufferVector bias = pending_biases.front();
      pending_biases.pop_front();
      bias_channel.Push(bias);
    }
  }

  // Delay output ready to prove result-channel backpressure is lossless
  void check_outputs() {
    output_channel.ResetRead();
    wait();
    while (!rstn.read()) {
      wait();
    }

    while (true) {
      if (expected_outputs.empty()) {
        wait(expected_output_event);
        continue;
      }

      const ExpectedOutput expected = expected_outputs.front();
      expected_outputs.pop_front();
      for (int cycle = 0; cycle < expected.stall_cycles; cycle++) {
        wait();
      }

      const BufferVector result = output_channel.Pop();
      if (expected.label.rfind("throughput ", 0) == 0) {
        throughput_output_cycles.push_back(current_cycle());
      }
      for (int n = 0; n < N; n++) {
        std::ostringstream message;
        message << expected.label << " n " << n << " expected "
                << expected.values[n].int_val.to_int() << " got "
                << result[n].int_val.to_int();
        require(
            result[n].int_val.to_int() == expected.values[n].int_val.to_int(),
            message.str());
      }
      checked_outputs++;
    }
  }

  // Apply deterministic read-address and write-request backpressure to bank 0
  void service_bank_0() {
    if (pending_read[0] && buffer_cycle >= pending_read_ready_cycle[0]) {
      accumulation_read_data_0.Push(
          accumulation_memory[0][pending_read_address[0].to_int()]);
      pending_read[0] = false;
    }

    ac_int<16, false> address;
    if (!pending_read[0] && buffer_cycle % 3 == 0 &&
        accumulation_read_address_0.PopNB(address)) {
      pending_read[0] = true;
      pending_read_address[0] = address;
      pending_read_ready_cycle[0] = buffer_cycle + 2;
      read_count[0]++;
    }

    WriteRequest write;
    if (buffer_cycle % 4 == 0 && accumulation_write_request_0.PopNB(write)) {
      accumulation_memory[0][write.address.to_int()] = write.data;
      write_count[0]++;
    }
  }

#if DOUBLE_BUFFERED_ACCUM_BUFFER
  // Apply a different deterministic backpressure phase to bank 1
  void service_bank_1() {
    if (pending_read[1] && buffer_cycle >= pending_read_ready_cycle[1]) {
      accumulation_read_data_1.Push(
          accumulation_memory[1][pending_read_address[1].to_int()]);
      pending_read[1] = false;
    }

    ac_int<16, false> address;
    if (!pending_read[1] && buffer_cycle % 3 == 1 &&
        accumulation_read_address_1.PopNB(address)) {
      pending_read[1] = true;
      pending_read_address[1] = address;
      pending_read_ready_cycle[1] = buffer_cycle + 2;
      read_count[1]++;
    }

    WriteRequest write;
    if (buffer_cycle % 4 == 1 && accumulation_write_request_1.PopNB(write)) {
      accumulation_memory[1][write.address.to_int()] = write.data;
      write_count[1]++;
    }
  }
#endif

  // Emulate MatrixUnit's persistent accumulation memory and ready/valid ports
  void run_accumulation_buffer() {
    accumulation_read_address_0.ResetRead();
    accumulation_read_data_0.ResetWrite();
    accumulation_write_request_0.ResetRead();
#if DOUBLE_BUFFERED_ACCUM_BUFFER
    accumulation_read_address_1.ResetRead();
    accumulation_read_data_1.ResetWrite();
    accumulation_write_request_1.ResetRead();
#endif
    wait();

    while (true) {
      service_bank_0();
#if DOUBLE_BUFFERED_ACCUM_BUFFER
      service_bank_1();
#endif
      buffer_cycle++;
      wait();
    }
  }

#if DOUBLE_BUFFERED_ACCUM_BUFFER
  // Consume the bank-0 completion handshake
  void consume_done_0() {
    accumulation_done_0.ResetRead();
    wait();
    while (!rstn.read()) {
      wait();
    }
    while (true) {
      accumulation_done_0.SyncPop();
      done_count[0]++;
    }
  }

  // Consume the bank-1 completion handshake
  void consume_done_1() {
    accumulation_done_1.ResetRead();
    wait();
    while (!rstn.read()) {
      wait();
    }
    while (true) {
      accumulation_done_1.SyncPop();
      done_count[1]++;
    }
  }
#endif

  // Drive heterogeneous jobs without draining prior results first
  void run() {
    params_channel.ResetWrite();
    input_channel.ResetWrite();
    weight_channel.ResetWrite();
    start_channel.ResetRead();
#if ENABLE_PERF_COUNTERS
    perf_counter_select.write(MatrixPerformance::SNAPSHOT_SEQUENCE);
#endif

    rstn.write(false);
    tick();
    tick();
    rstn.write(true);
    tick();

    std::cout << "RTL_WIDTHS base_c=" << BASE_C_WIDTH
              << " tile_c=" << Processor::Array::TILE_C_WIDTH
              << " reduced_c=" << Processor::C_WIDTH
              << " psum_c=" << DataTypes::int24::width << std::endl;

    static constexpr int kThroughputOperations = 24;
    const BufferVector throughput_expected = expected_partial(6, 60);
    for (int operation = 0; operation < kThroughputOperations; operation++) {
      std::ostringstream label;
      label << "throughput " << operation;
      expect_output(label.str(), throughput_expected, 0);
    }
    send_throughput_job(kThroughputOperations, 60, 6);
    while (static_cast<int>(throughput_output_cycles.size()) <
           kThroughputOperations) {
      tick();
    }
    report_throughput("input", throughput_input_cycles, kThroughputOperations);
    report_throughput("output", throughput_output_cycles,
                      kThroughputOperations);

    const BufferVector nominal_bias = queue_bias(0);
    BufferVector nominal_0 = nominal_bias;
    add_vector(nominal_0, expected_partial(0, 0));
    add_vector(nominal_0, expected_partial(0, 1));
    BufferVector nominal_1 = nominal_bias;
    add_vector(nominal_1, expected_partial(0, 2));
    add_vector(nominal_1, expected_partial(0, 3));
    expect_output("nominal address 0", nominal_0, 0);
    expect_output("nominal address 1", nominal_1, 0);
    send_job(make_accumulation_params(true), {0, 1, 2, 3},
             {true, true, true, true}, 0);

    const BufferVector reused_x = expected_partial(1, 20);
    for (int output_x = 0; output_x < 4; output_x++) {
      std::ostringstream label;
      label << "resident-weight reuse output X " << output_x;
      expect_output(label.str(), reused_x, 3);
    }
    send_job(make_weight_reuse_params(true), {20, 20, 20, 20},
             {true, false, false, false}, 1);

    const BufferVector reused_y = expected_partial(2, 30);
    for (int output_y = 0; output_y < 4; output_y++) {
      std::ostringstream label;
      label << "resident-weight reuse output Y " << output_y;
      expect_output(label.str(), reused_y, 3);
    }
    send_job(make_weight_reuse_params(false), {30, 30, 30, 30},
             {true, false, false, false}, 2);

    const BufferVector backpressure_bias = queue_bias(100);
    BufferVector backpressured = backpressure_bias;
    add_vector(backpressured, expected_partial(3, 40));
    add_vector(backpressured, expected_partial(3, 41));
    expect_output("backpressured temporal accumulation", backpressured, 25);
    send_job(make_backpressure_params(), {40, 41}, {true, true}, 3);

    const int expected_output_count = 11 + kThroughputOperations;
    while (checked_outputs < expected_output_count) {
      tick();
    }
    while (write_count[0] < 3 || read_count[0] < 3) {
      tick();
    }
    require(write_count[0] == 3,
            "expected three intermediate writes before double-buffered jobs");
    require(read_count[0] == 3,
            "expected three accumulation reads before double-buffered jobs");

#if DOUBLE_BUFFERED_ACCUM_BUFFER
    const BufferVector bank_0_expected = expected_partial(4, 50);
    send_job(make_double_buffer_params(), {50}, {true}, 4);
    const BufferVector bank_1_expected = expected_partial(5, 55);
    send_job(make_double_buffer_params(), {55}, {true}, 5);

    while (done_count[0] < 1 || done_count[1] < 1 || write_count[0] < 4 ||
           write_count[1] < 1) {
      tick();
    }
    std::ostringstream bank_0_write_message;
    bank_0_write_message
        << "expected four writes into accumulation bank 0, got "
        << write_count[0];
    require(write_count[0] == 4, bank_0_write_message.str());
    std::ostringstream bank_1_write_message;
    bank_1_write_message << "expected one write into accumulation bank 1, got "
                         << write_count[1];
    require(write_count[1] == 1, bank_1_write_message.str());
    for (int n = 0; n < N; n++) {
      std::ostringstream bank_0_data_message;
      bank_0_data_message << "bank 0 n " << n << " expected "
                          << bank_0_expected[n].int_val.to_int() << " got "
                          << accumulation_memory[0][0][n].int_val.to_int();
      require(accumulation_memory[0][0][n].int_val.to_int() ==
                  bank_0_expected[n].int_val.to_int(),
              bank_0_data_message.str());
      std::ostringstream bank_1_data_message;
      bank_1_data_message << "bank 1 n " << n << " expected "
                          << bank_1_expected[n].int_val.to_int() << " got "
                          << accumulation_memory[1][0][n].int_val.to_int();
      require(accumulation_memory[1][0][n].int_val.to_int() ==
                  bank_1_expected[n].int_val.to_int(),
              bank_1_data_message.str());
    }
#endif

    if (!test_failed) {
      std::cout << "[PASS] cim_processor_nominal_accumulation" << std::endl;
      std::cout << "[PASS] cim_processor_weight_reuse" << std::endl;
      std::cout << "[PASS] cim_processor_backpressure" << std::endl;
      std::cout << "[PASS] cim_processor_heterogeneous_jobs" << std::endl;
#if DOUBLE_BUFFERED_ACCUM_BUFFER
      std::cout << "[PASS] cim_processor_double_buffered_accumulation"
                << std::endl;
#endif
#if CIM_PROCESSOR_LARGE_TEST
      std::cout << "[PASS] cim_processor_large_64x64_macro" << std::endl;
#endif
    }
    sc_stop();
  }
};

// Elaborate the selected CIMProcessor geometry and scenario set
int sc_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  CIMProcessorTb testbench("cim_processor_int8");
  sc_start();
  return testbench.test_failed ? 1 : 0;
}
