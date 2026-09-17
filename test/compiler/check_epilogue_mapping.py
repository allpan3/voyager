# Check exported vector metadata and both mapping backends on tiny fused layers
import argparse
import json
from pathlib import Path

from google.protobuf import text_format
from voyager_compiler.codegen import param_pb2
from voyager_compiler.mapping.results import write_tilings
from voyager_compiler.mapping.driver import export_epilogues, generate_tilings
from voyager_compiler.mapping.target import CIMTarget, SATarget
from voyager_compiler.mapping.operations import evaluate_operation_epilogue


# Use a 16x16 matrix with a scalar dequantization or one external vector operand
def tiny_model(epilogue="dequantize"):
    model = param_pb2.Model()
    op = model.ops.add()
    op.fused_op.name = "dense"
    matrix = op.fused_op.op_list.add(name="linear", target="linear")
    for key, shape, address in (("input", [1, 16, 16], 0), ("weight", [16, 16], 256)):
        tensor = matrix.kwargs[key].tensor
        tensor.shape.extend(shape)
        tensor.dtype = "int8"
        tensor.memory.address = address
    vector = op.fused_op.op_list.add(target=epilogue)
    vector.kwargs["input"].tensor.shape.extend([1, 16, 16])
    vector.kwargs["input"].tensor.dtype = "int24"
    tensor = vector.kwargs["scale" if epilogue == "dequantize" else "other"].tensor
    tensor.shape.extend([1] if epilogue == "dequantize" else [16])
    tensor.dtype = "bfloat16"
    tensor.memory.address = 512
    if epilogue == "add":
        op.fused_op.op_list.add(target="relu")
    op.output.shape.extend([1, 16, 16])
    op.output.dtype = "bfloat16" if epilogue == "dequantize" else "int8"
    op.output.memory.address = 1024
    return model


# Keep matrix geometry identical while exercising independent SA and CIM evaluators
def targets():
    sa = SATarget(k=8, n=8, input_buffer_words=256, weight_buffer_words=256,
                 accum_buffer_words=32, double_buffered_accum=False, ic_port_bits=64,
                 oc_port_bits=64, input_bits=8, weight_bits=8, accum_bits=24, datatype="INT8", vector_config=dict(lanes=8, output_fifo_packets=8))
    cim = CIMTarget(datatype="INT8", input_bits=8, weight_bits=8, accum_bits=24, ch_in=8, ch_out=8, b_sets=8, base_a_width=8,
                    base_b_width=8, base_c_width=24, write_ch_in=1, mac_latency=3, mode=0,
                    tile_input_axis_elements=1, tile_output_axis_elements=1,
                    input_axis_tiles=1, output_axis_tiles=1, a_port_tiles=1, b_port_tiles=1,
                    c_port_tiles=1, c_beat_layout=1, result_slots_per_output_lane=8,
                    local_accum_contexts=4, input_buffer_words=256, accum_buffer_words=32,
                    double_buffered_accum=False, ic_port_bits=64, oc_port_bits=64,
                    accumulation_policy="loop-lifetime-prefix", vector_config=dict(lanes=8, output_fifo_packets=8))
    return sa, cim


# Check exported epilogue descriptions through tiny searches and result writing
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exporter", type=Path, required=True)
    parser.add_argument("--output_dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    model = tiny_model()
    epilogues = export_epilogues(model, args.exporter)
    assert epilogues["dense"]["passes"][0]["dequantize"]
    for target in targets():
        tilings, report = generate_tilings(model, target, epilogues=epilogues)
        assert report["operations"][0]["vector_unit"]["cycles_per_vector"] == 2
        output = args.output_dir / target.backend
        write_tilings(tilings, report, output)
        saved = json.loads((output / "mapping-evaluations.json").read_text())
        assert saved["operations"][0]["metrics"] == report["operations"][0]["metrics"]
        assert (output / "tilings.txtpb").read_text() == text_format.MessageToString(tilings)
    model = tiny_model("add")
    epilogues = export_epilogues(model, args.exporter)
    assert epilogues["dense"]["passes"][0]["stages"] == ["add", "relu", "", ""]
    timing = evaluate_operation_epilogue(targets()[0], model.ops[0], epilogues["dense"])
    assert dict(timing.resource_cycles_per_vector) == dict(vector=1, output=1, fetch1=2)
    (args.output_dir / "model.txt").write_text(text_format.MessageToString(model))
    for target in targets():
        tilings, report = generate_tilings(model, target, epilogues=epilogues)
        (args.output_dir / (target.backend + "-tilings.txtpb")).write_text(text_format.MessageToString(tilings))
    # Renaming an already lowered operation does not change the resource calculation
    model.ops[0].fused_op.op_list[1].target = "new_frontend_name"
    assert evaluate_operation_epilogue(targets()[0], model.ops[0], epilogues["dense"]) == timing
    # Invalid fusion remains the command generator's decision
    model = tiny_model("add")
    model.ops[0].fused_op.op_list.add(target="relu")
    try:
        export_epilogues(model, args.exporter)
    except ValueError as error:
        assert "does not fit the hardware pipeline" in str(error)
    else:
        raise AssertionError("invalid fusion was accepted")
    print("Epilogue mapping smoke passed: C++ export, SA/CIM search, results, and port widths")
