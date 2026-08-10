"""Machine-readable and paper-ready tables for performance comparisons."""

from __future__ import annotations

import csv
import io
from dataclasses import dataclass

from .data import (
    LAYER_LABELS,
    OPERATION_SHAPES,
    DesignPoint,
    Study,
    concise_label,
    geomean_speedup,
    mean_result_slot_stall,
    mean_utilization,
    point,
    reference,
    reference_groups,
    speedups,
)

# Store one ranked CIM design row
@dataclass(frozen=True)
class RankingRow:
    reference: str
    rank: int
    design: DesignPoint
    geomean: float
    minimum: float
    maximum: float
    utilization: float
    result_slot_stall: float


# Return every CIM point ranked within its matched SA reference group
def ranking_rows(study: Study) -> list[RankingRow]:
    rows = []
    for baseline, designs in reference_groups(study):
        ordered = sorted(
            designs,
            key=lambda design: (
                -geomean_speedup(study, design),
                -(design.ch_in or 0),
                -(design.sets or 0),
                design.config,
            ),
        )
        for rank, design in enumerate(ordered, 1):
            values = list(speedups(study, design).values())
            rows.append(
                RankingRow(
                    reference=concise_label(baseline),
                    rank=rank,
                    design=design,
                    geomean=geomean_speedup(study, design),
                    minimum=min(values),
                    maximum=max(values),
                    utilization=mean_utilization(design),
                    result_slot_stall=mean_result_slot_stall(design),
                )
            )
    return rows


# Render the full ranking as a CSV data artifact
def rankings_csv(study: Study) -> str:
    output = io.StringIO()
    writer = csv.writer(output, lineterminator="\n")
    writer.writerow(
        [
            "sa_reference",
            "rank_within_reference",
            "config",
            "geometry",
            "input_port_bits",
            "output_port_bits",
            "native_width_bits",
            "mode",
            "sets",
            "ch_in",
            "ch_out",
            "result_slots_per_lane",
            "geomean_speedup",
            "minimum_speedup",
            "maximum_speedup",
            "mean_matrix_utilization",
            "mean_result_slot_stall_pct",
            "is_pd_point",
        ]
    )
    for row in ranking_rows(study):
        design = row.design
        writer.writerow(
            [
                row.reference,
                row.rank,
                design.config,
                design.geometry,
                design.input_port_bits,
                design.output_port_bits,
                design.native_width,
                design.mode,
                design.sets,
                design.ch_in,
                design.ch_out,
                design.slots,
                f"{row.geomean:.6f}",
                f"{row.minimum:.6f}",
                f"{row.maximum:.6f}",
                f"{row.utilization:.6f}",
                f"{row.result_slot_stall:.3f}",
                "yes" if design.key == study.pd_key else "no",
            ]
        )
    return output.getvalue()


# Return top-ranked rows from each independent SA reference group
def _top_rows(study: Study, limit: int) -> list[RankingRow]:
    selected = []
    for baseline, _designs in reference_groups(study):
        reference_name = concise_label(baseline)
        selected.extend(
            row
            for row in ranking_rows(study)
            if row.reference == reference_name and row.rank <= limit
        )
    return selected


# Render top-ranked designs as a compact Markdown table
def rankings_markdown(study: Study, limit: int = 6) -> str:
    lines = [
        "| SA reference | Rank | CIM design | GM speedup | Min | Max | Mean util. | Mean slot stall |",
        "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in _top_rows(study, limit):
        lines.append(
            f"| {row.reference} | {row.rank} | {concise_label(row.design)} | "
            f"{row.geomean:.3f}x | {row.minimum:.3f}x | {row.maximum:.3f}x | "
            f"{row.utilization:.1%} | {row.result_slot_stall:.1f}% |"
        )
    return "\n".join(lines) + "\n"


# Escape the small subset of LaTeX syntax used in generated labels
def _latex(text: str) -> str:
    return (
        text.replace("\\", r"\textbackslash{}")
        .replace("&", r"\&")
        .replace("%", r"\%")
        .replace("_", r"\_")
        .replace("×", r"$\times$")
    )


# Render top-ranked designs as a booktabs LaTeX table
def rankings_latex(study: Study, limit: int = 6) -> str:
    lines = [
        r"\begin{tabular}{rllrrrr}",
        r"\toprule",
        r"Rank & SA reference & CIM design & GM & Min. & Max. & Util. \\",
        r"\midrule",
    ]
    prior_reference = None
    for row in _top_rows(study, limit):
        if prior_reference is not None and prior_reference != row.reference:
            lines.append(r"\midrule")
        lines.append(
            f"{row.rank} & {_latex(row.reference)} & {_latex(concise_label(row.design))} & "
            f"{row.geomean:.3f}$\\times$ & {row.minimum:.3f}$\\times$ & "
            f"{row.maximum:.3f}$\\times$ & {row.utilization * 100:.1f}\\% \\\\"
        )
        prior_reference = row.reference
    lines.extend([r"\bottomrule", r"\end{tabular}", ""])
    return "\n".join(lines)


# Return one row per workload for the exact rslots=8 PD point
def pd_workload_rows(study: Study) -> list[dict[str, str | int | float]]:
    pd = point(study, study.pd_key)
    baseline = reference(study, pd)
    pd_speedups = speedups(study, pd)
    return [
        {
            "workload": LAYER_LABELS[layer],
            "layer": layer,
            "sa_runtime_cycles": baseline.results[layer].runtime_cycles,
            "pd_runtime_cycles": pd.results[layer].runtime_cycles,
            "speedup": pd_speedups[layer],
            "matrix_utilization": pd.results[layer].matrix_utilization,
            "result_slot_stall_pct": pd.results[layer].result_slot_stall_pct,
            "completion_storage_stall_pct": pd.results[layer].completion_storage_stall_pct,
            "mac_wait_weight_set_load_pct": pd.results[
                layer
            ].mac_wait_weight_set_load_pct,
        }
        for layer in study.layers
    ]


# Render the exact PD workload comparison as CSV
def pd_workloads_csv(study: Study) -> str:
    output = io.StringIO()
    fields = list(pd_workload_rows(study)[0])
    writer = csv.DictWriter(output, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    writer.writerows(pd_workload_rows(study))
    return output.getvalue()


# Render the exact PD workload comparison as Markdown
def pd_workloads_markdown(study: Study) -> str:
    lines = [
        "| Workload | SA cycles | PD r8 cycles | Speedup | Utilization | Slot stall | Weight-load wait |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in pd_workload_rows(study):
        lines.append(
            f"| {row['workload']} | {row['sa_runtime_cycles']:,} | {row['pd_runtime_cycles']:,} | "
            f"{row['speedup']:.3f}x | {row['matrix_utilization']:.1%} | "
            f"{row['result_slot_stall_pct']:.1f}% | "
            f"{row['mac_wait_weight_set_load_pct']:.1f}% |"
        )
    return "\n".join(lines) + "\n"


# Render the exact PD workload comparison as a booktabs LaTeX table
def pd_workloads_latex(study: Study) -> str:
    lines = [
        r"\begin{tabular}{lrrrrrr}",
        r"\toprule",
        r"Workload & SA cycles & PD cycles & Speedup & Util. & Slot stall & Weight-load wait \\",
        r"\midrule",
    ]
    for row in pd_workload_rows(study):
        lines.append(
            f"{_latex(str(row['workload']))} & {row['sa_runtime_cycles']:,} & "
            f"{row['pd_runtime_cycles']:,} & {row['speedup']:.3f}$\\times$ & "
            f"{row['matrix_utilization'] * 100:.1f}\\% & "
            f"{row['result_slot_stall_pct']:.1f}\\% & "
            f"{row['mac_wait_weight_set_load_pct']:.1f}\\% \\\\"
        )
    lines.extend([r"\bottomrule", r"\end{tabular}", ""])
    return "\n".join(lines)


# Return machine-readable operation-shape rows in declared workload order
def operation_shape_rows(study: Study) -> list[dict[str, str | int]]:
    return [
        {
            "workload": LAYER_LABELS[layer],
            "layer": layer,
            "operation": OPERATION_SHAPES[layer].operation,
            "input_shape": OPERATION_SHAPES[layer].input_shape,
            "weight_or_rhs_shape": OPERATION_SHAPES[layer].weight_shape,
            "output_shape": OPERATION_SHAPES[layer].output_shape,
            "M": OPERATION_SHAPES[layer].m,
            "K": OPERATION_SHAPES[layer].k,
            "N": OPERATION_SHAPES[layer].n,
            "equivalent_gemm": OPERATION_SHAPES[layer].gemm,
            "logical_macs": OPERATION_SHAPES[layer].macs,
        }
        for layer in study.layers
    ]


# Render the operation-shape table as CSV
def operation_shapes_csv(study: Study) -> str:
    output = io.StringIO()
    fields = list(operation_shape_rows(study)[0])
    writer = csv.DictWriter(output, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    writer.writerows(operation_shape_rows(study))
    return output.getvalue()


# Render the operation-shape table as Markdown
def operation_shapes_markdown(study: Study) -> str:
    lines = [
        "| Workload | Operation | Input | Weight / RHS | Output | Equivalent GEMM | Logical MACs |",
        "| --- | --- | --- | --- | --- | --- | ---: |",
    ]
    for row in operation_shape_rows(study):
        lines.append(
            f"| {row['workload']} | {row['operation']} | {row['input_shape']} | "
            f"{row['weight_or_rhs_shape']} | {row['output_shape']} | {row['equivalent_gemm']} | "
            f"{row['logical_macs']:,} |"
        )
    return "\n".join(lines) + "\n"


# Render the operation-shape table as booktabs LaTeX
def operation_shapes_latex(study: Study) -> str:
    lines = [
        r"\begin{tabular}{lllllrrr}",
        r"\toprule",
        r"Workload & Operation & Input & Weight/RHS & Output & $M$ & $K$ & $N$ \\",
        r"\midrule",
    ]
    for row in operation_shape_rows(study):
        lines.append(
            f"{_latex(str(row['workload']))} & {_latex(str(row['operation']))} & "
            f"{_latex(str(row['input_shape']))} & {_latex(str(row['weight_or_rhs_shape']))} & "
            f"{_latex(str(row['output_shape']))} & {row['M']} & {row['K']} & {row['N']} \\\\"
        )
    lines.extend([r"\bottomrule", r"\end{tabular}", ""])
    return "\n".join(lines)
