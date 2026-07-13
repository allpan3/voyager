#pragma once

#include <ac_int.h>
#include <mc_connections.h>
#include <systemc.h>

#include <type_traits>

#include "ArchitectureParams.h"
#include "CIMArray.h"
#include "Params.h"

// CIMProcessor replaces MatrixProcessor for the strict mapping where
// one controller word exactly spans the CIM input or output axis
template <typename InputTypeTuple, typename WeightTypeTuple, typename Input, typename Weight, typename Psum,
          typename Buffer, typename Scale, int rows, int cols, int buffer_size, int CH_IN, int CH_OUT, int B_SETS,
          int BASE_A_WIDTH, int BASE_B_WIDTH, int BASE_C_WIDTH, int WRITE_CH_IN, int MAC_LATENCY, int MODE, int A_WIDTH,
          int B_WIDTH, bool SIGNED, int TILE_INPUT_LANES, int TILE_OUTPUT_LANES, int REDUCTION_GROUPS,
          int MULTICAST_GROUPS, int A_PORT_TILES, int B_PORT_TILES, int C_PORT_TILES, int C_PORT_ORIENTATION>
SC_MODULE(CIMProcessor) {
 private:
  // Return the ceil log2 used for the cross-reduction-group result width
  static constexpr int log2_ceil(int value) { return (value <= 1) ? 0 : 1 + log2_ceil((value + 1) / 2); }

 public:
  using Array = CIMArray<CH_IN, CH_OUT, B_SETS, BASE_A_WIDTH, BASE_B_WIDTH, BASE_C_WIDTH, WRITE_CH_IN, MAC_LATENCY,
                         MODE, A_WIDTH, B_WIDTH, SIGNED, TILE_INPUT_LANES, TILE_OUTPUT_LANES, REDUCTION_GROUPS,
                         MULTICAST_GROUPS, A_PORT_TILES, B_PORT_TILES, C_PORT_TILES, C_PORT_ORIENTATION>;
  using ABeat = typename Array::ABeat;
  using BBeat = typename Array::BBeat;
  using CBeat = typename Array::CBeat;
  using ElementSet = typename Array::ElementSet;
  using MACRequest = typename Array::MACRequest;
  using StoreRequest = typename Array::StoreRequest;

  static constexpr int LOOP_WIDTH = 10;
  static constexpr int SPATIAL_C_WIDTH = Array::C_WIDTH + log2_ceil(REDUCTION_GROUPS);

#if DOUBLE_BUFFERED_ACCUM_BUFFER
  static constexpr int ACCUM_BUFFER_BANKS = 2;
#else
  static constexpr int ACCUM_BUFFER_BANKS = 1;
#endif

  static_assert(std::is_same<Input, DataTypes::int8>::value, "CIMProcessor v1 supports native INT8 input only");
  static_assert(std::is_same<Weight, DataTypes::int8>::value, "CIMProcessor v1 supports native INT8 weights only");
  static_assert(!SUPPORT_MX, "CIMProcessor v1 does not define microscaling across reduction groups");
  static_assert(!SUPPORT_CODEBOOK_QUANT, "CIMProcessor v1 does not decode codebook operands");
  static_assert(MODE == 0, "CIMProcessor v1 requires native bit-parallel CIM macros");
  static_assert(SIGNED, "CIMProcessor v1 requires signed native INT8 arithmetic");
  static_assert(C_PORT_ORIENTATION == CIM_C_PORT_MULTICAST_MAJOR, "CIMProcessor v1 requires multicast-major C beats");
  static_assert(WRITE_CH_IN == 1, "CIMProcessor v1 stores one CIM weight row per request");
  static_assert(B_SETS >= 2, "CIMProcessor v1 alternates at least two resident weight sets");
  static_assert(rows == Array::A_COLS, "CIM input scalar extent must match IC_DIMENSION");
  static_assert(rows == 4 || rows == 8 || rows == 16 || rows == 32 || rows == 64,
                "CIMProcessor v1 requires an InputController-supported input extent");
  static_assert(cols == Array::C_COLS, "CIM output scalar extent must match OC_DIMENSION");
  static_assert(INPUT_BUFFER_WIDTH == rows * A_WIDTH, "One input-buffer word must contain one complete CIM A beat");
  static_assert(WEIGHT_BUFFER_WIDTH == cols * B_WIDTH, "One weight-buffer word must contain one complete CIM B row");
  static_assert(Input::width == A_WIDTH, "CIM A width must match the configured input datatype");
  static_assert(Weight::width == B_WIDTH, "CIM B width must match the configured weight datatype");
  static_assert(Psum::width >= SPATIAL_C_WIDTH, "ACCUM_DATATYPE must hold the reduced CIM result");

  sc_in<bool> CCS_INIT_S1(clk);
  sc_in<bool> CCS_INIT_S1(rstn);

  Connections::In<ac_int<INPUT_BUFFER_WIDTH, false>> CCS_INIT_S1(input_channel);
  Connections::In<ac_int<WEIGHT_BUFFER_WIDTH, false>> CCS_INIT_S1(weight_channel);
  Connections::In<Pack1D<Buffer, cols>> CCS_INIT_S1(bias_channel);
  Connections::In<MatrixParams> CCS_INIT_S1(params_in);

  Connections::Out<Pack1D<Buffer, cols>> CCS_INIT_S1(output_channel);
  Connections::Out<ac_int<16, false>> accumulation_buffer_read_address[ACCUM_BUFFER_BANKS];
  Connections::In<Pack1D<Buffer, cols>> accumulation_buffer_read_data[ACCUM_BUFFER_BANKS];
  Connections::Out<BufferWriteRequest<Pack1D<Buffer, cols>>> accumulation_buffer_write_request[ACCUM_BUFFER_BANKS];

#if DOUBLE_BUFFERED_ACCUM_BUFFER
  Connections::SyncOut accumulation_buffer_done[ACCUM_BUFFER_BANKS];
#endif

#if SUPPORT_MX
  Connections::In<ac_int<Scale::width, false>> CCS_INIT_S1(input_scale_channel);
  Connections::In<ac_int<Scale::width * cols, false>> CCS_INIT_S1(weight_scale_channel);
#endif

  Connections::SyncOut CCS_INIT_S1(start);

 private:
  Array CCS_INIT_S1(cim_array);
  Connections::Combinational<MACRequest> CCS_INIT_S1(mac_request_channel);
  Connections::Combinational<ABeat> CCS_INIT_S1(a_channel);
  Connections::Combinational<StoreRequest> CCS_INIT_S1(store_channel);
  Connections::Combinational<CBeat> CCS_INIT_S1(result_channel);

  Connections::Fifo<MatrixParams, 1> CCS_INIT_S1(result_params_fifo);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(result_params_enq);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(result_params_deq);

 public:
  // Construct the array and the independent issue/result controllers
  SC_CTOR(CIMProcessor) {
    cim_array.clk(clk);
    cim_array.rstn(rstn);
    cim_array.mac_request_channel(mac_request_channel);
    cim_array.a_channel(a_channel);
    cim_array.store_channel(store_channel);
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
    return params.loops[0][0] * params.loops[0][1] * params.loops[0][2] * params.loops[0][3] * params.loops[0][4] *
           params.loops[1][0] * params.loops[1][1] * params.loops[1][2] * params.loops[1][3] * params.loops[1][4] *
           params.loops[1][5];
  }

  // Advance the shared two-level loop-counter representation by one operation
  static void advance_loop_counters(ac_int<LOOP_WIDTH, false> loop_counters[2][6], const MatrixParams &params) {
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

  // Return whether the scheduled operation keeps one resident weight block
  static bool reuses_weights(const MatrixParams &params) {
    const auto fy1 = params.loops[0][params.fy_loop_idx[0]];
    const auto c2 = params.loops[0][params.reduction_loop_idx[0]];
    const auto fy0 = params.loops[1][params.fy_loop_idx[1]];
    const auto fx = params.loops[1][params.fx_loop_idx];
    const auto c1 = params.loops[1][params.reduction_loop_idx[1]];
    const auto k1 = params.loops[1][params.weight_loop_idx[1]];
    const bool x_inner = params.weight_loop_idx[0] < params.x_loop_idx[0];
    const bool y_inner = params.weight_loop_idx[0] < params.y_loop_idx[0];
    return fy1 == 1 && c2 == 1 && fy0 == 1 && fx == 1 && c1 == 1 && k1 == 1 && (x_inner || y_inner);
  }

  // Select the outer-loop indices used by the existing weight-reuse schedule
  static void select_weight_reuse_indices(const MatrixParams &params, ac_int<3, false> indices[2]) {
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

  // Return whether this loop position selects a new resident weight block
  static bool swaps_weights(const MatrixParams &params, const ac_int<LOOP_WIDTH, false> loop_counters[2][6],
                            const ac_int<3, false> outer_reuse_indices[2], bool reuse_weights, ac_int<32, false> step) {
    const bool inner =
        loop_counters[1][params.weight_reuse_idx[0]] == 0 && loop_counters[1][params.weight_reuse_idx[1]] == 0;
    const bool outer = loop_counters[0][outer_reuse_indices[0]] == 0 && loop_counters[0][outer_reuse_indices[1]] == 0;
    return step == 0 || (inner && (!reuse_weights || outer));
  }

  // Store one complete controller weight block into a resident CIM set
  void load_weight_set(ElementSet set) {
    for (int row = 0; row < rows; row++) {
      const ac_int<WEIGHT_BUFFER_WIDTH, false> weights = weight_channel.Pop();
      StoreRequest request;
      request.set = set;
      request.lane = row / CH_IN;
      request.waddr = row % CH_IN;
      request.fanout = 0;
      clear_pack(request.data);

#pragma hls_unroll yes
      for (int output = 0; output < cols; output++) {
        const int output_lane = output / Array::ELEMENT_B_COLS;
        const int b_col = output % Array::ELEMENT_B_COLS;
        request.data[output_lane][b_col][0] = weights.template slc<B_WIDTH>(output * B_WIDTH);
      }
      store_channel.Push(request);
    }
  }

  // Pack one controller input word into the array's logical input-lane layout
  static ABeat pack_a_beat(const ac_int<INPUT_BUFFER_WIDTH, false> &inputs) {
    ABeat beat;
    clear_pack(beat);
#pragma hls_unroll yes
    for (int input = 0; input < rows; input++) {
      const int input_lane = input / CH_IN;
      const int a_col = input % CH_IN;
      beat[input_lane][a_col] = inputs.template slc<A_WIDTH>(input * A_WIDTH);
    }
    return beat;
  }

  // Load weights at swap points and issue broadcast CIM operations in loop
  // order
  void issue_operations() {
    params_in.Reset();
    input_channel.Reset();
    weight_channel.Reset();
    result_params_enq.ResetWrite();
    mac_request_channel.ResetWrite();
    a_channel.ResetWrite();
    store_channel.ResetWrite();
    start.Reset();

    wait();

    ElementSet next_set = 0;
    while (true) {
      const MatrixParams params = params_in.Pop();
      result_params_enq.Push(params);
      start.SyncPush();

#ifndef __SYNTHESIS__
      if (params.use_input_codebook || params.use_weight_codebook) {
        SC_REPORT_FATAL("CIMProcessor", "v1 does not support codebook operands");
      }
      if (params.is_mx_op || params.is_resnet_replication || params.is_generic_replication) {
        SC_REPORT_FATAL("CIMProcessor", "v1 does not support MX or replicated operands");
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
      ElementSet active_set = 0;

      for (ac_int<32, false> step = 0; step < total_ops; step++) {
        if (swaps_weights(params, loop_counters, outer_reuse_indices, reuse_weights, step)) {
          load_weight_set(next_set);
          active_set = next_set;
          next_set = (next_set == ElementSet(B_SETS - 1)) ? ElementSet(0) : ElementSet(next_set + ElementSet(1));
        }

        const ABeat beat = pack_a_beat(input_channel.Pop());
        MACRequest request;
        request.set = active_set;
        request.group = 0;
        request.bcast = 1;
        mac_request_channel.Push(request);
        a_channel.Push(beat);

        advance_loop_counters(loop_counters, params);
      }
    }
  }

  // Return the accumulation-buffer address for the current output position
  static ac_int<16, false> accumulation_address(const MatrixParams &params,
                                                const ac_int<LOOP_WIDTH, false> loop_counters[2][6]) {
    const ac_int<LOOP_WIDTH, false> y0 = params.loops[1][params.y_loop_idx[1]];
    const ac_int<LOOP_WIDTH, false> x0 = params.loops[1][params.x_loop_idx[1]];
    const ac_int<16, false> k_stride = y0 * x0;
    return loop_counters[1][params.weight_loop_idx[1]] * k_stride + loop_counters[1][params.y_loop_idx[1]] * x0 +
           loop_counters[1][params.x_loop_idx[1]];
  }

  // Return whether this partial starts a new temporal accumulation
  static bool starts_accumulation(const MatrixParams &params, const ac_int<LOOP_WIDTH, false> loop_counters[2][6]) {
    return loop_counters[0][params.reduction_loop_idx[0]] == 0 && loop_counters[1][params.reduction_loop_idx[1]] == 0 &&
           loop_counters[1][params.fx_loop_idx] == 0 && loop_counters[0][params.fy_loop_idx[0]] == 0 &&
           loop_counters[1][params.fy_loop_idx[1]] == 0;
  }

  // Return whether this partial completes the temporal accumulation
  static bool finishes_accumulation(const MatrixParams &params, const ac_int<LOOP_WIDTH, false> loop_counters[2][6]) {
    return loop_counters[0][params.reduction_loop_idx[0]] == params.loops[0][params.reduction_loop_idx[0]] - 1 &&
           loop_counters[1][params.reduction_loop_idx[1]] == params.loops[1][params.reduction_loop_idx[1]] - 1 &&
           loop_counters[1][params.fx_loop_idx] == params.loops[1][params.fx_loop_idx] - 1 &&
           loop_counters[0][params.fy_loop_idx[0]] == params.loops[0][params.fy_loop_idx[0]] - 1 &&
           loop_counters[1][params.fy_loop_idx[1]] == params.loops[1][params.fy_loop_idx[1]] - 1;
  }

  // Reduce one multicast-major sequence of R beats into one full output vector
  Pack1D<Psum, cols> reduce_result_beats() {
    ac_int<SPATIAL_C_WIDTH, SIGNED> spatial_sum[cols];
#pragma hls_unroll yes
    for (int output = 0; output < cols; output++) {
      spatial_sum[output] = 0;
    }

    for (int reduce = 0; reduce < REDUCTION_GROUPS; reduce++) {
      const CBeat beat = result_channel.Pop();
#pragma hls_unroll yes
      for (int group = 0; group < MULTICAST_GROUPS; group++) {
#pragma hls_unroll yes
        for (int output_lane = 0; output_lane < TILE_OUTPUT_LANES; output_lane++) {
#pragma hls_unroll yes
          for (int b_col = 0; b_col < Array::ELEMENT_B_COLS; b_col++) {
            const int output = (group * TILE_OUTPUT_LANES + output_lane) * Array::ELEMENT_B_COLS + b_col;
            ac_int<Array::C_WIDTH, SIGNED> tile_value;
            tile_value.set_slc(0, beat[group][output_lane][b_col]);
            spatial_sum[output] += tile_value;
          }
        }
      }
    }

    Pack1D<Psum, cols> reduced;
#pragma hls_unroll yes
    for (int output = 0; output < cols; output++) {
      reduced[output] = Psum(spatial_sum[output]);
    }
    return reduced;
  }

  // Collect, reduce, accumulate, and publish results in matrix-loop order
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

      Pack1D<Buffer, cols> bias = Pack1D<Buffer, cols>::zero();
      int bias_reuse_indices[4] = {5, 5, 5, 5};
      for (int loop = 5; loop > params.weight_loop_idx[1]; loop--) {
        bias_reuse_indices[5 - loop] = loop;
      }

      const ac_int<32, false> total_ops = total_operations(params);
      for (ac_int<32, false> step = 0; step < total_ops; step++) {
        const Pack1D<Psum, cols> reduced = reduce_result_beats();
        const bool first = starts_accumulation(params, loop_counters);
        const bool last = finishes_accumulation(params, loop_counters);
        Pack1D<Buffer, cols> previous = Pack1D<Buffer, cols>::zero();

        if (first) {
          if (params.has_bias) {
            const bool read_bias =
                loop_counters[1][bias_reuse_indices[0]] == 0 && loop_counters[1][bias_reuse_indices[1]] == 0 &&
                loop_counters[1][bias_reuse_indices[2]] == 0 && loop_counters[1][bias_reuse_indices[3]] == 0;
            if (read_bias) {
              bias = bias_channel.Pop();
            }
            previous = bias;
          }
        } else {
          const ac_int<16, false> address = accumulation_address(params, loop_counters);
          accumulation_buffer_read_address[accumulation_buffer_bank].Push(address);
          previous = accumulation_buffer_read_data[accumulation_buffer_bank].Pop();
        }

#pragma hls_unroll yes
        for (int output = 0; output < cols; output++) {
          previous[output] += static_cast<Buffer>(reduced[output]);
        }

        if (last && !(DOUBLE_BUFFERED_ACCUM_BUFFER && params.write_output_to_accum_buffer)) {
          output_channel.Push(previous);
        } else {
          BufferWriteRequest<Pack1D<Buffer, cols>> request;
          request.address = accumulation_address(params, loop_counters);
          request.data = previous;
          request.last = false;
          accumulation_buffer_write_request[accumulation_buffer_bank].Push(request);
        }

#if DOUBLE_BUFFERED_ACCUM_BUFFER
        if (params.write_output_to_accum_buffer) {
          const bool output_tile_completed =
              last && loop_counters[1][params.weight_loop_idx[1]] == params.loops[1][params.weight_loop_idx[1]] - 1 &&
              loop_counters[1][params.x_loop_idx[1]] == params.loops[1][params.x_loop_idx[1]] - 1 &&
              loop_counters[1][params.y_loop_idx[1]] == params.loops[1][params.y_loop_idx[1]] - 1;
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
