// CIMArray is a grid of CIM elements partitioned into multicast groups

#pragma once

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include "AccelTypes.h"
#include "ArchitectureParams.h"
#include "CIMElement.h"

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

// CIMArray reduces streamed A rows against the resident B operand: every
// output lane holds a matrix operand tile in each input lane's CIMElement, 
// and CIMarray sums the per-CIMElement vector-matrix dot products across the 
// input lanes into one result row tile per output lane.
//
// Output lanes are statically partitioned into MULTICAST_GROUPS multicast
// groups, each covering the full input-lane depth. A multicast group owns one
// operand station, so different groups can compute against different A rows
// concurrently (replicated and independent mappings). A MAC request either
// targets one group or is broadcast to every group; MULTICAST_GROUPS=1 collapses
// the array to a single group, making the two equivalent. C beats are per
// multicast group, emitted in issue order; a bcast emits MULTICAST_GROUPS beats
// in ascending group order.
//
// The store side dually addresses a single input lane per write (its control
// partition of the input axis); see StoreRequest.
template <int CH_IN = CIM_CH_IN, int CH_OUT = CIM_CH_OUT,
          int B_SETS = CIM_B_SETS, int BASE_A_WIDTH = CIM_BASE_A_WIDTH,
          int BASE_B_WIDTH = CIM_BASE_B_WIDTH,
          int BASE_C_WIDTH = CIM_BASE_C_WIDTH, int WRITE_CH_IN = CIM_WRITE_CH_IN,
          int MAC_LATENCY = CIM_MAC_LATENCY, int MODE = CIM_MODE,
          int A_WIDTH = INPUT_DTYPE_WIDTH, int B_WIDTH = WEIGHT_DTYPE_WIDTH,
          bool SIGNED = CIM_SIGNED, int INPUT_LANES = CIM_DEFAULT_INPUT_LANES,
          int OUTPUT_LANES = CIM_DEFAULT_OUTPUT_LANES, int MULTICAST_GROUPS = 1,
          // Streamed-side transfer multiplicity: multicast-group rows delivered per
          // MAC transfer, set by the upstream A buffer bandwidth
          int A_PORTS = 1,
          // Resident-side transfer multiplicity: store beats delivered per
          // transfer to distinct input lanes, set by the upstream B buffer
          // bandwidth (WRITE_CH_IN is the macro's own write-port geometry and
          // is not a free knob)
          int B_PORTS = 1>
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
  static_assert(MULTICAST_GROUPS > 0 && MULTICAST_GROUPS <= OUTPUT_LANES,
                "MULTICAST_GROUPS must partition the output lanes");
  static_assert((OUTPUT_LANES % MULTICAST_GROUPS) == 0,
                "OUTPUT_LANES must be divisible by MULTICAST_GROUPS");
  static_assert(A_PORTS == 1,
                "CIMArray currently delivers one multicast-group request per beat");
  static_assert(B_PORTS == 1,
                "CIMArray currently accepts one store beat per transfer");

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

  // output lane:  0     1  |  2     3
  // group:        └─ 0 ─┘  |  └─ 1 ─┘   contiguous lanes
  // Output lanes covered by one multicast group
  static constexpr int MULTICAST_GROUP_LANES = OUTPUT_LANES / MULTICAST_GROUPS;

  // CIMElement-level scalar input/output types
  using ElementAValue = ac_int<A_WIDTH, false>;
  using ElementBValue = ac_int<B_WIDTH, false>;
  using ElementCValue = ac_int<ELEMENT_C_WIDTH, false>;
  using ElementSet = ac_int<Element::BITS_SET, false>;
  using ElementAddr = ac_int<Element::BITS_CH_IN, false>;

  // CIMElement-level input/output types
  using ElementAInput = Pack1D<ElementAValue, ELEMENT_A_COLS>;
  using ElementBInput = Pack1D<Pack1D<ElementBValue, ELEMENT_B_WRITE_ROWS>, ELEMENT_B_COLS>;
  using ElementCOutput = Pack1D<ElementCValue, ELEMENT_B_COLS>;

  // CIMArray-level input/output types
  using CValue = ac_int<C_WIDTH, false>;
  using COutput = Pack1D<CValue, ELEMENT_B_COLS>;
  using ABeat = Pack1D<ElementAInput, INPUT_LANES>;
  using BBeat = Pack1D<ElementBInput, OUTPUT_LANES>;
  // One C beat carries the reduced output columns of a single multicast group
  using CBeat = Pack1D<COutput, MULTICAST_GROUP_LANES>;

  // Multicast-group index (output axis): a MAC issue targets one of
  // MULTICAST_GROUPS multicast groups, so the width follows MULTICAST_GROUPS
  static constexpr int MULTICAST_GROUP_INDEX_WIDTH = (MULTICAST_GROUPS <= 1) ? 1 : log2_ceil(MULTICAST_GROUPS);
  using MulticastGroupIndex = ac_int<MULTICAST_GROUP_INDEX_WIDTH, false>;

  // Input-lane index (input axis): a store targets a single input lane, so the
  // width follows INPUT_LANES. Addressing is per lane because distinct input
  // lanes hold distinct reduction slices of B: replicating one payload across
  // lanes is fanout (no extra bandwidth), and bundling several distinct-data
  // lanes under one address would cost the same bandwidth while losing
  // independent per-lane addressing -- so there is no reason for a store to
  // span more than one lane. The transpose of the multicast group, whose width
  // instead follows MULTICAST_GROUPS
  static constexpr int INPUT_LANE_INDEX_WIDTH = (INPUT_LANES <= 1) ? 1 : log2_ceil(INPUT_LANES);
  using InputLaneIndex = ac_int<INPUT_LANE_INDEX_WIDTH, false>;

  // MACRequest carries one A row, its B set, and its target multicast group
  struct MACRequest {
    ElementSet set;
    MulticastGroupIndex group;      // target multicast group; ignored when bcast is set
    ac_int<1, false> bcast;  // deliver this row to every multicast group and start them together
    ABeat data;

    static const unsigned int width = Element::BITS_SET + MULTICAST_GROUP_INDEX_WIDTH + 1 + ABeat::width;

    template <unsigned int Size>
    void Marshall(Marshaller<Size>& m) {
      m & set;
      m & group;
      m & bcast;
      m & data;
    }

    inline friend void sc_trace(sc_trace_file* tf, const MACRequest& request, const std::string& name) {
      sc_trace(tf, request.set, name + ".set");
      sc_trace(tf, request.group, name + ".group");
      sc_trace(tf, request.bcast, name + ".bcast");
      sc_trace(tf, request.data, name + ".data");
    }

    inline friend std::ostream& operator<<(ostream& os, const MACRequest& request) {
      os << request.set << " ";
      os << request.group << " ";
      os << request.bcast << " ";
      os << request.data << " ";
      return os;
    }

    inline friend bool operator==(const MACRequest& lhs, const MACRequest& rhs) {
      return lhs.set == rhs.set && lhs.group == rhs.group &&
             lhs.bcast == rhs.bcast && lhs.data == rhs.data;
    }
  };

  // StoreRequest carries one B beat and its explicit write target
  //
  // A direct write (fanout = 0) targets the one input lane named by `lane` and
  // carries distinct data for every output lane. A fanout write (fanout = 1)
  // treats `lane` as a base and reinterprets the beat as MULTICAST_GROUPS
  // sections of MULTICAST_GROUP_LANES output-lane columns each: section r is
  // written to input lane base + r, fanned to the same output-lane positions in
  // every multicast group. The beat always carries only unique data;
  // replication is realized as wen decode and wire branching inside the array.
  //
  // Fanout example with base lane = 0, INPUT_LANES = 2, MULTICAST_GROUPS = 2,
  // OUTPUT_LANES = 4 (so MULTICAST_GROUP_LANES = 2, beat holds sections s0, s1
  // of two columns each):
  //
  //   output lanes :           0     1     2     3
  //   beat data    :         [ s0c0  s0c1  s1c0  s1c1 ]
  //                            \_section0/  \_section1/
  //
  //   output lanes           out0  out1 | out2  out3
  //   (input x output):  -- multicast group 0 | multicast group 1 --
  //   input lane 0:          s0c0  s0c1 | s0c0  s0c1   <- section 0, replicated
  //   input lane 1:          s1c0  s1c1 | s1c0  s1c1   <- section 1, replicated
  struct StoreRequest {
    ElementSet set;
    InputLaneIndex lane;  // target input lane; the base lane of a fanout write
    ElementAddr waddr;
    ac_int<1, false> fanout;    // replicate the packed sections across all multicast groups
    BBeat data;

    static const unsigned int width = Element::BITS_SET + INPUT_LANE_INDEX_WIDTH + Element::BITS_CH_IN + 1 + BBeat::width;

    template <unsigned int Size>
    void Marshall(Marshaller<Size>& m) { m & set; m & lane; m & waddr; m & fanout; m & data; }

    inline friend void sc_trace(sc_trace_file* tf, const StoreRequest& request, const std::string& name) {
      sc_trace(tf, request.set, name + ".set");
      sc_trace(tf, request.lane, name + ".lane");
      sc_trace(tf, request.waddr, name + ".waddr");
      sc_trace(tf, request.fanout, name + ".fanout");
      sc_trace(tf, request.data, name + ".data");
    }

    inline friend std::ostream& operator<<(ostream& os, const StoreRequest& request) {
      os << request.set << " ";
      os << request.lane << " ";
      os << request.waddr << " ";
      os << request.fanout << " ";
      os << request.data << " ";
      return os;
    }

    inline friend bool operator==(const StoreRequest& lhs, const StoreRequest& rhs) {
      return lhs.set == rhs.set && lhs.lane == rhs.lane &&
             lhs.waddr == rhs.waddr && lhs.fanout == rhs.fanout && lhs.data == rhs.data;
    }
  };

 private:
  Element* elements[INPUT_LANES][OUTPUT_LANES];

  // Multicast-group stations hold one stable A row per group for the element consume window
  // Elements of the same group share the station signals, so a shared row is stored once per group
  ZeroInitializedSignal<ElementAValue> station_a[MULTICAST_GROUPS][INPUT_LANES][ELEMENT_A_COLS];
  ZeroInitializedSignal<ElementSet> station_mset[MULTICAST_GROUPS];
  sc_signal<bool> station_start[MULTICAST_GROUPS];

  // Store-side control is shared per input lane; only the B payload differs per element
  sc_signal<bool> store_wen[INPUT_LANES];
  ZeroInitializedSignal<ElementAddr> store_waddr[INPUT_LANES];
  ZeroInitializedSignal<ElementSet> store_wset[INPUT_LANES];
  ZeroInitializedSignal<ElementBValue> element_b[INPUT_LANES][OUTPUT_LANES][ELEMENT_B_COLS][ELEMENT_B_WRITE_ROWS];

  // Per-element outputs
  ZeroInitializedSignal<ElementCValue> element_c[INPUT_LANES][OUTPUT_LANES][ELEMENT_B_COLS];
  sc_signal<bool> element_c_retire[INPUT_LANES][OUTPUT_LANES];
  sc_signal<bool> element_mac_ready[INPUT_LANES][OUTPUT_LANES];

  // Per-group credits: issue keeps a local issued count and compares it with
  // the collected count published here, bounding uncollected results per group.
  // The counters wrap, so their modular difference is exact only while the
  // outstanding count stays representable: the width must satisfy
  // 2^CREDIT_COUNT_WIDTH > GROUP_CREDITS
  static constexpr int GROUP_CREDITS = 2;
  static constexpr int CREDIT_COUNT_WIDTH = log2_ceil(GROUP_CREDITS + 1);
  using CreditCount = ac_int<CREDIT_COUNT_WIDTH, false>;
  ZeroInitializedSignal<CreditCount> collected_count[MULTICAST_GROUPS];

  // InflightToken records {bcast, group} of one accepted request in issue order
  using InflightToken = ac_int<MULTICAST_GROUP_INDEX_WIDTH + 1, false>;
  Connections::Fifo<InflightToken, GROUP_CREDITS * MULTICAST_GROUPS + 1> CCS_INIT_S1(inflight_fifo);
  Connections::Combinational<InflightToken> CCS_INIT_S1(inflight_enq);
  Connections::Combinational<InflightToken> CCS_INIT_S1(inflight_deq);

 public:
  // Ports
  sc_in<bool> CCS_INIT_S1(clk);
  sc_in<bool> CCS_INIT_S1(rstn);

  Connections::In<MACRequest> CCS_INIT_S1(mac_channel);
  Connections::In<StoreRequest> CCS_INIT_S1(store_channel);
  Connections::Out<CBeat> CCS_INIT_S1(result_channel);

  // Construct CIMElement lanes and HLS control threads
  SC_CTOR(CIMArray) {
    inflight_fifo.clk(clk);
    inflight_fifo.rst(rstn);
    // Connections::Fifo is a module with channel ports, not a callable queue;
    // the enq/deq Combinational channels bind those ports so issue_mac can
    // Push tokens and collect_mac can Pop them from separate threads
    inflight_fifo.enq(inflight_enq);
    inflight_fifo.deq(inflight_deq);

    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      for (int output_lane_idx = 0; output_lane_idx < OUTPUT_LANES; output_lane_idx++) {
        // Multicast group this element belongs to
        const int group_idx = output_lane_idx / MULTICAST_GROUP_LANES;

        // Instantiate CIM element
        elements[input_lane_idx][output_lane_idx] = new Element(sc_gen_unique_name("element"));

        // Connect CIM element ports to CIMArray signals
        elements[input_lane_idx][output_lane_idx]->wclk(clk);
        elements[input_lane_idx][output_lane_idx]->mclk(clk);
        elements[input_lane_idx][output_lane_idx]->rstn(rstn);
        elements[input_lane_idx][output_lane_idx]->wen(store_wen[input_lane_idx]);
        elements[input_lane_idx][output_lane_idx]->waddr(store_waddr[input_lane_idx]);
        elements[input_lane_idx][output_lane_idx]->wset(store_wset[input_lane_idx]);
        elements[input_lane_idx][output_lane_idx]->mac_issue(station_start[group_idx]);
        elements[input_lane_idx][output_lane_idx]->mset(station_mset[group_idx]);
        elements[input_lane_idx][output_lane_idx]->c_retire(
            element_c_retire[input_lane_idx][output_lane_idx]);
        elements[input_lane_idx][output_lane_idx]->mac_ready(
            element_mac_ready[input_lane_idx][output_lane_idx]);

        for (int a_col_idx = 0; a_col_idx < ELEMENT_A_COLS; a_col_idx++) {
          elements[input_lane_idx][output_lane_idx]->a[a_col_idx](
              station_a[group_idx][input_lane_idx][a_col_idx]);
        }

        for (int b_col_idx = 0; b_col_idx < ELEMENT_B_COLS; b_col_idx++) {
          elements[input_lane_idx][output_lane_idx]->c[b_col_idx](
              element_c[input_lane_idx][output_lane_idx][b_col_idx]);
          for (int b_row_idx = 0; b_row_idx < ELEMENT_B_WRITE_ROWS; b_row_idx++) {
            elements[input_lane_idx][output_lane_idx]->b[b_col_idx][b_row_idx](
                element_b[input_lane_idx][output_lane_idx][b_col_idx][b_row_idx]);
          }
        }
      }
    }

    SC_THREAD(store_b);
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
  // Clear the write-enable pulse on every input lane
  void clear_store_controls() {
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      store_wen[input_lane_idx].write(false);
    }
  }

  // Reset store-side signals driven by store_b
  void reset_store_side() {
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      store_wen[input_lane_idx].write(false);
      store_waddr[input_lane_idx].write(0);
      store_wset[input_lane_idx].write(0);
#pragma hls_unroll yes
      for (int output_lane_idx = 0; output_lane_idx < OUTPUT_LANES; output_lane_idx++) {
#pragma hls_unroll yes
        for (int element_b_col = 0; element_b_col < ELEMENT_B_COLS; element_b_col++) {
#pragma hls_unroll yes
          for (int element_b_row = 0; element_b_row < ELEMENT_B_WRITE_ROWS; element_b_row++) {
            element_b[input_lane_idx][output_lane_idx][element_b_col][element_b_row].write(0);
          }
        }
      }
    }
  }

  // Drive one store request into the covered input lanes and all output lanes
  void drive_store_request(const StoreRequest& request) {
#pragma hls_unroll yes
    const bool fanout_write = request.fanout != 0;
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      // A fanout write covers MULTICAST_GROUPS consecutive input lanes from the base lane
      const int section_idx = input_lane_idx - request.lane.to_int();
      const bool lane_active = fanout_write
          ? (section_idx >= 0 && section_idx < MULTICAST_GROUPS)
          : (request.lane == input_lane_idx);
      store_wen[input_lane_idx].write(lane_active);
      if (lane_active) {
        store_waddr[input_lane_idx].write(request.waddr);
        store_wset[input_lane_idx].write(request.set);
#pragma hls_unroll yes
        for (int output_lane_idx = 0; output_lane_idx < OUTPUT_LANES; output_lane_idx++) {
          // Fanout beats carry one section per covered input lane; every multicast
          // group receives the same column positions of that section
          const int beat_lane_idx = fanout_write
              ? section_idx * MULTICAST_GROUP_LANES + (output_lane_idx % MULTICAST_GROUP_LANES)
              : output_lane_idx;
#pragma hls_unroll yes
          for (int element_b_col = 0; element_b_col < ELEMENT_B_COLS; element_b_col++) {
#pragma hls_unroll yes
            for (int element_b_row = 0; element_b_row < ELEMENT_B_WRITE_ROWS; element_b_row++) {
              const ElementBValue b_value = request.data[beat_lane_idx][element_b_col][element_b_row];
              element_b[input_lane_idx][output_lane_idx][element_b_col][element_b_row].write(b_value);
            }
          }
        }
      }
    }
  }

  // Store requests: write B beat data into the addressed B set
  void store_b() {
    store_channel.Reset();
    reset_store_side();

    wait();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode bubble
    while (true) {
      // default/deassertion of the write pulse
      clear_store_controls();
      const StoreRequest request = store_channel.Pop();

#ifndef __SYNTHESIS__
      // QUES: fanout can only ever be 0 or 1 right? And I don't get the math here. What are we comparing here?
      if (request.fanout && request.lane.to_int() + MULTICAST_GROUPS > INPUT_LANES) {
        std::cerr << "Error: CIMArray fanout write from base lane "
                  << request.lane.to_int() << " exceeds INPUT_LANES" << std::endl;
      }
#endif

      drive_store_request(request);
      wait();
    }
  }

  // Clear MAC start pulses on every delivery station
  void clear_station_start() {
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      station_start[group_idx].write(false);
    }
  }

  // Reset delivery-station signals driven by issue_mac
  void reset_stations() {
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      station_start[group_idx].write(false);
      station_mset[group_idx].write(0);
#pragma hls_unroll yes
      for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
#pragma hls_unroll yes
        for (int element_a_col = 0; element_a_col < ELEMENT_A_COLS;
             element_a_col++) {
          station_a[group_idx][input_lane_idx][element_a_col].write(0);
        }
      }
    }
  }

  // Load one MAC request payload into the selected delivery station
  void load_station(int group_idx, const MACRequest& request) {
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
#pragma hls_unroll yes
      for (int element_a_col = 0; element_a_col < ELEMENT_A_COLS; element_a_col++) {
        station_a[group_idx][input_lane_idx][element_a_col].write(request.data[input_lane_idx][element_a_col]);
      }
    }
    station_mset[group_idx].write(request.set);
    station_start[group_idx].write(true);
  }

  // Number of cycles a group cannot accept a new issue after one is launched.
  // Tracked by the array rather than read from element mac_ready so the issue
  // decision has no combinational feedback race with the element window
  static constexpr int GROUP_BUSY_CYCLES = Element::issue_window();

  // Return whether one group can accept an issue: its window has elapsed and a
  // result credit is free. The credit reserves the retire slot before issuing
  bool group_can_issue(const CreditCount issued_count[MULTICAST_GROUPS], const int busy_cycles[MULTICAST_GROUPS], const MulticastGroupIndex& group) const {
    bool can_issue = false;
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      if (MulticastGroupIndex(group_idx) == group) {
        const CreditCount outstanding = issued_count[group_idx] - collected_count[group_idx].read();
        can_issue = (busy_cycles[group_idx] == 0) && (outstanding < CreditCount(GROUP_CREDITS));
      }
    }
    return can_issue;
  }

  // Return whether every group can accept an issue
  bool all_groups_can_issue(const CreditCount issued_count[MULTICAST_GROUPS], const int busy_cycles[MULTICAST_GROUPS]) const {
    bool can_issue = true;
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      const CreditCount outstanding = issued_count[group_idx] - collected_count[group_idx].read();
      can_issue = can_issue && (busy_cycles[group_idx] == 0) && (outstanding < CreditCount(GROUP_CREDITS));
    }
    return can_issue;
  }

  // Count down each group's issue window; call once per issue-thread cycle
  static void tick_busy_cycles(int busy_cycles[MULTICAST_GROUPS]) {
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      if (busy_cycles[group_idx] > 0) {
        busy_cycles[group_idx]--;
      }
    }
  }

  // Issue MAC requests into reserved group stations and record completion order
  void issue_mac() {
    mac_channel.Reset();
    inflight_enq.ResetWrite();
    reset_stations();

    // Local issued counts pair with collected_count to form per-group credits
    CreditCount issued_count[MULTICAST_GROUPS];
    // Per-group issue-window countdown; a group is issuable when it reaches 0
    int busy_cycles[MULTICAST_GROUPS];
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      issued_count[group_idx] = 0;
      busy_cycles[group_idx] = 0;
    }

    wait();

    while (true) {
      const MACRequest request = mac_channel.Pop();
      const bool bcast = request.bcast != 0;

#ifndef __SYNTHESIS__
      if (!bcast && request.group.to_int() >= MULTICAST_GROUPS) {
        std::cerr << "Error: CIMArray MAC request targets multicast group "
                  << request.group.to_int() << " beyond MULTICAST_GROUPS" << std::endl;
      }
#endif

      // Issue only with a reserved retire slot and an elapsed window, so the
      // element never sees an issue while not ready (no-drop by construction)
      if (bcast) {
        while (!all_groups_can_issue(issued_count, busy_cycles)) {
          wait();
          tick_busy_cycles(busy_cycles);
        }
#pragma hls_unroll yes
        for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
          load_station(group_idx, request);
          issued_count[group_idx] = issued_count[group_idx] + CreditCount(1);
          busy_cycles[group_idx] = GROUP_BUSY_CYCLES;
        }
      } else {
        while (!group_can_issue(issued_count, busy_cycles, request.group)) {
          wait();
          tick_busy_cycles(busy_cycles);
        }
#pragma hls_unroll yes
        for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
          if (MulticastGroupIndex(group_idx) == request.group) {
            load_station(group_idx, request);
            issued_count[group_idx] = issued_count[group_idx] + CreditCount(1);
            busy_cycles[group_idx] = GROUP_BUSY_CYCLES;
          }
        }
      }

      // Hold the issue pulse for exactly one observed cycle; channel ops must
      // not be relied on for signal pulse timing
      wait();
      tick_busy_cycles(busy_cycles);
      clear_station_start();

      InflightToken token = 0;
      token.set_slc(0, request.group);
      token.set_slc(MULTICAST_GROUP_INDEX_WIDTH, request.bcast);
      inflight_enq.Push(token);
    }
  }

  // Return whether every element in one group has flipped past the seen toggle
  bool group_retired(int group_idx, const bool seen_retire[MULTICAST_GROUPS]) const {
    bool retired = true;
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
#pragma hls_unroll yes
      for (int group_lane = 0; group_lane < MULTICAST_GROUP_LANES; group_lane++) {
        const int output_lane_idx = group_idx * MULTICAST_GROUP_LANES + group_lane;
        retired = retired && (element_c_retire[input_lane_idx][output_lane_idx].read() != seen_retire[group_idx]);
      }
    }
    return retired;
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

  // Pack one multicast group's reduced output columns into a C beat
  CBeat pack_group_output(int group_idx) const {
    CBeat c_beat;
#pragma hls_unroll yes
    for (int group_lane = 0; group_lane < MULTICAST_GROUP_LANES; group_lane++) {
      const int output_lane_idx = group_idx * MULTICAST_GROUP_LANES + group_lane;
#pragma hls_unroll yes
      for (int element_b_col = 0; element_b_col < ELEMENT_B_COLS; element_b_col++) {
        c_beat[group_lane][element_b_col] = reduce_input_lanes(output_lane_idx, element_b_col);
      }
    }
    return c_beat;
  }

  // Collect retirements in issue order and emit per-group C beats. Retire
  // toggles accumulate, so a blocked push can never lose a completion
  void collect_mac() {
    result_channel.Reset();
    inflight_deq.ResetRead();

    // seen_retire mirrors each group's last observed retire toggle; the
    // collected counts release issue credits after the result is drained
    bool seen_retire[MULTICAST_GROUPS];
    CreditCount collected_local[MULTICAST_GROUPS];
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      seen_retire[group_idx] = false;
      collected_local[group_idx] = 0;
      collected_count[group_idx].write(0);
    }

    wait();

    while (true) {
      const InflightToken token = inflight_deq.Pop();
      const bool bcast = (token.template slc<1>(MULTICAST_GROUP_INDEX_WIDTH) != 0);
      const MulticastGroupIndex token_group = token.template slc<MULTICAST_GROUP_INDEX_WIDTH>(0);

      if (bcast) {
        for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
          while (!group_retired(group_idx, seen_retire)) {
            wait();
          }
          result_channel.Push(pack_group_output(group_idx));
          seen_retire[group_idx] = !seen_retire[group_idx];
          collected_local[group_idx] = collected_local[group_idx] + CreditCount(1);
          collected_count[group_idx].write(collected_local[group_idx]);
        }
      } else {
        const int group_idx = token_group.to_int();
        while (!group_retired(group_idx, seen_retire)) {
          wait();
        }
        result_channel.Push(pack_group_output(group_idx));
#pragma hls_unroll yes
        for (int flip_idx = 0; flip_idx < MULTICAST_GROUPS; flip_idx++) {
          if (flip_idx == group_idx) {
            seen_retire[flip_idx] = !seen_retire[flip_idx];
            collected_local[flip_idx] = collected_local[flip_idx] + CreditCount(1);
            collected_count[flip_idx].write(collected_local[flip_idx]);
          }
        }
      }
    }
  }
};
