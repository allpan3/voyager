#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

#if !defined(MXNF4) && !defined(INT8)
#error "Build this regression with DATATYPE=MXNF4 or DATATYPE=INT8"
#endif

#include "src/Params.h"
#include "test/toolchain/MapOperation.h"

voyager::TensorBoxRef tensor(const char* name, const char* dtype, int rows,
                             int columns, int address) {
  voyager::TensorBoxRef ref;
  auto* box = ref.mutable_box();
  box->set_node(name);
  box->set_dtype(dtype);
  box->add_shape(rows);
  box->add_shape(columns);
  box->mutable_memory()->set_level(voyager::MEMORY_LEVEL_SCRATCHPAD);
  box->mutable_memory()->set_address(address);
  ref.add_output_shape(rows);
  ref.add_output_shape(columns);
  return ref;
}

voyager::Operation operation(const char* input_dtype, const char* output_dtype,
                             bool routed) {
  voyager::Operation op;
  op.set_name("first_partial");
  auto* prim = op.mutable_prim();
  prim->set_name("first_partial");
  prim->set_target("aten::linear");
  auto& kwargs = *prim->mutable_kwargs();
  *kwargs["input"].mutable_tensor_box() =
      tensor("input", input_dtype, 16, IC_DIMENSION, 0);
  *kwargs["weight"].mutable_tensor_box() =
      tensor("weight", input_dtype, OC_DIMENSION, IC_DIMENSION, 8192);
  if (routed)
    kwargs["output_via_vector"].mutable_scalar()->set_bool_value(true);
  *op.add_outputs()->mutable_destination() =
      tensor("output", output_dtype, 16, OC_DIMENSION, 16384);
  return op;
}

int sc_main(int, char**) {
  ScalarEnv env;
  std::deque<BaseParams*> params;
#ifdef INT8
  auto direct = operation("int8", "int24", false);
#else
  auto direct = operation("int6", "bfloat16", false);
#endif
  map_operation(direct, env, params);
  assert(params.size() == 1);
  auto* matrix = dynamic_cast<MatrixParams*>(params.front());
  assert(matrix && matrix->output_to_memory);
  assert(matrix->output_offset == 16384);
  for (auto* param : params) delete param;
  params.clear();

  // An explicit false marker must preserve the original direct path too.
  auto explicit_direct = direct;
  (*explicit_direct.mutable_prim()->mutable_kwargs())["output_via_vector"]
      .mutable_scalar()
      ->set_bool_value(false);
  map_operation(explicit_direct, env, params);
  assert(params.size() == 1);
  matrix = dynamic_cast<MatrixParams*>(params.front());
  assert(matrix && matrix->output_to_memory);
  assert(matrix->output_offset == 16384);
  for (auto* param : params) delete param;
  params.clear();

#ifndef INT8
  auto routed = operation("int6", "bfloat16", true);
  map_operation(routed, env, params);
  assert(params.size() == 3);
  bool saw_matrix = false, saw_vector = false, saw_instructions = false;
  for (auto* param : params) {
    if (auto* value = dynamic_cast<MatrixParams*>(param)) {
      assert(!value->output_to_memory);
      saw_matrix = true;
    }
    if (auto* value = dynamic_cast<VectorParams*>(param)) {
      assert(value->vector_output_offset == 16384);
      assert(value->output_dtype ==
             get_index_from_type_name<OUTPUT_DATATYPES>("bfloat16"));
      saw_vector = true;
    }
    if (auto* value = dynamic_cast<VectorInstructionConfig*>(param)) {
      assert(value->num_inst == 1);
      const auto& inst = value->inst[0];
      assert(inst.vector_op0_src0 == VectorInstructions::from_matrix_unit);
      assert(inst.vdest == VectorInstructions::to_output);
      assert(inst.vector_op0 == VectorInstructions::op0_nop);
      assert(inst.vector_op1 == VectorInstructions::op1_nop);
      assert(inst.vector_op2 == VectorInstructions::op2_nop);
      assert(inst.vector_op3 == VectorInstructions::op3_nop);
      saw_instructions = true;
    }
    delete param;
  }
  assert(saw_matrix && saw_vector && saw_instructions);
  params.clear();
#endif

  auto invalid = direct;
  (*invalid.mutable_prim()->mutable_kwargs())["output_via_vector"]
      .mutable_scalar()
      ->set_bool_value(true);
  invalid.mutable_outputs(0)->mutable_destination()->mutable_box()->set_dtype(
      "int24");
  bool rejected = false;
  try {
    map_operation(invalid, env, params);
  } catch (const std::runtime_error& error) {
    rejected =
        std::string(error.what())
            .find("Vector output does not support int24") != std::string::npos;
  }
  assert(rejected);
  std::cout << "Output routing checks passed\n";
  return 0;
}
