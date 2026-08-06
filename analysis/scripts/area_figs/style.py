"""Figure styling: fixed geometry, validated palette, print-safe defaults.

Two presets. ``paper`` is a single-column IEEE/ACM figure — serif text, 8 pt
base, sized in inches so a PDF drops into LaTeX at 1:1 with no scaling.
``slide`` is the same chart re-typeset for 16:9 projection — sans-serif and
larger type.

The categorical palette is the validated default from the data-viz reference
instance, used in slot order so adjacent stack segments keep their checked
colour-vision separation:

    node scripts/validate_palette.js "#2a78d6,#eb6834,#1baf7a,#eda100" --mode light
    -> lightness band PASS, chroma floor PASS,
       CVD separation PASS (worst adjacent dE 9.1, protan),
       normal-vision floor PASS (worst adjacent dE 22.9),
       contrast WARN -> relief required, satisfied by the direct value labels
       every chart here carries plus the companion table.
"""

from __future__ import annotations

from dataclasses import dataclass

import matplotlib as mpl

# --- categorical slots, in fixed order -------------------------------------- #
SERIES = ("#2a78d6", "#eb6834", "#1baf7a", "#eda100")

#: Component key -> categorical slot. Stack order follows this order, so the
#: adjacency the validator checked is the adjacency that gets rendered.
PART_COLOR = {
    "sram": SERIES[0],
    "periph": SERIES[1],
    "array_logic": SERIES[2],
    "cim_macro": SERIES[3],
}

# --- text and surface tokens ------------------------------------------------ #
SURFACE = "#ffffff"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#8a8880"
GRID = "#e4e3df"
RULE = "#c9c8c3"


@dataclass(frozen=True)
class Style:
    name: str
    base_pt: float
    small_pt: float
    tick_pt: float
    title_pt: float
    bar_thickness_in: float  # bar thickness, held constant across figure sizes
    corner_pt: float  # rounded radius on the data end of a bar
    gap_pt: float  # surface gap between touching marks
    min_segment_pt: float  # floor on a stack segment's drawn thickness
    dpi: int

    @property
    def label_pt(self) -> float:
        return self.small_pt


PAPER = Style(
    name="paper",
    base_pt=8.0,
    small_pt=7.0,
    tick_pt=7.0,
    title_pt=8.5,
    bar_thickness_in=0.46,
    corner_pt=0.0,  # square bars are the convention in print
    gap_pt=0.9,
    min_segment_pt=1.4,
    dpi=600,
)

SLIDE = Style(
    name="slide",
    base_pt=13.0,
    small_pt=11.5,
    tick_pt=11.5,
    title_pt=15.0,
    bar_thickness_in=0.72,
    corner_pt=2.5,
    gap_pt=1.6,
    min_segment_pt=2.4,
    dpi=300,
)

STYLES = {"paper": PAPER, "slide": SLIDE}

_SERIF = ["Times New Roman", "Nimbus Roman", "Liberation Serif", "STIXGeneral", "DejaVu Serif"]
_SANS = ["Helvetica Neue", "Helvetica", "Arial", "Liberation Sans", "DejaVu Sans"]


def apply(style: Style) -> Style:
    """Install ``style`` into the global rcParams and return it."""
    serif = style.name == "paper"
    mpl.rcParams.update(
        {
            "font.family": "serif" if serif else "sans-serif",
            "font.serif": _SERIF,
            "font.sans-serif": _SANS,
            "mathtext.fontset": "stix" if serif else "dejavusans",
            "font.size": style.base_pt,
            "axes.titlesize": style.title_pt,
            "axes.labelsize": style.base_pt,
            "xtick.labelsize": style.tick_pt,
            "ytick.labelsize": style.tick_pt,
            "legend.fontsize": style.small_pt,
            "axes.edgecolor": RULE,
            "axes.labelcolor": INK,
            "axes.titlecolor": INK,
            "axes.linewidth": 0.6,
            "axes.spines.top": False,
            "axes.spines.right": False,
            "axes.grid": False,
            "grid.color": GRID,
            "grid.linewidth": 0.6,
            "grid.linestyle": "-",
            "text.color": INK,
            "xtick.color": INK_SECONDARY,
            "ytick.color": INK_SECONDARY,
            "xtick.direction": "out",
            "ytick.direction": "out",
            "xtick.major.size": 2.5,
            "ytick.major.size": 2.5,
            "xtick.major.width": 0.6,
            "ytick.major.width": 0.6,
            "legend.frameon": False,
            "legend.handlelength": 1.1,
            "legend.handleheight": 0.9,
            "legend.handletextpad": 0.5,
            "legend.columnspacing": 1.3,
            "legend.labelspacing": 0.45,
            "figure.facecolor": SURFACE,
            "axes.facecolor": SURFACE,
            "savefig.facecolor": SURFACE,
            "savefig.dpi": style.dpi,
            "pdf.fonttype": 42,  # embed TrueType so the PDF is editable/searchable
            "ps.fonttype": 42,
            "svg.fonttype": "none",
        }
    )
    return style


def ink_on(hex_color: str) -> str:
    """Pick white or ink for a label set inside a filled mark."""
    r, g, b = (int(hex_color[i : i + 2], 16) / 255 for i in (1, 3, 5))
    lin = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in (r, g, b)]
    luminance = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]
    return "#ffffff" if luminance < 0.45 else INK
