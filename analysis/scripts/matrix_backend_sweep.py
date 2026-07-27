#!/usr/bin/env python3
"""Run isolated CIM and systolic RTL builds and SCVerify tests in parallel."""

import csv
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import asdict, dataclass
from datetime import datetime


WORKLOADS = [
    ("mobilebert_encoder", "mobilebert_encoder_layer_0_ffn_0_output_dense_fused"),
    ("mobilebert_encoder", "mobilebert_encoder_layer_0_output_bottleneck_dense_fused"),
    ("mobilebert_encoder", "mobilebert_encoder_layer_0_attention_output_dense_fused"),
    ("mobilebert_encoder", "matmul_6_fused"),
    ("mobilebert_encoder", "matmul_2_fused"),
    ("resnet18", "layer1_0_conv1_fused"),
    ("resnet18", "layer2_0_conv1_fused"),
    ("resnet18", "layer4_1_conv2_fused"),
]
LAYERS = [layer for _, layer in WORKLOADS]
ALLOWED_INCREMENTAL_BLOCKS = frozenset({"CIMArray", "CIMProcessor", "Accelerator"})
PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_VCS_GNU_PACKAGE = Path(
    "/cad/synopsys/vcs_gnu_package/S-2021.09/gnu_9/linux"
)
REQUIRED_VCS_HOME = Path("/cad/synopsys/vcs/T-2022.06-SP2")
VCS_GNU_SETUP_FILENAME = "source_me_gcc9.sh"
VCS_GNU_SCVERIFY_SETUP_FILENAME = "source_me_gcc9.csh"
VCS_GNU_COMPILER_ROOT = Path("gcc-9.2.0_64-shared")
DEFAULT_CATAPULT_BIN = Path("/cad/mentor/2024.2_2/Mgc_home/bin")
CIM_SET_ROWS = 64
SWEEP_RESULTS_ROOT = Path("cmp_results/runs")
RTL_PROVENANCE_FILENAME = ".matrix_backend_sweep_rtl_provenance.json"
RTL_PROVENANCE_SCHEMA_VERSION = 3
RTL_PROVENANCE_ENVIRONMENT_KEYS = (
    "DATATYPE",
    "INPUT_BUFFER_SIZE",
    "WEIGHT_BUFFER_SIZE",
    "ACCUM_BUFFER_SIZE",
    "CLOCK_PERIOD",
    "MATRIX_BACKEND",
    "IC_DIMENSION",
    "OC_DIMENSION",
    "TECHNOLOGY",
    "ENABLE_PERF_COUNTERS",
    "IC_PORT_WIDTH",
    "OC_PORT_WIDTH",
    "CIM_CH_IN",
    "CIM_CH_OUT",
    "CIM_B_SETS",
    "CIM_BASE_A_WIDTH",
    "CIM_BASE_B_WIDTH",
    "CIM_BASE_C_WIDTH",
    "CIM_WRITE_CH_IN",
    "CIM_MAC_LATENCY",
    "CIM_MODE",
    "CIM_TILE_INPUT_AXIS_ELEMENTS",
    "CIM_TILE_OUTPUT_AXIS_ELEMENTS",
    "CIM_INPUT_AXIS_TILES",
    "CIM_OUTPUT_AXIS_TILES",
    "CIM_A_PORT_TILES",
    "CIM_B_PORT_TILES",
    "CIM_C_PORT_TILES",
    "CIM_C_BEAT_LAYOUT",
    "CIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE",
    "CATAPULT_ROOT",
    "MGC_HOME",
    "VCS_HOME",
)
MANIFEST_FIELDS = [
    "config",
    "backend",
    "K",
    "N",
    "technology",
    "datatype",
    "logical_a_width_bits",
    "logical_b_width_bits",
    "macro_native_width_bits",
    "macro_base_a_width_bits",
    "macro_base_b_width_bits",
    "macro_base_c_width_bits",
    "ch_in",
    "ch_out",
    "input_axis_tiles",
    "output_axis_tiles",
    "tile_input_axis_elements",
    "tile_output_axis_elements",
    "cim_mac_latency",
    "cim_mode",
    "cim_b_sets",
    "cim_result_slots_per_output_lane",
    "ic_matched_port_bits",
    "oc_matched_port_bits",
    "baseline_geometry",
    "clock_period_ns",
    "port_widths",
]
WORKLOAD_MANIFEST_FIELDS = ["network", "layer", "requires_matrix_performance"]


@dataclass(frozen=True)
class Config:
    name: str
    backend: int
    k: int
    n: int
    datatype: str
    input_width: int
    weight_width: int
    native: int | None = None
    ch_in: int | None = None
    ch_out: int | None = None
    iat: int | None = None
    oat: int | None = None
    tie: int | None = None
    toe: int | None = None
    latency: int | None = None
    mode: int | None = None
    b_sets: int | None = None
    result_slots: int | None = None
    matched_only: bool = False


@dataclass(frozen=True)
class Run:
    config: Config
    width_name: str
    ic_bits: int
    oc_bits: int


# Return the modeled L1 weight-buffer depth for one backend configuration
def weight_buffer_depth(config):
    if config.backend == 1:
        if config.b_sets is None or config.b_sets <= 0:
            raise ValueError("CIM configurations require positive resident-set capacity")
        return CIM_SET_ROWS * config.b_sets
    return 1024


# Return a wall-clock label for progress output
def now():
    return datetime.now().strftime("%H:%M:%S")


# Read logical operand widths from the authoritative architecture configuration
def architecture_widths(datatype):
    script = (
        f"set DATATYPE {datatype}\n"
        "set IC_DIMENSION 32\n"
        "set OC_DIMENSION 32\n"
        "set MATRIX_BACKEND 0\n"
        "source scripts/architecture.tcl\n"
        "if {![string is integer -strict $INPUT_DTYPE_WIDTH] && [info exists IO_DATATYPE_WIDTH]} "
        "{set INPUT_DTYPE_WIDTH $IO_DATATYPE_WIDTH}\n"
        "if {![string is integer -strict $WEIGHT_DTYPE_WIDTH] && [info exists IO_DATATYPE_WIDTH]} "
        "{set WEIGHT_DTYPE_WIDTH $IO_DATATYPE_WIDTH}\n"
        'puts "$INPUT_DTYPE_WIDTH|$WEIGHT_DTYPE_WIDTH"\n'
    )
    result = subprocess.run(["tclsh"], input=script, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise SystemExit(f"cannot resolve {datatype} operand widths: {result.stderr.strip()}")
    try:
        input_width, weight_width = (int(value) for value in result.stdout.splitlines()[-1].split("|"))
    except (IndexError, ValueError):
        raise SystemExit(f"architecture.tcl returned non-numeric {datatype} operand widths: {result.stdout.strip()}")
    return input_width, weight_width


# Emit one legal CIM design point
def cim_config(datatype, input_width, weight_width, k, n, native, ch_in, ch_out, mode, latency, b_sets,
               result_slots, matched_only=False):
    n_per_element = ch_out * native // weight_width
    if ch_out * native % weight_width or k % ch_in or n % n_per_element:
        return None
    mode_name = "_serial" if mode else ""
    name = (
        f"cim{native}b{native}b{mode_name}_{k}x{n}_co{ch_out}_ci{ch_in}_sets{b_sets}"
        f"_rslots{result_slots}"
    )
    return Config(
        name=name,
        backend=1,
        k=k,
        n=n,
        datatype=datatype,
        input_width=input_width,
        weight_width=weight_width,
        native=native,
        ch_in=ch_in,
        ch_out=ch_out,
        iat=k // ch_in,
        oat=n // n_per_element,
        tie=1,
        toe=1,
        latency=latency,
        mode=mode,
        b_sets=b_sets,
        result_slots=result_slots,
        matched_only=matched_only,
    )


# Expand one legal CIM geometry into explicit result-slot counts
def cim_slot_configs(datatype, input_width, weight_width, k, n, native, ch_in,
                     ch_out, mode, latency, b_sets, result_slots):
    if k % ch_in:
        return []
    configs = []
    for slots in sorted(set(result_slots)):
        config = cim_config(
            datatype, input_width, weight_width, k, n, native, ch_in, ch_out,
            mode, latency, b_sets, slots,
        )
        if config:
            configs.append(config)
    return configs


# Build explicit parallel slot sweeps and one native serial point
def build_configs(datatype, input_width, weight_width, b_sets, parallel_latency, serial_latency):
    configs = []
    for k, n in [(32, 32), (64, 64)]:
        configs.append(
            Config(
                name=f"sa_{k}x{n}", backend=0, k=k, n=n, datatype=datatype,
                input_width=input_width, weight_width=weight_width,
            )
        )
        ch_in_values = [k] if k == 32 else [k, 32]
        for parallel_sets in dict.fromkeys([b_sets, 8]):
            for native in [8, 4]:
                for ch_in in ch_in_values:
                    iat = k // ch_in
                    result_slots = [iat, 6 if native == 8 else 4]
                    result_slots.append(8 if native == 8 else 6)
                    configs.extend(cim_slot_configs(
                        datatype, input_width, weight_width, k, n, native,
                        ch_in, 8, 0, parallel_latency, parallel_sets,
                        result_slots,
                    ))
    # Preserve completed co16 comparison points and add the requested 8-slot point
    configs.extend(cim_slot_configs(
        datatype, input_width, weight_width, 32, 32, 8, 32, 16,
        0, parallel_latency, b_sets, [1, 6, 8],
    ))
    # Preserve the completed matched-port 64x64 ci64 two-slot comparison
    configs.append(cim_config(
        datatype, input_width, weight_width, 64, 64, 8, 64, 8,
        0, parallel_latency, b_sets, 2, matched_only=True,
    ))
    configs.extend(cim_slot_configs(
        datatype, input_width, weight_width, 32, 32, 8, 32, 32,
        1, serial_latency, 2, [1],
    ))
    return [config for config in configs if config]


# Expand matched and baseline-width runs without duplicates
def build_runs(configs):
    runs = []
    for config in configs:
        matched = (config.k * config.input_width, config.n * config.weight_width)
        widths = [("matched", *matched)]
        if config.backend == 1 and not config.matched_only and matched != (256, 256):
            widths.append(("baseline", 256, 256))
        runs.extend(Run(config, name, ic_bits, oc_bits) for name, ic_bits, oc_bits in widths)
    return runs


# Identify mapper inputs while excluding result storage that cannot change tiling
def network_proto_key(config, network):
    return (
        network, config.backend, config.k, config.n, config.datatype,
        config.native, config.ch_in, config.ch_out, config.iat, config.oat,
        config.tie, config.toe, config.latency, config.mode, config.b_sets,
    )


# Write parser-compatible design metadata, optionally preserving unrelated rows
def write_manifest(path, configs, runs, technology, clock_period, append):
    widths = {}
    for run in runs:
        token = (
            f"{run.ic_bits}:{run.width_name}"
            if run.ic_bits == run.oc_bits
            else f"{run.ic_bits}x{run.oc_bits}:{run.width_name}"
        )
        widths.setdefault(run.config.name, []).append(token)
    selected = {config.name for config in configs}
    rows = []
    if append and path.exists():
        with path.open(newline="") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames != MANIFEST_FIELDS:
                raise SystemExit(f"cannot append to incompatible manifest: {path}")
            rows.extend(row for row in reader if row["config"] not in selected)
    for config in configs:
        rows.append(
            {
                "config": config.name,
                "backend": config.backend,
                "K": config.k,
                "N": config.n,
                "technology": technology,
                "datatype": config.datatype,
                "logical_a_width_bits": config.input_width,
                "logical_b_width_bits": config.weight_width,
                "macro_native_width_bits": config.native or "",
                "macro_base_a_width_bits": config.native or "",
                "macro_base_b_width_bits": config.native or "",
                "macro_base_c_width_bits": (config.native + 16) if config.native else "",
                "ch_in": config.ch_in or "",
                "ch_out": config.ch_out or "",
                "input_axis_tiles": config.iat or "",
                "output_axis_tiles": config.oat or "",
                "tile_input_axis_elements": config.tie or "",
                "tile_output_axis_elements": config.toe or "",
                "cim_mac_latency": config.latency or "",
                "cim_mode": config.mode if config.mode is not None else "",
                "cim_b_sets": config.b_sets or "",
                "cim_result_slots_per_output_lane": config.result_slots or "",
                "ic_matched_port_bits": config.k * config.input_width,
                "oc_matched_port_bits": config.n * config.weight_width,
                "baseline_geometry": "32x32",
                "clock_period_ns": clock_period,
                "port_widths": " ".join(widths[config.name]),
            }
        )
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=MANIFEST_FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    os.replace(temporary, path)


# Record the declared comparison workloads and their network contexts
def write_workload_manifest(path, workloads):
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=WORKLOAD_MANIFEST_FIELDS)
        writer.writeheader()
        for network, layer in workloads:
            writer.writerow(
                {
                    "network": network,
                    "layer": layer,
                    "requires_matrix_performance": "yes",
                }
            )
    os.replace(temporary, path)


# Require the VCS 2022 GCC 9 compatibility package to be active
def vcs_gnu_package(env):
    package = Path(env.get("VG_GNU_PACKAGE", DEFAULT_VCS_GNU_PACKAGE)).resolve()
    for setup_filename in (
        VCS_GNU_SETUP_FILENAME,
        VCS_GNU_SCVERIFY_SETUP_FILENAME,
    ):
        setup = package / setup_filename
        if not setup.is_file():
            raise RuntimeError(f"missing VCS GNU setup: {setup}")
    compiler = shutil.which("gcc", path=env.get("PATH"))
    if compiler is None:
        raise RuntimeError("GCC is missing from PATH")
    try:
        Path(compiler).resolve().relative_to(package / VCS_GNU_COMPILER_ROOT)
    except ValueError as error:
        raise RuntimeError(
            f"active GCC is not from the VCS GCC 9 package: {compiler}"
        ) from error
    return package


# Construct the complete environment for one hardware instance
def run_environment(run, technology, clock_period):
    config = run.config
    env = os.environ.copy()
    python_paths = [
        str(PROJECT_ROOT / "voyager-compiler" / "src"),
        str(PROJECT_ROOT / "interstellar" / "src"),
    ]
    if env.get("PYTHONPATH"):
        python_paths.append(env["PYTHONPATH"])
    env["PYTHONPATH"] = os.pathsep.join(python_paths)
    if not env.get("MGLS_LICENSE_FILE"):
        raise RuntimeError(
            "Catapult license environment is missing; load module catapult/2024.2_2"
        )
    vcs_home = Path(env.get("VCS_HOME", "")).resolve()
    if not env.get("VCS_HOME") or not (vcs_home / "bin" / "vcs").is_file():
        raise RuntimeError(
            "VCS environment is missing; load module vcs/T-2022.06-SP2"
        )
    required_vcs_home = REQUIRED_VCS_HOME.resolve()
    if vcs_home != required_vcs_home:
        raise RuntimeError(
            f"wrong VCS_HOME: expected {required_vcs_home}, found {vcs_home}"
        )
    env["VCS_HOME"] = str(vcs_home)
    gnu_package = vcs_gnu_package(env)
    env["VG_GNU_PACKAGE"] = str(gnu_package)
    env.update(
        {
            "DATATYPE": config.datatype,
            "INPUT_BUFFER_SIZE": "1024",
            "WEIGHT_BUFFER_SIZE": str(weight_buffer_depth(config)),
            "ACCUM_BUFFER_SIZE": "1024",
            "CLOCK_PERIOD": str(clock_period),
            "NETWORK": WORKLOADS[0][0],
            "MATRIX_BACKEND": str(config.backend),
            "IC_DIMENSION": str(config.k),
            "OC_DIMENSION": str(config.n),
            "TECHNOLOGY": technology,
            "ENABLE_PERF_COUNTERS": "1",
        }
    )
    catapult = DEFAULT_CATAPULT_BIN / "catapult"
    if not catapult.is_file():
        raise RuntimeError(f"missing Catapult executable: {catapult}")
    env["PATH"] = os.pathsep.join(
        [
            str(gnu_package / VCS_GNU_COMPILER_ROOT / "bin"),
            str(DEFAULT_CATAPULT_BIN),
            env["PATH"],
        ]
    )
    if run.width_name == "matched":
        env.pop("IC_PORT_WIDTH", None)
        env.pop("OC_PORT_WIDTH", None)
    else:
        env["IC_PORT_WIDTH"] = str(run.ic_bits)
        env["OC_PORT_WIDTH"] = str(run.oc_bits)
    if config.backend:
        env.update(
            {
                "CIM_CH_IN": str(config.ch_in),
                "CIM_CH_OUT": str(config.ch_out),
                "CIM_B_SETS": str(config.b_sets),
                "CIM_BASE_A_WIDTH": str(config.native),
                "CIM_BASE_B_WIDTH": str(config.native),
                "CIM_BASE_C_WIDTH": str(config.native + 16),
                "CIM_WRITE_CH_IN": "1",
                "CIM_MAC_LATENCY": str(config.latency),
                "CIM_MODE": str(config.mode),
                "CIM_TILE_INPUT_AXIS_ELEMENTS": str(config.tie),
                "CIM_TILE_OUTPUT_AXIS_ELEMENTS": str(config.toe),
                "CIM_INPUT_AXIS_TILES": str(config.iat),
                "CIM_OUTPUT_AXIS_TILES": str(config.oat),
                "CIM_A_PORT_TILES": str(config.iat),
                "CIM_B_PORT_TILES": str(config.oat),
                "CIM_C_PORT_TILES": str(config.oat),
                "CIM_C_BEAT_LAYOUT": "1",
                "CIM_ARRAY_RESULT_SLOTS_PER_OUTPUT_LANE": str(config.result_slots),
            }
        )
    return env


# Return the unique output tag for one config and port width
def run_tag(run):
    if run.width_name == "matched":
        return run.config.name
    suffix = f"_pw{run.ic_bits}" if run.ic_bits == run.oc_bits else f"_pw{run.ic_bits}x{run.oc_bits}"
    return run.config.name + suffix


# Run a command with one complete log
def logged_command(command, log_path, env, cwd=None, timeout=None):
    with log_path.open("w") as stream:
        try:
            result = subprocess.run(
                command,
                cwd=cwd,
                env=env,
                stdout=stream,
                stderr=subprocess.STDOUT,
                timeout=timeout,
            )
            return result.returncode
        except subprocess.TimeoutExpired:
            stream.write("\nTIMEOUT\n")
            return 124


# Ask Make for one scalar configuration value
def make_value(target, env):
    output = subprocess.check_output(["make", "-s", target], env=env, text=True)
    return [line for line in output.splitlines() if line.strip()][-1].strip()


# Return the provenance stamp path owned by one generated RTL directory
def rtl_provenance_stamp_path(rtl_dir):
    return rtl_dir / RTL_PROVENANCE_FILENAME


# Describe the exact source and build configuration expected for one RTL run
def rtl_provenance(run, build_folder, rtl_dir, rtl_makefile, rtl_args,
                   technology, clock_period, env):
    return {
        "schema_version": RTL_PROVENANCE_SCHEMA_VERSION,
        "tag": run_tag(run),
        "config": asdict(run.config),
        "run": {
            "width_name": run.width_name,
            "ic_bits": run.ic_bits,
            "oc_bits": run.oc_bits,
        },
        "build": {
            "build_folder": str(build_folder.resolve()),
            "rtl_dir": str(rtl_dir.resolve()),
            "rtl_makefile": rtl_makefile,
            "rtl_args": list(rtl_args),
            "technology": technology,
            "clock_period_ns": str(clock_period),
            "environment": {
                key: env.get(key)
                for key in RTL_PROVENANCE_ENVIRONMENT_KEYS
            },
        },
    }


# Return whether a provenance stamp exactly matches and why it does not
def matching_rtl_provenance(stamp_path, expected):
    try:
        actual = json.loads(stamp_path.read_text())
    except FileNotFoundError:
        return False, f"missing {stamp_path}"
    except json.JSONDecodeError as error:
        return False, f"malformed {stamp_path}: {error.msg}"
    except OSError as error:
        return False, f"cannot read {stamp_path}: {error}"
    if not isinstance(actual, dict):
        return False, f"invalid {stamp_path}: expected a JSON object"
    if actual == expected:
        return True, ""
    differing = sorted(
        key for key in set(actual) | set(expected)
        if actual.get(key) != expected.get(key)
    )
    return False, f"mismatched {stamp_path} fields: {', '.join(differing)}"


# Atomically record provenance only after the full RTL target succeeds
def write_rtl_provenance_stamp(stamp_path, provenance):
    temporary = stamp_path.with_suffix(stamp_path.suffix + ".tmp")
    temporary.write_text(json.dumps(provenance, indent=2, sort_keys=True) + "\n")
    os.replace(temporary, stamp_path)


# Return whether one layer log contains its required functional and counter evidence
def valid_layer_log(path, layer):
    if not path.exists():
        return False
    text = path.read_text(errors="replace")
    return (
        "Error count: 0" in text
        and "MatrixPerfHardware:" in text
    )


# Build the explicit hardware, workload, and profile completion identity
def completion_identity(run, workloads):
    config = asdict(run.config)
    return {
        "schema_version": 2,
        "backend": "cim" if run.config.backend else "sa",
        "hardware": {
            "tag": run_tag(run),
            "configuration": config,
            "capacity": config.get("b_sets") if run.config.backend else None,
            "port_width_name": run.width_name,
            "input_port_bits": run.ic_bits,
            "output_port_bits": run.oc_bits,
        },
        "workloads": [
            {
                "network": network,
                "layer": layer,
                "tiling_profile": "interstellar-default",
            }
            for network, layer in workloads
        ],
    }


# Return whether a prior build exactly matches the current source and has all requested valid layer logs
def completed_build(run, results, workloads):
    tag = run_tag(run)
    stamp_path = results / f"{tag}.complete.json"
    try:
        stamp = json.loads(stamp_path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return False
    identity = completion_identity(run, workloads)
    if (
        stamp.get("schema_version") != 2
        or stamp.get("identity") != identity
    ):
        return False
    return all(
        valid_layer_log(results / f"{tag}__rtl__{layer}.log", layer)
        for _, layer in workloads
    )


# Write one successful build stamp atomically
def write_completion_stamp(run, results, workloads):
    tag = run_tag(run)
    stamp_path = results / f"{tag}.complete.json"
    temporary = stamp_path.with_suffix(stamp_path.suffix + ".tmp")
    identity = completion_identity(run, workloads)
    temporary.write_text(
        json.dumps(
            {
                "schema_version": 2,
                "identity": identity,
                "completed_at": datetime.now().isoformat(timespec="seconds"),
            },
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )
    os.replace(temporary, stamp_path)


# Require one dry-run Make plan to contain only the approved incremental Catapult blocks
def incremental_preflight(run, results, technology, clock_period):
    tag = run_tag(run)
    env = run_environment(run, technology, clock_period)
    result = subprocess.run(["make", "-n", "rtl"], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log_path = results / f"{tag}_incremental_preflight.log"
    log_path.write_text(result.stdout)
    blocks = set(re.findall(r"\bBLOCK=([A-Za-z0-9_]+)\b", result.stdout))
    unexpected = blocks - ALLOWED_INCREMENTAL_BLOCKS
    if result.returncode:
        print(f"=== [{now()}] PREFLIGHT MAKE FAILED {tag}; see {log_path} ===", flush=True)
        return False
    if unexpected:
        print(f"=== [{now()}] PREFLIGHT REJECTED {tag}: unexpected blocks {' '.join(sorted(unexpected))} ===", flush=True)
        return False
    block_label = " ".join(sorted(blocks)) if blocks else "none (already up to date)"
    print(f"=== [{now()}] PREFLIGHT OK {tag}: {block_label} ===", flush=True)
    return True


# Run one direct SCVerify layer, retrying the known transient make lookup failure
def run_layer(workload, tag, results, env, rtl_dir, rtl_makefile, rtl_args):
    network, layer = workload
    log = results / f"{tag}__rtl__{layer}.log"
    sim_env = env.copy()
    sim_env.update(
        {
            "TESTS": layer,
            "SIMS": "gold,accelerator",
            "NETWORK": network,
            "LD_PRELOAD": f"{env['CONDA_PREFIX']}/lib/libstdc++.so.6",
        }
    )
    gnu_package = vcs_gnu_package(sim_env)
    sim_env["VG_GNU_PACKAGE"] = str(gnu_package)
    make_args = [
        *rtl_args,
        f"VCS_VG_GNU_PACKAGE={gnu_package}",
        f"VCS_VG_ENV64_SCRIPT={VCS_GNU_SCVERIFY_SETUP_FILENAME}",
    ]
    if os.environ.get("SCVERIFY_KDB") != "1":
        make_args.extend(["KDB_ELAB=", "KDB=", "DEBUG_ELAB="])
    for _ in range(3):
        rc = logged_command(
            ["make", "-f", f"scverify/{rtl_makefile}", *make_args, "sim"],
            log,
            sim_env,
            cwd=rtl_dir,
            timeout=10 * 60 * 60,
        )
        text = log.read_text(errors="replace")
        if "No rule to make target" not in text:
            return rc == 0 and valid_layer_log(log, layer)
    return False


# Synthesize when requested and directly cosimulate one isolated hardware configuration
def run_hardware(run, results, technology, clock_period, sim_workers, workloads, sim_only):
    tag = run_tag(run)
    env = run_environment(run, technology, clock_period)
    build_folder = Path(make_value("print-build-dir", env)).resolve()
    rtl_config = make_value("print-scverify-rtl-config", env)
    rtl_makefile, _, args = rtl_config.partition("|")
    rtl_args = shlex.split(args)
    rtl_dir = build_folder / "Catapult" / env["TECHNOLOGY"] / f"clock_{clock_period}" / "Accelerator" / "Accelerator.v1"
    provenance_path = rtl_provenance_stamp_path(rtl_dir)
    expected_provenance = rtl_provenance(
        run, build_folder, rtl_dir, rtl_makefile, rtl_args,
        technology, clock_period, env,
    )
    print(f"=== [{now()}] START {tag} ===", flush=True)
    if sim_only:
        if not (rtl_dir / "scverify" / rtl_makefile).exists():
            print(f"=== [{now()}] MISSING RTL {tag}: {rtl_dir} ===", flush=True)
            return False
        provenance_matches, reason = matching_rtl_provenance(
            provenance_path, expected_provenance,
        )
        if not provenance_matches:
            print(
                f"=== [{now()}] RTL PROVENANCE REJECTED {tag}: {reason}; "
                "regenerate without SIM_ONLY ===",
                flush=True,
            )
            return False
        print(f"=== [{now()}] REUSING VERIFIED RTL {tag} ===", flush=True)
    else:
        (results / f"{tag}.complete.json").unlink(missing_ok=True)
        provenance_path.unlink(missing_ok=True)
        hls_jobs = int(os.environ.get("HLS_JOBS", "8"))
        rtl_rc = logged_command(
            ["make", f"-j{hls_jobs}", "rtl"],
            results / f"{tag}_rtl_gen.log",
            env,
        )
        if rtl_rc:
            print(f"=== [{now()}] RTL GEN FAILED {tag} ===", flush=True)
            return False
        if not (rtl_dir / "scverify" / rtl_makefile).exists():
            print(f"=== [{now()}] GENERATED RTL MISSING {tag}: {rtl_dir} ===", flush=True)
            return False
        write_rtl_provenance_stamp(provenance_path, expected_provenance)
        print(f"=== [{now()}] RTL GENERATED {tag} ===", flush=True)
    # Build the shared SCVerify executable once before parallel layer runs
    first_passed = run_layer(workloads[0], tag, results, env, rtl_dir, rtl_makefile, rtl_args)
    with ThreadPoolExecutor(max_workers=max(1, min(sim_workers, len(workloads) - 1))) as executor:
        futures = [
            executor.submit(run_layer, workload, tag, results, env, rtl_dir, rtl_makefile, rtl_args)
            for workload in workloads[1:]
        ]
        passed = int(first_passed) + sum(future.result() for future in futures)
    print(f"=== [{now()}] RTL SIMS {tag}: {passed}/{len(workloads)} passed ===", flush=True)
    if passed == len(workloads):
        if not sim_only:
            write_completion_stamp(run, results, workloads)
        return True
    return False


USAGE = """usage: matrix_backend_sweep.py cmp_results/runs/<run-name> [config ...]

Runs isolated RTL builds and SCVerify tests for the CIM/systolic design space.
With no config names every design point runs; naming configs restricts the
sweep to those. Run records must stay under cmp_results/runs/<run-name>.

Environment knobs:
  CIM_B_SETS                 resident weight sets for the primary space (default 8)
  CIM_PARALLEL_MAC_LATENCY   macro MAC latency, bit-parallel points (default 3)
  CIM_SERIAL_MAC_LATENCY     macro MAC latency, bit-serial points (default 1)
  CIM_OUTPUT_BOTTLENECK_SET_TILING
                              output-bottleneck experiment: 1=IC, 2=OC, 3=footprint-balanced
  CIM_OUTPUT_BOTTLENECK_RESIDENT_X
                              override the experiment's inner OX bound
  CIM_OUTPUT_BOTTLENECK_RESIDENT_OC
                              override the experiment's inner OC bound
  CLOCK_PERIOD               ns (default 5)
  TECHNOLOGY                 technology path below scripts/tech (default generic)
                              e.g. internal-tech/tsmc7
  PARALLEL_BUILDS            builds in flight (default 10)
  PARALLEL_SIMS_PER_BUILD    sims in flight per build (default 5)
  HLS_JOBS                   Make jobs within each build (default 8)
  APPEND_RESULTS=1           merge selected configs into an existing manifest
  RUN_TAGS="tag ..."         run only the named port-width builds while retaining all widths in the manifest
  RUN_LAYERS="layer ..."     run only the named layers in their declared order
  SIM_ONLY=1                 reuse existing RTL and rerun direct SCVerify only
  INCREMENTAL_ONLY=1         abort unless make -n needs only CIMArray/CIMProcessor/Accelerator
  RESUME=1                   skip source-matched builds with all complete performance logs
  SCVERIFY_KDB=1             generate optional Verdi KDB data (disabled by default)
  DRY_RUN=1                  list the builds and exit without running them

Parallel native-8 points include 8 result slots per output lane, and parallel
native-4 points include 6. Existing comparison counts are retained without any
inferred capacity guarantee. The native bit-serial point uses its single default
slot."""


# Run the selected design points with bounded configuration concurrency
def main():
    args = sys.argv[1:]
    if any(arg in ("-h", "--help") for arg in args):
        print(USAGE)
        return
    if not args:
        raise SystemExit(USAGE)
    # A leading dash is always a mistyped option, never a results directory.
    # Accepting one silently turns it into a directory name and launches the
    # whole sweep, which is how a stray "--help" once ran for fifteen hours
    if args[0].startswith("-"):
        raise SystemExit(
            f"unknown option: {args[0]}\n\n{USAGE}")
    results_root = (Path.cwd() / SWEEP_RESULTS_ROOT).resolve()
    results = Path(args[0]).resolve()
    try:
        results.relative_to(results_root)
    except ValueError:
        raise SystemExit(
            f"results directory must be under {results_root}: {results}"
        )
    if results == results_root:
        raise SystemExit("results directory must name one run under cmp_results/runs")
    results.mkdir(parents=True, exist_ok=True)
    selected = set(args[1:])
    b_sets = int(os.environ.get("CIM_B_SETS", "8"))
    parallel_latency = int(os.environ.get("CIM_PARALLEL_MAC_LATENCY", "3"))
    serial_latency = int(os.environ.get("CIM_SERIAL_MAC_LATENCY", "1"))
    clock_period = os.environ.get("CLOCK_PERIOD", "5")
    technology = os.environ.get("TECHNOLOGY", "generic")
    workers = int(os.environ.get("PARALLEL_BUILDS", "10"))
    sim_workers = int(os.environ.get("PARALLEL_SIMS_PER_BUILD", "5"))
    selected_layers = set(os.environ.get("RUN_LAYERS", "").split())
    unknown_layers = selected_layers - set(LAYERS)
    if unknown_layers:
        raise SystemExit(f"unknown layers: {', '.join(sorted(unknown_layers))}")
    workloads = [
        workload for workload in WORKLOADS
        if not selected_layers or workload[1] in selected_layers
    ]
    sim_only = os.environ.get("SIM_ONLY") == "1"
    if sim_only and (os.environ.get("RESUME") == "1" or os.environ.get("INCREMENTAL_ONLY") == "1"):
        raise SystemExit("SIM_ONLY cannot be combined with RESUME or INCREMENTAL_ONLY")
    datatype = os.environ.get("DATATYPE", "INT8")
    input_width, weight_width = architecture_widths(datatype)
    configs = build_configs(datatype, input_width, weight_width, b_sets, parallel_latency, serial_latency)
    if selected:
        unknown = selected - {config.name for config in configs}
        if unknown:
            raise SystemExit(f"unknown configs: {', '.join(sorted(unknown))}")
        configs = [config for config in configs if config.name in selected]
    manifest_runs = build_runs(configs)
    selected_run_tags = set(os.environ.get("RUN_TAGS", "").split())
    if selected_run_tags:
        unknown = selected_run_tags - {run_tag(run) for run in manifest_runs}
        if unknown:
            raise SystemExit(f"unknown run tags: {', '.join(sorted(unknown))}")
        runs = [run for run in manifest_runs if run_tag(run) in selected_run_tags]
    else:
        runs = manifest_runs
    append_results = os.environ.get("APPEND_RESULTS") == "1"
    write_manifest(
        results / "manifest.csv", configs, manifest_runs,
        technology, clock_period, append_results,
    )
    write_workload_manifest(results / "workloads.csv", workloads)
    print(
        f"RTL-only sweep: {len(configs)} configs, {len(runs)} builds, "
        f"{workers} parallel builds, {sim_workers} sims/build, "
        f"{len(workloads)} workloads, technology={technology}, "
        f"clock_period={clock_period}ns, sim_only={sim_only}",
        flush=True,
    )
    if os.environ.get("DRY_RUN") == "1":
        for run in runs:
            print(
                f"{run_tag(run)} mode={run.config.mode} sets={run.config.b_sets} "
                f"latency={run.config.latency} slots={run.config.result_slots} "
                f"ports={run.ic_bits}/{run.oc_bits}",
                flush=True,
            )
        return
    if os.environ.get("INCREMENTAL_ONLY") == "1":
        rejected = [
            run_tag(run)
            for run in runs
            if not incremental_preflight(run, results, technology, clock_period)
        ]
        if rejected:
            raise SystemExit(f"INCREMENTAL PREFLIGHT FAILED {len(rejected)} builds: {' '.join(rejected)}")
        print(f"INCREMENTAL PREFLIGHT PASSED {len(runs)}/{len(runs)} builds", flush=True)
    if os.environ.get("RESUME") == "1":
        pending = []
        for run in runs:
            if completed_build(run, results, workloads):
                print(f"=== [{now()}] RESUME SKIP {run_tag(run)} ===", flush=True)
            else:
                pending.append(run)
        runs = pending
        print(f"RESUME: {len(runs)} builds pending", flush=True)
        if not runs:
            print(f"ALL DONE {now()}", flush=True)
            return
    # Generate shared codegen inputs serially before concurrent Make processes
    if not sim_only:
        pending_configs = {run.config for run in runs}
        generated_proto = set()
        for config in configs:
            if config not in pending_configs:
                continue
            run = next(run for run in runs if run.config == config)
            for network in dict.fromkeys(network for network, _ in workloads):
                proto_key = network_proto_key(config, network)
                if proto_key in generated_proto:
                    continue
                env = run_environment(run, technology, clock_period)
                env["NETWORK"] = network
                proto_rc = logged_command(
                    ["make", "network-proto"],
                    results / f"{config.name}_{network}_proto.log",
                    env,
                )
                if proto_rc:
                    raise SystemExit(f"PROTO FAILED {config.name} {network}")
                generated_proto.add(proto_key)
    with ThreadPoolExecutor(max_workers=workers) as executor:
        future_to_run = {
            executor.submit(
                run_hardware, run, results, technology, clock_period,
                sim_workers, workloads, sim_only,
            ): run
            for run in runs
        }
        failures = []
        for future in as_completed(future_to_run):
            run = future_to_run[future]
            try:
                passed = future.result()
            except Exception as error:
                print(f"=== [{now()}] EXCEPTION {run_tag(run)}: {error} ===", flush=True)
                passed = False
            if not passed:
                failures.append(run_tag(run))
    if failures:
        raise SystemExit(f"FAILED {len(failures)} builds: {' '.join(sorted(failures))}")
    print(f"ALL DONE {now()}", flush=True)


if __name__ == "__main__":
    main()
