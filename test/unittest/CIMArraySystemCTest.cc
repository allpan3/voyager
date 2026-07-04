// SystemC behavioral tests for the standalone CIMArray lane protocol
//
// These tests stream MAC and B write requests through the CIMArray Connections
// interface and compare per-sector C beats against an independent delivery model

#include <ac_int.h>
#include <systemc.h>

#include <deque>
#include <iostream>
#include <sstream>
#include <string>

#include "CIMArray.h"

static constexpr int CIM_MODE_BIT_PARALLEL_VALUE = 0;
static constexpr int CIM_MODE_BIT_SERIAL_VALUE = 1;

static int g_cases_remaining = 0;

// Return a mask covering the requested bit width
static constexpr long long mask_for_width(int width) {
  return (1LL << width) - 1;
}

// One parameterized CIMArray SystemC test case
template <int CH_IN, int CH_OUT, int B_SETS, int BASE_A_WIDTH,
          int BASE_B_WIDTH, int BASE_C_WIDTH, int WRITE_CH_IN, int MAC_LATENCY,
          int MODE, int A_WIDTH, int B_WIDTH, bool IS_SIGNED, int INPUT_LANES,
          int OUTPUT_LANES, int SECTORS,
          typename DutType =
              CIMArray<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH,
                      BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH,
                      B_WIDTH, IS_SIGNED, INPUT_LANES, OUTPUT_LANES, SECTORS>>
struct CIMArraySystemCTestCase : sc_module {
  using Dut = DutType;
  using CBeat = typename Dut::CBeat;
  using MACRequest = typename Dut::MACRequest;
  using BWriteRequest = typename Dut::BWriteRequest;
  using ElementSet = typename Dut::ElementSet;
  using SectorIndex = typename Dut::SectorIndex;

  static constexpr int SECTOR_OUTPUT_LANES = Dut::SECTOR_OUTPUT_LANES;

  static_assert(A_WIDTH < 31, "A_WIDTH must fit this unit test golden model");
  static_assert(B_WIDTH < 31, "B_WIDTH must fit this unit test golden model");
  static_assert(Dut::C_WIDTH < 62,
                "C_WIDTH must fit this unit test golden model");

  Dut dut;
  sc_clock clk;
  sc_signal<bool> rstn;
  Connections::Combinational<MACRequest> a_channel;
  Connections::Combinational<BWriteRequest> b_channel;
  Connections::Combinational<CBeat> c_channel;

  ac_int<B_WIDTH, false>
      expected_weights[B_SETS][Dut::ELEMENTS][Dut::ELEMENT_B_COLS][CH_IN];

  // ExpectedBeat mirrors one per-sector C beat in issue order
  struct ExpectedBeat {
    long long value[SECTOR_OUTPUT_LANES][Dut::ELEMENT_B_COLS];
  };
  std::deque<ExpectedBeat> expected_beats;

  SC_HAS_PROCESS(CIMArraySystemCTestCase);

  // Construct one case and bind the standalone CIMArray ports
  explicit CIMArraySystemCTestCase(sc_module_name name)
      : sc_module(name), dut("dut"), clk("clk", 10, SC_NS) {
    g_cases_remaining++;

    dut.clk(clk);
    dut.rstn(rstn);
    dut.a_channel(a_channel);
    dut.b_channel(b_channel);
    dut.c_channel(c_channel);

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
    SC_REPORT_FATAL("CIMArraySystemCTest", report.c_str());
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
    a_channel.ResetWrite();
    b_channel.ResetWrite();
    c_channel.ResetRead();
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

  // Drive one B write request and mirror it into the expected weight model
  void drive_b_write(ElementSet set, int input_lane_idx, int chunk, int phase) {
    BWriteRequest request;
    request.set = set;
    request.input_lane = input_lane_idx;
    request.widx = chunk * Dut::ELEMENT_B_WRITE_ROWS;

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

    b_channel.Push(request);
    settle();
  }

  // Load every B transfer required for one CIMArray transaction
  void load_b_operands(ElementSet set, int phase) {
    for (int chunk = 0; chunk < Dut::ELEMENT_B_BEATS; chunk++) {
      for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
        drive_b_write(set, input_lane_idx, chunk, phase);
      }
    }
  }

  // Compute one delivery chunk's expected C beat for one MAC request
  ExpectedBeat expected_sector_beat(ElementSet set, int sector_idx,
                                   int phase) const {
    ExpectedBeat beat;
    const int set_idx = set.to_int();
    for (int sector_lane = 0; sector_lane < SECTOR_OUTPUT_LANES; sector_lane++) {
      const int output_lane_idx = sector_idx * SECTOR_OUTPUT_LANES + sector_lane;
      for (int col = 0; col < Dut::ELEMENT_B_COLS; col++) {
        long long sum = 0;
        for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
          const int element_idx = element_index(input_lane_idx, output_lane_idx);
          for (int chi = 0; chi < CH_IN; chi++) {
            const long long a_value = decode_value<A_WIDTH>(
                activation_value(input_lane_idx, chi, phase));
            const long long b_value = decode_value<B_WIDTH>(
                expected_weights[set_idx][element_idx][col][chi]);
            sum += a_value * b_value;
          }
        }
        beat.value[sector_lane][col] = sum;
      }
    }
    return beat;
  }

  // Build the shared MAC payload for one deterministic phase
  MACRequest build_mac_request(ElementSet set, int phase) const {
    MACRequest request;
    request.set = set;
    request.sector = 0;
    request.span = 0;

    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      for (int chi = 0; chi < CH_IN; chi++) {
        request.data[input_lane_idx][chi] =
            activation_value(input_lane_idx, chi, phase);
      }
    }
    return request;
  }

  // Drive one chunk-addressed MAC request and queue its expected beat
  void drive_mac_sector(ElementSet set, int sector_idx, int phase) {
    MACRequest request = build_mac_request(set, phase);
    request.sector = sector_idx;

    expected_beats.push_back(expected_sector_beat(set, sector_idx, phase));

    a_channel.Push(request);
    settle();
  }

  // Drive one spanning MAC request and queue expected beats for every chunk
  void drive_mac_span(ElementSet set, int phase) {
    MACRequest request = build_mac_request(set, phase);
    request.span = 1;

    for (int sector_idx = 0; sector_idx < SECTORS; sector_idx++) {
      expected_beats.push_back(expected_sector_beat(set, sector_idx, phase));
    }

    a_channel.Push(request);
    settle();
  }

  // Pop one C beat and compare it against the oldest expected beat
  void pop_and_check() {
    require(!expected_beats.empty(), "c beat popped with no expected beat");

    const CBeat actual = c_channel.Pop();
    const ExpectedBeat expected = expected_beats.front();
    expected_beats.pop_front();

    for (int sector_lane = 0; sector_lane < SECTOR_OUTPUT_LANES; sector_lane++) {
      for (int col = 0; col < Dut::ELEMENT_B_COLS; col++) {
        const ac_int<Dut::C_WIDTH, false> expected_bits =
            encode_value<Dut::C_WIDTH>(expected.value[sector_lane][col]);
        if (actual[sector_lane][col] == expected_bits) {
          continue;
        }

        std::ostringstream text;
        text << "unexpected c beat[" << sector_lane << "][" << col
             << "] got " << actual[sector_lane][col].to_int() << " expected "
             << expected_bits.to_int();
        require(false, text.str());
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

  // Run one spanning transaction with optional idle and backpressure cycles
  void run_span_transaction(int phase, bool insert_idle_cycles,
                             int c_backpressure_cycles) {
    const ElementSet set = transaction_set(phase);
    load_b_operands(set, phase);

    if (insert_idle_cycles) {
      tick();
    }

    drive_mac_span(set, phase + 3);

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
    run_span_transaction(1, true, 3);
    run_span_transaction(13, false, 3);
  }

  // Check that loaded rows remain independently addressable across later loads
  void run_set_retention_check() {
    const ElementSet first_set = ElementSet(0);
    const ElementSet second_set = ElementSet((B_SETS > 1) ? 1 : 0);

    load_b_operands(first_set, 21);
    load_b_operands(second_set, 29);

    drive_mac_span(first_set, 35);
    drain_expected_beats();

    drive_mac_span(second_set, 39);
    drain_expected_beats();

    drive_mac_span(first_set, 43);
    drain_expected_beats();
  }

  // Check per-chunk issue of distinct phases to every delivery chunk back to back
  void run_sector_issue_check() {
    const ElementSet set = transaction_set(47);
    load_b_operands(set, 47);

    // Issue one request per chunk without collecting in between
    for (int sector_idx = 0; sector_idx < SECTORS; sector_idx++) {
      drive_mac_sector(set, sector_idx, 49 + sector_idx);
    }

    drain_expected_beats();
  }

  // Check that reissuing a busy chunk serializes on the collected result
  void run_same_sector_reissue_check() {
    const ElementSet set = transaction_set(53);

    load_b_operands(set, 53);
    drive_mac_sector(set, 0, 59);
    drive_mac_sector(set, 0, 61);
    drain_expected_beats();

    drive_mac_span(set, 63);
    drain_expected_beats();
  }

  // Check long C-channel backpressure after all elements have produced results
  void run_long_c_backpressure_check() {
    const ElementSet set = transaction_set(67);

    load_b_operands(set, 67);
    drive_mac_span(set, 71);

    for (int cycle = 0; cycle < 9; cycle++) {
      tick();
    }

    drain_expected_beats();
  }

  // Check B writes to another set while a MAC is already in flight
  void run_b_write_during_mac_check() {
    const ElementSet mac_set = ElementSet(0);
    const ElementSet write_set = ElementSet((B_SETS > 1) ? 1 : 0);

    load_b_operands(mac_set, 79);
    drive_mac_span(mac_set, 83);

    load_b_operands(write_set, 89);
    drain_expected_beats();

    drive_mac_span(write_set, 97);
    drain_expected_beats();
  }

  // Reset an in-flight transaction and confirm the next one completes cleanly
  void run_reset_recovery_check() {
    const ElementSet set = transaction_set(31);
    load_b_operands(set, 31);
    drive_mac_sector(set, 0, 37);

    rstn.write(false);
    reset_channels();
    expected_beats.clear();
    settle();
    tick();
    rstn.write(true);
    tick();

    run_span_transaction(103, true, 3);
  }

  // Run the full case sequence
  void run() {
    initialize_inputs();
    apply_reset();
    clear_expected_state();

    run_basic_transaction_checks();
    run_set_retention_check();
    run_sector_issue_check();
    run_same_sector_reissue_check();
    run_long_c_backpressure_check();
    run_b_write_during_mac_check();
    run_reset_recovery_check();

    std::cout << "[PASS] " << name() << std::endl;
    g_cases_remaining--;
    if (g_cases_remaining == 0) {
      sc_stop();
    }
  }
};

// Elaborate all deterministic CIMArray SystemC cases
int sc_main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  CIMArraySystemCTestCase<4, 2, 2, 4, 4, 12, 2, 2,
                         CIM_MODE_BIT_PARALLEL_VALUE, 4, 4, false, 2, 3, 1>
      spanning_dense_unsigned("spanning_dense_unsigned");

  CIMArraySystemCTestCase<5, 2, 3, 4, 4, 16, 1, 4,
                         CIM_MODE_BIT_PARALLEL_VALUE, 5, 4, true, 3, 2, 1>
      reduce_input_lanes_signed("reduce_input_lanes_signed");

  CIMArraySystemCTestCase<4, 4, 2, 4, 4, 12, 2, 2,
                         CIM_MODE_BIT_PARALLEL_VALUE, 4, 8, true, 2, 2, 1>
      wider_b_width_signed("wider_b_width_signed");

  CIMArraySystemCTestCase<4, 2, 2, 4, 4, 12, 2, 2,
                         CIM_MODE_BIT_PARALLEL_VALUE, 4, 4, true, 2, 4, 2>
      sectored_parallel_signed("sectored_parallel_signed");

  CIMArraySystemCTestCase<4, 2, 2, 4, 4, 12, 2, 2,
                         CIM_MODE_BIT_SERIAL_VALUE, 4, 4, false, 2, 2, 2>
      sectored_serial_unsigned("sectored_serial_unsigned");

  sc_start();
  return g_cases_remaining;
}
