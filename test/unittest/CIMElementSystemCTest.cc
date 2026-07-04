// SystemC/RTL co-simulation tests for the PE-level CIMElement adapter
//
// These tests drive the fast SystemC CIMElement model and a Verilated
// CIMIntElement RTL model with the same native CIM interface, then compare the
// observable start/busy contract and valid result payloads cycle by cycle

#include <ac_int.h>
#include <systemc.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include <verilated.h>

#include "CIMElement.h"

static constexpr int CIM_MODE_BIT_SERIAL_VALUE = 1;

static int g_cases_remaining = 0;

// One generated co-simulation case descriptor
struct CIMElementSystemCTestCaseDescriptor {
  const char* name;
  void (*instantiate)();
};

// Provide Verilator's non-SystemC $time hook from the active SystemC kernel
double sc_time_stamp() {
  return sc_core::sc_time_stamp().to_default_time_units();
}

// SystemC signal with deterministic zero initialization for ac_int values
template <typename T>
class ZeroSignal : public sc_signal<T> {
 public:
  // Construct a uniquely named signal whose current and pending values are zero
  ZeroSignal() : sc_signal<T>(sc_gen_unique_name("zero_signal"), T(0)) {}
};

// Return a mask covering the requested bit width
static constexpr std::uint64_t mask_for_width(int width) {
  return width >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << width) - 1);
}

// One parameterized CIMElement SystemC/RTL co-simulation case
template <typename RtlModel, int A_COLS, int CH_OUT, int B_SETS,
          int BASE_A_WIDTH, int BASE_B_WIDTH, int BASE_C_WIDTH, int WRITE_CH_IN,
          int MAC_LATENCY, int MODE, int A_WIDTH, int B_WIDTH, bool IS_SIGNED>
struct CIMElementSystemCTestCase : sc_module {
  using Dut = CIMElement<A_COLS, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH,
                         BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH,
                         B_WIDTH, IS_SIGNED>;

  Dut dut;
  RtlModel rtl;
  sc_signal<bool> clk;
  sc_signal<bool> rstn;
  ZeroSignal<ac_int<A_WIDTH, false>> a[A_COLS];
  ZeroSignal<ac_int<B_WIDTH, false>> b[Dut::B_COLS][Dut::B_ROWS];
  sc_signal<bool> wen;
  ZeroSignal<ac_int<Dut::BITS_CH_IN, false>> widx;
  ZeroSignal<ac_int<Dut::BITS_SET, false>> wset;
  sc_signal<bool> mac_start;
  ZeroSignal<ac_int<Dut::BITS_SET, false>> mset;
  ZeroSignal<ac_int<Dut::C_WIDTH, false>> c[Dut::B_COLS];
  sc_signal<bool> c_valid;
  sc_signal<bool> mac_busy;

  SC_HAS_PROCESS(CIMElementSystemCTestCase);

  // Construct one case and bind all native CIMElement ports
  explicit CIMElementSystemCTestCase(sc_module_name name)
      : sc_module(name), dut("dut"), rtl(name) {
    g_cases_remaining++;

    dut.wclk(clk);
    dut.mclk(clk);
    dut.rstn(rstn);
    dut.wen(wen);
    dut.widx(widx);
    dut.wset(wset);
    dut.mac_start(mac_start);
    dut.mset(mset);
    dut.c_valid(c_valid);
    dut.mac_busy(mac_busy);

    for (int chi = 0; chi < A_COLS; chi++) {
      dut.a[chi](a[chi]);
    }
    for (int col = 0; col < Dut::B_COLS; col++) {
      dut.c[col](c[col]);
      for (int lane = 0; lane < Dut::B_ROWS; lane++) {
        dut.b[col][lane](b[col][lane]);
      }
    }

    SC_THREAD(run);
  }

  // Fail this case with a contextual SystemC report
  void require(bool condition, const std::string& message) const {
    if (condition) {
      return;
    }

    std::ostringstream text;
    text << name() << ": " << message;
    const std::string report = text.str();
    SC_REPORT_FATAL("CIMElementSystemCTest", report.c_str());
  }

  // Return one unsigned SystemC signal value as a masked integer
  template <int WIDTH>
  static std::uint64_t signal_value(sc_signal<ac_int<WIDTH, false>>& signal) {
    return static_cast<std::uint64_t>(signal.read().to_uint64()) &
           mask_for_width(WIDTH);
  }

  // Encode one integer as an unsigned ac_int bit pattern
  template <int WIDTH>
  ac_int<WIDTH, false> encode_value(int value) const {
    const std::uint64_t raw = static_cast<std::uint64_t>(value) &
                              mask_for_width(WIDTH);
    return ac_int<WIDTH, false>(raw);
  }

  // Create deterministic activation data for one phase
  ac_int<A_WIDTH, false> activation_value(int chi, int phase) const {
    if (IS_SIGNED) {
      return encode_value<A_WIDTH>(((chi * 3 + phase * 2) % 9) - 4);
    }
    return encode_value<A_WIDTH>(((chi + 1) * (phase + 2)) & 0x1f);
  }

  // Create deterministic weight data for one matrix element
  ac_int<B_WIDTH, false> weight_value(int row, int col, int chi) const {
    if (IS_SIGNED) {
      return encode_value<B_WIDTH>(((row * 5 + col * 3 + chi * 2) % 11) - 5);
    }
    return encode_value<B_WIDTH>(((row + 1) * (col + 2) + chi + 1) & 0x1f);
  }

  // Copy current SystemC inputs into the Verilated RTL model
  void drive_rtl_inputs() {
    rtl.wclk = clk.read();
    rtl.mclk = clk.read();
    rtl.rstn = rstn.read();
    rtl.wen = wen.read();
    rtl.widx = signal_value(widx);
    rtl.wset = signal_value(wset);
    rtl.mac_start = mac_start.read();
    rtl.mset = signal_value(mset);

    for (int chi = 0; chi < A_COLS; chi++) {
      rtl.a[chi] = signal_value(a[chi]);
    }
    for (int col = 0; col < Dut::B_COLS; col++) {
      for (int lane = 0; lane < Dut::B_ROWS; lane++) {
        rtl.b[col][lane] = signal_value(b[col][lane]);
      }
    }
  }

  // Evaluate RTL, settle SystemC deltas, and compare observable outputs
  void settle_and_compare(const char* label) {
    drive_rtl_inputs();
    rtl.eval();

    for (int delta = 0; delta < 4; delta++) {
      wait(SC_ZERO_TIME);
      drive_rtl_inputs();
      rtl.eval();
    }

    compare_outputs(label);
  }

  // Compare start/busy every cycle and payload while valid or reset is active
  void compare_outputs(const char* label) {
    if (mac_busy.read() != static_cast<bool>(rtl.mac_busy)) {
      std::ostringstream text;
      text << label << ": mac_busy mismatch, SystemC=" << mac_busy.read()
           << " RTL=" << static_cast<int>(rtl.mac_busy);
      require(false, text.str());
    }

    if (c_valid.read() != static_cast<bool>(rtl.c_valid)) {
      std::ostringstream text;
      text << label << ": c_valid mismatch, SystemC=" << c_valid.read()
           << " RTL=" << static_cast<int>(rtl.c_valid);
      require(false, text.str());
    }

    if (!c_valid.read() && rstn.read()) {
      return;
    }

    for (int col = 0; col < Dut::B_COLS; col++) {
      const std::uint64_t sysc_value = signal_value(c[col]);
      const std::uint64_t rtl_value =
          static_cast<std::uint64_t>(rtl.c[col]) & mask_for_width(Dut::C_WIDTH);
      if (sysc_value == rtl_value) {
        continue;
      }

      std::ostringstream text;
      text << label << ": c[" << col << "] mismatch, SystemC=" << sysc_value
           << " RTL=" << rtl_value;
      require(false, text.str());
    }
  }

  // Advance one shared wclk/mclk cycle and check both models after each phase
  void tick(const char* label) {
    clk.write(false);
    settle_and_compare(label);

    clk.write(true);
    settle_and_compare(label);

    clk.write(false);
    settle_and_compare(label);
  }

  // Drive inactive defaults onto all inputs
  void initialize_inputs() {
    clk.write(false);
    rstn.write(false);
    wen.write(false);
    widx.write(0);
    wset.write(0);
    mac_start.write(false);
    mset.write(0);
    for (int chi = 0; chi < A_COLS; chi++) {
      a[chi].write(0);
    }
    for (int col = 0; col < Dut::B_COLS; col++) {
      for (int lane = 0; lane < Dut::B_ROWS; lane++) {
        b[col][lane].write(0);
      }
    }
    settle_and_compare("initialize");
  }

  // Apply reset and release into an idle non-busy state
  void apply_reset() {
    rstn.write(false);
    wen.write(false);
    mac_start.write(false);
    settle_and_compare("reset asserted");
    tick("reset clock");

    rstn.write(true);
    settle_and_compare("reset release");
    require(!mac_busy.read(), "mac_busy stayed high after reset release");
    require(!c_valid.read(), "c_valid stayed high after reset release");
  }

  // Drive one activation vector
  void drive_activation(int phase) {
    for (int chi = 0; chi < A_COLS; chi++) {
      a[chi].write(activation_value(chi, phase));
    }
  }

  // Write one WRITE_CH_IN group into both models
  void write_weight_group(int row, int base) {
    wen.write(true);
    wset.write(row);
    widx.write(base);

    for (int col = 0; col < Dut::B_COLS; col++) {
      for (int lane = 0; lane < Dut::B_ROWS; lane++) {
        const int chi = base + lane;
        b[col][lane].write(weight_value(row, col, chi));
      }
    }

    tick("write weight");
    wen.write(false);
    settle_and_compare("write idle");
  }

  // Load every logical CIM weight row before any MAC reads it
  void load_weights() {
    for (int row = 0; row < B_SETS; row++) {
      for (int base = 0; base < A_COLS; base += WRITE_CH_IN) {
        write_weight_group(row, base);
      }
    }
  }

  // Wait until both models report c_valid or fail on timeout
  void wait_for_valid() {
    for (int cycle = 0; cycle < 128; cycle++) {
      if (c_valid.read()) {
        require(static_cast<bool>(rtl.c_valid),
                "SystemC c_valid asserted before RTL c_valid");
        return;
      }
      tick("wait valid");
    }
    require(false, "timed out waiting for c_valid");
  }

  // Launch a MAC and optionally pulse a start while the held payload stays stable
  void run_mac_check(int row, int phase, bool pulse_while_busy) {
    drive_activation(phase);
    mset.write(row);
    mac_start.write(true);
    tick("launch mac");
    require(mac_busy.read(), "mac_busy did not assert after accepted MAC");

    if (pulse_while_busy) {
      mac_start.write(true);
      tick("busy mac pulse");
    }

    mac_start.write(false);
    settle_and_compare("mac idle");
    wait_for_valid();
    compare_outputs("completed mac");
    tick("hold result");
    compare_outputs("held result");
  }

  // Reset an in-flight MAC and confirm resetless weights serve later operations
  void run_reset_mid_operation_check() {
    drive_activation(7);
    mset.write(0);
    mac_start.write(true);
    tick("launch reset test");
    require(mac_busy.read(), "mac_busy did not assert before mid-operation reset");

    mac_start.write(false);
    rstn.write(false);
    settle_and_compare("mid-operation reset");
    tick("mid-operation reset clock");
    rstn.write(true);
    settle_and_compare("mid-operation reset release");
    require(!mac_busy.read(),
            "mac_busy stayed high after mid-operation reset");
    require(!c_valid.read(),
            "c_valid recovered high after mid-operation reset");

    run_mac_check(B_SETS - 1, 8, false);
  }

  // Reset while idle and confirm resetless weights serve later operations
  void run_idle_reset_weight_retention_check() {
    rstn.write(false);
    settle_and_compare("idle reset");
    tick("idle reset clock");
    rstn.write(true);
    settle_and_compare("idle reset release");
    require(!mac_busy.read(), "mac_busy stayed high after idle reset");
    require(!c_valid.read(), "c_valid recovered high after idle reset");

    run_mac_check(B_SETS - 1, 8, false);
  }

  // Run the full case sequence
  void run() {
    initialize_inputs();
    apply_reset();
    load_weights();
    run_mac_check(0, 1, true);
    run_mac_check(B_SETS / 2, 3, false);
    if constexpr (MODE == CIM_MODE_BIT_SERIAL_VALUE) {
      run_idle_reset_weight_retention_check();
    } else {
      run_reset_mid_operation_check();
    }

    rtl.final();
    std::cout << "[PASS] " << name() << std::endl;
    g_cases_remaining--;
    if (g_cases_remaining == 0) {
      sc_stop();
    }
  }
};

#include "CIMElementSystemCTestCases.inc"

// Elaborate all deterministic CIMElement co-simulation cases
int sc_main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);

  for (int index = 0; index < kCIMElementSystemCTestCaseCount; index++) {
    kCIMElementSystemCTestCases[index].instantiate();
  }

  sc_start();
  return g_cases_remaining;
}
