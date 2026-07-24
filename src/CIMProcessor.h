#pragma once

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include <type_traits>

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
          int B_PORT_TILES, int C_PORT_TILES, int C_BEAT_LAYOUT>
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
               C_PORT_TILES, C_BEAT_LAYOUT>;
  using ABeat = typename Array::ABeat;
  using CBeat = typename Array::CBeat;
  using Set = typename Array::Set;
  using MACRequest = typename Array::MACRequest;
  using WriteRequest = typename Array::WriteRequest;

  // One array B-port write
  static constexpr int WEIGHT_WRITE_WIDTH = Array::BBeat::width;
  // One logical B row matches MatrixProcessor's weight-channel convention
  static constexpr int WEIGHT_ROW_WIDTH = N * B_WIDTH;
  // Narrowing the B port splits one row into more array writes
  static constexpr int WEIGHT_BEATS_PER_ROW = OUTPUT_AXIS_TILES / B_PORT_TILES;

  static constexpr int LOOP_WIDTH = 10;
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
  static_assert(
      MODE == 0,
      "CIMProcessor currently requires native bit-parallel CIM macros");
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
  static_assert(
      B_SETS >= 2,
      "CIMProcessor currently alternates at least two resident weight sets");
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
  Array CCS_INIT_S1(cim_array);
  Connections::Combinational<MACRequest> CCS_INIT_S1(mac_request_channel);
  Connections::Combinational<WriteRequest> CCS_INIT_S1(write_request_channel);
  Connections::Combinational<CBeat> CCS_INIT_S1(result_channel);

  // Swap interlock mirroring the weight DoubleBuffer's bank alternation: a set
  // becomes MAC-able once completely filled, and is refilled only after every
  // result computed from it has been collected (the analog of a bank finishing
  // its read phase), which closes each element's MAC issue window
  // The release tokens pass through small FIFOs so the result thread never
  // blocks on a token that only a future job's fill will consume
  Connections::Combinational<bool> set_filled[2];
  Connections::Fifo<bool, 2> CCS_INIT_S1(set_consumed_fifo_0);
  Connections::Fifo<bool, 2> CCS_INIT_S1(set_consumed_fifo_1);
  Connections::Combinational<bool> set_consumed_enq[2];
  Connections::Combinational<bool> set_consumed_deq[2];

  // MatrixProcessor has separate accumulation/write-back parameter FIFOs; the
  // fused result thread needs one
  Connections::Fifo<MatrixParams, 1> CCS_INIT_S1(result_params_fifo);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(result_params_enq);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(result_params_deq);

#if ENABLE_PERF_COUNTERS
  sc_signal<bool> perf_completion_toggle;
  sc_signal<MatrixPerformance::SnapshotSequence> perf_snapshot_sequence;
  sc_signal<MatrixPerformance::Counter>
      perf_snapshot[MatrixPerformance::COMMON_PERFORMANCE_COUNTER_COUNT];
#endif

 public:
  // Construct the array and the independent issue/result controllers
  SC_CTOR(CIMProcessor) {
    cim_array.clk(clk);
    cim_array.rstn(rstn);
    cim_array.mac_request_channel(mac_request_channel);
    cim_array.write_request_channel(write_request_channel);
    cim_array.result_channel(result_channel);

    result_params_fifo.clk(clk);
    result_params_fifo.rst(rstn);
    result_params_fifo.enq(result_params_enq);
    result_params_fifo.deq(result_params_deq);

    set_consumed_fifo_0.clk(clk);
    set_consumed_fifo_0.rst(rstn);
    set_consumed_fifo_0.enq(set_consumed_enq[0]);
    set_consumed_fifo_0.deq(set_consumed_deq[0]);

    set_consumed_fifo_1.clk(clk);
    set_consumed_fifo_1.rst(rstn);
    set_consumed_fifo_1.enq(set_consumed_enq[1]);
    set_consumed_fifo_1.deq(set_consumed_deq[1]);

    SC_THREAD(issue_operations);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(load_weights);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(process_results);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

#if ENABLE_PERF_COUNTERS
    SC_THREAD(monitor_performance);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_METHOD(read_performance_counter);
    sensitive << perf_counter_select;
    sensitive << perf_snapshot_sequence;
    for (int i = 0; i < MatrixPerformance::COMMON_PERFORMANCE_COUNTER_COUNT;
         i++) {
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

  // Load each fixed-size beat stream into alternating resident sets
  void load_weights() {
    weight_channel.Reset();
    write_request_channel.ResetWrite();
    set_filled[0].ResetWrite();
    set_filled[1].ResetWrite();
    set_consumed_deq[0].ResetRead();
    set_consumed_deq[1].ResetRead();

    wait();

    bool bank = 0;
    bool filled_before[2] = {false, false};
    while (true) {
      if (filled_before[bank]) {
        // The DoubleBuffer's write-after-read interlock: no overwrite of a live
        // set
        set_consumed_deq[bank].Pop();
      }

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      for (int k = 0; k < K; k++) {
        for (int span = 0; span < WEIGHT_BEATS_PER_ROW; span++) {
          const ac_int<WEIGHT_WRITE_WIDTH, false> beat = weight_channel.Pop();
          write_weight_beat(Set(bank), k, span, beat);
        }
      }

      set_filled[bank].Push(true);
      filled_before[bank] = true;
      bank = !bank;
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
    result_params_enq.ResetWrite();
    mac_request_channel.ResetWrite();
    set_filled[0].ResetRead();
    set_filled[1].ResetRead();
    start.Reset();

    wait();

    // The active set follows the controller's bank alternation; this thread
    // only times the swaps, it no longer chooses which set to load
    Set active_wset = 0;
    bool have_active = false;
    while (true) {
      const MatrixParams params = params_in.Pop();
      // MatrixProcessor::push_inputs also sends params to push_weights_params;
      // this thread owns both schedules
      result_params_enq.Push(params);
      start.SyncPush();

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
        // needs_weight_load times the swap exactly like MatrixProcessor
        // push_inputs swap_weights Waiting on set_filled guarantees every row
        // of the incoming set is committed first The result thread releases the
        // outgoing set once its results are all collected
        if (needs_weight_load(params, loop_counters, outer_reuse_indices,
                              reuse_weights, step)) {
          if (have_active) {
            active_wset = (active_wset & 1) ? Set(0) : Set(1);
          }
          set_filled[active_wset & 1].Pop();
          have_active = true;
        }

        MACRequest request;
        request.a = pack_a_beat(input_channel.Pop());
        request.mset = active_wset;
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
      }
    }
  }

  // Match the address expression in MatrixProcessor::process_accumulation and
  // write_back
  static ac_int<16, false> accumulation_address(
      const MatrixParams &params,
      const ac_int<LOOP_WIDTH, false> loop_counters[2][6]) {
    const ac_int<LOOP_WIDTH, false> y0 = params.loops[1][params.y_loop_idx[1]];
    const ac_int<LOOP_WIDTH, false> x0 = params.loops[1][params.x_loop_idx[1]];
    const ac_int<16, false> k_stride = y0 * x0;
    return loop_counters[1][params.weight_loop_idx[1]] * k_stride +
           loop_counters[1][params.y_loop_idx[1]] * x0 +
           loop_counters[1][params.x_loop_idx[1]];
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

  // Collect, accumulate, and publish results in matrix-loop order
  // Fuse MatrixProcessor::process_accumulation and write_back for the direct
  // CIM result path
  void process_results() {
    result_params_deq.ResetRead();
    result_channel.ResetRead();
    bias_channel.Reset();
    output_channel.Reset();
#pragma hls_unroll yes
    for (int bank = 0; bank < ACCUM_BUFFER_BANKS; bank++) {
      accumulation_buffer_read_address[bank].Reset();
      accumulation_buffer_read_data[bank].Reset();
      accumulation_buffer_write_request[bank].Reset();
#if DOUBLE_BUFFERED_ACCUM_BUFFER
      accumulation_buffer_done[bank].Reset();
#endif
    }

    set_consumed_enq[0].ResetWrite();
    set_consumed_enq[1].ResetWrite();

#if ENABLE_PERF_COUNTERS
    perf_completion_toggle.write(false);
#endif

    bool accumulation_buffer_bank = false;
    wait();

    // Mirror the issue thread's set alternation to release each set only
    // after the last result computed from it has been collected
    Set release_wset = 0;
    bool have_release = false;
    while (true) {
      const MatrixParams params = result_params_deq.Pop();
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

      const bool reuse_weights = reuses_weights(params);
      ac_int<3, false> outer_reuse_indices[2];
      select_weight_reuse_indices(params, outer_reuse_indices);

      const ac_int<32, false> total_ops = total_operations(params);
#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      for (ac_int<32, false> step = 0; step < total_ops; step++) {
        // At a swap boundary every result of the outgoing set has been
        // collected, so its elements' MAC issue windows are closed and the
        // fill thread may safely overwrite it
        if (needs_weight_load(params, loop_counters, outer_reuse_indices,
                              reuse_weights, step)) {
          if (have_release) {
            set_consumed_enq[release_wset & 1].Push(true);
            release_wset = (release_wset & 1) ? Set(0) : Set(1);
          }
          have_release = true;
        }

        const Pack1D<Psum, N> result = collect_complete_result();
        const bool first = starts_accumulation(params, loop_counters);
        const bool last = finishes_accumulation(params, loop_counters);
        Pack1D<Buffer, N> previous = Pack1D<Buffer, N>::zero();

        if (first) {
          if (params.has_bias) {
            const bool read_bias =
                loop_counters[1][bias_reuse_indices[0]] == 0 &&
                loop_counters[1][bias_reuse_indices[1]] == 0 &&
                loop_counters[1][bias_reuse_indices[2]] == 0 &&
                loop_counters[1][bias_reuse_indices[3]] == 0;
            if (read_bias) {
              bias = bias_channel.Pop();
            }
            previous = bias;
          }
        } else {
          const ac_int<16, false> address =
              accumulation_address(params, loop_counters);
          accumulation_buffer_read_address[accumulation_buffer_bank].Push(
              address);
          previous =
              accumulation_buffer_read_data[accumulation_buffer_bank].Pop();
        }

#pragma hls_unroll yes
        // Match MatrixProcessor::process_accumulation previous_accumulation
        // plus outputs
        for (int n = 0; n < N; n++) {
          previous[n] += static_cast<Buffer>(result[n]);
        }

        // Match MatrixProcessor::write_back direct-output versus
        // accumulation-buffer decision
        if (last && !(DOUBLE_BUFFERED_ACCUM_BUFFER &&
                      params.write_output_to_accum_buffer)) {
          output_channel.Push(previous);
        } else {
          BufferWriteRequest<Pack1D<Buffer, N>> request;
          request.address = accumulation_address(params, loop_counters);
          request.data = previous;
          request.last = false;
          accumulation_buffer_write_request[accumulation_buffer_bank].Push(
              request);
        }

#if DOUBLE_BUFFERED_ACCUM_BUFFER
        if (params.write_output_to_accum_buffer) {
          const bool output_tile_completed =
              last &&
              loop_counters[1][params.weight_loop_idx[1]] ==
                  params.loops[1][params.weight_loop_idx[1]] - 1 &&
              loop_counters[1][params.x_loop_idx[1]] ==
                  params.loops[1][params.x_loop_idx[1]] - 1 &&
              loop_counters[1][params.y_loop_idx[1]] ==
                  params.loops[1][params.y_loop_idx[1]] - 1;
          if (output_tile_completed) {
            accumulation_buffer_done[accumulation_buffer_bank].SyncPush();
            accumulation_buffer_bank = !accumulation_buffer_bank;
          }
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
        counters[MatrixPerformance::COMMON_PERFORMANCE_COUNTER_COUNT];
    ac_int<16, false> inflight = 0;
    MatrixPerformance::SnapshotSequence snapshot_sequence = 0;
    bool active = false;
    bool observed_completion_toggle = false;

#pragma hls_unroll yes
    for (int i = 0; i < MatrixPerformance::COMMON_PERFORMANCE_COUNTER_COUNT;
         i++) {
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
#pragma hls_unroll yes
        for (int i = 0; i < MatrixPerformance::COMMON_PERFORMANCE_COUNTER_COUNT;
             i++) {
          perf_snapshot[i].write(counters[i]);
        }
        active = false;
        observed_completion_toggle = completion_toggle;
      }

      if (started) {
#pragma hls_unroll yes
        for (int i = 0; i < MatrixPerformance::COMMON_PERFORMANCE_COUNTER_COUNT;
             i++) {
          counters[i] = 0;
        }
        inflight = 0;
        active = true;
      } else if (active && !completed) {
        counters[MatrixPerformance::storage_index(
            MatrixPerformance::CORE_CYCLES)]++;

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
        if (mac_request_channel.vld.read() && !mac_request_channel.rdy.read())
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::INPUT_BACKPRESSURE_CYCLES)]++;
#endif
        if (weight_channel.rdy.read() && !weight_channel.vld.read())
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::WEIGHT_UNAVAILABLE_CYCLES)]++;
#ifdef __SYNTHESIS__
        if (write_request_channel.vld.read() &&
            !write_request_channel.rdy.read())
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

        if (accumulation_stalled)
          counters[MatrixPerformance::storage_index(
              MatrixPerformance::ACCUMULATION_STALL_CYCLES)]++;
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
    if (perf_counter_select.read() == MatrixPerformance::SCHEMA_VERSION)
      value = MatrixPerformance::SCHEMA_VERSION_VALUE;
    if (perf_counter_select.read() == MatrixPerformance::SNAPSHOT_SEQUENCE)
      value = perf_snapshot_sequence.read();
#pragma hls_unroll yes
    for (int i = 0; i < MatrixPerformance::COMMON_PERFORMANCE_COUNTER_COUNT;
         i++) {
      if (perf_counter_select.read() == MatrixPerformance::CORE_CYCLES + i)
        value = perf_snapshot[i].read();
    }
    perf_counter_value.write(value);
  }
#endif
};
