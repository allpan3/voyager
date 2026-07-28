// SystemC HLS adapter for the integer CIM element RTL block
//
// CIMElement is a PE-level Catapult block boundary for the native CIM vector
// and matrix interface. The synthesized implementation blackboxes the existing
// SystemVerilog CIMIntElement, while the C++ body provides event-level
// simulation of the issue/retire protocol: mac_issue accepted while mac_ready,
// results retiring into c with a one-cycle c_retire pulse after a fixed latency
//
// tensor geometry:
//
//                         N
//                   +-----------+
//               K   |  B[K][N]  |
//                   +-----------+
//       K                 N
//   +---------+       +-----------+
// M | A[M][K] |  x  = | C[M][N]  |
//   +---------+       +-----------+
//
// M is temporal: one issue consumes A[m][0:K] and produces C[m][0:N]
// BK is the B write block along K, so b[bk][n] writes B[wchi + bk][n]
// The macro mapping is CH_IN=K, WRITE_CH_IN=BK, and CH_OUT=N*NUM_B_SLICES

#pragma once

#include <ac_blackbox.h>
#include <ac_int.h>
#include <systemc.h>

#include <sstream>

#include "AccelTypes.h"
#include "ArchitectureParams.h"

// CH_IN, CH_OUT, and WRITE_CH_IN describe the physical macro shape
// K, N, and BK describe the logical tensor shape
// A_WIDTH, B_WIDTH, and SIGNED describe the logical arithmetic
// CIMElementPacked owns the Catapult blackbox ABI with packed A/B/C buses
template <int CH_IN, int CH_OUT, int B_SETS, int BASE_A_WIDTH, int BASE_B_WIDTH,
          int BASE_C_WIDTH, int WRITE_CH_IN, int MAC_LATENCY, int MODE,
          int A_WIDTH, int B_WIDTH, bool SIGNED>
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
  static constexpr int CIM_MODE_BIT_PARALLEL_VALUE = 0;
  static constexpr int CIM_MODE_BIT_SERIAL_VALUE = 1;
  static constexpr int K = CH_IN;
  static constexpr int BK = WRITE_CH_IN;
  static constexpr int SUM_GUARD_WIDTH = (K <= 1) ? 1 : log2_ceil(K);
  static constexpr int NUM_B_SLICES =
      (BASE_B_WIDTH > 0 && B_WIDTH >= BASE_B_WIDTH) ? (B_WIDTH / BASE_B_WIDTH)
                                                    : 0;
  static constexpr int N = (NUM_B_SLICES > 0) ? (CH_OUT / NUM_B_SLICES) : 0;
  static constexpr int C_WIDTH = A_WIDTH + B_WIDTH + SUM_GUARD_WIDTH;
  static constexpr int BITS_K = (K <= 1) ? 1 : log2_ceil(K);
  static constexpr int BITS_SET = (B_SETS <= 1) ? 1 : log2_ceil(B_SETS);
  static constexpr int A_BUS_WIDTH = K * A_WIDTH;
  static constexpr int B_BUS_WIDTH = N * BK * B_WIDTH;
  static constexpr int C_BUS_WIDTH = N * C_WIDTH;

  // B-set selector shared by B writes and MAC issues
  using WSet = ac_int<BITS_SET, false>;

  static_assert(K > 0, "K must be positive");
  static_assert(CH_OUT > 0, "CH_OUT must be positive");
  static_assert(B_SETS > 0, "B_SETS must be positive");
  static_assert(BASE_A_WIDTH > 0, "BASE_A_WIDTH must be positive");
  static_assert(BASE_B_WIDTH > 0, "BASE_B_WIDTH must be positive");
  static_assert(BASE_C_WIDTH > 0, "BASE_C_WIDTH must be positive");
  static_assert(BK > 0, "BK must be positive");
  static_assert((K % BK) == 0, "K must be divisible by BK");
  static_assert(MAC_LATENCY > 0, "MAC_LATENCY must be positive");
  static_assert(A_WIDTH > 0, "A_WIDTH must be positive");
  static_assert(B_WIDTH > 0, "B_WIDTH must be positive");
  static_assert(B_WIDTH >= BASE_B_WIDTH,
                "B_WIDTH must be at least BASE_B_WIDTH");
  static_assert((BASE_B_WIDTH > 0) && ((B_WIDTH % BASE_B_WIDTH) == 0),
                "B_WIDTH must be a multiple of BASE_B_WIDTH");
  static_assert((NUM_B_SLICES > 0) && ((CH_OUT % NUM_B_SLICES) == 0),
                "CH_OUT must be divisible by NUM_B_SLICES");
  static_assert(MODE == 0 || MODE == CIM_MODE_BIT_SERIAL_VALUE,
                "MODE must be bit-parallel (0) or bit-serial (1)");
  // A bit-serial macro shift-accumulates one A slice internally, so its own
  // accumulator has to hold that slice's partial sum: the slice itself, one B
  // operand, and the reduction guard over K.
  static_assert(MODE == CIM_MODE_BIT_PARALLEL_VALUE ||
                    BASE_C_WIDTH > BASE_B_WIDTH + SUM_GUARD_WIDTH,
                "bit-serial requires BASE_C_WIDTH > BASE_B_WIDTH + "
                "SUM_GUARD_WIDTH so one A slice is at least 1 bit");

  // Return the number of mclk cycles an accepted issue keeps the element not
  // ready
  static constexpr int issue_window() {
    constexpr int serial_max_slice_width =
        BASE_C_WIDTH - BASE_B_WIDTH - SUM_GUARD_WIDTH;
    constexpr int serial_slice_width = min_int(A_WIDTH, serial_max_slice_width);
    constexpr int slice_width =
        (MODE == CIM_MODE_BIT_SERIAL_VALUE) ? serial_slice_width : BASE_A_WIDTH;
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

  // Packed logical A vector; a_bus and mset remain stable throughout an
  // accepted issue window
  sc_in<ac_int<A_BUS_WIDTH, false>> CCS_INIT_S1(a_bus);

  // Packed logical B matrix with all N columns and BK rows along K
  sc_in<ac_int<B_BUS_WIDTH, false>> CCS_INIT_S1(b_bus);
  sc_in<bool> CCS_INIT_S1(wen);
  sc_in<ac_int<BITS_K, false>> CCS_INIT_S1(wchi);
  sc_in<WSet> CCS_INIT_S1(wset);

  // CIMElement MAC issue control
  sc_in<bool> CCS_INIT_S1(mac_issue);
  sc_in<WSet> CCS_INIT_S1(mset);

  // CIMElement packed result interface; c_bus holds the last retired result
  sc_out<ac_int<C_BUS_WIDTH, false>> CCS_INIT_S1(c_bus);
  sc_out<bool> CCS_INIT_S1(c_retire);
  sc_out<bool> CCS_INIT_S1(mac_ready);

 private:
  // Resetless row-major B storage; each write fills BK rows along K
  ac_int<B_WIDTH, false> b_mem[B_SETS][K][N];

  // PendingResult carries one computed result through the fixed retire latency
  struct PendingResult {
    ac_int<C_WIDTH, false> value[N];
    int cycles_remaining;
  };

  // Resettable C++ registers owned by the mclk process. The pending queue is
  // statically bounded so Catapult can analyze the behavioral model while the
  // RTL implementation remains an ac_blackbox.
  static constexpr int MAX_PENDING_RESULTS = operation_latency() + 1;
  PendingResult pending_results[MAX_PENDING_RESULTS];
  int pending_results_size;
  int window_remaining;
  sc_signal<bool> window_idle_state;

 public:
  // Construct the packed CIMElement behavioral model and blackbox metadata
  SC_CTOR(CIMElementPacked)
      : pending_results_size(0), window_remaining(0) {
    initialize_model_state();

    SC_METHOD(write_b);
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
        .verilog_files(
            "CIM/cim_macro_wrapper.sv CIM/cim_macro_model.sv "
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
  // Initialize observable model state while leaving resetless B storage
  // untouched
  void initialize_model_state() {
    pending_results_size = 0;
    window_remaining = 0;
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
  static constexpr int b_bus_offset(int bk, int n) {
    return ((bk * N) + n) * B_WIDTH;
  }

  // Write one logical BK-wide B block into the C++ model
  void write_b() {
    if (!wen.read()) {
      return;
    }

    const int set = wset.read().to_int();
    const int base_k = wchi.read().to_int();

    if (set >= B_SETS) {
      return;
    }

    const ac_int<B_BUS_WIDTH, false> b_value = b_bus.read();
    for (int bk = 0; bk < BK; bk++) {
      for (int n = 0; n < N; n++) {
        const int k = base_k + bk;
        if (k < K) {
          b_mem[set][k][n] = b_value.template slc<B_WIDTH>(b_bus_offset(bk, n));
        }
      }
    }
  }

  // Compute one native CIM matrix-vector operation for the issued payload
  PendingResult compute_result() {
    PendingResult pending;
    pending.cycles_remaining = operation_latency();
    const int set = mset.read().to_int();
    const ac_int<A_BUS_WIDTH, false> a_value_bus = a_bus.read();

    for (int n = 0; n < N; n++) {
      ac_int<C_WIDTH, SIGNED> acc = 0;

      if (set < B_SETS) {
        for (int k = 0; k < K; k++) {
          const ac_int<A_WIDTH, SIGNED> a_value = decode_operand<A_WIDTH>(
              a_value_bus.template slc<A_WIDTH>(k * A_WIDTH));
          const ac_int<B_WIDTH, SIGNED> b_value =
              decode_operand<B_WIDTH>(b_mem[set][k][n]);
          acc += a_value * b_value;
        }
      }
      pending.value[n] = acc;
    }
    return pending;
  }

  // Clear resettable CIM element state while preserving resetless B storage
  void reset_element_state() {
    pending_results_size = 0;
    window_remaining = 0;
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

    // c_retire is a one-cycle pulse: default it low every edge and raise it
    // only on the edge a result retires
    c_retire.write(false);

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
        for (int n = 0; n < N; n++) {
          packed_result.set_slc(n * C_WIDTH, pending_results[0].value[n]);
        }
        c_bus.write(packed_result);
        c_retire.write(true);
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
  // Report an illegal write to the B set consumed by the active issue window
  void check_write_mac_collision() {
    const bool window_active = !window_idle_state.read();
    const bool issue_start = mac_issue.read() && !window_active;
    if (rstn.read() && wen.read() && (issue_start || window_active) &&
        wset.read() == mset.read()) {
      std::ostringstream message;
      message << "write targets B set " << wset.read().to_int()
              << " while its MAC issue window is active";
      SC_REPORT_ERROR("CIMElement B set protocol violation",
                      message.str().c_str());
    }
  }
#endif
};

// CIMElement keeps the native A/B/C interface and adapts it to the packed
// blackbox ABI
template <int CH_IN, int CH_OUT, int B_SETS, int BASE_A_WIDTH, int BASE_B_WIDTH,
          int BASE_C_WIDTH, int WRITE_CH_IN, int MAC_LATENCY, int MODE,
          int A_WIDTH, int B_WIDTH, bool SIGNED>
SC_MODULE(CIMElement) {
 private:
  using PackedElement =
      CIMElementPacked<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH,
                       BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH,
                       B_WIDTH, SIGNED>;

 public:
  static constexpr int CIM_MODE_BIT_SERIAL_VALUE =
      PackedElement::CIM_MODE_BIT_SERIAL_VALUE;
  static constexpr int K = PackedElement::K;
  static constexpr int BK = PackedElement::BK;
  static constexpr int SUM_GUARD_WIDTH = PackedElement::SUM_GUARD_WIDTH;
  static constexpr int NUM_B_SLICES = PackedElement::NUM_B_SLICES;
  static constexpr int N = PackedElement::N;
  static constexpr int C_WIDTH = PackedElement::C_WIDTH;
  static constexpr int BITS_K = PackedElement::BITS_K;
  static constexpr int BITS_SET = PackedElement::BITS_SET;
  static constexpr int A_BUS_WIDTH = PackedElement::A_BUS_WIDTH;
  static constexpr int B_BUS_WIDTH = PackedElement::B_BUS_WIDTH;
  static constexpr int C_BUS_WIDTH = PackedElement::C_BUS_WIDTH;

  // CIMElement data shapes match one K-wide A vector, one BK-by-N B block, and
  // one N-wide C vector
  using WSet = typename PackedElement::WSet;
  using AData = Pack1D<ac_int<A_WIDTH, false>, K>;
  using BData = Pack1D<Pack1D<ac_int<B_WIDTH, false>, N>, BK>;
  using CData = Pack1D<ac_int<C_WIDTH, false>, N>;

  // Return the number of mclk cycles an accepted issue keeps the element not
  // ready
  static constexpr int issue_window() { return PackedElement::issue_window(); }

  // Return the number of mclk cycles from an accepted issue to its retirement
  static constexpr int operation_latency() {
    return PackedElement::operation_latency();
  }

  // CIMElement clock and reset interface
  sc_in<bool> CCS_INIT_S1(wclk);
  sc_in<bool> CCS_INIT_S1(mclk);
  sc_in<bool> CCS_INIT_S1(rstn);

  // Logical A vector; a and mset remain stable throughout an accepted issue
  // window
  sc_in<AData> CCS_INIT_S1(a);

  // Logical B matrix block with all N columns and BK rows along K
  sc_in<BData> CCS_INIT_S1(b);
  sc_in<bool> CCS_INIT_S1(wen);
  sc_in<ac_int<BITS_K, false>> CCS_INIT_S1(wchi);
  sc_in<WSet> CCS_INIT_S1(wset);

  // CIMElement MAC issue interface
  sc_in<bool> CCS_INIT_S1(mac_issue);
  sc_in<WSet> CCS_INIT_S1(mset);

  // CIMElement result interface
  sc_out<CData> CCS_INIT_S1(c);
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
    packed.wchi(wchi);
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
  static constexpr int b_bus_offset(int bk, int n) {
    return ((bk * N) + n) * B_WIDTH;
  }

  // Pack array-shaped public A/B ports into stable vector ports for Catapult
  void pack_inputs() {
    ac_int<A_BUS_WIDTH, false> packed_a = 0;
    ac_int<B_BUS_WIDTH, false> packed_b = 0;
    const AData a_data = a.read();
    const BData b_data = b.read();

    for (int k = 0; k < K; k++) {
      packed_a.set_slc(k * A_WIDTH, a_data[k]);
    }

    for (int bk = 0; bk < BK; bk++) {
      for (int n = 0; n < N; n++) {
        packed_b.set_slc(b_bus_offset(bk, n), b_data[bk][n]);
      }
    }

    a_bus.write(packed_a);
    b_bus.write(packed_b);
  }

  // Unpack the packed result bus back into the native CIMElement result ports
  void unpack_outputs() {
    const ac_int<C_BUS_WIDTH, false> packed_c = c_bus.read();
    CData c_data;
    for (int n = 0; n < N; n++) {
      c_data[n] = packed_c.template slc<C_WIDTH>(n * C_WIDTH);
    }
    c.write(c_data);
  }
};
