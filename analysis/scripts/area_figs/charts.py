"""Chart builders for the MatrixUnit area comparison.

Every axes is placed with an explicit rectangle rather than a layout engine, so
the inch geometry is known before anything is drawn. That makes the point-to-
data conversions exact, which is what lets bars keep a fixed thickness, stack
segments keep a real surface gap instead of a stroke, and inline labels be
measured for fit before they are committed.
"""

from __future__ import annotations

from dataclasses import dataclass

import matplotlib.ticker as mticker
from matplotlib.figure import Figure
from matplotlib.patches import Patch, PathPatch
from matplotlib.path import Path

from . import style as S, tables as T
from .data import AreaModel

KUM2 = 1e3  # charts report area in 10^3 um^2


# --------------------------------------------------------------------------- #
# geometry helper
# --------------------------------------------------------------------------- #


@dataclass
class Frame:
    """An axes whose size in inches is known, so points convert to data units."""

    fig: Figure
    ax: object

    @property
    def size_in(self) -> tuple[float, float]:
        fw, fh = self.fig.get_size_inches()
        pos = self.ax.get_position()
        return pos.width * fw, pos.height * fh

    def dx(self, pt: float) -> float:
        """``pt`` points expressed in x data units."""
        lo, hi = self.ax.get_xlim()
        return (pt / 72.0) / self.size_in[0] * (hi - lo)

    def dy(self, pt: float) -> float:
        """``pt`` points expressed in y data units."""
        lo, hi = self.ax.get_ylim()
        return (pt / 72.0) / self.size_in[1] * (hi - lo)

    def x_in(self, inches: float) -> float:
        lo, hi = self.ax.get_xlim()
        return inches / self.size_in[0] * (hi - lo)

    def y_in(self, inches: float) -> float:
        lo, hi = self.ax.get_ylim()
        return inches / self.size_in[1] * (hi - lo)

    def text_size_pt(self, txt) -> tuple[float, float]:
        """Rendered width and height of a text artist, in points."""
        bb = txt.get_window_extent(renderer=self.fig.canvas.get_renderer())
        k = 72.0 / self.fig.dpi
        return bb.width * k, bb.height * k

    def shift_log_x(self, x: float, pt: float) -> float:
        """Move ``x`` by ``pt`` points along a log-scaled x axis."""
        import math

        lo, hi = self.ax.get_xlim()
        span = math.log10(hi) - math.log10(lo)
        return 10 ** (math.log10(x) + (pt / 72.0) / self.size_in[0] * span)


def _capped_rect(x0, x1, y0, y1, rx, ry, *, cap: str) -> Path:
    """Rectangle with the two corners on ``cap`` rounded and the rest square."""
    rx = min(rx, abs(x1 - x0) / 2)
    ry = min(ry, abs(y1 - y0) / 2)
    if rx <= 0 or ry <= 0 or cap not in ("top", "right"):
        return Path(
            [(x0, y0), (x0, y1), (x1, y1), (x1, y0), (x0, y0)],
            [Path.MOVETO, Path.LINETO, Path.LINETO, Path.LINETO, Path.CLOSEPOLY],
        )
    if cap == "top":
        verts = [
            (x0, y0), (x0, y1 - ry), (x0, y1), (x0 + rx, y1),
            (x1 - rx, y1), (x1, y1), (x1, y1 - ry), (x1, y0), (x0, y0),
        ]
        codes = [
            Path.MOVETO, Path.LINETO, Path.CURVE3, Path.CURVE3,
            Path.LINETO, Path.CURVE3, Path.CURVE3, Path.LINETO, Path.CLOSEPOLY,
        ]
    else:  # cap == "right"
        verts = [
            (x0, y0), (x1 - rx, y0), (x1, y0), (x1, y0 + ry),
            (x1, y1 - ry), (x1, y1), (x1 - rx, y1), (x0, y1), (x0, y0),
        ]
        codes = [
            Path.MOVETO, Path.LINETO, Path.CURVE3, Path.CURVE3,
            Path.LINETO, Path.CURVE3, Path.CURVE3, Path.LINETO, Path.CLOSEPOLY,
        ]
    return Path(verts, codes)


def _visible_span(lo: float, hi: float, y0: float, y1: float, floor: float):
    """Keep a stack segment readable when it is only a fraction of a point thick.

    Give the surface gap back first, so exact geometry survives wherever it can;
    only widen the band symmetrically if the segment would still be invisible.
    Returns ``(lo, hi, widened)``.
    """
    if hi - lo >= floor:
        return lo, hi, False
    if y1 - y0 >= floor:
        return y0, y1, False
    mid = (y0 + y1) / 2
    return mid - floor / 2, mid + floor / 2, True


def _column_major(items: list, ncol: int) -> list:
    """Reorder so Matplotlib's column-major legend fill reads left-to-right."""
    nrow = -(-len(items) // ncol)
    out = []
    for c in range(ncol):
        for r in range(nrow):
            i = r * ncol + c
            if i < len(items):
                out.append(items[i])
    return out


def _legend(fig, model: AreaModel, *, ncol: int, y: float, fontsize: float):
    handles = _column_major(
        [Patch(facecolor=S.PART_COLOR[p.key], edgecolor="none", label=p.short) for p in model.parts],
        ncol,
    )
    return fig.legend(
        handles=handles,
        loc="lower center",
        bbox_to_anchor=(0.5, y),
        ncol=ncol,
        fontsize=fontsize,
        borderpad=0.0,
        borderaxespad=0.0,
    )


def _fmt_k(v: float) -> str:
    return f"{v / KUM2:,.1f}"


# --------------------------------------------------------------------------- #
# panel: absolute stacked columns
# --------------------------------------------------------------------------- #


def draw_stacked_absolute(fr: Frame, model: AreaModel, st: S.Style, *, annotate: bool = True):
    ax, fig = fr.ax, fr.fig
    xs = {"sa": 0.0, "cim": 1.0}
    totals = {"sa": model.sa_total, "cim": model.cim_total}

    ax.set_xlim(-0.62, 1.82 if annotate else 1.62)
    ax.set_ylim(0, model.sa_total * 1.13)
    ax.set_xticks(list(xs.values()))
    geom = model.meta["geometry"].replace("×", "$\\times$")
    ax.set_xticklabels([f"Systolic array\n{geom}", f"CIM\n{geom}"], color=S.INK)
    ax.tick_params(axis="x", length=0, pad=4)
    ax.set_ylabel("Total cell area (10$^3$ $\\mu$m$^2$)")
    ax.yaxis.set_major_locator(mticker.MultipleLocator(200 * KUM2))
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v / KUM2:,.0f}"))
    ax.set_axisbelow(True)
    ax.grid(axis="y", which="major")
    ax.spines["left"].set_visible(False)
    ax.tick_params(axis="y", length=0)

    width = min(fr.x_in(st.bar_thickness_in), 0.62)
    gap = fr.dy(st.gap_pt)
    rx, ry = fr.dx(st.corner_pt), fr.dy(st.corner_pt)

    overflow: dict[str, list] = {"sa": [], "cim": []}
    for design in ("sa", "cim"):
        cursor = 0.0
        drawn = [p for p in model.parts if getattr(p, design) > 0]
        for i, part in enumerate(drawn):
            value = getattr(part, design)
            y0, y1 = cursor, cursor + value
            cursor = y1
            top = i == len(drawn) - 1
            lo = y0 + (gap / 2 if i else 0.0)
            hi = y1 - (0.0 if top else gap / 2)
            lo, hi, widened = _visible_span(lo, hi, y0, y1, fr.dy(st.min_segment_pt))
            x0 = xs[design] - width / 2
            path = _capped_rect(x0, x0 + width, lo, hi, rx, ry, cap="top" if top else "none")
            ax.add_patch(
                PathPatch(
                    path,
                    facecolor=S.PART_COLOR[part.key],
                    edgecolor="none",
                    zorder=3.5 if widened else 3,
                )
            )

            # Prefer value + share inside the segment, fall back to the value
            # alone, and only then to a leader line. A label is never clipped.
            share = value / totals[design] * 100
            for candidate in (f"{_fmt_k(value)}\n{share:.1f}%", _fmt_k(value)):
                txt = ax.text(
                    xs[design],
                    (y0 + y1) / 2,
                    candidate,
                    ha="center",
                    va="center",
                    color=S.ink_on(S.PART_COLOR[part.key]),
                    fontsize=st.label_pt,
                    linespacing=1.25,
                    zorder=5,
                )
                _, h_pt = fr.text_size_pt(txt)
                if fr.dy(h_pt + 4.0) <= (y1 - y0):
                    break
                txt.remove()
                txt = None
            if txt is None:
                overflow[design].append(
                    (part, y0, y1, f"{_fmt_k(value)} ({share:.1f}%)", _fmt_k(value))
                )

        ax.text(
            xs[design],
            totals[design] + fr.dy(5),
            _fmt_k(totals[design]),
            ha="center",
            va="bottom",
            fontsize=st.base_pt,
            fontweight="bold",
            color=S.INK,
            zorder=5,
        )

    # segments too thin for an inline label get a leader line instead of a crop
    x_hi = ax.get_xlim()[1]
    for design, items in overflow.items():
        anchor_x = xs[design] + width / 2
        cursor_y = None
        min_step = fr.dy(st.label_pt * 1.9)
        for part, y0, y1, label, short in items:
            y = (y0 + y1) / 2
            if cursor_y is not None and y - cursor_y < min_step:
                y = cursor_y + min_step
            cursor_y = y
            tip_x = anchor_x + fr.x_in(0.10)
            ax.plot(
                [anchor_x, tip_x],
                [(y0 + y1) / 2, y],
                color=S.INK_MUTED,
                linewidth=0.6,
                solid_capstyle="butt",
                zorder=4,
            )
            txt = ax.text(
                tip_x + fr.dx(1.5),
                y,
                label,
                ha="left",
                va="center",
                fontsize=st.label_pt,
                color=S.INK_SECONDARY,
                zorder=5,
            )
            # Drop the share when the full callout would run past the axes.
            w_pt, _ = fr.text_size_pt(txt)
            if txt.get_position()[0] + fr.dx(w_pt) > x_hi:
                txt.set_text(short)

    if annotate:
        guide_x = 1.0 + width / 2 + fr.x_in(0.06)
        arrow_x = min(guide_x + fr.x_in(0.34), ax.get_xlim()[1] - fr.x_in(0.30))
        ax.plot(
            [-0.62, arrow_x],
            [model.sa_total] * 2,
            color=S.INK_MUTED,
            linewidth=0.6,
            linestyle=(0, (2.5, 2.0)),
            zorder=2,
        )
        ax.annotate(
            "",
            xy=(arrow_x, model.cim_total),
            xytext=(arrow_x, model.sa_total),
            arrowprops=dict(arrowstyle="<->", color=S.INK, linewidth=0.8, shrinkA=0, shrinkB=0),
            zorder=5,
        )
        ax.text(
            arrow_x + fr.dx(4),
            (model.sa_total + model.cim_total) / 2,
            f"{model.reduction:.2f}$\\times$ smaller",
            rotation=90,
            ha="left",
            va="center",
            fontsize=st.small_pt,
            color=S.INK,
            zorder=5,
        )


# --------------------------------------------------------------------------- #
# panel: ratio dot plot
# --------------------------------------------------------------------------- #


def ratio_rows(model: AreaModel):
    """Rows for the ratio panel: ``(label, SA/CIM factor, colour, emphasised)``."""
    p = model.part
    return [
        ("Total MatrixUnit", model.reduction, S.INK, True),
        ("SRAM buffers", 1 / p("sram").ratio, S.PART_COLOR["sram"], False),
        ("MatrixUnit logic", 1 / p("periph").ratio, S.PART_COLOR["periph"], False),
        ("Compute array (all-in)", 1 / p("compute").ratio, S.INK_SECONDARY, False),
        ("Array logic only", 1 / p("array_logic").ratio, S.PART_COLOR["array_logic"], False),
        ("All standard cells", 1 / p("std_cells").ratio, S.INK_SECONDARY, False),
    ]


def draw_ratio(fr: Frame, model: AreaModel, st: S.Style):
    ax = fr.ax
    rows = ratio_rows(model)
    ys = list(range(len(rows) - 1, -1, -1))

    ax.set_xscale("log")
    ax.set_xlim(0.92, 175)
    ax.set_ylim(-0.6, len(rows) - 0.4)
    ax.set_yticks(ys)
    ax.set_yticklabels([lbl for lbl, _, _, _ in rows], color=S.INK)
    for tick, (_, _, _, emph) in zip(ax.get_yticklabels(), rows):
        tick.set_fontweight("bold" if emph else "normal")
        tick.set_color(S.INK if emph else S.INK_SECONDARY)
    ax.tick_params(axis="y", length=0, pad=4)
    ax.set_xlabel("SA area / CIM area  (log scale)")
    ax.xaxis.set_major_locator(mticker.FixedLocator([1, 2, 5, 10, 20, 50, 100]))
    ax.xaxis.set_minor_locator(mticker.NullLocator())
    ax.xaxis.set_major_formatter(mticker.FuncFormatter(lambda v, _: f"{v:g}$\\times$"))
    ax.set_axisbelow(True)
    ax.grid(axis="x", which="major")
    ax.spines["left"].set_visible(False)
    ax.spines["bottom"].set_visible(False)
    ax.tick_params(axis="x", length=0)

    # Stems grow from parity; the 1x tick already names it, so no extra label.
    ax.axvline(1.0, color=S.RULE, linewidth=0.8, zorder=2)

    marker_pt = max(6.5, st.base_pt * 0.85)
    hi = ax.get_xlim()[1]
    for y, (_, factor, color, emph) in zip(ys, rows):
        size = marker_pt * (1.15 if emph else 1.0)
        # Value label sits right of the dot when there is room, otherwise it
        # flips left and the stem is cut back so the two never overlap.
        txt = ax.text(
            factor,
            y,
            f"{factor:.2f}$\\times$",
            ha="left",
            va="center",
            fontsize=st.label_pt,
            fontweight="bold" if emph else "normal",
            color=S.INK if emph else S.INK_SECONDARY,
            zorder=5,
        )
        w_pt, _ = fr.text_size_pt(txt)
        pad = size / 2 + 3.0
        stem_end = factor
        if fr.shift_log_x(factor, pad + w_pt) < hi * 0.97:
            txt.set_x(fr.shift_log_x(factor, pad))
        else:
            txt.set_ha("right")
            txt.set_x(fr.shift_log_x(factor, -pad))
            stem_end = fr.shift_log_x(factor, -(pad + w_pt + 3.0))

        ax.plot(
            [1.0, stem_end],
            [y, y],
            color=color,
            linewidth=2.0 if emph else 1.4,
            alpha=1.0 if emph else 0.55,
            solid_capstyle="butt",
            zorder=3,
        )
        ax.plot(
            [factor],
            [y],
            marker="o",
            markersize=size,
            color=color,
            markeredgecolor=S.SURFACE,
            markeredgewidth=1.6,
            zorder=4,
        )


# --------------------------------------------------------------------------- #
# figures
# --------------------------------------------------------------------------- #


def _new(figsize, rect):
    fig = Figure(figsize=figsize)
    from matplotlib.backends.backend_agg import FigureCanvasAgg

    FigureCanvasAgg(fig)
    ax = fig.add_axes(rect)
    return Frame(fig, ax)


#: Default sizes. Heights match so the two line up when placed side by side,
#: but each gets the width its form wants: a two-column bar chart is narrow, a
#: six-row dot plot is wide. Widths sum to 9.25 in, inside a 9.30 in slide box.
SIZE_STACKED = {"paper": (3.30, 3.30), "slide": (4.10, 4.30)}
SIZE_RATIO = {"paper": (3.80, 3.30), "slide": (5.15, 4.30)}

TITLE_STACKED = "Absolute area breakdown"
TITLE_RATIO = "Area reduction by component"


def _figure_title(fig, title: str, st: S.Style) -> None:
    fig.text(0.018, 0.985, title, ha="left", va="top", fontsize=st.title_pt, color=S.INK)


def fig_stacked(model: AreaModel, st: S.Style, figsize=None, title=TITLE_STACKED) -> Figure:
    """Absolute stacked breakdown. Pass ``title=None`` for an untitled figure."""
    paper = st.name == "paper"
    figsize = figsize or SIZE_STACKED[st.name]
    if title:
        rect = (0.185, 0.140, 0.770, 0.635) if paper else (0.165, 0.140, 0.790, 0.635)
        legend_y = 0.790
    else:
        rect = (0.155, 0.155, 0.795, 0.735) if paper else (0.125, 0.145, 0.845, 0.745)
        legend_y = 0.905 if paper else 0.90
    fr = _new(figsize, rect)
    draw_stacked_absolute(fr, model, st)
    _legend(fr.fig, model, ncol=2, y=legend_y, fontsize=st.small_pt)
    if title:
        _figure_title(fr.fig, title, st)
    return fr.fig


def fig_ratio(model: AreaModel, st: S.Style, figsize=None, title=TITLE_RATIO) -> Figure:
    """SA/CIM reduction dot plot. Pass ``title=None`` for an untitled figure."""
    paper = st.name == "paper"
    figsize = figsize or SIZE_RATIO[st.name]
    if title:
        rect = (0.340, 0.155, 0.630, 0.715) if paper else (0.325, 0.150, 0.640, 0.720)
    else:
        rect = (0.300, 0.185, 0.520, 0.755) if paper else (0.285, 0.165, 0.545, 0.775)
    fr = _new(figsize, rect)
    draw_ratio(fr, model, st)
    if title:
        _figure_title(fr.fig, title, st)
    return fr.fig


def fig_table(model: AreaModel, st: S.Style, figsize=None, *, sub_rows: bool = True) -> Figure:
    """The breakdown typeset as a booktabs-style table, rendered as a figure."""
    rows = T.build_rows(model, sub_rows=sub_rows)
    paper = st.name == "paper"
    n = len(rows) + 2  # + group header + column header
    row_in = 0.185 if paper else 0.30
    figsize = figsize or (7.0 if paper else 10.6, n * row_in + (0.30 if paper else 0.45))
    pad_y = 0.16 / figsize[1]
    fr = _new(figsize, (0.012, pad_y, 0.976, 1 - 2 * pad_y))
    ax = fr.ax
    ax.set_xlim(0, 1)
    ax.set_ylim(0, n)
    ax.axis("off")

    # right edge of each numeric column, plus the label column at the left
    cols = (0.470, 0.560, 0.755, 0.845, 1.000)
    swatch_w, label_x = 0.011, 0.020

    def rule(y, x0=0.0, x1=1.0, lw=0.9, color=S.INK):
        ax.plot([x0, x1], [y, y], color=color, linewidth=lw, solid_capstyle="butt", clip_on=False)

    def cell(x, y, text, *, bold=False, italic=False, color=S.INK, ha="right"):
        ax.text(
            x, y, text, ha=ha, va="center", fontsize=st.label_pt, color=color,
            fontweight="bold" if bold else "normal",
            fontstyle="italic" if italic else "normal",
        )

    y = n - 0.5
    rule(y + 0.5, lw=1.1)  # toprule
    for label, (a, b) in (
        ("Systolic array", (0.375, 0.560)),
        ("CIM", (0.655, 0.845)),
    ):
        cell((a + b) / 2, y, label, color=S.INK_SECONDARY, ha="center")
        rule(y - 0.34, a, b, lw=0.6, color=S.RULE)
    y -= 1
    cell(label_x, y, "Component", bold=True, ha="left")
    for x, text in zip(cols, ("10³ µm²", "%", "10³ µm²", "%", "SA/CIM")):
        cell(x, y, text, bold=True)
    rule(y - 0.5)

    for row in rows:
        y -= 1
        if row.kind == "section":
            cell(label_x, y, row.label, italic=True, color=S.INK_SECONDARY, ha="left")
            continue
        if row.kind == "total":
            rule(y + 0.5, lw=0.6, color=S.RULE)
        bold = row.kind == "total"
        italic = row.kind == "sub"
        color = S.INK if row.kind in ("component", "total") else S.INK_SECONDARY

        x = label_x
        if row.kind == "component":
            key = next((p.key for p in model.parts if p.label == row.label), None)
            if key:
                ax.add_patch(
                    PathPatch(
                        _capped_rect(
                            label_x, label_x + swatch_w, y - 0.19, y + 0.19, 0, 0, cap="none"
                        ),
                        facecolor=S.PART_COLOR[key], edgecolor="none",
                    )
                )
                x = label_x + swatch_w + 0.010
        elif row.kind == "sub":
            x = label_x + 0.030
        cell(x, y, row.label, bold=bold, italic=italic, color=color, ha="left")

        values = (
            T._num(row.sa, KUM2, 1),
            T._pct(row.sa, model.sa_total),
            T._num(row.cim, KUM2, 1),
            T._pct(row.cim, model.cim_total),
            T._factor(row).replace("×", "$\\times$"),
        )
        for cx, text in zip(cols, values):
            cell(cx, y, text, bold=bold, italic=italic, color=color)

    rule(y - 0.5, lw=1.1)
    return fr.fig
