// Combined SystemC and Catapult SCVerify tests for the standalone CIMArray
//
// Normal builds elaborate all deterministic SystemC cases. SCVerify builds
// define SCVERIFY, wrap the DUT with CCS_DESIGN(), and elaborate only
// the geometry synthesized by scverify-array

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

#ifndef CIM_TEST_C_PORT_ORIENTATION
#define CIM_TEST_C_PORT_ORIENTATION CIM_C_PORT_REDUCTION_MAJOR
#endif

static int g_cases_remaining = 0;

// Return a mask covering the requested bit width
static constexpr long long mask_for_width(int width) {
  return (1LL << width) - 1;
}

// One parameterized CIMArray SystemC test case
template <int CH_IN, int CH_OUT, int B_SETS, int BASE_A_WIDTH,
          int BASE_B_WIDTH, int BASE_C_WIDTH, int WRITE_CH_IN, int MAC_LATENCY,
          int MODE, int A_WIDTH, int B_WIDTH, bool IS_SIGNED,
          int TILE_INPUT_LANES, int TILE_OUTPUT_LANES,
          int REDUCTION_GROUPS = 1, int MULTICAST_GROUPS = 1,
          int A_PORT_TILES = REDUCTION_GROUPS,
          int B_PORT_TILES = MULTICAST_GROUPS,
          int C_PORT_TILES = REDUCTION_GROUPS,
          int C_PORT_ORIENTATION = CIM_C_PORT_REDUCTION_MAJOR,
          typename DutType =
              CIMArray<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH,
                      BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH,
                      B_WIDTH, IS_SIGNED, TILE_INPUT_LANES, TILE_OUTPUT_LANES,
                      REDUCTION_GROUPS, MULTICAST_GROUPS, A_PORT_TILES,
                      B_PORT_TILES, C_PORT_TILES, C_PORT_ORIENTATION>>
struct CIMArrayTbCase : sc_module {
  using Dut = DutType;
  using ABeat = typename Dut::ABeat;
  using CBeat = typename Dut::CBeat;
  using MACRequest = typename Dut::MACRequest;
  using StoreRequest = typename Dut::StoreRequest;
  using ElementSet = typename Dut::ElementSet;
  using MulticastGroupIndex = typename Dut::MulticastGroupIndex;

  static constexpr int INPUT_LANES = TILE_INPUT_LANES * REDUCTION_GROUPS;
  static constexpr int OUTPUT_LANES = TILE_OUTPUT_LANES * MULTICAST_GROUPS;
  static constexpr int MULTICAST_GROUP_LANES = Dut::MULTICAST_GROUP_LANES;

  static_assert(A_WIDTH < 31, "A_WIDTH must fit this unit test golden model");
  static_assert(B_WIDTH < 31, "B_WIDTH must fit this unit test golden model");
  static_assert(Dut::C_WIDTH < 62,
                "C_WIDTH must fit this unit test golden model");

  CIMARRAY_DUT_TYPE(Dut) dut;
  sc_clock clk;
  sc_signal<bool> rstn;
  Connections::Combinational<MACRequest> mac_request_channel;
  Connections::Combinational<ABeat> a_channel;
  Connections::Combinational<StoreRequest> store_channel;
  Connections::Combinational<CBeat> result_channel;

  ac_int<B_WIDTH, false>
      expected_weights[B_SETS][Dut::ELEMENTS][Dut::ELEMENT_B_COLS][CH_IN];

  // ExpectedBeat mirrors the selected C-port tile axis in issue order
  struct ExpectedBeat {
    long long value[C_PORT_TILES][MULTICAST_GROUP_LANES][Dut::ELEMENT_B_COLS];
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
    dut.a_channel(a_channel);
    dut.store_channel(store_channel);
    dut.result_channel(result_channel);

    clear_expected_state();

    SC_THREAD(run);
    sensitive << clk.posedge_event();
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

  // Wait enough delta cycles for combinational output methods to settle
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

  // Reset the testbench-side Connections endpoints
  void reset_channels() {
    mac_request_channel.ResetWrite();
    a_channel.ResetWrite();
    store_channel.ResetWrite();
    result_channel.ResetRead();
  }

  // Encode one integer as an unsigned ac_int bit pattern
  template <int WIDTH>
  ac_int<WIDTH, false> encode_value(long long value) const {
    const long long mask = mask_for_width(WIDTH);
    const long long raw = value & mask;
    return ac_int<WIDTH, false>(raw);
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
    if ((raw & sign_bit) == 0) {
      return raw;
    }
    return raw | ~mask;
  }

  // Return a flat element index from its logical lane coordinates
  static constexpr int element_index(int input_lane_idx, int output_lane_idx) {
    return (input_lane_idx * OUTPUT_LANES) + output_lane_idx;
  }

  // Create deterministic activation data for one MAC request
  ac_int<A_WIDTH, false> activation_value(int input_lane_idx, int chi,
                                          int phase) const {
    if (IS_SIGNED) {
      return encode_value<A_WIDTH>(
          ((input_lane_idx * 5 + chi * 3 + phase) % 13) - 6);
    }
    return encode_value<A_WIDTH>((input_lane_idx + 2) * (chi + 1) + phase);
  }

  // Create deterministic weight data for one B write request
  ac_int<B_WIDTH, false> weight_value(int input_lane_idx, int chunk,
                                      int output_lane_idx, int col, int lane,
                                      int phase) const {
    if (IS_SIGNED) {
      const int value =
          ((input_lane_idx * 13 + chunk * 7 + output_lane_idx * 5 + col * 3 + lane +
            phase) %
           11) -
          5;
      return encode_value<B_WIDTH>(value);
    }
    return encode_value<B_WIDTH>(
        (input_lane_idx + 1) * (chunk + 2) + (output_lane_idx * 5) + (col * 3) +
        lane + phase);
  }

  // Clear independent B storage and pending expected beats
  void clear_expected_state() {
    for (int set_idx = 0; set_idx < B_SETS; set_idx++) {
      for (int element_idx = 0; element_idx < Dut::ELEMENTS; element_idx++) {
        for (int col = 0; col < Dut::ELEMENT_B_COLS; col++) {
          for (int chi = 0; chi < CH_IN; chi++) {
            expected_weights[set_idx][element_idx][col][chi] = 0;
          }
        }
      }
    }

    expected_beats.clear();
  }

  // Drive inactive defaults onto all inputs
  void initialize_inputs() {
    rstn.write(false);
    reset_channels();
    settle();
  }

  // Apply reset and release into B-loading phase
  void apply_reset() {
    rstn.write(false);
    reset_channels();
    settle();
    tick();
    rstn.write(true);
    tick();
  }

  // Drive one store request and mirror it into the expected weight model
  void drive_store(ElementSet set, int input_lane_idx, int chunk, int phase) {
    StoreRequest request;
    request.set = set;
    request.lane = input_lane_idx;
    request.waddr = chunk * Dut::ELEMENT_B_WRITE_ROWS;
    request.fanout = 0;

    for (int output_lane_idx = 0; output_lane_idx < OUTPUT_LANES; output_lane_idx++) {
      for (int col = 0; col < Dut::ELEMENT_B_COLS; col++) {
        for (int lane = 0; lane < Dut::ELEMENT_B_WRITE_ROWS; lane++) {
          request.data[output_lane_idx][col][lane] =
              weight_value(input_lane_idx, chunk, output_lane_idx, col, lane, phase);
        }
      }
    }

    const int set_idx = set.to_int();
    const int base_chi = chunk * Dut::ELEMENT_B_WRITE_ROWS;
    for (int output_lane_idx = 0; output_lane_idx < OUTPUT_LANES; output_lane_idx++) {
      const int element_idx = element_index(input_lane_idx, output_lane_idx);
      for (int col = 0; col < Dut::ELEMENT_B_COLS; col++) {
        for (int lane = 0; lane < Dut::ELEMENT_B_WRITE_ROWS; lane++) {
          const int chi = base_chi + lane;
          if (chi < CH_IN) {
            expected_weights[set_idx][element_idx][col][chi] =
                request.data[output_lane_idx][col][lane];
          }
        }
      }
    }

    store_channel.Push(request);
    settle();
  }

  // Load every B transfer required for one CIMArray transaction
  void load_b_operands(ElementSet set, int phase) {
    for (int chunk = 0; chunk < Dut::ELEMENT_B_BEATS; chunk++) {
      for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
        drive_store(set, input_lane_idx, chunk, phase);
      }
    }
    tick();  // commit the final write before a same-row MAC can be issued
  }

  // Load B operands with an empty cycle after every store request
  void load_b_operands_with_idle(ElementSet set, int phase) {
    for (int chunk = 0; chunk < Dut::ELEMENT_B_BEATS; chunk++) {
      for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
        drive_store(set, input_lane_idx, chunk, phase);
        tick();  // allow the pushed request to be accepted
        tick();  // leave the following empty cycle visible to the DUT
      }
    }
  }

  // Drive one fanout store and mirror the replicated tile into every multicast group
  void drive_store_fanout(ElementSet set, int base_lane, int chunk, int phase) {
    StoreRequest request;
    request.set = set;
    request.lane = base_lane;
    request.waddr = chunk * Dut::ELEMENT_B_WRITE_ROWS;
    request.fanout = 1;

    for (int section = 0; section < MULTICAST_GROUPS; section++) {
      for (int group_lane = 0; group_lane < MULTICAST_GROUP_LANES; group_lane++) {
        for (int col = 0; col < Dut::ELEMENT_B_COLS; col++) {
          for (int lane = 0; lane < Dut::ELEMENT_B_WRITE_ROWS; lane++) {
            request.data[section * MULTICAST_GROUP_LANES + group_lane][col][lane] =
                weight_value(base_lane + section, chunk, group_lane, col, lane, phase);
          }
        }
      }
    }

    const int set_idx = set.to_int();
    const int base_chi = chunk * Dut::ELEMENT_B_WRITE_ROWS;
    for (int section = 0; section < MULTICAST_GROUPS; section++) {
      const int input_lane_idx = base_lane + section;
      for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
        for (int group_lane = 0; group_lane < MULTICAST_GROUP_LANES; group_lane++) {
          const int output_lane_idx = group_idx * MULTICAST_GROUP_LANES + group_lane;
          const int element_idx = element_index(input_lane_idx, output_lane_idx);
          for (int col = 0; col < Dut::ELEMENT_B_COLS; col++) {
            for (int lane = 0; lane < Dut::ELEMENT_B_WRITE_ROWS; lane++) {
              const int chi = base_chi + lane;
              if (chi < CH_IN) {
                expected_weights[set_idx][element_idx][col][chi] =
                    request.data[section * MULTICAST_GROUP_LANES + group_lane][col][lane];
              }
            }
          }
        }
      }
    }

    store_channel.Push(request);
    settle();
  }

  // Load a replicated tile through fanout writes covering all input lanes
  void load_b_operands_fanout(ElementSet set, int phase) {
    for (int chunk = 0; chunk < Dut::ELEMENT_B_BEATS; chunk++) {
      for (int base_lane = 0; base_lane < INPUT_LANES; base_lane += MULTICAST_GROUPS) {
        drive_store_fanout(set, base_lane, chunk, phase);
      }
    }
    tick();  // commit the final write before a same-row MAC can be issued
  }

  // Load fanout B operands with an empty cycle after every store request
  void load_b_operands_fanout_with_idle(ElementSet set, int phase) {
    for (int chunk = 0; chunk < Dut::ELEMENT_B_BEATS; chunk++) {
      for (int base_lane = 0; base_lane < INPUT_LANES; base_lane += MULTICAST_GROUPS) {
        drive_store_fanout(set, base_lane, chunk, phase);
        tick();  // allow the pushed request to be accepted
        tick();  // leave the following empty cycle visible to the DUT
      }
    }
  }

  // Compute one tile's expected result for one MAC request
  void compute_expected_tile(ExpectedBeat& beat, int port_tile_idx,
                             ElementSet set, int reduce_idx, int group_idx,
                             int phase) const {
    const int set_idx = set.to_int();
    for (int group_lane = 0; group_lane < MULTICAST_GROUP_LANES; group_lane++) {
      const int output_lane_idx = group_idx * MULTICAST_GROUP_LANES + group_lane;
      for (int col = 0; col < Dut::ELEMENT_B_COLS; col++) {
        long long sum = 0;
        for (int til = 0; til < TILE_INPUT_LANES; til++) {
          const int input_lane_idx = reduce_idx * TILE_INPUT_LANES + til;
          const int element_idx = element_index(input_lane_idx, output_lane_idx);
          for (int chi = 0; chi < CH_IN; chi++) {
            const long long a_value = decode_value<A_WIDTH>(
                activation_value(input_lane_idx, chi, phase));
            const long long b_value = decode_value<B_WIDTH>(
                expected_weights[set_idx][element_idx][col][chi]);
            sum += a_value * b_value;
          }
        }
        beat.value[port_tile_idx][group_lane][col] = sum;
      }
    }
  }

  // Queue the C beats produced by one targeted or broadcast request
  void queue_expected_beats(ElementSet set, bool bcast, int target_group,
                            int phase) {
    if constexpr (C_PORT_ORIENTATION == CIM_C_PORT_REDUCTION_MAJOR) {
      for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
        if (bcast || group_idx == target_group) {
          ExpectedBeat beat = {};
          for (int reduce_idx = 0; reduce_idx < REDUCTION_GROUPS; reduce_idx++) {
            compute_expected_tile(beat, reduce_idx, set, reduce_idx, group_idx,
                                  phase);
          }
          expected_beats.push_back(beat);
        }
      }
    } else {
      for (int reduce_idx = 0; reduce_idx < REDUCTION_GROUPS; reduce_idx++) {
        ExpectedBeat beat = {};
        for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
          if (bcast || group_idx == target_group) {
            compute_expected_tile(beat, group_idx, set, reduce_idx, group_idx,
                                  phase);
          }
        }
        expected_beats.push_back(beat);
      }
    }
  }

  // Build narrow MAC metadata for one request
  MACRequest build_mac_request(ElementSet set) const {
    MACRequest request;
    request.set = set;
    request.group = 0;
    request.bcast = 0;
    return request;
  }

  // Build the shared A beat for one deterministic phase
  ABeat build_a_beat(int phase) const {
    ABeat a_beat;
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      for (int chi = 0; chi < CH_IN; chi++) {
        a_beat[input_lane_idx][chi] =
            activation_value(input_lane_idx, chi, phase);
      }
    }
    return a_beat;
  }

  // Drive one group-addressed MAC request and queue its expected beat
  void drive_mac_group(ElementSet set, int group_idx, int phase) {
    MACRequest request = build_mac_request(set);
    request.group = group_idx;

    queue_expected_beats(set, false, group_idx, phase);

    mac_request_channel.Push(request);
    a_channel.Push(build_a_beat(phase));
    settle();
  }

  // Drive one broadcast MAC request and queue expected beats for every group
  void drive_mac_bcast(ElementSet set, int phase) {
    MACRequest request = build_mac_request(set);
    request.bcast = 1;

    queue_expected_beats(set, true, 0, phase);

    mac_request_channel.Push(request);
    a_channel.Push(build_a_beat(phase));
    settle();
  }

  // Pop one C beat and compare it against the oldest expected beat
  void pop_and_check() {
    require(!expected_beats.empty(), "c beat popped with no expected beat");

    const CBeat actual = result_channel.Pop();
    const ExpectedBeat expected = expected_beats.front();
    expected_beats.pop_front();

    for (int port_tile_idx = 0; port_tile_idx < C_PORT_TILES; port_tile_idx++) {
      for (int group_lane = 0; group_lane < MULTICAST_GROUP_LANES; group_lane++) {
        for (int col = 0; col < Dut::ELEMENT_B_COLS; col++) {
          const ac_int<Dut::C_WIDTH, false> expected_bits =
              encode_value<Dut::C_WIDTH>(expected.value[port_tile_idx][group_lane][col]);
          if (actual[port_tile_idx][group_lane][col] == expected_bits) {
            continue;
          }

          std::ostringstream text;
          text << "unexpected c beat[" << port_tile_idx << "][" << group_lane << "]["
               << col << "] got " << actual[port_tile_idx][group_lane][col].to_int()
               << " expected " << expected_bits.to_int();
          require(false, text.str());
        }
      }
    }
    settle();
  }

  // Pop and check every outstanding expected beat
  void drain_expected_beats() {
    while (!expected_beats.empty()) {
      pop_and_check();
    }
  }

  // Return a valid B set for one deterministic transaction phase
  ElementSet transaction_set(int phase) const {
    return ElementSet(phase % B_SETS);
  }

  // Run one broadcast transaction with optional idle and backpressure cycles
  void run_bcast_transaction(int phase, bool insert_idle_cycles,
                             int c_backpressure_cycles) {
    const ElementSet set = transaction_set(phase);
    load_b_operands(set, phase);

    if (insert_idle_cycles) {
      tick();
    }

    drive_mac_bcast(set, phase + 3);

    if (insert_idle_cycles) {
      tick();
    }

    for (int cycle = 0; cycle < c_backpressure_cycles; cycle++) {
      tick();
    }

    drain_expected_beats();
  }

  // Run the basic load/MAC/result path with and without extra idle spacing
  void run_basic_transaction_checks() {
    run_bcast_transaction(1, true, 3);
    run_bcast_transaction(13, false, 3);
  }

  // Check B loads across explicit idle gaps between store requests
  void run_gapped_store_check() {
    const ElementSet direct_set = transaction_set(117);
    load_b_operands_with_idle(direct_set, 117);
    drive_mac_bcast(direct_set, 121);
    drain_expected_beats();

    if constexpr ((INPUT_LANES % MULTICAST_GROUPS) == 0) {
      const ElementSet fanout_set = transaction_set(127);
      load_b_operands_fanout_with_idle(fanout_set, 127);
      drive_mac_bcast(fanout_set, 131);
      drain_expected_beats();
    }
  }

  // Check that loaded rows remain independently addressable across later loads
  void run_set_retention_check() {
    const ElementSet first_set = ElementSet(0);
    const ElementSet second_set = ElementSet((B_SETS > 1) ? 1 : 0);

    load_b_operands(first_set, 21);
    load_b_operands(second_set, 29);

    drive_mac_bcast(first_set, 35);
    drain_expected_beats();

    drive_mac_bcast(second_set, 39);
    drain_expected_beats();

    drive_mac_bcast(first_set, 43);
    drain_expected_beats();
  }

  // Check per-group issue of distinct phases to every group back to back
  void run_group_issue_check() {
    const ElementSet set = transaction_set(47);
    load_b_operands(set, 47);

    // Issue one request per chunk without collecting in between
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      drive_mac_group(set, group_idx, 49 + group_idx);
    }

    drain_expected_beats();
  }

  // Check that reissuing a busy group serializes on the collected result
  void run_same_group_reissue_check() {
    const ElementSet set = transaction_set(53);

    load_b_operands(set, 53);
    drive_mac_group(set, 0, 59);
    drive_mac_group(set, 0, 61);
    drain_expected_beats();

    drive_mac_bcast(set, 63);
    drain_expected_beats();
  }

  // Load a replicated tile via fanout writes, then verify per-group and broadcast MACs
  void run_fanout_write_check() {
    if constexpr ((INPUT_LANES % MULTICAST_GROUPS) != 0) {
      return;
    }

    const ElementSet set = transaction_set(107);
    load_b_operands_fanout(set, 107);

    // Groups hold identical tiles; distinct phases verify per-group A delivery
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      drive_mac_group(set, group_idx, 109 + group_idx);
    }
    drain_expected_beats();

    drive_mac_bcast(set, 113);
    drain_expected_beats();
  }

  // Check long C-channel backpressure after all elements have produced results
  void run_long_c_backpressure_check() {
    const ElementSet set = transaction_set(67);

    load_b_operands(set, 67);
    drive_mac_bcast(set, 71);

    for (int cycle = 0; cycle < 9; cycle++) {
      tick();
    }

    drain_expected_beats();
  }

  // Check stores to another set while a MAC is already in flight
  void run_store_during_mac_check() {
    const ElementSet mac_set = ElementSet(0);
    const ElementSet store_set = ElementSet((B_SETS > 1) ? 1 : 0);

    load_b_operands(mac_set, 79);
    drive_mac_bcast(mac_set, 83);

    load_b_operands(store_set, 89);
    drain_expected_beats();

    drive_mac_bcast(store_set, 97);
    drain_expected_beats();
  }

  // Reset an in-flight transaction and confirm the next one completes cleanly
  void run_reset_recovery_check() {
    const ElementSet set = transaction_set(31);
    load_b_operands(set, 31);
    drive_mac_group(set, 0, 37);

    rstn.write(false);
    reset_channels();
    expected_beats.clear();
    settle();
    tick();
    rstn.write(true);
    tick();

    run_bcast_transaction(103, true, 3);
  }

  // Run the full case sequence
  void run() {
    initialize_inputs();
    apply_reset();
    clear_expected_state();

    run_basic_transaction_checks();
    run_gapped_store_check();
    run_set_retention_check();
    run_group_issue_check();
    run_same_group_reissue_check();
    run_fanout_write_check();
    run_long_c_backpressure_check();
    run_store_during_mac_check();
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
  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2,
                         CIM_MODE_BIT_PARALLEL_VALUE, 4, 4, false, 2, 3, 1, 1>
      bcast_dense_unsigned("bcast_dense_unsigned");

  CIMArrayTbCase<5, 2, 3, 4, 4, 16, 1, 4,
                         CIM_MODE_BIT_PARALLEL_VALUE, 5, 4, true, 3, 2, 1, 1>
      reduce_input_lanes_signed("reduce_input_lanes_signed");

  CIMArrayTbCase<4, 4, 2, 4, 4, 12, 2, 2,
                         CIM_MODE_BIT_PARALLEL_VALUE, 4, 8, true, 2, 2, 1, 1>
      wider_b_width_signed("wider_b_width_signed");

  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2,
                         CIM_MODE_BIT_PARALLEL_VALUE, 4, 4, true, 2, 2, 1, 2>
      grouped_parallel_signed("grouped_parallel_signed");

  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2,
                         CIM_MODE_BIT_SERIAL_VALUE, 4, 4, false, 2, 1, 1, 2>
      grouped_serial_unsigned("grouped_serial_unsigned");

  // Two reduction-group rows by two multicast-group columns of 2 x 2 tiles
  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2,
                         CIM_MODE_BIT_PARALLEL_VALUE, 4, 4, true, 2, 2, 2, 2>
      segmented_parallel_signed("segmented_parallel_signed");

  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2,
                         CIM_MODE_BIT_SERIAL_VALUE, 4, 4, false, 2, 2, 2, 2>
      segmented_serial_unsigned("segmented_serial_unsigned");

  // Multicast-major C beats span three groups and stream two reduction rows
  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2,
                         CIM_MODE_BIT_PARALLEL_VALUE, 4, 4, true, 2, 2, 2, 3,
                         2, 3, 3, CIM_C_PORT_MULTICAST_MAJOR>
      multicast_major_signed("multicast_major_signed");
#else
  CIMArrayTbCase<4, 2, 2, 4, 4, 12, 2, 2,
                         CIM_MODE_BIT_PARALLEL_VALUE, 4, 4, false, 2, 2, 2, 3,
                         2, 3,
                         (CIM_TEST_C_PORT_ORIENTATION == CIM_C_PORT_REDUCTION_MAJOR) ? 2 : 3,
                         CIM_TEST_C_PORT_ORIENTATION>
      cim_array_scverify("cim_array_scverify");
#endif

  sc_start();
  return g_cases_remaining;
}
