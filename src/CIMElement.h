// SystemC HLS adapter for the integer CIM element RTL block
//
// CIMElement is a PE-level Catapult block boundary for the native CIM vector and
// matrix interface. The synthesized implementation blackboxes the existing
// SystemVerilog CIMIntElement, while the C++ body provides event-level simulation

#pragma once

#include <ac_int.h>
#include <systemc.h>

#include <ac_blackbox.h>

#include "ArchitectureParams.h"

// Provide standalone lint/test defaults when generated CIM macro wrappers are absent
#ifndef CIM_CH_IN
#define CIM_CH_IN 4
#endif

#ifndef CIM_CH_OUT
#define CIM_CH_OUT 2
#endif

#ifndef CIM_B_SETS
#define CIM_B_SETS 2
#endif

#ifndef CIM_BASE_A_WIDTH
#define CIM_BASE_A_WIDTH 4
#endif

#ifndef CIM_BASE_B_WIDTH
#define CIM_BASE_B_WIDTH 4
#endif

#ifndef CIM_BASE_C_WIDTH
#define CIM_BASE_C_WIDTH 12
#endif

#ifndef CIM_WRITE_CH_IN
#define CIM_WRITE_CH_IN 2
#endif

#ifndef CIM_MAC_LATENCY
#define CIM_MAC_LATENCY 2
#endif

#ifndef CIM_MODE
#define CIM_MODE 0
#endif

#ifndef CIM_SIGNED
#define CIM_SIGNED false
#endif

// CIMElement adapts CIMIntElement with the same native CIM interface at PE hierarchy
template <int CH_IN = CIM_CH_IN, int CH_OUT = CIM_CH_OUT,
          int B_SETS = CIM_B_SETS, int BASE_A_WIDTH = CIM_BASE_A_WIDTH,
          int BASE_B_WIDTH = CIM_BASE_B_WIDTH,
          int BASE_C_WIDTH = CIM_BASE_C_WIDTH, int WRITE_CH_IN = CIM_WRITE_CH_IN,
          int MAC_LATENCY = CIM_MAC_LATENCY, int MODE = CIM_MODE,
          int A_WIDTH = INPUT_DTYPE_WIDTH, int B_WIDTH = WEIGHT_DTYPE_WIDTH,
          bool SIGNED = CIM_SIGNED>
SC_MODULE(CIMElement) {
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

  // CIMElement clock and reset interface
  sc_in<bool> CCS_INIT_S1(wclk);
  sc_in<bool> CCS_INIT_S1(mclk);
  sc_in<bool> CCS_INIT_S1(rstn);

  // CIMElement weight-write interface
  sc_in<ac_int<A_WIDTH, false>> a[A_COLS];
  sc_in<ac_int<B_WIDTH, false>> b[B_COLS][B_ROWS];
  sc_in<bool> CCS_INIT_S1(wen);
  sc_in<ac_int<BITS_CH_IN, false>> CCS_INIT_S1(widx);
  sc_in<ac_int<BITS_SET, false>> CCS_INIT_S1(wset);

  // CIMElement MAC start interface
  sc_in<bool> CCS_INIT_S1(mac_start);
  sc_in<ac_int<BITS_SET, false>> CCS_INIT_S1(mset);

  // CIMElement result and flow-control interface
  sc_out<ac_int<C_WIDTH, false>> c[B_COLS];
  sc_out<bool> CCS_INIT_S1(c_valid);
  sc_out<bool> CCS_INIT_S1(mac_busy);

 private:
  // Resetless weight storage matching the RTL CIM macro wrapper memory
  ac_int<B_WIDTH, false> weight_mem[B_SETS][B_COLS][A_COLS];

  // Resettable C++ registers owned by the mclk process
  ac_int<C_WIDTH, false> pending_result[B_COLS];
  ac_int<C_WIDTH, false> c_reg[B_COLS];
  bool operation_active;
  int cycles_remaining;
  sc_signal<bool> operation_active_state;

 public:
  // Construct the CIMElement behavioral model and blackbox metadata
  SC_CTOR(CIMElement)
      : operation_active(false),
      cycles_remaining(0) {
    initialize_model_state();

    SC_METHOD(write_weights);
    sensitive << wclk.pos();
    dont_initialize();

    SC_METHOD(run_mclk);
    sensitive << mclk.pos() << rstn.neg();
    dont_initialize();

    SC_METHOD(drive_mac_busy);
    sensitive << rstn << operation_active_state;

    ac_blackbox()
        .entity("CIMIntElement")
        .verilog_files("CIM/cim_typedefs.svh CIM/cim_macro_wrapper.sv "
                       "CIM/cim_macro_model.sv CIM/cim_macro_1.sv CIM/cim_element.sv")
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
  // Return the logical number of mclk cycles before a result is valid
  static constexpr int operation_latency() {
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
    return num_slices * slice_launch_interval + MAC_LATENCY - 1;
  }

  // Initialize observable model state while leaving resetless weights untouched
  void initialize_model_state() {
    for (int b_col_idx = 0; b_col_idx < B_COLS; b_col_idx++) {
      pending_result[b_col_idx] = 0;
      c_reg[b_col_idx] = 0;
    }
    operation_active_state.write(false);
  }

  // Decode a CIM operand with the configured signedness
  template <int WIDTH>
  static ac_int<WIDTH, SIGNED> decode_operand(ac_int<WIDTH, false> value) {
    ac_int<WIDTH, SIGNED> decoded;
    decoded.set_slc(0, value);
    return decoded;
  }

  // Write one logical B row group into the resetless C++ weight model
  void write_weights() {
    if (!wen.read()) {
      return;
    }

    const int macro_row_idx = wset.read().to_int();
    const int base_a_col = widx.read().to_int();

    if (macro_row_idx >= B_SETS) {
      return;
    }

    for (int b_col_idx = 0; b_col_idx < B_COLS; b_col_idx++) {
      for (int b_row_idx = 0; b_row_idx < B_ROWS; b_row_idx++) {
        const int a_col_idx = base_a_col + b_row_idx;
        if (a_col_idx < A_COLS) {
          weight_mem[macro_row_idx][b_col_idx][a_col_idx] =
              b[b_col_idx][b_row_idx].read();
        }
      }
    }
  }

  // Compute one native CIM matrix-vector operation into the pending result
  void compute_result() {
    const int macro_row_idx = mset.read().to_int();

    for (int b_col_idx = 0; b_col_idx < B_COLS; b_col_idx++) {
      ac_int<C_WIDTH, SIGNED> acc = 0;

      if (macro_row_idx < B_SETS) {
        for (int a_col_idx = 0; a_col_idx < A_COLS; a_col_idx++) {
          const ac_int<A_WIDTH, SIGNED> a_value =
              decode_operand<A_WIDTH>(a[a_col_idx].read());
          const ac_int<B_WIDTH, SIGNED> b_value =
              decode_operand<B_WIDTH>(
                  weight_mem[macro_row_idx][b_col_idx][a_col_idx]);
          acc += a_value * b_value;
        }
      }
      pending_result[b_col_idx] = acc;
    }
  }

  // Clear resettable CIM element state while preserving resetless weights
  void reset_element_state() {
    operation_active = false;
    operation_active_state.write(false);
    cycles_remaining = 0;
    for (int b_col_idx = 0; b_col_idx < B_COLS; b_col_idx++) {
      pending_result[b_col_idx] = 0;
      c_reg[b_col_idx] = 0;
      c[b_col_idx].write(0);
    }
    c_valid.write(false);
  }

  // Advance the mclk-domain request and result state machine
  void run_mclk() {
    if (!rstn.read()) {
      reset_element_state();
      return;
    }

    if (operation_active) {
      if (cycles_remaining <= 1) {
        operation_active = false;
        operation_active_state.write(false);
        cycles_remaining = 0;
        for (int b_col_idx = 0; b_col_idx < B_COLS; b_col_idx++) {
          c_reg[b_col_idx] = pending_result[b_col_idx];
          c[b_col_idx].write(c_reg[b_col_idx]);
        }
        c_valid.write(true);
      } else {
        cycles_remaining--;
      }
      return;
    }

    if (mac_start.read()) {
      compute_result();
      operation_active = true;
      operation_active_state.write(true);
      cycles_remaining = operation_latency();
      c_valid.write(false);
      return;
    }
  }

  // Drive combinational busy state from reset and active MAC state
  void drive_mac_busy() {
    mac_busy.write(!rstn.read() || operation_active_state.read());
  }
};
