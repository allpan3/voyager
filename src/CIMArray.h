// CIMArray is a sectored grid of CIM elements

#pragma once

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include "AccelTypes.h"
#include "ArchitectureParams.h"
#include "CIMElement.h"

// CIMDeliveryMode names the supported operand delivery policies
enum class CIMDeliveryMode {
  Multicast,
  RandomAccess,
};

// ZeroInitializedSignal gives ac_int-backed SystemC signals deterministic startup values
template <typename T>
class ZeroInitializedSignal : public sc_signal<T> {
 public:
  ZeroInitializedSignal()
      : sc_signal<T>(sc_gen_unique_name("zero_initialized_signal"), T(0)) {}
};

// Default CIMArray lane counts for local CIMElement composition
constexpr int CIM_DEFAULT_INPUT_LANES = 2;
constexpr int CIM_DEFAULT_OUTPUT_LANES = 4;

// CIMArray composes CIM elements into tiled A[K] * B[K][N] = C[N]
//
// Output lanes are statically partitioned into SECTORS sectors, each spanning
// the full input-lane depth. A sector owns one operand station, so different
// sectors can compute against different A rows concurrently (replicated and
// independent mappings). SECTORS=1 degenerates to the spanning-only array.
// A MAC request either targets one sector or spans every sector. C beats are
// per sector, emitted in issue order; a span emits SECTORS beats in ascending
// sector order.
template <int CH_IN = CIM_CH_IN, int CH_OUT = CIM_CH_OUT,
          int B_SETS = CIM_B_SETS, int BASE_A_WIDTH = CIM_BASE_A_WIDTH,
          int BASE_B_WIDTH = CIM_BASE_B_WIDTH,
          int BASE_C_WIDTH = CIM_BASE_C_WIDTH, int WRITE_CH_IN = CIM_WRITE_CH_IN,
          int MAC_LATENCY = CIM_MAC_LATENCY, int MODE = CIM_MODE,
          int A_WIDTH = INPUT_DTYPE_WIDTH, int B_WIDTH = WEIGHT_DTYPE_WIDTH,
          bool SIGNED = CIM_SIGNED, int INPUT_LANES = CIM_DEFAULT_INPUT_LANES,
          int OUTPUT_LANES = CIM_DEFAULT_OUTPUT_LANES, int SECTORS = 1,
          int A_PORTS = 1,
          CIMDeliveryMode B_DELIVERY = CIMDeliveryMode::RandomAccess>
SC_MODULE(CIMArray) {
 private:
  // Return the ceil log2 used for static port widths
  static constexpr int log2_ceil(int value) {
    return (value <= 1) ? 0 : 1 + log2_ceil((value + 1) / 2);
  }

  // Return the ceiling division for static transfer counts
  static constexpr int ceil_div(int dividend, int divisor) {
    return (dividend + divisor - 1) / divisor;
  }

  using Element = CIMElement<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH,
                             BASE_B_WIDTH, BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY,
                             MODE, A_WIDTH, B_WIDTH, SIGNED>;

 public:
  static_assert(INPUT_LANES > 0, "INPUT_LANES must be positive");
  static_assert(OUTPUT_LANES > 0, "OUTPUT_LANES must be positive");
  static_assert(SECTORS > 0 && SECTORS <= OUTPUT_LANES,
                "SECTORS must partition the output lanes");
  static_assert((OUTPUT_LANES % SECTORS) == 0,
                "OUTPUT_LANES must be divisible by SECTORS");
  static_assert(A_PORTS == 1,
                "CIMArray currently delivers one sector request per beat");
  static_assert(B_DELIVERY == CIMDeliveryMode::RandomAccess,
                "CIMArray currently supports random-access B delivery");

  // Per issue, each sector performs a vector-matrix multiplication over its resident columns
  static constexpr int ELEMENTS = INPUT_LANES * OUTPUT_LANES;
  static constexpr int ELEMENT_A_COLS = Element::A_COLS;
  static constexpr int ELEMENT_B_COLS = Element::B_COLS;
  static constexpr int ELEMENT_B_WRITE_ROWS = Element::B_ROWS;
  static constexpr int A_COLS = ELEMENT_A_COLS * INPUT_LANES;
  static constexpr int C_COLS = ELEMENT_B_COLS * OUTPUT_LANES;
  static constexpr int ELEMENT_C_WIDTH = Element::C_WIDTH;
  static constexpr int A_REDUCTION_GUARD_WIDTH =
      (INPUT_LANES <= 1) ? 0 : log2_ceil(INPUT_LANES);
  static constexpr int C_WIDTH = ELEMENT_C_WIDTH + A_REDUCTION_GUARD_WIDTH;
  static constexpr int ELEMENT_B_BEATS =
      ceil_div(ELEMENT_A_COLS, ELEMENT_B_WRITE_ROWS);

  // Output lanes covered by one sector
  static constexpr int SECTOR_OUTPUT_LANES = OUTPUT_LANES / SECTORS;

  // CIMElement-level scalar input/output types
  using ElementAValue = ac_int<A_WIDTH, false>;
  using ElementBValue = ac_int<B_WIDTH, false>;
  using ElementCValue = ac_int<ELEMENT_C_WIDTH, false>;
  using ElementSet = ac_int<Element::BITS_SET, false>;
  using ElementIndex = ac_int<Element::BITS_CH_IN, false>;

  // CIMElement-level input/output types
  using ElementAInput = Pack1D<ElementAValue, ELEMENT_A_COLS>;
  using ElementBInput = Pack1D<Pack1D<ElementBValue, ELEMENT_B_WRITE_ROWS>, ELEMENT_B_COLS>;
  using ElementCOutput = Pack1D<ElementCValue, ELEMENT_B_COLS>;

  // CIMArray-level input/output types
  using CValue = ac_int<C_WIDTH, false>;
  using COutput = Pack1D<CValue, ELEMENT_B_COLS>;
  using ABeat = Pack1D<ElementAInput, INPUT_LANES>;
  using BBeat = Pack1D<ElementBInput, OUTPUT_LANES>;
  // One C beat carries the reduced output columns of a single sector
  using CBeat = Pack1D<COutput, SECTOR_OUTPUT_LANES>;

  static constexpr int INPUT_LANE_INDEX_WIDTH = (INPUT_LANES <= 1) ? 1 : log2_ceil(INPUT_LANES);
  using InputLaneIndex = ac_int<INPUT_LANE_INDEX_WIDTH, false>;

  static constexpr int SECTOR_INDEX_WIDTH = (SECTORS <= 1) ? 1 : log2_ceil(SECTORS);
  using SectorIndex = ac_int<SECTOR_INDEX_WIDTH, false>;

  // MACRequest carries one A row, its B set, and its target sector
  struct MACRequest {
    ElementSet set;
    SectorIndex sector;       // target sector; ignored when span is set
    ac_int<1, false> span;  // deliver this row to every sector and start them together
    ABeat data;

    static const unsigned int width = Element::BITS_SET + SECTOR_INDEX_WIDTH + 1 + ABeat::width;

    template <unsigned int Size>
    void Marshall(Marshaller<Size>& m) {
      m & set;
      m & sector;
      m & span;
      m & data;
    }

    inline friend void sc_trace(sc_trace_file* tf, const MACRequest& request, const std::string& name) {
      sc_trace(tf, request.set, name + ".set");
      sc_trace(tf, request.sector, name + ".sector");
      sc_trace(tf, request.span, name + ".span");
      sc_trace(tf, request.data, name + ".data");
    }

    inline friend std::ostream& operator<<(ostream& os, const MACRequest& request) {
      os << request.set << " ";
      os << request.sector << " ";
      os << request.span << " ";
      os << request.data << " ";
      return os;
    }

    inline friend bool operator==(const MACRequest& lhs, const MACRequest& rhs) {
      return lhs.set == rhs.set && lhs.sector == rhs.sector &&
             lhs.span == rhs.span && lhs.data == rhs.data;
    }
  };

  // BWriteRequest carries one unit-level B beat and its explicit write target
  struct BWriteRequest {
    ElementSet set;
    InputLaneIndex input_lane;
    ElementIndex widx;
    BBeat data;

    static const unsigned int width = Element::BITS_SET + INPUT_LANE_INDEX_WIDTH + Element::BITS_CH_IN + BBeat::width;

    template <unsigned int Size>
    void Marshall(Marshaller<Size>& m) { m & set; m & input_lane; m & widx; m & data; }

    inline friend void sc_trace(sc_trace_file* tf, const BWriteRequest& request, const std::string& name) {
      sc_trace(tf, request.set, name + ".set");
      sc_trace(tf, request.input_lane, name + ".input_lane");
      sc_trace(tf, request.widx, name + ".widx");
      sc_trace(tf, request.data, name + ".data");
    }

    inline friend std::ostream& operator<<(ostream& os, const BWriteRequest& request) {
      os << request.set << " ";
      os << request.input_lane << " ";
      os << request.widx << " ";
      os << request.data << " ";
      return os;
    }

    inline friend bool operator==(const BWriteRequest& lhs, const BWriteRequest& rhs) {
      return lhs.set == rhs.set && lhs.input_lane == rhs.input_lane &&
             lhs.widx == rhs.widx && lhs.data == rhs.data;
    }
  };

 private:
  Element* elements[INPUT_LANES][OUTPUT_LANES];

  // Sector stations hold one stable A row per sector for the element consume window
  // Elements of the same sector share the station signals, so a shared row is stored once per sector
  ZeroInitializedSignal<ElementAValue> station_a[SECTORS][INPUT_LANES][ELEMENT_A_COLS];
  ZeroInitializedSignal<ElementSet> station_mset[SECTORS];
  sc_signal<bool> station_start[SECTORS];

  // Write-side control is shared per input lane; only the B payload differs per element
  sc_signal<bool> write_wen[INPUT_LANES];
  ZeroInitializedSignal<ElementIndex> write_widx[INPUT_LANES];
  ZeroInitializedSignal<ElementSet> write_wrow[INPUT_LANES];
  ZeroInitializedSignal<ElementBValue> element_b[INPUT_LANES][OUTPUT_LANES][ELEMENT_B_COLS][ELEMENT_B_WRITE_ROWS];

  // Per-element outputs
  ZeroInitializedSignal<ElementCValue> element_c[INPUT_LANES][OUTPUT_LANES][ELEMENT_B_COLS];
  sc_signal<bool> element_c_valid[INPUT_LANES][OUTPUT_LANES];
  sc_signal<bool> element_mac_busy[INPUT_LANES][OUTPUT_LANES];

  // done_token toggles once per collected sector result; issue compares it
  // against its local issue toggle so a station is reused only after sampling
  sc_signal<bool> done_token[SECTORS];

  // InflightToken records {span, sector} of one accepted request in issue order
  using InflightToken = ac_int<SECTOR_INDEX_WIDTH + 1, false>;
  Connections::Fifo<InflightToken, SECTORS + 1> CCS_INIT_S1(inflight_fifo);
  Connections::Combinational<InflightToken> CCS_INIT_S1(inflight_enq);
  Connections::Combinational<InflightToken> CCS_INIT_S1(inflight_deq);

 public:
  // Ports
  sc_in<bool> CCS_INIT_S1(clk);
  sc_in<bool> CCS_INIT_S1(rstn);

  Connections::In<MACRequest> CCS_INIT_S1(a_channel);
  Connections::In<BWriteRequest> CCS_INIT_S1(b_channel);
  Connections::Out<CBeat> CCS_INIT_S1(c_channel);

  // Construct CIMElement lanes and HLS control threads
  SC_CTOR(CIMArray) {
    inflight_fifo.clk(clk);
    inflight_fifo.rst(rstn);
    inflight_fifo.enq(inflight_enq);
    inflight_fifo.deq(inflight_deq);

    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      for (int output_lane_idx = 0; output_lane_idx < OUTPUT_LANES; output_lane_idx++) {
        // Sector this element belongs to
        const int sector_idx = output_lane_idx / SECTOR_OUTPUT_LANES;

        // Instantiate CIM element
        elements[input_lane_idx][output_lane_idx] = new Element(sc_gen_unique_name("element"));

        // Connect CIM element ports to CIMArray signals
        elements[input_lane_idx][output_lane_idx]->wclk(clk);
        elements[input_lane_idx][output_lane_idx]->mclk(clk);
        elements[input_lane_idx][output_lane_idx]->rstn(rstn);
        elements[input_lane_idx][output_lane_idx]->wen(write_wen[input_lane_idx]);
        elements[input_lane_idx][output_lane_idx]->widx(write_widx[input_lane_idx]);
        elements[input_lane_idx][output_lane_idx]->wset(write_wrow[input_lane_idx]);
        elements[input_lane_idx][output_lane_idx]->mac_start(station_start[sector_idx]);
        elements[input_lane_idx][output_lane_idx]->mset(station_mset[sector_idx]);
        elements[input_lane_idx][output_lane_idx]->c_valid(
            element_c_valid[input_lane_idx][output_lane_idx]);
        elements[input_lane_idx][output_lane_idx]->mac_busy(
            element_mac_busy[input_lane_idx][output_lane_idx]);

        for (int a_col_idx = 0; a_col_idx < ELEMENT_A_COLS; a_col_idx++) {
          elements[input_lane_idx][output_lane_idx]->a[a_col_idx](
              station_a[sector_idx][input_lane_idx][a_col_idx]);
        }

        for (int b_col_idx = 0; b_col_idx < ELEMENT_B_COLS; b_col_idx++) {
          elements[input_lane_idx][output_lane_idx]->c[b_col_idx](
              element_c[input_lane_idx][output_lane_idx][b_col_idx]);
          for (int b_row_idx = 0; b_row_idx < ELEMENT_B_WRITE_ROWS;
               b_row_idx++) {
            elements[input_lane_idx][output_lane_idx]->b[b_col_idx][b_row_idx](
                element_b[input_lane_idx][output_lane_idx][b_col_idx][b_row_idx]);
          }
        }
      }
    }

    SC_THREAD(write_b);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(issue_mac);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(collect_mac);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
  }

 private:
  // Clear write-enable pulses on every A lane
  void clear_write_controls() {
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      write_wen[input_lane_idx].write(false);
    }
  }

  // Reset write-side signals driven by write_b
  void reset_write_side() {
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      write_wen[input_lane_idx].write(false);
      write_widx[input_lane_idx].write(0);
      write_wrow[input_lane_idx].write(0);
#pragma hls_unroll yes
      for (int output_lane_idx = 0; output_lane_idx < OUTPUT_LANES; output_lane_idx++) {
#pragma hls_unroll yes
        for (int element_b_col = 0; element_b_col < ELEMENT_B_COLS;
             element_b_col++) {
#pragma hls_unroll yes
          for (int element_b_row = 0; element_b_row < ELEMENT_B_WRITE_ROWS;
               element_b_row++) {
            element_b[input_lane_idx][output_lane_idx][element_b_col][element_b_row]
                .write(0);
          }
        }
      }
    }
  }

  // Drive one B write request into the selected A lane and all B lanes
  void drive_b_write_request(const BWriteRequest& request) {
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      // Whether this is the target of the current B write request
      const bool target_a_lane = request.input_lane == input_lane_idx;
      write_wen[input_lane_idx].write(target_a_lane);
      if (target_a_lane) {
        write_widx[input_lane_idx].write(request.widx);
        write_wrow[input_lane_idx].write(request.set);
#pragma hls_unroll yes
        for (int output_lane_idx = 0; output_lane_idx < OUTPUT_LANES; output_lane_idx++) {
#pragma hls_unroll yes
          for (int element_b_col = 0; element_b_col < ELEMENT_B_COLS; element_b_col++) {
#pragma hls_unroll yes
            for (int element_b_row = 0; element_b_row < ELEMENT_B_WRITE_ROWS; element_b_row++) {
              const ElementBValue b_value = request.data[output_lane_idx][element_b_col][element_b_row];
              element_b[input_lane_idx][output_lane_idx][element_b_col][element_b_row].write(b_value);
            }
          }
        }
      }
    }
  }

  // Write streamed B requests into controller-selected CIM rows
  void write_b() {
    b_channel.Reset();
    reset_write_side();

    wait();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode bubble
    while (true) {
      // default/deassertion of the write pulse
      clear_write_controls();
      const BWriteRequest request = b_channel.Pop();
      drive_b_write_request(request);
      wait();
    }
  }

  // Clear MAC start pulses on every delivery station
  void clear_station_start() {
#pragma hls_unroll yes
    for (int sector_idx = 0; sector_idx < SECTORS; sector_idx++) {
      station_start[sector_idx].write(false);
    }
  }

  // Reset delivery-station signals driven by issue_mac
  void reset_stations() {
#pragma hls_unroll yes
    for (int sector_idx = 0; sector_idx < SECTORS; sector_idx++) {
      station_start[sector_idx].write(false);
      station_mset[sector_idx].write(0);
#pragma hls_unroll yes
      for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
#pragma hls_unroll yes
        for (int element_a_col = 0; element_a_col < ELEMENT_A_COLS;
             element_a_col++) {
          station_a[sector_idx][input_lane_idx][element_a_col].write(0);
        }
      }
    }
  }

  // Load one MAC request payload into the selected delivery station
  void load_station(int sector_idx, const MACRequest& request) {
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
#pragma hls_unroll yes
      for (int element_a_col = 0; element_a_col < ELEMENT_A_COLS; element_a_col++) {
        station_a[sector_idx][input_lane_idx][element_a_col].write(request.data[input_lane_idx][element_a_col]);
      }
    }
    station_mset[sector_idx].write(request.set);
    station_start[sector_idx].write(true);
  }

  // Return whether the selected station has no uncollected operation
  bool sector_free(const bool issue_token[SECTORS], const SectorIndex& sector) const {
    bool free_flag = false;
#pragma hls_unroll yes
    for (int sector_idx = 0; sector_idx < SECTORS; sector_idx++) {
      if (SectorIndex(sector_idx) == sector) {
        free_flag = issue_token[sector_idx] == done_token[sector_idx].read();
      }
    }
    return free_flag;
  }

  // Return whether every station has no uncollected operation
  bool all_sectors_free(const bool issue_token[SECTORS]) const {
    bool free_flag = true;
#pragma hls_unroll yes
    for (int sector_idx = 0; sector_idx < SECTORS; sector_idx++) {
      free_flag = free_flag && (issue_token[sector_idx] == done_token[sector_idx].read());
    }
    return free_flag;
  }

  // Issue MAC requests into free delivery stations and record completion order
  void issue_mac() {
    a_channel.Reset();
    inflight_enq.ResetWrite();
    reset_stations();

    // Local issue toggles mirror done_token; unequal means the sector is in flight
    bool issue_token[SECTORS];
#pragma hls_unroll yes
    for (int sector_idx = 0; sector_idx < SECTORS; sector_idx++) {
      issue_token[sector_idx] = false;
    }

    wait();

    while (true) {
      const MACRequest request = a_channel.Pop();
      const bool spanning = request.span != 0;

#ifndef __SYNTHESIS__
      if (!spanning && request.sector.to_int() >= SECTORS) {
        std::cerr << "Error: CIMArray MAC request targets sector "
                  << request.sector.to_int() << " beyond SECTORS" << std::endl;
      }
#endif

      // A station is reused only after collect_mac has sampled its result
      if (spanning) {
        while (!all_sectors_free(issue_token)) {
          wait();
        }
#pragma hls_unroll yes
        for (int sector_idx = 0; sector_idx < SECTORS; sector_idx++) {
          load_station(sector_idx, request);
          issue_token[sector_idx] = !issue_token[sector_idx];
        }
      } else {
        while (!sector_free(issue_token, request.sector)) {
          wait();
        }
#pragma hls_unroll yes
        for (int sector_idx = 0; sector_idx < SECTORS; sector_idx++) {
          if (SectorIndex(sector_idx) == request.sector) {
            load_station(sector_idx, request);
            issue_token[sector_idx] = !issue_token[sector_idx];
          }
        }
      }

      // Hold the start pulse for exactly one observed cycle; channel ops must
      // not be relied on for signal pulse timing
      wait();
      clear_station_start();

      InflightToken token = 0;
      token.set_slc(0, request.sector);
      token.set_slc(SECTOR_INDEX_WIDTH, request.span);
      inflight_enq.Push(token);
    }
  }

  // Return whether every element in one sector holds a completed result
  bool sector_all_valid(int sector_idx) const {
    bool all_valid = true;
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
#pragma hls_unroll yes
      for (int sector_lane = 0; sector_lane < SECTOR_OUTPUT_LANES; sector_lane++) {
        const int output_lane_idx = sector_idx * SECTOR_OUTPUT_LANES + sector_lane;
        all_valid = all_valid && element_c_valid[input_lane_idx][output_lane_idx].read();
      }
    }
    return all_valid;
  }

  // Widen one CIMElement output before unit-level reduction
  static CValue widen_element_c(ElementCValue value) {
    CValue widened = 0;
    if constexpr (SIGNED) {
      ac_int<ELEMENT_C_WIDTH, true> signed_value;
      // Copies the raw bits from the unsigned ElementCValue into the signed value
      // It preserves the two’s-complement bit pattern and lets signed_value interpret the top bit as sign.
      signed_value.set_slc(0, value);
      // Sign extension
      ac_int<C_WIDTH, true> signed_widened = signed_value;
      // Return as raw bits
      widened = signed_widened;
    } else {
      widened = value;
    }
    return widened;
  }

  // Reduce one B-lane output column across all A lanes
  CValue reduce_input_lanes(int output_lane_idx, int element_b_col) const {
    CValue sum = 0;
    // WARN: this is using a reduction tree for now
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      sum += widen_element_c(element_c[input_lane_idx][output_lane_idx][element_b_col].read());
    }
    return sum;
  }

  // Pack one sector's reduced output columns into a C beat
  CBeat pack_sector_output(int sector_idx) const {
    CBeat c_beat;
#pragma hls_unroll yes
    for (int sector_lane = 0; sector_lane < SECTOR_OUTPUT_LANES; sector_lane++) {
      const int output_lane_idx = sector_idx * SECTOR_OUTPUT_LANES + sector_lane;
#pragma hls_unroll yes
      for (int element_b_col = 0; element_b_col < ELEMENT_B_COLS; element_b_col++) {
        c_beat[sector_lane][element_b_col] = reduce_input_lanes(output_lane_idx, element_b_col);
      }
    }
    return c_beat;
  }

  // Collect completed sector results in issue order and emit per-sector C beats
  void collect_mac() {
    c_channel.Reset();
    inflight_deq.ResetRead();

    // Local done toggles mirror the done_token signals owned by this thread
    bool done_state[SECTORS];
#pragma hls_unroll yes
    for (int sector_idx = 0; sector_idx < SECTORS; sector_idx++) {
      done_state[sector_idx] = false;
      done_token[sector_idx].write(false);
    }

    wait();

    while (true) {
      const InflightToken token = inflight_deq.Pop();
      const bool spanning = (token.template slc<1>(SECTOR_INDEX_WIDTH) != 0);
      const SectorIndex token_sector = token.template slc<SECTOR_INDEX_WIDTH>(0);

      // One guard cycle so the freshly started op's cleared c_valid is visible
      wait();

      if (spanning) {
        for (int sector_idx = 0; sector_idx < SECTORS; sector_idx++) {
          while (!sector_all_valid(sector_idx)) {
            wait();
          }
          c_channel.Push(pack_sector_output(sector_idx));
          done_state[sector_idx] = !done_state[sector_idx];
          done_token[sector_idx].write(done_state[sector_idx]);
        }
      } else {
        const int sector_idx = token_sector.to_int();
        while (!sector_all_valid(sector_idx)) {
          wait();
        }
        c_channel.Push(pack_sector_output(sector_idx));
#pragma hls_unroll yes
        for (int flip_idx = 0; flip_idx < SECTORS; flip_idx++) {
          if (flip_idx == sector_idx) {
            done_state[flip_idx] = !done_state[flip_idx];
            done_token[flip_idx].write(done_state[flip_idx]);
          }
        }
      }
    }
  }
};
