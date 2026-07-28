// Native SystemC timing and data tests for CIMTile

#include <ac_int.h>
#include <systemc.h>

#include <deque>
#include <iostream>
#include <sstream>
#include <string>

#include "CIMTile.h"

// Verify aggregate A/B/C ports, tile-local B channel decoding, and consecutive
// retirements
struct CIMTileTb : sc_module {
  static constexpr int CH_IN = 4;
  static constexpr int CH_OUT = 2;
  static constexpr int B_SETS = 2;
  static constexpr int BASE_A_WIDTH = 4;
  static constexpr int BASE_B_WIDTH = 4;
  static constexpr int BASE_C_WIDTH = 12;
  static constexpr int WRITE_CH_IN = 2;
  static constexpr int MAC_LATENCY = 2;
  static constexpr int MODE = 0;
  static constexpr int A_WIDTH = 4;
  static constexpr int B_WIDTH = 4;
  static constexpr bool IS_SIGNED = false;
  static constexpr int INPUT_AXIS_ELEMENTS = 2;
  static constexpr int OUTPUT_AXIS_ELEMENTS = 3;
  static constexpr int OPERATIONS = 6;

  using Dut =
      CIMTile<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH, BASE_C_WIDTH,
              WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH, B_WIDTH, IS_SIGNED,
              INPUT_AXIS_ELEMENTS, OUTPUT_AXIS_ELEMENTS>;
  using WSet = typename Dut::WSet;
  using WChi = typename Dut::WChi;
  using AValue = typename Dut::AValue;
  using BValue = typename Dut::BValue;
  using AData = typename Dut::AData;
  using BData = typename Dut::BData;
  using CData = typename Dut::CData;

  // ExpectedResult holds every C channel produced by one tile
  struct ExpectedResult {
    long long value[Dut::N];
  };

  Dut dut;
  sc_clock clk;
  sc_signal<bool> rstn;
  sc_signal<bool> write;
  sc_signal<WSet> wset;
  sc_signal<WChi> wchi;
  sc_signal<BData> b;
  sc_signal<AData> a;
  sc_signal<WSet> mset;
  sc_signal<bool> mac_issue;
  sc_signal<bool> mac_ready;
  sc_signal<bool> mac_busy;
  sc_signal<CData> c;
  sc_signal<bool> c_retire;

  std::deque<ExpectedResult> expected_results;
  int cycle;
  int last_retire_cycle;
  int retirements;

  SC_HAS_PROCESS(CIMTileTb);

  // Construct and bind the standalone tile
  explicit CIMTileTb(sc_module_name name)
      : sc_module(name),
        dut("dut"),
        clk("clk", 10, SC_NS),
        cycle(0),
        last_retire_cycle(-1),
        retirements(0) {
    dut.wclk(clk);
    dut.mclk(clk);
    dut.rstn(rstn);
    dut.write(write);
    dut.wset(wset);
    dut.wchi(wchi);
    dut.b(b);
    dut.a(a);
    dut.mset(mset);
    dut.mac_issue(mac_issue);
    dut.mac_ready(mac_ready);
    dut.mac_busy(mac_busy);
    dut.c(c);
    dut.c_retire(c_retire);

    SC_THREAD(run);
  }

  // Fail with testbench context
  void require(bool condition, const std::string& message) const {
    if (condition) {
      return;
    }

    std::ostringstream text;
    text << name() << ": " << message;
    const std::string report = text.str();
    SC_REPORT_FATAL("CIMTileTb", report.c_str());
  }

  // Let combinational methods settle
  void settle() {
    for (int delta = 0; delta < 4; delta++) {
      wait(SC_ZERO_TIME);
    }
  }

  // Return deterministic A data
  AValue a_value(int operation, int k) const {
    const int input_element_idx = k / Dut::ELEMENT_K;
    const int element_k = k % Dut::ELEMENT_K;
    return AValue(1 + operation + input_element_idx + element_k);
  }

  // Return deterministic resident B data
  BValue b_value(int set_idx, int n, int k) const {
    const int input_element_idx = k / Dut::ELEMENT_K;
    const int element_k = k % Dut::ELEMENT_K;
    const int output_element_idx = n / Dut::ELEMENT_N;
    const int element_n = n % Dut::ELEMENT_N;
    return BValue(1 + set_idx + input_element_idx + output_element_idx +
                  element_n + element_k);
  }

  // Compute the expected input-axis reduction for one operation
  ExpectedResult expected_result(int operation) const {
    ExpectedResult expected;
    const int set_idx = operation % B_SETS;
    for (int n = 0; n < Dut::N; n++) {
      long long sum = 0;
      for (int k = 0; k < Dut::K; k++) {
        sum += a_value(operation, k).to_int() * b_value(set_idx, n, k).to_int();
      }
      expected.value[n] = sum;
    }
    return expected;
  }

  // Check and consume one retirement pulse
  void check_retirement() {
    if (!c_retire.read()) {
      return;
    }

    require(!expected_results.empty(), "unexpected tile retirement");
    const ExpectedResult& expected = expected_results.front();
    const CData c_data = c.read();
    for (int n = 0; n < Dut::N; n++) {
      std::ostringstream context;
      context << "result " << retirements << " C channel " << n;
      require(c_data[n].to_int64() == expected.value[n], context.str());
    }

    if (last_retire_cycle >= 0) {
      require(cycle == last_retire_cycle + 1,
              "back-to-back issues did not retire on consecutive cycles");
    }
    last_retire_cycle = cycle;
    retirements++;
    expected_results.pop_front();
  }

  // Advance one tile clock and inspect C
  void tick() {
    wait(clk.posedge_event());
    settle();
    cycle++;
    check_retirement();
  }

  // Drive inactive values onto the tile ports
  void initialize_ports() {
    rstn.write(false);
    write.write(false);
    wset.write(0);
    wchi.write(0);
    BData zero_b;
    clear_pack(zero_b);
    b.write(zero_b);
    AData zero_a;
    clear_pack(zero_a);
    a.write(zero_a);
    mset.write(0);
    mac_issue.write(false);
    settle();
  }

  // Reset the tile while preserving its resetless B storage
  void apply_reset() {
    rstn.write(false);
    tick();
    rstn.write(true);
    tick();
  }

  // Load every BK-wide B block in both sets through the aggregate tile port
  void load_b_sets() {
    for (int set_idx = 0; set_idx < B_SETS; set_idx++) {
      for (int base_k = 0; base_k < Dut::K; base_k += Dut::BK) {
        BData b_data;
        for (int bk = 0; bk < Dut::BK; bk++) {
          for (int n = 0; n < Dut::N; n++) {
            b_data[bk][n] = b_value(set_idx, n, base_k + bk);
          }
        }
        wset.write(set_idx);
        wchi.write(base_k);
        b.write(b_data);
        write.write(true);
        tick();
        write.write(false);
      }
    }
    tick();
  }

  // Issue distinct A vectors on consecutive clock edges
  void issue_back_to_back() {
    for (int operation = 0; operation < OPERATIONS; operation++) {
      require(mac_ready.read(), "tile was not ready for a consecutive issue");
      AData a_data;
      for (int k = 0; k < Dut::K; k++) {
        a_data[k] = a_value(operation, k);
      }
      a.write(a_data);
      mset.write(operation % B_SETS);
      mac_issue.write(true);
      expected_results.push_back(expected_result(operation));
      tick();
    }
    mac_issue.write(false);
  }

  // Run the tile timing and data test
  void run() {
    initialize_ports();
    apply_reset();
    load_b_sets();
    issue_back_to_back();

    for (int drain_cycle = 0; drain_cycle < 20 && !expected_results.empty();
         drain_cycle++) {
      tick();
    }

    require(expected_results.empty(), "timed out waiting for tile results");
    require(retirements == OPERATIONS, "tile did not retire every operation");
    std::cout << "[PASS] " << name() << std::endl;
    sc_stop();
  }
};

// Elaborate the aggregate tile case
int sc_main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  CIMTileTb tile_ii1("tile_ii1");
  sc_start();
  return 0;
}
