"""Derive the SA-vs-CIM MatrixUnit area model from the comparison workbook.

Figures, tables, and slides all read this module, so a number in a plot and the
same number in a table cannot drift apart. Every derived quantity is checked
against the workbook's own ``Comparison`` sheet before it is handed out.

Composition rule (from the workbook methodology): a MatrixUnit DC report counts
its compute-array child once as a reduced block abstraction, so the full-design
area is ``parent report - embedded abstraction + standalone mapped child``,
applied recursively. For the systolic array that means replacing the
``SystolicArray`` abstraction inside ``MatrixUnit`` and then the 4,096
``ProcessingElement`` abstractions inside ``SystolicArray``.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import openpyxl

DEFAULT_WORKBOOK = (
    Path(__file__).resolve().parents[2] / "matrixunit_sa_vs_cim_64x64_area_comparison_1ns.xlsx"
)

#: Result-slot points present in the workbook, mapped to their ``Area Data`` key prefix.
RSLOT_PREFIX = {2: "cim2", 6: "cim6", 8: "cim8"}

_TOL = 1e-6  # relative tolerance for the cross-checks against the Comparison sheet


class WorkbookMismatch(RuntimeError):
    """Raised when a derived value disagrees with the workbook's own total."""


# --------------------------------------------------------------------------- #
# model
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class Part:
    """One area component, measured for both designs.

    ``sa``/``cim`` are total cell area in um^2. ``comb``/``noncomb`` carry the
    standard-cell split as ``(sa, cim)`` pairs where the component has one.
    """

    key: str
    label: str  # full label, for tables
    short: str  # compact label, for chart legends and axes
    sa: float
    cim: float
    kind: str = "cell"  # "cell" (standard cells) or "macro" (hard macro area)
    comb: tuple[float, float] | None = None
    noncomb: tuple[float, float] | None = None
    note: str = ""

    @property
    def ratio(self) -> float | None:
        """CIM area divided by SA area, or ``None`` where SA has no counterpart."""
        return self.cim / self.sa if self.sa else None


@dataclass(frozen=True)
class AreaModel:
    rslots: int
    parts: tuple[Part, ...]
    rollups: tuple[Part, ...]
    meta: dict = field(default_factory=dict)

    @property
    def sa_total(self) -> float:
        return sum(p.sa for p in self.parts)

    @property
    def cim_total(self) -> float:
        return sum(p.cim for p in self.parts)

    @property
    def ratio(self) -> float:
        """CIM total / SA total. Below 1.0 means CIM is smaller."""
        return self.cim_total / self.sa_total

    @property
    def reduction(self) -> float:
        """SA total / CIM total, i.e. the 'N x smaller' factor."""
        return self.sa_total / self.cim_total

    @property
    def savings(self) -> float:
        """Absolute area removed, in um^2."""
        return self.sa_total - self.cim_total

    def part(self, key: str) -> Part:
        for p in (*self.parts, *self.rollups):
            if p.key == key:
                return p
        raise KeyError(key)


# --------------------------------------------------------------------------- #
# workbook readers
# --------------------------------------------------------------------------- #


def _area_data(wb) -> dict[str, float]:
    """``Area Data`` sheet: column A is the key, column E the value."""
    ws = wb["Area Data"]
    out: dict[str, float] = {}
    for row in ws.iter_rows(min_row=5, values_only=True):
        key, value = row[0], row[4]
        if isinstance(key, str) and isinstance(value, (int, float)):
            out[key] = float(value)
    return out


def _comparison_rows(wb) -> dict[str, list]:
    """``Comparison`` sheet keyed by the row's column-A label.

    First occurrence wins: the headline comparison block sits at the top of the
    sheet, and supplementary blocks (storage capacity, macro plug-in, ...) get
    appended below it over time. Keeping the first match means a later block
    reusing a label cannot silently shadow the value we actually want.
    """
    ws = wb["Comparison"]
    out: dict[str, list] = {}
    for row in ws.iter_rows(values_only=True):
        if isinstance(row[0], str):
            out.setdefault(row[0].strip(), list(row))
    return out


def _design_columns(wb, rslots: int) -> tuple[int, int]:
    """Locate the SA and CIM columns from the header row, not by position.

    The workbook gains rows and blocks as the study grows; reading the header
    means an inserted column cannot silently shift every number by one.
    """
    ws = wb["Comparison"]
    want = f"CIM rslots={rslots}"
    for row in ws.iter_rows(values_only=True):
        cells = [c.strip() if isinstance(c, str) else c for c in row]
        if cells[0] != "Metric" or "SA 64x64" not in cells:
            continue
        if want in cells:
            return cells.index("SA 64x64"), cells.index(want)
    raise WorkbookMismatch(
        f"could not find a Comparison header row with 'SA 64x64' and {want!r}; "
        "the sheet layout changed"
    )


def _check(name: str, derived: float, expected: float) -> None:
    if expected == 0:
        ok = abs(derived) < 1e-9
    else:
        ok = abs(derived - expected) / abs(expected) < _TOL
    if not ok:
        raise WorkbookMismatch(
            f"{name}: derived {derived:.6f} um^2 but the workbook reports {expected:.6f} um^2"
        )


# --------------------------------------------------------------------------- #
# loader
# --------------------------------------------------------------------------- #


def load(workbook: Path | str = DEFAULT_WORKBOOK, rslots: int = 8) -> AreaModel:
    """Build the area model for ``SA 64x64`` versus ``CIM rslots=<rslots>``."""
    if rslots not in RSLOT_PREFIX:
        raise ValueError(f"rslots must be one of {sorted(RSLOT_PREFIX)}, got {rslots}")

    wb = openpyxl.load_workbook(workbook, data_only=True)
    d = _area_data(wb)
    cmp_rows = _comparison_rows(wb)
    cim = RSLOT_PREFIX[rslots]
    sa_col, cim_col = _design_columns(wb, rslots)

    # --- physical CIM macro plug-in (an editable input on the Comparison sheet) ---
    macro_each = float(cmp_rows["CIM macro area per instance (µm²)"][1])
    macro_count = int(cmp_rows["CIM macro instances (count)"][1])
    cim_macro_area = macro_each * macro_count

    # --- SA: compose SystolicArray from its 4,096 ProcessingElement children ---
    pe_n = int(d["sa_array_pe_abstraction_count"])
    pe_abstraction_total = d["sa_array_pe_abstraction_total"]
    # Whatever the standalone SystolicArray holds beyond the PE abstractions is
    # array-level glue; it is 0.11 um^2 here and the workbook books it as
    # combinational, so mirror that.
    sa_array_glue = d["sa_array_total"] - pe_abstraction_total
    sa_array_comb = pe_n * d["sa_pe_comb"] + sa_array_glue
    sa_array_noncomb = pe_n * d["sa_pe_noncomb"]

    # The block abstraction the parent counts is flagged noncombinational in DC,
    # so it comes out of the parent's noncombinational area only.
    sa_periph_comb = d["sa_top_comb"]
    sa_periph_noncomb = d["sa_top_noncomb"] - d["sa_top_array_abstraction"]

    # --- CIM: one CIMArray replacement, no recursion below it ---
    cim_array_comb = d[f"{cim}_array_comb"]
    cim_array_noncomb = d[f"{cim}_array_noncomb"]
    cim_periph_comb = d[f"{cim}_top_comb"]
    cim_periph_noncomb = d[f"{cim}_top_noncomb"] - d[f"{cim}_top_array_abstraction"]

    parts = (
        Part(
            key="sram",
            label="SRAM buffer macros",
            short="SRAM buffers",
            sa=d["sa_top_macro"],
            cim=d[f"{cim}_top_macro"],
            kind="macro",
            note=(
                f"Characterised SRAM .db area; "
                f"{int(d['sa_top_macro_cells'])} macros (SA) vs "
                f"{int(d[f'{cim}_top_macro_cells'])} (CIM)"
            ),
        ),
        Part(
            key="periph",
            label="MatrixUnit control & datapath logic",
            short="MatrixUnit logic",
            sa=sa_periph_comb + sa_periph_noncomb,
            cim=cim_periph_comb + cim_periph_noncomb,
            comb=(sa_periph_comb, cim_periph_comb),
            noncomb=(sa_periph_noncomb, cim_periph_noncomb),
            note="Parent standard cells outside the compute array",
        ),
        Part(
            key="array_logic",
            label="Compute array — digital logic",
            short="Array logic",
            sa=sa_array_comb + sa_array_noncomb,
            cim=cim_array_comb + cim_array_noncomb,
            comb=(sa_array_comb, cim_array_comb),
            noncomb=(sa_array_noncomb, cim_array_noncomb),
            note=(
                f"SA: {pe_n} composed ProcessingElements. "
                "CIM: CIMArray sequencing, accumulation, and result slots"
            ),
        ),
        Part(
            key="cim_macro",
            label="CIM compute macros",
            short="CIM macros",
            sa=0.0,
            cim=cim_macro_area,
            kind="macro",
            note=f"{macro_count} x {macro_each:,.1f} um^2 physical 64x8x18 macro",
        ),
    )

    # --- cross-check every derived number against the Comparison sheet ---
    sa_cells = sum(p.sa for p in parts if p.kind == "cell")
    cim_cells = sum(p.cim for p in parts if p.kind == "cell")
    _check("SA standard-cell area", sa_cells, float(cmp_rows["Standard cell area"][sa_col]))
    _check("CIM standard-cell area", cim_cells, float(cmp_rows["Standard cell area"][cim_col]))
    _check(
        "SA combinational area",
        sa_periph_comb + sa_array_comb,
        float(cmp_rows["↳ Combinational area"][sa_col]),
    )
    _check(
        "CIM combinational area",
        cim_periph_comb + cim_array_comb,
        float(cmp_rows["↳ Combinational area"][cim_col]),
    )
    _check(
        "SA noncombinational area",
        sa_periph_noncomb + sa_array_noncomb,
        float(cmp_rows["↳ Noncombinational area"][sa_col]),
    )
    _check(
        "CIM noncombinational area",
        cim_periph_noncomb + cim_array_noncomb,
        float(cmp_rows["↳ Noncombinational area"][cim_col]),
    )
    _check("SA total cell area", sum(p.sa for p in parts), float(cmp_rows["Total cell area"][sa_col]))
    _check(
        "CIM total cell area",
        sum(p.cim for p in parts),
        float(cmp_rows["Total cell area"][cim_col]),
    )

    by_key = {p.key: p for p in parts}
    rollups = (
        Part(
            key="compute",
            label="Compute array, all-in (logic + macros)",
            short="Compute array",
            sa=by_key["array_logic"].sa + by_key["cim_macro"].sa,
            cim=by_key["array_logic"].cim + by_key["cim_macro"].cim,
            note="What actually performs the MACs, however it is implemented",
        ),
        Part(
            key="std_cells",
            label="All standard cells",
            short="Standard cells",
            sa=sa_cells,
            cim=cim_cells,
            note="Synthesised logic only, excluding every hard macro",
        ),
        Part(
            key="macros",
            label="All hard macros (SRAM + CIM)",
            short="Hard macros",
            sa=by_key["sram"].sa,
            cim=by_key["sram"].cim + by_key["cim_macro"].cim,
            kind="macro",
            note="Physical macro area from characterised libraries",
        ),
        Part(
            key="no_sram",
            label="MatrixUnit excluding SRAM buffers",
            short="Excl. SRAM",
            sa=sum(p.sa for p in parts) - by_key["sram"].sa,
            cim=sum(p.cim for p in parts) - by_key["sram"].cim,
            note="Isolates the compute path from the buffer memories",
        ),
    )

    meta = {
        "rslots": rslots,
        "technology": "TSMC 7 nm",
        "corner": "SS, 0.675 V, 125 °C",
        "datatype": "signed INT8 × INT8, INT32 accumulate",
        "geometry": "64×64",
        "clock_ns": 2.0,
        "clock_mhz": 500.0,
        "dc_version": "X-2025.06-SP4",
        "sa_sram_macros": int(d["sa_top_macro_cells"]),
        "cim_sram_macros": int(d[f"{cim}_top_macro_cells"]),
        "pe_count": pe_n,
        "pe_area": d["sa_pe_total"],
        "cim_macro_each": macro_each,
        "cim_macro_count": macro_count,
        "cim_macro_geometry": "64×8×18",
        "sa_cell_count": int(d["sa_top_cells"]),
        "workbook": str(Path(workbook).resolve()),
    }

    return AreaModel(rslots=rslots, parts=parts, rollups=rollups, meta=meta)
