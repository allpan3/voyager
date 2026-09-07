# Export resolved mapping targets and compare hardware build settings
import argparse
import json
from pathlib import Path
import subprocess
import shlex

from voyager_compiler.mapping.results import write_if_changed, json_text
from voyager_compiler.mapping.target import ACCUMULATION_POLICY, CIMTarget, SATarget, load_target


# Resolve macro defaults by executing code compiled with the hardware build flags
def target_from_build(config_binary):
    values = json.loads(subprocess.check_output([str(config_binary.resolve())], text=True))
    if values["backend"] == "cim":
        return CIMTarget(accumulation_policy=ACCUMULATION_POLICY, **values)
    return SATarget(**values)


# Compare the recorded target and HLS settings before using an RTL artifact
def check_artifact(target, artifact, receipt, settings=None):
    record = json.loads(receipt.read_text())
    if record["target"] != target.to_dict():
        raise ValueError("hardware build does not match the mapping target; rebuild RTL")
    for name, expected in (settings or {}).items():
        if record["hls_settings"].get(name) != expected:
            raise ValueError(f"hardware build {name} differs from the requested HLS option")
    if not artifact.is_file():
        raise ValueError("RTL artifact is missing; rebuild RTL")


# Compare effective HLS preprocessor values with the compiled parameter export
def verify_hls_settings(target, settings):
    defines = dict((flag[2:].split("=", 1) + ["1"])[:2]
                   for flag in shlex.split(settings["CompilerFlags"]) if flag.startswith("-D"))
    expected = dict(IC_DIMENSION=target.k, OC_DIMENSION=target.n,
                    IC_PORT_WIDTH=target.ic_port_bits, OC_PORT_WIDTH=target.oc_port_bits,
                    INPUT_BUFFER_SIZE=target.input_buffer_words, ACCUM_BUFFER_SIZE=target.accum_buffer_words,
                    DOUBLE_BUFFERED_ACCUM_BUFFER=target.double_buffered_accum,
                    MATRIX_BACKEND=int(target.backend == "cim"))
    expected.update({name.upper(): value for name, value in target.hardware_options.items()})
    if target.backend == "sa":
        expected["WEIGHT_BUFFER_SIZE"] = target.weight_buffer_words
    else:
        fields = ("ch_in", "ch_out", "b_sets", "base_a_width", "base_b_width", "base_c_width",
                  "write_ch_in", "mac_latency", "mode", "tile_input_axis_elements", "tile_output_axis_elements",
                  "input_axis_tiles", "output_axis_tiles", "a_port_tiles", "b_port_tiles", "c_port_tiles",
                  "c_beat_layout", "local_accum_contexts")
        expected.update({"CIM_" + name.upper(): getattr(target, name) for name in fields})
        expected["CIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE"] = target.result_slots_per_output_lane
    if target.datatype not in defines:
        raise ValueError("HLS datatype differs from the mapping target")
    for name, value in expected.items():
        actual = defines.get(name)
        allowed = (str(value).lower(), str(int(value))) if type(value) is bool else (str(value),)
        if actual not in allowed:
            raise ValueError(f"HLS {name}={actual} differs from resolved target {value}")


# Validate generated tilings against the effective hardware configuration
def check_tilings(target, path):
    from google.protobuf import text_format
    from voyager_compiler.codegen import tiling_pb2
    tilings = text_format.Parse(path.read_text(), tiling_pb2.ModelTiling())
    if tilings.backend != target.backend or json.loads(tilings.target_configuration) != target.to_dict():
        raise ValueError("tilings do not match the mapping target; run make network-proto")


# Export targets or verify the artifacts consumed by the selected build path
def main():
    parser = argparse.ArgumentParser(description="Export and verify resolved mapping targets")
    parser.add_argument("action", choices=("export", "begin", "finish", "check"))
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--config-binary", type=Path)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--artifact", type=Path)
    parser.add_argument("--tilings", type=Path)
    parser.add_argument("--setting", action="append", default=[])
    args = parser.parse_args()
    if args.action == "export":
        target = target_from_build(args.config_binary)
        write_if_changed(args.target, json_text(target.to_dict()))
        if args.header:
            write_if_changed(args.header, '// Store the resolved mapping target consumed by the runner\n'
                             '#pragma once\nstatic constexpr const char* MAPPING_TARGET_CONFIGURATION = '
                             + json.dumps(target.configuration_json) + ';\n')
        return
    target = load_target(args.target)
    if args.tilings:
        check_tilings(target, args.tilings)
    if args.artifact:
        receipt = args.artifact.with_name("mapping-build.json")
        # Do not create a future Catapult solution directory before synthesis
        pending = args.artifact.parent.parent / (args.artifact.parent.name + "-mapping-pending.json")
        if args.action == "begin":
            settings = dict(item.split("=", 1) for item in args.setting)
            verify_hls_settings(target, settings)
            write_if_changed(pending, json_text(dict(target=target.to_dict(),
                                                     hls_settings=settings)))
            receipt.unlink(missing_ok=True)  # A failed rebuild must not retain a success receipt
        elif args.action == "finish":
            record = json.loads(pending.read_text())
            if record["target"] != target.to_dict():
                raise ValueError("hardware configuration changed during synthesis")
            if not args.artifact.is_file():
                raise ValueError("RTL artifact is missing; synthesis did not produce output")
            write_if_changed(receipt, json_text(record))
            pending.unlink()
        elif args.action == "check":
            check_artifact(target, args.artifact, receipt, dict(item.split("=", 1) for item in args.setting))


if __name__ == "__main__":
    main()
