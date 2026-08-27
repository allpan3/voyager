#pragma once

#include <mc_connections.h>
#include <systemc.h>

#include "AccelTypes.h"
#include "ArchitectureParams.h"

template <typename WeightTypeTuple, typename Bias, int rows, int cols,
          int port_width, int buffer_width,
          int weight_channel_width = buffer_width, int B_SETS = CIM_B_SETS>
struct WeightController;

// Fetch, decode, and order weights for the selected matrix backend
//
// The CIM specialization also owns resident-set sequence policy: it removes
// contiguous output-dimension replays from the fetch traversal and declares
// their set count and lifetime to CIMProcessor
template <typename... WeightTypes, typename Bias, int rows, int cols,
          int port_width, int buffer_width, int weight_channel_width,
          int B_SETS>
struct WeightController<std::tuple<WeightTypes...>, Bias, rows, cols,
                        port_width, buffer_width, weight_channel_width, B_SETS>
    : public sc_module {
  static constexpr int LOOP_WIDTH = 10;
  static constexpr int LOOP_LEVEL_COUNT = 2;
  static constexpr int LOOP_SLOT_COUNT = 6;
  // Derive one scalar value's width from a cols-wide resident weight row
  static constexpr int DATA_WIDTH = buffer_width / cols;
#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
  // A transposed source row holds *rows* scalars and becomes one resident
  // column
  static constexpr int SOURCE_ROW_WIDTH = rows * DATA_WIDTH;
  static constexpr int WEIGHT_BEATS_PER_ROW =
      buffer_width / weight_channel_width;
  static_assert(rows == CIM_ARRAY_K_DIMENSION,
                "WeightController rows must match the CIM array K dimension");
  static_assert(cols == CIM_ARRAY_N_DIMENSION,
                "WeightController cols must match the CIM array N dimension");
  static_assert(B_SETS > 0,
                "WeightController requires a CIM resident weight set");
  static_assert(B_SETS <= 0xFFFF,
                "CIM resident set count exceeds schedule metadata");
  static_assert(buffer_width % weight_channel_width == 0,
                "One logical weight row must split into whole channel beats");
#else
  static_assert(weight_channel_width == buffer_width,
                "The systolic weight channel must carry one buffer row");
#endif
#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
  // packed_bits holds a cols-wide weight row or rows-wide transposed source row
  static constexpr int MAX_FETCH_WIDTH = std::max(
      {dtype_fetch_config<WeightTypes, rows, port_width>::max_fetch_width...,
       dtype_fetch_config<WeightTypes, cols, port_width>::max_fetch_width...});
#else
  // The systolic path fetches cols-wide rows for both normal and square tiles
  static constexpr int MAX_FETCH_WIDTH = std::max(
      {dtype_fetch_config<WeightTypes, cols, port_width>::max_fetch_width...});
#endif

  sc_in<bool> CCS_INIT_S1(clk);
  sc_in<bool> CCS_INIT_S1(rstn);

  Connections::Out<MemoryRequest> CCS_INIT_S1(weight_req);
  Connections::In<ac_int<port_width, false>> CCS_INIT_S1(weight_resp);

#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
  // Stream one physical weight-port beat using the weight_channel protocol
  Connections::Out<ac_int<weight_channel_width, false>> CCS_INIT_S1(
      weight_channel);
  Connections::Out<CIMWeightDescriptor> CCS_INIT_S1(weight_descriptor_channel);
#else
  Connections::Out<BufferWriteRequest<ac_int<buffer_width, false>>>
      write_request[2];
  Connections::Out<BufferReadRequest> read_request[2];
#endif

  Connections::Out<MemoryRequest> CCS_INIT_S1(bias_req);
  Connections::In<ac_int<port_width, false>> CCS_INIT_S1(bias_resp);
  Connections::Out<Pack1D<Bias, cols>> CCS_INIT_S1(bias_data);

  Connections::In<MatrixParams> CCS_INIT_S1(params_in);
#if MATRIX_BACKEND != MATRIX_BACKEND_CIM
  Connections::Combinational<MatrixParams> CCS_INIT_S1(fetcher_params);
#endif
  Connections::Combinational<MatrixParams> CCS_INIT_S1(writer_params);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(reader_params);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(weight_packer_params);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(transposer_params);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(bias_fetcher_params);
  Connections::Combinational<MatrixParams> CCS_INIT_S1(bias_feeder_params);

  // Independent end markers let the packer and transposer retire one fetch
  // stream without coupling their backpressure
  sc_fifo<bool> packer_stream_end;
  sc_fifo<bool> transposer_stream_end;

  // Carry one assembled memory fetch into the datatype unpacker
  Connections::Combinational<ac_int<MAX_FETCH_WIDTH, false>> packed_bits;
  // Carry one complete logical weight row after optional transposition
  Connections::Combinational<ac_int<buffer_width, false>> transpose_out;
#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
  // Queue one unpacking tag for each in-order response
  //
  // Two tags are live before steady response retirement begins
  // Connections::Fifo cannot replace an entry in a cycle that starts full, so
  // use two live-tag entries plus one replacement slot
  //
  // A two-entry FIFO therefore circulated one request bubble every five cycles
  // The third entry also retains a registered response-to-request boundary
  //
  // Connections::Fifo depth one is an II-one pipeline but throttles this
  // two-tag startup window A custom two-entry full-replacement queue avoids the
  // extra slot at the cost of bespoke control
  Connections::Fifo<ac_int<4, false>, 3> CCS_INIT_S1(packing_indices_fifo);
  Connections::Combinational<ac_int<4, false>> CCS_INIT_S1(packing_indices_enq);
  Connections::Combinational<ac_int<4, false>> CCS_INIT_S1(packing_indices_deq);
  // False announces one resident-set payload; true terminates the matrix job
  sc_fifo<bool> resident_set_stream_end;
#endif

  SC_CTOR(WeightController) {
#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
    packing_indices_fifo.clk(clk);
    packing_indices_fifo.rst(rstn);
    packing_indices_fifo.enq(packing_indices_enq);
    packing_indices_fifo.deq(packing_indices_deq);
#endif

    SC_THREAD(read_params);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

#if MATRIX_BACKEND != MATRIX_BACKEND_CIM
    SC_THREAD(fetcher);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
#endif

    SC_THREAD(reader);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(writer);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(transposer);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(weight_packer);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(bias_fetcher);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);

    SC_THREAD(bias_feeder);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
  }

#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
  // Zero-fill and slice logical weight rows into physical weight-port beats
  void writer() {
    writer_params.ResetRead();
    transpose_out.ResetRead();
    weight_channel.Reset();

    wait();

    while (true) {
      const MatrixParams params = writer_params.Pop();
      // The innermost IC extent is the number of source rows with real data
      const ac_int<LOOP_WIDTH, false> source_row_count =
          params.weight_addr_loops[1][params.weight_addr_reduction_loop_idx[2]];
      const ac_int<6, false> dtype_width =
          get_type_width<WeightTypes...>(params.weight_dtype);

      // Normal fetches may pack multiple rows, so recover one row's valid cols
      // The transposer always emits a complete cols-wide resident row
      ac_int<LOOP_WIDTH, false> valid_columns_per_row = cols;
      if (!params.weight_transpose) {
        // Convert burst bytes into the total fetched scalar count
        valid_columns_per_row = params.weight_burst_size * 8 / dtype_width;
        // Divide by the number of logical rows packed into the fetch
        valid_columns_per_row >>= params.weight_pack_factor_lg2;
        if (valid_columns_per_row > cols) valid_columns_per_row = cols;
      }

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      while (!resident_set_stream_end.read()) {
        for (int row = 0; row < rows; row++) {
          // Every resident set receives a complete rows-by-cols weight matrix
          ac_int<buffer_width, false> data = 0;
          if (params.weight_transpose || row < source_row_count) {
            const ac_int<buffer_width, false> fetched = transpose_out.Pop();
#pragma hls_unroll yes
            for (int col = 0; col < cols; col++) {
              if (col < valid_columns_per_row) {
                data.set_slc(col * DATA_WIDTH, fetched.template slc<DATA_WIDTH>(
                                                   col * DATA_WIDTH));
              }
            }
          }
          for (int beat = 0; beat < WEIGHT_BEATS_PER_ROW; beat++) {
            weight_channel.Push(data.template slc<weight_channel_width>(
                beat * weight_channel_width));
          }
        }
      }
    }
  }
#else
  void fetcher() {
    weight_req.Reset();
    fetcher_params.ResetRead();

    wait();

    while (true) {
      const MatrixParams params = fetcher_params.Pop();

      ac_int<LOOP_WIDTH, false> loop_counters[2][5];
      ac_int<LOOP_WIDTH, false> loop_bounds[2][5];

#pragma hls_unroll yes
      for (int i = 0; i < 2; i++) {
#pragma hls_unroll yes
        for (int j = 0; j < 5; j++) {
          loop_bounds[i][j] = params.weight_addr_loops[i][j] - 1;
        }
      }

      ac_int<LOOP_WIDTH, false> K2 =
          params.weight_addr_loops[0][params.weight_addr_weight_loop_idx[0]];
      ac_int<LOOP_WIDTH, false> C2 =
          params.weight_addr_loops[0][params.weight_addr_reduction_loop_idx[0]];
      ac_int<LOOP_WIDTH, false> C1 =
          params.weight_addr_loops[1][params.weight_addr_reduction_loop_idx[1]];
      ac_int<LOOP_WIDTH, false> C0 =
          params.weight_addr_loops[1][params.weight_addr_reduction_loop_idx[2]];
      ac_int<LOOP_WIDTH, false> FX =
          params.weight_addr_loops[1][params.weight_addr_fx_idx];
      ac_int<LOOP_WIDTH, false> FY0 =
          params.weight_addr_loops[1][params.weight_addr_fy_idx[1]];
      ac_int<LOOP_WIDTH, false> FY1 =
          params.weight_addr_loops[0][params.weight_addr_fy_idx[0]];
      ac_int<LOOP_WIDTH, false> K1 =
          params.weight_addr_loops[1][params.weight_addr_weight_loop_idx[1]];

      // reduce the number of iterations by packing factor
      K1 = K1 >> params.weight_pack_factor_lg2;
      loop_bounds[1][params.weight_addr_weight_loop_idx[1]] = K1 - 1;

      ac_int<16, false> k_stride = cols << params.weight_pack_factor_lg2;
      ac_int<24, false> c_stride = K2 * K1 * k_stride;
      ac_int<24, false> fx_stride = C2 * C1 * C0 * c_stride;
      ac_int<24, false> fy_stride = FX * fx_stride;

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      for (loop_counters[0][0] = 0;; loop_counters[0][0]++) {
        for (loop_counters[0][1] = 0;; loop_counters[0][1]++) {
          for (loop_counters[0][2] = 0;; loop_counters[0][2]++) {
            for (loop_counters[0][3] = 0;; loop_counters[0][3]++) {
              for (loop_counters[0][4] = 0;; loop_counters[0][4]++) {
                // inner memory
                for (loop_counters[1][0] = 0;; loop_counters[1][0]++) {
                  for (loop_counters[1][1] = 0;; loop_counters[1][1]++) {
                    for (loop_counters[1][2] = 0;; loop_counters[1][2]++) {
                      for (loop_counters[1][3] = 0;; loop_counters[1][3]++) {
                        for (loop_counters[1][4] = 0;; loop_counters[1][4]++) {
                          ac_int<LOOP_WIDTH, false> k2 = loop_counters
                              [0][params.weight_addr_weight_loop_idx[0]];
                          ac_int<LOOP_WIDTH, false> c2 = loop_counters
                              [0][params.weight_addr_reduction_loop_idx[0]];
                          ac_int<LOOP_WIDTH, false> c1 = loop_counters
                              [1][params.weight_addr_reduction_loop_idx[1]];
                          ac_int<LOOP_WIDTH, false> c0 = loop_counters
                              [1][params.weight_addr_reduction_loop_idx[2]];
                          ac_int<LOOP_WIDTH, false> fx =
                              loop_counters[1][params.weight_addr_fx_idx];
                          ac_int<LOOP_WIDTH, false> fy0 =
                              loop_counters[1][params.weight_addr_fy_idx[1]];
                          ac_int<LOOP_WIDTH, false> fy1 =
                              loop_counters[0][params.weight_addr_fy_idx[0]];
                          ac_int<LOOP_WIDTH, false> k1 = loop_counters
                              [1][params.weight_addr_weight_loop_idx[1]];

                          ac_int<16, false> k = (k2 * K1 + k1) * k_stride;
                          ac_int<16, false> c = (c2 * C1 + c1) * C0 + c0;
                          ac_int<16, false> fy = fy0 * FY1 + fy1;
                          ac_int<32, false> address = fy * fy_stride +
                                                      fx * fx_stride +
                                                      c * c_stride + k;

                          if (params.weight_transpose) {
                            address =
                                ((k + c0) * C2 * C1 + c2 * C1 + c1) * cols;
                          }

                          send_packed_request<WeightTypes...>(
                              params.weight_dtype, params.weight_offset,
                              address, params.weight_burst_size, weight_req);
                          packer_stream_end.write(false);
                          if (!params.weight_transpose) {
                            transposer_stream_end.write(false);
                          }

                          if (loop_counters[1][4] == loop_bounds[1][4]) break;
                        }
                        if (loop_counters[1][3] == loop_bounds[1][3]) break;
                      }
                      if (loop_counters[1][2] == loop_bounds[1][2]) break;
                    }
                    if (loop_counters[1][1] == loop_bounds[1][1]) break;
                  }
                  if (loop_counters[1][0] == loop_bounds[1][0]) break;
                }
                if (loop_counters[0][4] == loop_bounds[0][4]) break;
              }
              if (loop_counters[0][3] == loop_bounds[0][3]) break;
            }
            if (loop_counters[0][2] == loop_bounds[0][2]) break;
          }
          if (loop_counters[0][1] == loop_bounds[0][1]) break;
        }
        if (loop_counters[0][0] == loop_bounds[0][0]) break;
      }
      packer_stream_end.write(true);
      transposer_stream_end.write(true);
    }
  }

  void writer() {
    writer_params.ResetRead();
    transpose_out.ResetRead();
    write_request[0].Reset();
    write_request[1].Reset();

    bool bank_sel = 0;

    wait();

    while (true) {
      const MatrixParams params = writer_params.Pop();

      ac_int<LOOP_WIDTH, false> loop_counters[2][5];
      ac_int<LOOP_WIDTH, false> loop_bounds[2][5];

#pragma hls_unroll yes
      for (int i = 0; i < 2; i++) {
#pragma hls_unroll yes
        for (int j = 0; j < 5; j++) {
          loop_bounds[i][j] = params.weight_addr_loops[i][j] - 1;
        }
      }

      ac_int<LOOP_WIDTH, false> K2 =
          params.weight_addr_loops[0][params.weight_addr_weight_loop_idx[0]];
      ac_int<LOOP_WIDTH, false> C1 =
          params.weight_addr_loops[1][params.weight_addr_reduction_loop_idx[1]];
      ac_int<LOOP_WIDTH, false> C0 =
          params.weight_addr_loops[1][params.weight_addr_reduction_loop_idx[2]];
      ac_int<LOOP_WIDTH, false> FX =
          params.weight_addr_loops[1][params.weight_addr_fx_idx];
      ac_int<LOOP_WIDTH, false> FY0 =
          params.weight_addr_loops[1][params.weight_addr_fy_idx[1]];
      ac_int<LOOP_WIDTH, false> K1 =
          params.weight_addr_loops[1][params.weight_addr_weight_loop_idx[1]];

      // reduce the number of iterations by packing factor
      K1 = K1 >> params.weight_pack_factor_lg2;
      loop_bounds[1][params.weight_addr_weight_loop_idx[1]] = K1 - 1;
      ac_int<4, false> pack_offset_bound =
          (1 << params.weight_pack_factor_lg2) - 1;

      ac_int<24, false> fx_stride = C1 * C0 * K1;
      ac_int<24, false> fy_stride = FX * fx_stride;

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      for (loop_counters[0][0] = 0;; loop_counters[0][0]++) {
        for (loop_counters[0][1] = 0;; loop_counters[0][1]++) {
          for (loop_counters[0][2] = 0;; loop_counters[0][2]++) {
            for (loop_counters[0][3] = 0;; loop_counters[0][3]++) {
              for (loop_counters[0][4] = 0;; loop_counters[0][4]++) {
                for (loop_counters[1][0] = 0;; loop_counters[1][0]++) {
                  for (loop_counters[1][1] = 0;; loop_counters[1][1]++) {
                    for (loop_counters[1][2] = 0;; loop_counters[1][2]++) {
                      for (loop_counters[1][3] = 0;; loop_counters[1][3]++) {
                        for (loop_counters[1][4] = 0;; loop_counters[1][4]++) {
                          for (ac_int<4, false> pack = 0;; pack++) {
                            ac_int<LOOP_WIDTH, false> k2 = loop_counters
                                [0][params.weight_addr_weight_loop_idx[0]];
                            ac_int<LOOP_WIDTH, false> c1 = loop_counters
                                [1][params.weight_addr_reduction_loop_idx[1]];
                            ac_int<LOOP_WIDTH, false> c0 = loop_counters
                                [1][params.weight_addr_reduction_loop_idx[2]];
                            ac_int<LOOP_WIDTH, false> fx =
                                loop_counters[1][params.weight_addr_fx_idx];
                            ac_int<LOOP_WIDTH, false> fy0 =
                                loop_counters[1][params.weight_addr_fy_idx[1]];
                            ac_int<LOOP_WIDTH, false> k1 = loop_counters
                                [1][params.weight_addr_weight_loop_idx[1]];

                            ac_int<buffer_width, false> data =
                                transpose_out.Pop();

                            ac_int<16, false> c = c1 * C0 + c0;
                            ac_int<16, false> address =
                                fy0 * fy_stride + fx * fx_stride + c * K1 + k1;
                            address =
                                (address << params.weight_pack_factor_lg2) +
                                pack;

                            const bool last_word =
                                loop_counters[1][4] == loop_bounds[1][4] &&
                                loop_counters[1][3] == loop_bounds[1][3] &&
                                loop_counters[1][2] == loop_bounds[1][2] &&
                                loop_counters[1][1] == loop_bounds[1][1] &&
                                loop_counters[1][0] == loop_bounds[1][0] &&
                                pack == pack_offset_bound;

                            BufferWriteRequest<ac_int<buffer_width, false>> req;
                            req.address = address;
                            req.data = data;
                            req.last = last_word;
                            write_request[bank_sel].Push(req);

                            if (pack == pack_offset_bound) break;
                          }
                          if (loop_counters[1][4] == loop_bounds[1][4]) break;
                        }
                        if (loop_counters[1][3] == loop_bounds[1][3]) break;
                      }
                      if (loop_counters[1][2] == loop_bounds[1][2]) break;
                    }
                    if (loop_counters[1][1] == loop_bounds[1][1]) break;
                  }
                  if (loop_counters[1][0] == loop_bounds[1][0]) break;
                }
                bank_sel = !bank_sel;
                if (loop_counters[0][4] == loop_bounds[0][4]) break;
              }
              if (loop_counters[0][3] == loop_bounds[0][3]) break;
            }
            if (loop_counters[0][2] == loop_bounds[0][2]) break;
          }
          if (loop_counters[0][1] == loop_bounds[0][1]) break;
        }
        if (loop_counters[0][0] == loop_bounds[0][0]) break;
      }
    }
  }
#endif

#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
  // Describe source B[FY][FX][IC][OC] extents and flattened memory strides
  struct CIMWeightTensorLayout {
    ac_int<LOOP_WIDTH, false> outer_oc_bound;
    ac_int<LOOP_WIDTH, false> outer_ic_bound;
    ac_int<LOOP_WIDTH, false> inner_ic_bound;
    ac_int<LOOP_WIDTH, false> rows_per_inner_ic;
    ac_int<LOOP_WIDTH, false> fx_bound;
    ac_int<LOOP_WIDTH, false> outer_fy_bound;
    ac_int<LOOP_WIDTH, false> packed_inner_oc_bound;
    ac_int<16, false> oc_tile_stride;
    ac_int<24, false> ic_stride;
    ac_int<24, false> fx_stride;
    ac_int<24, false> fy_stride;
  };

  // Name one B-tensor coordinate decoded from the physical reader loop nest
  struct CIMWeightTensorCoordinate {
    ac_int<LOOP_WIDTH, false> outer_oc;
    ac_int<LOOP_WIDTH, false> outer_ic;
    ac_int<LOOP_WIDTH, false> inner_ic;
    ac_int<LOOP_WIDTH, false> fx;
    ac_int<LOOP_WIDTH, false> inner_fy;
    ac_int<LOOP_WIDTH, false> outer_fy;
    ac_int<LOOP_WIDTH, false> packed_inner_oc;
    ac_int<4, false> packing_index;
  };

  // Decode legacy weight-address metadata into the source B-tensor layout
  static CIMWeightTensorLayout cim_weight_tensor_layout(
      const MatrixParams &params) {
    CIMWeightTensorLayout layout;
    layout.outer_oc_bound =
        params.weight_addr_loops[0][params.weight_addr_weight_loop_idx[0]];
    layout.outer_ic_bound =
        params.weight_addr_loops[0][params.weight_addr_reduction_loop_idx[0]];
    layout.inner_ic_bound =
        params.weight_addr_loops[1][params.weight_addr_reduction_loop_idx[1]];
    layout.rows_per_inner_ic =
        params.weight_addr_loops[1][params.weight_addr_reduction_loop_idx[2]];
    layout.fx_bound = params.weight_addr_loops[1][params.weight_addr_fx_idx];
    layout.outer_fy_bound =
        params.weight_addr_loops[0][params.weight_addr_fy_idx[0]];
    layout.packed_inner_oc_bound =
        params.weight_addr_loops[1][params.weight_addr_weight_loop_idx[1]] >>
        params.weight_pack_factor_lg2;

    layout.oc_tile_stride = cols << params.weight_pack_factor_lg2;
    layout.ic_stride = layout.outer_oc_bound * layout.packed_inner_oc_bound *
                       layout.oc_tile_stride;
    layout.fx_stride = layout.outer_ic_bound * layout.inner_ic_bound *
                       layout.rows_per_inner_ic * layout.ic_stride;
    layout.fy_stride = layout.fx_bound * layout.fx_stride;
    return layout;
  }

  // Read one source B-tensor coordinate from the physical loop counters
  static CIMWeightTensorCoordinate cim_weight_tensor_coordinate(
      const MatrixParams &params,
      const ac_int<LOOP_WIDTH, false> loop_counters[2][6]) {
    CIMWeightTensorCoordinate coordinate;
    coordinate.outer_oc = loop_counters[0][params.weight_loop_idx[0]];
    coordinate.outer_ic = loop_counters[0][params.reduction_loop_idx[0]];
    coordinate.inner_ic = loop_counters[1][params.reduction_loop_idx[1]];
    coordinate.fx = loop_counters[1][params.fx_loop_idx];
    coordinate.inner_fy = loop_counters[1][params.fy_loop_idx[1]];
    coordinate.outer_fy = loop_counters[0][params.fy_loop_idx[0]];
    const ac_int<LOOP_WIDTH, false> inner_oc =
        loop_counters[1][params.weight_loop_idx[1]];
    coordinate.packed_inner_oc = inner_oc >> params.weight_pack_factor_lg2;
    coordinate.packing_index = inner_oc - (coordinate.packed_inner_oc
                                           << params.weight_pack_factor_lg2);
    return coordinate;
  }

  // Flatten one B-tensor coordinate into its packed source-memory address
  static ac_int<32, false> cim_weight_source_address(
      const MatrixParams &params, const CIMWeightTensorLayout &layout,
      const CIMWeightTensorCoordinate &coordinate, int row) {
    const ac_int<16, false> oc_offset =
        (coordinate.outer_oc * layout.packed_inner_oc_bound +
         coordinate.packed_inner_oc) *
        layout.oc_tile_stride;
    if (params.weight_transpose) {
      // Rows select output and columns select reduction in transposed storage
      return ((oc_offset + row) * layout.outer_ic_bound *
                  layout.inner_ic_bound +
              coordinate.outer_ic * layout.inner_ic_bound +
              coordinate.inner_ic) *
             rows;
    }

    const ac_int<16, false> ic_offset =
        (coordinate.outer_ic * layout.inner_ic_bound + coordinate.inner_ic) *
            layout.rows_per_inner_ic +
        row;
    const ac_int<16, false> fy =
        coordinate.inner_fy * layout.outer_fy_bound + coordinate.outer_fy;
    return fy * layout.fy_stride + coordinate.fx * layout.fx_stride +
           ic_offset * layout.ic_stride + oc_offset;
  }

  // Return whether the physical L1 traversal begins its weight sequence
  static bool cim_starts_l1_sequence(
      const ac_int<LOOP_WIDTH, false> inner_loop_counters[6]) {
    bool starts = true;
#pragma hls_unroll yes
    for (int slot = 0; slot < LOOP_SLOT_COUNT; slot++) {
      starts = starts && inner_loop_counters[slot] == 0;
    }
    return starts;
  }

  // Count sets in the L1 weight sequence, capped one past physical capacity
  static ac_int<32, false> cim_l1_set_count(
      const ac_int<LOOP_WIDTH, false> inner_loop_bounds[6]) {
    ac_int<32, false> set_count = 1;
#pragma hls_unroll yes
    for (int slot = 0; slot < LOOP_SLOT_COUNT; slot++) {
      const ac_int<32, false> next_set_count =
          set_count * inner_loop_bounds[slot];
      set_count = next_set_count > B_SETS ? ac_int<32, false>(B_SETS + 1)
                                          : next_set_count;
    }
    return set_count;
  }

  // Identify an L1 spatial loop that repeats the complete inner weight sequence
  static bool cim_l1_spatial_replays_sequence(const MatrixParams &params,
                                              int spatial_loop_idx) {
    const auto l1_oc = params.loops[1][params.weight_loop_idx[1]];
    const auto l1_ic = params.loops[1][params.reduction_loop_idx[1]];
    const auto l1_fy = params.loops[1][params.fy_loop_idx[1]];
    const auto l1_fx = params.loops[1][params.fx_loop_idx];
    return params.loops[1][spatial_loop_idx] > 1 &&
           (l1_oc == 1 || params.weight_loop_idx[1] > spatial_loop_idx) &&
           (l1_ic == 1 || params.reduction_loop_idx[1] > spatial_loop_idx) &&
           (l1_fy == 1 || params.fy_loop_idx[1] > spatial_loop_idx) &&
           (l1_fx == 1 || params.fx_loop_idx > spatial_loop_idx);
  }
#endif

  // Own weight-order policy and emit backend-specific weight uses
  //
  // Innermost L1 OX/OY loops keep one selected weight. Outer L1 or eligible L2
  // OX/OY loops replay a fitting L1 sequence. CIM descriptors carry that
  // repetition to CIMProcessor, while the systolic path follows
  // MatrixProcessor's legacy swap protocol
  void reader() {
    reader_params.ResetRead();

#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
    weight_req.Reset();
    packing_indices_enq.ResetWrite();
    weight_descriptor_channel.Reset();
#else
    read_request[0].Reset();
    read_request[1].Reset();

    bool bank_sel = 0;
#endif

    wait();

    while (true) {
      const MatrixParams params = reader_params.Pop();

      ac_int<LOOP_WIDTH, false> loop_counters[2][6];
      ac_int<LOOP_WIDTH, false> loop_bounds[2][6];

#pragma hls_unroll yes
      for (int level = 0; level < LOOP_LEVEL_COUNT; level++) {
#pragma hls_unroll yes
        for (int slot = 0; slot < LOOP_SLOT_COUNT; slot++) {
          loop_bounds[level][slot] = params.loops[level][slot];
        }
      }

      const auto l1_oc = params.loops[1][params.weight_loop_idx[1]];
      const auto l1_ic = params.loops[1][params.reduction_loop_idx[1]];
      const auto l1_fy = params.loops[1][params.fy_loop_idx[1]];
      const auto l1_fx = params.loops[1][params.fx_loop_idx];
#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
      const bool l1_ox_reuses_weights =
          (l1_oc == 1 || params.weight_loop_idx[1] < params.x_loop_idx[1]) &&
          (l1_ic == 1 || params.reduction_loop_idx[1] < params.x_loop_idx[1]) &&
          (l1_fy == 1 || params.fy_loop_idx[1] < params.x_loop_idx[1]) &&
          (l1_fx == 1 || params.fx_loop_idx < params.x_loop_idx[1]);
      const bool l1_oy_reuses_weights =
          (l1_oc == 1 || params.weight_loop_idx[1] < params.y_loop_idx[1]) &&
          (l1_ic == 1 || params.reduction_loop_idx[1] < params.y_loop_idx[1]) &&
          (l1_fy == 1 || params.fy_loop_idx[1] < params.y_loop_idx[1]) &&
          (l1_fx == 1 || params.fx_loop_idx < params.y_loop_idx[1]);
      if (l1_ox_reuses_weights) {
        loop_bounds[1][params.x_loop_idx[1]] = 1;
      }
      if (l1_oy_reuses_weights) {
        loop_bounds[1][params.y_loop_idx[1]] = 1;
      }

      const bool l1_ox_replays_sequence =
          !l1_ox_reuses_weights &&
          cim_l1_spatial_replays_sequence(params, params.x_loop_idx[1]);
      const bool l1_oy_replays_sequence =
          !l1_oy_reuses_weights &&
          cim_l1_spatial_replays_sequence(params, params.y_loop_idx[1]);
      if (l1_ox_replays_sequence) {
        loop_bounds[1][params.x_loop_idx[1]] = 1;
      }
      if (l1_oy_replays_sequence) {
        loop_bounds[1][params.y_loop_idx[1]] = 1;
      }
      const bool l1_group_replay_requested =
          l1_ox_replays_sequence || l1_oy_replays_sequence;
      const bool l1_group_replay_fits =
          cim_l1_set_count(loop_bounds[1]) <= B_SETS;
      const bool l1_group_replay_enabled =
          l1_group_replay_requested && l1_group_replay_fits;
      if (l1_group_replay_requested && !l1_group_replay_fits) {
        if (l1_ox_replays_sequence) {
          loop_bounds[1][params.x_loop_idx[1]] =
              params.loops[1][params.x_loop_idx[1]];
        }
        if (l1_oy_replays_sequence) {
          loop_bounds[1][params.y_loop_idx[1]] =
              params.loops[1][params.y_loop_idx[1]];
        }
      }
#else
      // Use the compiler-selected SA collapse slots across synthesized blocks
      loop_bounds[1][params.weight_reuse_idx[0]] = 1;
      loop_bounds[1][params.weight_reuse_idx[1]] = 1;
#endif

      // Add replay only when a transposed tile spans multiple array rows
      int transpose_replay_count = 1;
#if MATRIX_BACKEND != MATRIX_BACKEND_CIM
      ac_int<LOOP_WIDTH, false> C2 =
          params.loops[0][params.reduction_loop_idx[0]];
      ac_int<LOOP_WIDTH, false> FY0 = l1_fy;
      ac_int<LOOP_WIDTH, false> FX = l1_fx;
      ac_int<LOOP_WIDTH, false> C1 = l1_ic;
      ac_int<LOOP_WIDTH, false> K1 = l1_oc;
      constexpr int ratio = cols > rows ? cols / rows : 1;
      if (ratio > 1 && params.weight_transpose && C2 >= ratio) {
        // we can reuse the weights already in the buffer
        loop_bounds[0][params.reduction_loop_idx[0]] = C2 / ratio;
        transpose_replay_count = ratio;
      }
#endif

      const auto l2_oc = params.loops[0][params.weight_loop_idx[0]];
      const auto l2_ic = params.loops[0][params.reduction_loop_idx[0]];
      const auto l2_fy = params.loops[0][params.fy_loop_idx[0]];
#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
      // Remove only output-dimension loops that repeat the L1 weight sequence
      const bool l2_ox_reuses_weights =
          (l2_oc == 1 || params.weight_loop_idx[0] < params.x_loop_idx[0]) &&
          (l2_ic == 1 || params.reduction_loop_idx[0] < params.x_loop_idx[0]) &&
          (l2_fy == 1 || params.fy_loop_idx[0] < params.x_loop_idx[0]);
      const bool l2_oy_reuses_weights =
          (l2_oc == 1 || params.weight_loop_idx[0] < params.y_loop_idx[0]) &&
          (l2_ic == 1 || params.reduction_loop_idx[0] < params.y_loop_idx[0]) &&
          (l2_fy == 1 || params.fy_loop_idx[0] < params.y_loop_idx[0]);
      const bool omit_outer_ox_from_weight_reader = l2_ox_reuses_weights;
      const bool omit_outer_oy_from_weight_reader = l2_oy_reuses_weights;
      ac_int<16, false> compute_sequence_replay_count = 1;
      if (l1_group_replay_enabled && l1_ox_replays_sequence) {
        compute_sequence_replay_count *= params.loops[1][params.x_loop_idx[1]];
      }
      if (l1_group_replay_enabled && l1_oy_replays_sequence) {
        compute_sequence_replay_count *= params.loops[1][params.y_loop_idx[1]];
      }
      if (omit_outer_ox_from_weight_reader) {
        loop_bounds[0][params.x_loop_idx[0]] = 1;
        compute_sequence_replay_count *= params.loops[0][params.x_loop_idx[0]];
      }
      if (omit_outer_oy_from_weight_reader) {
        loop_bounds[0][params.y_loop_idx[0]] = 1;
        compute_sequence_replay_count *= params.loops[0][params.y_loop_idx[0]];
      }

      // A fitting sequence is fetched once and retained for every descriptor
      // replay. An oversized sequence streams singleton descriptors and must be
      // fetched again for every replay
      const ac_int<32, false> l1_sequence_set_count =
          cim_l1_set_count(loop_bounds[1]);
      const bool l1_sequence_fits = l1_sequence_set_count <= B_SETS;
      const ac_int<16, false> fetch_sequence_replay_count =
          l1_sequence_fits ? ac_int<16, false>(1)
                           : compute_sequence_replay_count;
      const CIMWeightTensorLayout weight_layout =
          cim_weight_tensor_layout(params);
#else
      const bool reuse_weights = l2_fy == 1 && l2_ic == 1 && l1_fy == 1 &&
                                 l1_fx == 1 && l1_ic == 1 && l1_oc == 1;

      // Restore the serialized SA absorb/replay contract
      const bool absorb_x =
          (!reuse_weights ||
           params.weight_loop_idx[0] < params.x_loop_idx[0]) &&
          (l2_oc == 1 || params.weight_loop_idx[0] < params.x_loop_idx[0]) &&
          (l2_ic == 1 || params.reduction_loop_idx[0] < params.x_loop_idx[0]) &&
          (l2_fy == 1 || params.fy_loop_idx[0] < params.x_loop_idx[0]);
      const bool absorb_y =
          (!reuse_weights ||
           params.weight_loop_idx[0] < params.y_loop_idx[0]) &&
          (l2_oc == 1 || params.weight_loop_idx[0] < params.y_loop_idx[0]) &&
          (l2_ic == 1 || params.reduction_loop_idx[0] < params.y_loop_idx[0]) &&
          (l2_fy == 1 || params.fy_loop_idx[0] < params.y_loop_idx[0]);
      ac_int<16, false> spatial_reuse_bound = 1;
      if (absorb_x) {
        if (!reuse_weights) {
          spatial_reuse_bound = params.loops[0][params.x_loop_idx[0]];
        }
        loop_bounds[0][params.x_loop_idx[0]] = 1;
      }
      if (absorb_y) {
        if (!reuse_weights) {
          spatial_reuse_bound *= params.loops[0][params.y_loop_idx[0]];
        }
        loop_bounds[0][params.y_loop_idx[0]] = 1;
      }

      const ac_int<16, false> fetch_sequence_replay_count = spatial_reuse_bound;
      ac_int<16, false> fx_stride = rows * C1 * K1;
      ac_int<16, false> fy_stride = FX * fx_stride;
      ac_int<16, false> fx_stride_with_replay =
          fx_stride * transpose_replay_count;
      ac_int<16, false> fy_stride_with_replay =
          fy_stride * transpose_replay_count;
#endif

      // Preserve the established twelve-loop HLS nest and II; the sixth L2
      // slot is fixed-unit FX and therefore has no physical loop here
#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      for (loop_counters[0][0] = 0;; loop_counters[0][0]++) {
        for (loop_counters[0][1] = 0;; loop_counters[0][1]++) {
          for (loop_counters[0][2] = 0;; loop_counters[0][2]++) {
            for (loop_counters[0][3] = 0;; loop_counters[0][3]++) {
              for (loop_counters[0][4] = 0;; loop_counters[0][4]++) {
                for (ac_int<16, false> fetch_replay_index = 0;;
                     fetch_replay_index++) {
                  for (int transpose_replay = 0;
                       transpose_replay < transpose_replay_count;
                       transpose_replay++) {
                    for (loop_counters[1][0] = 0;; loop_counters[1][0]++) {
                      for (loop_counters[1][1] = 0;; loop_counters[1][1]++) {
                        for (loop_counters[1][2] = 0;; loop_counters[1][2]++) {
                          for (loop_counters[1][3] = 0;;
                               loop_counters[1][3]++) {
                            for (loop_counters[1][4] = 0;;
                                 loop_counters[1][4]++) {
                              for (loop_counters[1][5] = 0;;
                                   loop_counters[1][5]++) {
#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
                                const bool starts_l1_sequence =
                                    cim_starts_l1_sequence(loop_counters[1]);
                                if (!l1_sequence_fits || starts_l1_sequence) {
                                  CIMWeightDescriptor descriptor;
                                  if (l1_sequence_fits) {
                                    descriptor.set_count =
                                        l1_sequence_set_count;
                                    descriptor.replay_count =
                                        compute_sequence_replay_count;
                                  } else {
                                    descriptor.set_count = 1;
                                    descriptor.replay_count = 1;
                                  }
                                  weight_descriptor_channel.Push(descriptor);
                                }

                                const CIMWeightTensorCoordinate
                                    weight_coordinate =
                                        cim_weight_tensor_coordinate(
                                            params, loop_counters);

                                resident_set_stream_end.write(false);
                                // Fetch one source row per resident weight row
                                // Transposition instead fetches one per column
                                for (int row = 0;
                                     row < (rows > cols ? rows : cols); row++) {
                                  const bool active_row =
                                      params.weight_transpose ? row < cols
                                                              : row < rows;
                                  if (active_row &&
                                      row < weight_layout.rows_per_inner_ic) {
                                    const ac_int<32, false> address =
                                        cim_weight_source_address(
                                            params, weight_layout,
                                            weight_coordinate, row);
                                    send_packed_request<WeightTypes...>(
                                        params.weight_dtype,
                                        params.weight_offset, address,
                                        params.weight_burst_size, weight_req);
                                    packing_indices_enq.Push(
                                        weight_coordinate.packing_index);
                                    packer_stream_end.write(false);
                                    transposer_stream_end.write(false);
                                  }
                                }
#else
                                ac_int<LOOP_WIDTH, false> c1 =
                                    loop_counters[1]
                                                 [params.reduction_loop_idx[1]];
                                ac_int<LOOP_WIDTH, false> fy0 =
                                    loop_counters[1][params.fy_loop_idx[1]];
                                ac_int<LOOP_WIDTH, false> fx =
                                    loop_counters[1][params.fx_loop_idx];
                                ac_int<LOOP_WIDTH, false> k1 =
                                    loop_counters[1][params.weight_loop_idx[1]];

                                /*
                                 * If we have replication, then need to zero
                                 * pad the unused rows For 7x7 filter, we
                                 * split it into 4 filters and 3 filters
                                 */
                                ac_int<4, false> replication_bound = 1;
                                ac_int<8, false> c_end = rows;
                                if (params.is_resnet_replication) {
                                  c_end = 3;
                                  if (rows == 4) {
                                    replication_bound = 1;
                                  } else if (rows == 8) {
                                    // last iteration only unrolls 1 fx
                                    replication_bound = fx == 3 ? 1 : 2;
                                  } else if (rows == 16) {
                                    replication_bound = fx == 0 ? 4 : 3;
                                  } else if (rows == 32 || rows == 64) {
                                    replication_bound = 7;
                                  }
                                } else if (params.is_generic_replication) {
                                  c_end = params.num_channels;
                                  replication_bound =
                                      1 << params.fx_unrolling_lg2;
                                }

                                ac_int<LOOP_WIDTH, false> fx_repl = 0;
                                ac_int<LOOP_WIDTH, false> c = 0;
                                for (int row = 0; row < rows; row++) {
                                  ac_int<LOOP_WIDTH, false> C = rows * C1;
                                  ac_int<LOOP_WIDTH, false> C0 = rows;
                                  ac_int<16, false> address;

                                  if (params.is_resnet_replication ||
                                      params.is_generic_replication) {
                                    ac_int<LOOP_WIDTH, false> cur_fx = fx;
                                    if (params.is_resnet_replication) {
                                      C = 3;
                                      C0 = 3;
                                      FX = 7;
                                      if (rows == 4) {
                                        cur_fx = fx;
                                      } else if (rows == 8) {
                                        cur_fx = fx * 2 + fx_repl;
                                      } else if (rows == 16) {
                                        cur_fx = fx * 4 + fx_repl;
                                      } else if (rows == 32 || rows == 64) {
                                        cur_fx = fx_repl;
                                      }
                                    } else {
                                      C = params.num_channels;
                                      C0 = params.num_channels;
                                      FX = l1_fx * replication_bound;
                                      cur_fx = fx * replication_bound + fx_repl;
                                    }

                                    if (fx_repl < replication_bound) {
                                      address = fy0 * FX * C * K1 +
                                                cur_fx * C * K1 + c * K1 + k1;
                                    } else {
                                      address = 0xFFFF;
                                    }

                                    // keep track of which C and FX we are on
                                    if (c < c_end - 1) {
                                      c++;
                                    } else {
                                      c = 0;
                                      fx_repl++;
                                    }
                                  } else {
                                    c = row;
                                    if (params.weight_transpose &&
                                        cols > rows) {
                                      address =
                                          fy0 * fy_stride_with_replay +
                                          fx * fx_stride_with_replay +
                                          ((c + transpose_replay * rows) +
                                           c1 * C0 * transpose_replay_count) *
                                              K1 +
                                          k1;
                                    } else {
                                      address = fy0 * fy_stride +
                                                fx * fx_stride +
                                                (c + c1 * C0) * K1 + k1;
                                    }
                                  }

                                  BufferReadRequest req;
                                  req.address = address;
                                  req.last =
                                      row == rows - 1 &&
                                      loop_counters[1][5] ==
                                          loop_bounds[1][5] - 1 &&
                                      loop_counters[1][4] ==
                                          loop_bounds[1][4] - 1 &&
                                      loop_counters[1][3] ==
                                          loop_bounds[1][3] - 1 &&
                                      loop_counters[1][2] ==
                                          loop_bounds[1][2] - 1 &&
                                      loop_counters[1][1] ==
                                          loop_bounds[1][1] - 1 &&
                                      loop_counters[1][0] ==
                                          loop_bounds[1][0] - 1 &&
                                      fetch_replay_index ==
                                          fetch_sequence_replay_count - 1 &&
                                      transpose_replay ==
                                          transpose_replay_count - 1;
                                  read_request[bank_sel].Push(req);
                                }
#endif

                                if (loop_counters[1][5] ==
                                    loop_bounds[1][5] - 1)
                                  break;
                              }
                              if (loop_counters[1][4] == loop_bounds[1][4] - 1)
                                break;
                            }
                            if (loop_counters[1][3] == loop_bounds[1][3] - 1)
                              break;
                          }
                          if (loop_counters[1][2] == loop_bounds[1][2] - 1)
                            break;
                        }
                        if (loop_counters[1][1] == loop_bounds[1][1] - 1) break;
                      }
                      if (loop_counters[1][0] == loop_bounds[1][0] - 1) break;
                    }
                    if (transpose_replay == transpose_replay_count - 1) break;
                  }
                  if (fetch_replay_index == fetch_sequence_replay_count - 1)
                    break;
                }
#if MATRIX_BACKEND != MATRIX_BACKEND_CIM
                bank_sel = !bank_sel;
#endif
                if (loop_counters[0][4] == loop_bounds[0][4] - 1) break;
              }
              if (loop_counters[0][3] == loop_bounds[0][3] - 1) break;
            }
            if (loop_counters[0][2] == loop_bounds[0][2] - 1) break;
          }
          if (loop_counters[0][1] == loop_bounds[0][1] - 1) break;
        }
        if (loop_counters[0][0] == loop_bounds[0][0] - 1) break;
      }
#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
      resident_set_stream_end.write(true);
      packer_stream_end.write(true);
      transposer_stream_end.write(true);
#endif
    }
  }

  // Assemble one logical source fetch from its memory-response beats
  void weight_packer() {
    weight_packer_params.ResetRead();
    weight_resp.Reset();
    packed_bits.ResetWrite();

    wait();

    while (true) {
      const MatrixParams params = weight_packer_params.Pop();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      while (!packer_stream_end.read()) {
        ac_int<MAX_FETCH_WIDTH, false> bits;

        for (ac_int<4, false> i = 0;; i++) {
          bits.set_slc(i * port_width, weight_resp.Pop());
          if (i == params.weight_num_beats - 1) break;
        }

        packed_bits.Push(bits);
      }
    }
  }

#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
  // Unpack weight rows or transpose [cols][rows] into [rows][cols]
  void transposer() {
    transposer_params.ResetRead();
    packed_bits.ResetRead();
    transpose_out.ResetWrite();
    packing_indices_deq.ResetRead();

    wait();

    while (true) {
      const MatrixParams params = transposer_params.Pop();

      // Same bound the systolic transposer uses. The compiler stops fusing the
      // transpose reshape into the matrix op once hardware unrolling reaches 64
      // and emits a standalone transpose node instead, so weight_transpose is
      // never set at that size. Spelling the bound out as a compile-time
      // constant lets Catapult delete the rows x cols corner-turn buffer, which
      // would otherwise cost rows*cols registers -- 32k flops at 64x64.
#ifndef __SYNTHESIS__
      if (params.weight_transpose && !(rows < 64 && cols < 64)) {
        // The coupling to the compiler is invisible in RTL; fail loudly here
        // rather than silently feed the array untransposed weights
        SC_REPORT_FATAL("WeightController",
                        "weight transpose is unsupported at this array size");
      }
#endif

      if (params.weight_transpose && rows < 64 && cols < 64) {
        // Each source row becomes one column of the resident weight set
        ac_int<DATA_WIDTH, false> transpose_buffer[rows][cols];

        // Keep the blocking gather and emit phases in one sequential set
        // transfer
        while (!transposer_stream_end.read()) {
          for (int source_col = 0; source_col < cols; source_col++) {
            if (source_col != 0) {
              const bool set_done = transposer_stream_end.read();
#ifndef __SYNTHESIS__
              if (set_done) {
                SC_REPORT_FATAL("WeightController",
                                "incomplete CIM transposed weight set");
              }
#endif
            }

            const ac_int<MAX_FETCH_WIDTH, false> bits = packed_bits.Pop();
            const ac_int<4, false> packing_index = packing_indices_deq.Pop();
            // Hold one transposed source row before scattering it by row
            ac_int<SOURCE_ROW_WIDTH, false> source_values = 0;
            const bool handled =
                (unpack_bits<WeightTypes, rows, SOURCE_ROW_WIDTH,
                             MAX_FETCH_WIDTH, WeightTypes...>(
                     params.weight_dtype, bits, source_values, packing_index) ||
                 ...);

#ifndef __SYNTHESIS__
            if (!handled) {
              throw std::runtime_error("Unsupported dtype for matrix weight: " +
                                       std::to_string(params.weight_dtype));
            }
#endif

#pragma hls_unroll yes
            for (int row = 0; row < rows; row++) {
              transpose_buffer[row][source_col] =
                  source_values.template slc<DATA_WIDTH>(row * DATA_WIDTH);
            }
          }

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
          for (int row = 0; row < rows; row++) {
            ac_int<buffer_width, false> transposed;
#pragma hls_unroll yes
            for (int col = 0; col < cols; col++) {
              transposed.set_slc(col * DATA_WIDTH, transpose_buffer[row][col]);
            }
            transpose_out.Push(transposed);
          }
        }
      } else {
#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
        while (!transposer_stream_end.read()) {
          const ac_int<MAX_FETCH_WIDTH, false> bits = packed_bits.Pop();
          const ac_int<4, false> packing_index = packing_indices_deq.Pop();
          ac_int<buffer_width, false> outputs = 0;
          const bool handled =
              (unpack_bits<WeightTypes, cols, buffer_width, MAX_FETCH_WIDTH,
                           WeightTypes...>(params.weight_dtype, bits, outputs,
                                           packing_index) ||
               ...);

#ifndef __SYNTHESIS__
          if (!handled) {
            throw std::runtime_error("Unsupported dtype for matrix weight: " +
                                     std::to_string(params.weight_dtype));
          }
#endif

          transpose_out.Push(outputs);
        }
      }
    }
  }
#else
  void transposer() {
    transposer_params.ResetRead();
    packed_bits.ResetRead();
    transpose_out.ResetWrite();

    wait();

    while (true) {
      const MatrixParams params = transposer_params.Pop();

      ac_int<LOOP_WIDTH, false> loop_bounds[2][5];

#pragma hls_unroll yes
      for (int i = 0; i < 2; i++) {
#pragma hls_unroll yes
        for (int j = 0; j < 5; j++) {
          loop_bounds[i][j] = params.weight_addr_loops[i][j];
        }
      }

      ac_int<32, false> total_values =
          loop_bounds[0][0] * loop_bounds[0][1] * loop_bounds[0][2] *
          loop_bounds[0][3] * loop_bounds[0][4] * loop_bounds[1][0] *
          loop_bounds[1][1] * loop_bounds[1][2] * loop_bounds[1][3];
      ac_int<32, false> count = 0;

      // don't support transpose when systolic array is larger
      // than 32x32, as it will require a very large buffer
      if (params.weight_transpose && rows < 64 && cols < 64) {
        // we need a square buffer to store the transpose
        ac_int<DATA_WIDTH, false> transpose_buffer[rows > cols ? rows : cols]
                                                  [rows > cols ? rows : cols];

#ifndef __SYNTHESIS__
        // Assume that the innermost loop is the c0 loop
        // Must be true for the transpose case
        assert(loop_bounds[1][4] == cols);
#endif

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
        while (count++ < total_values) {
          for (int c0 = 0; c0 < cols; c0++) {
            ac_int<buffer_width, false> bits = packed_bits.Pop();

            ac_int<buffer_width, false> outputs = 0;
            bool handled = (unpack_bits<WeightTypes, cols, buffer_width,
                                        MAX_FETCH_WIDTH, WeightTypes...>(
                                params.weight_dtype, bits, outputs, 0) ||
                            ...);

#ifndef __SYNTHESIS__
            if (!handled) {
              throw std::runtime_error("Unsupported dtype for matrix weight: " +
                                       std::to_string(params.weight_dtype));
            }
#endif

#pragma hls_unroll yes
            for (int dim = 0; dim < cols; dim++) {
              transpose_buffer[dim][c0] =
                  outputs.template slc<DATA_WIDTH>(dim * DATA_WIDTH);
            }
          }

          for (int c0 = 0; c0 < cols; c0++) {
            ac_int<buffer_width, false> transposed;

#pragma hls_unroll yes
            for (int dim = 0; dim < cols; dim++) {
              transposed.set_slc(dim * DATA_WIDTH, transpose_buffer[c0][dim]);
            }

            transpose_out.Push(transposed);
          }
        }
      } else {  // passthrough
        ac_int<4, false> pack_offset_bound =
            (1 << params.weight_pack_factor_lg2) - 1;

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
        while (!transposer_stream_end.read()) {
          ac_int<MAX_FETCH_WIDTH, false> bits = packed_bits.Pop();

          // Unpack bits into outputs based on dtype
          for (ac_int<4, false> i = 0;; i++) {
            ac_int<buffer_width, false> outputs = 0;
            bool handled = (unpack_bits<WeightTypes, cols, buffer_width,
                                        MAX_FETCH_WIDTH, WeightTypes...>(
                                params.weight_dtype, bits, outputs, i) ||
                            ...);

#ifndef __SYNTHESIS__
            if (!handled) {
              throw std::runtime_error("Unsupported dtype for matrix weight: " +
                                       std::to_string(params.weight_dtype));
            }
#endif

            transpose_out.Push(outputs);

            if (i == pack_offset_bound) break;
          }
        }
      }
    }
  }
#endif

  // Restrict bias traversal to the points where the processor loads its cache
  static void set_bias_loop_bounds(
      const MatrixParams &params,
      ac_int<LOOP_WIDTH, false> loop_bounds[2][LOOP_SLOT_COUNT]) {
    loop_bounds[0][params.reduction_loop_idx[0]] = 0;
    loop_bounds[0][params.fy_loop_idx[0]] = 0;
    loop_bounds[1][params.fx_loop_idx] = 0;
    loop_bounds[1][params.fy_loop_idx[1]] = 0;
    loop_bounds[1][params.reduction_loop_idx[1]] = 0;

    // The processor retains bias while loops nested inside L1 OC advance
#pragma hls_unroll yes
    for (int slot = 0; slot < LOOP_SLOT_COUNT; slot++) {
      if (slot > params.weight_loop_idx[1]) {
        loop_bounds[1][slot] = 0;
      }
    }
  }

  void bias_fetcher() {
    bias_fetcher_params.ResetRead();
    bias_req.Reset();

    wait();

    while (true) {
      const MatrixParams params = bias_fetcher_params.Pop();

      ac_int<LOOP_WIDTH, false> loop_counters[2][6];
      ac_int<LOOP_WIDTH, false> loop_bounds[2][6];

#pragma hls_unroll yes
      for (int i = 0; i < 2; i++) {
#pragma hls_unroll yes
        for (int j = 0; j < 6; j++) {
          loop_bounds[i][j] = params.loops[i][j] - 1;
        }
      }

      set_bias_loop_bounds(params, loop_bounds);

      ac_int<LOOP_WIDTH, false> K2 = params.loops[0][params.weight_loop_idx[0]];
      ac_int<LOOP_WIDTH, false> K1 = params.loops[1][params.weight_loop_idx[1]];

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      for (loop_counters[0][0] = 0;; loop_counters[0][0]++) {
        for (loop_counters[0][1] = 0;; loop_counters[0][1]++) {
          for (loop_counters[0][2] = 0;; loop_counters[0][2]++) {
            for (loop_counters[0][3] = 0;; loop_counters[0][3]++) {
              for (loop_counters[0][4] = 0;; loop_counters[0][4]++) {
                for (loop_counters[1][0] = 0;; loop_counters[1][0]++) {
                  for (loop_counters[1][1] = 0;; loop_counters[1][1]++) {
                    for (loop_counters[1][2] = 0;; loop_counters[1][2]++) {
                      for (loop_counters[1][3] = 0;; loop_counters[1][3]++) {
                        for (loop_counters[1][4] = 0;; loop_counters[1][4]++) {
                          for (loop_counters[1][5] = 0;;
                               loop_counters[1][5]++) {
                            ac_int<LOOP_WIDTH, false> k2 =
                                loop_counters[0][params.weight_loop_idx[0]];
                            ac_int<LOOP_WIDTH, false> k1 =
                                loop_counters[1][params.weight_loop_idx[1]];

                            ac_int<16, false> address =
                                k2 * K1 * cols + k1 * cols;

                            MemoryRequest request = {
                                params.bias_offset + address * Bias::width / 8,
                                cols * Bias::width / 8};

                            bias_req.Push(request);

                            if (loop_counters[1][5] == loop_bounds[1][5]) break;
                          }
                          if (loop_counters[1][4] == loop_bounds[1][4]) break;
                        }
                        if (loop_counters[1][3] == loop_bounds[1][3]) break;
                      }
                      if (loop_counters[1][2] == loop_bounds[1][2]) break;
                    }
                    if (loop_counters[1][1] == loop_bounds[1][1]) break;
                  }
                  if (loop_counters[1][0] == loop_bounds[1][0]) break;
                }
                if (loop_counters[0][4] == loop_bounds[0][4]) break;
              }
              if (loop_counters[0][3] == loop_bounds[0][3]) break;
            }
            if (loop_counters[0][2] == loop_bounds[0][2]) break;
          }
          if (loop_counters[0][1] == loop_bounds[0][1]) break;
        }
        if (loop_counters[0][0] == loop_bounds[0][0]) break;
      }
    }
  }

  void bias_feeder() {
    bias_feeder_params.ResetRead();
    bias_resp.Reset();
    bias_data.Reset();

    wait();

    while (true) {
      const MatrixParams params = bias_feeder_params.Pop();

      ac_int<LOOP_WIDTH, false> loop_counters[2][6];
      ac_int<LOOP_WIDTH, false> loop_bounds[2][6];

#pragma hls_unroll yes
      for (int i = 0; i < 2; i++) {
#pragma hls_unroll yes
        for (int j = 0; j < 6; j++) {
          loop_bounds[i][j] = params.loops[i][j] - 1;
        }
      }

      set_bias_loop_bounds(params, loop_bounds);

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
      for (loop_counters[0][0] = 0;; loop_counters[0][0]++) {
        for (loop_counters[0][1] = 0;; loop_counters[0][1]++) {
          for (loop_counters[0][2] = 0;; loop_counters[0][2]++) {
            for (loop_counters[0][3] = 0;; loop_counters[0][3]++) {
              for (loop_counters[0][4] = 0;; loop_counters[0][4]++) {
                for (loop_counters[1][0] = 0;; loop_counters[1][0]++) {
                  for (loop_counters[1][1] = 0;; loop_counters[1][1]++) {
                    for (loop_counters[1][2] = 0;; loop_counters[1][2]++) {
                      for (loop_counters[1][3] = 0;; loop_counters[1][3]++) {
                        for (loop_counters[1][4] = 0;; loop_counters[1][4]++) {
                          for (loop_counters[1][5] = 0;;
                               loop_counters[1][5]++) {
                            ac_int<Bias::width * cols, false> bits;

                            process_matrix_input<Bias, cols, port_width,
                                                 Bias::width * cols>(bias_resp,
                                                                     bits);

                            Pack1D<Bias, cols> biases =
                                BitsToType<Pack1D<Bias, cols>>(
                                    TypeToBits(bits));

                            bias_data.Push(biases);
                            if (loop_counters[1][5] == loop_bounds[1][5]) break;
                          }
                          if (loop_counters[1][4] == loop_bounds[1][4]) break;
                        }
                        if (loop_counters[1][3] == loop_bounds[1][3]) break;
                      }
                      if (loop_counters[1][2] == loop_bounds[1][2]) break;
                    }
                    if (loop_counters[1][1] == loop_bounds[1][1]) break;
                  }
                  if (loop_counters[1][0] == loop_bounds[1][0]) break;
                }
                if (loop_counters[0][4] == loop_bounds[0][4]) break;
              }
              if (loop_counters[0][3] == loop_bounds[0][3]) break;
            }
            if (loop_counters[0][2] == loop_bounds[0][2]) break;
          }
          if (loop_counters[0][1] == loop_bounds[0][1]) break;
        }
        if (loop_counters[0][0] == loop_bounds[0][0]) break;
      }
    }
  }

  void read_params() {
    params_in.Reset();
#if MATRIX_BACKEND != MATRIX_BACKEND_CIM
    fetcher_params.ResetWrite();
#endif
    writer_params.ResetWrite();
    reader_params.ResetWrite();
    transposer_params.ResetWrite();
    weight_packer_params.ResetWrite();
    bias_fetcher_params.ResetWrite();
    bias_feeder_params.ResetWrite();

    wait();

    while (true) {
      const MatrixParams params = params_in.Pop();

#if MATRIX_BACKEND != MATRIX_BACKEND_CIM
      fetcher_params.Push(params);
#endif
      writer_params.Push(params);
      reader_params.Push(params);
      transposer_params.Push(params);
      weight_packer_params.Push(params);

      if (params.has_bias) {
        bias_fetcher_params.Push(params);
        bias_feeder_params.Push(params);
      }
    }
  }
};
