// Export epilogue execution descriptions from the compiler's shared lowering
#include <iostream>
#include <iterator>
#include <string>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/util/json_util.h>
#include "test/toolchain/EpilogueLowering.h"

using google::protobuf::Struct;

// Record one physical port's datatype without choosing matrix lane geometry
void add_transfer(Struct& pass, const std::string& port,
                  const codegen::Tensor& tensor) {
  auto* transfer = (*pass.mutable_fields())["transfers"].mutable_list_value()
                       ->add_values()->mutable_struct_value();
  (*transfer->mutable_fields())["resource"].set_string_value(port);
  (*transfer->mutable_fields())["dtype"].set_string_value(tensor.dtype());
}

// Project the same epilogue used by MatrixOps into resource and stage metadata
Struct describe_epilogue(const codegen::Operation& operation) {
  std::vector<codegen::OpOverload> ops;
  if (operation.has_op()) ops.push_back(operation.op());
  else ops.assign(operation.fused_op().op_list().begin(), operation.fused_op().op_list().end());
  const auto& output = operation.has_output() ? operation.output() :
      operation.outputs().tensors(operation.outputs().tensors_size() - 1);
  const auto epilogue = lower_epilogue(ops, output);
  Struct result;
  (*result.mutable_fields())["direct"].set_bool_value(epilogue.direct);
  auto& pass = *(*result.mutable_fields())["passes"].mutable_list_value()
                    ->add_values()->mutable_struct_value();
  (*pass.mutable_fields())["source"].set_string_value("matrix");
  auto* stages = (*pass.mutable_fields())["stages"].mutable_list_value();
  for (int stage = 0; stage < 4; ++stage) stages->add_values()->set_string_value("");
  (*pass.mutable_fields())["dequantize"].set_bool_value(false);
  (*pass.mutable_fields())["modes"].mutable_list_value();
  add_transfer(pass, "output", output);
  for (const auto& step : epilogue.steps) {
    const auto& op = ops[step.operation];
    if (step.dequantize) {
      (*pass.mutable_fields())["dequantize"].set_bool_value(true);
      continue;
    }
    stages->mutable_values(step.stage)->set_string_value(op.target());
    if (step.polynomial) stages->mutable_values(2)->set_string_value(op.target());
    if (step.microscaling)
      (*pass.mutable_fields())["modes"].mutable_list_value()->add_values()->set_string_value("microscaling");
    if (step.fetch)
      add_transfer(pass, "fetch" + std::to_string(step.fetch), op.kwargs().at(step.tensor_key).tensor());
  }
  if (ops.front().kwargs().contains("A_indptr"))
    (*pass.mutable_fields())["modes"].mutable_list_value()->add_values()->set_string_value("sparse_merge");
  return result;
}

// Export one epilogue execution description per selected matrix operation group
int main() {
  try {
    codegen::Model model;
    const std::string input{std::istreambuf_iterator<char>(std::cin), {}};
    if (!google::protobuf::TextFormat::ParseFromString(input, &model))
      throw std::invalid_argument("cannot parse compiler model");
    Struct result;
    for (const auto& operation : model.ops()) {
      const std::string name = operation.has_op() ? operation.op().name() : operation.fused_op().name();
      if (operation.has_loop() || (operation.has_fused_op() && operation.fused_op().op_list().empty()))
        throw std::invalid_argument("epilogue export requires flat nonempty operations");
      if (!operation.has_output() && operation.outputs().tensors().empty())
        throw std::invalid_argument(name + ": epilogue export requires an output tensor");
      if (result.fields().contains(name)) throw std::invalid_argument("duplicate operation: " + name);
      try {
        *(*result.mutable_fields())[name].mutable_struct_value() = describe_epilogue(operation);
      } catch (const std::exception& error) {
        throw std::invalid_argument(name + ": " + error.what());
      }
    }
    std::string json;
    const auto status = google::protobuf::util::MessageToJsonString(result, &json);
    if (!status.ok()) throw std::runtime_error(status.ToString());
    std::cout << json << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
