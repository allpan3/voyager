"""Validated data model for the 71-point CIM-versus-SA RTL sweep."""

from __future__ import annotations

import csv
import math
import statistics
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

ANALYSIS_DIR = Path(__file__).resolve().parents[2]
REPO_ROOT = ANALYSIS_DIR.parent
DEFAULT_SUMMARY = (
    REPO_ROOT
    / "cmp_results/runs/019fc8ce-df84-7d40-be91-180a41d1ff07/sweep/summary.csv"
)
DEFAULT_PD_CONFIG = "cim8b8b_64x64_co8_ci64_sets18_rslots8"
DEFAULT_PD_PORTS = (512, 512)

LAYER_LABELS = {
    "mobilebert_encoder_layer_0_ffn_0_output_dense_fused": "MBERT FFN output",
    "mobilebert_encoder_layer_0_output_bottleneck_dense_fused": "MBERT bottleneck",
    "mobilebert_encoder_layer_0_attention_output_dense_fused": "MBERT attn. output",
    "matmul_6_fused": "MBERT attn. context",
    "matmul_2_fused": "MBERT attn. scores",
    "layer1_0_conv1_fused": "ResNet-18 L1 conv1",
    "layer2_0_conv1_fused": "ResNet-18 L2 conv1",
    "layer4_1_conv2_fused": "ResNet-18 L4 conv2",
}

LAYER_STEMS = {
    "mobilebert_encoder_layer_0_ffn_0_output_dense_fused": "mobilebert_ffn_output",
    "mobilebert_encoder_layer_0_output_bottleneck_dense_fused": "mobilebert_bottleneck",
    "mobilebert_encoder_layer_0_attention_output_dense_fused": "mobilebert_attention_output",
    "matmul_6_fused": "mobilebert_attention_context",
    "matmul_2_fused": "mobilebert_attention_scores",
    "layer1_0_conv1_fused": "resnet18_layer1_conv1",
    "layer2_0_conv1_fused": "resnet18_layer2_conv1",
    "layer4_1_conv2_fused": "resnet18_layer4_conv2",
}

# Describe one logical matrix operation independently of compiler padding
@dataclass(frozen=True)
class OperationShape:
    operation: str
    input_shape: str
    weight_shape: str
    output_shape: str
    m: int
    k: int
    n: int

    @property
    def macs(self) -> int:
        return self.m * self.k * self.n

    @property
    def gemm(self) -> str:
        return f"{self.m}×{self.k} × {self.k}×{self.n}"


OPERATION_SHAPES = {
    "mobilebert_encoder_layer_0_ffn_0_output_dense_fused": OperationShape(
        "Linear", "1×128×512", "512×128", "1×128×128", 128, 512, 128
    ),
    "mobilebert_encoder_layer_0_output_bottleneck_dense_fused": OperationShape(
        "Linear", "1×128×128", "128×512", "1×128×512", 128, 128, 512
    ),
    "mobilebert_encoder_layer_0_attention_output_dense_fused": OperationShape(
        "Linear", "1×128×128", "128×128", "1×128×128", 128, 128, 128
    ),
    "matmul_6_fused": OperationShape(
        "Matmul", "128×128", "128×64", "128×64", 128, 128, 64
    ),
    "matmul_2_fused": OperationShape(
        "Matmul", "128×64", "64×128", "128×128", 128, 64, 128
    ),
    "layer1_0_conv1_fused": OperationShape(
        "Conv 3×3, s1", "1×56×56×64", "3×3×64×64", "1×56×56×64", 3136, 576, 64
    ),
    "layer2_0_conv1_fused": OperationShape(
        "Conv 3×3, s2", "1×56×56×64", "3×3×64×128", "1×28×28×128", 784, 576, 128
    ),
    "layer4_1_conv2_fused": OperationShape(
        "Conv 3×3, s1", "1×7×7×512", "3×3×512×512", "1×7×7×512", 49, 4608, 512
    ),
}

PointKey = tuple[str, int, int]


# Store the measured counters for one hardware point and workload
@dataclass(frozen=True)
class LayerResult:
    network: str
    layer: str
    runtime_cycles: int
    matrix_unit_cycles: int
    ideal_cycles: int
    matrix_utilization: float
    processor_active_cycles: int
    result_slot_stall_pct: float
    completion_storage_stall_pct: float
    descriptor_stall_pct: float
    set_wait_pct: float


# Store one width-expanded hardware point and all eight RTL results
@dataclass(frozen=True)
class DesignPoint:
    key: PointKey
    config: str
    backend: str
    geometry: str
    input_port_bits: int
    output_port_bits: int
    native_width: int | None
    mode: str | None
    sets: int | None
    ch_in: int | None
    ch_out: int | None
    mac_latency: int | None
    slots: int | None
    results: dict[str, LayerResult]


# Store the validated study and the exact SA reference for every point
@dataclass(frozen=True)
class Study:
    summary_path: Path
    layers: tuple[str, ...]
    points: tuple[DesignPoint, ...]
    references: dict[PointKey, PointKey]
    pd_key: PointKey
    clock_period_ns: float


# Parse an integer-like CSV cell while preserving blanks
def _integer(value: str | None) -> int | None:
    return int(float(value)) if value not in (None, "") else None


# Parse a required floating-point CSV cell
def _number(row: dict[str, str], field: str) -> float:
    value = row.get(field, "")
    if value == "":
        raise ValueError(f"missing {field} for {row.get('config')} {row.get('layer')}")
    return float(value)


# Sort hardware points in the same broad order as the workbook
def _point_order(point: DesignPoint) -> tuple:
    geometry = int(point.geometry.split("x", 1)[0])
    return (
        point.backend != "systolic",
        geometry,
        point.input_port_bits,
        point.native_width or 0,
        point.mode or "",
        point.sets or 0,
        point.ch_in or 0,
        point.ch_out or 0,
        point.slots or 0,
        point.config,
    )


# Load and fully validate reportable RTL rows from one parsed sweep summary
def load(
    summary_path: Path = DEFAULT_SUMMARY,
    *,
    pd_config: str = DEFAULT_PD_CONFIG,
    pd_ports: tuple[int, int] = DEFAULT_PD_PORTS,
) -> Study:
    summary_path = summary_path.resolve()
    workloads_path = summary_path.with_name("workloads.csv")
    if not summary_path.exists() or not workloads_path.exists():
        raise FileNotFoundError("summary.csv and its sibling workloads.csv are required")

    with workloads_path.open(newline="", encoding="utf-8-sig") as handle:
        workload_rows = list(csv.DictReader(handle))
    layers = tuple(row["layer"] for row in workload_rows)
    if len(layers) != 8 or len(set(layers)) != 8 or "fc_1" in layers:
        raise ValueError(f"expected eight unique matrix workloads, found {layers}")
    if any(row.get("requires_matrix_performance") != "yes" for row in workload_rows):
        raise ValueError("every declared workload must require matrix performance evidence")
    if set(layers) != set(OPERATION_SHAPES):
        raise ValueError("operation-shape metadata must exactly cover the declared workloads")

    with summary_path.open(newline="", encoding="utf-8-sig") as handle:
        all_rows = list(csv.DictReader(handle))
    reportable_rows = [row for row in all_rows if row.get("layer") in layers]
    if len(reportable_rows) != 568:
        raise ValueError(f"expected 568 reportable RTL rows, found {len(reportable_rows)}")

    grouped: dict[PointKey, list[dict[str, str]]] = defaultdict(list)
    for row in reportable_rows:
        if row.get("passed") != "True" or row.get("sim") != "rtl":
            raise ValueError(f"non-passing reportable row: {row.get('config')} {row.get('layer')}")
        technology = row.get("technology") or ""
        if Path(technology).name != "tsmc7" or float(row.get("clock_period_ns", 0)) != 2.0:
            raise ValueError(f"unexpected technology or clock: {row.get('config')}")
        for field in (
            "runtime_cycles",
            "matrix_unit_cycles",
            "ideal_cycles",
            "matrix_utilization",
            "processor_active_cycles",
        ):
            _number(row, field)
        key = (
            row["config"],
            int(row["ic_port_width_bits"]),
            int(row["oc_port_width_bits"]),
        )
        grouped[key].append(row)

    points: list[DesignPoint] = []
    for key, rows in grouped.items():
        if len(rows) != len(layers) or {row["layer"] for row in rows} != set(layers):
            raise ValueError(f"incomplete workload coverage for {key}")
        representative = rows[0]
        results = {
            row["layer"]: LayerResult(
                network=row["network"],
                layer=row["layer"],
                runtime_cycles=int(row["runtime_cycles"]),
                matrix_unit_cycles=int(row["matrix_unit_cycles"]),
                ideal_cycles=int(row["ideal_cycles"]),
                matrix_utilization=float(row["matrix_utilization"]),
                processor_active_cycles=int(row["processor_active_cycles"]),
                result_slot_stall_pct=float(row.get("cim_result_slot_stall_pct_of_processor") or 0),
                completion_storage_stall_pct=float(
                    row.get("cim_completion_storage_stall_pct_of_processor") or 0
                ),
                descriptor_stall_pct=float(
                    row.get("cim_completion_descriptor_stall_pct_of_processor") or 0
                ),
                set_wait_pct=float(row.get("cim_set_wait_pct_of_processor") or 0),
            )
            for row in rows
        }
        backend = representative["backend"]
        points.append(
            DesignPoint(
                key=key,
                config=representative["config"],
                backend=backend,
                geometry=representative["geometry"],
                input_port_bits=int(representative["ic_port_width_bits"]),
                output_port_bits=int(representative["oc_port_width_bits"]),
                native_width=(
                    int(representative["cim_macro_native_width"].rstrip("b"))
                    if backend == "cim"
                    else None
                ),
                mode=representative.get("cim_mode") or None,
                sets=_integer(representative.get("cim_b_sets")),
                ch_in=_integer(representative.get("cim_ch_in")),
                ch_out=_integer(representative.get("cim_ch_out")),
                mac_latency=_integer(representative.get("cim_mac_latency")),
                slots=_integer(representative.get("cim_result_slots_per_output_lane")),
                results=results,
            )
        )

    points.sort(key=_point_order)
    if len(points) != 71 or sum(point.backend == "systolic" for point in points) != 2:
        raise ValueError(f"expected 71 points including two SA baselines, found {len(points)}")

    for rows in grouped.values():
        if rows[0]["backend"] != "systolic" or rows[0]["geometry"] != "64x64":
            continue
        for row in rows:
            shape = OPERATION_SHAPES[row["layer"]]
            array_macs = int(row["macs"])
            if shape.macs % array_macs or int(row["raw_ideal_cycles"]) != shape.macs // array_macs:
                raise ValueError(f"operation shape disagrees with RTL ideal cycles: {row['layer']}")
    by_key = {point.key: point for point in points}
    sa_points = [point for point in points if point.backend == "systolic"]
    baseline = min(sa_points, key=lambda point: int(point.geometry.split("x", 1)[0]))
    references: dict[PointKey, PointKey] = {}
    for point in points:
        if point.backend == "systolic":
            references[point.key] = point.key
            continue
        exact = next(
            (
                candidate
                for candidate in sa_points
                if candidate.geometry == point.geometry
                and candidate.input_port_bits == point.input_port_bits
                and candidate.output_port_bits == point.output_port_bits
            ),
            baseline,
        )
        references[point.key] = exact.key

    pd_key = (pd_config, pd_ports[0], pd_ports[1])
    if pd_key not in by_key:
        raise ValueError(f"PD point not found: {pd_key}")
    pd = by_key[pd_key]
    if pd.backend != "cim" or pd.slots != 8:
        raise ValueError(f"PD point must be the native rslots=8 CIM row, found {pd}")
    return Study(summary_path, layers, tuple(points), references, pd_key, 2.0)


# Return one hardware point by its exact width-expanded key
def point(study: Study, key: PointKey) -> DesignPoint:
    return next(candidate for candidate in study.points if candidate.key == key)


# Return the exact SA reference used by the comparison workbook
def reference(study: Study, design: DesignPoint) -> DesignPoint:
    return point(study, study.references[design.key])


# Return end-to-end RTL speedup for every declared workload
def speedups(study: Study, design: DesignPoint) -> dict[str, float]:
    baseline = reference(study, design)
    return {
        layer: baseline.results[layer].runtime_cycles / design.results[layer].runtime_cycles
        for layer in study.layers
    }


# Return the geometric-mean end-to-end speedup across workloads
def geomean_speedup(study: Study, design: DesignPoint) -> float:
    values = speedups(study, design).values()
    return math.exp(statistics.fmean(math.log(value) for value in values))


# Return the arithmetic mean MatrixUnit utilization across workloads
def mean_utilization(design: DesignPoint) -> float:
    return statistics.fmean(result.matrix_utilization for result in design.results.values())


# Return the arithmetic mean measured result-slot stall percentage
def mean_result_slot_stall(design: DesignPoint) -> float:
    return statistics.fmean(result.result_slot_stall_pct for result in design.results.values())


# Return CIM points grouped by the SA reference used for speedup
def reference_groups(study: Study) -> list[tuple[DesignPoint, list[DesignPoint]]]:
    groups = []
    for baseline in (point for point in study.points if point.backend == "systolic"):
        designs = [
            candidate
            for candidate in study.points
            if candidate.backend == "cim" and study.references[candidate.key] == baseline.key
        ]
        groups.append((baseline, designs))
    return sorted(groups, key=lambda group: group[0].input_port_bits)


# Return a concise plot label for one width-expanded design point
def concise_label(design: DesignPoint) -> str:
    if design.backend == "systolic":
        return f"SA {design.geometry.replace('x', '×')}, {design.input_port_bits}b"
    mode = "serial" if design.mode == "bit-serial" else f"native {design.native_width}b"
    return (
        f"{design.geometry.replace('x', '×')}, {mode}, ci{design.ch_in}, "
        f"{design.sets} sets, r{design.slots}, {design.input_port_bits}b"
    )


# Return a slot-sweep family key that excludes only the slot count
def slot_family_key(design: DesignPoint) -> tuple:
    return (
        design.geometry,
        design.input_port_bits,
        design.output_port_bits,
        design.native_width,
        design.mode,
        design.sets,
        design.ch_in,
        design.ch_out,
        design.mac_latency,
    )


# Return every measured slot variant in the PD design family
def pd_family(study: Study) -> list[DesignPoint]:
    pd = point(study, study.pd_key)
    family = [
        design
        for design in study.points
        if design.backend == "cim" and slot_family_key(design) == slot_family_key(pd)
    ]
    family.sort(key=lambda design: design.slots or 0)
    if not family or family[-1].key != pd.key:
        raise ValueError("PD family does not terminate at the rslots=8 PD point")
    return family


# Return a set-sweep family key that excludes only the B-set count
def set_family_key(design: DesignPoint) -> tuple:
    return (
        design.geometry,
        design.input_port_bits,
        design.output_port_bits,
        design.native_width,
        design.mode,
        design.ch_in,
        design.ch_out,
        design.mac_latency,
        design.slots,
    )


# Return every measured B-set variant matching the rslots=8 PD point
def pd_set_family(study: Study) -> list[DesignPoint]:
    pd = point(study, study.pd_key)
    family = [
        design
        for design in study.points
        if design.backend == "cim" and set_family_key(design) == set_family_key(pd)
    ]
    family.sort(key=lambda design: design.sets or 0)
    if not family or pd not in family:
        raise ValueError("PD set family does not contain the rslots=8 PD point")
    return family
