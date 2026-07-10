// CIMArray is a grid of CIM tiles partitioned into multicast groups

#pragma once

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include "AccelTypes.h"
#include "ArchitectureParams.h"
#include "CIMElement.h"
#include "CIMTile.h"

// CIMArray reduces streamed A rows against the resident B operand. It is a
// REDUCTION_GROUPS x MULTICAST_GROUPS grid of CIMTiles, each containing
// TILE_INPUT_LANES x TILE_OUTPUT_LANES CIMElements and reducing its input lanes
// into one registered result row per tile output lane.
//
// Each multicast-group column owns an operand station per reduction-group tile,
// so different columns can compute against different A rows concurrently. A MAC
// request either targets one column or is broadcast to every column;
// MULTICAST_GROUPS=1 makes the two equivalent. C beats are per multicast group,
// emitted in issue order; a bcast emits MULTICAST_GROUPS beats in ascending
// group order.
//
// The store side dually addresses a single input lane per write (its control
// partition of the input axis); see StoreRequest. The array is the tile grid
// plus three control threads; all delivery-side protocol state lives here, and
// the tiles are timing-pure (VG issue, registered result, SC writes).
template <int CH_IN, int CH_OUT, int B_SETS, int BASE_A_WIDTH,
          int BASE_B_WIDTH, int BASE_C_WIDTH, int WRITE_CH_IN, int MAC_LATENCY,
          int MODE, int A_WIDTH, int B_WIDTH, bool SIGNED,
          int TILE_INPUT_LANES, int TILE_OUTPUT_LANES,
          // Input-axis summation partition: the input lanes are split into
          // REDUCTION_GROUPS independent reductions, so a multicast-group column
          // holds REDUCTION_GROUPS tiles, each a shorter-contraction vector-matrix
          // MAC with its own result. No datapath sums across a tile boundary
          int REDUCTION_GROUPS,
          // Output-axis partition: each multicast group is one tile column
          int MULTICAST_GROUPS,
          // --- Transfer ports (beat vs port) --------------------------------
          // A *beat* is the operand data a tile op consumes/commits atomically;
          // the array fires only once a beat is fully assembled. A *port* is the
          // per-transfer channel width, in tiles. The two are decoupled: a beat
          // assembles over ceil(beat / port) transfers. Each port is pinned to
          // "one beat per transfer" today, but that is the temporary limit.
          //
          // Future direction (not yet implemented): rename these to
          // A_PORT_BEATS / B_PORT_BEATS once a "beat" is the finer, macro-aligned
          // granule instead of a whole column/row. For B a beat becomes the macro
          // CH_OUT width times the number of macros in a tile; that finer granule
          // lets a transfer carry offset/stride-addressed beats -- e.g. writing
          // only the first CH_IN of each tile (partial/strided weight writes) --
          // which a full-column/row beat cannot express.
          //
          // A_PORT_TILES: reduction-group tiles of A per transfer. An A beat is a
          // full multicast-group column (REDUCTION_GROUPS tiles), so pinned to
          // REDUCTION_GROUPS (sustained-rate law: active tiles <= A_PORT_TILES x
          // II).
          int A_PORT_TILES,
          // B_PORT_TILES: multicast-group tiles of B per transfer. A B beat is a
          // full input-lane write-row across the output axis (MULTICAST_GROUPS
          // tiles), so pinned to MULTICAST_GROUPS. Counted in tiles, not lanes,
          // so the port width is decoupled from the lane geometry.
          //
          // Store-path layout note: weights sit in the buffer in natural
          // row-major order (rows consecutive, possibly several packed in one
          // SRAM word), but a macro needs *its own* rows contiguous. Prefer to
          // resolve this at load time -- have the weight DMA write the buffer in
          // macro-consumption order via address generation -- since weights are
          // resident and the repack amortizes over every reuse, keeping
          // steady-state reads contiguous with no read-side crossbar. The
          // decoupled port is the fallback: a per-tile assembly register, fed by
          // strided/gather addressing, collects the row-beat over several
          // B_PORT_TILES transfers (temporal repack) instead of a wide
          // single-cycle read crossbar. Weights load ahead of compute, so that
          // assembly latency hides.
          int B_PORT_TILES>
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

  // One tile is the intersection of one reduction and one multicast group
  using Tile = CIMTile<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH,
                       BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH,
                       B_WIDTH, SIGNED, TILE_INPUT_LANES, TILE_OUTPUT_LANES>;

 public:
  static_assert(TILE_INPUT_LANES > 0, "TILE_INPUT_LANES must be positive");
  static_assert(TILE_OUTPUT_LANES > 0, "TILE_OUTPUT_LANES must be positive");
  static_assert(REDUCTION_GROUPS > 0, "REDUCTION_GROUPS must be positive");
  static_assert(MULTICAST_GROUPS > 0, "MULTICAST_GROUPS must be positive");
  static_assert(A_PORT_TILES == REDUCTION_GROUPS,
                "CIMArray currently delivers one full column per A transfer");
  static_assert(B_PORT_TILES == MULTICAST_GROUPS,
                "CIMArray currently delivers one full write-row per B transfer");

  static constexpr int INPUT_LANES = TILE_INPUT_LANES * REDUCTION_GROUPS;
  static constexpr int OUTPUT_LANES = TILE_OUTPUT_LANES * MULTICAST_GROUPS;
  static constexpr int ELEMENTS = INPUT_LANES * OUTPUT_LANES;
  static constexpr int ELEMENT_A_COLS = Element::A_COLS;
  static constexpr int ELEMENT_B_COLS = Element::B_COLS;
  static constexpr int ELEMENT_B_WRITE_ROWS = Element::B_ROWS;
  static constexpr int A_COLS = ELEMENT_A_COLS * INPUT_LANES;
  static constexpr int C_COLS = ELEMENT_B_COLS * OUTPUT_LANES;
  static constexpr int ELEMENT_C_WIDTH = Element::C_WIDTH;
  // The tile owns the input-lane reduction and its result width
  static constexpr int C_WIDTH = Tile::C_WIDTH;
  static constexpr int ELEMENT_B_BEATS =
      ceil_div(ELEMENT_A_COLS, ELEMENT_B_WRITE_ROWS);

  // output lane:  0     1  |  2     3
  // group:        └─ 0 ─┘  |  └─ 1 ─┘   contiguous lanes
  // Output lanes covered by one multicast group
  static constexpr int MULTICAST_GROUP_LANES = TILE_OUTPUT_LANES;

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
  // One C beat carries a whole multicast-group column: the outer index is the
  // reduction group (input axis), the inner index the output lane within the
  // group. The R tiles' results ride side by side; no datapath sums across them
  using CColumn = Pack1D<COutput, MULTICAST_GROUP_LANES>;
  using CBeat = Pack1D<CColumn, REDUCTION_GROUPS>;

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
  // Tile grid: outer index reduction group (input axis), inner multicast group
  // (output axis). A column of REDUCTION_GROUPS tiles issues and retires together
  Tile* tiles[REDUCTION_GROUPS][MULTICAST_GROUPS];

  // Shared combinational A bus plus one admission pulse per multicast group.
  // The selected tile station captures the request on the channel handshake,
  // making that station the only registered copy of the A payload
  sc_signal<ElementAValue> bus_a[INPUT_LANES][ELEMENT_A_COLS];
  sc_signal<ElementSet> bus_mset;
  sc_signal<bool> col_start[MULTICAST_GROUPS];

  // Store-side control is shared per input lane; only the B payload differs per element
  sc_signal<bool> store_wen[INPUT_LANES];
  sc_signal<ElementAddr> store_waddr[INPUT_LANES];
  sc_signal<ElementSet> store_wset[INPUT_LANES];
  sc_signal<ElementBValue> element_b[INPUT_LANES][OUTPUT_LANES][ELEMENT_B_COLS][ELEMENT_B_WRITE_ROWS];

  // Per-tile registered results: each tile reduces its input lanes and drives
  // one result row per output lane plus a per-retirement toggle
  sc_signal<CValue> tile_c[REDUCTION_GROUPS][MULTICAST_GROUPS][MULTICAST_GROUP_LANES][ELEMENT_B_COLS];
  sc_signal<bool> tile_c_retire[REDUCTION_GROUPS][MULTICAST_GROUPS];
  sc_signal<bool> tile_mac_ready[REDUCTION_GROUPS][MULTICAST_GROUPS];

  // Per-group credits: issue keeps a local issued count and compares it with
  // the collected count published here, bounding uncollected results per group.
  // The counters wrap, so their modular difference is exact only while the
  // outstanding count stays representable: the width must satisfy
  // 2^CREDIT_COUNT_WIDTH > GROUP_CREDITS
  static constexpr int GROUP_CREDITS = 2;
  static constexpr int CREDIT_COUNT_WIDTH = log2_ceil(GROUP_CREDITS + 1);
  using CreditCount = ac_int<CREDIT_COUNT_WIDTH, false>;
  sc_signal<CreditCount> collected_count[MULTICAST_GROUPS];

  // InflightToken records {bcast, group} of one accepted request in issue order
  using InflightToken = ac_int<MULTICAST_GROUP_INDEX_WIDTH + 1, false>;

  // The narrow token queue preserves issue order across independently retiring groups
  static constexpr int INFLIGHT_QUEUE_DEPTH =
      1 << log2_ceil(GROUP_CREDITS * MULTICAST_GROUPS);
  static constexpr int INFLIGHT_QUEUE_INDEX_WIDTH =
      log2_ceil(INFLIGHT_QUEUE_DEPTH);
  using InflightQueuePointer =
      ac_int<INFLIGHT_QUEUE_INDEX_WIDTH + 1, false>;

  sc_signal<InflightToken> inflight_queue[INFLIGHT_QUEUE_DEPTH];
  sc_signal<InflightQueuePointer> inflight_write_pointer;
  sc_signal<InflightQueuePointer> inflight_read_pointer;

  // Registered per-group admission state; request decoding only selects it
  sc_signal<bool> group_issue_ready[MULTICAST_GROUPS];

 public:
  // Ports
  sc_in<bool> CCS_INIT_S1(clk);
  sc_in<bool> CCS_INIT_S1(rstn);

  Connections::In<MACRequest, Connections::SYN_PORT> CCS_INIT_S1(mac_channel);
  Connections::In<StoreRequest> CCS_INIT_S1(store_channel);
  Connections::Out<CBeat> CCS_INIT_S1(result_channel);

  // Construct CIM tiles and HLS control threads
  SC_CTOR(CIMArray) {
    for (int reduce_idx = 0; reduce_idx < REDUCTION_GROUPS; reduce_idx++) {
      for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
        // Instantiate the tile at this (reduction group, multicast group)
        tiles[reduce_idx][group_idx] = new Tile(sc_gen_unique_name("tile"));

        tiles[reduce_idx][group_idx]->wclk(clk);
        tiles[reduce_idx][group_idx]->mclk(clk);
        tiles[reduce_idx][group_idx]->rstn(rstn);
        // The whole column shares one issue pulse; a/mset come from the shared bus
        tiles[reduce_idx][group_idx]->mset(bus_mset);
        tiles[reduce_idx][group_idx]->mac_issue(col_start[group_idx]);
        tiles[reduce_idx][group_idx]->mac_ready(tile_mac_ready[reduce_idx][group_idx]);
        tiles[reduce_idx][group_idx]->c_retire(tile_c_retire[reduce_idx][group_idx]);

        // Write side and A delivery are indexed by the tile's input lanes; the
        // global input lane is reduce_idx * TILE_INPUT_LANES + tile input lane
        for (int til = 0; til < TILE_INPUT_LANES; til++) {
          const int input_lane_idx = reduce_idx * TILE_INPUT_LANES + til;
          tiles[reduce_idx][group_idx]->wen[til](store_wen[input_lane_idx]);
          tiles[reduce_idx][group_idx]->waddr[til](store_waddr[input_lane_idx]);
          tiles[reduce_idx][group_idx]->wset[til](store_wset[input_lane_idx]);
          for (int a_col_idx = 0; a_col_idx < ELEMENT_A_COLS; a_col_idx++) {
            tiles[reduce_idx][group_idx]->a[til][a_col_idx](
                bus_a[input_lane_idx][a_col_idx]);
          }
        }

        // Result and B payload are indexed by the tile's output lanes
        for (int group_lane = 0; group_lane < MULTICAST_GROUP_LANES; group_lane++) {
          const int output_lane_idx = group_idx * MULTICAST_GROUP_LANES + group_lane;
          for (int b_col_idx = 0; b_col_idx < ELEMENT_B_COLS; b_col_idx++) {
            tiles[reduce_idx][group_idx]->c[group_lane][b_col_idx](
                tile_c[reduce_idx][group_idx][group_lane][b_col_idx]);
            for (int til = 0; til < TILE_INPUT_LANES; til++) {
              const int input_lane_idx = reduce_idx * TILE_INPUT_LANES + til;
              for (int b_row_idx = 0; b_row_idx < ELEMENT_B_WRITE_ROWS; b_row_idx++) {
                tiles[reduce_idx][group_idx]->b[til][group_lane][b_col_idx][b_row_idx](
                    element_b[input_lane_idx][output_lane_idx][b_col_idx][b_row_idx]);
              }
            }
          }
        }
      }
    }

    SC_THREAD(store_b);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_METHOD(drive_admission);
    sensitive << rstn << mac_channel.vld << mac_channel.dat;
    sensitive << inflight_write_pointer << inflight_read_pointer;
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      sensitive << group_issue_ready[group_idx];
    }

    SC_THREAD(issue_mac);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(collect_mac);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
  }

 private:
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
  void drive_store_request(const StoreRequest& request, const bool request_valid) {
    const bool fanout_write = request.fanout != 0;
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      // A fanout write covers MULTICAST_GROUPS consecutive input lanes from the base lane
      const int section_idx = input_lane_idx - request.lane.to_int();
      const bool lane_active = request_valid && (fanout_write
          ? (section_idx >= 0 && section_idx < MULTICAST_GROUPS)
          : (request.lane == input_lane_idx));
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
      StoreRequest request;
      const bool request_valid = store_channel.PopNB(request, false);

#ifndef __SYNTHESIS__
      // A fanout write covers MULTICAST_GROUPS consecutive lanes from the base
      // (one beat section per covered lane), so the last covered lane is
      // lane + MULTICAST_GROUPS - 1; reject a base whose covered range would
      // run past the last input lane
      if (request_valid && request.fanout != 0 && request.lane.to_int() + MULTICAST_GROUPS > INPUT_LANES) {
        std::cerr << "Error: CIMArray fanout write from base lane "
                  << request.lane.to_int() << " exceeds INPUT_LANES" << std::endl;
      }
#endif

      drive_store_request(request, request_valid);
      wait();
    }
  }

  // Number of cycles a group cannot accept a new issue after one is launched.
  // Tracked by the array rather than read from tile mac_ready so the issue
  // decision has no combinational feedback race with the tile window
  static constexpr int GROUP_BUSY_CYCLES = Tile::issue_window();

  // Count down each group's issue window; call once per issue-thread cycle
  static void tick_busy_cycles(int busy_cycles[MULTICAST_GROUPS]) {
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      if (busy_cycles[group_idx] > 0) {
        busy_cycles[group_idx]--;
      }
    }
  }

  // Return the storage index selected by one inflight queue pointer
  static int inflight_queue_index(const InflightQueuePointer& pointer) {
    return pointer.template slc<INFLIGHT_QUEUE_INDEX_WIDTH>(0).to_int();
  }

  // Return the next inflight queue pointer, including its wrap bit
  static InflightQueuePointer advance_inflight_queue(
      const InflightQueuePointer& pointer) {
    return pointer + InflightQueuePointer(1);
  }

  // Return whether the inflight token queue has no free entry
  static bool inflight_queue_full(const InflightQueuePointer& write_pointer,
                                  const InflightQueuePointer& read_pointer) {
    return InflightQueuePointer(write_pointer - read_pointer) ==
           InflightQueuePointer(INFLIGHT_QUEUE_DEPTH);
  }

  // Decode the current input payload without completing its handshake
  MACRequest peek_mac_request() const {
    using MacPort = Connections::In<MACRequest, Connections::SYN_PORT>;
    using WrappedRequest = typename MacPort::WMessage;
    const typename MacPort::MsgBits bits = mac_channel.dat.read();
    Marshaller<WrappedRequest::width> marshaller(bits);
    WrappedRequest wrapped;
    wrapped.Marshall(marshaller);
    return wrapped.val;
  }

  // Drive input ready, A payload, and per-group issue pulses
  void drive_admission() {
    MACRequest request;
    request.set = 0;
    request.group = 0;
    request.bcast = 0;
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES;
         input_lane_idx++) {
#pragma hls_unroll yes
      for (int element_a_col = 0; element_a_col < ELEMENT_A_COLS;
           element_a_col++) {
        request.data[input_lane_idx][element_a_col] = 0;
      }
    }
    const bool request_valid = rstn.read() && mac_channel.vld.read();
    if (request_valid) {
      request = peek_mac_request();
    }

    const bool target_valid = request.bcast != 0 ||
        request.group.to_int() < MULTICAST_GROUPS;
    bool groups_ready = request_valid && target_valid;
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      const bool group_selected = request.bcast != 0 ||
          MulticastGroupIndex(group_idx) == request.group;
      if (group_selected) {
        groups_ready = groups_ready && group_issue_ready[group_idx].read();
      }
    }
    const bool inflight_full = inflight_queue_full(
        inflight_write_pointer.read(), inflight_read_pointer.read());
    const bool request_accepted = groups_ready && !inflight_full;
    mac_channel.rdy.write(request_accepted);

    bus_mset.write(request.set);
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES;
         input_lane_idx++) {
#pragma hls_unroll yes
      for (int element_a_col = 0; element_a_col < ELEMENT_A_COLS;
           element_a_col++) {
        bus_a[input_lane_idx][element_a_col].write(
            request.data[input_lane_idx][element_a_col]);
      }
    }

#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      const bool group_selected = request.bcast != 0 ||
          MulticastGroupIndex(group_idx) == request.group;
      col_start[group_idx].write(request_accepted && group_selected);
    }
  }

  // Issue MAC requests into reserved group stations and record completion order
  void issue_mac() {
    // Local issued counts pair with collected_count to form per-group credits
    CreditCount issued_count[MULTICAST_GROUPS];
    // Per-group issue-window countdown; a group is issuable when it reaches 0
    int busy_cycles[MULTICAST_GROUPS];
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      issued_count[group_idx] = 0;
      busy_cycles[group_idx] = 0;
      group_issue_ready[group_idx].write(true);
    }

    InflightQueuePointer inflight_pointer = 0;
    inflight_write_pointer.write(inflight_pointer);
#pragma hls_unroll yes
    for (int queue_idx = 0; queue_idx < INFLIGHT_QUEUE_DEPTH; queue_idx++) {
      inflight_queue[queue_idx].write(InflightToken(0));
    }

    wait();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode bubble
    while (true) {
      tick_busy_cycles(busy_cycles);

      const bool request_accepted =
          mac_channel.vld.read() && mac_channel.rdy.read();
      if (request_accepted) {
        const MACRequest request = peek_mac_request();
        const bool bcast = request.bcast != 0;

#ifndef __SYNTHESIS__
        if (!bcast && request.group.to_int() >= MULTICAST_GROUPS) {
          std::cerr << "Error: CIMArray MAC request targets multicast group "
                    << request.group.to_int() << " beyond MULTICAST_GROUPS" << std::endl;
        }
#endif

#pragma hls_unroll yes
        for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
          const bool group_selected =
              bcast || (MulticastGroupIndex(group_idx) == request.group);
          if (group_selected) {
            issued_count[group_idx] =
                issued_count[group_idx] + CreditCount(1);
            busy_cycles[group_idx] = GROUP_BUSY_CYCLES;
          }
        }

        InflightToken token = 0;
        token.set_slc(0, request.group);
        token.set_slc(MULTICAST_GROUP_INDEX_WIDTH, request.bcast);
        inflight_queue[inflight_queue_index(inflight_pointer)].write(token);
        inflight_pointer = advance_inflight_queue(inflight_pointer);
        inflight_write_pointer.write(inflight_pointer);
      }

#pragma hls_unroll yes
      for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
        const CreditCount outstanding =
            issued_count[group_idx] - collected_count[group_idx].read();
        group_issue_ready[group_idx].write(
            busy_cycles[group_idx] == 0 &&
            outstanding < CreditCount(GROUP_CREDITS));
      }

      wait();
    }
  }

  // Return whether every tile in one column has flipped its retire toggle past
  // the seen state. The column's R tiles issue together and retire together
  bool group_retired(int group_idx, const bool seen_retire[MULTICAST_GROUPS]) const {
    bool retired = true;
#pragma hls_unroll yes
    for (int reduce_idx = 0; reduce_idx < REDUCTION_GROUPS; reduce_idx++) {
      retired = retired && (tile_c_retire[reduce_idx][group_idx].read() != seen_retire[group_idx]);
    }
    return retired;
  }

  // Pack one column's registered results into a C beat: outer index is the
  // reduction group, inner the output lane. No reduction here -- each tile
  // already reduced across its own input lanes, and none sum across tiles
  CBeat pack_group_output(int group_idx) const {
    CBeat c_beat;
#pragma hls_unroll yes
    for (int reduce_idx = 0; reduce_idx < REDUCTION_GROUPS; reduce_idx++) {
#pragma hls_unroll yes
      for (int group_lane = 0; group_lane < MULTICAST_GROUP_LANES; group_lane++) {
#pragma hls_unroll yes
        for (int element_b_col = 0; element_b_col < ELEMENT_B_COLS; element_b_col++) {
          c_beat[reduce_idx][group_lane][element_b_col] =
              tile_c[reduce_idx][group_idx][group_lane][element_b_col].read();
        }
      }
    }
    return c_beat;
  }

  // Collect retirements in issue order and send each registered tile result
  void collect_mac() {
    result_channel.Reset();

    // seen_retire mirrors each group's last observed retire toggle; the
    // collected counts release issue credits after the result is accepted
    bool seen_retire[MULTICAST_GROUPS];
    CreditCount collected_local[MULTICAST_GROUPS];
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      seen_retire[group_idx] = false;
      collected_local[group_idx] = 0;
      collected_count[group_idx].write(0);
    }

    InflightQueuePointer inflight_pointer = 0;
    int broadcast_group = 0;
    inflight_read_pointer.write(inflight_pointer);

    wait();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode bubble
    while (true) {
      const InflightQueuePointer inflight_write =
          inflight_write_pointer.read();
      const bool token_available = inflight_pointer != inflight_write;

      if (token_available) {
        const InflightToken token =
            inflight_queue[inflight_queue_index(inflight_pointer)].read();
        const bool bcast =
            (token.template slc<1>(MULTICAST_GROUP_INDEX_WIDTH) != 0);
        const MulticastGroupIndex token_group =
            token.template slc<MULTICAST_GROUP_INDEX_WIDTH>(0);
        const int group_idx = bcast ? broadcast_group : token_group.to_int();

        if (group_retired(group_idx, seen_retire)) {
          result_channel.Push(pack_group_output(group_idx));

#pragma hls_unroll yes
          for (int update_idx = 0; update_idx < MULTICAST_GROUPS;
               update_idx++) {
            if (update_idx == group_idx) {
              seen_retire[update_idx] = !seen_retire[update_idx];
              collected_local[update_idx] =
                  collected_local[update_idx] + CreditCount(1);
              collected_count[update_idx].write(collected_local[update_idx]);
            }
          }

          const bool token_complete =
              !bcast || (broadcast_group == MULTICAST_GROUPS - 1);
          if (token_complete) {
            inflight_pointer = advance_inflight_queue(inflight_pointer);
            inflight_read_pointer.write(inflight_pointer);
            broadcast_group = 0;
          } else {
            broadcast_group++;
          }
        } else {
          wait();
        }
      } else {
        wait();
      }
    }
  }
};
