// Combined SystemC and Catapult SCVerify tests for the standalone CIMArray

#ifdef SCVERIFY
#include <mc_scverify.h>
#define CIMARRAY_DUT_TYPE(T) CCS_DESIGN(T)
#else
#define CIMARRAY_DUT_TYPE(T) T
#endif

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include <deque>
#include <iostream>
#include <sstream>
#include <string>

#include "CIMArray.h"

static constexpr int CIM_MODE_BIT_PARALLEL_VALUE = 0;
static constexpr int CIM_MODE_BIT_SERIAL_VALUE = 1;

#ifndef CIM_TEST_B_PORT_TILES
#define CIM_TEST_B_PORT_TILES 3
#endif

#ifndef CIM_TEST_C_BEAT_LAYOUT
#define CIM_TEST_C_BEAT_LAYOUT CIM_C_BEAT_INPUT_MAJOR
#endif

#ifndef CIM_TEST_C_WIDTH
#define CIM_TEST_C_WIDTH 20
#endif

#ifndef CIM_ARRAY_TEST_RESULT_SLOTS_PER_OUTPUT_LANE
#define CIM_ARRAY_TEST_RESULT_SLOTS_PER_OUTPUT_LANE 2
#endif

static int g_cases_remaining = 0;

// Return a mask covering the requested bit width
static constexpr long long mask_for_width(int width) {
  return (1LL << width) - 1;
}

// Exercise one parameterized CIMArray geometry using logical A/B/C coordinates
template <
    int CH_IN, int CH_OUT, int B_SETS, int BASE_A_WIDTH, int BASE_B_WIDTH,
    int BASE_C_WIDTH, int WRITE_CH_IN, int MAC_LATENCY, int MODE, int A_WIDTH,
    int B_WIDTH, bool IS_SIGNED, int TILE_INPUT_AXIS_ELEMENTS,
    int TILE_OUTPUT_AXIS_ELEMENTS, int INPUT_AXIS_TILES = 1,
    int OUTPUT_AXIS_TILES = 1, int A_PORT_TILES = INPUT_AXIS_TILES,
    int B_PORT_TILES = OUTPUT_AXIS_TILES, int C_PORT_TILES = INPUT_AXIS_TILES,
    int C_BEAT_LAYOUT = CIM_C_BEAT_INPUT_MAJOR,
    // Accumulator width; the processor takes this from ACCUM_DATATYPE, so the
    // standalone testbench over-provisions from the base width. The array's own
    // static_assert enforces that it holds one reduced result
    int C_WIDTH = BASE_C_WIDTH + 8,
    int RESULT_SLOTS_PER_OUTPUT_LANE = INPUT_AXIS_TILES,
    typename DutType = CIMArray<
        CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH, BASE_C_WIDTH,
        WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH, B_WIDTH, C_WIDTH, IS_SIGNED,
        TILE_INPUT_AXIS_ELEMENTS, TILE_OUTPUT_AXIS_ELEMENTS, INPUT_AXIS_TILES,
        OUTPUT_AXIS_TILES, A_PORT_TILES, B_PORT_TILES, C_PORT_TILES,
        C_BEAT_LAYOUT, RESULT_SLOTS_PER_OUTPUT_LANE>>
struct CIMArrayTbCase : sc_module {
  using Dut = DutType;
  using ABeat = typename Dut::ABeat;
  using CBeat = typename Dut::CBeat;
  using MACRequest = typename Dut::MACRequest;
  using WriteRequest = typename Dut::WriteRequest;
  using Set = typename Dut::Set;

  static_assert(A_WIDTH < 31, "A_WIDTH must fit this unit test golden model");
  static_assert(B_WIDTH < 31, "B_WIDTH must fit this unit test golden model");
  static_assert(C_WIDTH < 62, "C_WIDTH must fit this unit test golden model");

  CIMARRAY_DUT_TYPE(Dut) dut;
  sc_clock clk;
  sc_signal<bool> rstn;
  Connections::Combinational<MACRequest> mac_request_channel;
  Connections::Combinational<WriteRequest> write_request_channel;
  Connections::Combinational<CBeat> result_channel;
#if ENABLE_PERF_COUNTERS
  sc_signal<bool> completion_storage_stall;
#endif
  unsigned mac_accept_count = 0;
  unsigned total_mac_accept_count = 0;
  unsigned result_fire_count = 0;
  unsigned long total_cycles = 0;
  unsigned long last_mac_fire_cycle = 0;
  unsigned long last_result_fire_cycle = 0;
#if ENABLE_PERF_COUNTERS
  unsigned long completion_storage_stall_cycles = 0;
#endif
  unsigned blocked_mac_accept_start = 0;

  ac_int<B_WIDTH, false> expected_b[B_SETS][Dut::K][Dut::N];

  // ExpectedBeat mirrors one C-port beat in issue order
  struct ExpectedBeat {
    long long value[C_PORT_TILES][Dut::TILE_N];
  };
  std::deque<ExpectedBeat> expected_beats;

  SC_HAS_PROCESS(CIMArrayTbCase);

  // Construct one case and bind the standalone CIMArray ports
  explicit CIMArrayTbCase(sc_module_name name)
      : sc_module(name), dut("dut"), clk("clk", 10, SC_NS) {
    g_cases_remaining++;

    dut.clk(clk);
    dut.rstn(rstn);
    dut.mac_request_channel(mac_request_channel);
    dut.write_request_channel(write_request_channel);
    dut.result_channel(result_channel);
#if ENABLE_PERF_COUNTERS
    dut.completion_storage_stall(completion_storage_stall);
#endif

    clear_expected_state();

    SC_THREAD(run);
    sensitive << clk.posedge_event();

    SC_METHOD(observe_mac_acceptance);
    sensitive << clk.posedge_event();
    dont_initialize();

    SC_THREAD(watchdog);
  }

  // Fail this case with a contextual SystemC report
  void require(bool condition, const std::string& message) const {
    if (condition) {
      return;
    }

    std::ostringstream text;
    text << name() << ": " << message;
    const std::string report = text.str();
    SC_REPORT_FATAL("CIMArrayTb", report.c_str());
  }

  // Stop a lost request with a bounded simulation failure
  void watchdog() {
    wait(100, SC_US);
    require(false, "timed out waiting for CIMArray completion");
  }

  // Wait enough delta cycles for combinational methods to settle
  void settle() {
    for (int delta = 0; delta < 4; delta++) {
      wait(SC_ZERO_TIME);
    }
  }

  // Advance one CIMArray clock edge
  void tick() {
    wait(clk.posedge_event());
    settle();
  }

  // Count actual array and result transfers at their DUT-side interfaces
  void observe_mac_acceptance() {
    total_cycles++;
    if (!rstn.read()) {
      mac_accept_count = 0;
      return;
    }
#if ENABLE_PERF_COUNTERS
    if (completion_storage_stall.read()) {
      completion_storage_stall_cycles++;
    }
#endif
#ifdef CONNECTIONS_SIM_ONLY
    const bool mac_fired = mac_request_channel._VLDNAMEOUT_.read() &&
                           mac_request_channel._RDYNAMEOUT_.read();
    const bool result_fired =
        result_channel._VLDNAMEIN_.read() && result_channel._RDYNAMEIN_.read();
#else
    const bool mac_fired = mac_request_channel._VLDNAME_.read() &&
                           mac_request_channel._RDYNAME_.read();
    const bool result_fired =
        result_channel._VLDNAME_.read() && result_channel._RDYNAME_.read();
#endif
    if (mac_fired) {
      mac_accept_count++;
      total_mac_accept_count++;
      last_mac_fire_cycle = total_cycles;
    }
    if (result_fired) {
      result_fire_count++;
      last_result_fire_cycle = total_cycles;
    }
  }

  // Reset the testbench-side Connections endpoints
  void reset_channels() {
    mac_request_channel.ResetWrite();
    write_request_channel.ResetWrite();
    result_channel.ResetRead();
  }

  // Encode one integer as an unsigned ac_int bit pattern
  template <int WIDTH>
  ac_int<WIDTH, false> encode_value(long long value) const {
    const long long mask = mask_for_width(WIDTH);
    return ac_int<WIDTH, false>(value & mask);
  }

  // Decode one unsigned ac_int bit pattern using the case signedness
  template <int WIDTH>
  long long decode_value(ac_int<WIDTH, false> value) const {
    const long long mask = mask_for_width(WIDTH);
    long long raw = static_cast<long long>(value.to_int()) & mask;
    if (!IS_SIGNED) {
      return raw;
    }

    const long long sign_bit = 1LL << (WIDTH - 1);
    return (raw & sign_bit) == 0 ? raw : raw | ~mask;
  }

  // Create deterministic A data for one array input channel
  ac_int<A_WIDTH, false> a_value(int k, int phase) const {
    if (IS_SIGNED) {
      return encode_value<A_WIDTH>(((k * 5 + phase) % 13) - 6);
    }
    return encode_value<A_WIDTH>((k + 2) * 3 + phase);
  }

  // Create deterministic B data for one logical matrix coordinate
  ac_int<B_WIDTH, false> b_value(int k, int n, int phase) const {
    if (IS_SIGNED) {
      return encode_value<B_WIDTH>(((k * 13 + n * 5 + phase) % 11) - 5);
    }
    return encode_value<B_WIDTH>((k + 1) * 2 + n * 3 + phase);
  }

  // Clear resident B state and pending expected C beats
  void clear_expected_state() {
    for (int set_idx = 0; set_idx < B_SETS; set_idx++) {
      for (int k = 0; k < Dut::K; k++) {
        for (int n = 0; n < Dut::N; n++) {
          expected_b[set_idx][k][n] = 0;
        }
      }
    }
    expected_beats.clear();
  }

  // Apply reset and release into the B-loading phase
  void apply_reset() {
    rstn.write(false);
    reset_channels();
    settle();
    tick();
    rstn.write(true);
    tick();
  }

  // Drive one direct B-port span and mirror it into the logical B model
  void drive_write_direct(Set wset, int input_axis_idx,
                          int output_axis_tile_base, int tile_wchi, int phase) {
    WriteRequest request;
    request.wset = wset;
    request.input_axis_idx = input_axis_idx;
    request.output_axis_tile_base = output_axis_tile_base;
    request.wchi = tile_wchi;
    request.replicate = 0;

    for (int port_tile_idx = 0; port_tile_idx < B_PORT_TILES; port_tile_idx++) {
      for (int tile_n = 0; tile_n < Dut::TILE_N; tile_n++) {
        for (int tile_bk = 0; tile_bk < Dut::TILE_BK; tile_bk++) {
          const int k = input_axis_idx * Dut::TILE_K + tile_wchi + tile_bk;
          const int n =
              (output_axis_tile_base + port_tile_idx) * Dut::TILE_N + tile_n;
          request.data[port_tile_idx][tile_bk][tile_n] = b_value(k, n, phase);
          expected_b[wset.to_int()][k][n] =
              request.data[port_tile_idx][tile_bk][tile_n];
        }
      }
    }

    write_request_channel.Push(request);
    settle();
  }

  // Drive one replicated B tile and mirror beat tile zero across the output
  // axis
  void drive_write_replicate(Set wset, int input_axis_idx, int tile_wchi,
                             int phase) {
    WriteRequest request;
    request.wset = wset;
    request.input_axis_idx = input_axis_idx;
    request.output_axis_tile_base = 0;
    request.wchi = tile_wchi;
    request.replicate = 1;

    for (int port_tile_idx = 0; port_tile_idx < B_PORT_TILES; port_tile_idx++) {
      for (int tile_n = 0; tile_n < Dut::TILE_N; tile_n++) {
        for (int tile_bk = 0; tile_bk < Dut::TILE_BK; tile_bk++) {
          const int k = input_axis_idx * Dut::TILE_K + tile_wchi + tile_bk;
          const int source_n = port_tile_idx * Dut::TILE_N + tile_n;
          request.data[port_tile_idx][tile_bk][tile_n] =
              b_value(k, source_n, phase);
        }
      }
    }

    for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
         output_axis_idx++) {
      for (int tile_n = 0; tile_n < Dut::TILE_N; tile_n++) {
        for (int tile_bk = 0; tile_bk < Dut::TILE_BK; tile_bk++) {
          const int k = input_axis_idx * Dut::TILE_K + tile_wchi + tile_bk;
          const int n = output_axis_idx * Dut::TILE_N + tile_n;
          expected_b[wset.to_int()][k][n] = request.data[0][tile_bk][tile_n];
        }
      }
    }

    write_request_channel.Push(request);
    settle();
  }

  // Load every direct B-port span required by one resident weight set
  void load_weight_set(Set wset, int phase, bool insert_idle = false) {
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
         input_axis_idx++) {
      for (int tile_wchi = 0; tile_wchi < Dut::TILE_K;
           tile_wchi += Dut::TILE_BK) {
        for (int output_axis_tile_base = 0;
             output_axis_tile_base < OUTPUT_AXIS_TILES;
             output_axis_tile_base += B_PORT_TILES) {
          drive_write_direct(wset, input_axis_idx, output_axis_tile_base,
                             tile_wchi, phase);
          if (insert_idle) {
            tick();
            tick();
          }
        }
      }
    }
    tick();
  }

  // Load one weight set by replicating beat tile zero
  void load_weight_set_replicate(Set wset, int phase,
                                 bool insert_idle = false) {
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
         input_axis_idx++) {
      for (int tile_wchi = 0; tile_wchi < Dut::TILE_K;
           tile_wchi += Dut::TILE_BK) {
        drive_write_replicate(wset, input_axis_idx, tile_wchi, phase);
        if (insert_idle) {
          tick();
          tick();
        }
      }
    }
    tick();
  }

  // Compute one tile C vector for an expected beat
  void compute_expected_tile(ExpectedBeat& beat, int port_tile_idx, Set mset,
                             int input_axis_idx, int output_axis_idx,
                             int phase) const {
    for (int tile_n = 0; tile_n < Dut::TILE_N; tile_n++) {
      const int n = output_axis_idx * Dut::TILE_N + tile_n;
      long long sum = 0;
      for (int tile_k = 0; tile_k < Dut::TILE_K; tile_k++) {
        const int k = input_axis_idx * Dut::TILE_K + tile_k;
        const long long a = decode_value<A_WIDTH>(a_value(k, phase));
        const long long b =
            decode_value<B_WIDTH>(expected_b[mset.to_int()][k][n]);
        sum += a * b;
      }
      beat.value[port_tile_idx][tile_n] += sum;
    }
  }

  // Queue the C beats produced by one targeted or multicast MAC request
  void queue_expected_beats(Set mset, bool multicast,
                            int target_output_axis_idx, bool reduce,
                            int phase) {
    const int selected_output_lanes = multicast ? OUTPUT_AXIS_TILES : 1;
    const int logical_results = reduce
                                    ? selected_output_lanes
                                    : selected_output_lanes * INPUT_AXIS_TILES;
    const int result_beats =
        (logical_results + C_PORT_TILES - 1) / C_PORT_TILES;
    for (int result_beat_idx = 0; result_beat_idx < result_beats;
         result_beat_idx++) {
      ExpectedBeat beat = {};
      for (int port_idx = 0; port_idx < C_PORT_TILES; port_idx++) {
        const int logical_idx = result_beat_idx * C_PORT_TILES + port_idx;
        if (logical_idx >= logical_results) {
          continue;
        }

        int output_axis_ordinal = 0;
        int input_axis_idx = 0;
        if (reduce) {
          output_axis_ordinal = logical_idx;
        } else if constexpr (C_BEAT_LAYOUT == CIM_C_BEAT_INPUT_MAJOR) {
          output_axis_ordinal = logical_idx / INPUT_AXIS_TILES;
          input_axis_idx = logical_idx % INPUT_AXIS_TILES;
        } else if (multicast) {
          input_axis_idx = logical_idx / OUTPUT_AXIS_TILES;
          output_axis_ordinal = logical_idx % OUTPUT_AXIS_TILES;
        } else {
          input_axis_idx = logical_idx;
        }
        const int output_axis_idx =
            multicast ? output_axis_ordinal : target_output_axis_idx;

        if (reduce) {
          for (int sum_input_axis_idx = 0;
               sum_input_axis_idx < INPUT_AXIS_TILES; sum_input_axis_idx++) {
            compute_expected_tile(beat, port_idx, mset, sum_input_axis_idx,
                                  output_axis_idx, phase);
          }
        } else {
          compute_expected_tile(beat, port_idx, mset, input_axis_idx,
                                output_axis_idx, phase);
        }
      }
      expected_beats.push_back(beat);
    }
  }

  // Build one complete A beat for a deterministic phase
  ABeat build_a_beat(int phase) const {
    ABeat beat;
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
         input_axis_idx++) {
      for (int tile_k = 0; tile_k < Dut::TILE_K; tile_k++) {
        const int k = input_axis_idx * Dut::TILE_K + tile_k;
        beat[input_axis_idx][tile_k] = a_value(k, phase);
      }
    }
    return beat;
  }

  // Build one atomic MAC request with its deterministic A beat
  MACRequest build_mac_request(Set mset, int phase) const {
    MACRequest request;
    request.mset = mset;
    request.output_axis_idx = 0;
    request.multicast = 0;
    request.reduce = 0;
    request.a = build_a_beat(phase);
    return request;
  }

  // Buffer one MAC request until completion storage accepts it
  void arm_blocked_mac_push(const MACRequest& request) {
    blocked_mac_accept_start = total_mac_accept_count;
    require(mac_request_channel.PushNB(request),
            "MAC source buffer was not empty before blocked request");
    settle();
  }

  // Return whether the held source request transferred into CIMArray
  bool blocked_mac_accepted() const {
    return total_mac_accept_count != blocked_mac_accept_start;
  }

  // Wait until one result has transferred into the testbench endpoint
  void wait_for_result_fire(unsigned start_count) {
    for (int cycle = 0; cycle < 100 && result_fire_count == start_count;
         cycle++) {
      tick();
    }
    require(result_fire_count == start_count + 1,
            "timed out waiting for one DUT-side result transfer");
  }

  // Admit one MAC while independently draining older ready results
  void push_mac_request(const MACRequest& request) {
    const unsigned accepted_before = mac_accept_count;
    while (!mac_request_channel.PushNB(request)) {
      CBeat actual;
      if (result_channel.PopNB(actual)) {
        check_beat(actual);
      }
      tick();
    }
    settle();
    while (mac_accept_count == accepted_before) {
      CBeat actual;
      if (result_channel.PopNB(actual)) {
        check_beat(actual);
      }
      tick();
    }
  }

  // Drive one targeted MAC request and queue its expected C beats
  void drive_targeted_mac(Set mset, int output_axis_idx, int phase,
                          bool reduce = false) {
    MACRequest request = build_mac_request(mset, phase);
    request.output_axis_idx = output_axis_idx;
    request.reduce = reduce;
    queue_expected_beats(mset, false, output_axis_idx, reduce, phase);
    push_mac_request(request);
  }

  // Drive one multicast MAC request and queue its expected C beats
  void drive_multicast_mac(Set mset, int phase, bool reduce = false) {
    MACRequest request = build_mac_request(mset, phase);
    request.multicast = 1;
    request.reduce = reduce;
    queue_expected_beats(mset, true, 0, reduce, phase);
    push_mac_request(request);
  }

  // Admit one MAC while deliberately holding all older output beats
  void drive_held_mac(Set mset, int output_axis_idx, bool multicast,
                      bool reduce, int phase) {
    MACRequest request = build_mac_request(mset, phase);
    request.output_axis_idx = output_axis_idx;
    request.multicast = multicast;
    request.reduce = reduce;
    queue_expected_beats(mset, multicast, output_axis_idx, reduce, phase);
    mac_request_channel.Push(request);
    settle();
  }

  // Compare one received C beat against the oldest expected beat
  void check_beat(const CBeat& actual) {
    require(!expected_beats.empty(), "C beat popped with no expected beat");

    const ExpectedBeat expected = expected_beats.front();
    expected_beats.pop_front();

    for (int port_tile_idx = 0; port_tile_idx < C_PORT_TILES; port_tile_idx++) {
      for (int tile_n = 0; tile_n < Dut::TILE_N; tile_n++) {
        const ac_int<C_WIDTH, false> expected_bits =
            encode_value<C_WIDTH>(expected.value[port_tile_idx][tile_n]);
        if (actual[port_tile_idx][tile_n] == expected_bits) {
          continue;
        }

        std::ostringstream text;
        text << "unexpected C beat[" << port_tile_idx << "][" << tile_n
             << "] got " << actual[port_tile_idx][tile_n].to_int()
             << " expected " << expected_bits.to_int();
        require(false, text.str());
      }
    }
  }

  // Pop one C beat and compare it against the oldest expected beat
  void pop_and_check() {
    check_beat(result_channel.Pop());
    settle();
  }

  // Pop and check every outstanding expected C beat
  void drain_expected_beats() {
    while (!expected_beats.empty()) {
      pop_and_check();
    }
  }

  // Return a valid weight set for one deterministic transaction phase
  Set transaction_wset(int phase) const { return Set(phase % B_SETS); }

  // Run one multicast transaction with optional idle and backpressure cycles
  void run_multicast_transaction(int phase, bool insert_idle_cycles,
                                 int c_backpressure_cycles) {
    const Set wset = transaction_wset(phase);
    load_weight_set(wset, phase);
    if (insert_idle_cycles) {
      tick();
    }
    drive_multicast_mac(wset, phase + 3);
    if (insert_idle_cycles) {
      tick();
    }
    for (int cycle = 0; cycle < c_backpressure_cycles; cycle++) {
      tick();
    }
    drain_expected_beats();
  }

  // Check direct B loading and C backpressure with and without idle spacing
  void run_basic_transaction_checks() {
    run_multicast_transaction(1, true, 3);
    run_multicast_transaction(13, false, 3);
  }

  // Check optional reduction for multicast and targeted requests
  void run_reduced_result_check() {
    const Set wset = transaction_wset(17);
    load_weight_set(wset, 17);
    drive_multicast_mac(wset, 19, true);
    drain_expected_beats();
    drive_targeted_mac(wset, OUTPUT_AXIS_TILES - 1, 23, true);
    drain_expected_beats();
  }

  // Check direct and replicated B requests separated by empty cycles
  void run_gapped_write_check() {
    const Set direct_wset = transaction_wset(117);
    load_weight_set(direct_wset, 117, true);
    drive_multicast_mac(direct_wset, 121);
    drain_expected_beats();

    const Set replicate_wset = transaction_wset(127);
    load_weight_set_replicate(replicate_wset, 127, true);
    drive_multicast_mac(replicate_wset, 131);
    drain_expected_beats();
  }

  // Check that distinct weight sets remain independently addressable
  void run_set_retention_check() {
    const Set first_wset = Set(0);
    const Set second_wset = Set((B_SETS > 1) ? 1 : 0);
    load_weight_set(first_wset, 21);
    load_weight_set(second_wset, 29);
    drive_multicast_mac(first_wset, 35);
    drain_expected_beats();
    drive_multicast_mac(second_wset, 39);
    drain_expected_beats();
    drive_multicast_mac(first_wset, 43);
    drain_expected_beats();
  }

  // Check aligned B-port spans and preservation of an untouched span
  void run_b_port_span_retention_check() {
    if constexpr (OUTPUT_AXIS_TILES > B_PORT_TILES) {
      const Set wset = transaction_wset(137);
      load_weight_set(wset, 137);
      for (int tile_wchi = 0; tile_wchi < Dut::TILE_K;
           tile_wchi += Dut::TILE_BK) {
        drive_write_direct(wset, 0, 0, tile_wchi, 149);
      }
      tick();
      drive_multicast_mac(wset, 151);
      drain_expected_beats();
    }
  }

  // Check targeted issue to every output-axis tile back to back
  void run_targeted_issue_check() {
    const Set mset = transaction_wset(47);
    load_weight_set(mset, 47);
    for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
         output_axis_idx++) {
      drive_targeted_mac(mset, output_axis_idx, 49 + output_axis_idx);
    }
    drain_expected_beats();
  }

  // Diverge output-lane cursors before reduced and raw multicasts join them
  void run_mixed_completion_cursor_check() {
    if constexpr (INPUT_AXIS_TILES != 2 || OUTPUT_AXIS_TILES < 3 ||
                  RESULT_SLOTS_PER_OUTPUT_LANE < 6) {
      return;
    } else {
      const Set mset = transaction_wset(157);
      load_weight_set(mset, 157);
      const unsigned accepted_before = total_mac_accept_count;

      drive_held_mac(mset, 0, false, true, 163);
      drive_held_mac(mset, 1, false, false, 167);
      drive_held_mac(mset, 0, true, true, 173);
      drive_held_mac(mset, 0, false, false, 179);
      drive_held_mac(mset, 2, false, true, 181);
      drive_held_mac(mset, 0, true, false, 191);

      for (int cycle = 0; cycle < MAC_LATENCY + 6; cycle++) {
        tick();
      }
      require(total_mac_accept_count - accepted_before == 6,
              "mixed completion sequence did not accept every request");

      drain_expected_beats();
      require(expected_beats.empty(),
              "mixed completion sequence left expected results undrained");
    }
  }

  // Check selective same-edge slot credit and one-shot final-beat release
  void run_exact_completion_release_check() {
    if constexpr (INPUT_AXIS_TILES != 2 || OUTPUT_AXIS_TILES < 3 ||
                  RESULT_SLOTS_PER_OUTPUT_LANE < 6) {
      return;
    } else {
      const Set mset = transaction_wset(193);
      load_weight_set(mset, 193);

      const unsigned blocker_start = result_fire_count;
      drive_held_mac(mset, 2, false, true, 197);
      wait_for_result_fire(blocker_start);

      const unsigned fill_accept_start = total_mac_accept_count;
      constexpr int kFillOperations = RESULT_SLOTS_PER_OUTPUT_LANE;
      const unsigned fill_accept_target =
          fill_accept_start + 2 * kFillOperations;
      for (int operation = 0; operation < kFillOperations; operation++) {
        drive_held_mac(mset, 0, false, true, 211 + operation * 2);
        drive_held_mac(mset, 1, false, true, 212 + operation * 2);
      }
      for (int cycle = 0;
           cycle < 100 && total_mac_accept_count != fill_accept_target;
           cycle++) {
        tick();
      }
      require(total_mac_accept_count == fill_accept_target,
              "targeted output-lane fill did not accept every request");

      MACRequest first_replacement = build_mac_request(mset, 227);
      first_replacement.output_axis_idx = 1;
      first_replacement.reduce = 1;
      queue_expected_beats(mset, false, 1, true, 227);
      arm_blocked_mac_push(first_replacement);
      const unsigned held_result_count = result_fire_count;
      tick();
      tick();
      require(!blocked_mac_accepted(),
              "full output lane did not hold its replacement request");
      require(result_fire_count == held_result_count,
              "buffered blocker failed to hold the next completion");

      pop_and_check();
      const unsigned before_lane0 = result_fire_count;
      wait_for_result_fire(before_lane0);
      require(!blocked_mac_accepted(),
              "lane-zero release credited a blocked lane-one request");

      pop_and_check();
      const unsigned before_lane1 = result_fire_count;
      wait_for_result_fire(before_lane1);
      require(blocked_mac_accepted() &&
                  last_mac_fire_cycle == last_result_fire_cycle,
              "lane-one release missed same-edge replacement admission");

      MACRequest second_replacement = build_mac_request(mset, 229);
      second_replacement.output_axis_idx = 1;
      second_replacement.reduce = 1;
      queue_expected_beats(mset, false, 1, true, 229);
      arm_blocked_mac_push(second_replacement);
      const unsigned second_held_result_count = result_fire_count;
      tick();
      tick();
      require(!blocked_mac_accepted(),
              "one lane-one release granted more than one slot credit");
      require(result_fire_count == second_held_result_count,
              "buffered lane-one result did not apply output backpressure");

      pop_and_check();
      const unsigned before_second_lane0 = result_fire_count;
      wait_for_result_fire(before_second_lane0);
      require(!blocked_mac_accepted(),
              "second lane-zero release credited lane one");

      pop_and_check();
      const unsigned before_second_lane1 = result_fire_count;
      wait_for_result_fire(before_second_lane1);
      require(blocked_mac_accepted() &&
                  last_mac_fire_cycle == last_result_fire_cycle,
              "second lane-one release missed same-edge admission");

      drain_expected_beats();
    }
  }

  // Check exact same-edge release and reuse of a one-entry completion queue
  void run_depth_one_completion_release_check() {
    if constexpr (Dut::COMPLETION_QUEUE_DEPTH != 1) {
      return;
    } else {
      const Set mset = transaction_wset(257);
      load_weight_set(mset, 257);

      const unsigned blocker_start = result_fire_count;
      drive_held_mac(mset, 0, false, true, 263);
      wait_for_result_fire(blocker_start);

      const unsigned fill_accept_start = total_mac_accept_count;
      drive_held_mac(mset, 0, false, true, 269);
      for (int cycle = 0;
           cycle < 100 && total_mac_accept_count != fill_accept_start + 1;
           cycle++) {
        tick();
      }
      require(total_mac_accept_count == fill_accept_start + 1,
              "depth-one completion did not fill its only slot");

      MACRequest replacement = build_mac_request(mset, 271);
      replacement.output_axis_idx = 0;
      replacement.reduce = 1;
      queue_expected_beats(mset, false, 0, true, 271);
      arm_blocked_mac_push(replacement);
      tick();
      tick();
      require(!blocked_mac_accepted(),
              "depth-one completion did not hold its replacement");

      pop_and_check();
      const unsigned before_release = result_fire_count;
      wait_for_result_fire(before_release);
      require(blocked_mac_accepted() &&
                  last_mac_fire_cycle == last_result_fire_cycle,
              "depth-one final release missed same-edge slot reuse");

      drain_expected_beats();
    }
  }

  // Check that only the accepted final beat releases raw multicast storage
  void run_multibeat_completion_release_check() {
    if constexpr (INPUT_AXIS_TILES != 2 || OUTPUT_AXIS_TILES < 3 ||
                  C_PORT_TILES != 2 || RESULT_SLOTS_PER_OUTPUT_LANE < 6) {
      return;
    } else {
      const Set mset = transaction_wset(233);
      load_weight_set(mset, 233);

      const unsigned blocker_start = result_fire_count;
      drive_held_mac(mset, 2, false, true, 239);
      wait_for_result_fire(blocker_start);

      const unsigned fill_accept_start = total_mac_accept_count;
      constexpr int kFillOperations =
          RESULT_SLOTS_PER_OUTPUT_LANE / INPUT_AXIS_TILES;
      const unsigned fill_accept_target = fill_accept_start + kFillOperations;
      for (int operation = 0; operation < kFillOperations; operation++) {
        drive_held_mac(mset, 0, true, false, 241 + operation);
      }
      for (int cycle = 0;
           cycle < 100 && total_mac_accept_count != fill_accept_target;
           cycle++) {
        tick();
      }
      require(total_mac_accept_count == fill_accept_target,
              "multicast output-lane fill did not accept every request");

      MACRequest replacement = build_mac_request(mset, 251);
      replacement.multicast = 1;
      replacement.reduce = 0;
      queue_expected_beats(mset, true, 0, false, 251);
      arm_blocked_mac_push(replacement);
      tick();
      tick();
      require(!blocked_mac_accepted(),
              "full multicast output lanes did not hold their replacement");

      pop_and_check();
      const unsigned before_beat0 = result_fire_count;
      wait_for_result_fire(before_beat0);
      require(!blocked_mac_accepted(),
              "first nonfinal C beat released multicast storage");

      pop_and_check();
      const unsigned before_beat1 = result_fire_count;
      wait_for_result_fire(before_beat1);
      require(!blocked_mac_accepted(),
              "second nonfinal C beat released multicast storage");

      const unsigned stalled_final_count = result_fire_count;
      constexpr int kFinalStallCycles =
          2 * Dut::COMPLETION_QUEUE_DEPTH + MAC_LATENCY + 3;
      for (int cycle = 0; cycle < kFinalStallCycles; cycle++) {
        tick();
      }
      require(
          result_fire_count == stalled_final_count && !blocked_mac_accepted(),
          "stalled final C beat released multicast storage");

      pop_and_check();
      const unsigned before_final = result_fire_count;
      wait_for_result_fire(before_final);
      require(blocked_mac_accepted() &&
                  last_mac_fire_cycle == last_result_fire_cycle,
              "accepted final C beat missed same-edge multicast admission");

      MACRequest second_replacement = build_mac_request(mset, 277);
      second_replacement.multicast = 1;
      second_replacement.reduce = 0;
      queue_expected_beats(mset, true, 0, false, 277);
      arm_blocked_mac_push(second_replacement);
      tick();
      tick();
      require(!blocked_mac_accepted(),
              "refilled multicast output lanes did not hold a replacement");

      pop_and_check();
      const unsigned before_next_beat0 = result_fire_count;
      wait_for_result_fire(before_next_beat0);
      require(!blocked_mac_accepted(),
              "final-to-nonfinal transition released on its first C beat");

      pop_and_check();
      const unsigned before_next_beat1 = result_fire_count;
      wait_for_result_fire(before_next_beat1);
      require(!blocked_mac_accepted(),
              "final-to-nonfinal transition released on its second C beat");

      pop_and_check();
      const unsigned before_next_final = result_fire_count;
      wait_for_result_fire(before_next_final);
      require(blocked_mac_accepted() &&
                  last_mac_fire_cycle == last_result_fire_cycle,
              "next accepted final C beat missed same-edge admission");

      drain_expected_beats();
    }
  }

  // Check sustained issue correctness while the configured slot pool applies
  // its measured backpressure
  void run_sustained_issue_check() {
    const Set mset = transaction_wset(53);
    load_weight_set(mset, 53);

    constexpr int kWrapOperations = 2 * Dut::COMPLETION_QUEUE_DEPTH + 3;
    constexpr int kOperations = kWrapOperations > 48 ? kWrapOperations : 48;
    const unsigned long start_cycle = total_cycles;
    const unsigned start_accepted = total_mac_accept_count;
#if ENABLE_PERF_COUNTERS
    const unsigned long start_stall = completion_storage_stall_cycles;
#endif
    int accepted = 0;
    int completed = 0;

    while (completed < kOperations) {
      if (accepted < kOperations) {
        const int phase = 200 + accepted;
        MACRequest request = build_mac_request(mset, phase);
        request.output_axis_idx = 0;
        request.reduce = 1;
        const bool request_accepted = mac_request_channel.PushNB(request);
        if (request_accepted) {
          queue_expected_beats(mset, false, 0, true, phase);
          accepted++;
        }
      }

      CBeat actual;
      if (result_channel.PopNB(actual)) {
        check_beat(actual);
        completed++;
      }
      tick();
    }
    require(expected_beats.empty(),
            "sustained issue check left expected results undrained");
#ifdef SCVERIFY
    constexpr const char* kSweepLabel = "RTL_COMPLETION_SWEEP";
#else
    constexpr const char* kSweepLabel = "SYSTEMC_FUNCTIONAL_SWEEP";
#endif
    std::cout << kSweepLabel
              << " slots_per_output_lane=" << RESULT_SLOTS_PER_OUTPUT_LANE
              << " total_slots=" << Dut::RESULT_SLOT_COUNT
              << " operations=" << kOperations
              << " cycles=" << total_cycles - start_cycle
              << " accepted=" << total_mac_accept_count - start_accepted;
#if ENABLE_PERF_COUNTERS
    std::cout << " stall_cycles="
              << completion_storage_stall_cycles - start_stall;
#endif
    std::cout << std::endl;
  }

  // Check that a full completion queue stops issue and retains every result
  // through output backpressure
  void run_completion_queue_backpressure_check() {
    const Set mset = transaction_wset(59);
    load_weight_set(mset, 59);

    for (int operation = 0; operation < RESULT_SLOTS_PER_OUTPUT_LANE;
         operation++) {
      const int phase = 300 + operation;
      MACRequest request = build_mac_request(mset, phase);
      request.output_axis_idx = 0;
      request.reduce = 1;
      queue_expected_beats(mset, false, 0, true, phase);
      mac_request_channel.Push(request);
      settle();
    }

    MACRequest blocked_request = build_mac_request(mset, 400);
    blocked_request.output_axis_idx = 0;
    blocked_request.reduce = 1;
    require(!mac_request_channel.PushNB(blocked_request),
            "completion queue accepted a request without a free slot");

    drain_expected_beats();

    queue_expected_beats(mset, false, 0, true, 400);
    mac_request_channel.Push(blocked_request);
    pop_and_check();
  }

  // Replicate beat tile zero, then verify targeted and multicast requests
  void run_replicate_write_check() {
    const Set mset = transaction_wset(107);
    load_weight_set_replicate(mset, 107);
    for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
         output_axis_idx++) {
      drive_targeted_mac(mset, output_axis_idx, 109 + output_axis_idx);
    }
    drain_expected_beats();
    drive_multicast_mac(mset, 113);
    drain_expected_beats();
  }

  // Check that an old set can be overwritten after a new-set MAC is accepted
  void run_overwrite_after_set_switch_check() {
    if constexpr (B_SETS < 2) {
      return;
    }

    const Set old_wset = Set(0);
    const Set new_wset = Set(1);
    load_weight_set(old_wset, 79);
    load_weight_set(new_wset, 89);
    drive_multicast_mac(old_wset, 83);
    drive_multicast_mac(new_wset, 97);
    load_weight_set(old_wset, 101);
    drain_expected_beats();
    drive_multicast_mac(old_wset, 103);
    drain_expected_beats();
  }

  // Reset a stalled final result and confirm the next operation completes
  void run_reset_recovery_check() {
    const Set mset = transaction_wset(31);
    load_weight_set(mset, 31);

    const unsigned blocker_start = result_fire_count;
    drive_held_mac(mset, 0, false, true, 37);
    wait_for_result_fire(blocker_start);

    const unsigned staged_accept_start = total_mac_accept_count;
    drive_held_mac(mset, 0, false, true, 41);
    for (int cycle = 0;
         cycle < 100 && total_mac_accept_count != staged_accept_start + 1;
         cycle++) {
      tick();
    }
    require(total_mac_accept_count == staged_accept_start + 1,
            "reset test did not accept its staged completion");

    const unsigned stalled_final_count = result_fire_count;
    for (int cycle = 0; cycle < MAC_LATENCY + 6; cycle++) {
      tick();
    }
    require(result_fire_count == stalled_final_count,
            "reset test failed to hold its final result");

    rstn.write(false);
    reset_channels();
    expected_beats.clear();
    settle();
    tick();
    rstn.write(true);
    tick();

    run_multicast_transaction(103, true, 3);
  }

  // Run the full case sequence
  void run() {
    apply_reset();
    clear_expected_state();

    run_basic_transaction_checks();
    run_reduced_result_check();
    run_gapped_write_check();
    run_set_retention_check();
    run_b_port_span_retention_check();
    run_targeted_issue_check();
    run_mixed_completion_cursor_check();
    run_exact_completion_release_check();
    run_depth_one_completion_release_check();
    run_multibeat_completion_release_check();
    run_sustained_issue_check();
    run_completion_queue_backpressure_check();
    run_replicate_write_check();
    run_overwrite_after_set_switch_check();
    run_reset_recovery_check();

    std::cout << "[PASS] " << name() << std::endl;
    g_cases_remaining--;
    if (g_cases_remaining == 0) {
      sc_stop();
    }
  }
};

// Elaborate deterministic CIMArray cases
int sc_main(int argc, char** argv) {
  (void)argc;
  (void)argv;

#ifndef SCVERIFY
  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2, CIM_MODE_BIT_PARALLEL_VALUE, 4, 4,
                 false, 2, 3, 1, 1>
      dense_unsigned("dense_unsigned");

  CIMArrayTbCase<5, 2, 3, 4, 4, 16, 1, 4, CIM_MODE_BIT_PARALLEL_VALUE, 5, 4,
                 true, 3, 2, 1, 1>
      tile_input_reduction_signed("tile_input_reduction_signed");

  CIMArrayTbCase<4, 4, 2, 4, 4, 12, 2, 2, CIM_MODE_BIT_PARALLEL_VALUE, 4, 8,
                 true, 2, 2, 1, 1>
      wider_b_signed("wider_b_signed");

  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2, CIM_MODE_BIT_PARALLEL_VALUE, 4, 4,
                 true, 2, 2, 1, 2>
      output_axis_parallel_signed("output_axis_parallel_signed");

  CIMArrayTbCase<4, 2, 2, 8, 8, 20, 2, 1, CIM_MODE_BIT_PARALLEL_VALUE, 8, 8,
                 true, 2, 2, 1, 2>
      native_ii1_minimum_slots_signed("native_ii1_minimum_slots_signed");

  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2, CIM_MODE_BIT_SERIAL_VALUE, 4, 4,
                 false, 2, 1, 1, 2>
      output_axis_serial_unsigned("output_axis_serial_unsigned");

  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2, CIM_MODE_BIT_PARALLEL_VALUE, 4, 4,
                 true, 2, 2, 2, 2>
      two_axis_parallel_signed("two_axis_parallel_signed");

  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2, CIM_MODE_BIT_SERIAL_VALUE, 4, 4,
                 false, 2, 2, 2, 2>
      two_axis_serial_unsigned("two_axis_serial_unsigned");

  // Signed bit-serial is the case the mode actually risks: the MSB slice
  // carries negative weight, so the macro negates that partial sum before
  // accumulating. The unsigned serial cases above never exercise that negation
  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2, CIM_MODE_BIT_SERIAL_VALUE, 4, 4, true,
                 2, 2, 2, 2>
      two_axis_serial_signed("two_axis_serial_signed");

  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2, CIM_MODE_BIT_PARALLEL_VALUE, 4, 4,
                 true, 2, 2, 2, 3, 2, 3, 2, CIM_C_BEAT_OUTPUT_MAJOR>
      output_major_signed("output_major_signed");

  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2, CIM_MODE_BIT_PARALLEL_VALUE, 4, 4,
                 true, 2, 2, 2, 4, 2, 2, 4, CIM_C_BEAT_OUTPUT_MAJOR>
      narrow_b_port_signed("narrow_b_port_signed");

  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2, CIM_MODE_BIT_PARALLEL_VALUE, 4, 4,
                 true, 2, 2, 2, 3, 2, 3, 2, CIM_C_BEAT_INPUT_MAJOR, 20, 6>
      mixed_cursor_input_major_signed("mixed_cursor_input_major_signed");

  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2, CIM_MODE_BIT_PARALLEL_VALUE, 4, 4,
                 true, 2, 2, 2, 3, 2, 3, 2, CIM_C_BEAT_OUTPUT_MAJOR, 20, 6>
      mixed_cursor_output_major_signed("mixed_cursor_output_major_signed");
#else
  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2, CIM_MODE_BIT_PARALLEL_VALUE, 4, 4,
                 false, 2, 2, 2, 3, 2, CIM_TEST_B_PORT_TILES,
                 (CIM_TEST_C_BEAT_LAYOUT == CIM_C_BEAT_INPUT_MAJOR) ? 2 : 3,
                 CIM_TEST_C_BEAT_LAYOUT, CIM_TEST_C_WIDTH,
                 CIM_ARRAY_TEST_RESULT_SLOTS_PER_OUTPUT_LANE>
      cim_array_scverify("cim_array_scverify");
#endif

  sc_start();
  return g_cases_remaining;
}
