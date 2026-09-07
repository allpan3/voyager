// Export effective parameters for the selected mapping target
#include <iostream>
#include <tuple>
#include <type_traits>

#include "ArchitectureParams.h"

#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
#if !defined(INT8) && !defined(INT8_32)
#error "current CIM target requires INT8 or INT8_32"
#endif

// Check build settings that the target schema represents implicitly
static_assert(std::is_same<INPUT_DATATYPE, DataTypes::int8>::value &&
                  std::is_same<WEIGHT_DATATYPE, DataTypes::int8>::value &&
                  INPUT_DTYPE_WIDTH == 8 && WEIGHT_DTYPE_WIDTH == 8 &&
                  CIM_SIGNED && !SUPPORT_MX && !SUPPORT_CODEBOOK_QUANT,
              "current CIM target requires signed native INT8 operands");
static_assert(std::is_same<ACCUM_BUFFER_DATATYPE, ACCUM_DATATYPE>::value,
              "current CIM target requires native accumulation storage");
static_assert(IC_DIMENSION == CIM_ARRAY_K_DIMENSION &&
                  OC_DIMENSION == CIM_ARRAY_N_DIMENSION,
              "compiler dimensions must match the physical CIM array");
static_assert(INPUT_BUFFER_WIDTH == CIM_ARRAY_K_DIMENSION * INPUT_DTYPE_WIDTH,
              "one input-buffer word must hold a full CIM A beat");
static_assert(VECTOR_UNIT_WIDTH == OC_DIMENSION &&
                  std::is_same<std::tuple<MU_OUTPUT_TYPES>,
                               std::tuple<ACCUM_BUFFER_DATATYPE>>::value &&
                  std::is_same<std::tuple<VU_INPUT_TYPES>,
                               std::tuple<DataTypes::int8, DataTypes::bfloat16>>::value &&
                  std::is_same<std::tuple<OUTPUT_DATATYPES>,
                               std::tuple<DataTypes::int8, DataTypes::bfloat16>>::value,
              "CIM search requires native matrix output and the full-width INT8/BF16 vector path");

#endif

// Append a numeric or boolean parameter to the JSON object
template <typename T>
void parameter(const char* name, T value) {
  std::cout << ",\n  \"" << name << "\": " << std::boolalpha << value;
}

// Emit values after C++ resolves datatype widths and dependent macros
int main() {
  std::cout << "{\n  \"datatype\": \"" << MAPPING_DATATYPE << "\"";
#if MATRIX_BACKEND == MATRIX_BACKEND_CIM
  std::cout << ",\n  \"backend\": \"cim\"";
  parameter("ch_in", CIM_CH_IN);
  parameter("ch_out", CIM_CH_OUT);
  parameter("b_sets", CIM_B_SETS);
  parameter("base_a_width", CIM_BASE_A_WIDTH);
  parameter("base_b_width", CIM_BASE_B_WIDTH);
  parameter("base_c_width", CIM_BASE_C_WIDTH);
  parameter("write_ch_in", CIM_WRITE_CH_IN);
  parameter("mac_latency", CIM_MAC_LATENCY);
  parameter("mode", CIM_MODE);
  parameter("tile_input_axis_elements", CIM_TILE_INPUT_AXIS_ELEMENTS);
  parameter("tile_output_axis_elements", CIM_TILE_OUTPUT_AXIS_ELEMENTS);
  parameter("input_axis_tiles", CIM_INPUT_AXIS_TILES);
  parameter("output_axis_tiles", CIM_OUTPUT_AXIS_TILES);
  parameter("a_port_tiles", CIM_A_PORT_TILES);
  parameter("b_port_tiles", CIM_B_PORT_TILES);
  parameter("c_port_tiles", CIM_C_PORT_TILES);
  parameter("c_beat_layout", CIM_C_BEAT_LAYOUT);
  parameter("result_slots_per_output_lane", CIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE);
  parameter("local_accum_contexts", CIM_LOCAL_ACCUM_CONTEXTS);
#else
  std::cout << ",\n  \"backend\": \"sa\"";
  parameter("k", IC_DIMENSION);
  parameter("n", OC_DIMENSION);
  parameter("weight_buffer_words", WEIGHT_BUFFER_SIZE);
#endif
  parameter("input_bits", INPUT_DTYPE_WIDTH);
  parameter("weight_bits", WEIGHT_DTYPE_WIDTH);
  parameter("accum_bits", ACCUM_BUFFER_DATATYPE::width);
  parameter("input_buffer_words", INPUT_BUFFER_SIZE);
  parameter("accum_buffer_words", ACCUM_BUFFER_SIZE);
  parameter("double_buffered_accum", bool(DOUBLE_BUFFERED_ACCUM_BUFFER));
  parameter("ic_port_bits", IC_PORT_WIDTH);
  parameter("oc_port_bits", OC_PORT_WIDTH);
  std::cout << ",\n  \"hardware_options\": {\n    \"support_mvm\": " << std::boolalpha << bool(SUPPORT_MVM);
  parameter("support_spmm", bool(SUPPORT_SPMM));
  parameter("support_dwc", bool(SUPPORT_DWC));
  parameter("enable_perf_counters", bool(ENABLE_PERF_COUNTERS));
  std::cout << "\n  }\n}\n";
}
