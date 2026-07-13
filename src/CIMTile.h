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
// result row per tile output lane. It exposes a self-contained issue station
// (a/mset/mac_issue in, mac_ready out), registered result (c/c_retire out), and
// SC write side (wen/waddr/wset/b in), with no Connections protocol at this level.
//
// Relative to a bare element the tile adds two registered stages: the input
// station captures A/mset on an accepted issue and pulses the elements on the
// following cycle, then the result stage registers the reduced element outputs.
template <int CH_IN, int CH_OUT, int B_SETS, int BASE_A_WIDTH, int BASE_B_WIDTH, int BASE_C_WIDTH, int WRITE_CH_IN,
          int MAC_LATENCY, int MODE, int A_WIDTH, int B_WIDTH, bool SIGNED, int TILE_INPUT_LANES, int TILE_OUTPUT_LANES>
SC_MODULE(CIMTile) {
 private:
  // Return the ceil log2 used for static port widths
  static constexpr int log2_ceil(int value) { return (value <= 1) ? 0 : 1 + log2_ceil((value + 1) / 2); }

  using Element = CIMElement<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH, BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY,
                             MODE, A_WIDTH, B_WIDTH, SIGNED>;

 public:
  static_assert(TILE_INPUT_LANES > 0, "TILE_INPUT_LANES must be positive");
  static_assert(TILE_OUTPUT_LANES > 0, "TILE_OUTPUT_LANES must be positive");

  static constexpr int ELEMENT_A_COLS = Element::A_COLS;
  static constexpr int ELEMENT_B_COLS = Element::B_COLS;
  static constexpr int ELEMENT_B_WRITE_ROWS = Element::B_ROWS;
  static constexpr int ELEMENT_C_WIDTH = Element::C_WIDTH;

  // Reduction across the tile's input lanes widens the result by a guard field
  static constexpr int REDUCTION_GUARD_WIDTH = (TILE_INPUT_LANES <= 1) ? 0 : log2_ceil(TILE_INPUT_LANES);
  static constexpr int C_WIDTH = ELEMENT_C_WIDTH + REDUCTION_GUARD_WIDTH;

  // CIMElement-level scalar types
  using ElementAValue = ac_int<A_WIDTH, false>;
  using ElementBValue = ac_int<B_WIDTH, false>;
  using ElementCValue = ac_int<ELEMENT_C_WIDTH, false>;
  using BSet = typename Element::BSet;
  using ElementAddr = ac_int<Element::BITS_CH_IN, false>;

  // Grouped CIMElement data-port types
  using ElementAInput = typename Element::AInput;
  using ElementBInput = typename Element::BInput;
  using ElementCOutput = typename Element::COutput;

  // Tile-level result scalar type (widened for the input-lane reduction)
  using CValue = ac_int<C_WIDTH, false>;
  using COutput = Pack1D<CValue, ELEMENT_B_COLS>;

  // Return the number of mclk cycles an accepted issue keeps the tile not ready
  static constexpr int issue_window() { return Element::issue_window(); }

  // Return the number of mclk cycles from an accepted tile issue to retirement
  static constexpr int operation_latency() { return Element::operation_latency() + 2; }

  // One logical operation result is retained across all c output lanes. The
  // next retirement overwrites those same registers and there is no result
  // FIFO, so the implemented completed-result capacity is exactly one
  static constexpr int RESULT_CAPACITY = 1;

  // Tile clock and reset interface
  sc_in<bool> CCS_INIT_S1(wclk);
  sc_in<bool> CCS_INIT_S1(mclk);
  sc_in<bool> CCS_INIT_S1(rstn);

  // Write interface (SC): pass-through per tile input lane, B payload per element
  sc_in<bool> wen[TILE_INPUT_LANES];
  sc_in<ElementAddr> waddr[TILE_INPUT_LANES];
  sc_in<BSet> wset[TILE_INPUT_LANES];
  sc_in<ElementBInput> b[TILE_INPUT_LANES][TILE_OUTPUT_LANES];

  // MAC issue interface: one A section captured by the tile station
  sc_in<ElementAInput> a[TILE_INPUT_LANES];
  sc_in<BSet> CCS_INIT_S1(mset);
  sc_in<bool> CCS_INIT_S1(mac_issue);
  sc_out<bool> CCS_INIT_S1(mac_ready);

  // Result interface: registered reduced result plus a per-retirement toggle
  sc_out<COutput> c[TILE_OUTPUT_LANES];
  sc_out<bool> CCS_INIT_S1(c_retire);

 private:
  Element* elements[TILE_INPUT_LANES][TILE_OUTPUT_LANES];

  // Input station held while the elements consume one issued operand
  sc_signal<ElementAInput> station_a[TILE_INPUT_LANES];
  sc_signal<BSet> station_mset;
  sc_signal<bool> element_mac_issue;
  sc_signal<bool> window_idle_state;

  // Per-element outputs. element_c must be a plain sc_signal (not the
  // ZeroInitializedTileSignal subclass): Catapult only models sc_signal<T> as a
  // synthesizable channel, so reading a subclass in run_collect's reduction
  // aborts go compile (CIN-242). It is read only after the element has driven it
  // (guarded by tile_retired), so it needs no forced zero startup value.
  sc_signal<ElementCOutput> element_c[TILE_INPUT_LANES][TILE_OUTPUT_LANES];
  sc_signal<bool> element_c_retire[TILE_INPUT_LANES][TILE_OUTPUT_LANES];
  sc_signal<bool> element_mac_ready[TILE_INPUT_LANES][TILE_OUTPUT_LANES];

 public:
  // Construct the element grid and bind it to the tile ports and result wires
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
        elements[til][tol]->a(station_a[til]);
        elements[til][tol]->b(b[til][tol]);
        elements[til][tol]->c(element_c[til][tol]);
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
  // Capture one tile operand and issue it to the elements on the next cycle.
  // Clocked sc_signal writes become visible after the edge like RTL nonblocking
  // assignments, so this station is a real pipeline register, not a simulator delay
  void run_issue() {
    int window_remaining = 0;
    ElementAInput zero_a;
    clear_pack(zero_a);

    element_mac_issue.write(false);
    station_mset.write(0);
#pragma hls_unroll yes
    for (int til = 0; til < TILE_INPUT_LANES; til++) {
      station_a[til].write(zero_a);
    }
    window_idle_state.write(true);

    wait();

    while (true) {
      const bool ready_now = window_remaining == 0;
      if (window_remaining > 0) {
        window_remaining--;
      }

      if (mac_issue.read() && ready_now) {
#pragma hls_unroll yes
        for (int til = 0; til < TILE_INPUT_LANES; til++) {
          station_a[til].write(a[til].read());
        }
        station_mset.write(mset.read());
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
      COutput reset_output;
      clear_pack(reset_output);
      c[tol].write(reset_output);
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
          COutput reduced_output;
#pragma hls_unroll yes
          for (int b_col = 0; b_col < ELEMENT_B_COLS; b_col++) {
            // The unroll exposes every lane in parallel, but this recurrence does
            // not require a balanced adder tree; Catapult chooses the reduction topology
            CValue sum = 0;
#pragma hls_unroll yes
            for (int til = 0; til < TILE_INPUT_LANES; til++) {
              sum += widen_element_c(element_c[til][tol].read()[b_col]);
            }
            reduced_output[b_col] = sum;
          }
          c[tol].write(reduced_output);
        }
        seen_retire = !seen_retire;
        tile_retire_state = !tile_retire_state;
        c_retire.write(tile_retire_state);
      }
      wait();
    }
  }

  // Drive ready when the tile input station can accept an issue
  void drive_mac_ready() { mac_ready.write(rstn.read() && window_idle_state.read()); }
};
