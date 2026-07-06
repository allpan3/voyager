// SystemC/RTL co-simulation tests for the PE-level CIMElement adapter
//
// These tests drive the fast SystemC CIMElement model and a Verilated
// CIMIntElementPacked RTL model through the Catapult blackbox ABI, then compare
// the observable issue/ready contract, retire toggles, and result payloads cycle by cycle

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
struct CIMElementTbCaseDescriptor {
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
struct CIMElementTbCase : sc_module {
  using Dut = CIMElement<A_COLS, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH,
                         BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH,
                         B_WIDTH, IS_SIGNED>;

  static_assert(Dut::A_BUS_WIDTH <= 64, "Verilated packed a_bus must fit uint64_t");
  static_assert(Dut::B_BUS_WIDTH <= 64, "Verilated packed b_bus must fit uint64_t");
  static_assert(Dut::C_BUS_WIDTH <= 64, "Verilated packed c_bus must fit uint64_t");

  Dut dut;
  RtlModel rtl;
  sc_signal<bool> clk;
  sc_signal<bool> rstn;
  ZeroSignal<ac_int<A_WIDTH, false>> a[A_COLS];
  ZeroSignal<ac_int<B_WIDTH, false>> b[Dut::B_COLS][Dut::B_ROWS];
  sc_signal<bool> wen;
  ZeroSignal<ac_int<Dut::BITS_CH_IN, false>> waddr;
  ZeroSignal<ac_int<Dut::BITS_SET, false>> wset;
  sc_signal<bool> mac_issue;
  ZeroSignal<ac_int<Dut::BITS_SET, false>> mset;
  ZeroSignal<ac_int<Dut::C_WIDTH, false>> c[Dut::B_COLS];
  sc_signal<bool> c_retire;
  sc_signal<bool> mac_ready;

  bool last_retire_ = false;

  SC_HAS_PROCESS(CIMElementTbCase);

  // Construct one case and bind all native CIMElement ports
  explicit CIMElementTbCase(sc_module_name name)
      : sc_module(name), dut("dut"), rtl(name) {
    g_cases_remaining++;

    dut.wclk(clk);
    dut.mclk(clk);
    dut.rstn(rstn);
    dut.wen(wen);
    dut.waddr(waddr);
    dut.wset(wset);
    dut.mac_issue(mac_issue);
    dut.mset(mset);
    dut.c_retire(c_retire);
    dut.mac_ready(mac_ready);

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
    SC_REPORT_FATAL("CIMElementTb", report.c_str());
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

  // Pack native activation lanes into the blackbox ABI order
  std::uint64_t pack_a_bus() {
    std::uint64_t packed = 0;
    for (int chi = 0; chi < A_COLS; chi++) {
      packed |= signal_value(a[chi]) << (chi * A_WIDTH);
    }
    return packed;
  }

  // Pack native weight lanes into the blackbox ABI order
  std::uint64_t pack_b_bus() {
    std::uint64_t packed = 0;
    for (int col = 0; col < Dut::B_COLS; col++) {
      for (int lane = 0; lane < Dut::B_ROWS; lane++) {
        const int bit_offset = ((col * Dut::B_ROWS) + lane) * B_WIDTH;
        packed |= signal_value(b[col][lane]) << bit_offset;
      }
    }
    return packed;
  }

  // Unpack one result column from the blackbox ABI order
  std::uint64_t rtl_c_value(int col) const {
    const std::uint64_t packed = static_cast<std::uint64_t>(rtl.c_bus);
    return (packed >> (col * Dut::C_WIDTH)) & mask_for_width(Dut::C_WIDTH);
  }

  // Copy current SystemC inputs into the Verilated RTL model
  void drive_rtl_inputs() {
    rtl.wclk = clk.read();
    rtl.mclk = clk.read();
    rtl.rstn = rstn.read();
    rtl.wen = wen.read();
    rtl.waddr = signal_value(waddr);
    rtl.wset = signal_value(wset);
    rtl.mac_issue = mac_issue.read();
    rtl.mset = signal_value(mset);
    rtl.a_bus = pack_a_bus();
    rtl.b_bus = pack_b_bus();
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

  // Compare issue/ready, retire toggles, and the registered result every cycle
  void compare_outputs(const char* label) {
    if (mac_ready.read() != static_cast<bool>(rtl.mac_ready)) {
      std::ostringstream text;
      text << label << ": mac_ready mismatch, SystemC=" << mac_ready.read()
           << " RTL=" << static_cast<int>(rtl.mac_ready);
      require(false, text.str());
    }

    if (c_retire.read() != static_cast<bool>(rtl.c_retire)) {
      std::ostringstream text;
      text << label << ": c_retire mismatch, SystemC=" << c_retire.read()
           << " RTL=" << static_cast<int>(rtl.c_retire);
      require(false, text.str());
    }

    for (int col = 0; col < Dut::B_COLS; col++) {
      const std::uint64_t sysc_value = signal_value(c[col]);
      const std::uint64_t rtl_value = rtl_c_value(col);
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
    waddr.write(0);
    wset.write(0);
    mac_issue.write(false);
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

  // Apply reset and release into an idle ready state
  void apply_reset() {
    rstn.write(false);
    wen.write(false);
    mac_issue.write(false);
    settle_and_compare("reset asserted");
    tick("reset clock");

    rstn.write(true);
    settle_and_compare("reset release");
    require(mac_ready.read(), "mac_ready stayed low after reset release");
    require(!c_retire.read(), "c_retire recovered high after reset release");
    last_retire_ = false;
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
    waddr.write(base);

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

  // Wait until both models flip c_retire or fail on timeout
  void wait_for_retire() {
    for (int cycle = 0; cycle < 128; cycle++) {
      if (c_retire.read() != last_retire_) {
        last_retire_ = c_retire.read();
        return;
      }
      tick("wait retire");
    }
    require(false, "timed out waiting for c_retire");
  }

  // Launch a MAC and optionally pulse an ignored issue while not ready
  void run_mac_check(int row, int phase, bool pulse_while_pending) {
    drive_activation(phase);
    mset.write(row);
    mac_issue.write(true);
    tick("launch mac");
    if (Dut::issue_window() > 1) {
      require(!mac_ready.read(), "mac_ready did not drop inside the issue window");
    } else {
      require(mac_ready.read(), "mac_ready must stay high for a one-cycle issue window");
    }

    if (pulse_while_pending && Dut::issue_window() > 1) {
      mac_issue.write(true);
      tick("dropped issue pulse");
    }

    mac_issue.write(false);
    settle_and_compare("mac idle");
    wait_for_retire();
    compare_outputs("completed mac");
    tick("hold result");
    compare_outputs("held result");
  }

  // Issue ops back to back at the ready cadence while comparing both models
  void run_pipelined_check(int row, int base_phase) {
    constexpr int kOps = 3;
    int issued = 0;
    int retired = 0;
    int guard = 0;

    while (retired < kOps) {
      if (issued < kOps && mac_ready.read()) {
        drive_activation(base_phase + issued);
        mset.write(row);
        mac_issue.write(true);
        issued++;
      } else {
        mac_issue.write(false);
      }
      tick("pipelined");
      mac_issue.write(false);

      if (c_retire.read() != last_retire_) {
        last_retire_ = c_retire.read();
        retired++;
      }

      require(++guard < 512, "pipelined check stalled");
    }
  }

  // Reset an in-flight MAC and confirm resetless weights serve later operations
  void run_reset_mid_operation_check() {
    drive_activation(7);
    mset.write(0);
    mac_issue.write(true);
    tick("launch reset test");
    if (Dut::issue_window() > 1) {
      require(!mac_ready.read(), "mac_ready did not drop before mid-operation reset");
    }

    mac_issue.write(false);
    rstn.write(false);
    settle_and_compare("mid-operation reset");
    tick("mid-operation reset clock");
    rstn.write(true);
    settle_and_compare("mid-operation reset release");
    require(mac_ready.read(),
            "mac_ready stayed low after mid-operation reset");
    require(!c_retire.read(),
            "c_retire recovered high after mid-operation reset");
    last_retire_ = false;

    run_mac_check(B_SETS - 1, 8, false);
  }

  // Reset while idle and confirm resetless weights serve later operations
  void run_idle_reset_weight_retention_check() {
    rstn.write(false);
    settle_and_compare("idle reset");
    tick("idle reset clock");
    rstn.write(true);
    settle_and_compare("idle reset release");
    require(mac_ready.read(), "mac_ready stayed low after idle reset");
    require(!c_retire.read(), "c_retire recovered high after idle reset");
    last_retire_ = false;

    run_mac_check(B_SETS - 1, 8, false);
  }

  // Run the full case sequence
  void run() {
    initialize_inputs();
    apply_reset();
    load_weights();
    run_mac_check(0, 1, true);
    run_mac_check(B_SETS / 2, 3, false);
    run_pipelined_check(0, 20);
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

#include "CIMElementTbCases.inc"

// Elaborate all deterministic CIMElement co-simulation cases
int sc_main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);

  for (int index = 0; index < kCIMElementTbCaseCount; index++) {
    kCIMElementTbCases[index].instantiate();
  }

  sc_start();
  return g_cases_remaining;
}
