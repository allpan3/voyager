// CIMTile is a self-contained grid of CIM elements with a registered result

#pragma once

#include <ac_int.h>
#include <systemc.h>

#include "AccelTypes.h"
#include "ArchitectureParams.h"
#include "CIMElement.h"

// CIMTile is the intersection of one multicast group and one reduction group:
// a TILE_INPUT_LANES x TILE_OUTPUT_LANES grid of CIMElements that reduces one
// streamed A section against the resident B tile and emits one registered
// result row per tile output lane. It exposes the CIMElement contract one level
// up -- VG issue side (a/mset/mac_issue in, mac_ready out), registered result
// (c/c_retire out), SC write side (wen/waddr/wset/b in) -- and is timing-pure:
// no Connections, no protocol state, only plain signals inside.
//
// Relative to a bare element the tile adds two registered stages: it latches the
// A section and set select on the issue pulse (so the delivered wires need only
// be stable for that one cycle) and pulses the elements one cycle later, and it
// registers the reduced result one cycle after the elements retire. issue_window
// therefore grows by one cycle and operation_latency by two.
template <int CH_IN = CIM_CH_IN, int CH_OUT = CIM_CH_OUT,
          int B_SETS = CIM_B_SETS, int BASE_A_WIDTH = CIM_BASE_A_WIDTH,
          int BASE_B_WIDTH = CIM_BASE_B_WIDTH,
          int BASE_C_WIDTH = CIM_BASE_C_WIDTH, int WRITE_CH_IN = CIM_WRITE_CH_IN,
          int MAC_LATENCY = CIM_MAC_LATENCY, int MODE = CIM_MODE,
          int A_WIDTH = INPUT_DTYPE_WIDTH, int B_WIDTH = WEIGHT_DTYPE_WIDTH,
          bool SIGNED = CIM_SIGNED, int TILE_INPUT_LANES = 1,
          int TILE_OUTPUT_LANES = 1>
SC_MODULE(CIMTile) {
 private:
  // Return the ceil log2 used for static port widths
  static constexpr int log2_ceil(int value) {
    return (value <= 1) ? 0 : 1 + log2_ceil((value + 1) / 2);
  }

  using Element = CIMElement<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH,
                             BASE_B_WIDTH, BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY,
                             MODE, A_WIDTH, B_WIDTH, SIGNED>;

 public:
  static_assert(TILE_INPUT_LANES > 0, "TILE_INPUT_LANES must be positive");
  static_assert(TILE_OUTPUT_LANES > 0, "TILE_OUTPUT_LANES must be positive");

  static constexpr int ELEMENT_A_COLS = Element::A_COLS;
  static constexpr int ELEMENT_B_COLS = Element::B_COLS;
  static constexpr int ELEMENT_B_WRITE_ROWS = Element::B_ROWS;
  static constexpr int ELEMENT_C_WIDTH = Element::C_WIDTH;

  // Reduction across the tile's input lanes widens the result by a guard field
  static constexpr int REDUCTION_GUARD_WIDTH =
      (TILE_INPUT_LANES <= 1) ? 0 : log2_ceil(TILE_INPUT_LANES);
  static constexpr int C_WIDTH = ELEMENT_C_WIDTH + REDUCTION_GUARD_WIDTH;

  // CIMElement-level scalar types
  using ElementAValue = ac_int<A_WIDTH, false>;
  using ElementBValue = ac_int<B_WIDTH, false>;
  using ElementCValue = ac_int<ELEMENT_C_WIDTH, false>;
  using ElementSet = ac_int<Element::BITS_SET, false>;
  using ElementAddr = ac_int<Element::BITS_CH_IN, false>;

  // Tile-level result scalar type (widened for the input-lane reduction)
  using CValue = ac_int<C_WIDTH, false>;

  // Return the number of mclk cycles an accepted issue keeps the tile not ready.
  // One more than the element window: the tile spends the issue cycle latching
  // the A section before the elements consume it
  static constexpr int issue_window() { return Element::issue_window() + 1; }

  // Return the number of mclk cycles from an accepted issue to its retirement.
  // Two more than the element: one latch stage in and one result-register stage
  // out
  static constexpr int operation_latency() {
    return Element::operation_latency() + 2;
  }

  // Tile clock and reset interface
  sc_in<bool> CCS_INIT_S1(wclk);
  sc_in<bool> CCS_INIT_S1(mclk);
  sc_in<bool> CCS_INIT_S1(rstn);

  // Write interface (SC): pass-through per tile input lane, B payload per element
  sc_in<bool> wen[TILE_INPUT_LANES];
  sc_in<ElementAddr> waddr[TILE_INPUT_LANES];
  sc_in<ElementSet> wset[TILE_INPUT_LANES];
  sc_in<ElementBValue> b[TILE_INPUT_LANES][TILE_OUTPUT_LANES][ELEMENT_B_COLS][ELEMENT_B_WRITE_ROWS];

  // MAC issue interface (VG): one A section shared across the tile's output
  // lanes, latched on the issue pulse. mac_ready is exposed for completeness;
  // the producer must not read it combinationally into its issue decision
  sc_in<ElementAValue> a[TILE_INPUT_LANES][ELEMENT_A_COLS];
  sc_in<ElementSet> CCS_INIT_S1(mset);
  sc_in<bool> CCS_INIT_S1(mac_issue);
  sc_out<bool> CCS_INIT_S1(mac_ready);

  // Result interface: registered reduced result plus a per-retirement toggle
  sc_out<CValue> c[TILE_OUTPUT_LANES][ELEMENT_B_COLS];
  sc_out<bool> CCS_INIT_S1(c_retire);

 private:
  Element* elements[TILE_INPUT_LANES][TILE_OUTPUT_LANES];

  // Station registers hold the latched A section and set select for the window.
  // Plain sc_signal (not the ZeroInitializedTileSignal subclass) so Catapult
  // recognizes them as channels when bound to the element a/mset ports (CIN-216);
  // they are latched before the elements consume them, so no forced zero start.
  sc_signal<ElementAValue> station_a[TILE_INPUT_LANES][ELEMENT_A_COLS];
  sc_signal<ElementSet> station_mset;
  // Registered issue pulse fed to the elements one cycle after the tile issue
  sc_signal<bool> element_mac_issue;

  // Per-element outputs. element_c must be a plain sc_signal (not the
  // ZeroInitializedTileSignal subclass): Catapult only models sc_signal<T> as a
  // synthesizable channel, so reading a subclass in run_collect's reduction
  // aborts go compile (CIN-242). It is read only after the element has driven it
  // (guarded by tile_retired), so it needs no forced zero startup value.
  sc_signal<ElementCValue> element_c[TILE_INPUT_LANES][TILE_OUTPUT_LANES][ELEMENT_B_COLS];
  sc_signal<bool> element_c_retire[TILE_INPUT_LANES][TILE_OUTPUT_LANES];
  sc_signal<bool> element_mac_ready[TILE_INPUT_LANES][TILE_OUTPUT_LANES];

  // Combinational ready state published by the issue thread; the issue-window
  // countdown and retire tracking are thread-local (see run_issue/run_collect)
  sc_signal<bool> window_idle_state;

 public:
  // Construct the element grid and bind it to the tile's station and result wires
  SC_CTOR(CIMTile) {
    window_idle_state.write(true);

    for (int til = 0; til < TILE_INPUT_LANES; til++) {
      for (int tol = 0; tol < TILE_OUTPUT_LANES; tol++) {
        elements[til][tol] = new Element(sc_gen_unique_name("element"));

        elements[til][tol]->wclk(wclk);
        elements[til][tol]->mclk(mclk);
        elements[til][tol]->rstn(rstn);
        elements[til][tol]->wen(wen[til]);
        elements[til][tol]->waddr(waddr[til]);
        elements[til][tol]->wset(wset[til]);
        elements[til][tol]->mac_issue(element_mac_issue);
        elements[til][tol]->mset(station_mset);
        elements[til][tol]->c_retire(element_c_retire[til][tol]);
        elements[til][tol]->mac_ready(element_mac_ready[til][tol]);

        for (int a_col = 0; a_col < ELEMENT_A_COLS; a_col++) {
          elements[til][tol]->a[a_col](station_a[til][a_col]);
        }

        for (int b_col = 0; b_col < ELEMENT_B_COLS; b_col++) {
          elements[til][tol]->c[b_col](element_c[til][tol][b_col]);
          for (int b_row = 0; b_row < ELEMENT_B_WRITE_ROWS; b_row++) {
            elements[til][tol]->b[b_col][b_row](b[til][tol][b_col][b_row]);
          }
        }
      }
    }

    SC_THREAD(run_issue);
    sensitive << mclk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(run_collect);
    sensitive << mclk.pos();
    async_reset_signal_is(rstn, false);

    SC_METHOD(drive_mac_ready);
    sensitive << rstn << window_idle_state;
  }

 private:
  // Latch the A section on an accepted issue and pulse the elements next cycle.
  // The tile tracks its own issue window statically, mirroring the element, so
  // it never depends on the element mac_ready combinationally. Clocked SC_THREAD
  // with async reset (Catapult wants sequential logic in a thread with reset);
  // the window countdown is thread-local. Work-then-wait keeps the same edge
  // ordering as the former SC_METHOD: reads return pre-edge values, and the one
  // wait() per loop places all writes on this edge (visible next edge)
  void run_issue() {
    // Reset section: clear the station, the element pulse, and the window
    int window_remaining = 0;
    element_mac_issue.write(false);
    station_mset.write(0);
#pragma hls_unroll yes
    for (int til = 0; til < TILE_INPUT_LANES; til++) {
#pragma hls_unroll yes
      for (int a_col = 0; a_col < ELEMENT_A_COLS; a_col++) {
        station_a[til][a_col].write(0);
      }
    }
    window_idle_state.write(true);

    wait();

    while (true) {
      // Sample readiness before this edge's updates, mirroring the element window
      const bool ready_now = (window_remaining == 0);
      if (window_remaining > 0) {
        window_remaining--;
      }

      if (mac_issue.read() && ready_now) {
#pragma hls_unroll yes
        for (int til = 0; til < TILE_INPUT_LANES; til++) {
#pragma hls_unroll yes
          for (int a_col = 0; a_col < ELEMENT_A_COLS; a_col++) {
            station_a[til][a_col].write(a[til][a_col].read());
          }
        }
        station_mset.write(mset.read());
        // Registered pulse: the elements observe it, with the latched station,
        // on the following edge
        element_mac_issue.write(true);
        window_remaining = issue_window() - 1;
      } else {
        element_mac_issue.write(false);
      }

      window_idle_state.write(window_remaining == 0);
      wait();
    }
  }

  // Widen one element output before the tile-level reduction
  static CValue widen_element_c(ElementCValue value) {
    CValue widened = 0;
    if constexpr (SIGNED) {
      ac_int<ELEMENT_C_WIDTH, true> signed_value;
      // Copy the raw bits, preserving the two's-complement pattern
      signed_value.set_slc(0, value);
      // Sign-extend to the widened result width
      ac_int<C_WIDTH, true> signed_widened = signed_value;
      widened = signed_widened;
    } else {
      widened = value;
    }
    return widened;
  }

  // Return whether every element in the tile has flipped past the seen toggle.
  // All of a tile's elements retire in the same cycle by construction
  bool tile_retired(bool seen_retire) const {
    bool retired = true;
#pragma hls_unroll yes
    for (int til = 0; til < TILE_INPUT_LANES; til++) {
#pragma hls_unroll yes
      for (int tol = 0; tol < TILE_OUTPUT_LANES; tol++) {
        retired = retired && (element_c_retire[til][tol].read() != seen_retire);
      }
    }
    return retired;
  }

  // Register the reduced result and flip the retire toggle on element retirement.
  // Clocked SC_THREAD with async reset; the seen/retire toggles are thread-local
  void run_collect() {
    // Reset section: clear the result registers and retire tracking
    bool seen_retire = false;
    bool tile_retire_state = false;
    for (int tol = 0; tol < TILE_OUTPUT_LANES; tol++) {
      for (int b_col = 0; b_col < ELEMENT_B_COLS; b_col++) {
        c[tol][b_col].write(0);
      }
    }
    c_retire.write(false);

    wait();

    while (true) {
      if (tile_retired(seen_retire)) {
        // Reduce inline so element_c is indexed only by loop variables, like
        // tile_retired. Catapult's front end statically enumerates these loops
        // to resolve each element_c signal object; passing tol/b_col as function
        // arguments (the old reduce_input_lanes helper) left the signal lvalue
        // unresolved and aborted go compile (CIN-242, NULL pointer for lvalue).
#pragma hls_unroll yes
        for (int tol = 0; tol < TILE_OUTPUT_LANES; tol++) {
#pragma hls_unroll yes
          for (int b_col = 0; b_col < ELEMENT_B_COLS; b_col++) {
            CValue sum = 0;
#pragma hls_unroll yes
            for (int til = 0; til < TILE_INPUT_LANES; til++) {
              sum += widen_element_c(element_c[til][tol][b_col].read());
            }
            c[tol][b_col].write(sum);
          }
        }
        seen_retire = !seen_retire;
        tile_retire_state = !tile_retire_state;
        c_retire.write(tile_retire_state);
      }
      wait();
    }
  }

  // Drive combinational ready state from reset and the issue-window state
  void drive_mac_ready() {
    mac_ready.write(rstn.read() && window_idle_state.read());
  }
};
