// SystemC HLS adapter for the integer CIM element RTL block
//
// CIMElement is a PE-level Catapult block boundary for the native CIM vector and
// matrix interface. The synthesized implementation blackboxes the existing
// SystemVerilog CIMIntElement, while the C++ body provides event-level simulation
// of the issue/retire protocol: mac_issue accepted while mac_ready, results
// retiring into c with a c_retire toggle after a fixed latency

#pragma once

#include <ac_int.h>
#include <systemc.h>
#include <sstream>
#include <ac_blackbox.h>
#include "AccelTypes.h"
#include "ArchitectureParams.h"

// CIMElementPacked owns the Catapult blackbox ABI with packed vector ports
template <int CH_IN, int CH_OUT, int B_SETS, int BASE_A_WIDTH,
          int BASE_B_WIDTH, int BASE_C_WIDTH, int WRITE_CH_IN,
          int MAC_LATENCY, int MODE, int A_WIDTH, int B_WIDTH, bool SIGNED>
SC_MODULE(CIMElementPacked) {
 private:
  // Return the ceil log2 used for static port widths
  static constexpr int log2_ceil(int value) {
    return (value <= 1) ? 0 : 1 + log2_ceil((value + 1) / 2);
  }

  // Return the ceiling division for static latency derivation
  static constexpr int ceil_div(int dividend, int divisor) {
    return (dividend + divisor - 1) / divisor;
  }

  // Return the smaller of two static integers
  static constexpr int min_int(int lhs, int rhs) {
    return (lhs < rhs) ? lhs : rhs;
  }

 public:
  static constexpr int CIM_MODE_BIT_SERIAL_VALUE = 1;
  static constexpr int A_COLS = CH_IN;
  static constexpr int SUM_GUARD_WIDTH = (A_COLS <= 1) ? 1 : log2_ceil(A_COLS);
  static constexpr int NUM_B_SLICES =
      (BASE_B_WIDTH > 0 && B_WIDTH >= BASE_B_WIDTH)
          ? (B_WIDTH / BASE_B_WIDTH)
          : 0;
  static constexpr int B_COLS =
      (NUM_B_SLICES > 0) ? (CH_OUT / NUM_B_SLICES) : 0;
  static constexpr int B_ROWS = WRITE_CH_IN;
  static constexpr int C_WIDTH = A_WIDTH + B_WIDTH + SUM_GUARD_WIDTH;
  static constexpr int BITS_CH_IN = (A_COLS <= 1) ? 1 : log2_ceil(A_COLS);
  static constexpr int BITS_SET = (B_SETS <= 1) ? 1 : log2_ceil(B_SETS);
  static constexpr int A_BUS_WIDTH = A_COLS * A_WIDTH;
  static constexpr int B_BUS_WIDTH = B_COLS * B_ROWS * B_WIDTH;
  static constexpr int C_BUS_WIDTH = B_COLS * C_WIDTH;

  static_assert(CH_IN > 0, "CH_IN must be positive");
  static_assert(CH_OUT > 0, "CH_OUT must be positive");
  static_assert(B_SETS > 0, "B_SETS must be positive");
  static_assert(BASE_A_WIDTH > 0, "BASE_A_WIDTH must be positive");
  static_assert(BASE_B_WIDTH > 0, "BASE_B_WIDTH must be positive");
  static_assert(BASE_C_WIDTH > 0, "BASE_C_WIDTH must be positive");
  static_assert(WRITE_CH_IN > 0, "WRITE_CH_IN must be positive");
  static_assert(MAC_LATENCY > 0, "MAC_LATENCY must be positive");
  static_assert(A_WIDTH > 0, "A_WIDTH must be positive");
  static_assert(B_WIDTH > 0, "B_WIDTH must be positive");
  static_assert(B_WIDTH >= BASE_B_WIDTH,
                "B_WIDTH must be at least BASE_B_WIDTH");
  static_assert((BASE_B_WIDTH > 0) && ((B_WIDTH % BASE_B_WIDTH) == 0),
                "B_WIDTH must be a multiple of BASE_B_WIDTH");
  static_assert((NUM_B_SLICES > 0) && ((CH_OUT % NUM_B_SLICES) == 0),
                "CH_OUT must divide wider B operands into logical columns");


  // Return the number of mclk cycles an accepted issue keeps the element not ready
  static constexpr int issue_window() {
    constexpr int serial_max_slice_width =
        BASE_C_WIDTH - BASE_B_WIDTH - SUM_GUARD_WIDTH;
    constexpr int serial_slice_width =
        min_int(A_WIDTH, serial_max_slice_width);
    constexpr int slice_width =
        (MODE == CIM_MODE_BIT_SERIAL_VALUE) ? serial_slice_width
                                            : BASE_A_WIDTH;
    constexpr int num_slices = ceil_div(A_WIDTH, slice_width);
    constexpr int serial_slice_interval =
        ceil_div(serial_slice_width, BASE_A_WIDTH) * BASE_A_WIDTH;
    constexpr int slice_launch_interval =
        (MODE == CIM_MODE_BIT_SERIAL_VALUE) ? serial_slice_interval : 1;
    return num_slices * slice_launch_interval;
  }

  // Return the number of mclk cycles from an accepted issue to its retirement
  static constexpr int operation_latency() {
    return issue_window() + MAC_LATENCY - 1;
  }

  // CIMElement clock and reset interface
  sc_in<bool> CCS_INIT_S1(wclk);
  sc_in<bool> CCS_INIT_S1(mclk);
  sc_in<bool> CCS_INIT_S1(rstn);

  // CIMElement packed weight-write interface
  sc_in<ac_int<A_BUS_WIDTH, false>> CCS_INIT_S1(a_bus);
  sc_in<ac_int<B_BUS_WIDTH, false>> CCS_INIT_S1(b_bus);
  sc_in<bool> CCS_INIT_S1(wen);
  sc_in<ac_int<BITS_CH_IN, false>> CCS_INIT_S1(waddr);
  sc_in<ac_int<BITS_SET, false>> CCS_INIT_S1(wset);

  // CIMElement MAC issue interface; a and mset must stay stable from an
  // accepted issue until mac_ready returns high (the issue window)
  sc_in<bool> CCS_INIT_S1(mac_issue);
  sc_in<ac_int<BITS_SET, false>> CCS_INIT_S1(mset);

  // CIMElement packed result interface; c_bus holds the last retired result
  sc_out<ac_int<C_BUS_WIDTH, false>> CCS_INIT_S1(c_bus);
  sc_out<bool> CCS_INIT_S1(c_retire);
  sc_out<bool> CCS_INIT_S1(mac_ready);

 private:
  // Resetless B storage matching the RTL CIM macro wrapper memory. B_ROWS is
  // the number of rows written per cycle; the resident contraction depth is A_COLS
  ac_int<B_WIDTH, false> b_mem[B_SETS][B_COLS][A_COLS];

  // PendingResult carries one computed result through the fixed retire latency
  struct PendingResult {
    ac_int<C_WIDTH, false> value[B_COLS];
    int cycles_remaining;
  };

  // Resettable C++ registers owned by the mclk process. The pending queue is
  // statically bounded so Catapult can analyze the behavioral model while the
  // RTL implementation remains an ac_blackbox.
  static constexpr int MAX_PENDING_RESULTS = operation_latency() + 1;
  PendingResult pending_results[MAX_PENDING_RESULTS];
  int pending_results_size;
  int window_remaining;
  bool retire_state;
  sc_signal<bool> window_idle_state;

 public:
  // Construct the packed CIMElement behavioral model and blackbox metadata
  SC_CTOR(CIMElementPacked)
      : pending_results_size(0),
        window_remaining(0),
        retire_state(false) {
    initialize_model_state();

    SC_METHOD(write_weights);
    sensitive << wclk.pos();
    dont_initialize();

    SC_METHOD(run_mclk);
    sensitive << mclk.pos() << rstn.neg();
    dont_initialize();

    SC_METHOD(drive_mac_ready);
    sensitive << rstn << window_idle_state;

#ifndef __SYNTHESIS__
    SC_METHOD(check_write_mac_collision);
    sensitive << wclk.pos();
    dont_initialize();
#endif

    ac_blackbox()
        .entity("CIMIntElementPacked")
        .verilog_files("CIM/cim_macro_wrapper.sv CIM/cim_macro_model.sv "
                       "CIM/cim_macro_1.sv CIM/cim_element.sv")
        .parameter("CH_IN", CH_IN)
        .parameter("CH_OUT", CH_OUT)
        .parameter("B_SETS", B_SETS)
        .parameter("BASE_A_WIDTH", BASE_A_WIDTH)
        .parameter("BASE_B_WIDTH", BASE_B_WIDTH)
        .parameter("BASE_C_WIDTH", BASE_C_WIDTH)
        .parameter("WRITE_CH_IN", WRITE_CH_IN)
        .parameter("MAC_LATENCY", MAC_LATENCY)
        .parameter("MODE", MODE)
        .parameter("A_WIDTH", A_WIDTH)
        .parameter("B_WIDTH", B_WIDTH)
        .parameter("SIGNED", static_cast<const int>(SIGNED))
        .inputs_registered(false)
        .end();
  }

 private:
  // Initialize observable model state while leaving resetless weights untouched
  void initialize_model_state() {
    pending_results_size = 0;
    window_remaining = 0;
    retire_state = false;
    window_idle_state.write(true);
  }

  // Decode a CIM operand with the configured signedness
  template <int WIDTH>
  static ac_int<WIDTH, SIGNED> decode_operand(ac_int<WIDTH, false> value) {
    ac_int<WIDTH, SIGNED> decoded;
    decoded.set_slc(0, value);
    return decoded;
  }

  // Return the packed B bus bit offset for one logical B value
  static constexpr int b_bus_offset(int b_col_idx, int b_row_idx) {
    return ((b_col_idx * B_ROWS) + b_row_idx) * B_WIDTH;
  }

  // Write one logical B row group into the resetless C++ weight model
  void write_weights() {
    if (!wen.read()) {
      return;
    }

    const int macro_row_idx = wset.read().to_int();
    const int base_a_col = waddr.read().to_int();

    if (macro_row_idx >= B_SETS) {
      return;
    }

    const ac_int<B_BUS_WIDTH, false> b_value = b_bus.read();
    for (int b_col_idx = 0; b_col_idx < B_COLS; b_col_idx++) {
      for (int b_row_idx = 0; b_row_idx < B_ROWS; b_row_idx++) {
        const int a_col_idx = base_a_col + b_row_idx;
        if (a_col_idx < A_COLS) {
          b_mem[macro_row_idx][b_col_idx][a_col_idx] =
              b_value.template slc<B_WIDTH>(b_bus_offset(b_col_idx, b_row_idx));
        }
      }
    }
  }

  // Compute one native CIM matrix-vector operation for the issued payload
  PendingResult compute_result() {
    PendingResult pending;
    pending.cycles_remaining = operation_latency();
    const int b_set_idx = mset.read().to_int();
    const ac_int<A_BUS_WIDTH, false> a_value_bus = a_bus.read();

    for (int b_col_idx = 0; b_col_idx < B_COLS; b_col_idx++) {
      ac_int<C_WIDTH, SIGNED> acc = 0;

      if (b_set_idx < B_SETS) {
        for (int a_col_idx = 0; a_col_idx < A_COLS; a_col_idx++) {
          const ac_int<A_WIDTH, SIGNED> a_value =
              decode_operand<A_WIDTH>(
                  a_value_bus.template slc<A_WIDTH>(a_col_idx * A_WIDTH));
          const ac_int<B_WIDTH, SIGNED> b_value =
              decode_operand<B_WIDTH>(
                  b_mem[b_set_idx][b_col_idx][a_col_idx]);
          acc += a_value * b_value;
        }
      }
      pending.value[b_col_idx] = acc;
    }
    return pending;
  }

  // Clear resettable CIM element state while preserving resetless weights
  void reset_element_state() {
    pending_results_size = 0;
    window_remaining = 0;
    retire_state = false;
    window_idle_state.write(true);
    c_bus.write(0);
    c_retire.write(false);
  }

  // Advance the mclk-domain issue and retire state
  void run_mclk() {
    if (!rstn.read()) {
      reset_element_state();
      return;
    }

    // Sample readiness before this edge's updates, mirroring the RTL comb ready
    const bool ready_now = (window_remaining == 0);

    // Advance the retire pipeline; ops are spaced by at least the issue window,
    // so at most one result retires per edge
    if (pending_results_size > 0) {
      for (int pending_idx = 0; pending_idx < MAX_PENDING_RESULTS;
           pending_idx++) {
        if (pending_idx < pending_results_size) {
          pending_results[pending_idx].cycles_remaining--;
        }
      }
      if (pending_results[0].cycles_remaining == 0) {
        ac_int<C_BUS_WIDTH, false> packed_result = 0;
        for (int b_col_idx = 0; b_col_idx < B_COLS; b_col_idx++) {
          packed_result.set_slc(b_col_idx * C_WIDTH,
                                pending_results[0].value[b_col_idx]);
        }
        c_bus.write(packed_result);
        retire_state = !retire_state;
        c_retire.write(retire_state);
        for (int pending_idx = 1; pending_idx < MAX_PENDING_RESULTS;
             pending_idx++) {
          if (pending_idx < pending_results_size) {
            pending_results[pending_idx - 1] = pending_results[pending_idx];
          }
        }
        pending_results_size--;
      }
    }

    if (window_remaining > 0) {
      window_remaining--;
    }

    if (mac_issue.read() && ready_now) {
      if (pending_results_size < MAX_PENDING_RESULTS) {
        pending_results[pending_results_size] = compute_result();
        pending_results_size++;
      }
      window_remaining = issue_window() - 1;
    }

    window_idle_state.write(window_remaining == 0);
  }

  // Drive combinational ready state from reset and the issue-window state
  void drive_mac_ready() {
    mac_ready.write(rstn.read() && window_idle_state.read());
  }

#ifndef __SYNTHESIS__
  // Report an illegal same-row write and MAC without changing model behavior
  void check_write_mac_collision() {
    if (wen.read() && mac_issue.read() && wset.read() == mset.read()) {
      std::ostringstream message;
      message << "write and MAC target row " << wset.read().to_int()
              << " while both enables are high";
      SC_REPORT_ERROR("CIMIntMacroModel row protocol violation", message.str().c_str());
    }
  }
#endif
};

// CIMElement keeps the native array-port interface and adapts it to packed ABI
template <int CH_IN, int CH_OUT, int B_SETS, int BASE_A_WIDTH,
          int BASE_B_WIDTH, int BASE_C_WIDTH, int WRITE_CH_IN,
          int MAC_LATENCY, int MODE, int A_WIDTH, int B_WIDTH, bool SIGNED>
SC_MODULE(CIMElement) {
 private:
  using PackedElement =
      CIMElementPacked<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH,
                       BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH,
                       B_WIDTH, SIGNED>;

 public:
  static constexpr int CIM_MODE_BIT_SERIAL_VALUE =
      PackedElement::CIM_MODE_BIT_SERIAL_VALUE;
  static constexpr int A_COLS = PackedElement::A_COLS;
  static constexpr int SUM_GUARD_WIDTH = PackedElement::SUM_GUARD_WIDTH;
  static constexpr int NUM_B_SLICES = PackedElement::NUM_B_SLICES;
  static constexpr int B_COLS = PackedElement::B_COLS;
  static constexpr int B_ROWS = PackedElement::B_ROWS;
  static constexpr int C_WIDTH = PackedElement::C_WIDTH;
  static constexpr int BITS_CH_IN = PackedElement::BITS_CH_IN;
  static constexpr int BITS_SET = PackedElement::BITS_SET;
  static constexpr int A_BUS_WIDTH = PackedElement::A_BUS_WIDTH;
  static constexpr int B_BUS_WIDTH = PackedElement::B_BUS_WIDTH;
  static constexpr int C_BUS_WIDTH = PackedElement::C_BUS_WIDTH;

  // Grouped CIMElement data-port types
  using AInput = Pack1D<ac_int<A_WIDTH, false>, A_COLS>;
  using BInput =
      Pack1D<Pack1D<ac_int<B_WIDTH, false>, B_ROWS>, B_COLS>;
  using COutput = Pack1D<ac_int<C_WIDTH, false>, B_COLS>;

  // Return the number of mclk cycles an accepted issue keeps the element not ready
  static constexpr int issue_window() { return PackedElement::issue_window(); }

  // Return the number of mclk cycles from an accepted issue to its retirement
  static constexpr int operation_latency() {
    return PackedElement::operation_latency();
  }

  // CIMElement clock and reset interface
  sc_in<bool> CCS_INIT_S1(wclk);
  sc_in<bool> CCS_INIT_S1(mclk);
  sc_in<bool> CCS_INIT_S1(rstn);

  // CIMElement weight-write interface
  sc_in<AInput> CCS_INIT_S1(a);
  sc_in<BInput> CCS_INIT_S1(b);
  sc_in<bool> CCS_INIT_S1(wen);
  sc_in<ac_int<BITS_CH_IN, false>> CCS_INIT_S1(waddr);
  sc_in<ac_int<BITS_SET, false>> CCS_INIT_S1(wset);

  // CIMElement MAC issue interface
  sc_in<bool> CCS_INIT_S1(mac_issue);
  sc_in<ac_int<BITS_SET, false>> CCS_INIT_S1(mset);

  // CIMElement result interface
  sc_out<COutput> CCS_INIT_S1(c);
  sc_out<bool> CCS_INIT_S1(c_retire);
  sc_out<bool> CCS_INIT_S1(mac_ready);

 private:
  PackedElement packed;
  sc_signal<ac_int<A_BUS_WIDTH, false>> a_bus;
  sc_signal<ac_int<B_BUS_WIDTH, false>> b_bus;
  sc_signal<ac_int<C_BUS_WIDTH, false>> c_bus;

 public:
  // Construct the public array-port adapter around the packed blackbox boundary
  SC_CTOR(CIMElement) : packed("packed") {
    packed.wclk(wclk);
    packed.mclk(mclk);
    packed.rstn(rstn);
    packed.a_bus(a_bus);
    packed.b_bus(b_bus);
    packed.wen(wen);
    packed.waddr(waddr);
    packed.wset(wset);
    packed.mac_issue(mac_issue);
    packed.mset(mset);
    packed.c_bus(c_bus);
    packed.c_retire(c_retire);
    packed.mac_ready(mac_ready);

    SC_METHOD(pack_inputs);
    sensitive << a << b;

    SC_METHOD(unpack_outputs);
    sensitive << c_bus;
  }

 private:
  // Return the packed B bus bit offset for one logical B value
  static constexpr int b_bus_offset(int b_col_idx, int b_row_idx) {
    return ((b_col_idx * B_ROWS) + b_row_idx) * B_WIDTH;
  }

  // Pack array-shaped public A/B ports into stable vector ports for Catapult
  void pack_inputs() {
    ac_int<A_BUS_WIDTH, false> packed_a = 0;
    ac_int<B_BUS_WIDTH, false> packed_b = 0;
    const AInput a_input = a.read();
    const BInput b_input = b.read();

    for (int a_col_idx = 0; a_col_idx < A_COLS; a_col_idx++) {
      packed_a.set_slc(a_col_idx * A_WIDTH, a_input[a_col_idx]);
    }

    for (int b_col_idx = 0; b_col_idx < B_COLS; b_col_idx++) {
      for (int b_row_idx = 0; b_row_idx < B_ROWS; b_row_idx++) {
        packed_b.set_slc(b_bus_offset(b_col_idx, b_row_idx),
                         b_input[b_col_idx][b_row_idx]);
      }
    }

    a_bus.write(packed_a);
    b_bus.write(packed_b);
  }

  // Unpack the packed result bus back into the native CIMElement result ports
  void unpack_outputs() {
    const ac_int<C_BUS_WIDTH, false> packed_c = c_bus.read();
    COutput c_output;
    for (int b_col_idx = 0; b_col_idx < B_COLS; b_col_idx++) {
      c_output[b_col_idx] =
          packed_c.template slc<C_WIDTH>(b_col_idx * C_WIDTH);
    }
    c.write(c_output);
  }
};
