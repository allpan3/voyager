# Export resolved mapping targets and compare hardware build settings
import argparse
import json
from pathlib import Path
import subprocess

from voyager_compiler.mapping.results import write_if_changed, json_text
from voyager_compiler.mapping.target import ACCUMULATION_POLICY, CIMTarget, SATarget, load_target


# Resolve macro defaults by executing code compiled with the hardware build flags
def target_from_build(config_binary):
    values = json.loads(subprocess.check_output([str(config_binary.resolve())], text=True))
    if values["backend"] == "cim":
        return CIMTarget(accumulation_policy=ACCUMULATION_POLICY, **values)
    return SATarget(**values)


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
    parser.add_argument("action", choices=("export", "verify"))
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--config-binary", type=Path)
    parser.add_argument("--header", type=Path)
    parser.add_argument("--tilings", type=Path)
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



if __name__ == "__main__":
    main()
