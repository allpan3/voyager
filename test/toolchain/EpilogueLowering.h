// Share matrix epilogue lowering between command generation and mapping
#pragma once

#include <array>
#include <cstdlib>
#include <functional>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "test/compiler/proto/param.pb.h"

inline const std::set<std::string> poly_ops = {
    "gelu",        "gelu_",        "silu",        "silu_",        "elu",
    "elu_",        "tanh",         "tanh_",       "tanh_1",       "tanh_1_",
    "sigmoid",     "sigmoid_",     "hardsigmoid", "hardsigmoid_", "hardswish",
    "hardswish_",  "mish",         "mish_",       "softplus",     "softplus_",
    "log_sigmoid", "log_sigmoid_", "selu",        "selu_",        "celu",
    "celu_",       "hardshrink",   "hardshrink_", "hardtanh",     "hardtanh_",
    "leaky_relu",  "leaky_relu_",  "rrelu",       "rrelu_",       "softshrink",
    "softshrink_", "threshold",    "threshold_"};

inline const std::vector<std::set<std::string>> vector_unit_ops = {
    {"add", "add_", "sub", "sub_", "mul", "mul_", "div", "div_", "neg",
     "quantize"},
    {"exp", "abs", "relu", "relu_"},
    {"add", "add_", "mul", "mul_", "div", "div_", "square", "quantize"},
    {"mul", "mul_", "div", "div_", "quantize", "quantize_mx",
     "quantize_mx_outlier"},
};

// Retain operand references until tiling and constants are bound
struct EpilogueStep {
  int operation;
  int stage = -1;
  int fetch = 0;
  std::string argument;
  std::string tensor_key;
  bool polynomial = false;
  bool dequantize = false;
  bool microscaling = false;
};

// Describe one existing fused matrix epilogue without addresses or loop counts
struct LoweredEpilogue {
  bool direct = false;
  std::vector<EpilogueStep> steps;
};

// Count logical tensor elements without reading constant data
inline int64_t vector_tensor_size(const codegen::Tensor& tensor) {
  // Match the command generator's reshape and SoC tile selection
  const auto product = [](const auto& shape) {
    return std::accumulate(shape.begin(), shape.end(), int64_t{1}, std::multiplies<int64_t>());
  };
  if (tensor.has_reshape())
    return product(tensor.reshape().kwargs().at("output_shape").int_list().values());
  const char* soc = std::getenv("SOC_SIM");
  if (soc && std::string(soc) == "1" && tensor.tiled_shape_size()) return product(tensor.tiled_shape());
  return product(tensor.shape());
}

// Select stages and operand ports once for both command generation and mapping
inline LoweredEpilogue lower_epilogue(
    const std::vector<codegen::OpOverload>& ops, const codegen::Tensor& output,
    bool ordinary_matrix = true) {
  if (ops.empty()) throw std::invalid_argument("empty matrix operation group");
  const auto& kwargs = ops.front().kwargs();
  const bool sparse = kwargs.contains("A_indptr") && kwargs.contains("A_indices") &&
                      kwargs.contains("A_data");
  LoweredEpilogue epilogue;
  epilogue.direct = ordinary_matrix && !sparse && ops.size() == 1 && !output.has_reshape();
  if (epilogue.direct) return epilogue;
  int stage = 0;
  std::array<bool, 2> fetch_used = {false, false};
  for (int i = 1; i < ops.size(); ++i) {
    const auto& op = ops[i];
    const auto& opcode = op.target();
    EpilogueStep step{i};
    if (opcode == "dequantize") {
      if (vector_tensor_size(op.kwargs().at("scale").tensor()) != 1)
        throw std::invalid_argument("matrix dequantization requires a scalar scale");
      step.dequantize = true;
      epilogue.steps.push_back(step);
      continue;
    }
    if (poly_ops.count(opcode)) {
      if (stage != 0)
        throw std::invalid_argument("polynomial approximation must be the first vector operation");
      step.stage = 0;
      step.polynomial = true;
      epilogue.steps.push_back(step);
      stage = 3;
      continue;
    }
    for (; stage < vector_unit_ops.size(); ++stage) {
      if (opcode == "quantize" && stage != 3 &&
          vector_tensor_size(op.kwargs().at("scale").tensor()) > 1) continue;
      if ((opcode == "add" || opcode == "add_") && stage == 0 && sparse) continue;
      if (vector_unit_ops[stage].count(opcode)) break;
    }
    if (stage == vector_unit_ops.size())
      throw std::invalid_argument("vector operation does not fit the hardware pipeline: " + opcode);
    step.stage = stage++;
    step.microscaling = opcode == "quantize_mx" || opcode == "quantize_mx_outlier";
    if (!step.microscaling && (op.kwargs().contains("other") || opcode == "quantize")) {
      step.argument = opcode == "quantize" ? "scale" : "other";
      const auto& argument = op.kwargs().at(step.argument);
      if (argument.has_tensor() && vector_tensor_size(argument.tensor()) != 1) {
        step.tensor_key = argument.tensor().has_memory() ? step.argument : "input";
        const auto& tensor = op.kwargs().at(step.tensor_key).tensor();
        if (!tensor.has_memory())
          throw std::invalid_argument("vector operand must have a memory allocation");
        step.fetch = step.stage == 0 ? 1 : 2;
        if (fetch_used[step.fetch - 1])
          throw std::invalid_argument("vector pipeline requests one fetch port twice");
        fetch_used[step.fetch - 1] = true;
      }
    }
    epilogue.steps.push_back(step);
  }
  return epilogue;
}
