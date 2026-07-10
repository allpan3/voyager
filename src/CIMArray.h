// CIMArray is a grid of CIM tiles partitioned into multicast groups

#pragma once

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include "AccelTypes.h"
#include "ArchitectureParams.h"
#include "CIMElement.h"
#include "CIMTile.h"

static constexpr int CIM_C_PORT_REDUCTION_MAJOR = 0;
static constexpr int CIM_C_PORT_MULTICAST_MAJOR = 1;

// CIMArray reduces streamed A rows against the resident B operand. It is a
// REDUCTION_GROUPS x MULTICAST_GROUPS grid of CIMTiles, each containing
// TILE_INPUT_LANES x TILE_OUTPUT_LANES CIMElements and reducing its input lanes
// into one registered result row per tile output lane.
//
// Each tile owns its A operand station. The array drives one shared delivery bus
// and pulses the selected multicast-group column, so different columns can hold
// different A rows over multi-cycle issue windows. A MAC request either targets
// one column or is broadcast to every column;
// MULTICAST_GROUPS=1 makes the two equivalent. C beats are emitted in issue
// order and pack either reduction-group tiles or multicast-group tiles according
// to the compile-time C_PORT_ORIENTATION.
//
// The store side dually addresses a single input lane per write (its control
// partition of the input axis); see StoreRequest. The array is the tile grid
// plus three control threads; channel and ordering state lives here, while each
// tile is self-contained (registered issue station, registered result, SC writes).
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
          int B_PORT_TILES,
          // C_PORT_TILES: complete tile results per transfer. Reduction-major
          // packs one multicast-group column; multicast-major packs one
          // reduction-group row. Counted in tiles so width stays independent of
          // each tile's output-lane geometry
          int C_PORT_TILES,
          // Compile-time C beat layout: reduction-major favors independently
          // addressed groups; multicast-major favors broadcast output parallelism
          int C_PORT_ORIENTATION>
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
  static_assert(C_PORT_ORIENTATION == CIM_C_PORT_REDUCTION_MAJOR ||
                    C_PORT_ORIENTATION == CIM_C_PORT_MULTICAST_MAJOR,
                "C_PORT_ORIENTATION must be reduction-major or multicast-major");
  static_assert(C_PORT_TILES ==
                    ((C_PORT_ORIENTATION == CIM_C_PORT_REDUCTION_MAJOR)
                         ? REDUCTION_GROUPS
                         : MULTICAST_GROUPS),
                "C_PORT_TILES must span the selected C port orientation");

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
  // One tile result contains every output lane from one (reduction, multicast)
  // tile. CBeat's outer index follows the selected compile-time grid axis
  using TileResult = Pack1D<COutput, MULTICAST_GROUP_LANES>;
  using CColumn = TileResult;
  using CBeat = Pack1D<TileResult, C_PORT_TILES>;

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

  // MACRequest identifies the B set and multicast-group destination for one A beat
  struct MACRequest {
    ElementSet set;
    MulticastGroupIndex group;      // target multicast group; ignored when bcast is set
    ac_int<1, false> bcast;  // deliver this row to every multicast group and start them together

    static const unsigned int width = Element::BITS_SET + MULTICAST_GROUP_INDEX_WIDTH + 1;

    template <unsigned int Size>
    void Marshall(Marshaller<Size>& m) {
      m & set;
      m & group;
      m & bcast;
    }

    inline friend void sc_trace(sc_trace_file* tf, const MACRequest& request, const std::string& name) {
      sc_trace(tf, request.set, name + ".set");
      sc_trace(tf, request.group, name + ".group");
      sc_trace(tf, request.bcast, name + ".bcast");
    }

    inline friend std::ostream& operator<<(ostream& os, const MACRequest& request) {
      os << request.set << " ";
      os << request.group << " ";
      os << request.bcast << " ";
      return os;
    }

    inline friend bool operator==(const MACRequest& lhs, const MACRequest& rhs) {
      return lhs.set == rhs.set && lhs.group == rhs.group &&
             lhs.bcast == rhs.bcast;
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

  // Shared delivery bus; each selected tile captures it into its local station
  sc_signal<ElementAInput> bus_a[INPUT_LANES];
  sc_signal<ElementSet> bus_mset;
  sc_signal<bool> mac_start[MULTICAST_GROUPS];

  // Store-side control is shared per input lane; only the B payload differs per element
  sc_signal<bool> store_wen[INPUT_LANES];
  sc_signal<ElementAddr> store_waddr[INPUT_LANES];
  sc_signal<ElementSet> store_wset[INPUT_LANES];
  sc_signal<ElementBInput> element_b[INPUT_LANES][OUTPUT_LANES];

  // Bind each tile's local registered result and retirement toggle. These
  // sc_signals are array interconnect, not additional array-level storage
  sc_signal<COutput> tile_c[REDUCTION_GROUPS][MULTICAST_GROUPS][MULTICAST_GROUP_LANES];
  sc_signal<bool> tile_c_retire[REDUCTION_GROUPS][MULTICAST_GROUPS];
  sc_signal<bool> tile_mac_ready[REDUCTION_GROUPS][MULTICAST_GROUPS];

  // Per-group credits protect result storage, independently of tile mac_ready,
  // which only protects the input station and element issue window. The array
  // owns the credit because it alone observes result_channel acceptance; moving
  // it into every tile would require a consumed-result acknowledgement and
  // replicated group state across reduction tiles.
  //
  // Derive capacity from the tile's registered result storage. With no result
  // FIFO, a second uncollected retirement could overwrite the first, so the
  // current capacity is one. The counters become one-bit issue/collect toggles.
  // The counters wrap, so their modular difference is exact only while the
  // outstanding count stays representable: the width must satisfy
  // 2^CREDIT_COUNT_WIDTH > GROUP_RESULT_CAPACITY
  static constexpr int GROUP_RESULT_CAPACITY = Tile::RESULT_CAPACITY;
  static constexpr int CREDIT_COUNT_WIDTH = log2_ceil(GROUP_RESULT_CAPACITY + 1);
  using CreditCount = ac_int<CREDIT_COUNT_WIDTH, false>;
  sc_signal<CreditCount> collected_count[MULTICAST_GROUPS];

  // InflightToken records {bcast, mcast_group} of one accepted request in issue order
  using InflightToken = ac_int<MULTICAST_GROUP_INDEX_WIDTH + 1, false>;

  // Groups retire independently and CBeat carries only one group, so this narrow
  // token queue selects the next permitted group and preserves request order.
  // Required depth is the maximum logical outstanding-token count. Actual depth
  // rounds that count up to a power of two for a binary circular pointer and is
  // at least two so the storage index and pointer wrap bit both have valid widths
  static constexpr int INFLIGHT_QUEUE_REQUIRED_DEPTH = GROUP_RESULT_CAPACITY * MULTICAST_GROUPS;
  static constexpr int INFLIGHT_QUEUE_DEPTH = (INFLIGHT_QUEUE_REQUIRED_DEPTH <= 1) ?
      2 : 1 << log2_ceil(INFLIGHT_QUEUE_REQUIRED_DEPTH);
  static constexpr int INFLIGHT_QUEUE_INDEX_WIDTH = log2_ceil(INFLIGHT_QUEUE_DEPTH);
  using InflightQueuePointer = ac_int<INFLIGHT_QUEUE_INDEX_WIDTH + 1, false>;

  sc_signal<InflightToken> inflight_queue[INFLIGHT_QUEUE_DEPTH];
  sc_signal<InflightQueuePointer> inflight_write_pointer;
  sc_signal<InflightQueuePointer> inflight_read_pointer;

 public:
  // Ports
  sc_in<bool> CCS_INIT_S1(clk);
  sc_in<bool> CCS_INIT_S1(rstn);

  // Connections transfers a message atomically and provides no metadata peek.
  // Keeping metadata and A separate lets the array hold only the narrow request
  // while channel backpressure leaves the wide A beat upstream until the target
  // tiles and result storage are ready
  Connections::In<MACRequest> CCS_INIT_S1(mac_request_channel);
  Connections::In<ABeat> CCS_INIT_S1(a_channel);
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
        // The whole column shares one issue pulse and captures the delivery bus
        tiles[reduce_idx][group_idx]->mset(bus_mset);
        tiles[reduce_idx][group_idx]->mac_issue(mac_start[group_idx]);
        tiles[reduce_idx][group_idx]->mac_ready(tile_mac_ready[reduce_idx][group_idx]);
        tiles[reduce_idx][group_idx]->c_retire(tile_c_retire[reduce_idx][group_idx]);

        // Write side and A delivery are indexed by the tile's input lanes; the
        // global input lane is reduce_idx * TILE_INPUT_LANES + tile input lane
        for (int til = 0; til < TILE_INPUT_LANES; til++) {
          const int input_lane_idx = reduce_idx * TILE_INPUT_LANES + til;
          tiles[reduce_idx][group_idx]->wen[til](store_wen[input_lane_idx]);
          tiles[reduce_idx][group_idx]->waddr[til](store_waddr[input_lane_idx]);
          tiles[reduce_idx][group_idx]->wset[til](store_wset[input_lane_idx]);
          tiles[reduce_idx][group_idx]->a[til](bus_a[input_lane_idx]);
        }

        // Result and B payload are indexed by the tile's output lanes
        for (int group_lane = 0; group_lane < MULTICAST_GROUP_LANES; group_lane++) {
          const int output_lane_idx = group_idx * MULTICAST_GROUP_LANES + group_lane;
          tiles[reduce_idx][group_idx]->c[group_lane](
              tile_c[reduce_idx][group_idx][group_lane]);
          for (int til = 0; til < TILE_INPUT_LANES; til++) {
            const int input_lane_idx = reduce_idx * TILE_INPUT_LANES + til;
            tiles[reduce_idx][group_idx]->b[til][group_lane](
                element_b[input_lane_idx][output_lane_idx]);
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
  // Reset store-side signals driven by store_b
  void reset_store_side() {
    ElementBInput zero_b;
    clear_pack(zero_b);
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      store_wen[input_lane_idx].write(false);
      store_waddr[input_lane_idx].write(0);
      store_wset[input_lane_idx].write(0);
#pragma hls_unroll yes
      for (int output_lane_idx = 0; output_lane_idx < OUTPUT_LANES; output_lane_idx++) {
        element_b[input_lane_idx][output_lane_idx].write(zero_b);
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
          element_b[input_lane_idx][output_lane_idx].write(
              request.data[beat_lane_idx]);
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

  // Return the storage index selected by one inflight queue pointer
  static int inflight_queue_index(const InflightQueuePointer& pointer) {
    return pointer.template slc<INFLIGHT_QUEUE_INDEX_WIDTH>(0).to_int();
  }

  // Return whether the inflight token queue has no free entry
  static bool inflight_queue_full(const InflightQueuePointer& write_pointer,
                                  const InflightQueuePointer& read_pointer) {
    return InflightQueuePointer(write_pointer - read_pointer) == InflightQueuePointer(INFLIGHT_QUEUE_DEPTH);
  }

  // Return whether one request targets a valid multicast-group selection
  static bool mac_target_valid(const MACRequest& request) {
    // This is a small constant-width comparator and rejects unused index encodings
    return request.bcast != 0 || request.group.to_int() < MULTICAST_GROUPS;
  }

  // Return whether one multicast group is selected by a request
  static bool mac_group_selected(const MACRequest& request, int group_idx) {
    return request.bcast != 0 || MulticastGroupIndex(group_idx) == request.group;
  }

  // Return whether every selected tile can accept input and retain its result
  bool mac_request_ready(const MACRequest& request,
                         const CreditCount issued_count[MULTICAST_GROUPS]) const {
    // Init to ready
    bool ready = mac_target_valid(request);
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      if (mac_group_selected(request, group_idx)) {
        const CreditCount outstanding = issued_count[group_idx] - collected_count[group_idx].read();
        // Credit covers output retention; mac_ready separately covers input admission
        ready = ready && outstanding < CreditCount(GROUP_RESULT_CAPACITY);
#pragma hls_unroll yes
        for (int reduce_idx = 0; reduce_idx < REDUCTION_GROUPS; reduce_idx++) {
          ready = ready && tile_mac_ready[reduce_idx][group_idx].read();
        }
      }
    }
    return ready && !inflight_queue_full(
                        inflight_write_pointer.read(),
                        inflight_read_pointer.read());
  }

  // Drive one A beat onto the tile delivery bus
  void drive_mac_operand(const MACRequest& request, const ABeat& a_beat) {
    bus_mset.write(request.set);
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      bus_a[input_lane_idx].write(a_beat[input_lane_idx]);
    }
  }

  // Issue a pending mac request when its A beat and group tiles are ready
  void issue_mac() {
    mac_request_channel.Reset();
    a_channel.Reset();

    // Local issued counts pair with collected_count to form per-group credits
    CreditCount issued_count[MULTICAST_GROUPS];
    // Recursively clear every packed field that feeds resettable sc_signals
    ElementAInput zero_a;
    clear_pack(zero_a);
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      issued_count[group_idx] = 0;
    }
    bus_mset.write(0);
#pragma hls_unroll yes
    for (int input_lane_idx = 0; input_lane_idx < INPUT_LANES; input_lane_idx++) {
      bus_a[input_lane_idx].write(zero_a);
    }
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      mac_start[group_idx].write(false);
    }

    InflightQueuePointer inflight_pointer = 0;
    inflight_write_pointer.write(inflight_pointer);
#pragma hls_unroll yes
    for (int queue_idx = 0; queue_idx < INFLIGHT_QUEUE_DEPTH; queue_idx++) {
      inflight_queue[queue_idx].write(InflightToken(0));
    }

    wait();

    MACRequest pending_request;
    pending_request.set = 0;
    pending_request.group = 0;
    pending_request.bcast = 0;
    bool pending_valid = false;

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode bubble
    while (true) {
      // Every loop iteration represents exactly one controller cycle in native
      // SystemC and synthesis; PopNB itself is explicitly non-waiting
      ABeat a_beat;
      // Build exactly one-cycle mac_start pulses
      bool issue_this_cycle = false;

      // First retain only narrow metadata. Once its selected tiles and result
      // slots are available, pop the associated wide A beat and issue atomically
      if (!pending_valid) {
        MACRequest next_request;
        const bool next_valid = mac_request_channel.PopNB(next_request, false);

        if (next_valid) {
          pending_request = next_request;
          pending_valid = true;

#ifndef __SYNTHESIS__
          if (pending_request.bcast == 0 && pending_request.group.to_int() >= MULTICAST_GROUPS) {
            std::cerr << "Error: CIMArray MAC request targets multicast group "
                      << pending_request.group.to_int() << " beyond MULTICAST_GROUPS" << std::endl;
          }
#endif
        }
      } else if (mac_request_ready(pending_request, issued_count)) {
        const bool a_valid = a_channel.PopNB(a_beat, false);
        if (a_valid) {
          drive_mac_operand(pending_request, a_beat);
          issue_this_cycle = true;

#pragma hls_unroll yes
          for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
            if (mac_group_selected(pending_request, group_idx)) {
              issued_count[group_idx] = issued_count[group_idx] + CreditCount(1);
            }
          }

          InflightToken token = 0;
          // Low bits select the group; the top bit marks a broadcast token
          token.set_slc(0, pending_request.group);
          token.set_slc(MULTICAST_GROUP_INDEX_WIDTH, pending_request.bcast);
          inflight_queue[inflight_queue_index(inflight_pointer)].write(token);
          inflight_pointer += InflightQueuePointer(1);
          inflight_write_pointer.write(inflight_pointer);

          pending_valid = false;
        }
      }
#pragma hls_unroll yes
      for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
        mac_start[group_idx].write(issue_this_cycle && mac_group_selected(pending_request, group_idx));
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

  // Return whether every selected tile in one reduction row has retired
  bool reduction_row_retired(int reduce_idx, bool bcast, int target_group,
                             const bool seen_retire[MULTICAST_GROUPS]) const {
    bool retired = true;
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < MULTICAST_GROUPS; group_idx++) {
      if (bcast || group_idx == target_group) {
        retired = retired &&
                  (tile_c_retire[reduce_idx][group_idx].read() != seen_retire[group_idx]);
      }
    }
    return retired;
  }

  // Pack one column's registered results into a C beat: outer index is the
  // reduction group, inner the output lane. No reduction here -- each tile
  // already reduced across its own input lanes, and none sum across tiles
  CBeat pack_group_output(int group_idx) const {
    CBeat c_beat;
#pragma hls_unroll yes
    for (int reduce_idx = 0; reduce_idx < C_PORT_TILES; reduce_idx++) {
#pragma hls_unroll yes
      for (int group_lane = 0; group_lane < MULTICAST_GROUP_LANES; group_lane++) {
        c_beat[reduce_idx][group_lane] = tile_c[reduce_idx][group_idx][group_lane].read();
      }
    }
    return c_beat;
  }

  // Pack one reduction row across multicast groups. A targeted request fills
  // only its addressed lane and zeroes the rest; in-order context identifies it
  CBeat pack_reduction_output(int reduce_idx, bool bcast, int target_group) const {
    CBeat c_beat;
    clear_pack(c_beat);
#pragma hls_unroll yes
    for (int group_idx = 0; group_idx < C_PORT_TILES; group_idx++) {
      if (bcast || group_idx == target_group) {
#pragma hls_unroll yes
        for (int group_lane = 0; group_lane < MULTICAST_GROUP_LANES; group_lane++) {
          c_beat[group_idx][group_lane] =
              tile_c[reduce_idx][group_idx][group_lane].read();
        }
      }
    }
    return c_beat;
  }

  // Collect retirements in issue order using the selected compile-time C layout
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
    int output_reduction = 0;
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
        const int target_group = token_group.to_int();

        if constexpr (C_PORT_ORIENTATION == CIM_C_PORT_REDUCTION_MAJOR) {
          const int group_idx = bcast ? broadcast_group : target_group;
          if (group_retired(group_idx, seen_retire)) {
            result_channel.Push(pack_group_output(group_idx));

#pragma hls_unroll yes
            for (int update_idx = 0; update_idx < MULTICAST_GROUPS; update_idx++) {
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
              inflight_pointer += InflightQueuePointer(1);
              inflight_read_pointer.write(inflight_pointer);
              broadcast_group = 0;
            } else {
              broadcast_group++;
            }
          } else {
            wait();
          }
        } else {
          if (reduction_row_retired(output_reduction, bcast, target_group,
                                    seen_retire)) {
            result_channel.Push(
                pack_reduction_output(output_reduction, bcast, target_group));

            const bool token_complete = output_reduction == REDUCTION_GROUPS - 1;
            if (token_complete) {
#pragma hls_unroll yes
              for (int update_idx = 0; update_idx < MULTICAST_GROUPS; update_idx++) {
                if (bcast || update_idx == target_group) {
                  seen_retire[update_idx] = !seen_retire[update_idx];
                  collected_local[update_idx] =
                      collected_local[update_idx] + CreditCount(1);
                  collected_count[update_idx].write(collected_local[update_idx]);
                }
              }

              inflight_pointer += InflightQueuePointer(1);
              inflight_read_pointer.write(inflight_pointer);
              output_reduction = 0;
            } else {
              output_reduction++;
            }
          } else {
            wait();
          }
        }
      } else {
        wait();
      }
    }
  }
};
