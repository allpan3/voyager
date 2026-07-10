// Native SystemC timing and data tests for CIMTile

#include <ac_int.h>
#include <systemc.h>

#include <deque>
#include <iostream>
#include <sstream>
#include <string>

#include "CIMTile.h"

// Verify that a width-matched bit-parallel tile accepts and retires every cycle
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
  static constexpr int TILE_INPUT_LANES = 2;
  static constexpr int TILE_OUTPUT_LANES = 3;
  static constexpr int OPERATIONS = 6;

  using Dut = CIMTile<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH,
                      BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH,
                      B_WIDTH, IS_SIGNED, TILE_INPUT_LANES, TILE_OUTPUT_LANES>;
  using ElementAddr = typename Dut::ElementAddr;
  using ElementSet = typename Dut::ElementSet;
  using ElementAValue = typename Dut::ElementAValue;
  using ElementBValue = typename Dut::ElementBValue;
  using CValue = typename Dut::CValue;

  // ExpectedResult holds one reduced tile result
  struct ExpectedResult {
    long long value[TILE_OUTPUT_LANES][Dut::ELEMENT_B_COLS];
  };

  Dut dut;
  sc_clock clk;
  sc_signal<bool> rstn;
  sc_signal<bool> wen[TILE_INPUT_LANES];
  sc_signal<ElementAddr> waddr[TILE_INPUT_LANES];
  sc_signal<ElementSet> wset[TILE_INPUT_LANES];
  sc_signal<ElementBValue> b[TILE_INPUT_LANES][TILE_OUTPUT_LANES]
                            [Dut::ELEMENT_B_COLS][Dut::ELEMENT_B_WRITE_ROWS];
  sc_signal<ElementAValue> a[TILE_INPUT_LANES][Dut::ELEMENT_A_COLS];
  sc_signal<ElementSet> mset;
  sc_signal<bool> mac_issue;
  sc_signal<bool> mac_ready;
  sc_signal<CValue> c[TILE_OUTPUT_LANES][Dut::ELEMENT_B_COLS];
  sc_signal<bool> c_retire;

  std::deque<ExpectedResult> expected_results;
  bool seen_retire;
  int cycle;
  int last_retire_cycle;
  int retirements;

  SC_HAS_PROCESS(CIMTileTb);

  // Construct and bind the standalone tile
  explicit CIMTileTb(sc_module_name name)
      : sc_module(name),
        dut("dut"),
        clk("clk", 10, SC_NS),
        seen_retire(false),
        cycle(0),
        last_retire_cycle(-1),
        retirements(0) {
    dut.wclk(clk);
    dut.mclk(clk);
    dut.rstn(rstn);
    dut.mset(mset);
    dut.mac_issue(mac_issue);
    dut.mac_ready(mac_ready);
    dut.c_retire(c_retire);

    for (int til = 0; til < TILE_INPUT_LANES; til++) {
      dut.wen[til](wen[til]);
      dut.waddr[til](waddr[til]);
      dut.wset[til](wset[til]);
      for (int a_col = 0; a_col < Dut::ELEMENT_A_COLS; a_col++) {
        dut.a[til][a_col](a[til][a_col]);
      }
      for (int tol = 0; tol < TILE_OUTPUT_LANES; tol++) {
        for (int b_col = 0; b_col < Dut::ELEMENT_B_COLS; b_col++) {
          for (int b_row = 0; b_row < Dut::ELEMENT_B_WRITE_ROWS; b_row++) {
            dut.b[til][tol][b_col][b_row](b[til][tol][b_col][b_row]);
          }
        }
      }
    }

    for (int tol = 0; tol < TILE_OUTPUT_LANES; tol++) {
      for (int b_col = 0; b_col < Dut::ELEMENT_B_COLS; b_col++) {
        dut.c[tol][b_col](c[tol][b_col]);
      }
    }

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

  // Return deterministic activation data for one operation
  ElementAValue activation_value(int operation, int til, int a_col) const {
    return ElementAValue(1 + operation + til + a_col);
  }

  // Return deterministic resident weight data
  ElementBValue weight_value(int set_idx, int til, int tol, int b_col,
                             int chi) const {
    return ElementBValue(1 + set_idx + til + tol + b_col + chi);
  }

  // Compute the expected reduction for one operation
  ExpectedResult expected_result(int operation) const {
    ExpectedResult expected;
    const int set_idx = operation % B_SETS;
    for (int tol = 0; tol < TILE_OUTPUT_LANES; tol++) {
      for (int b_col = 0; b_col < Dut::ELEMENT_B_COLS; b_col++) {
        long long sum = 0;
        for (int til = 0; til < TILE_INPUT_LANES; til++) {
          for (int chi = 0; chi < CH_IN; chi++) {
            sum += activation_value(operation, til, chi).to_int() *
                   weight_value(set_idx, til, tol, b_col, chi).to_int();
          }
        }
        expected.value[tol][b_col] = sum;
      }
    }
    return expected;
  }

  // Check and consume one retirement toggle
  void check_retirement() {
    const bool retire_now = c_retire.read();
    if (retire_now == seen_retire) {
      return;
    }

    require(!expected_results.empty(), "unexpected tile retirement");
    const ExpectedResult& expected = expected_results.front();
    for (int tol = 0; tol < TILE_OUTPUT_LANES; tol++) {
      for (int b_col = 0; b_col < Dut::ELEMENT_B_COLS; b_col++) {
        std::ostringstream context;
        context << "result " << retirements << " lane " << tol << " column "
                << b_col;
        require(c[tol][b_col].read().to_int64() == expected.value[tol][b_col],
                context.str());
      }
    }

    if (last_retire_cycle >= 0) {
      require(cycle == last_retire_cycle + 1,
              "back-to-back issues did not retire on consecutive cycles");
    }
    last_retire_cycle = cycle;
    retirements++;
    seen_retire = retire_now;
    expected_results.pop_front();
  }

  // Advance one tile clock and inspect its output
  void tick() {
    wait(clk.posedge_event());
    settle();
    cycle++;
    check_retirement();
  }

  // Drive inactive values onto every tile input
  void initialize_inputs() {
    rstn.write(false);
    mac_issue.write(false);
    mset.write(0);
    for (int til = 0; til < TILE_INPUT_LANES; til++) {
      wen[til].write(false);
      waddr[til].write(0);
      wset[til].write(0);
      for (int a_col = 0; a_col < Dut::ELEMENT_A_COLS; a_col++) {
        a[til][a_col].write(0);
      }
      for (int tol = 0; tol < TILE_OUTPUT_LANES; tol++) {
        for (int b_col = 0; b_col < Dut::ELEMENT_B_COLS; b_col++) {
          for (int b_row = 0; b_row < Dut::ELEMENT_B_WRITE_ROWS; b_row++) {
            b[til][tol][b_col][b_row].write(0);
          }
        }
      }
    }
    settle();
  }

  // Reset the tile while preserving its resetless weight storage
  void apply_reset() {
    rstn.write(false);
    tick();
    rstn.write(true);
    tick();
    seen_retire = c_retire.read();
  }

  // Load all weight rows in both sets
  void load_weights() {
    for (int set_idx = 0; set_idx < B_SETS; set_idx++) {
      for (int til = 0; til < TILE_INPUT_LANES; til++) {
        for (int base_chi = 0; base_chi < CH_IN;
             base_chi += Dut::ELEMENT_B_WRITE_ROWS) {
          wen[til].write(true);
          waddr[til].write(base_chi);
          wset[til].write(set_idx);
          for (int tol = 0; tol < TILE_OUTPUT_LANES; tol++) {
            for (int b_col = 0; b_col < Dut::ELEMENT_B_COLS; b_col++) {
              for (int b_row = 0; b_row < Dut::ELEMENT_B_WRITE_ROWS; b_row++) {
                b[til][tol][b_col][b_row].write(
                    weight_value(set_idx, til, tol, b_col, base_chi + b_row));
              }
            }
          }
          tick();
          wen[til].write(false);
        }
      }
    }
    tick();
  }

  // Issue distinct operations on consecutive clock edges
  void issue_back_to_back() {
    for (int operation = 0; operation < OPERATIONS; operation++) {
      require(mac_ready.read(), "tile was not ready for a consecutive issue");
      mset.write(operation % B_SETS);
      mac_issue.write(true);
      for (int til = 0; til < TILE_INPUT_LANES; til++) {
        for (int a_col = 0; a_col < Dut::ELEMENT_A_COLS; a_col++) {
          a[til][a_col].write(activation_value(operation, til, a_col));
        }
      }
      expected_results.push_back(expected_result(operation));
      tick();
    }
    mac_issue.write(false);
  }

  // Run the timing and data test
  void run() {
    initialize_inputs();
    apply_reset();
    load_weights();
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

// Elaborate the width-matched bit-parallel tile case
int sc_main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  CIMTileTb tile_ii1("tile_ii1");
  sc_start();
  return 0;
}
