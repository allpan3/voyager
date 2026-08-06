"""Table renderings of the area model: LaTeX (booktabs), Markdown, and CSV.

All three come off the same row list, so the paper table, the README table, and
the spreadsheet export can never disagree.
"""

from __future__ import annotations

import csv
import io
from dataclasses import dataclass

from .data import AreaModel


@dataclass(frozen=True)
class Row:
    label: str
    sa: float | None
    cim: float | None
    kind: str = "component"  # component | sub | total | rollup | section
    note: str = ""

    @property
    def ratio(self) -> float | None:
        if self.sa and self.cim is not None:
            return self.cim / self.sa
        return None


def build_rows(model: AreaModel, *, sub_rows: bool = True) -> list[Row]:
    rows: list[Row] = [Row("Breakdown", None, None, kind="section")]
    for part in model.parts:
        rows.append(Row(part.label, part.sa, part.cim, note=part.note))
        if sub_rows and part.comb and part.noncomb:
            rows.append(Row("combinational", part.comb[0], part.comb[1], kind="sub"))
            rows.append(Row("noncombinational", part.noncomb[0], part.noncomb[1], kind="sub"))
    rows.append(Row("Total cell area", model.sa_total, model.cim_total, kind="total"))
    rows.append(Row("Roll-ups", None, None, kind="section"))
    for part in model.rollups:
        rows.append(Row(part.label, part.sa, part.cim, kind="rollup", note=part.note))
    return rows


#: A zero component area means the design has no such block, not a measured zero.
ABSENT = "—"


def _num(v: float | None, scale: float = 1.0, digits: int = 0) -> str:
    return ABSENT if not v else f"{v / scale:,.{digits}f}"


def _pct(v: float | None, total: float) -> str:
    return ABSENT if not v else f"{v / total * 100:.1f}"


def _factor(row: Row) -> str:
    """SA area divided by CIM area: above 1 CIM is smaller, below 1 it is larger.

    Always the same quantity, including the hard-macro roll-up where CIM comes
    out bigger — flipping that one row to CIM/SA would contradict the column.
    """
    r = row.ratio
    if r is None:
        return "CIM only" if row.sa == 0 else ABSENT
    return f"{1 / r:.2f}×"


def caption(model: AreaModel) -> str:
    m = model.meta
    return (
        f"MatrixUnit {m['geometry']} area, systolic array versus CIM. "
        f"{m['technology']}, {m['corner']}, "
        f"{m['datatype']}, {m['clock_ns']:.0f} ns target (Synopsys DC {m['dc_version']}). "
        f"Areas are hierarchy-corrected full-design cell areas in µm²; DC reports net "
        f"interconnect area as undefined, so no routing area is included. "
        f"CIM is {model.reduction:.2f}× smaller overall "
        f"({model.savings:,.0f} µm², {(1 - model.ratio) * 100:.1f} % less)."
    )


# --------------------------------------------------------------------------- #
# renderers
# --------------------------------------------------------------------------- #


def to_markdown(model: AreaModel, *, sub_rows: bool = True) -> str:
    rows = build_rows(model, sub_rows=sub_rows)
    out = [
        f"### MatrixUnit {model.meta['geometry']} area — systolic array vs CIM",
        "",
        "| Component | SA (µm²) | SA % | CIM (µm²) | CIM % | SA/CIM |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for r in rows:
        if r.kind == "section":
            out.append(f"| **{r.label}** | | | | | |")
            continue
        label = f"&nbsp;&nbsp;↳ *{r.label}*" if r.kind == "sub" else r.label
        if r.kind == "total":
            label = f"**{label}**"
        out.append(
            f"| {label} | {_num(r.sa)} | {_pct(r.sa, model.sa_total)} "
            f"| {_num(r.cim)} | {_pct(r.cim, model.cim_total)} | {_factor(r)} |"
        )
    out += ["", caption(model)]
    return "\n".join(out) + "\n"


def to_latex(model: AreaModel, *, sub_rows: bool = True, label: str = "tab:area") -> str:
    rows = build_rows(model, sub_rows=sub_rows)
    body: list[str] = []
    for r in rows:
        if r.kind == "section":
            body.append(r"\addlinespace")
            body.append(rf"\multicolumn{{6}}{{@{{}}l}}{{\textit{{{r.label}}}}} \\")
            continue
        if r.kind == "total":
            body.append(r"\midrule")
        name = r.label.replace("&", r"\&").replace("—", "---")
        if r.kind == "sub":
            name = rf"\quad\textit{{{name}}}"
        elif r.kind == "total":
            name = rf"\textbf{{{name}}}"
        cells = [
            name,
            _num(r.sa),
            _pct(r.sa, model.sa_total),
            _num(r.cim),
            _pct(r.cim, model.cim_total),
            _factor(r),
        ]
        cells = [c.replace("×", r"$\times$").replace(ABSENT, "---") for c in cells]
        if r.kind == "total":
            cells = [cells[0]] + [rf"\textbf{{{c}}}" for c in cells[1:]]
        body.append(" & ".join(cells) + r" \\")

    head = "\n".join(
        [
            r"\begin{table}[t]",
            r"\centering",
            r"\caption{"
            + caption(model)
            .replace("×", r"$\times$")
            .replace("µm²", r"$\mu$m$^2$")
            .replace("%", r"\%")
            .replace("°", r"$^\circ$"),
            r"}",
            rf"\label{{{label}}}",
            r"\footnotesize",
            r"\setlength{\tabcolsep}{4pt}",
            r"\begin{tabular}{@{}lrrrrr@{}}",
            r"\toprule",
            r" & \multicolumn{2}{c}{Systolic array} & \multicolumn{2}{c}{CIM} & \\",
            r"\cmidrule(lr){2-3}\cmidrule(lr){4-5}",
            r"Component & $\mu$m$^2$ & \% & $\mu$m$^2$ & \% & SA/CIM \\",
            r"\midrule",
        ]
    )
    tail = "\n".join([r"\bottomrule", r"\end{tabular}", r"\end{table}"])
    return "\n".join([head, *body, tail]) + "\n"


def to_csv(model: AreaModel, *, sub_rows: bool = True) -> str:
    buf = io.StringIO()
    w = csv.writer(buf)
    w.writerow(
        [
            "section",
            "component",
            "sa_um2",
            "sa_pct",
            "cim_um2",
            "cim_pct",
            "sa_over_cim",
            "delta_um2",
            "note",
        ]
    )
    section = ""
    for r in build_rows(model, sub_rows=sub_rows):
        if r.kind == "section":
            section = r.label
            continue
        ratio = r.ratio
        w.writerow(
            [
                section,
                ("  " if r.kind == "sub" else "") + r.label,
                f"{r.sa:.6f}" if r.sa is not None else "",
                f"{r.sa / model.sa_total * 100:.4f}" if r.sa is not None else "",
                f"{r.cim:.6f}" if r.cim is not None else "",
                f"{r.cim / model.cim_total * 100:.4f}" if r.cim is not None else "",
                f"{1 / ratio:.4f}" if ratio else "",
                f"{(r.cim - r.sa):.6f}" if r.sa is not None and r.cim is not None else "",
                r.note,
            ]
        )
    return buf.getvalue()


def to_text(model: AreaModel) -> str:
    """Fixed-width console summary, handy when checking a regenerated run."""
    rows = [r for r in build_rows(model, sub_rows=True) if r.kind != "section"]
    width = max(len(("  " if r.kind == "sub" else "") + r.label) for r in rows)
    lines = [
        f"MatrixUnit {model.meta['geometry']} area  |  SA vs CIM  |  "
        f"{model.meta['technology']}, {model.meta['clock_ns']:.0f} ns",
        "",
        f"{'Component'.ljust(width)}  {'SA (um2)':>12}  {'CIM (um2)':>12}  {'SA/CIM':>9}",
        "-" * (width + 39),
    ]
    for r in rows:
        name = ("  " if r.kind == "sub" else "") + r.label
        lines.append(f"{name.ljust(width)}  {_num(r.sa):>12}  {_num(r.cim):>12}  {_factor(r):>9}")
    lines += ["", caption(model)]
    return "\n".join(lines) + "\n"
