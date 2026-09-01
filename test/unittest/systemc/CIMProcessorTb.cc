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

#ifndef CIM_PROCESSOR_TEST_STRICT_CADENCE
#define CIM_PROCESSOR_TEST_STRICT_CADENCE 0
#endif

static_assert(CIM_PROCESSOR_TEST_STRICT_CADENCE == 0 ||
                  CIM_PROCESSOR_TEST_STRICT_CADENCE == 1,
              "CIM_PROCESSOR_TEST_STRICT_CADENCE must be 0 or 1");

#ifndef CIM_PROCESSOR_TEST_B_SETS
#define CIM_PROCESSOR_TEST_B_SETS 8
#endif

// Macro MAC mode: 0 bit-parallel, 1 bit-serial. The same vectors must pass in
// either mode -- bit-serial only lengthens the issue window, so a mode change
// is a timing change and never a numerical one
#ifndef CIM_PROCESSOR_TEST_MODE
#define CIM_PROCESSOR_TEST_MODE 0
#endif

#if CIM_PROCESSOR_LARGE_TEST
static constexpr int CH_IN = 64;
static constexpr int CH_OUT = 64;
static constexpr int B_SETS = CIM_PROCESSOR_TEST_B_SETS;
static constexpr int BASE_A_WIDTH = 8;  // One A slice matches production II1
static constexpr int BASE_B_WIDTH = 8;
static constexpr int BASE_C_WIDTH = 20;
static constexpr int TILE_INPUT_AXIS_ELEMENTS = 1;
static constexpr int TILE_OUTPUT_AXIS_ELEMENTS = 2;
static constexpr int INPUT_AXIS_TILES = CIM_INPUT_AXIS_TILES;
static constexpr int OUTPUT_AXIS_TILES = CIM_OUTPUT_AXIS_TILES;
#else
static constexpr int CH_IN = 2;
static constexpr int CH_OUT = CIM_PROCESSOR_TEST_CH_OUT;
static constexpr int B_SETS = CIM_PROCESSOR_TEST_B_SETS;
static constexpr int BASE_A_WIDTH = CIM_PROCESSOR_TEST_BASE_A_WIDTH;
static constexpr int BASE_B_WIDTH = CIM_PROCESSOR_TEST_BASE_B_WIDTH;
static constexpr int BASE_C_WIDTH = CIM_PROCESSOR_TEST_BASE_C_WIDTH;
static constexpr int TILE_INPUT_AXIS_ELEMENTS = 2;
static constexpr int TILE_OUTPUT_AXIS_ELEMENTS = 1;
static constexpr int INPUT_AXIS_TILES = CIM_INPUT_AXIS_TILES;
static constexpr int OUTPUT_AXIS_TILES = CIM_OUTPUT_AXIS_TILES;
#endif

static constexpr int WRITE_CH_IN = 1;
static constexpr int MAC_LATENCY = 1;
static constexpr int MODE = CIM_PROCESSOR_TEST_MODE;
static constexpr int A_WIDTH = 8;
static constexpr int B_WIDTH = 8;
static constexpr bool SIGNED = true;
static constexpr int A_PORT_TILES = INPUT_AXIS_TILES;
#ifndef CIM_TEST_B_PORT_TILES
#define CIM_TEST_B_PORT_TILES 1
#endif
static constexpr int B_PORT_TILES = CIM_TEST_B_PORT_TILES;
static constexpr int C_PORT_TILES = OUTPUT_AXIS_TILES;
static constexpr int RESULT_SLOTS_PER_OUTPUT_LANE =
    CIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE;
static constexpr int K = CH_IN * TILE_INPUT_AXIS_ELEMENTS * INPUT_AXIS_TILES;
static constexpr int TILE_N =
    (CH_OUT / (B_WIDTH / BASE_B_WIDTH)) * TILE_OUTPUT_AXIS_ELEMENTS;
static constexpr int N = TILE_N * OUTPUT_AXIS_TILES;
static constexpr int BUFFER_DEPTH = 16;

using Processor = CIMProcessor<
    std::tuple<DataTypes::int8>, std::tuple<DataTypes::int8>, DataTypes::int8,
    DataTypes::int8, DataTypes::int24, DataTypes::int24, DataTypes::fp8_e8m0, K,
    N, BUFFER_DEPTH, CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH,
    BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY, MODE, SIGNED,
    TILE_INPUT_AXIS_ELEMENTS, TILE_OUTPUT_AXIS_ELEMENTS, INPUT_AXIS_TILES,
    OUTPUT_AXIS_TILES, A_PORT_TILES, B_PORT_TILES, C_PORT_TILES,
    CIM_C_BEAT_OUTPUT_MAJOR, RESULT_SLOTS_PER_OUTPUT_LANE,
    CIM_LOCAL_ACCUM_CONTEXTS>;

using Dut = CIMPROCESSOR_DUT_TYPE(Processor);
using Buffer = DataTypes::int24;
using BufferVector = Pack1D<Buffer, N>;
using WriteRequest = typename Processor::AccumulationWriteRequest;

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
  Connections::Combinational<CIMWeightDescriptor> weight_descriptor_channel;
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
  unsigned long last_write_completion_cycle[Processor::ACCUM_BUFFER_BANKS]
                                           [BUFFER_DEPTH];
  int ordered_dependent_read_count;
  std::vector<int> read_addresses[Processor::ACCUM_BUFFER_BANKS];
  std::vector<int> write_addresses[Processor::ACCUM_BUFFER_BANKS];
  unsigned long buffer_cycle;
  bool inject_stale_read = false;
  bool independent_read_probe = false;
  bool independent_read_seen = false;
  int independent_probe_writes_begin = 0;
  unsigned long independent_probe_hold_begin = 0;

  std::deque<ExpectedOutput> expected_outputs;
  std::deque<BufferVector> pending_biases;
  std::deque<CIMWeightDescriptor> pending_weight_descriptors;
  std::deque<int> pending_weight_sets;
  std::vector<unsigned long> throughput_input_cycles;
  std::vector<unsigned long> throughput_output_cycles;
  sc_event expected_output_event;
  sc_event bias_event;
  sc_event weight_descriptor_event;
  sc_event weight_event;
  sc_event weight_completion_event;
  int queued_weight_sets;
  int completed_weight_sets;
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
        ordered_dependent_read_count(0),
        buffer_cycle(0),
        queued_weight_sets(0),
        completed_weight_sets(0),
        checked_outputs(0),
        test_failed(false) {
    dut.clk(clk);
    dut.rstn(rstn);
    dut.input_channel(input_channel);
    dut.weight_channel(weight_channel);
    dut.weight_descriptor_channel(weight_descriptor_channel);
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
        last_write_completion_cycle[bank][address] = 0;
      }
    }

    SC_THREAD(run);
    sensitive << clk.posedge_event();

    SC_THREAD(drive_bias);
    sensitive << clk.posedge_event();

    SC_THREAD(drive_weight_descriptors);
    sensitive << clk.posedge_event();

    SC_THREAD(drive_weights);
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

  // Interleave two output addresses across two reduction contributions
  MatrixParams make_interleaved_accumulation_params() const {
    MatrixParams params = make_base_params();
    params.reduction_loop_idx[1] = 4;
    params.x_loop_idx[1] = 5;
    params.loops[1][params.reduction_loop_idx[1]] = 2;
    params.loops[1][params.x_loop_idx[1]] = 2;
    params.has_bias = true;
    return params;
  }

  // Replay each outer-reduction set across two live outer-X contexts
  MatrixParams make_set_major_replay_params() const {
    MatrixParams params = make_base_params();
    params.reduction_loop_idx[0] = 0;
    params.x_loop_idx[0] = 1;
    params.y_loop_idx[0] = 2;
    params.weight_loop_idx[0] = 3;
    params.fy_loop_idx[0] = 4;
    params.loops[0][params.reduction_loop_idx[0]] = 2;
    params.loops[0][params.x_loop_idx[0]] = 2;
    params.has_bias = false;
    return params;
  }

  // Exercise K/Y/X address carries when Y is the innermost output loop
  MatrixParams make_strided_accumulation_params() const {
    MatrixParams params = make_base_params();
    params.weight_loop_idx[1] = 2;
    params.x_loop_idx[1] = 3;
    params.reduction_loop_idx[1] = 4;
    params.y_loop_idx[1] = 5;
    params.loops[1][params.weight_loop_idx[1]] = 2;
    params.loops[1][params.x_loop_idx[1]] = 2;
    params.loops[1][params.reduction_loop_idx[1]] = 2;
    params.loops[1][params.y_loop_idx[1]] = 3;
    params.has_bias = false;
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

  // Create one direct output per independently loaded resident set
  MatrixParams make_weight_reload_params(int operations) const {
    MatrixParams params = make_base_params();
    params.loops[0][params.weight_loop_idx[0]] = operations;
    params.has_bias = false;
    return params;
  }

  // Create one output address with two contributions for the backpressure case
  MatrixParams make_backpressure_params() const {
    MatrixParams params = make_base_params();
    params.loops[1][params.reduction_loop_idx[1]] = 2;
    params.has_bias = true;
    return params;
  }

  // Create a resident set sequence replayed across two outer-X positions
  MatrixParams make_multiset_reuse_params(int set_count) const {
    MatrixParams params = make_base_params();
    params.weight_loop_idx[0] = 0;
    params.x_loop_idx[0] = 1;
    params.y_loop_idx[0] = 2;
    params.fy_loop_idx[0] = 3;
    params.reduction_loop_idx[0] = 4;
    params.loops[0][params.x_loop_idx[0]] = 2;
    params.loops[1][params.weight_loop_idx[1]] = set_count;
    params.has_bias = false;
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
    const ac_int<B_WIDTH, true> value =
        weight_pattern + 1 + output_axis_idx + k;
    return value.to_int();
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

  // Push one complete row-major resident-set payload
  void write_weight_set(int weight_pattern) {
    for (int k = 0; k < K; k++) {
      for (int span = 0; span < Processor::WEIGHT_BEATS_PER_ROW; span++) {
        weight_channel.Push(make_weight_beat(weight_pattern, k, span));
      }
    }
  }

  // Program every queued set from one channel-driving process
  void drive_weights() {
    weight_channel.ResetWrite();
    wait();
    while (!rstn.read()) {
      wait();
    }

    while (true) {
      if (pending_weight_sets.empty()) {
        wait(weight_event);
        continue;
      }
      const int weight_pattern = pending_weight_sets.front();
      pending_weight_sets.pop_front();
      write_weight_set(weight_pattern);
      completed_weight_sets++;
      weight_completion_event.notify(SC_ZERO_TIME);
    }
  }

  // Queue one set without coupling its weight load to input acceptance
  int queue_weight_set(int weight_pattern) {
    pending_weight_sets.push_back(weight_pattern);
    queued_weight_sets++;
    weight_event.notify(SC_ZERO_TIME);
    return queued_weight_sets;
  }

  // Wait until every queued set has reached the processor input
  void wait_for_weight_sets(int target) {
    while (completed_weight_sets < target) {
      wait(weight_completion_event);
    }
  }

  // Program one complete set before the caller continues
  void push_weight_set(int weight_pattern) {
    wait_for_weight_sets(queue_weight_set(weight_pattern));
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

  // Create one tile descriptor for consecutive physical sets
  CIMWeightDescriptor make_weight_descriptor(int set_count, int replay_count)
      const {
    CIMWeightDescriptor descriptor;
    descriptor.set_count = set_count;
    descriptor.replay_count = replay_count;
    return descriptor;
  }

  // Match WeightController's resident-capacity decision for a tile sequence
  std::vector<CIMWeightDescriptor> make_weight_descriptors(int set_count)
      const {
    std::vector<CIMWeightDescriptor> descriptors;
    if (set_count <= Processor::RESIDENT_SET_COUNT) {
      descriptors.push_back(make_weight_descriptor(set_count, 1));
    } else {
      for (int set = 0; set < set_count; set++) {
        descriptors.push_back(make_weight_descriptor(1, 1));
      }
    }
    return descriptors;
  }

  // Queue tile metadata without blocking the weight-data producer
  void queue_weight_descriptors(
      const std::vector<CIMWeightDescriptor> &descriptors) {
    for (const CIMWeightDescriptor &descriptor : descriptors) {
      pending_weight_descriptors.push_back(descriptor);
    }
    weight_descriptor_event.notify(SC_ZERO_TIME);
  }

  // Queue one expected output and its deliberate ready stall
  void expect_output(const std::string &label, const BufferVector &values,
                     int stall_cycles) {
    expected_outputs.push_back(ExpectedOutput{label, values, stall_cycles});
    expected_output_event.notify(SC_ZERO_TIME);
  }

  // Send one mapper job whose operations may use distinct input patterns
  void send_job(const MatrixParams &params,
                const std::vector<int> &weight_patterns,
                const std::vector<bool> &load_weights,
                const std::vector<CIMWeightDescriptor> &descriptors,
                const std::vector<int> &input_patterns) {
    require(weight_patterns.size() == load_weights.size() &&
                weight_patterns.size() == input_patterns.size(),
            "test job vectors must have equal lengths");
    queue_weight_descriptors(descriptors);
    params_channel.Push(params);
    start_channel.SyncPop();

    for (std::size_t operation = 0; operation < weight_patterns.size();
         operation++) {
      if (load_weights[operation]) {
        push_weight_set(weight_patterns[operation]);
      }
      input_channel.Push(make_inputs(input_patterns[operation]));
    }
  }

  // Send one mapper job with a shared input pattern
  void send_job(
      const MatrixParams &params, const std::vector<int> &weight_patterns,
      const std::vector<bool> &load_weights,
      const std::vector<CIMWeightDescriptor> &descriptors, int input_pattern) {
    send_job(params, weight_patterns, load_weights, descriptors,
             std::vector<int>(weight_patterns.size(), input_pattern));
  }

  // Check one job's exact accumulation-buffer address subsequence
  void require_address_sequence(
      const std::string &label, const std::vector<int> &addresses,
      std::size_t begin, const std::vector<int> &expected) {
    std::ostringstream count_message;
    count_message << label << " expected " << expected.size()
                  << " addresses got " << addresses.size() - begin;
    require(addresses.size() == begin + expected.size(), count_message.str());
    if (addresses.size() < begin + expected.size()) {
      return;
    }
    for (std::size_t index = 0; index < expected.size(); index++) {
      std::ostringstream address_message;
      address_message << label << " index " << index << " expected "
                      << expected[index] << " got " << addresses[begin + index];
      require(addresses[begin + index] == expected[index],
              address_message.str());
    }
  }

  // Check pinned registers and excess-row spills with arithmetic and address traces
  void check_context_rows(int rows, int contributions, bool interleaved,
                          bool banked, int bank, int reduction = 0) {
    MatrixParams params = make_base_params();
    const int reduction_position = interleaved ? 4 : 5;
    params.x_loop_idx[1] = interleaved ? 5 : 4;
    if (reduction == 1) {
      params.reduction_loop_idx[1] = 1;
      params.fx_loop_idx = reduction_position;
    } else if (reduction == 2) {
      params.reduction_loop_idx[1] = 0;
      params.fy_loop_idx[1] = reduction_position;
    } else {
      params.reduction_loop_idx[1] = reduction_position;
    }
    params.loops[1][params.x_loop_idx[1]] = rows;
    params.loops[1][reduction_position] = contributions;
    params.has_bias = true;
    params.write_output_to_accum_buffer = banked;
    const BufferVector bias = queue_bias(-300 - checked_outputs);
    const int outputs_before = checked_outputs;
    const int done_before = done_count[bank];
    const auto reads_begin = read_addresses[bank].size();
    const auto writes_begin = write_addresses[bank].size();
    std::vector<BufferVector> expected(rows, bias);
    std::vector<int> weights, inputs, expected_reads, expected_writes;
    std::vector<bool> loads;
    for (int operation = 0; operation < rows * contributions; operation++) {
      const int row = interleaved ? operation % rows : operation / contributions;
      const int term = interleaved ? operation / rows : operation % contributions;
      weights.push_back(220 + term);
      inputs.push_back(20 + row);
      loads.push_back((!interleaved && contributions > 1) || row == 0);
      add_vector(expected[row], expected_partial(inputs.back(), weights.back()));
      const bool local = row < CIM_LOCAL_ACCUM_CONTEXTS;
      if (!local && term > 0) expected_reads.push_back(row);
      if ((!local && term + 1 < contributions) ||
          (banked && term + 1 == contributions)) expected_writes.push_back(row);
    }
    if (!banked) {
      for (int row = 0; row < rows; row++) {
        expect_output("reusable context row " + std::to_string(row), expected[row], 25);
      }
    }
    send_job(params, weights, loads,
             make_weight_descriptors(interleaved || contributions == 1
                                         ? contributions : rows * contributions), inputs);
    while (banked ? (done_count[bank] == done_before ||
                    write_addresses[bank].size() < writes_begin + expected_writes.size())
                  : checked_outputs < outputs_before + rows) {
      tick();
    }
    require_address_sequence("context reads", read_addresses[bank], reads_begin, expected_reads);
    require_address_sequence("context writes", write_addresses[bank], writes_begin, expected_writes);
    if (banked) {
      for (int row = 0; row < rows; row++) {
        for (int lane = 0; lane < N; lane++) {
          require(accumulation_memory[bank][row][lane] == expected[row][lane],
                  "buffered context result mismatch");
        }
      }
    }
    std::cout << "CIM_CONTEXT_REUSE contexts=" << CIM_LOCAL_ACCUM_CONTEXTS
              << " rows=" << rows << " contributions=" << contributions
              << " interleaved=" << interleaved << " reduction=" << reduction
              << " intermediate_reads=" << expected_reads.size()
              << " intermediate_writes=" << expected_writes.size() - (banked ? rows : 0)
              << " final_writes=" << (banked ? rows : 0) << std::endl;
  }

  // Hold one spill write until a later spill reads its committed partial sum
  void check_independent_sram_read() {
    independent_read_probe = true;
    independent_read_seen = false;
    independent_probe_writes_begin = write_count[0];
    independent_probe_hold_begin = 0;
    check_context_rows(CIM_LOCAL_ACCUM_CONTEXTS + 3, 3, true, false, 0);
    independent_read_probe = false;
    require(independent_read_seen,
            "independent SRAM read waited for an unrelated write");
    if (independent_read_seen) {
      std::cout << "[PASS] cim_processor_independent_sram_read" << std::endl;
    }
  }

  // Release a deliberately stalled write only after the unrelated read progresses
  bool hold_independent_probe_write() {
    if (!independent_read_probe || independent_read_seen ||
        write_count[0] < independent_probe_writes_begin + 3) {
      return false;
    }
    if (independent_probe_hold_begin == 0) {
      independent_probe_hold_begin = buffer_cycle;
    }
    if (buffer_cycle - independent_probe_hold_begin < 1000) {
      return true;
    }
    require(false, "unrelated SRAM write blocked read progress for 1000 cycles");
    independent_read_probe = false;
    return false;
  }

  // Preload resident tiles before admitting their MAC inputs
  void send_preloaded_tiles_job(
      const MatrixParams &params, const std::vector<int> &weight_patterns,
      const std::vector<CIMWeightDescriptor> &descriptors, int input_pattern,
      std::vector<unsigned long> *weight_completion_cycles = nullptr) {
    std::size_t scheduled_set_count = 0;
    for (const CIMWeightDescriptor &descriptor : descriptors) {
      scheduled_set_count += descriptor.set_count.to_uint();
    }
    require(scheduled_set_count == weight_patterns.size(),
            "preloaded tile sizes must match the weight sequence");
    queue_weight_descriptors(descriptors);
    params_channel.Push(params);
    start_channel.SyncPop();

    for (const int weight_pattern : weight_patterns) {
      push_weight_set(weight_pattern);
      if (weight_completion_cycles != nullptr) {
        weight_completion_cycles->push_back(current_cycle());
      }
    }
    for (std::size_t operation = 0; operation < weight_patterns.size();
         operation++) {
      input_channel.Push(make_inputs(input_pattern));
    }
  }

  // Interleave final uses from a full tile with the next tile's ring refills
  void send_progressive_release_job(int input_pattern, int weight_pattern) {
    queue_weight_descriptors(
        {make_weight_descriptor(B_SETS, 1), make_weight_descriptor(2, 1)});
    params_channel.Push(make_weight_reload_params(B_SETS + 2));
    start_channel.SyncPop();

    for (int set = 0; set < B_SETS; set++) {
      push_weight_set(weight_pattern + set);
    }

    // The next push cannot start until the first old set is released
    input_channel.Push(make_inputs(input_pattern));
    push_weight_set(weight_pattern + B_SETS);
    input_channel.Push(make_inputs(input_pattern));
    push_weight_set(weight_pattern + B_SETS + 1);

    for (int set = 2; set < B_SETS; set++) {
      input_channel.Push(make_inputs(input_pattern));
    }
    input_channel.Push(make_inputs(input_pattern));
    input_channel.Push(make_inputs(input_pattern));
  }

  // Refill the complete ring while the next descriptor is already runnable
  void send_consecutive_full_ring_replay_job(
      int input_pattern, int first_weight_pattern, int second_weight_pattern) {
    static constexpr int kReplaysPerDescriptor = 2;
    MatrixParams params = make_multiset_reuse_params(B_SETS);
    params.loops[0][params.x_loop_idx[0]] = 2 * kReplaysPerDescriptor;
    queue_weight_descriptors(
        {make_weight_descriptor(B_SETS, kReplaysPerDescriptor),
         make_weight_descriptor(B_SETS, kReplaysPerDescriptor)});
    params_channel.Push(params);
    start_channel.SyncPop();

    for (int set = 0; set < B_SETS; set++) {
      push_weight_set(first_weight_pattern + set);
    }

    int completion_target = completed_weight_sets;
    for (int set = 0; set < B_SETS; set++) {
      completion_target = queue_weight_set(second_weight_pattern + set);
    }

    for (int descriptor = 0; descriptor < 2; descriptor++) {
      for (int replay = 0; replay < kReplaysPerDescriptor; replay++) {
        for (int set = 0; set < B_SETS; set++) {
          input_channel.Push(make_inputs(input_pattern));
        }
      }
    }
    wait_for_weight_sets(completion_target);
  }

  // Return the current clock index for ready/valid cadence measurements
  unsigned long current_cycle() const {
    return static_cast<unsigned long>(sc_time_stamp() / sc_time(10, SC_NS));
  }

  // Send a no-accumulation stream that reuses one resident weight set
  void send_throughput_job(int operations, int weight_pattern,
                           int input_pattern) {
    queue_weight_descriptors({make_weight_descriptor(1, operations)});
    params_channel.Push(make_throughput_params(operations));
    start_channel.SyncPop();

    push_weight_set(weight_pattern);

    for (int operation = 0; operation < operations; operation++) {
      input_channel.Push(make_inputs(input_pattern));
      throughput_input_cycles.push_back(current_cycle());
    }
  }

  // Print acceptance intervals and validate the configured cadence
  void report_throughput(const char *name,
                         const std::vector<unsigned long> &cycles,
                         int expected_count, int expected_interval) {
    std::ostringstream count_message;
    count_message << name << " expected " << expected_count << " samples got "
                  << cycles.size();
    require(static_cast<int>(cycles.size()) == expected_count,
            count_message.str());

    std::cout << "CIM_PROCESSOR_CADENCE " << name << "_intervals=";
    for (std::size_t index = 1; index < cycles.size(); index++) {
      if (index > 1) {
        std::cout << ",";
      }
      const unsigned long interval = cycles[index] - cycles[index - 1];
      std::cout << interval;
      if (expected_interval > 0) {
        std::ostringstream interval_message;
        interval_message << name << " interval " << index << " expected "
                         << expected_interval << " got " << interval;
        require(interval == static_cast<unsigned long>(expected_interval),
                interval_message.str());
      }
    }
    std::cout << std::endl;
  }

  // Require descriptor grouping to preserve resident-set load cadence
  void require_matching_weight_intervals(
      const std::vector<unsigned long> &split_cycles,
      const std::vector<unsigned long> &grouped_cycles) {
    require(split_cycles.size() == grouped_cycles.size(),
            "descriptor cadence sample counts differ");
    for (std::size_t index = 1;
         index < split_cycles.size() && index < grouped_cycles.size();
         index++) {
      const unsigned long split_interval =
          split_cycles[index] - split_cycles[index - 1];
      const unsigned long grouped_interval =
          grouped_cycles[index] - grouped_cycles[index - 1];
      std::cout << "CIM_DESCRIPTOR_CADENCE set=" << index
                << " split=" << split_interval
                << " grouped=" << grouped_interval << std::endl;
      std::ostringstream message;
      message << "descriptor boundary changed set " << index
              << " load interval from " << grouped_interval << " to "
              << split_interval;
      require(split_interval == grouped_interval, message.str());
    }
  }

#if ENABLE_PERF_COUNTERS
  // Read one stable synthesized performance snapshot counter
  MatrixPerformance::Counter read_performance_counter(
      MatrixPerformance::CounterId counter_id) {
    perf_counter_select.write(counter_id);
    tick();
    tick();
    return perf_counter_value.read();
  }

  // Report direct completion-storage stalls for the throughput job
  void report_completion_storage_counters() {
    perf_counter_select.write(MatrixPerformance::SNAPSHOT_SEQUENCE);
    tick();
    tick();
    while (perf_counter_value.read() == 0) {
      tick();
    }

    const MatrixPerformance::Counter active_cycles =
        read_performance_counter(MatrixPerformance::PROCESSOR_ACTIVE_CYCLES);
    const MatrixPerformance::Counter issue_cycles =
        read_performance_counter(MatrixPerformance::ARRAY_ISSUE_CYCLES);
    const MatrixPerformance::Counter input_backpressure_cycles =
        read_performance_counter(MatrixPerformance::INPUT_BACKPRESSURE_CYCLES);
    const MatrixPerformance::Counter set_fills =
        read_performance_counter(MatrixPerformance::CIM_SET_FILLS);
    const MatrixPerformance::Counter weight_load_bytes =
        read_performance_counter(MatrixPerformance::CIM_WEIGHT_LOAD_BYTES);
    const MatrixPerformance::Counter weight_load_cycles =
        read_performance_counter(MatrixPerformance::CIM_WEIGHT_LOAD_CYCLES);
    const MatrixPerformance::Counter completion_storage_stall_cycles =
        read_performance_counter(
            MatrixPerformance::CIM_COMPLETION_STORAGE_STALL_CYCLES);
    const MatrixPerformance::Counter result_slot_stall_cycles =
        read_performance_counter(
            MatrixPerformance::CIM_RESULT_SLOT_STALL_CYCLES);
    const MatrixPerformance::Counter completion_descriptor_stall_cycles =
        read_performance_counter(
            MatrixPerformance::CIM_COMPLETION_DESCRIPTOR_STALL_CYCLES);

    std::cout << "RTL_COMPLETION_COUNTER input_axis_tiles=" << INPUT_AXIS_TILES
              << " output_axis_tiles=" << OUTPUT_AXIS_TILES
              << " slots_per_output_lane=" << RESULT_SLOTS_PER_OUTPUT_LANE
              << " active_cycles=" << active_cycles
              << " issue_cycles=" << issue_cycles
              << " input_backpressure_cycles=" << input_backpressure_cycles
              << " set_fills=" << set_fills
              << " weight_load_bytes=" << weight_load_bytes
              << " weight_load_cycles=" << weight_load_cycles
              << " completion_storage_stall_cycles="
              << completion_storage_stall_cycles
              << " result_slot_stall_cycles=" << result_slot_stall_cycles
              << " completion_descriptor_stall_cycles="
              << completion_descriptor_stall_cycles << std::endl;

    const unsigned bytes_per_set = K * N * ((B_WIDTH + 7) / 8);
    const unsigned bytes_per_beat =
        (Processor::WEIGHT_WRITE_WIDTH + 7) / 8;
    require(set_fills > 0, "hardware counter missed resident-set fills");
    require(weight_load_bytes == set_fills * bytes_per_set,
            "hardware weight-load byte count does not match completed sets");
    require(weight_load_cycles >= weight_load_bytes / bytes_per_beat,
            "hardware weight-load cycles are shorter than accepted transfers");
  }
#endif

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

  // Drive resident-tile metadata independently of the weight beat stream
  void drive_weight_descriptors() {
    weight_descriptor_channel.ResetWrite();
    wait();
    while (!rstn.read()) {
      wait();
    }

    while (true) {
      if (pending_weight_descriptors.empty()) {
        wait(weight_descriptor_event);
        continue;
      }
      const CIMWeightDescriptor descriptor = pending_weight_descriptors.front();
      pending_weight_descriptors.pop_front();
      weight_descriptor_channel.Push(descriptor);
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
      BufferVector value = accumulation_memory[0][pending_read_address[0].to_int()];
      if (inject_stale_read) {
        value[0].int_val += 1;
        inject_stale_read = false;
        std::cout << "Injecting stale partial sum for the simulation checker" << std::endl;
      }
      accumulation_read_data_0.Push(value);
      pending_read[0] = false;
    }

    ac_int<16, false> address;
    if (!pending_read[0] && buffer_cycle % 3 == 0 &&
        accumulation_read_address_0.PopNB(address)) {
      require(write_count[0] > read_count[0] &&
                  last_write_completion_cycle[0][address.to_int()] != 0,
              "bank 0 dependent read preceded its physical write completion");
      ordered_dependent_read_count++;
      pending_read[0] = true;
      pending_read_address[0] = address;
      pending_read_ready_cycle[0] = buffer_cycle + 2;
      read_addresses[0].push_back(address.to_int());
      read_count[0]++;
      if (independent_read_probe && address == CIM_LOCAL_ACCUM_CONTEXTS + 2) {
        independent_read_seen = true;
      }
    }

    WriteRequest write;
    if (!hold_independent_probe_write() && buffer_cycle % 4 == 0 &&
        accumulation_write_request_0.PopNB(write)) {
      accumulation_memory[0][write.address.to_int()] = write.data;
      last_write_completion_cycle[0][write.address.to_int()] = buffer_cycle + 1;
      write_addresses[0].push_back(write.address.to_int());
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
      require(write_count[1] > read_count[1] &&
                  last_write_completion_cycle[1][address.to_int()] != 0,
              "bank 1 dependent read preceded its physical write completion");
      ordered_dependent_read_count++;
      pending_read[1] = true;
      pending_read_address[1] = address;
      pending_read_ready_cycle[1] = buffer_cycle + 2;
      read_addresses[1].push_back(address.to_int());
      read_count[1]++;
    }

    WriteRequest write;
    if (buffer_cycle % 4 == 1 && accumulation_write_request_1.PopNB(write)) {
      accumulation_memory[1][write.address.to_int()] = write.data;
      last_write_completion_cycle[1][write.address.to_int()] = buffer_cycle + 1;
      write_addresses[1].push_back(write.address.to_int());
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
    report_throughput("input", throughput_input_cycles, kThroughputOperations,
                      0);
    const int expected_output_interval =
        CIM_PROCESSOR_TEST_STRICT_CADENCE ? Processor::Array::MAC_ISSUE_WINDOW
                                          : 0;
    report_throughput("output", throughput_output_cycles, kThroughputOperations,
                      expected_output_interval);
#if ENABLE_PERF_COUNTERS
    report_completion_storage_counters();
#endif

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
             {true, true, true, true}, make_weight_descriptors(4), 0);

    const BufferVector reused_x = expected_partial(1, 20);
    for (int output_x = 0; output_x < 4; output_x++) {
      std::ostringstream label;
      label << "resident-weight reuse output X " << output_x;
      expect_output(label.str(), reused_x, 3);
    }
    send_job(make_weight_reuse_params(true), {20, 20, 20, 20},
             {true, false, false, false}, {make_weight_descriptor(1, 4)}, 1);

    const BufferVector reused_y = expected_partial(2, 30);
    for (int output_y = 0; output_y < 4; output_y++) {
      std::ostringstream label;
      label << "resident-weight reuse output Y " << output_y;
      expect_output(label.str(), reused_y, 3);
    }
    send_job(make_weight_reuse_params(false), {30, 30, 30, 30},
             {true, false, false, false}, {make_weight_descriptor(1, 4)}, 2);

    const BufferVector backpressure_bias = queue_bias(100);
    BufferVector backpressured = backpressure_bias;
    add_vector(backpressured, expected_partial(3, 40));
    add_vector(backpressured, expected_partial(3, 41));
    expect_output("backpressured temporal accumulation", backpressured, 25);
    send_job(make_backpressure_params(), {40, 41}, {true, true},
             make_weight_descriptors(2), 3);

    const BufferVector interleaved_bias = queue_bias(200);
    BufferVector interleaved_0 = interleaved_bias;
    add_vector(interleaved_0, expected_partial(12, 120));
    add_vector(interleaved_0, expected_partial(12, 121));
    BufferVector interleaved_1 = interleaved_bias;
    add_vector(interleaved_1, expected_partial(13, 120));
    add_vector(interleaved_1, expected_partial(13, 121));
    expect_output("interleaved accumulation address 0", interleaved_0, 0);
    expect_output("interleaved accumulation address 1", interleaved_1, 0);
    send_job(make_interleaved_accumulation_params(), {120, 120, 121, 121},
             {true, false, true, false}, {make_weight_descriptor(2, 1)},
             {12, 13, 12, 13});

    while (checked_outputs < kThroughputOperations + 13) {
      tick();
    }

    static constexpr int kSetMajorFirstInput = 14;
    static constexpr int kSetMajorSecondInput = 15;
    static constexpr int kSetMajorFirstWeight = 160;
    static constexpr int kSetMajorSecondWeight = 161;
    BufferVector set_major_0 =
        expected_partial(kSetMajorFirstInput, kSetMajorFirstWeight);
    add_vector(set_major_0,
               expected_partial(kSetMajorFirstInput, kSetMajorSecondWeight));
    BufferVector set_major_1 =
        expected_partial(kSetMajorSecondInput, kSetMajorFirstWeight);
    add_vector(set_major_1,
               expected_partial(kSetMajorSecondInput, kSetMajorSecondWeight));
    const int set_major_outputs_before = checked_outputs;
    const std::size_t set_major_writes_begin = write_addresses[0].size();
    const std::size_t set_major_reads_begin = read_addresses[0].size();
    expect_output("set-major replay outer X 0", set_major_0, 0);
    expect_output("set-major replay outer X 1", set_major_1, 0);
    send_job(make_set_major_replay_params(),
             {kSetMajorFirstWeight, kSetMajorFirstWeight, kSetMajorSecondWeight,
              kSetMajorSecondWeight},
             {true, false, true, false},
             {make_weight_descriptor(1, 2), make_weight_descriptor(1, 2)},
             {kSetMajorFirstInput, kSetMajorSecondInput, kSetMajorFirstInput,
              kSetMajorSecondInput});
    while (checked_outputs < set_major_outputs_before + 2) {
      tick();
    }
    const std::vector<int> set_major_buffer_addresses =
        CIM_LOCAL_ACCUM_CONTEXTS >= 2
            ? std::vector<int>{}
            : std::vector<int>{1};
    // With R>=2 both live indices remain in local accum contexts
    require_address_sequence("set-major local accum context writes",
                             write_addresses[0], set_major_writes_begin,
                             set_major_buffer_addresses);
    require_address_sequence("set-major local accum context reads",
                             read_addresses[0], set_major_reads_begin,
                             set_major_buffer_addresses);

    static constexpr int kStridedInputPattern = 13;
    static constexpr int kStridedWeightBase = 130;
    static constexpr int kStridedK = 2;
    static constexpr int kStridedX = 2;
    static constexpr int kStridedReduction = 2;
    static constexpr int kStridedY = 3;
    static constexpr int kStridedOutputCount =
        kStridedK * kStridedX * kStridedY;
    static constexpr int kBufferBackedStridedContexts =
        kStridedOutputCount > CIM_LOCAL_ACCUM_CONTEXTS
            ? kStridedOutputCount - CIM_LOCAL_ACCUM_CONTEXTS
            : 0;
    static constexpr int kBufferBackedWritesPerStridedJob =
        kBufferBackedStridedContexts * (kStridedReduction - 1);
    static constexpr int kStridedJobCount = 2;
    std::vector<int> strided_weight_patterns;
    std::vector<bool> strided_weight_loads;
    for (int k = 0; k < kStridedK; k++) {
      for (int x = 0; x < kStridedX; x++) {
        for (int reduction = 0; reduction < kStridedReduction; reduction++) {
          for (int y = 0; y < kStridedY; y++) {
            const int weight_pattern =
                kStridedWeightBase + k * kStridedReduction + reduction;
            strided_weight_patterns.push_back(weight_pattern);
            strided_weight_loads.push_back(y == 0);
          }
        }
      }
    }
    // Repeat strided reductions without resetting between jobs
    for (int job = 0; job < kStridedJobCount; job++) {
      for (int k = 0; k < kStridedK; k++) {
        for (int x = 0; x < kStridedX; x++) {
          for (int y = 0; y < kStridedY; y++) {
            BufferVector expected = expected_partial(
                kStridedInputPattern,
                kStridedWeightBase + k * kStridedReduction);
            add_vector(expected,
                       expected_partial(kStridedInputPattern,
                                        kStridedWeightBase +
                                            k * kStridedReduction + 1));
            std::ostringstream label;
            label << "strided accumulation job " << job << " K " << k
                  << " X " << x << " Y " << y;
            expect_output(label.str(), expected, 0);
          }
        }
      }
      send_job(
          make_strided_accumulation_params(), strided_weight_patterns,
          strided_weight_loads,
          make_weight_descriptors(kStridedK * kStridedX * kStridedReduction),
          kStridedInputPattern);
    }

    if constexpr (B_SETS >= 2) {
      std::vector<int> full_set_patterns;
      std::vector<bool> full_set_loads;
      for (int replay = 0; replay < 2; replay++) {
        for (int set = 0; set < B_SETS; set++) {
          std::ostringstream label;
          label << "full-set replay " << replay << " set " << set;
          expect_output(label.str(), expected_partial(8, 8 + set), 0);
          full_set_patterns.push_back(8 + set);
          full_set_loads.push_back(replay == 0);
        }
      }
      send_job(make_multiset_reuse_params(B_SETS), full_set_patterns,
               full_set_loads, {make_weight_descriptor(B_SETS, 2)}, 8);

      static constexpr int kConsecutiveInputPattern = 16;
      static constexpr int kConsecutiveFirstWeightBase = 180;
      static constexpr int kConsecutiveSecondWeightBase = 200;
      for (int descriptor = 0; descriptor < 2; descriptor++) {
        const int weight_base = descriptor == 0 ? kConsecutiveFirstWeightBase
                                                : kConsecutiveSecondWeightBase;
        for (int replay = 0; replay < 2; replay++) {
          for (int set = 0; set < B_SETS; set++) {
            std::ostringstream label;
            label << "consecutive full-ring descriptor " << descriptor
                  << " replay " << replay << " set " << set;
            expect_output(
                label.str(),
                expected_partial(kConsecutiveInputPattern, weight_base + set),
                0);
          }
        }
      }
      send_consecutive_full_ring_replay_job(kConsecutiveInputPattern,
                                            kConsecutiveFirstWeightBase,
                                            kConsecutiveSecondWeightBase);

      std::vector<CIMWeightDescriptor> single_set_descriptors;
      std::vector<int> individual_set_patterns;
      std::vector<unsigned long> individual_set_load_cycles;
      for (int set = 0; set < B_SETS; set++) {
        std::ostringstream label;
        label << "individual-set preload " << set;
        expect_output(label.str(), expected_partial(9, 44 + set), 0);
        single_set_descriptors.push_back(make_weight_descriptor(1, 1));
        individual_set_patterns.push_back(44 + set);
      }
      const int individual_outputs_before = checked_outputs;
      send_preloaded_tiles_job(make_weight_reload_params(B_SETS),
                               individual_set_patterns, single_set_descriptors,
                               9, &individual_set_load_cycles);
      while (checked_outputs < individual_outputs_before + B_SETS) {
        tick();
      }

      std::vector<int> grouped_set_patterns;
      std::vector<unsigned long> grouped_set_load_cycles;
      for (int set = 0; set < B_SETS; set++) {
        std::ostringstream label;
        label << "grouped-set preload " << set;
        expect_output(label.str(), expected_partial(10, 64 + set), 0);
        grouped_set_patterns.push_back(64 + set);
      }
      const int grouped_outputs_before = checked_outputs;
      send_preloaded_tiles_job(
          make_weight_reload_params(B_SETS), grouped_set_patterns,
          {make_weight_descriptor(B_SETS, 1)}, 10, &grouped_set_load_cycles);
      while (checked_outputs < grouped_outputs_before + B_SETS) {
        tick();
      }
      require_matching_weight_intervals(individual_set_load_cycles,
                                        grouped_set_load_cycles);

      std::vector<int> variable_tile_patterns;
      for (int set = 0; set < B_SETS; set++) {
        std::ostringstream label;
        label << "variable-tile preload set " << set;
        expect_output(label.str(), expected_partial(10, 80 + set), 0);
        variable_tile_patterns.push_back(80 + set);
      }
      send_preloaded_tiles_job(
          make_weight_reload_params(B_SETS), variable_tile_patterns,
          {make_weight_descriptor(1, 1), make_weight_descriptor(B_SETS - 1, 1)},
          10);

      static constexpr int kProgressivePatternBase = 100;
      for (int set = 0; set < B_SETS + 2; set++) {
        std::ostringstream label;
        label << "progressive release set " << set;
        expect_output(label.str(),
                      expected_partial(11, kProgressivePatternBase + set), 0);
      }
      send_progressive_release_job(11, kProgressivePatternBase);
    }

    const int expected_output_count = 15 +
                                      kStridedJobCount * kStridedOutputCount +
                                      kThroughputOperations +
                                      (B_SETS >= 2 ? 10 * B_SETS + 2 : 0);
    while (checked_outputs < expected_output_count) {
      tick();
    }
    static constexpr int kBufferBackedTwoContextJobs =
        CIM_LOCAL_ACCUM_CONTEXTS < 2 ? 3 : 0;
    static constexpr int kExpectedPartialSumTransfers =
        kBufferBackedTwoContextJobs +
        kStridedJobCount * kBufferBackedWritesPerStridedJob;
    require(write_count[0] == kExpectedPartialSumTransfers,
            "unexpected buffer-backed reduction write count before banked jobs");
    require(read_count[0] == kExpectedPartialSumTransfers,
            "unexpected buffer-backed reduction read count before banked jobs");
    if constexpr (kBufferBackedStridedContexts > 0) {
      require(ordered_dependent_read_count > 0,
              "buffer-backed reductions must exercise ordered dependent reads");
    }
    std::cout << "CIM_ACCUMULATION_TRAFFIC local_accum_contexts="
              << CIM_LOCAL_ACCUM_CONTEXTS
              << " strided_live=" << kStridedOutputCount
              << " buffer_backed_strided=" << kBufferBackedStridedContexts
              << " reads=" << read_count[0]
              << " writes=" << write_count[0]
              << " ordered_dependent_reads="
              << ordered_dependent_read_count << std::endl;

#if DOUBLE_BUFFERED_ACCUM_BUFFER
    const int bank_0_writes_before_banked_jobs = write_count[0];
    const int bank_1_writes_before_banked_jobs = write_count[1];
    const int bank_0_done_before_banked_jobs = done_count[0];
    const int bank_1_done_before_banked_jobs = done_count[1];
    const BufferVector bank_0_expected = expected_partial(4, 50);
    send_job(make_double_buffer_params(), {50}, {true},
             {make_weight_descriptor(1, 1)}, 4);
    const BufferVector bank_1_expected = expected_partial(5, 55);
    send_job(make_double_buffer_params(), {55}, {true},
             {make_weight_descriptor(1, 1)}, 5);

    while (done_count[0] < bank_0_done_before_banked_jobs + 1 ||
           done_count[1] < bank_1_done_before_banked_jobs + 1 ||
           write_count[0] < bank_0_writes_before_banked_jobs + 1 ||
           write_count[1] < bank_1_writes_before_banked_jobs + 1) {
      tick();
    }
    std::ostringstream bank_0_write_message;
    bank_0_write_message << "expected one additional write into accumulation "
                            "bank 0, got "
                         << write_count[0] - bank_0_writes_before_banked_jobs;
    require(write_count[0] == bank_0_writes_before_banked_jobs + 1,
            bank_0_write_message.str());
    std::ostringstream bank_1_write_message;
    bank_1_write_message << "expected one additional write into accumulation "
                            "bank 1, got "
                         << write_count[1] - bank_1_writes_before_banked_jobs;
    require(write_count[1] == bank_1_writes_before_banked_jobs + 1,
            bank_1_write_message.str());
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

    // A direct result after banked jobs detects any leaked final-output entry
    const int outputs_before_direct_resume = checked_outputs;
    const BufferVector direct_resume_expected = expected_partial(7, 70);
    expect_output("direct output after double buffering",
                  direct_resume_expected, 0);
    send_job(make_throughput_params(1), {70}, {true},
             {make_weight_descriptor(1, 1)}, 7);
    while (checked_outputs == outputs_before_direct_resume) {
      tick();
    }
#endif

    if constexpr (CIM_LOCAL_ACCUM_CONTEXTS + 3 <= BUFFER_DEPTH) {
      check_independent_sram_read();
    }

    if (!test_failed) {
      if constexpr (CIM_PROCESSOR_TEST_STRICT_CADENCE) {
        std::cout << "[PASS] cim_processor_result_throughput" << std::endl;
      } else {
        std::cout << "[PASS] cim_processor_result_stream" << std::endl;
      }
      std::cout << "[PASS] cim_processor_nominal_accumulation" << std::endl;
      std::cout << "[PASS] cim_processor_local_accumulation" << std::endl;
      std::cout << "[PASS] cim_processor_buffer_write_completion_ordering"
                << std::endl;
      std::cout << "[PASS] cim_processor_strided_accumulation" << std::endl;
      std::cout << "[PASS] cim_processor_set_major_replay" << std::endl;
      std::cout << "[PASS] cim_processor_weight_reuse" << std::endl;
      std::cout << "[PASS] cim_processor_backpressure" << std::endl;
      std::cout << "[PASS] cim_processor_heterogeneous_jobs" << std::endl;
      if constexpr (B_SETS >= 2) {
        std::cout << "[PASS] cim_processor_full_set_replay" << std::endl;
        std::cout << "[PASS] cim_processor_consecutive_full_ring_descriptors"
                  << std::endl;
        std::cout << "[PASS] cim_processor_individual_set_preload" << std::endl;
        std::cout << "[PASS] cim_processor_variable_tile_preload" << std::endl;
        std::cout << "[PASS] cim_processor_progressive_release" << std::endl;
      }
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
  CIMProcessorTb testbench("cim_processor_int8");
  testbench.inject_stale_read =
      argc > 1 && std::string(argv[1]) == "--inject-stale-read";
  sc_start();
  return testbench.test_failed ? 1 : 0;
}
