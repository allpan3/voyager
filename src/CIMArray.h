// CIMArray is a CIM-tile grid organized by input and output axes

#pragma once

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include <sstream>

#include "AccelTypes.h"
#include "ArchitectureParams.h"
#include "CIMTile.h"
#include "ConnectionsSignal.h"

static constexpr int CIM_C_BEAT_INPUT_MAJOR = 0;
static constexpr int CIM_C_BEAT_OUTPUT_MAJOR = 1;

// CIMArray routes tile-shaped A/B/C data across input and output axes
template <int CH_IN, int CH_OUT, int B_SETS, int BASE_A_WIDTH, int BASE_B_WIDTH,
          int BASE_C_WIDTH, int WRITE_CH_IN, int MAC_LATENCY, int MODE,
          // A_WIDTH/B_WIDTH/C_WIDTH are the operand and accumulator widths from
          // the datatype: A and B size the input and weight, C sizes the
          // reduced result the processor accumulates and stores. C_WIDTH comes
          // from the accumulation datatype (ACCUM_DATATYPE::width), the same
          // way the systolic array takes its accumulator type; the per-tile
          // BASE_C_WIDTH stays a separate free knob that only sizes one
          // vector-matrix mul
          int A_WIDTH, int B_WIDTH, int C_WIDTH, bool SIGNED,
          // Tile-internal layout; the input axis reduces into C while the
          // output axis retains distinct B/C channels
          int TILE_INPUT_AXIS_ELEMENTS, int TILE_OUTPUT_AXIS_ELEMENTS,
          // Input-axis tiles hold C partials that MACRequest::reduce optionally
          // combines
          int INPUT_AXIS_TILES,
          // Output-axis tiles hold distinct B/C channels that
          // MACRequest::multicast optionally shares A among
          int OUTPUT_AXIS_TILES,
          // --- Beat and port geometry
          // --------------------------------------------------------------- One
          // ready/valid transfer moves one beat, whose payload width is an
          // integer number of complete tiles A_PORT_TILES is the number of A
          // tiles per beat Each A tile carries one complete Tile::AData payload
          // of Tile::K scalars The current A beat spans the input axis; any
          // future narrower beats must be assembled before MAC issue
          int A_PORT_TILES,
          // B_PORT_TILES is the number of B tiles per beat
          // A direct request addresses one aligned, non-wrapping span along the
          // output axis, so B_PORT_TILES must be no wider than that axis and
          // must evenly divide it; data[k] maps to output_axis_tile_base + k A
          // replicate request copies data[0] across the output axis and leaves
          // the remaining payload tiles unused Routing a B beat wider than the
          // output axis is deferred
          int B_PORT_TILES,
          // C_PORT_TILES is the number of C tiles per beat
          int C_PORT_TILES,
          // C_BEAT_LAYOUT selects which tile axis advances first when one
          // operation requires multiple C beats Input-major advances input
          // indices first; output-major advances output indices first
          int C_BEAT_LAYOUT>
SC_MODULE(CIMArray) {
 private:
  // Return the ceil log2 used for static port widths
  static constexpr int log2_ceil(int value) {
    return (value <= 1) ? 0 : 1 + log2_ceil((value + 1) / 2);
  }

  // Return the ceiling division for static beat counts
  static constexpr int ceil_div(int dividend, int divisor) {
    return (dividend + divisor - 1) / divisor;
  }

  using Tile =
      CIMTile<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH, BASE_C_WIDTH,
              WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH, B_WIDTH, SIGNED,
              TILE_INPUT_AXIS_ELEMENTS, TILE_OUTPUT_AXIS_ELEMENTS>;

 public:
  static_assert(TILE_INPUT_AXIS_ELEMENTS > 0,
                "TILE_INPUT_AXIS_ELEMENTS must be positive");
  static_assert(TILE_OUTPUT_AXIS_ELEMENTS > 0,
                "TILE_OUTPUT_AXIS_ELEMENTS must be positive");
  static_assert(INPUT_AXIS_TILES > 0, "INPUT_AXIS_TILES must be positive");
  static_assert(OUTPUT_AXIS_TILES > 0, "OUTPUT_AXIS_TILES must be positive");
  static_assert(A_PORT_TILES > 0, "A_PORT_TILES must be positive");
  static_assert(B_PORT_TILES > 0, "B_PORT_TILES must be positive");
  static_assert(C_PORT_TILES > 0, "C_PORT_TILES must be positive");
  static_assert(
      A_PORT_TILES == INPUT_AXIS_TILES,
      "CIMArray currently requires one A beat to span the input axis");
  static_assert(
      B_PORT_TILES <= OUTPUT_AXIS_TILES,
      "CIMArray currently does not route one B beat beyond the output axis");
  static_assert(B_PORT_TILES == 0 || OUTPUT_AXIS_TILES % B_PORT_TILES == 0,
                "B_PORT_TILES must evenly divide OUTPUT_AXIS_TILES");
  static_assert(C_BEAT_LAYOUT == CIM_C_BEAT_INPUT_MAJOR ||
                    C_BEAT_LAYOUT == CIM_C_BEAT_OUTPUT_MAJOR,
                "C_BEAT_LAYOUT must be input-major or output-major");

  static constexpr int TILE_K = Tile::K;
  static constexpr int TILE_BK = Tile::BK;
  static constexpr int TILE_N = Tile::N;
  static constexpr int K = TILE_K * INPUT_AXIS_TILES;
  static constexpr int N = TILE_N * OUTPUT_AXIS_TILES;
  static constexpr int TILE_C_WIDTH = Tile::C_WIDTH;
  static constexpr int REDUCTION_GUARD_WIDTH =
      (INPUT_AXIS_TILES <= 1) ? 0 : log2_ceil(INPUT_AXIS_TILES);
  // C_WIDTH is the accumulation datatype width supplied by the caller; it must
  // hold one fully reduced result (all input tiles summed)
  static_assert(C_WIDTH >= TILE_C_WIDTH + REDUCTION_GUARD_WIDTH,
                "C_WIDTH must hold one reduced CIM array result");

  using AValue = typename Tile::AValue;
  using BValue = typename Tile::BValue;
  using TileCValue = typename Tile::CValue;
  using CValue = ac_int<C_WIDTH, false>;
  using Set = typename Tile::WSet;
  using TileWChi = typename Tile::WChi;
  using TileAData = typename Tile::AData;
  using TileBData = typename Tile::BData;
  using TileCData = typename Tile::CData;
  using CData = Pack1D<CValue, TILE_N>;

  // A beat, B beat, and C beat each match the corresponding port payload
  using ABeat = Pack1D<TileAData, A_PORT_TILES>;
  using BBeat = Pack1D<TileBData, B_PORT_TILES>;
  using CBeat = Pack1D<CData, C_PORT_TILES>;

  // A complete MAC result may exceed the C port; this index selects one
  // C_PORT_TILES-wide result transfer
  static constexpr int MAX_C_RESULT_BEATS =
      ceil_div(INPUT_AXIS_TILES * OUTPUT_AXIS_TILES, C_PORT_TILES);
  static constexpr int C_RESULT_BEAT_INDEX_WIDTH =
      (MAX_C_RESULT_BEATS <= 1) ? 1 : log2_ceil(MAX_C_RESULT_BEATS);
  using CResultBeatIndex = ac_int<C_RESULT_BEAT_INDEX_WIDTH, false>;

  // Reserve the fixed retire/capture loop plus an equal elastic window
  static constexpr int MAC_ISSUE_WINDOW = Tile::issue_window();
  static constexpr int RESULT_CREDIT_LATENCY = Tile::operation_latency() + 2;
  static constexpr int RESULT_QUEUE_DEPTH_PER_OUTPUT =
      ceil_div(RESULT_CREDIT_LATENCY, MAC_ISSUE_WINDOW);
  static constexpr int RESULT_QUEUE_TARGETED_DEPTH =
      OUTPUT_AXIS_TILES * RESULT_QUEUE_DEPTH_PER_OUTPUT;
  static constexpr int RESULT_QUEUE_PIPELINE_DEPTH =
      (RESULT_CREDIT_LATENCY < RESULT_QUEUE_TARGETED_DEPTH)
          ? RESULT_CREDIT_LATENCY
          : RESULT_QUEUE_TARGETED_DEPTH;
  static constexpr int RESULT_QUEUE_DEPTH = 2 * RESULT_QUEUE_PIPELINE_DEPTH;

  static constexpr int INPUT_AXIS_INDEX_WIDTH =
      (INPUT_AXIS_TILES <= 1) ? 1 : log2_ceil(INPUT_AXIS_TILES);
  static constexpr int OUTPUT_AXIS_INDEX_WIDTH =
      (OUTPUT_AXIS_TILES <= 1) ? 1 : log2_ceil(OUTPUT_AXIS_TILES);
  using InputAxisIndex = ac_int<INPUT_AXIS_INDEX_WIDTH, false>;
  using OutputAxisIndex = ac_int<OUTPUT_AXIS_INDEX_WIDTH, false>;

  // MACRequest carries one A beat plus tile-axis control
  struct MACRequest {
    Set mset;
    OutputAxisIndex output_axis_idx;  // selected tile when multicast is clear
    ac_int<1, false> multicast;
    ac_int<1, false> reduce;
    ABeat a;

    static const unsigned int width =
        Tile::BITS_B_SET + OUTPUT_AXIS_INDEX_WIDTH + 1 + 1 + ABeat::width;

    template <unsigned int Size>
    void Marshall(Marshaller<Size>& m) {
      m & mset;
      m & output_axis_idx;
      m & multicast;
      m & reduce;
      m & a;
    }

    inline friend void sc_trace(sc_trace_file* tf, const MACRequest& request,
                                const std::string& name) {
      sc_trace(tf, request.mset, name + ".mset");
      sc_trace(tf, request.output_axis_idx, name + ".output_axis_idx");
      sc_trace(tf, request.multicast, name + ".multicast");
      sc_trace(tf, request.reduce, name + ".reduce");
      sc_trace(tf, request.a, name + ".a");
    }

    inline friend std::ostream& operator<<(ostream& os,
                                           const MACRequest& request) {
      os << request.mset << " ";
      os << request.output_axis_idx << " ";
      os << request.multicast << " ";
      os << request.reduce << " ";
      os << request.a << " ";
      return os;
    }

    inline friend bool operator==(const MACRequest& lhs,
                                  const MACRequest& rhs) {
      return lhs.mset == rhs.mset &&
             lhs.output_axis_idx == rhs.output_axis_idx &&
             lhs.multicast == rhs.multicast && lhs.reduce == rhs.reduce &&
             lhs.a == rhs.a;
    }
  };

  // WriteRequest carries one B beat plus tile coordinates, wset, and tile-local
  // write channel
  struct WriteRequest {
    Set wset;
    InputAxisIndex input_axis_idx;
    OutputAxisIndex
        output_axis_tile_base;  // first tile in a direct B-port span
    TileWChi wchi;
    ac_int<1, false> replicate;
    BBeat data;

    static const unsigned int width =
        Tile::BITS_B_SET + INPUT_AXIS_INDEX_WIDTH + OUTPUT_AXIS_INDEX_WIDTH +
        Tile::BITS_K + 1 + BBeat::width;

    template <unsigned int Size>
    void Marshall(Marshaller<Size>& m) {
      m & wset;
      m & input_axis_idx;
      m & output_axis_tile_base;
      m & wchi;
      m & replicate;
      m & data;
    }

    inline friend void sc_trace(sc_trace_file* tf, const WriteRequest& request,
                                const std::string& name) {
      sc_trace(tf, request.wset, name + ".wset");
      sc_trace(tf, request.input_axis_idx, name + ".input_axis_idx");
      sc_trace(tf, request.output_axis_tile_base,
               name + ".output_axis_tile_base");
      sc_trace(tf, request.wchi, name + ".wchi");
      sc_trace(tf, request.replicate, name + ".replicate");
      sc_trace(tf, request.data, name + ".data");
    }

    inline friend std::ostream& operator<<(ostream& os,
                                           const WriteRequest& request) {
      os << request.wset << " ";
      os << request.input_axis_idx << " ";
      os << request.output_axis_tile_base << " ";
      os << request.wchi << " ";
      os << request.replicate << " ";
      os << request.data << " ";
      return os;
    }

    inline friend bool operator==(const WriteRequest& lhs,
                                  const WriteRequest& rhs) {
      return lhs.wset == rhs.wset && lhs.input_axis_idx == rhs.input_axis_idx &&
             lhs.output_axis_tile_base == rhs.output_axis_tile_base &&
             lhs.wchi == rhs.wchi && lhs.replicate == rhs.replicate &&
             lhs.data == rhs.data;
    }
  };

 private:
  Tile* tiles[INPUT_AXIS_TILES][OUTPUT_AXIS_TILES];

  sc_signal<TileAData> bus_a[INPUT_AXIS_TILES];
  sc_signal<Set> bus_mset;
  sc_signal<TileAData> held_a[INPUT_AXIS_TILES];
  sc_signal<Set> held_mset;
  sc_signal<bool> mac_issue[OUTPUT_AXIS_TILES];

  sc_signal<bool> tile_write[INPUT_AXIS_TILES][OUTPUT_AXIS_TILES];
  sc_signal<Set> tile_wset[INPUT_AXIS_TILES][OUTPUT_AXIS_TILES];
  sc_signal<TileWChi> tile_wchi[INPUT_AXIS_TILES][OUTPUT_AXIS_TILES];
  sc_signal<TileBData> tile_b[INPUT_AXIS_TILES][OUTPUT_AXIS_TILES];

  sc_signal<TileCData> tile_c[INPUT_AXIS_TILES][OUTPUT_AXIS_TILES];
  sc_signal<bool> tile_c_retire[INPUT_AXIS_TILES][OUTPUT_AXIS_TILES];
  sc_signal<bool> tile_mac_ready[INPUT_AXIS_TILES][OUTPUT_AXIS_TILES];

  // Each allocated completion slot carries request metadata and a bank per
  // physical tile
  using CompletionToken = ac_int<OUTPUT_AXIS_INDEX_WIDTH + 2, false>;
  static constexpr int COMPLETION_RESULT_WIDTH =
      TILE_C_WIDTH * TILE_N * INPUT_AXIS_TILES * OUTPUT_AXIS_TILES;
  using CompletionResult = ac_int<COMPLETION_RESULT_WIDTH, false>;
  static constexpr int RESULT_QUEUE_POINTER_WIDTH =
      (RESULT_QUEUE_DEPTH <= 1) ? 1 : log2_ceil(2 * RESULT_QUEUE_DEPTH);
  using ResultQueuePointer = ac_int<RESULT_QUEUE_POINTER_WIDTH, false>;

  sc_signal<CompletionToken> completion_tokens[RESULT_QUEUE_DEPTH];
  sc_signal<CompletionResult> completion_results[RESULT_QUEUE_DEPTH];
  sc_signal<ResultQueuePointer> completion_allocate_pointer;
  sc_signal<ResultQueuePointer> completion_capture_pointer;
  sc_signal<ResultQueuePointer> completion_release_pointer;
  sc_signal<bool> completion_release_pending;

#ifndef __SYNTHESIS__
  bool replicate_waste_warning_reported = false;
#endif

 public:
  sc_in<bool> CCS_INIT_S1(clk);
  sc_in<bool> CCS_INIT_S1(rstn);

  ConnectionsSignal::In<MACRequest> CCS_INIT_S1(mac_request_channel);
  ConnectionsSignal::In<WriteRequest> CCS_INIT_S1(write_request_channel);
  Connections::Out<CBeat> CCS_INIT_S1(result_channel);

  // Construct CIM tiles and HLS control threads
  SC_CTOR(CIMArray) {
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
         input_axis_idx++) {
      for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
           output_axis_idx++) {
        tiles[input_axis_idx][output_axis_idx] =
            new Tile(sc_gen_unique_name("tile"));

        tiles[input_axis_idx][output_axis_idx]->clk(clk);
        tiles[input_axis_idx][output_axis_idx]->rstn(rstn);
        tiles[input_axis_idx][output_axis_idx]->write(
            tile_write[input_axis_idx][output_axis_idx]);
        tiles[input_axis_idx][output_axis_idx]->wset(
            tile_wset[input_axis_idx][output_axis_idx]);
        tiles[input_axis_idx][output_axis_idx]->wchi(
            tile_wchi[input_axis_idx][output_axis_idx]);
        tiles[input_axis_idx][output_axis_idx]->b(
            tile_b[input_axis_idx][output_axis_idx]);
        tiles[input_axis_idx][output_axis_idx]->a(bus_a[input_axis_idx]);
        tiles[input_axis_idx][output_axis_idx]->mset(bus_mset);
        tiles[input_axis_idx][output_axis_idx]->mac_issue(
            mac_issue[output_axis_idx]);
        tiles[input_axis_idx][output_axis_idx]->mac_ready(
            tile_mac_ready[input_axis_idx][output_axis_idx]);
        tiles[input_axis_idx][output_axis_idx]->c(
            tile_c[input_axis_idx][output_axis_idx]);
        tiles[input_axis_idx][output_axis_idx]->c_retire(
            tile_c_retire[input_axis_idx][output_axis_idx]);
      }
    }

    SC_METHOD(drive_write_admission);
    sensitive << rstn << write_request_channel.vld << write_request_channel.dat;

    SC_METHOD(drive_mac_issue);
    sensitive << rstn << mac_request_channel.vld << mac_request_channel.dat;
    sensitive << held_mset;
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
         input_axis_idx++) {
      sensitive << held_a[input_axis_idx];
    }
    sensitive << completion_allocate_pointer << completion_release_pointer;
    sensitive << completion_release_pending << result_channel.vld
              << result_channel.rdy;
    for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
         output_axis_idx++) {
      for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
           input_axis_idx++) {
        sensitive << tile_mac_ready[input_axis_idx][output_axis_idx];
      }
    }

    SC_THREAD(record_mac_issue);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(capture_mac);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(drain_mac);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
  }

 private:
  // Clear tile-level B signals
  void reset_write_side() {
    TileBData zero_b;
    clear_pack(zero_b);
#pragma hls_unroll yes
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
         input_axis_idx++) {
#pragma hls_unroll yes
      for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
           output_axis_idx++) {
        tile_write[input_axis_idx][output_axis_idx].write(false);
        tile_wset[input_axis_idx][output_axis_idx].write(0);
        tile_wchi[input_axis_idx][output_axis_idx].write(0);
        tile_b[input_axis_idx][output_axis_idx].write(zero_b);
      }
    }
  }

  // Route one direct aligned B-port span or replicate B beat tile zero
  void drive_write_request(const WriteRequest& request, bool request_valid) {
    TileBData zero_b;
    clear_pack(zero_b);
#pragma hls_unroll yes
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
         input_axis_idx++) {
      const bool input_selected =
          request.input_axis_idx == InputAxisIndex(input_axis_idx);
#pragma hls_unroll yes
      for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
           output_axis_idx++) {
        const int beat_offset = (output_axis_idx / B_PORT_TILES) * B_PORT_TILES;
        const bool span_selected =
            request.output_axis_tile_base == OutputAxisIndex(beat_offset);
        const bool selected =
            input_selected && (request.replicate != 0 || span_selected);
        tile_write[input_axis_idx][output_axis_idx].write(request_valid &&
                                                          selected);
        tile_wset[input_axis_idx][output_axis_idx].write(request.wset);
        tile_wchi[input_axis_idx][output_axis_idx].write(request.wchi);
        tile_b[input_axis_idx][output_axis_idx].write(
            selected ? request.data[request.replicate != 0
                                        ? 0
                                        : output_axis_idx % B_PORT_TILES]
                     : zero_b);
      }
    }
  }

  // Admit the B stream combinationally so acceptance is the physical write edge
  void drive_write_admission() {
    WriteRequest request;
    request.wset = 0;
    request.input_axis_idx = 0;
    request.output_axis_tile_base = 0;
    request.wchi = 0;
    request.replicate = 0;
    clear_pack(request.data);

    if (!rstn.read()) {
      ConnectionsSignal::set_ready(write_request_channel, false);
      reset_write_side();
      return;
    }

    const bool request_valid = ConnectionsSignal::valid(write_request_channel);
    if (request_valid) {
      request = ConnectionsSignal::peek(write_request_channel);
    }
    ConnectionsSignal::set_ready(write_request_channel, true);

#ifndef __SYNTHESIS__
    if (request_valid && request.input_axis_idx.to_int() >= INPUT_AXIS_TILES) {
      SC_REPORT_FATAL("CIMArray",
                      "B request input_axis_idx is outside the input axis");
    }
    if (request_valid && request.replicate == 0 &&
        (request.output_axis_tile_base.to_int() % B_PORT_TILES != 0 ||
         request.output_axis_tile_base.to_int() + B_PORT_TILES >
             OUTPUT_AXIS_TILES)) {
      SC_REPORT_FATAL(
          "CIMArray",
          "direct B request must select one aligned, non-wrapping B-port span");
    }
    if (request_valid && request.replicate != 0 &&
        request.output_axis_tile_base != OutputAxisIndex(0)) {
      SC_REPORT_FATAL(
          "CIMArray",
          "replicate B request requires output_axis_tile_base zero");
    }
    if constexpr (B_PORT_TILES > 1) {
      if (request_valid && request.replicate != 0 &&
          !replicate_waste_warning_reported) {
        SC_REPORT_WARNING("CIMArray",
                          "replicate B request uses beat tile zero and ignores "
                          "the remaining tiles");
        replicate_waste_warning_reported = true;
      }
    }
#endif

    drive_write_request(request, request_valid);
  }

  // Return the exact-depth storage index selected by one completion queue
  // pointer
  static int result_queue_index(const ResultQueuePointer& pointer) {
    const int pointer_value = pointer.to_uint();
    return pointer_value < RESULT_QUEUE_DEPTH
               ? pointer_value
               : pointer_value - RESULT_QUEUE_DEPTH;
  }

  // Advance one exact-depth queue pointer through its index and phase range
  static ResultQueuePointer next_result_queue_pointer(
      const ResultQueuePointer& pointer) {
    return pointer == ResultQueuePointer(2 * RESULT_QUEUE_DEPTH - 1)
               ? ResultQueuePointer(0)
               : ResultQueuePointer(pointer + 1);
  }

  // Return whether every completion slot has been reserved by an accepted
  // request
  static bool result_queue_full(const ResultQueuePointer& allocate_pointer,
                                const ResultQueuePointer& release_pointer) {
    const int release_value = release_pointer.to_uint();
    const ResultQueuePointer full_pointer =
        release_value < RESULT_QUEUE_DEPTH
            ? ResultQueuePointer(release_value + RESULT_QUEUE_DEPTH)
            : ResultQueuePointer(release_value - RESULT_QUEUE_DEPTH);
    return allocate_pointer == full_pointer;
  }

#ifndef __SYNTHESIS__
  // Return whether one MAC request selects a valid output-axis tile
  static bool mac_target_valid(const MACRequest& request) {
    return request.multicast != 0 ||
           request.output_axis_idx.to_int() < OUTPUT_AXIS_TILES;
  }
#endif

  // Return whether one output-axis tile is selected by a MAC request
  static bool mac_output_axis_selected(const MACRequest& request,
                                       int output_axis_idx) {
    return request.multicast != 0 ||
           OutputAxisIndex(output_axis_idx) == request.output_axis_idx;
  }

  // Return whether the full queue releases its head on the current edge
  bool completion_releases_on_fire() const {
    return completion_release_pending.read() && result_channel.vld.read() &&
           result_channel.rdy.read();
  }

  // Return whether the completion queue and shared tile issue bus can accept an
  // operation
  bool request_path_ready() const {
    const bool queue_full = result_queue_full(
        completion_allocate_pointer.read(), completion_release_pointer.read());
    bool ready = !queue_full || completion_releases_on_fire();

#pragma hls_unroll yes
    for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
         output_axis_idx++) {
#pragma hls_unroll yes
      for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
           input_axis_idx++) {
        ready = ready && tile_mac_ready[input_axis_idx][output_axis_idx].read();
      }
    }
    return ready;
  }

  // Drive a newly accepted request, then hold its shared operands until every
  // tile closes the issue window
  void drive_mac_issue() {
    MACRequest request;
    request.mset = 0;
    request.output_axis_idx = 0;
    request.multicast = 0;
    request.reduce = 0;
    clear_pack(request.a);

    const bool valid =
        rstn.read() && ConnectionsSignal::valid(mac_request_channel);
    if (valid) {
      request = ConnectionsSignal::peek(mac_request_channel);
    }
#ifndef __SYNTHESIS__
    if (valid && !mac_target_valid(request)) {
      SC_REPORT_FATAL("CIMArray",
                      "MAC request output_axis_idx is outside the output axis");
    }
#endif
    const bool ready = rstn.read() && request_path_ready();
    const bool fire = valid && ready;
    ConnectionsSignal::set_ready(mac_request_channel, ready);

    bus_mset.write(fire ? request.mset : held_mset.read());
#pragma hls_unroll yes
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
         input_axis_idx++) {
      bus_a[input_axis_idx].write(fire ? request.a[input_axis_idx]
                                       : held_a[input_axis_idx].read());
    }
#pragma hls_unroll yes
    for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
         output_axis_idx++) {
      mac_issue[output_axis_idx].write(
          fire && mac_output_axis_selected(request, output_axis_idx));
    }
  }

  // Reserve one completion slot and record result metadata for each accepted
  // MAC
  void record_mac_issue() {
    ResultQueuePointer allocate_pointer = 0;
    completion_allocate_pointer.write(allocate_pointer);
    held_mset.write(0);
    TileAData zero_a;
    clear_pack(zero_a);
#pragma hls_unroll yes
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
         input_axis_idx++) {
      held_a[input_axis_idx].write(zero_a);
    }
#pragma hls_unroll yes
    for (int queue_idx = 0; queue_idx < RESULT_QUEUE_DEPTH; queue_idx++) {
      completion_tokens[queue_idx].write(CompletionToken(0));
    }

    wait();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode bubble
    while (true) {
      if (ConnectionsSignal::fired(mac_request_channel)) {
        const MACRequest request = ConnectionsSignal::peek(mac_request_channel);
        held_mset.write(request.mset);
#pragma hls_unroll yes
        for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
             input_axis_idx++) {
          held_a[input_axis_idx].write(request.a[input_axis_idx]);
        }

#ifndef __SYNTHESIS__
        if (result_queue_full(allocate_pointer,
                              completion_release_pointer.read()) &&
            !completion_releases_on_fire()) {
          SC_REPORT_FATAL("CIMArray",
                          "MAC issue overflowed the completion queue despite "
                          "unavailable storage");
        }
#endif

        CompletionToken token = 0;
        token.set_slc(0, request.output_axis_idx);
        token.set_slc(OUTPUT_AXIS_INDEX_WIDTH, request.multicast);
        token.set_slc(OUTPUT_AXIS_INDEX_WIDTH + 1, request.reduce);
        completion_tokens[result_queue_index(allocate_pointer)].write(token);
        allocate_pointer = next_result_queue_pointer(allocate_pointer);
        completion_allocate_pointer.write(allocate_pointer);
      }
      wait();
    }
  }

  // Return whether every tile selected by one operation has retired
  bool request_retired(bool multicast, int target_output_axis_idx,
                       const bool seen_retire[OUTPUT_AXIS_TILES]) const {
    bool retired = true;
#pragma hls_unroll yes
    for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
         output_axis_idx++) {
      if (multicast || output_axis_idx == target_output_axis_idx) {
#pragma hls_unroll yes
        for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
             input_axis_idx++) {
          retired = retired &&
                    tile_c_retire[input_axis_idx][output_axis_idx].read() !=
                        seen_retire[output_axis_idx];
        }
      }
    }
    return retired;
  }

  // Sign- or zero-extend one tile C value to the array C width
  static CValue widen_tile_value(TileCValue value) {
    ac_int<TILE_C_WIDTH, SIGNED> decoded;
    decoded.set_slc(0, value);
    const ac_int<C_WIDTH, SIGNED> widened = decoded;
    CValue result;
    result.set_slc(0, widened);
    return result;
  }

  // Return one scalar from a packed queued tile result
  static TileCValue queued_tile_value(const CompletionResult& completion_result,
                                      int input_axis_idx, int output_axis_idx,
                                      int tile_n) {
    const int bit_offset =
        ((input_axis_idx * OUTPUT_AXIS_TILES + output_axis_idx) * TILE_N +
         tile_n) *
        TILE_C_WIDTH;
    return completion_result.template slc<TILE_C_WIDTH>(bit_offset);
  }

  // Sum one queued C output channel across the input axis
  CValue reduce_queued_tile_values(const CompletionResult& completion_result,
                                   int output_axis_idx, int tile_n) const {
    ac_int<C_WIDTH, SIGNED> sum = 0;
#pragma hls_unroll yes
    for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
         input_axis_idx++) {
      ac_int<TILE_C_WIDTH, SIGNED> value;
      value.set_slc(0, queued_tile_value(completion_result, input_axis_idx,
                                         output_axis_idx, tile_n));
      sum += value;
    }
    CValue result;
    result.set_slc(0, sum);
    return result;
  }

  // Pack one port-width C beat from a queued complete MAC result in layout
  // order
  CBeat pack_queued_output(int queue_idx, CResultBeatIndex result_beat_idx,
                           bool multicast, bool reduce,
                           int target_output_axis_idx) const {
    const CompletionResult completion_result =
        completion_results[queue_idx].read();
    CBeat c_beat;
    clear_pack(c_beat);

    const int selected_output_tiles = multicast ? OUTPUT_AXIS_TILES : 1;
    const int logical_results = reduce
                                    ? selected_output_tiles
                                    : selected_output_tiles * INPUT_AXIS_TILES;
#pragma hls_unroll yes
    for (int port_idx = 0; port_idx < C_PORT_TILES; port_idx++) {
      const int logical_idx =
          result_beat_idx.to_int() * C_PORT_TILES + port_idx;
      if (logical_idx < logical_results) {
        int output_axis_ordinal = 0;
        int input_axis_idx = 0;
        if (reduce) {
          output_axis_ordinal = logical_idx;
        } else if constexpr (C_BEAT_LAYOUT == CIM_C_BEAT_INPUT_MAJOR) {
          output_axis_ordinal = logical_idx / INPUT_AXIS_TILES;
          input_axis_idx = logical_idx % INPUT_AXIS_TILES;
        } else if (multicast) {
          input_axis_idx = logical_idx / OUTPUT_AXIS_TILES;
          output_axis_ordinal = logical_idx % OUTPUT_AXIS_TILES;
        } else {
          input_axis_idx = logical_idx;
        }
        const int output_axis_idx =
            multicast ? output_axis_ordinal : target_output_axis_idx;

#pragma hls_unroll yes
        for (int tile_n = 0; tile_n < TILE_N; tile_n++) {
          if (reduce) {
            c_beat[port_idx][tile_n] = reduce_queued_tile_values(
                completion_result, output_axis_idx, tile_n);
          } else {
            c_beat[port_idx][tile_n] = widen_tile_value(queued_tile_value(
                completion_result, input_axis_idx, output_axis_idx, tile_n));
          }
        }
      }
    }
    return c_beat;
  }

  // Capture every fixed-latency retirement without waiting for result-channel
  // readiness
  void capture_mac() {
    bool seen_retire[OUTPUT_AXIS_TILES];
#pragma hls_unroll yes
    for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
         output_axis_idx++) {
      seen_retire[output_axis_idx] = false;
    }

    ResultQueuePointer capture_pointer = 0;
    completion_capture_pointer.write(capture_pointer);
#pragma hls_unroll yes
    for (int queue_idx = 0; queue_idx < RESULT_QUEUE_DEPTH; queue_idx++) {
      completion_results[queue_idx].write(CompletionResult(0));
    }

    wait();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode bubble
    while (true) {
      const ResultQueuePointer allocate_pointer =
          completion_allocate_pointer.read();
      const bool token_available = capture_pointer != allocate_pointer;

      if (token_available) {
        const int queue_idx = result_queue_index(capture_pointer);
        const CompletionToken token = completion_tokens[queue_idx].read();
        const bool multicast =
            token.template slc<1>(OUTPUT_AXIS_INDEX_WIDTH) != 0;
        const OutputAxisIndex token_output_axis_idx =
            token.template slc<OUTPUT_AXIS_INDEX_WIDTH>(0);
        const int target_output_axis_idx = token_output_axis_idx.to_int();

        if (request_retired(multicast, target_output_axis_idx, seen_retire)) {
          CompletionResult completion_result = 0;
#pragma hls_unroll yes
          for (int input_axis_idx = 0; input_axis_idx < INPUT_AXIS_TILES;
               input_axis_idx++) {
#pragma hls_unroll yes
            for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
                 output_axis_idx++) {
              if (multicast || output_axis_idx == target_output_axis_idx) {
                const TileCData tile_result =
                    tile_c[input_axis_idx][output_axis_idx].read();
#pragma hls_unroll yes
                for (int tile_n = 0; tile_n < TILE_N; tile_n++) {
                  const int bit_offset =
                      ((input_axis_idx * OUTPUT_AXIS_TILES + output_axis_idx) *
                           TILE_N +
                       tile_n) *
                      TILE_C_WIDTH;
                  completion_result.set_slc(bit_offset, tile_result[tile_n]);
                }
              }
            }
          }
          completion_results[queue_idx].write(completion_result);

#pragma hls_unroll yes
          for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
               output_axis_idx++) {
            if (multicast || output_axis_idx == target_output_axis_idx) {
              seen_retire[output_axis_idx] = !seen_retire[output_axis_idx];
            }
          }

          capture_pointer = next_result_queue_pointer(capture_pointer);
          completion_capture_pointer.write(capture_pointer);
        }
      }
      wait();
    }
  }

  // Serialize captured results and return a completion credit after the final
  // accepted beat
  void drain_mac() {
    result_channel.Reset();

    ResultQueuePointer release_pointer = 0;
    CResultBeatIndex result_beat_idx = 0;
    completion_release_pointer.write(release_pointer);
    completion_release_pending.write(false);

    wait();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode bubble
    while (true) {
      const ResultQueuePointer capture_pointer =
          completion_capture_pointer.read();
      const bool result_available = release_pointer != capture_pointer;

      if (result_available) {
        const int queue_idx = result_queue_index(release_pointer);
        const CompletionToken token = completion_tokens[queue_idx].read();
        const bool multicast =
            token.template slc<1>(OUTPUT_AXIS_INDEX_WIDTH) != 0;
        const bool reduce =
            token.template slc<1>(OUTPUT_AXIS_INDEX_WIDTH + 1) != 0;
        const OutputAxisIndex token_output_axis_idx =
            token.template slc<OUTPUT_AXIS_INDEX_WIDTH>(0);
        const int target_output_axis_idx = token_output_axis_idx.to_int();

        const int selected_output_tiles = multicast ? OUTPUT_AXIS_TILES : 1;
        const int logical_results =
            reduce ? selected_output_tiles
                   : selected_output_tiles * INPUT_AXIS_TILES;
        const int result_beats = ceil_div(logical_results, C_PORT_TILES);
        const bool token_complete =
            result_beat_idx == CResultBeatIndex(result_beats - 1);
        completion_release_pending.write(token_complete);

        result_channel.Push(pack_queued_output(queue_idx, result_beat_idx,
                                               multicast, reduce,
                                               target_output_axis_idx));
        completion_release_pending.write(false);

        if (token_complete) {
          release_pointer = next_result_queue_pointer(release_pointer);
          completion_release_pointer.write(release_pointer);
          result_beat_idx = 0;
        } else {
          result_beat_idx += CResultBeatIndex(1);
        }
      } else {
        completion_release_pending.write(false);
        wait();
      }
    }
  }
};
