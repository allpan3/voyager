#pragma once

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include <type_traits>

#include "ArchitectureParams.h"
#include "CIMArray.h"
#include "Params.h"

// CIMProcessor directly drives a resident-weight CIMArray for the strict native
// mapping Its issue controller fuses MatrixProcessor's input and weight
// controllers: it loads a resident weight set at each schedule swap point, then
// issues the associated A beat. Its result controller unpacks array results and
// uses MatrixUnit's buffer for temporal sums rows and cols preserve the
// MatrixProcessor signature; K and N are their internal matrix-dimension
// aliases buffer_size mirrors the MatrixProcessor backend signature; MatrixUnit
// owns the storage
template <typename InputTypeTuple, typename WeightTypeTuple, typename Input,
          typename Weight, typename Psum, typename Buffer, typename Scale,
          int rows, int cols, int buffer_size, int CH_IN, int CH_OUT,
          int B_SETS, int BASE_A_WIDTH, int BASE_B_WIDTH, int BASE_C_WIDTH,
          int WRITE_CH_IN, int MAC_LATENCY, int MODE, int A_WIDTH, int B_WIDTH,
          bool SIGNED, int TILE_INPUT_AXIS_ELEMENTS,
          int TILE_OUTPUT_AXIS_ELEMENTS, int INPUT_AXIS_TILES,
          int OUTPUT_AXIS_TILES, int A_PORT_TILES, int B_PORT_TILES,
          int C_PORT_TILES, int C_BEAT_LAYOUT>
SC_MODULE(CIMProcessor) {
 public:
  static constexpr int K = rows;
  static constexpr int N = cols;

  using Array =
      CIMArray<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH, BASE_C_WIDTH,
               WRITE_CH_IN, MAC_LATENCY, MODE, A_WIDTH, B_WIDTH, SIGNED,
               TILE_INPUT_AXIS_ELEMENTS, TILE_OUTPUT_AXIS_ELEMENTS,
               INPUT_AXIS_TILES, OUTPUT_AXIS_TILES, A_PORT_TILES, B_PORT_TILES,
               C_PORT_TILES, C_BEAT_LAYOUT>;
  using ABeat = typename Array::ABeat;
  using CBeat = typename Array::CBeat;
  using Set = typename Array::Set;
  using MACRequest = typename Array::MACRequest;
  using WriteRequest = typename Array::WriteRequest;

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
  static_assert(rows == Array::K, "CIM K extent must match IC_DIMENSION");
  // InputController explicitly defines packing, boundary, and
  // replication-unroll rules for these extents
  static_assert(K == 4 || K == 8 || K == 16 || K == 32 || K == 64,
                "CIMProcessor currently requires an InputController-supported "
                "input extent");
  static_assert(cols == Array::N, "CIM N extent must match OC_DIMENSION");
  static_assert(buffer_size > 0,
                "CIM accumulation buffer depth must be positive");
  static_assert(INPUT_BUFFER_WIDTH == ABeat::width,
                "One input-buffer word must contain one complete CIM A beat");
  static_assert(
      WEIGHT_BUFFER_WIDTH == N * B_WIDTH,
      "One weight-buffer word must contain one complete logical B row");
  static_assert(Input::width == A_WIDTH,
                "CIM A width must match the configured input datatype");
  static_assert(Weight::width == B_WIDTH,
                "CIM B width must match the configured weight datatype");
  static_assert(Psum::width >= Array::C_WIDTH,
                "ACCUM_DATATYPE must hold one CIM result");

  sc_in<bool> CCS_INIT_S1(clk);
  sc_in<bool> CCS_INIT_S1(rstn);

  Connections::In<ac_int<INPUT_BUFFER_WIDTH, false>> CCS_INIT_S1(input_channel);
  Connections::In<ac_int<WEIGHT_BUFFER_WIDTH, false>> CCS_INIT_S1(
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

 private:
  Array CCS_INIT_S1(cim_array);
  Connections::Combinational<MACRequest> CCS_INIT_S1(mac_request_channel);
  Connections::Combinational<WriteRequest> CCS_INIT_S1(write_request_channel);
  Connections::Combinational<CBeat> CCS_INIT_S1(result_channel);

  // MatrixProcessor has separate accumulation/write-back parameter FIFOs; the
  // fused result thread needs one
  Connections::Fifo<MatrixParams, 1> CCS_INIT_S1(result_params_fifo);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(result_params_enq);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(result_params_deq);

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

    SC_THREAD(issue_operations);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(process_results);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
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

  // Replace MatrixProcessor::push_weights K loop and weight_skewer_din.Push
  // with direct weight-set writes
  void load_weight_set(Set wset) {
    for (int k = 0; k < K; k++) {
      const ac_int<WEIGHT_BUFFER_WIDTH, false> weights = weight_channel.Pop();
      for (int output_axis_tile_base = 0;
           output_axis_tile_base < OUTPUT_AXIS_TILES;
           output_axis_tile_base += B_PORT_TILES) {
        WriteRequest request;
        request.wset = wset;
        request.input_axis_idx = k / Array::TILE_K;
        request.output_axis_tile_base = output_axis_tile_base;
        request.wchi = k % Array::TILE_K;
        request.replicate = 0;
        clear_pack(request.data);

#pragma hls_unroll yes
        for (int port_tile_idx = 0; port_tile_idx < B_PORT_TILES;
             port_tile_idx++) {
#pragma hls_unroll yes
          for (int tile_n = 0; tile_n < Array::TILE_N; tile_n++) {
            const int n =
                (output_axis_tile_base + port_tile_idx) * Array::TILE_N +
                tile_n;
            request.data[port_tile_idx][0][tile_n] =
                weights.template slc<B_WIDTH>(n * B_WIDTH);
          }
        }
        write_request_channel.Push(request);
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
    weight_channel.Reset();
    result_params_enq.ResetWrite();
    mac_request_channel.ResetWrite();
    write_request_channel.ResetWrite();
    start.Reset();

    wait();

    Set next_wset = 0;
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
      Set active_wset = 0;

      for (ac_int<32, false> step = 0; step < total_ops; step++) {
        // Current controller loads at the demand boundary rather than
        // preloading after the prior block's final use A false load decision
        // keeps active_wset unchanged, so following inputs reuse its resident
        // weights A true decision overwrites next_wset and advances it only
        // after the complete block is loaded next_wset wraps after B_SETS load
        // events; each full load covers every output-axis B-port span The
        // MAC-request handshake blocks loop progress until the array commits
        // the current multicast issue With at least two sets, an intervening
        // multicast closes the prior issue window before round-robin wraparound
        // Future independent preloading still requires explicit per-set
        // liveness instead of this ordered schedule MatrixProcessor tags input
        // with swap_weights; CIM completes that weight load before issuing
        // input
        if (needs_weight_load(params, loop_counters, outer_reuse_indices,
                              reuse_weights, step)) {
          // Finish the complete weight set load before popping the mac request
          // and input data that use it
          load_weight_set(next_wset);
          active_wset = next_wset;
          next_wset =
              (next_wset == Set(B_SETS - 1)) ? Set(0) : Set(next_wset + Set(1));
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
        ac_int<Array::C_WIDTH, SIGNED> value;
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

    bool accumulation_buffer_bank = false;
    wait();

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

      const ac_int<32, false> total_ops = total_operations(params);
      for (ac_int<32, false> step = 0; step < total_ops; step++) {
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
    }
  }
};
