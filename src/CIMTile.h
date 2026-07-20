// CIMTile owns a two-dimensional grid of CIM elements and exposes tile-shaped A/B/C data

#pragma once

#include <ac_int.h>
#include <systemc.h>

#include <sstream>

#include "AccelTypes.h"
#include "ArchitectureParams.h"
#include "CIMElement.h"

// CIMTile maps K/BK/N-shaped A/B/C data onto its CIMElement grid
//
// Relative to a bare element the tile adds two registered stages: the A station
// captures A/mset on an accepted issue and pulses the elements on the following
// cycle, then the C stage registers the reduction across the input-axis elements
// INPUT_AXIS_ELEMENTS and OUTPUT_AXIS_ELEMENTS describe the grid dimensions; no
// Connections protocol is exposed here. The input axis reduces into C while the
// output axis retains distinct B/C channels
template <int CH_IN, int CH_OUT, int B_SETS, int BASE_A_WIDTH, int BASE_B_WIDTH, int BASE_C_WIDTH, int WRITE_CH_IN,
          int MAC_LATENCY, int MODE, int A_WIDTH, int B_WIDTH, bool SIGNED, int INPUT_AXIS_ELEMENTS,
          int OUTPUT_AXIS_ELEMENTS>
SC_MODULE(CIMTile) {
 private:
  // Return the ceil log2 used for static port widths
  static constexpr int log2_ceil(int value) { return (value <= 1) ? 0 : 1 + log2_ceil((value + 1) / 2); }

  using Element = CIMElement<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH, BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY,
                             MODE, A_WIDTH, B_WIDTH, SIGNED>;

 public:
  static_assert(INPUT_AXIS_ELEMENTS > 0, "INPUT_AXIS_ELEMENTS must be positive");
  static_assert(OUTPUT_AXIS_ELEMENTS > 0, "OUTPUT_AXIS_ELEMENTS must be positive");

  static constexpr int ELEMENT_K = Element::K;
  static constexpr int ELEMENT_N = Element::N;
  static constexpr int ELEMENT_BK = Element::BK;
  static constexpr int ELEMENT_C_WIDTH = Element::C_WIDTH;
  static constexpr int K = INPUT_AXIS_ELEMENTS * ELEMENT_K;
  static constexpr int BK = ELEMENT_BK;
  // N is the visible B/C channel count across the tile's output-axis elements
  static constexpr int N = OUTPUT_AXIS_ELEMENTS * ELEMENT_N;
  static constexpr int BITS_B_SET = Element::BITS_SET;
  static constexpr int BITS_K = (K <= 1) ? 1 : log2_ceil(K);

  // One B write carries BK contiguous K positions for every N channel
  // CIMTile converts wchi into the selected input-axis element and its element-local K index

  // Reduction across the input-axis elements widens C by one guard field
  static constexpr int REDUCTION_GUARD_WIDTH = (INPUT_AXIS_ELEMENTS <= 1) ? 0 : log2_ceil(INPUT_AXIS_ELEMENTS);
  static constexpr int C_WIDTH = ELEMENT_C_WIDTH + REDUCTION_GUARD_WIDTH;

  using AValue = ac_int<A_WIDTH, false>;
  using BValue = ac_int<B_WIDTH, false>;
  using ElementCValue = ac_int<ELEMENT_C_WIDTH, false>;
  using CValue = ac_int<C_WIDTH, false>;
  using WSet = typename Element::WSet;
  using WChi = ac_int<BITS_K, false>;
  using ElementWChi = ac_int<Element::BITS_K, false>;
  using ElementAData = typename Element::AData;
  using ElementBData = typename Element::BData;
  using ElementCData = typename Element::CData;
  using AData = Pack1D<AValue, K>;
  using BData = Pack1D<Pack1D<BValue, N>, BK>;
  using CData = Pack1D<CValue, N>;

  // Return the number of mclk cycles an accepted issue keeps the tile not ready
  static constexpr int issue_window() { return Element::issue_window(); }

  // Return the number of mclk cycles from an accepted tile issue to retirement
  static constexpr int operation_latency() { return Element::operation_latency() + 2; }

  // One C register holds the last retired tile result; a later retirement overwrites it if it's not collected
  // The caller must ensure that no later issue occurs until the result is collected
  // An imporvement would be to add a small FIFO to hold multiple outstanding results
  static constexpr int RESULT_CAPACITY = 1;

  sc_in<bool> CCS_INIT_S1(wclk);
  sc_in<bool> CCS_INIT_S1(mclk);
  sc_in<bool> CCS_INIT_S1(rstn);

  // Tile write interface
  sc_in<bool> CCS_INIT_S1(write);
  sc_in<WSet> CCS_INIT_S1(wset);
  sc_in<WChi> CCS_INIT_S1(wchi);
  sc_in<BData> CCS_INIT_S1(b);

  // Tile MAC issue interface
  sc_in<AData> CCS_INIT_S1(a);
  sc_in<WSet> CCS_INIT_S1(mset);
  sc_in<bool> CCS_INIT_S1(mac_issue);
  sc_out<bool> CCS_INIT_S1(mac_ready);

  // Tile C interface
  sc_out<CData> CCS_INIT_S1(c);
  sc_out<bool> CCS_INIT_S1(c_retire);

 private:
  Element* elements[INPUT_AXIS_ELEMENTS][OUTPUT_AXIS_ELEMENTS];

  sc_signal<bool> element_write[INPUT_AXIS_ELEMENTS];
  sc_signal<ElementWChi> element_wchi[INPUT_AXIS_ELEMENTS];
  sc_signal<WSet> element_wset[INPUT_AXIS_ELEMENTS];
  sc_signal<ElementBData> element_b[INPUT_AXIS_ELEMENTS][OUTPUT_AXIS_ELEMENTS];

  // Combinationally slice the aggregate A payload before the registered station
  sc_signal<ElementAData> element_a_bus[INPUT_AXIS_ELEMENTS];

  // A station remains stable while the elements consume one issued operation
  sc_signal<ElementAData> station_a[INPUT_AXIS_ELEMENTS];
  sc_signal<WSet> station_mset;
  sc_signal<bool> element_mac_issue;
  sc_signal<bool> window_idle_state;

  sc_signal<ElementCData> element_c[INPUT_AXIS_ELEMENTS][OUTPUT_AXIS_ELEMENTS];
  sc_signal<bool> element_c_retire[INPUT_AXIS_ELEMENTS][OUTPUT_AXIS_ELEMENTS];
  sc_signal<bool> element_mac_ready[INPUT_AXIS_ELEMENTS][OUTPUT_AXIS_ELEMENTS];

 public:
  // Construct the element grid and bind tile-local signals to its ports
  SC_CTOR(CIMTile) {
    window_idle_state.write(true);

    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_ELEMENTS; input_axis_idx++) {
      for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_ELEMENTS; output_axis_idx++) {
        elements[input_axis_idx][output_axis_idx] = new Element(sc_gen_unique_name("element"));

        elements[input_axis_idx][output_axis_idx]->wclk(wclk);
        elements[input_axis_idx][output_axis_idx]->mclk(mclk);
        elements[input_axis_idx][output_axis_idx]->rstn(rstn);
        elements[input_axis_idx][output_axis_idx]->wen(element_write[input_axis_idx]);
        elements[input_axis_idx][output_axis_idx]->wchi(element_wchi[input_axis_idx]);
        elements[input_axis_idx][output_axis_idx]->wset(element_wset[input_axis_idx]);
        elements[input_axis_idx][output_axis_idx]->b(element_b[input_axis_idx][output_axis_idx]);
        elements[input_axis_idx][output_axis_idx]->a(station_a[input_axis_idx]);
        elements[input_axis_idx][output_axis_idx]->mset(station_mset);
        elements[input_axis_idx][output_axis_idx]->mac_issue(element_mac_issue);
        elements[input_axis_idx][output_axis_idx]->mac_ready(element_mac_ready[input_axis_idx][output_axis_idx]);
        elements[input_axis_idx][output_axis_idx]->c(element_c[input_axis_idx][output_axis_idx]);
        elements[input_axis_idx][output_axis_idx]->c_retire(element_c_retire[input_axis_idx][output_axis_idx]);
      }
    }

    SC_METHOD(drive_write);
    sensitive << rstn << write << wset << wchi << b;

    SC_METHOD(drive_a);
    sensitive << rstn << a;

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
  // Decode tile-local wchi to determine the selected input-axis element and its element-local input channel index
  void drive_write() {
    const bool reset_released = rstn.read();
    const bool write_enabled = reset_released && write.read();
    int base_k = 0;
    WSet selected_wset = 0;
    BData tile_b;
    clear_pack(tile_b);
    if (reset_released) {
      base_k = wchi.read().to_int();
      selected_wset = wset.read();
      tile_b = b.read();
    }
    ElementBData zero_b;
    clear_pack(zero_b);

#ifndef __SYNTHESIS__
    if (write_enabled && base_k >= K) {
      std::ostringstream message;
      message << "wchi " << base_k << " is outside K " << K;
      SC_REPORT_ERROR("CIMTile wchi out of range", message.str().c_str());
    }
#endif

#pragma hls_unroll yes
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_ELEMENTS; input_axis_idx++) {
      const int element_k_base = input_axis_idx * ELEMENT_K;
      const bool element_selected = base_k >= element_k_base && base_k < element_k_base + ELEMENT_K;
      const int element_local_k = base_k - element_k_base;
      const bool write_fits = element_local_k >= 0 && element_local_k + ELEMENT_BK <= ELEMENT_K;
      element_write[input_axis_idx].write(write_enabled && element_selected && write_fits);
      element_wchi[input_axis_idx].write(write_fits ? element_local_k : 0);
      element_wset[input_axis_idx].write(selected_wset);

#ifndef __SYNTHESIS__
      if (write_enabled && element_selected && !write_fits) {
        std::ostringstream message;
        message << "write at wchi " << base_k << " crosses an element boundary";
        SC_REPORT_ERROR("CIMTile wchi protocol violation", message.str().c_str());
      }
#endif

#pragma hls_unroll yes
      for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_ELEMENTS; output_axis_idx++) {
        ElementBData element_data = zero_b;
#pragma hls_unroll yes
        for (int element_bk = 0; element_bk < ELEMENT_BK; element_bk++) {
#pragma hls_unroll yes
          for (int element_n = 0; element_n < ELEMENT_N; element_n++) {
            element_data[element_bk][element_n] = tile_b[element_bk][output_axis_idx * ELEMENT_N + element_n];
          }
        }
        element_b[input_axis_idx][output_axis_idx].write(element_data);
      }
    }
  }

  // Slice the aggregate tile A payload into one payload per input-axis element
  void drive_a() {
    AData tile_a;
    clear_pack(tile_a);
    if (rstn.read()) {
      tile_a = a.read();
    }
#pragma hls_unroll yes
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_ELEMENTS; input_axis_idx++) {
      ElementAData element_a;
#pragma hls_unroll yes
      for (int element_k = 0; element_k < ELEMENT_K; element_k++) {
        element_a[element_k] = tile_a[input_axis_idx * ELEMENT_K + element_k];
      }
      element_a_bus[input_axis_idx].write(element_a);
    }
  }

  // Capture one tile A payload and issue it to every element on the next cycle
  // Clocked sc_signal writes become visible after the edge, so this station is a pipeline register
  void run_issue() {
    int window_remaining = 0;
    ElementAData zero_a;
    clear_pack(zero_a);

    element_mac_issue.write(false);
    station_mset.write(0);
#pragma hls_unroll yes
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_ELEMENTS; input_axis_idx++) {
      station_a[input_axis_idx].write(zero_a);
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
        for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_ELEMENTS; input_axis_idx++) {
          station_a[input_axis_idx].write(element_a_bus[input_axis_idx].read());
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

  // Extend one element C value to the tile accumulator width while preserving signedness
  static CValue widen_element_c(ElementCValue value) {
    CValue widened = 0;
    if constexpr (SIGNED) {
      ac_int<ELEMENT_C_WIDTH, true> signed_value;
      signed_value.set_slc(0, value);
      ac_int<C_WIDTH, true> signed_widened = signed_value;
      widened = signed_widened;
    } else {
      widened = value;
    }
    return widened;
  }

#ifndef __SYNTHESIS__
  // Verify that every element stays synchronized with representative element [0][0]
  void check_element_lockstep(bool representative_retire) const {
    const bool reference_ready = element_mac_ready[0][0].read();
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_ELEMENTS; input_axis_idx++) {
      for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_ELEMENTS; output_axis_idx++) {
        if (element_mac_ready[input_axis_idx][output_axis_idx].read() != reference_ready) {
          std::ostringstream message;
          message << "element [" << input_axis_idx << "][" << output_axis_idx << "] mac_ready diverged from [0][0]";
          SC_REPORT_ERROR("CIMTile element-ready lockstep violation", message.str().c_str());
        }
        if (element_c_retire[input_axis_idx][output_axis_idx].read() != representative_retire) {
          std::ostringstream message;
          message << "element [" << input_axis_idx << "][" << output_axis_idx << "] c_retire diverged from [0][0]";
          SC_REPORT_ERROR("CIMTile element-retire lockstep violation", message.str().c_str());
        }
      }
    }
  }
#endif

  // Register the tile C payload when representative element [0][0] retires
  void run_collect() {
    bool seen_retire = false;
    bool tile_retire_state = false;
    CData reset_c;
    clear_pack(reset_c);
    c.write(reset_c);
    c_retire.write(false);

    wait();

    while (true) {
      const bool representative_retire = element_c_retire[0][0].read();
#ifndef __SYNTHESIS__
      check_element_lockstep(representative_retire);
#endif

      if (representative_retire != seen_retire) {
        CData tile_c;
        // Keep the reduction inline so Catapult can statically enumerate every element_c signal
#pragma hls_unroll yes
        for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_ELEMENTS; output_axis_idx++) {
#pragma hls_unroll yes
          for (int element_n = 0; element_n < ELEMENT_N; element_n++) {
            CValue sum = 0;
#pragma hls_unroll yes
            for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_ELEMENTS; input_axis_idx++) {
              sum += widen_element_c(element_c[input_axis_idx][output_axis_idx].read()[element_n]);
            }
            tile_c[output_axis_idx * ELEMENT_N + element_n] = sum;
          }
        }
        c.write(tile_c);
        seen_retire = representative_retire;
        // Publish one new tile result generation
        tile_retire_state = !tile_retire_state;
        c_retire.write(tile_retire_state);
      }
      wait();
    }
  }

  // Drive ready when the tile A station can accept an operation
  void drive_mac_ready() { mac_ready.write(rstn.read() && window_idle_state.read()); }
};
