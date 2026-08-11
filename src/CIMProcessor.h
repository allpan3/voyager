#pragma once

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include <type_traits>

#include "AccelTypes.h"
#include "ArchitectureParams.h"
#include "CIMArray.h"
#include "Params.h"
#include "PerfMonitor.h"

// CIMProcessor directly drives a resident-weight CIMArray for the strict native
// mapping Its issue controller fuses MatrixProcessor's input and weight
// controllers: it loads a resident weight set at each schedule swap point, then
// issues the associated A beat. Its result controller unpacks array results and
// uses MatrixUnit's buffer for temporal sums rows and cols preserve the
// MatrixProcessor template positions; the CIM array interprets them as its
// resident B[K][N] shape MatrixUnit owns the storage, while buffer_size mirrors
// the MatrixProcessor backend signature
template <typename InputTypeTuple, typename WeightTypeTuple, typename Input,
          typename Weight, typename Psum, typename Buffer, typename Scale,
          int rows, int cols, int buffer_size, int CH_IN, int CH_OUT,
          int B_SETS, int BASE_A_WIDTH, int BASE_B_WIDTH, int BASE_C_WIDTH,
          int WRITE_CH_IN, int MAC_LATENCY, int MODE, bool SIGNED,
          int TILE_INPUT_AXIS_ELEMENTS, int TILE_OUTPUT_AXIS_ELEMENTS,
          int INPUT_AXIS_TILES, int OUTPUT_AXIS_TILES, int A_PORT_TILES,
          int B_PORT_TILES, int C_PORT_TILES, int C_BEAT_LAYOUT,
          int RESULT_SLOTS_PER_OUTPUT_LANE = INPUT_AXIS_TILES>
SC_MODULE(CIMProcessor) {
 public:
  static constexpr int K = rows;
  static constexpr int N = cols;

  // A/B/C operand widths follow the datatypes the same way MatrixProcessor
  // takes Input/Weight/Psum; the CIM array is width-parameterized, so derive
  // the widths from those types here rather than passing them in separately
  static constexpr int A_WIDTH = Input::width;
  static constexpr int B_WIDTH = Weight::width;
  static constexpr int C_WIDTH = Psum::width;

  using Array =
      CIMArray<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH, BASE_C_WIDTH,
               WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH, B_WIDTH, C_WIDTH,
               SIGNED, TILE_INPUT_AXIS_ELEMENTS, TILE_OUTPUT_AXIS_ELEMENTS,
               INPUT_AXIS_TILES, OUTPUT_AXIS_TILES, A_PORT_TILES, B_PORT_TILES,
               C_PORT_TILES, C_BEAT_LAYOUT, RESULT_SLOTS_PER_OUTPUT_LANE>;
  using ABeat = typename Array::ABeat;
  using CBeat = typename Array::CBeat;
  using Set = typename Array::Set;
  using MACRequest = typename Array::MACRequest;
  using WriteRequest = typename Array::WriteRequest;

  static constexpr int LOOP_WIDTH = 10;
  // Carry one ordered accumulator transaction between read issue and completion
  struct AccumulationMetadata {
    Pack1D<Psum, N> result;
    Pack1D<Buffer, N> initial;
    ac_int<1, false> first;
    ac_int<1, false> bank;

    static const unsigned int width =
        Pack1D<Psum, N>::width + Pack1D<Buffer, N>::width + 2;

    template <unsigned int Size>
    void Marshall(Marshaller<Size> &m) {
      m & result;
      m & initial;
      m & first;
      m & bank;
    }

    inline friend void sc_trace(sc_trace_file *tf,
                                const AccumulationMetadata &metadata,
                                const std::string &name) {
      sc_trace(tf, metadata.result, name + ".result");
      sc_trace(tf, metadata.initial, name + ".initial");
      sc_trace(tf, metadata.first, name + ".first");
      sc_trace(tf, metadata.bank, name + ".bank");
    }

    inline friend std::ostream &operator<<(
        std::ostream &os, const AccumulationMetadata &metadata) {
      os << metadata.result << " ";
      os << metadata.initial << " ";
      os << metadata.first << " ";
      os << metadata.bank;
      return os;
    }

    inline friend bool operator==(const AccumulationMetadata &lhs,
                                  const AccumulationMetadata &rhs) {
      return lhs.result == rhs.result && lhs.initial == rhs.initial &&
             lhs.first == rhs.first && lhs.bank == rhs.bank;
    }
  };

  // One array B-port write
  static constexpr int WEIGHT_WRITE_WIDTH = Array::BBeat::width;
  // One logical B row matches MatrixProcessor's weight-channel convention
  static constexpr int WEIGHT_ROW_WIDTH = N * B_WIDTH;
  // Narrowing the B port splits one row into more array writes
  static constexpr int WEIGHT_BEATS_PER_ROW = OUTPUT_AXIS_TILES / B_PORT_TILES;

  static constexpr int ACCUM_TO_WB_FIFO_DEPTH = SUPPORT_MX ? 8 : 1;
  static constexpr int OUTPUT_FIFO_DEPTH = 8;
  static constexpr int RESIDENT_SET_COUNT = B_SETS;
#if DOUBLE_BUFFERED_ACCUM_BUFFER
  static constexpr int ACCUM_BUFFER_BANKS = 2;
#else
  static constexpr int ACCUM_BUFFER_BANKS = 1;
#endif

  static_assert(std::is_same<Input, DataTypes::int8>::value,
                "CIMProcessor currently supports native INT8 input only");
  static_assert(std::is_same<Weight, DataTypes::int8>::value,
                "CIMProcessor currently supports native INT8 weights only");
  static_assert(!SUPPORT_MX,
                "CIMProcessor currently does not define microscaling across "
                "the input axis");
  static_assert(!SUPPORT_CODEBOOK_QUANT,
                "CIMProcessor currently does not decode codebook operands");
  // Both macro modes are driven identically from here. The processor never
  // assumes an issue-window length: it hands MAC requests to the array and
  // waits on the array's ready/credit handshake, and every window- and
  // latency-derived constant below the array comes from
  // Element::issue_window(), which already accounts for the per-slice serial
  // walk. Bit-serial therefore only makes each issue longer -- it does not
  // change the protocol. The widths one serial slice needs are enforced in
  // CIMElement
  static_assert(MODE == 0 || MODE == 1,
                "CIMProcessor supports bit-parallel (0) and bit-serial (1) CIM "
                "macros");
  static_assert(
      SIGNED, "CIMProcessor currently requires signed native INT8 arithmetic");
  static_assert(C_BEAT_LAYOUT == CIM_C_BEAT_OUTPUT_MAJOR,
                "CIMProcessor currently requires output-major C beats");
  static_assert(
      A_PORT_TILES == INPUT_AXIS_TILES,
      "CIMProcessor currently sends the complete input axis in one A beat");
  static_assert(C_PORT_TILES == OUTPUT_AXIS_TILES,
                "CIMProcessor requires one complete reduced MAC result to fit "
                "in one C beat");
  static_assert(WRITE_CH_IN == 1,
                "CIMProcessor currently stores one CIM weight row per request");
  static_assert(B_SETS > 0, "CIMProcessor requires a resident weight set");
  static_assert(B_SETS <= 0xFFFF,
                "CIM resident set count exceeds schedule metadata");
  static_assert(K == Array::K, "CIMProcessor K must match the CIM input axis");
  static_assert(K == CIM_ARRAY_K_DIMENSION,
                "CIMProcessor rows must match the CIM array K dimension");
  // InputController explicitly defines packing, boundary, and
  // replication-unroll rules for these extents
  static_assert(K == 4 || K == 8 || K == 16 || K == 32 || K == 64,
                "CIMProcessor currently requires an InputController-supported "
                "input extent");
  static_assert(N == Array::N, "CIMProcessor N must match the CIM output axis");
  static_assert(N == CIM_ARRAY_N_DIMENSION,
                "CIMProcessor cols must match the CIM array N dimension");
  static_assert(buffer_size > 0,
                "CIM accumulation buffer depth must be positive");
  static_assert(INPUT_BUFFER_WIDTH == ABeat::width,
                "One input-buffer word must contain one complete CIM A beat");
  static_assert(OUTPUT_AXIS_TILES % B_PORT_TILES == 0,
                "One logical B row must split into whole B-port beats");
  static_assert(K % WRITE_CH_IN == 0,
                "One resident set must hold a whole number of write blocks");
  // The lane order inside a beat is a convention, but its size is not: pin the
  // width to the array geometry so a beat cannot silently hold the wrong lanes
  static_assert(WEIGHT_WRITE_WIDTH ==
                    WRITE_CH_IN * B_PORT_TILES * Array::TILE_N * B_WIDTH,
                "A beat must hold exactly the lanes one B-port write covers");
  static_assert(WEIGHT_ROW_WIDTH ==
                    WEIGHT_WRITE_WIDTH * WEIGHT_BEATS_PER_ROW / WRITE_CH_IN,
                "Beats must tile one logical B row exactly");

  sc_in<bool> CCS_INIT_S1(clk);
  sc_in<bool> CCS_INIT_S1(rstn);

  Connections::In<ac_int<INPUT_BUFFER_WIDTH, false>> CCS_INIT_S1(input_channel);
  // Retain MatrixProcessor's weight_channel protocol at physical B-beat width
  Connections::In<ac_int<WEIGHT_WRITE_WIDTH, false>> CCS_INIT_S1(
      weight_channel);
  Connections::In<CIMWeightDescriptor> CCS_INIT_S1(weight_descriptor_channel);
  Connections::In<Pack1D<Buffer, N>> CCS_INIT_S1(bias_channel);
  Connections::In<MatrixParams> CCS_INIT_S1(params_in);

  Connections::Out<Pack1D<Buffer, N>> CCS_INIT_S1(output_channel);
  Connections::Out<ac_int<16, false>>
      accumulation_buffer_read_address[ACCUM_BUFFER_BANKS];
  Connections::In<Pack1D<Buffer, N>>
      accumulation_buffer_read_data[ACCUM_BUFFER_BANKS];
  Connections::Out<BufferWriteRequest<Pack1D<Buffer, N>>>
      accumulation_buffer_write_request[ACCUM_BUFFER_BANKS];

#if DOUBLE_BUFFERED_ACCUM_BUFFER
  Connections::SyncOut accumulation_buffer_done[ACCUM_BUFFER_BANKS];
#endif

#if SUPPORT_MX
  Connections::In<ac_int<Scale::width, false>> CCS_INIT_S1(input_scale_channel);
  Connections::In<ac_int<Scale::width * N, false>> CCS_INIT_S1(
      weight_scale_channel);
#endif

  Connections::SyncOut CCS_INIT_S1(start);

#if ENABLE_PERF_COUNTERS
  sc_in<MatrixPerformance::CounterIndex> CCS_INIT_S1(perf_counter_select);
  sc_out<MatrixPerformance::Counter> CCS_INIT_S1(perf_counter_value);
#endif

 private:
  // Bind one logical resident tile to a contiguous range on the set ring
  struct ScheduledWeightDescriptor {
    CIMWeightDescriptor logical;
    Set first_set;

    static const unsigned int width = CIMWeightDescriptor::width + Set::width;

    template <unsigned int Size>
    void Marshall(Marshaller<Size> &m) {
      m & logical;
      m & first_set;
    }

    inline friend void sc_trace(sc_trace_file *tf,
                                const ScheduledWeightDescriptor &descriptor,
                                const std::string &name) {
      sc_trace(tf, descriptor.logical, name + ".logical");
      sc_trace(tf, descriptor.first_set, name + ".first_set");
    }

    inline friend std::ostream &operator<<(
        std::ostream &os, const ScheduledWeightDescriptor &descriptor) {
      os << descriptor.logical << " " << descriptor.first_set;
      return os;
    }

    inline friend bool operator==(const ScheduledWeightDescriptor &lhs,
                                  const ScheduledWeightDescriptor &rhs) {
      return lhs.logical == rhs.logical && lhs.first_set == rhs.first_set;
    }
  };

  // ResidentSetState records whether one physical set is free or ready
  enum ResidentSetState { SET_FREE = 0, SET_READY = 1 };
  using ResidentSetStateBits = ac_int<1, false>;

  Array CCS_INIT_S1(cim_array);
  Connections::Combinational<MACRequest> CCS_INIT_S1(mac_request_channel);
  Connections::Combinational<WriteRequest> CCS_INIT_S1(write_request_channel);
  Connections::Combinational<CBeat> CCS_INIT_S1(result_channel);
#if ENABLE_PERF_COUNTERS
  sc_signal<bool> completion_storage_stall;
  sc_signal<bool> result_slot_stall;
  sc_signal<bool> completion_descriptor_stall;
#endif

  // Isolate CIM result retirement from the accumulation pipeline
  Connections::Combinational<Pack1D<Psum, N>> CCS_INIT_S1(
      result_to_accum_channel);

  // Decouple accumulator reads from their ordered response/addition stage
  Connections::Fifo<AccumulationMetadata, 2> CCS_INIT_S1(
      accumulation_metadata_fifo);
  Connections::Combinational<AccumulationMetadata> CCS_INIT_S1(
      accumulation_metadata_enq);
  Connections::Combinational<AccumulationMetadata> CCS_INIT_S1(
      accumulation_metadata_deq);

  // Match MatrixProcessor's accumulation-to-write-back decoupling
  Connections::Fifo<Pack1D<Buffer, N>, ACCUM_TO_WB_FIFO_DEPTH> CCS_INIT_S1(
      accum_to_wb_fifo);
  Connections::Combinational<Pack1D<Buffer, N>> CCS_INIT_S1(accum_to_wb_enq);
  Connections::Combinational<Pack1D<Buffer, N>> CCS_INIT_S1(accum_to_wb_deq);

  // Match MatrixProcessor's final-output decoupling after accumulation
  Connections::Fifo<Pack1D<Buffer, N>, OUTPUT_FIFO_DEPTH> CCS_INIT_S1(
      accum_output_fifo);
  Connections::Combinational<Pack1D<Buffer, N>> CCS_INIT_S1(accum_output_enq);

  // Buffer one ring traversal of descriptor assignments
  Connections::Fifo<ScheduledWeightDescriptor, B_SETS> CCS_INIT_S1(
      scheduled_weight_descriptor_fifo);
  Connections::Combinational<ScheduledWeightDescriptor> CCS_INIT_S1(
      scheduled_weight_descriptor_enq);
  Connections::Combinational<ScheduledWeightDescriptor> CCS_INIT_S1(
      scheduled_weight_descriptor_deq);

  // Centralize per-set state updates from the loader and compute scheduler
  sc_signal<ResidentSetStateBits> resident_set_state[B_SETS];
  Connections::Combinational<Set> CCS_INIT_S1(set_ready_channel);
  Connections::Combinational<Set> CCS_INIT_S1(set_release_channel);

  Connections::Fifo<MatrixParams, 1> CCS_INIT_S1(
      process_accumulation_params_fifo);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(
      process_accumulation_params_enq);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(
      process_accumulation_params_deq);

  Connections::Fifo<MatrixParams, 1> CCS_INIT_S1(collect_results_params_fifo);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(
      collect_results_params_enq);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(
      collect_results_params_deq);

  Connections::Fifo<MatrixParams, 1> CCS_INIT_S1(write_back_params_fifo);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(write_back_params_enq);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(write_back_params_deq);

#if ENABLE_PERF_COUNTERS
  sc_signal<bool> perf_completion_toggle;
  // Publish scheduler-observed weight-set stalls at job granularity
  sc_signal<MatrixPerformance::Counter> perf_mac_wait_weight_set_load_cycles;
  sc_signal<MatrixPerformance::SnapshotSequence> perf_snapshot_sequence;
  sc_signal<MatrixPerformance::Counter>
      perf_snapshot[MatrixPerformance::PERFORMANCE_COUNTER_COUNT];
#endif

 public:
  // Construct the array and the independent issue/result controllers
  SC_CTOR(CIMProcessor) {
    cim_array.clk(clk);
    cim_array.rstn(rstn);
    cim_array.mac_request_channel(mac_request_channel);
    cim_array.write_request_channel(write_request_channel);
    cim_array.result_channel(result_channel);
#if ENABLE_PERF_COUNTERS
    cim_array.completion_storage_stall(completion_storage_stall);
    cim_array.result_slot_stall(result_slot_stall);
    cim_array.completion_descriptor_stall(completion_descriptor_stall);
#endif

    accum_to_wb_fifo.clk(clk);
    accum_to_wb_fifo.rst(rstn);
    accum_to_wb_fifo.enq(accum_to_wb_enq);
    accum_to_wb_fifo.deq(accum_to_wb_deq);

    accum_output_fifo.clk(clk);
    accum_output_fifo.rst(rstn);
    accum_output_fifo.enq(accum_output_enq);
    accum_output_fifo.deq(output_channel);

    accumulation_metadata_fifo.clk(clk);
    accumulation_metadata_fifo.rst(rstn);
    accumulation_metadata_fifo.enq(accumulation_metadata_enq);
    accumulation_metadata_fifo.deq(accumulation_metadata_deq);

    scheduled_weight_descriptor_fifo.clk(clk);
    scheduled_weight_descriptor_fifo.rst(rstn);
    scheduled_weight_descriptor_fifo.enq(scheduled_weight_descriptor_enq);
    scheduled_weight_descriptor_fifo.deq(scheduled_weight_descriptor_deq);

    process_accumulation_params_fifo.clk(clk);
    process_accumulation_params_fifo.rst(rstn);
    process_accumulation_params_fifo.enq(process_accumulation_params_enq);
    process_accumulation_params_fifo.deq(process_accumulation_params_deq);

    collect_results_params_fifo.clk(clk);
    collect_results_params_fifo.rst(rstn);
    collect_results_params_fifo.enq(collect_results_params_enq);
    collect_results_params_fifo.deq(collect_results_params_deq);

    write_back_params_fifo.clk(clk);
    write_back_params_fifo.rst(rstn);
    write_back_params_fifo.enq(write_back_params_enq);
    write_back_params_fifo.deq(write_back_params_deq);

    SC_THREAD(set_scoreboard);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(issue_operations);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(load_weights);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(collect_results);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(issue_accumulation_reads);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(complete_accumulation);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(write_back);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

#if ENABLE_PERF_COUNTERS
    SC_THREAD(monitor_performance);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_METHOD(read_performance_counter);
    sensitive << perf_counter_select;
    sensitive << perf_snapshot_sequence;
    for (int i = 0; i < MatrixPerformance::PERFORMANCE_COUNTER_COUNT; i++) {
      sensitive << perf_snapshot[i];
    }
#endif
  }

 private:
  // Return the product of every scheduled matrix loop bound
  static ac_int<32, false> total_operations(const MatrixParams &params) {
    return params.loops[0][0] * params.loops[0][1] * params.loops[0][2] *
           params.loops[0][3] * params.loops[0][4] * params.loops[1][0] *
           params.loops[1][1] * params.loops[1][2] * params.loops[1][3] *
           params.loops[1][4] * params.loops[1][5];
  }

  // Advance the shared two-level loop-counter representation by one operation
  static void advance_loop_counters(
      ac_int<LOOP_WIDTH, false> loop_counters[2][6],
      const MatrixParams &params) {
    loop_counters[1][5]++;
#pragma hls_unroll yes
    for (int level = 1; level >= 0; level--) {
#pragma hls_unroll yes
      for (int loop = 5; loop >= 0; loop--) {
        if (loop_counters[level][loop] == params.loops[level][loop]) {
          loop_counters[level][loop] = 0;
          if (loop > 0) {
            loop_counters[level][loop - 1]++;
          } else if (level > 0) {
            loop_counters[level - 1][5]++;
          }
        }
      }
    }
  }

  // Match MatrixProcessor::push_inputs when deciding whether one resident
  // weight block spans X/Y operations
  static bool reuses_weights(const MatrixParams &params) {
    const auto fy1 = params.loops[0][params.fy_loop_idx[0]];
    const auto c2 = params.loops[0][params.reduction_loop_idx[0]];
    const auto fy0 = params.loops[1][params.fy_loop_idx[1]];
    const auto fx = params.loops[1][params.fx_loop_idx];
    const auto c1 = params.loops[1][params.reduction_loop_idx[1]];
    const auto k1 = params.loops[1][params.weight_loop_idx[1]];
    const bool x_inner = params.weight_loop_idx[0] < params.x_loop_idx[0];
    const bool y_inner = params.weight_loop_idx[0] < params.y_loop_idx[0];
    return fy1 == 1 && c2 == 1 && fy0 == 1 && fx == 1 && c1 == 1 && k1 == 1 &&
           (x_inner || y_inner);
  }

  // Match MatrixProcessor::push_inputs weight_reuse_idx selection for the outer
  // swap condition
  static void select_weight_reuse_indices(const MatrixParams &params,
                                          ac_int<3, false> indices[2]) {
    const bool x_inner = params.weight_loop_idx[0] < params.x_loop_idx[0];
    const bool y_inner = params.weight_loop_idx[0] < params.y_loop_idx[0];
    if (x_inner && y_inner) {
      indices[0] = params.x_loop_idx[0];
      indices[1] = params.y_loop_idx[0];
    } else {
      const auto index = x_inner ? params.x_loop_idx[0] : params.y_loop_idx[0];
      indices[0] = index;
      indices[1] = index;
    }
  }

  // Mapper-provided loop indices define weight lifetimes; this matches
  // MatrixProcessor::push_inputs swap_weights
  static bool needs_weight_load(
      const MatrixParams &params,
      const ac_int<LOOP_WIDTH, false> loop_counters[2][6],
      const ac_int<3, false> outer_reuse_indices[2], bool reuse_weights,
      ac_int<32, false> step) {
    const bool inner = loop_counters[1][params.weight_reuse_idx[0]] == 0 &&
                       loop_counters[1][params.weight_reuse_idx[1]] == 0;
    const bool outer = loop_counters[0][outer_reuse_indices[0]] == 0 &&
                       loop_counters[0][outer_reuse_indices[1]] == 0;
    return step == 0 || (inner && (!reuse_weights || outer));
  }

  // Return whether an outer loop is nested inside a live reduction
  static bool outer_loop_is_inside_reduction(const MatrixParams &params,
                                             ac_int<3, false> loop_idx) {
    const auto c2 = params.loops[0][params.reduction_loop_idx[0]];
    const auto fy1 = params.loops[0][params.fy_loop_idx[0]];
    return (c2 > 1 && params.reduction_loop_idx[0] < loop_idx) ||
           (fy1 > 1 && params.fy_loop_idx[0] < loop_idx);
  }

  // Return the accumulation entries needed by all simultaneously live outputs
  static ac_int<32, false> accumulation_footprint(const MatrixParams &params) {
    const bool include_x1 =
        outer_loop_is_inside_reduction(params, params.x_loop_idx[0]);
    const bool include_y1 =
        outer_loop_is_inside_reduction(params, params.y_loop_idx[0]);
    ac_int<32, false> x_extent = params.loops[1][params.x_loop_idx[1]];
    ac_int<32, false> y_extent = params.loops[1][params.y_loop_idx[1]];
    if (include_x1) x_extent *= params.loops[0][params.x_loop_idx[0]];
    if (include_y1) y_extent *= params.loops[0][params.y_loop_idx[0]];
    return params.loops[1][params.weight_loop_idx[1]] * y_extent * x_extent;
  }

  // Commit one ordered weight-channel beat to the selected resident set
  // WEIGHT CHANNEL CONTRACT. Width is checked, but stream and lane order are
  // conventions shared with WeightController and must change on both sides
  //
  // One resident set arrives in row-major order, with output-axis spans inside
  // each input-axis row:
  //
  //   [ k=0 span=0 ][ k=0 span=1 ] ... [ k=K-1 span=last ]
  //
  //   bit 0                              WEIGHT_WRITE_WIDTH - 1
  //   |                                                         |
  //   [ port tile 0 ][ port tile 1 ] ... [ port tile B_PORT_TILES-1 ]
  //       [n0][n1]...      [n0][n1]...       TILE_N lanes per tile
  //
  //   request.data[port_tile][0][tile_n]
  //       = beat[(port_tile * TILE_N + tile_n) * B_WIDTH]
  //   request.output_axis_tile_base = span * B_PORT_TILES
  //
  // WRITE_CH_IN is currently constrained to one, so every request writes one
  // input-axis row across B_PORT_TILES output tiles
  void write_weight_beat(Set wset, int k, int span,
                         const ac_int<WEIGHT_WRITE_WIDTH, false> &beat) {
    WriteRequest request;
    request.wset = wset;
    request.input_axis_idx = k / Array::TILE_K;
    request.output_axis_tile_base = span * B_PORT_TILES;
    request.wchi = k % Array::TILE_K;
    request.replicate = 0;
    clear_pack(request.data);

#pragma hls_unroll yes
    for (int port_tile_idx = 0; port_tile_idx < B_PORT_TILES; port_tile_idx++) {
#pragma hls_unroll yes
      for (int tile_n = 0; tile_n < Array::TILE_N; tile_n++) {
        const int lane = port_tile_idx * Array::TILE_N + tile_n;
        request.data[port_tile_idx][0][tile_n] =
            beat.template slc<B_WIDTH>(lane * B_WIDTH);
      }
    }
    write_request_channel.Push(request);
  }

  // Advance one physical set index around the resident-set ring
  static Set next_resident_set(Set current) {
    return current == Set(B_SETS - 1) ? Set(0) : Set(current + 1);
  }

  // Select one tile-relative physical set around the resident-set ring
  static Set resident_set_at_offset(Set first, ac_int<16, false> offset) {
    ac_int<17, false> index = first;
    index += offset;
    if (index >= B_SETS) {
      index -= B_SETS;
    }
    return Set(index);
  }

  // Return whether the selected physical set currently has the requested state
  bool resident_set_has_state(Set selected, ResidentSetState expected) const {
    bool matches = false;
#pragma hls_unroll yes
    for (int set = 0; set < B_SETS; set++) {
      if (selected == Set(set)) {
        matches = resident_set_state[set].read() == expected;
      }
    }
    return matches;
  }

  // Apply loader and compute transitions to the per-set state scoreboard
  void set_scoreboard() {
    set_ready_channel.ResetRead();
    set_release_channel.ResetRead();
#pragma hls_unroll yes
    for (int set = 0; set < B_SETS; set++) {
      resident_set_state[set].write(ResidentSetStateBits(SET_FREE));
    }

    wait();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
    while (true) {
      Set ready_set = 0;
      Set released_set = 0;
      const bool became_ready = set_ready_channel.PopNB(ready_set);
      const bool released = set_release_channel.PopNB(released_set);
#ifndef __SYNTHESIS__
      if (became_ready && !resident_set_has_state(ready_set, SET_FREE)) {
        SC_REPORT_FATAL("CIMProcessor", "completed resident set is not free");
      }
      if (released && !resident_set_has_state(released_set, SET_READY)) {
        SC_REPORT_FATAL("CIMProcessor", "released resident set is not ready");
      }
#endif

#pragma hls_unroll yes
      for (int set = 0; set < B_SETS; set++) {
        ResidentSetStateBits next = resident_set_state[set].read();
        if (became_ready && ready_set == Set(set)) {
          next = SET_READY;
        }
        if (released && released_set == Set(set)) {
          next = SET_FREE;
        }
        resident_set_state[set].write(next);
      }
      wait();
    }
  }

  // Pack logical tiles sequentially around the physical set ring
  void load_weights() {
    weight_channel.Reset();
    weight_descriptor_channel.Reset();
    write_request_channel.ResetWrite();
    scheduled_weight_descriptor_enq.ResetWrite();
    set_ready_channel.ResetWrite();

    wait();

    Set next_set = 0;
    while (true) {
      const CIMWeightDescriptor logical = weight_descriptor_channel.Pop();

#ifndef __SYNTHESIS__
      if (logical.set_count == 0 || logical.set_count > B_SETS ||
          logical.replay_count == 0) {
        SC_REPORT_FATAL("CIMProcessor", "invalid resident weight tile");
      }
#endif

      ScheduledWeightDescriptor scheduled;
      scheduled.logical = logical;
      scheduled.first_set = next_set;
      scheduled_weight_descriptor_enq.Push(scheduled);

      ac_int<16, false> tile_slot = 0;
      int k = 0;
      int span = 0;
      Set write_set = next_set;

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      while (tile_slot < logical.set_count) {
        // Only the first payload beat needs to wait for ownership of this set
        if (k == 0 && span == 0) {
          while (!resident_set_has_state(write_set, SET_FREE)) {
            wait();
          }
        }

        const ac_int<WEIGHT_WRITE_WIDTH, false> beat = weight_channel.Pop();
        write_weight_beat(write_set, k, span, beat);

        const bool row_complete = span == WEIGHT_BEATS_PER_ROW - 1;
        const bool set_complete = row_complete && k == K - 1;
        if (set_complete) {
          set_ready_channel.Push(write_set);
          tile_slot++;
          write_set = next_resident_set(write_set);
          next_set = write_set;
          k = 0;
          span = 0;
        } else if (row_complete) {
          k++;
          span = 0;
        } else {
          span++;
        }
      }
    }
  }

  // Pack one complete A beat from an input-buffer word
  static ABeat pack_a_beat(const ac_int<INPUT_BUFFER_WIDTH, false> &inputs) {
    ABeat beat;
    clear_pack(beat);
#pragma hls_unroll yes
    for (int k = 0; k < K; k++) {
      const int input_axis_idx = k / Array::TILE_K;
      const int tile_k = k % Array::TILE_K;
      beat[input_axis_idx][tile_k] = inputs.template slc<A_WIDTH>(k * A_WIDTH);
    }
    return beat;
  }

  // Fuse MatrixProcessor::push_inputs and push_weights into one direct CIM
  // issue controller
  void issue_operations() {
    params_in.Reset();
    input_channel.Reset();
    process_accumulation_params_enq.ResetWrite();
    collect_results_params_enq.ResetWrite();
    write_back_params_enq.ResetWrite();
    mac_request_channel.ResetWrite();
    scheduled_weight_descriptor_deq.ResetRead();
    set_release_channel.ResetWrite();
    start.Reset();
#if ENABLE_PERF_COUNTERS
    perf_mac_wait_weight_set_load_cycles.write(0);
#endif

    wait();

    ScheduledWeightDescriptor descriptor;
    Set selected_weight_set = 0;
    ac_int<16, false> set_slot = 0;
    ac_int<16, false> replay = 0;
    bool need_descriptor = true;
    bool selected_set_final_visit = false;
    while (true) {
      const MatrixParams params = params_in.Pop();
      process_accumulation_params_enq.Push(params);
      collect_results_params_enq.Push(params);
      write_back_params_enq.Push(params);
      start.SyncPush();
#if ENABLE_PERF_COUNTERS
      MatrixPerformance::Counter mac_wait_weight_set_load_cycles = 0;
#endif

#ifndef __SYNTHESIS__
      if (params.use_input_codebook || params.use_weight_codebook) {
        SC_REPORT_FATAL("CIMProcessor",
                        "currently does not support codebook operands");
      }
      if (params.is_mx_op || params.is_resnet_replication ||
          params.is_generic_replication) {
        SC_REPORT_FATAL("CIMProcessor",
                        "currently does not support MX or replicated operands");
      }
      const bool outer_k_context =
          params.loops[0][params.weight_loop_idx[0]] > 1 &&
          outer_loop_is_inside_reduction(params, params.weight_loop_idx[0]);
      if (outer_k_context) {
        SC_REPORT_FATAL("CIMProcessor",
                        "outer output-column partial contexts are unsupported");
      }
#if DOUBLE_BUFFERED_ACCUM_BUFFER
      const bool outer_spatial_context =
          outer_loop_is_inside_reduction(params, params.x_loop_idx[0]) ||
          outer_loop_is_inside_reduction(params, params.y_loop_idx[0]);
      if (outer_spatial_context && params.write_output_to_accum_buffer) {
        SC_REPORT_FATAL(
            "CIMProcessor",
            "outer partial contexts cannot use banked accumulation output");
      }
#endif
      if (accumulation_footprint(params) > buffer_size) {
        SC_REPORT_FATAL("CIMProcessor",
                        "live partial outputs exceed accumulation capacity");
      }
#endif

      ac_int<LOOP_WIDTH, false> loop_counters[2][6];
#pragma hls_unroll yes
      for (int level = 0; level < 2; level++) {
#pragma hls_unroll yes
        for (int loop = 0; loop < 6; loop++) {
          loop_counters[level][loop] = 0;
        }
      }

      const ac_int<32, false> total_ops = total_operations(params);
      const bool reuse_weights = reuses_weights(params);
      ac_int<3, false> outer_reuse_indices[2];
      select_weight_reuse_indices(params, outer_reuse_indices);

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      for (ac_int<32, false> step = 0; step < total_ops; step++) {
        if (needs_weight_load(params, loop_counters, outer_reuse_indices,
                              reuse_weights, step)) {
          if (need_descriptor) {
            descriptor = scheduled_weight_descriptor_deq.Pop();

#ifndef __SYNTHESIS__
            if (descriptor.logical.set_count == 0 ||
                descriptor.logical.set_count > B_SETS ||
                descriptor.first_set >= B_SETS ||
                descriptor.logical.replay_count == 0) {
              SC_REPORT_FATAL("CIMProcessor",
                              "invalid scheduled resident weight tile");
            }
#endif

            set_slot = 0;
            replay = 0;
            need_descriptor = false;
          }

          selected_weight_set =
              resident_set_at_offset(descriptor.first_set, set_slot);
          selected_set_final_visit =
              replay == descriptor.logical.replay_count - 1;
          if (replay == 0) {
            while (!resident_set_has_state(selected_weight_set, SET_READY)) {
#if ENABLE_PERF_COUNTERS
              mac_wait_weight_set_load_cycles++;
#endif
              wait();
            }
          }

          set_slot++;
          if (set_slot == descriptor.logical.set_count) {
            set_slot = 0;
            replay++;
            if (replay == descriptor.logical.replay_count) {
              replay = 0;
              need_descriptor = true;
            }
          }
        }

        MACRequest request;
        request.a = pack_a_beat(input_channel.Pop());
        request.mset = selected_weight_set;
        // Current mapping multicasts each A beat to every tile along the output
        // axis output_axis_idx is ignored for multicast requests; zero is its
        // canonical unused value
        request.output_axis_idx = 0;
        request.multicast = 1;
        // Reduction leaves one result per output-axis tile, and the C-port
        // assertion packs all N values in one beat
        request.reduce = 1;
        // Atomic request acceptance is the physical issue acknowledged by
        // CIMArray
        mac_request_channel.Push(request);

        advance_loop_counters(loop_counters, params);
        const bool leaves_selected_set =
            step + 1 == total_ops ||
            needs_weight_load(params, loop_counters, outer_reuse_indices,
                              reuse_weights, step + 1);
        if (selected_set_final_visit && leaves_selected_set) {
          // The array independently blocks same-set writes until this accepted
          // MAC closes its physical issue window
          set_release_channel.Push(selected_weight_set);
        }
      }

#if ENABLE_PERF_COUNTERS
      perf_mac_wait_weight_set_load_cycles.write(
          mac_wait_weight_set_load_cycles);
#endif
#ifndef __SYNTHESIS__
      if (!need_descriptor) {
        SC_REPORT_FATAL("CIMProcessor",
                        "matrix job ended inside a resident weight tile");
      }
#endif
    }
  }

  // Match the address expression in MatrixProcessor accumulation and write-back
  static ac_int<16, false> accumulation_address(
      const MatrixParams &params,
      const ac_int<LOOP_WIDTH, false> loop_counters[2][6]) {
    const bool include_x1 =
        outer_loop_is_inside_reduction(params, params.x_loop_idx[0]);
    const bool include_y1 =
        outer_loop_is_inside_reduction(params, params.y_loop_idx[0]);
    const ac_int<16, false> x0_extent = params.loops[1][params.x_loop_idx[1]];
    const ac_int<16, false> y0_extent = params.loops[1][params.y_loop_idx[1]];
    ac_int<16, false> x_extent = x0_extent;
    ac_int<16, false> y_extent = y0_extent;
    ac_int<16, false> x = loop_counters[1][params.x_loop_idx[1]];
    ac_int<16, false> y = loop_counters[1][params.y_loop_idx[1]];
    if (include_x1) {
      x_extent *= params.loops[0][params.x_loop_idx[0]];
      x += loop_counters[0][params.x_loop_idx[0]] * x0_extent;
    }
    if (include_y1) {
      y_extent *= params.loops[0][params.y_loop_idx[0]];
      y += loop_counters[0][params.y_loop_idx[0]] * y0_extent;
    }
    return loop_counters[1][params.weight_loop_idx[1]] * y_extent * x_extent +
           y * x_extent + x;
  }

  // Match MatrixProcessor::process_accumulation is_non_accumulating_tile
  static bool starts_accumulation(
      const MatrixParams &params,
      const ac_int<LOOP_WIDTH, false> loop_counters[2][6]) {
    return loop_counters[0][params.reduction_loop_idx[0]] == 0 &&
           loop_counters[1][params.reduction_loop_idx[1]] == 0 &&
           loop_counters[1][params.fx_loop_idx] == 0 &&
           loop_counters[0][params.fy_loop_idx[0]] == 0 &&
           loop_counters[1][params.fy_loop_idx[1]] == 0;
  }

  // Match MatrixProcessor::write_back accumulation_finished
  static bool finishes_accumulation(
      const MatrixParams &params,
      const ac_int<LOOP_WIDTH, false> loop_counters[2][6]) {
    return loop_counters[0][params.reduction_loop_idx[0]] ==
               params.loops[0][params.reduction_loop_idx[0]] - 1 &&
           loop_counters[1][params.reduction_loop_idx[1]] ==
               params.loops[1][params.reduction_loop_idx[1]] - 1 &&
           loop_counters[1][params.fx_loop_idx] ==
               params.loops[1][params.fx_loop_idx] - 1 &&
           loop_counters[0][params.fy_loop_idx[0]] ==
               params.loops[0][params.fy_loop_idx[0]] - 1 &&
           loop_counters[1][params.fy_loop_idx[1]] ==
               params.loops[1][params.fy_loop_idx[1]] - 1;
  }

  // Return whether this operation writes a partial sum for a later operation
  static bool writes_accumulation(
      const MatrixParams &params,
      const ac_int<LOOP_WIDTH, false> loop_counters[2][6]) {
    return !finishes_accumulation(params, loop_counters) ||
           (DOUBLE_BUFFERED_ACCUM_BUFFER &&
            params.write_output_to_accum_buffer);
  }

  // Match MatrixProcessor's double-buffer bank-switch boundary
  static bool finishes_output_tile(
      const MatrixParams &params,
      const ac_int<LOOP_WIDTH, false> loop_counters[2][6]) {
    const bool finishes_x_context =
        !outer_loop_is_inside_reduction(params, params.x_loop_idx[0]) ||
        loop_counters[0][params.x_loop_idx[0]] ==
            params.loops[0][params.x_loop_idx[0]] - 1;
    const bool finishes_y_context =
        !outer_loop_is_inside_reduction(params, params.y_loop_idx[0]) ||
        loop_counters[0][params.y_loop_idx[0]] ==
            params.loops[0][params.y_loop_idx[0]] - 1;
    return finishes_accumulation(params, loop_counters) && finishes_x_context &&
           finishes_y_context &&
           loop_counters[1][params.weight_loop_idx[1]] ==
               params.loops[1][params.weight_loop_idx[1]] - 1 &&
           loop_counters[1][params.x_loop_idx[1]] ==
               params.loops[1][params.x_loop_idx[1]] - 1 &&
           loop_counters[1][params.y_loop_idx[1]] ==
               params.loops[1][params.y_loop_idx[1]] - 1;
  }

  // Pop the one complete reduced MAC-result beat and unpack its N lanes
  Pack1D<Psum, N> collect_complete_result() {
    const CBeat beat = result_channel.Pop();
    Pack1D<Psum, N> result;
#pragma hls_unroll yes
    for (int output_axis_idx = 0; output_axis_idx < OUTPUT_AXIS_TILES;
         output_axis_idx++) {
#pragma hls_unroll yes
      for (int tile_n = 0; tile_n < Array::TILE_N; tile_n++) {
        const int n = output_axis_idx * Array::TILE_N + tile_n;
        ac_int<Psum::width, SIGNED> value;
        value.set_slc(0, beat[output_axis_idx][tile_n]);
        result[n] = Psum(value);
      }
    }
    return result;
  }

  // Retire raw CIM results before accumulation
  void collect_results() {
    collect_results_params_deq.ResetRead();
    result_channel.ResetRead();
    result_to_accum_channel.ResetWrite();

    wait();

    while (true) {
      const MatrixParams params = collect_results_params_deq.Pop();
      const ac_int<32, false> total_ops = total_operations(params);
#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      for (ac_int<32, false> step = 0; step < total_ops; step++) {
        result_to_accum_channel.Push(collect_complete_result());
      }
    }
  }

  // Issue ordered accumulator reads and preserve their partial-result metadata
  void issue_accumulation_reads() {
    process_accumulation_params_deq.ResetRead();
    result_to_accum_channel.ResetRead();
    bias_channel.Reset();
    accumulation_metadata_enq.ResetWrite();
#pragma hls_unroll yes
    for (int bank = 0; bank < ACCUM_BUFFER_BANKS; bank++) {
      accumulation_buffer_read_address[bank].Reset();
    }

    bool accumulation_buffer_bank = false;
    wait();

    while (true) {
      const MatrixParams params = process_accumulation_params_deq.Pop();
      ac_int<LOOP_WIDTH, false> loop_counters[2][6];
#pragma hls_unroll yes
      for (int level = 0; level < 2; level++) {
#pragma hls_unroll yes
        for (int loop = 0; loop < 6; loop++) {
          loop_counters[level][loop] = 0;
        }
      }

      Pack1D<Buffer, N> bias = Pack1D<Buffer, N>::zero();
      int bias_reuse_indices[4] = {5, 5, 5, 5};
      for (int loop = 5; loop > params.weight_loop_idx[1]; loop--) {
        bias_reuse_indices[5 - loop] = loop;
      }

      const ac_int<32, false> total_ops = total_operations(params);
#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      for (ac_int<32, false> step = 0; step < total_ops; step++) {
        AccumulationMetadata metadata;
        metadata.result = result_to_accum_channel.Pop();
        metadata.first = starts_accumulation(params, loop_counters);
        metadata.initial = Pack1D<Buffer, N>::zero();
        metadata.bank = accumulation_buffer_bank;

        if (metadata.first) {
          if (params.has_bias) {
            const bool read_bias =
                loop_counters[1][bias_reuse_indices[0]] == 0 &&
                loop_counters[1][bias_reuse_indices[1]] == 0 &&
                loop_counters[1][bias_reuse_indices[2]] == 0 &&
                loop_counters[1][bias_reuse_indices[3]] == 0;
            if (read_bias) {
              bias = bias_channel.Pop();
            }
            metadata.initial = bias;
          }
        } else {
          accumulation_buffer_read_address[accumulation_buffer_bank].Push(
              accumulation_address(params, loop_counters));
        }

        accumulation_metadata_enq.Push(metadata);

#if DOUBLE_BUFFERED_ACCUM_BUFFER
        if (params.write_output_to_accum_buffer &&
            finishes_output_tile(params, loop_counters)) {
          accumulation_buffer_bank = !accumulation_buffer_bank;
        }
#endif

        advance_loop_counters(loop_counters, params);
      }
    }
  }

  // Complete accumulator reads and enqueue lane additions in issue order
  void complete_accumulation() {
    accumulation_metadata_deq.ResetRead();
    accum_to_wb_enq.ResetWrite();
#pragma hls_unroll yes
    for (int bank = 0; bank < ACCUM_BUFFER_BANKS; bank++) {
      accumulation_buffer_read_data[bank].Reset();
    }

    wait();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
    while (true) {
      const AccumulationMetadata metadata = accumulation_metadata_deq.Pop();
      Pack1D<Buffer, N> previous = metadata.initial;

      if (!metadata.first) {
#pragma hls_unroll yes
        for (int bank = 0; bank < ACCUM_BUFFER_BANKS; bank++) {
          if (metadata.bank == bank) {
            previous = accumulation_buffer_read_data[bank].Pop();
          }
        }
      }

#pragma hls_unroll yes
      for (int n = 0; n < N; n++) {
        previous[n] += static_cast<Buffer>(metadata.result[n]);
      }

      accum_to_wb_enq.Push(previous);
    }
  }

  // Write accumulated values to the buffer or final-output FIFO
  void write_back() {
    write_back_params_deq.ResetRead();
    accum_to_wb_deq.ResetRead();
    accum_output_enq.ResetWrite();
#pragma hls_unroll yes
    for (int bank = 0; bank < ACCUM_BUFFER_BANKS; bank++) {
      accumulation_buffer_write_request[bank].Reset();
#if DOUBLE_BUFFERED_ACCUM_BUFFER
      accumulation_buffer_done[bank].Reset();
#endif
    }

#if ENABLE_PERF_COUNTERS
    perf_completion_toggle.write(false);
#endif

    bool accumulation_buffer_bank = false;
    wait();

    while (true) {
      const MatrixParams params = write_back_params_deq.Pop();
      ac_int<LOOP_WIDTH, false> loop_counters[2][6];
#pragma hls_unroll yes
      for (int level = 0; level < 2; level++) {
#pragma hls_unroll yes
        for (int loop = 0; loop < 6; loop++) {
          loop_counters[level][loop] = 0;
        }
      }

      const ac_int<32, false> total_ops = total_operations(params);
#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      for (ac_int<32, false> step = 0; step < total_ops; step++) {
        const Pack1D<Buffer, N> accumulated = accum_to_wb_deq.Pop();
        const bool write_accumulation =
            writes_accumulation(params, loop_counters);

        if (!write_accumulation) {
          accum_output_enq.Push(accumulated);
        } else {
          BufferWriteRequest<Pack1D<Buffer, N>> request;
          request.address = accumulation_address(params, loop_counters);
          request.data = accumulated;
          request.last = false;
          accumulation_buffer_write_request[accumulation_buffer_bank].Push(
              request);
        }

#if DOUBLE_BUFFERED_ACCUM_BUFFER
        if (params.write_output_to_accum_buffer &&
            finishes_output_tile(params, loop_counters)) {
          accumulation_buffer_done[accumulation_buffer_bank].SyncPush();
          accumulation_buffer_bank = !accumulation_buffer_bank;
        }
#endif

        advance_loop_counters(loop_counters, params);
      }

#if ENABLE_PERF_COUNTERS
      perf_completion_toggle.write(!perf_completion_toggle.read());
#endif
    }
  }

#if ENABLE_PERF_COUNTERS
  // Count synthesized CIM handshakes and stalls without participating in
  // datapath control
  void monitor_performance() {
    MatrixPerformance::Counter
        counters[MatrixPerformance::PERFORMANCE_COUNTER_COUNT];
    ac_int<16, false> inflight = 0;
    MatrixPerformance::SnapshotSequence snapshot_sequence = 0;
    bool active = false;
    bool observed_completion_toggle = false;

#pragma hls_unroll yes
    for (int i = 0; i < MatrixPerformance::PERFORMANCE_COUNTER_COUNT; i++) {
      counters[i] = 0;
      perf_snapshot[i].write(0);
    }
    perf_snapshot_sequence.write(0);

    wait();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
    while (true) {
      const bool completion_toggle = perf_completion_toggle.read();
      const bool completed = completion_toggle != observed_completion_toggle;
      const bool started = params_in.vld.read() && params_in.rdy.read();
#ifdef __SYNTHESIS__
      const bool issue =
          mac_request_channel.vld.read() && mac_request_channel.rdy.read();
      const bool retire =
          result_channel.vld.read() && result_channel.rdy.read();
#else
      const bool issue = false;
      const bool retire = false;
#endif

      if (completed) {
        snapshot_sequence++;
        perf_snapshot_sequence.write(snapshot_sequence);
        counters[MatrixPerformance::storage_index(
            MatrixPerformance::MAC_WAIT_WEIGHT_SET_LOAD_CYCLES)] =
            perf_mac_wait_weight_set_load_cycles.read();
#pragma hls_unroll yes
        for (int i = 0; i < MatrixPerformance::PERFORMANCE_COUNTER_COUNT; i++) {
          perf_snapshot[i].write(counters[i]);
        }
        active = false;
        observed_completion_toggle = completion_toggle;
      }

      if (started) {
#pragma hls_unroll yes
        for (int i = 0; i < MatrixPerformance::PERFORMANCE_COUNTER_COUNT; i++) {
          counters[i] = 0;
        }
        inflight = 0;
        active = true;
      } else if (active && !completed) {
        counters[MatrixPerformance::storage_index(
            MatrixPerformance::PROCESSOR_ACTIVE_CYCLES)]++;

        if (inflight != 0 || issue)
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::ARRAY_RESIDENT_CYCLES)]++;
        if (issue)
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::ARRAY_ISSUE_CYCLES)]++;

        if (input_channel.rdy.read() && !input_channel.vld.read())
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::INPUT_UNAVAILABLE_CYCLES)]++;
#ifdef __SYNTHESIS__
        const bool input_backpressured =
            mac_request_channel.vld.read() && !mac_request_channel.rdy.read();
        if (input_backpressured)
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::INPUT_BACKPRESSURE_CYCLES)]++;
        if (completion_storage_stall.read())
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::CIM_COMPLETION_STORAGE_STALL_CYCLES)]++;
        if (result_slot_stall.read())
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::CIM_RESULT_SLOT_STALL_CYCLES)]++;
        if (completion_descriptor_stall.read())
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::CIM_COMPLETION_DESCRIPTOR_STALL_CYCLES)]++;
#endif
        if (weight_channel.rdy.read() && !weight_channel.vld.read())
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::WEIGHT_UNAVAILABLE_CYCLES)]++;
#ifdef __SYNTHESIS__
        if (weight_channel.vld.read() && !weight_channel.rdy.read())
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::WEIGHT_BACKPRESSURE_CYCLES)]++;
        if (result_channel.vld.read() && !result_channel.rdy.read())
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::RESULT_BACKPRESSURE_CYCLES)]++;
#endif

        bool accumulation_stalled = false;
#pragma hls_unroll yes
        for (int i = 0; i < ACCUM_BUFFER_BANKS; i++) {
          accumulation_stalled |=
              accumulation_buffer_read_address[i].vld.read() &&
              !accumulation_buffer_read_address[i].rdy.read();
          accumulation_stalled |= accumulation_buffer_read_data[i].rdy.read() &&
                                  !accumulation_buffer_read_data[i].vld.read();
          accumulation_stalled |=
              accumulation_buffer_write_request[i].vld.read() &&
              !accumulation_buffer_write_request[i].rdy.read();
        }

#ifdef __SYNTHESIS__
        if (accum_to_wb_enq.vld.read() && !accum_to_wb_enq.rdy.read())
          accumulation_stalled = true;
#endif

        bool final_output_stalled = false;
#ifdef __SYNTHESIS__
        final_output_stalled =
            accum_output_enq.vld.read() && !accum_output_enq.rdy.read();
#endif
        if (accumulation_stalled)
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::ACCUMULATION_STALL_CYCLES)]++;
#ifdef __SYNTHESIS__
        if (final_output_stalled)
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::OUTPUT_FIFO_FULL_CYCLES)]++;
#endif
        if (output_channel.vld.read() && !output_channel.rdy.read())
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::OUTPUT_BACKPRESSURE_CYCLES)]++;

        if (issue && !retire) {
          inflight++;
        } else if (retire && !issue && inflight != 0) {
          inflight--;
        }
      }

      wait();
    }
  }

  // Select one stable snapshot register for the external CSR-style read port
  void read_performance_counter() {
    MatrixPerformance::Counter value = 0;
    if (perf_counter_select.read() == MatrixPerformance::SNAPSHOT_SEQUENCE)
      value = perf_snapshot_sequence.read();
#pragma hls_unroll yes
    for (int i = 0; i < MatrixPerformance::PERFORMANCE_COUNTER_COUNT; i++) {
      if (perf_counter_select.read() ==
          MatrixPerformance::PROCESSOR_ACTIVE_CYCLES + i)
        value = perf_snapshot[i].read();
    }
    perf_counter_value.write(value);
  }
#endif
};
