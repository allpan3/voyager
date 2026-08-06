"""Area-normalized RTL performance analysis for the physical-design point."""

from __future__ import annotations

import csv
import io
from dataclasses import dataclass
from pathlib import Path

import matplotlib.ticker as mticker
from matplotlib.figure import Figure

from area_figs.data import load as load_area

from . import style as S
from .data import LAYER_LABELS, OPERATION_SHAPES, Study, point

DEFAULT_AREA_WORKBOOK = (
    Path(__file__).resolve().parents[2] / "matrixunit_sa_vs_cim_64x64_area_comparison_1ns.xlsx"
)
AREA_DESIGNS = (
    ("SA 64×64", ("sa_64x64", 512, 512), None),
    ("CIM rslots=8 PD", ("cim8b8b_64x64_co8_ci64_sets18_rslots8", 512, 512), 8),
)


# Store one workload result normalized by hierarchy-corrected MatrixUnit area
@dataclass(frozen=True)
class AreaNormalizedPerformanceRow:
    workload: str
    layer: str
    design: str
    config: str
    result_slots: int | None
    logical_macs: int
    runtime_cycles: int
    runtime_ns: float
    area_um2: float
    throughput_gmac_s: float
    throughput_density_gmac_s_mm2: float
    speedup_vs_sa: float
    throughput_density_vs_sa: float


# Escape the small subset of LaTeX syntax used in generated labels
def _latex(text: str) -> str:
    return (
        text.replace("\\", r"\textbackslash{}")
        .replace("&", r"\&")
        .replace("%", r"\%")
        .replace("_", r"\_")
        .replace("×", r"$\times$")
    )


# Return SA and rslots=8 PD throughput-density results for every workload
def area_normalized_performance_rows(
    study: Study,
    area_workbook: Path = DEFAULT_AREA_WORKBOOK,
) -> list[AreaNormalizedPerformanceRow]:
    area_model = load_area(area_workbook, rslots=8)
    areas = {None: area_model.sa_total, 8: area_model.cim_total}
    baseline = point(study, AREA_DESIGNS[0][1])
    rows = []
    for layer in study.layers:
        shape = OPERATION_SHAPES[layer]
        baseline_cycles = baseline.results[layer].runtime_cycles
        baseline_density = None
        for design_name, key, slots in AREA_DESIGNS:
            design = point(study, key)
            cycles = design.results[layer].runtime_cycles
            runtime_ns = cycles * study.clock_period_ns
            throughput = shape.macs / runtime_ns
            density = throughput / (areas[slots] / 1_000_000)
            if baseline_density is None:
                baseline_density = density
            rows.append(
                AreaNormalizedPerformanceRow(
                    workload=LAYER_LABELS[layer],
                    layer=layer,
                    design=design_name,
                    config=design.config,
                    result_slots=slots,
                    logical_macs=shape.macs,
                    runtime_cycles=cycles,
                    runtime_ns=runtime_ns,
                    area_um2=areas[slots],
                    throughput_gmac_s=throughput,
                    throughput_density_gmac_s_mm2=density,
                    speedup_vs_sa=baseline_cycles / cycles,
                    throughput_density_vs_sa=density / baseline_density,
                )
            )
    return rows


# Render long-form area-normalized performance as CSV
def area_normalized_performance_csv(study: Study, area_workbook: Path = DEFAULT_AREA_WORKBOOK) -> str:
    rows = area_normalized_performance_rows(study, area_workbook)
    output = io.StringIO()
    fields = list(rows[0].__dataclass_fields__)
    writer = csv.DictWriter(output, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    writer.writerows({field: getattr(row, field) for field in fields} for row in rows)
    return output.getvalue()


# Render area-normalized performance as a compact Markdown table
def area_normalized_performance_markdown(
    study: Study,
    area_workbook: Path = DEFAULT_AREA_WORKBOOK,
) -> str:
    rows = area_normalized_performance_rows(study, area_workbook)
    by_layer = {layer: [row for row in rows if row.layer == layer] for layer in study.layers}
    lines = [
        "| Workload | SA GMAC/s/mm² | CIM rslots=8 PD GMAC/s/mm² (×SA) |",
        "| --- | ---: | ---: |",
    ]
    for layer in study.layers:
        values = by_layer[layer]
        lines.append(
            f"| {LAYER_LABELS[layer]} | {values[0].throughput_density_gmac_s_mm2:,.1f} | "
            f"{values[1].throughput_density_gmac_s_mm2:,.1f} "
            f"({values[1].throughput_density_vs_sa:.3f}x) |"
        )
    return "\n".join(lines) + "\n"


# Render area-normalized performance as booktabs LaTeX
def area_normalized_performance_latex(
    study: Study,
    area_workbook: Path = DEFAULT_AREA_WORKBOOK,
) -> str:
    rows = area_normalized_performance_rows(study, area_workbook)
    by_layer = {layer: [row for row in rows if row.layer == layer] for layer in study.layers}
    lines = [
        r"\begin{tabular}{lrr}",
        r"\toprule",
        r"Workload & SA & CIM rslots=8 PD \\",
        r"\midrule",
    ]
    for layer in study.layers:
        values = by_layer[layer]
        lines.append(
            f"{_latex(LAYER_LABELS[layer])} & {values[0].throughput_density_gmac_s_mm2:,.1f} & "
            f"{values[1].throughput_density_gmac_s_mm2:,.1f} "
            f"({values[1].throughput_density_vs_sa:.3f}$\\times$) \\\\"
        )
    lines.extend(
        [
            r"\addlinespace",
            r"\multicolumn{3}{l}{\footnotesize GMAC/s/mm$^2$; parentheses are relative to SA.} \\",
            r"\bottomrule",
            r"\end{tabular}",
            "",
        ]
    )
    return "\n".join(lines)


# Draw throughput density for the rslots=8 physical-design point relative to SA
def fig_area_normalized_performance(
    study: Study,
    style: S.Style,
    area_workbook: Path = DEFAULT_AREA_WORKBOOK,
) -> Figure:
    rows = area_normalized_performance_rows(study, area_workbook)
    by_layer = {layer: [row for row in rows if row.layer == layer] for layer in study.layers}
    size = (3.45, 4.45) if style.name == "paper" else (8.0, 7.0)
    figure = Figure(figsize=size, facecolor=S.SURFACE)
    axis = figure.subplots()
    figure.subplots_adjust(left=0.36, right=0.97, top=0.73, bottom=0.16)
    figure.suptitle(
        "Area-normalized RTL performance",
        x=0.5,
        y=0.985,
        fontsize=style.title_pt + 0.8,
        fontweight="bold",
    )
    area_by_design = {row.design: row.area_um2 / 1_000_000 for row in rows[:2]}
    figure.text(
        0.5,
        0.915,
        "64×64 INT8, 2 ns; hierarchy-corrected MatrixUnit area",
        ha="center",
        va="top",
        fontsize=style.small_pt,
        color=S.INK_SECONDARY,
    )
    figure.text(
        0.5,
        0.865,
        "Area (mm²): SA "
        f"{area_by_design['SA 64×64']:.3f}; "
        f"CIM rslots=8 PD {area_by_design['CIM rslots=8 PD']:.3f}",
        ha="center",
        va="top",
        fontsize=style.small_pt,
        color=S.INK_SECONDARY,
    )

    labels = [LAYER_LABELS[layer] for layer in study.layers]
    positions = list(reversed(range(len(labels))))
    values = [by_layer[layer][1].throughput_density_vs_sa for layer in study.layers]
    axis.barh(
        positions,
        values,
        height=0.20,
        color=S.PD,
        edgecolor="none",
        label="CIM rslots=8 PD",
        zorder=3,
    )
    for y, value in zip(positions, values):
        inside = value < 1
        axis.text(
            value - 0.035 if inside else value + 0.035,
            y,
            f"{value:.2f}",
            ha="right" if inside else "left",
            va="center",
            fontsize=style.small_pt,
            color=S.INK,
        )

    axis.axvline(1, color=S.SA, linewidth=0.8, linestyle=(0, (2, 2)), zorder=2)
    axis.text(1, len(labels) - 0.15, "SA = 1", ha="center", va="bottom", fontsize=style.small_pt)
    axis.set_yticks(positions)
    axis.set_yticklabels(labels)
    axis.set_ylim(-0.65, len(labels) - 0.35)
    axis.set_xlim(0, max(values) * 1.18)
    axis.set_xlabel("Throughput density relative to SA\n(×; higher is better)")
    axis.xaxis.set_major_locator(mticker.MultipleLocator(0.5))
    axis.xaxis.set_major_formatter(mticker.FormatStrFormatter("%.1f"))
    axis.grid(axis="x", color=S.GRID, linewidth=0.55, zorder=0)
    axis.spines["bottom"].set_visible(False)
    axis.tick_params(axis="both", length=0)
    axis.legend(
        loc="upper center",
        bbox_to_anchor=(0.5, 0.815),
        bbox_transform=figure.transFigure,
        ncol=1,
        fontsize=style.small_pt,
    )
    return figure
