"""Print-safe styling shared by the performance figures."""

from __future__ import annotations

from dataclasses import dataclass

import matplotlib as mpl

PD = "#a66f00"
SA = "#282828"
CIM_R1 = "#c4cfdd"
CIM_R2 = "#92acd0"
CIM_R6 = "#557fac"
CIM_SETS2 = "#92acd0"
CIM_SETS8 = "#557fac"
SURFACE = "#ffffff"
INK = "#0b0b0b"
INK_SECONDARY = "#52514e"
INK_MUTED = "#8a8880"
GRID = "#e4e3df"
RULE = "#c9c8c3"


# Store typography and raster-resolution choices for one output preset
@dataclass(frozen=True)
class Style:
    name: str
    base_pt: float
    small_pt: float
    tick_pt: float
    title_pt: float
    dpi: int


PAPER = Style("paper", base_pt=8.0, small_pt=7.0, tick_pt=7.0, title_pt=8.5, dpi=600)
SLIDE = Style("slide", base_pt=13.0, small_pt=11.5, tick_pt=11.5, title_pt=15.0, dpi=300)
STYLES = {"paper": PAPER, "slide": SLIDE}

_SERIF = ["Times New Roman", "Nimbus Roman", "Liberation Serif", "STIXGeneral", "DejaVu Serif"]
_SANS = ["Helvetica Neue", "Helvetica", "Arial", "Liberation Sans", "DejaVu Sans"]


# Install one print or presentation style into Matplotlib
def apply(style: Style) -> Style:
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
            "pdf.fonttype": 42,
            "ps.fonttype": 42,
            "svg.fonttype": "none",
        }
    )
    return style
