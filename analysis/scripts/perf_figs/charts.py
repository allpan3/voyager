"""Academic bar charts for an exact CIM family against its SA baseline."""

from __future__ import annotations

import matplotlib.ticker as mticker
from matplotlib.figure import Figure
from matplotlib.patches import Rectangle

from . import style as S
from .data import (
    LAYER_LABELS,
    DesignPoint,
    Study,
    pd_family,
    pd_set_family,
    point,
    reference,
    speedups,
)
from .tables import operation_shape_rows


# Return a paper or slide figure size while preserving the single-panel layout
def _size(style: S.Style) -> tuple[float, float]:
    return (3.45, 3.15) if style.name == "paper" else (7.4, 5.3)


# Validate that the slot comparison varies only result-slot count
def result_slot_comparison_family(study: Study) -> tuple[DesignPoint, list[DesignPoint]]:
    pd = point(study, study.pd_key)
    family = pd_family(study)
    baseline = reference(study, pd)
    common = (
        pd.geometry,
        pd.input_port_bits,
        pd.output_port_bits,
        pd.native_width,
        pd.mode,
        pd.sets,
        pd.ch_in,
        pd.ch_out,
        pd.mac_latency,
    )
    for design in family:
        candidate = (
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
        if candidate != common or reference(study, design).key != baseline.key:
            raise ValueError(f"comparison family differs beyond result slots: {design.key}")
    if [design.slots for design in family] != [1, 2, 6, 8]:
        raise ValueError(f"expected r1/r2/r6/r8 comparison family, found {[d.slots for d in family]}")
    return baseline, family


# Validate that the set comparison varies only B-set count at rslots=8
def b_set_comparison_family(study: Study) -> tuple[DesignPoint, list[DesignPoint]]:
    pd = point(study, study.pd_key)
    family = pd_set_family(study)
    baseline = reference(study, pd)
    common = (
        pd.geometry,
        pd.input_port_bits,
        pd.output_port_bits,
        pd.native_width,
        pd.mode,
        pd.ch_in,
        pd.ch_out,
        pd.mac_latency,
        pd.slots,
    )
    for design in family:
        candidate = (
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
        if candidate != common or reference(study, design).key != baseline.key:
            raise ValueError(f"set comparison differs beyond B-set count: {design.key}")
    if [design.sets for design in family] != [2, 8, 18]:
        raise ValueError(f"expected 2/8/18 B-set comparison family, found {[d.sets for d in family]}")
    if pd.slots != 8 or any(design.slots != 8 for design in family):
        raise ValueError("every B-set comparison point must use rslots=8")
    if family[-1].key != pd.key:
        raise ValueError("18-set rslots=8 point must be the PD point")
    return baseline, family


# Return fixed parameters for the result-slot comparison charts
def result_slot_parameter_lines(study: Study) -> tuple[str, str, str]:
    _baseline, family = result_slot_comparison_family(study)
    design = family[-1]
    geometry = design.geometry.replace("x", "$\\times$")
    mode = str(design.mode)
    return (
        f"CIM family: {geometry} array, INT8, native {design.native_width}b {mode}",
        f"{design.input_port_bits}b input / {design.output_port_bits}b output, "
        f"CH_IN/CH_OUT={design.ch_in}/{design.ch_out}, {design.sets} B sets",
        f"MAC latency={design.mac_latency}, tsmc7, 2 ns; only rslots varies",
    )


# Return fixed parameters for the B-set comparison charts
def b_set_parameter_lines(study: Study) -> tuple[str, str, str]:
    _baseline, family = b_set_comparison_family(study)
    design = family[-1]
    geometry = design.geometry.replace("x", "$\\times$")
    mode = str(design.mode)
    return (
        f"CIM family: {geometry} array, INT8, native {design.native_width}b {mode}",
        f"{design.input_port_bits}b input / {design.output_port_bits}b output, "
        f"CH_IN/CH_OUT={design.ch_in}/{design.ch_out}, rslots={design.slots}",
        f"MAC latency={design.mac_latency}, tsmc7, 2 ns; only B-set count varies",
    )


# Draw one independently readable workload comparison in raw RTL cycles
def _comparison_bars(
    study: Study,
    style: S.Style,
    layer: str,
    baseline: DesignPoint,
    family: list[DesignPoint],
    parameter_lines: tuple[str, str, str],
    labels: list[str],
    colors: tuple[str, ...],
) -> Figure:
    if layer not in study.layers:
        raise ValueError(f"unknown workload: {layer}")
    designs = [baseline, *family]
    if len(labels) != len(designs) or len(colors) != len(designs):
        raise ValueError("bar labels and colors must match the comparison points")
    cycles = [design.results[layer].runtime_cycles for design in designs]
    family_speedups = {design.key: speedups(study, design)[layer] for design in family}
    pd = point(study, study.pd_key)

    fig = Figure(figsize=_size(style), facecolor=S.SURFACE)
    ax = fig.subplots()
    fig.subplots_adjust(left=0.18, right=0.98, top=0.70, bottom=0.20)
    title_size = style.title_pt + (0.5 if style.name == "paper" else 1.0)
    fig.text(
        0.5,
        0.965,
        LAYER_LABELS[layer],
        ha="center",
        va="top",
        fontsize=title_size,
        fontweight="bold",
        color=S.INK,
    )
    for y, line in zip((0.895, 0.845, 0.795), parameter_lines):
        fig.text(0.5, y, line, ha="center", va="top", fontsize=style.small_pt, color=S.INK_SECONDARY)

    edge_colors = (S.SA, *([S.SURFACE] * (len(family) - 1)), S.INK)
    line_widths = (0.5, *([0.4] * (len(family) - 1)), 0.8)
    bars = ax.bar(
        range(len(designs)),
        cycles,
        width=0.68,
        color=colors,
        edgecolor=edge_colors,
        linewidth=line_widths,
        zorder=3,
    )
    limit = max(cycles) * 1.20
    padding = max(cycles) * 0.022
    for bar, design, value in zip(bars, designs, cycles):
        if design.backend == "systolic":
            annotation = f"{value:,}"
        else:
            annotation = f"{value:,}\n{family_speedups[design.key]:.2f}$\\times$ SA"
        ax.text(
            bar.get_x() + bar.get_width() / 2,
            value + padding,
            annotation,
            ha="center",
            va="bottom",
            fontsize=style.small_pt,
            color=S.INK,
            linespacing=1.15,
        )
        if design.key == pd.key:
            ax.plot(
                bar.get_x() + bar.get_width() / 2,
                value,
                marker="*",
                markersize=5.5 if style.name == "paper" else 9,
                markerfacecolor=S.PD,
                markeredgecolor=S.INK,
                markeredgewidth=0.6,
                zorder=5,
            )

    ax.set_ylim(0, limit)
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels)
    ax.tick_params(axis="x", length=0, pad=5)
    ax.set_ylabel("End-to-end RTL cycles\n(lower is better)")
    ax.yaxis.set_major_formatter(mticker.StrMethodFormatter("{x:,.0f}"))
    ax.grid(axis="y", color=S.GRID, linewidth=0.55, zorder=0)
    ax.spines["left"].set_visible(False)
    ax.tick_params(axis="y", length=0)
    return fig


# Draw one workload while varying only result-slot count
def fig_workload_result_slots(study: Study, style: S.Style, layer: str) -> Figure:
    baseline, family = result_slot_comparison_family(study)
    labels = [
        f"SA baseline\n{baseline.geometry.replace('x', '×')}, "
        f"{baseline.input_port_bits}/{baseline.output_port_bits}b",
        *[
            f"CIM\nrslots={design.slots}" + (" (PD)" if design.key == study.pd_key else "")
            for design in family
        ],
    ]
    return _comparison_bars(
        study,
        style,
        layer,
        baseline,
        family,
        result_slot_parameter_lines(study),
        labels,
        (S.SA, S.CIM_R1, S.CIM_R2, S.CIM_R6, S.PD),
    )


# Draw one workload while varying only B-set count at rslots=8
def fig_workload_b_sets(study: Study, style: S.Style, layer: str) -> Figure:
    baseline, family = b_set_comparison_family(study)
    labels = [
        f"SA baseline\n{baseline.geometry.replace('x', '×')}, "
        f"{baseline.input_port_bits}/{baseline.output_port_bits}b",
        *[
            f"CIM\n{design.sets} B sets" + (" (PD)" if design.key == study.pd_key else "")
            for design in family
        ],
    ]
    return _comparison_bars(
        study,
        style,
        layer,
        baseline,
        family,
        b_set_parameter_lines(study),
        labels,
        (S.SA, S.CIM_SETS2, S.CIM_SETS8, S.PD),
    )


# Draw a double-column paper table of logical operation shapes
def fig_operation_shapes(study: Study, style: S.Style) -> Figure:
    rows = operation_shape_rows(study)
    size = (7.1, 3.15) if style.name == "paper" else (12.0, 5.6)
    fig = Figure(figsize=size, facecolor=S.SURFACE)
    ax = fig.add_axes((0.025, 0.05, 0.95, 0.86))
    ax.set_axis_off()
    fig.text(
        0.5,
        0.97,
        "Logical operation shapes for measured workloads",
        ha="center",
        va="top",
        fontsize=style.title_pt + 1,
        fontweight="bold",
    )

    headers = ("Workload", "Operation", "Input", "Weight / RHS", "Output", "Equivalent GEMM", "MACs")
    widths = (0.22, 0.105, 0.135, 0.135, 0.135, 0.16, 0.11)
    x_edges = [0.0]
    for width in widths:
        x_edges.append(x_edges[-1] + width)
    row_height = 1 / (len(rows) + 1)
    ax.add_patch(Rectangle((0, 1 - row_height), 1, row_height, facecolor=S.SA, edgecolor="none"))
    for index, (header, left, right) in enumerate(zip(headers, x_edges[:-1], x_edges[1:])):
        align = "right" if index == len(headers) - 1 else "left"
        x = right - 0.008 if align == "right" else left + 0.008
        ax.text(
            x,
            1 - row_height / 2,
            header,
            ha=align,
            va="center",
            color=S.SURFACE,
            fontsize=style.small_pt,
            fontweight="bold",
        )

    for row_index, row in enumerate(rows):
        top = 1 - (row_index + 1) * row_height
        bottom = top - row_height
        if row_index % 2:
            ax.add_patch(Rectangle((0, bottom), 1, row_height, facecolor="#f4f5f6", edgecolor="none"))
        values = (
            row["workload"],
            row["operation"],
            row["input_shape"],
            row["weight_or_rhs_shape"],
            row["output_shape"],
            row["equivalent_gemm"],
            f"{row['logical_macs']:,}",
        )
        for index, (value, left, right) in enumerate(zip(values, x_edges[:-1], x_edges[1:])):
            align = "right" if index == len(values) - 1 else "left"
            x = right - 0.008 if align == "right" else left + 0.008
            ax.text(
                x,
                bottom + row_height / 2,
                str(value),
                ha=align,
                va="center",
                fontsize=style.small_pt,
                color=S.INK,
            )
        ax.plot((0, 1), (bottom, bottom), color=S.GRID, linewidth=0.5)

    ax.text(
        0,
        -0.025,
        "Convolutions use M = output spatial positions, K = kernel height × kernel width × input channels, and N = output channels.",
        ha="left",
        va="top",
        fontsize=style.small_pt,
        color=S.INK_SECONDARY,
    )
    return fig
