#pragma once

#include <map>
#include <set>

#include "src/Params.h"
#include "test/common/Utils.h"
#include "test/compiler/proto/param.pb.h"
#include "test/toolchain/ApproximationConstants.h"

constexpr int MAX_LOOP_VALUE = 65535;

#include "test/toolchain/EpilogueLowering.h"

// --------------------------------------------------------------------------
// Stage 0: Basic Arithmetic (Add, Sub, Mult)
// --------------------------------------------------------------------------
unsigned int get_stage0_op(const std::string& opcode) {
  if (opcode == "add" || opcode == "add_") return VectorInstructions::op0_add;
  if (opcode == "sub" || opcode == "sub_" || opcode == "neg")
    return VectorInstructions::op0_sub;
  if (opcode == "mul" || opcode == "mul_") return VectorInstructions::op0_mul;

  // Division/Quantization in Stage 0 is implemented as Multiplication by
  // reciprocal
  if (opcode == "div" || opcode == "div_" || opcode == "quantize")
    return VectorInstructions::op0_mul;

  return VectorInstructions::op0_nop;
}

// --------------------------------------------------------------------------
// Stage 1: Abs, Exp, and Activations Functions
// --------------------------------------------------------------------------
unsigned int get_stage1_op(const std::string& opcode) {
  if (opcode == "abs") return VectorInstructions::op1_abs;
  if (opcode == "exp") return VectorInstructions::op1_exp;
  if (opcode == "relu" || opcode == "relu_")
    return VectorInstructions::op1_relu;

  return VectorInstructions::op1_nop;
}

// --------------------------------------------------------------------------
// Stage 2: Advanced Arithmetic (Add, Mult, Square)
// --------------------------------------------------------------------------
unsigned int get_stage2_op(const std::string& opcode) {
  if (opcode == "add" || opcode == "add_") return VectorInstructions::op2_add;
  if (opcode == "mul" || opcode == "mul_") return VectorInstructions::op2_mul;
  if (opcode == "square") return VectorInstructions::op2_sqr;

  if (opcode == "div" || opcode == "div_" || opcode == "quantize")
    return VectorInstructions::op2_mul;

  return VectorInstructions::op2_nop;
}

// --------------------------------------------------------------------------
// Stage 3: Quantization & True Division
// --------------------------------------------------------------------------
unsigned int get_stage3_op(const std::string& opcode) {
  if (opcode == "mul" || opcode == "mul_") return VectorInstructions::op3_mul;
  if (opcode == "div" || opcode == "div_" || opcode == "quantize")
    return VectorInstructions::op3_div;
  if (opcode == "quantize_mx") return VectorInstructions::op3_quantize_mx;
  if (opcode == "quantize_mx_outlier")
    return VectorInstructions::op3_quantize_mx_outlier;

  return VectorInstructions::op3_nop;
}

// --------------------------------------------------------------------------
// Approximation Constants Loader
// --------------------------------------------------------------------------
void load_approx_params(const std::string& opcode,
                        VectorInstructionConfig* vector_instruction_config,
                        const std::map<std::string, float>& kwargs) {
  auto& config = vector_instruction_config->approx_config;

// Macro updated to use braces for all loops
#define LOAD_ACTIVATION_CONSTANTS(NAME)            \
  do {                                             \
    for (int i = 0; i < NUM_MAXES; i++) {          \
      config.maxes[i] = NAME##_MAXES[i];           \
    }                                              \
    for (int i = 0; i < NUM_RANGES; i++) {         \
      for (int j = 0; j < NUM_COEFFS; j++) {       \
        config.ranges[i][j] = NAME##_RANGES[i][j]; \
      }                                            \
    }                                              \
    config.clamp_min = NAME##_CLAMP_MIN;           \
    config.clamp_max = NAME##_CLAMP_MAX;           \
  } while (0)

  // Standard Activations
  if (opcode == "gelu" || opcode == "gelu_") {
    LOAD_ACTIVATION_CONSTANTS(GELU);
  } else if (opcode == "silu" || opcode == "silu_") {
    LOAD_ACTIVATION_CONSTANTS(SILU);
  } else if (opcode == "elu" || opcode == "elu_") {
    LOAD_ACTIVATION_CONSTANTS(ELU);
  } else if (opcode == "log_sigmoid" || opcode == "log_sigmoid_") {
    LOAD_ACTIVATION_CONSTANTS(LOGSIGMOID);
  } else if (opcode == "tanh" || opcode == "tanh_") {
    LOAD_ACTIVATION_CONSTANTS(TANH);
  } else if (opcode == "tanh_1" || opcode == "tanh_1_") {
    LOAD_ACTIVATION_CONSTANTS(TANHSHRINK);
  } else if (opcode == "softplus" || opcode == "softplus_") {
    LOAD_ACTIVATION_CONSTANTS(SOFTPLUS);
  } else if (opcode == "mish" || opcode == "mish_") {
    LOAD_ACTIVATION_CONSTANTS(MISH);
  } else if (opcode == "sigmoid" || opcode == "sigmoid_") {
    LOAD_ACTIVATION_CONSTANTS(SIGMOID);
  } else if (opcode == "selu" || opcode == "selu_") {
    LOAD_ACTIVATION_CONSTANTS(SELU);
  } else if (opcode == "celu" || opcode == "celu_") {
    LOAD_ACTIVATION_CONSTANTS(CELU);
  } else if (opcode == "hardsigmoid" || opcode == "hardsigmoid_") {
    LOAD_ACTIVATION_CONSTANTS(HARDSIGMOID);
  } else if (opcode == "hardswish" || opcode == "hardswish_") {
    LOAD_ACTIVATION_CONSTANTS(HARDSWISH);
  }

  // Parametric Activations
  else if (opcode == "hardshrink" || opcode == "hardshrink_") {
    const VECTOR_DATATYPE lambda = kwargs.at("lambd");
    const VECTOR_DATATYPE HARDSHRINK_MAXES[NUM_MAXES] = {-lambda, 0.0, 0.0,
                                                         0.0,     0.0, lambda};
    LOAD_ACTIVATION_CONSTANTS(HARDSHRINK);
  } else if (opcode == "hardtanh" || opcode == "hardtanh_") {
    const VECTOR_DATATYPE min_val = kwargs.at("min_val");
    const VECTOR_DATATYPE max_val = kwargs.at("max_val");
    const VECTOR_DATATYPE HARDTANH_MAXES[NUM_MAXES] = {
        min_val, max_val, max_val, max_val, max_val, max_val};
    const VECTOR_DATATYPE HARDTANH_RANGES[NUM_RANGES][NUM_COEFFS] = {
        {min_val, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 1.0, 0.0},
        {0.0, 1.0, 0.0},     {0.0, 1.0, 0.0}, {0.0, 1.0, 0.0},
        {max_val, 0.0, 0.0}};
    LOAD_ACTIVATION_CONSTANTS(HARDTANH);
  } else if (opcode == "leaky_relu" || opcode == "leaky_relu_") {
    const VECTOR_DATATYPE negative_slope = kwargs.at("negative_slope");
    const VECTOR_DATATYPE LEAKYRELU_RANGES[NUM_RANGES][NUM_COEFFS] = {
        {0.0, negative_slope, 0.0},
        {0.0, 0.0, 0.0},
        {0.0, 0.0, 0.0},
        {0.0, 0.0, 0.0},
        {0.0, 0.0, 0.0},
        {0.0, 0.0, 0.0},
        {0.0, 1.0, 0.0}};
    LOAD_ACTIVATION_CONSTANTS(LEAKYRELU);
  } else if (opcode == "rrelu" || opcode == "rrelu_") {
    const VECTOR_DATATYPE lower = kwargs.at("lower");
    const VECTOR_DATATYPE upper = kwargs.at("upper");
    const VECTOR_DATATYPE alpha = (lower + upper) / VECTOR_DATATYPE(2);
    const VECTOR_DATATYPE RRELU_RANGES[NUM_RANGES][NUM_COEFFS] = {
        {0.0, alpha, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0},
        {0.0, 0.0, 0.0},   {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}};
    LOAD_ACTIVATION_CONSTANTS(RRELU);
  } else if (opcode == "softshrink" || opcode == "softshrink_") {
    const VECTOR_DATATYPE lambda = kwargs.at("lambd");
    const VECTOR_DATATYPE SOFTSHRINK_MAXES[NUM_MAXES] = {-lambda, 0.0, 0.0,
                                                         0.0,     0.0, lambda};
    const VECTOR_DATATYPE SOFTSHRINK_RANGES[NUM_RANGES][NUM_COEFFS] = {
        {lambda, 1.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0},
        {0.0, 0.0, 0.0},    {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0},
        {-lambda, 1.0, 0.0}};
    LOAD_ACTIVATION_CONSTANTS(SOFTSHRINK);
  } else if (opcode == "threshold" || opcode == "threshold_") {
    const VECTOR_DATATYPE threshold = kwargs.at("threshold");
    const VECTOR_DATATYPE value = kwargs.at("value");
    const VECTOR_DATATYPE THRESHOLD_MAXES[NUM_MAXES] = {0.0, 0.0, 0.0,
                                                        0.0, 0.0, threshold};
    const VECTOR_DATATYPE THRESHOLD_RANGES[NUM_RANGES][NUM_COEFFS] = {
        {value, 0.0, 0.0}, {value, 0.0, 0.0}, {value, 0.0, 0.0},
        {value, 0.0, 0.0}, {value, 0.0, 0.0}, {value, 0.0, 0.0},
        {0.0, 1.0, 0.0}};
    LOAD_ACTIVATION_CONSTANTS(THRESHOLD);
  }

#undef LOAD_ACTIVATION_CONSTANTS
}

inline std::vector<int> squeeze_shape(const std::vector<int>& input) {
  std::vector<int> result;
  for (int value : input) {
    if (value > 1) {
      result.push_back(value);
    }
  }
  return result;
}

inline void squeeze_front_ones(std::vector<int>& vec) {
  vec.erase(vec.begin(),
            std::find_if(vec.begin(), vec.end(), [](int x) { return x != 1; }));
}

std::vector<int> split_loops(const std::vector<int> loops, int max_value) {
  if (max_value <= 1) {
    throw std::invalid_argument("max_value must be greater than 1.");
  }

  std::vector<int> result;

  for (int value : loops) {
    if (value <= max_value) {
      result.push_back(value);
    } else {
      // Find two factors of value such that both are <= max_value
      bool split_found = false;
      for (int factor = std::sqrt(value); factor > 1; --factor) {
        if (value % factor == 0) {
          int other_factor = value / factor;
          if (factor <= max_value && other_factor <= max_value) {
            result.push_back(factor);
            result.push_back(other_factor);
            split_found = true;
            break;
          }
        }
      }

      if (!split_found) {
        throw std::runtime_error("Unable to split value " +
                                 std::to_string(value) +
                                 " into two factors <= max_value.");
      }
    }
  }

  return result;
}

// Function to compute the prime factors of a given number
std::vector<int> prime_factors(int n) {
  std::vector<int> factors;
  for (int i = 2; i <= n / i; ++i) {
    while (n % i == 0) {
      factors.push_back(i);
      n /= i;
    }
  }
  if (n > 1) {
    factors.push_back(n);
  }
  return factors;
}

/**
 * Adjusts loop indices such that the prime factors of the target factor are
 * distributed among the indices, starting from the rightmost index.
 *
 * @param loops A vector of integers representing the loop indices.
 * @param target_factor An integer whose prime factors are to be distributed.
 * @return A vector of adjusted loop indices.
 * @throws std::runtime_error if the prime factors cannot be fully distributed.
 */
std::vector<int> adjust_loop_indices(const std::vector<int>& loops,
                                     int target_factor) {
  // Prime factorization of the target factor
  std::vector<int> remaining_factors = prime_factors(target_factor);

  // Result vector
  std::vector<int> result = loops;

  // Process from right to left
  for (int i = result.size() - 1; i >= 0; --i) {
    for (auto it = remaining_factors.begin(); it != remaining_factors.end();) {
      int factor = *it;
      if (result[i] % factor == 0) {
        result[i] /= factor;  // Divide the current index by the factor
        it = remaining_factors.erase(it);  // Remove the used factor
      } else {
        ++it;  // Move to the next factor
      }
    }
  }

  // If any factors remain unused, error out
  if (!remaining_factors.empty()) {
    throw std::runtime_error("Unused prime factors remain.");
  }

  // Multiply the rightmost index by the target factor
  result[result.size() - 1] *= target_factor;

  return result;
}

bool is_transpose(const std::vector<int>& dims) {
  int n = dims.size();
  // If there are fewer than 2 axes, there's nothing to swap.
  if (n < 2) return false;

  // Check that all axes except the last two are in their natural order.
  for (int i = 0; i < n - 2; ++i) {
    if (dims[i] != i) {
      return false;
    }
  }

  // Check that the last two axes are swapped.
  return (dims[n - 2] == n - 1 && dims[n - 1] == n - 2);
}

std::pair<std::vector<int>, std::vector<int>> factor_out_non_broadcastable_dim(
    std::vector<int> shape1, std::vector<int> shape2) {
  if (shape1.size() != shape2.size()) {
    throw std::invalid_argument(
        "Shapes must have the same number of dimensions");
  }

  int ndim = shape1.size();
  int non_broadcast_dim = -1;
  int min_dim;
  int max_dim;

  // Find the non-broadcastable dimension where one shape is bs times larger
  // than the other
  for (int i = 0; i < ndim; ++i) {
    min_dim = std::min(shape1[i], shape2[i]);
    max_dim = std::max(shape1[i], shape2[i]);
    if (shape1[i] != shape2[i] && max_dim % min_dim == 0) {
      non_broadcast_dim = i;
      break;
    }
  }

  if (non_broadcast_dim == -1) {
    throw std::runtime_error("No non-broadcastable dimension found");
  }

  // Merge the non-broadcastable dimension into the previous dimension
  int merge_dim = non_broadcast_dim - 1;
  if (merge_dim < 0) {
    throw std::runtime_error(
        "Cannot merge into a non-existent previous dimension");
  }

  shape1[merge_dim] *= min_dim;
  shape2[merge_dim] *= min_dim;

  shape1[non_broadcast_dim] /= min_dim;
  shape2[non_broadcast_dim] /= min_dim;

  return {shape1, shape2};
}

void update_tensor_shape(codegen::Tensor& tensor,
                         const std::vector<int>& new_shape) {
  if (is_soc_sim()) {
    tensor.clear_tiled_shape();
    for (int dim : new_shape) {
      tensor.add_tiled_shape(dim);
    }
  } else {
    tensor.clear_shape();
    for (int dim : new_shape) {
      tensor.add_shape(dim);
    }
  }
}

void set_dequantize_scale(const codegen::Tensor& tensor,
                          VectorInstructions& inst) {
  if (tensor.has_dequant()) {
    VECTOR_DATATYPE immediate = get_tensor_scalar_scale(tensor);
    inst.vdequantize = true;
    inst.vector_dq_scale = immediate.bits_rep();
  }
}

void set_codebook_config(const codegen::OpOverload& op,
                         VectorParams* vector_params,
                         VectorInstructionConfig* vector_instruction_config) {
  const auto kwargs = op.kwargs();

  if (!kwargs.contains("output_code")) {
    return;
  }

  const auto code = kwargs.at("output_code").tensor();
  const int size = get_size(code);

  float* array = read_constant_param(code);

  auto& config = vector_instruction_config->codebook_config;

  config.enable = true;
  for (int i = 0; i < size; i++) {
    config.output_code[i] = array[i];
  }

  vector_params->is_codebook_quantization = true;

  delete[] array;
}

void set_quantize_mx_params(
    const codegen::Operation& param, VectorParams* vector_params,
    VectorInstructions& inst,
    VectorInstructionConfig* vector_instruction_config) {
  const auto op_list = get_op_list(param);
  const auto op = op_list.back();
  const auto kwargs = op.kwargs();
  const std::string opcode = op.target();

  if (opcode != "quantize_mx" && opcode != "quantize_mx_outlier") {
    return;
  }

  int block_size = kwargs.at("block_size").int_value();
  float quant_max = kwargs.at("quant_max").float_value();
  bool force_scale_power_of_two =
      kwargs.at("force_scale_power_of_two").bool_value();

  assert(block_size == IC_DIMENSION);

  inst.vector_op3 = VectorInstructions::op3_quantize_mx;

  if (force_scale_power_of_two) {
    inst.immediate2 = floor(log2(quant_max));
  } else {
    VECTOR_DATATYPE scale = quant_max;
    inst.immediate2 = scale.bits_rep();
  }

  const auto outputs = get_op_outputs(param);
  const int num_outputs = outputs.size();

  vector_params->quantize_output_mx = true;
  vector_params->mx_scale_offset = get_address(outputs[num_outputs - 2]);

  if (opcode == "quantize_mx_outlier") {
    inst.vector_op3 = VectorInstructions::op3_quantize_mx_outlier;
    vector_params->has_sparse_output = true;
    vector_params->csr_data_offset = get_address(outputs[0]);
    vector_params->csr_indices_offset = get_address(outputs[1]);
    vector_params->csr_indptr_offset = get_address(outputs[2]);

    auto& config = vector_instruction_config->outlier_filter;

    VECTOR_DATATYPE threshold = kwargs.at("threshold").float_value();
    config.outlier_threshold = threshold.bits_rep();

    const auto quantize_input = kwargs.at("input").tensor();
    const auto quantize_shape = get_shape(quantize_input);
    config.dense_input_shape[1] = quantize_shape.back() / VECTOR_UNIT_WIDTH;
    config.dense_input_shape[0] =
        get_size(quantize_input) / quantize_shape.back();
  }

  set_codebook_config(op, vector_params, vector_instruction_config);
}

void set_quantize_params(const codegen::Operation& param,
                         VectorParams* vector_params, VectorInstructions& inst,
                         VectorInstructionConfig* vector_instruction_config) {
  const auto op_list = get_op_list(param);
  const auto op = op_list.back();
  const auto kwargs = op.kwargs();
  const std::string opcode = op.target();

  if (opcode == "quantize") {
    const auto scale = kwargs.at("scale").tensor();
    assert(get_size(scale) == 1);

    inst.vector_op3 = VectorInstructions::op3_div;
    inst.vector_op3_src1 = VectorInstructions::from_immediate_2;

    // scalar scale factor
    float* array = read_constant_param(scale);
    VECTOR_DATATYPE immediate = array[0];
    inst.immediate2 = immediate.bits_rep();

    delete[] array;
  } else if (opcode == "quantize_mx" || opcode == "quantize_mx_outlier") {
    set_quantize_mx_params(param, vector_params, inst,
                           vector_instruction_config);
  }
}

template <typename T, size_t N, int port_width, typename... Ts>
bool try_fetch_params_for_type(ac_int<DTYPE_INDEX_WIDTH, false> dtype,
                               int loop_bound, int& out_pf, int& out_fw) {
  if (get_type_index<T, Ts...>() != dtype) {
    return false;
  }

  using cfg = dtype_fetch_config<T, N, port_width>;
  constexpr int pf = cfg::packing_factor;

  // decide if we can do a full pack
  out_pf = (loop_bound % pf == 0 ? pf : 1);

  // Only allow powers of two packing factors
  if (out_pf <= 0 || (out_pf & (out_pf - 1)) != 0) {
    out_pf = 1;
  }

  // choose the matching width
  out_fw = (out_pf > 1 ? cfg::packed_fetch_width : cfg::fetch_width);

  return true;
}

template <size_t N, int port_width, typename... Ts>
int get_packing_factor(ac_int<DTYPE_INDEX_WIDTH, false> dtype, int loop_bound,
                       int& effective_fetch_width) {
  int pf = 1;

  // fold: try each Ts in turn. ||-fold stops updating pf once one returns true.
  bool found = (try_fetch_params_for_type<Ts, N, port_width, Ts...>(
                    dtype, loop_bound, pf, effective_fetch_width) ||
                ...);

#ifndef __SYNTHESIS__
  if (!found) {
    throw std::runtime_error("Unsupported dtype for packing‐factor: " +
                             std::to_string(dtype));
  }
#endif

  return pf;
}
