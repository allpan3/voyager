#!/usr/bin/env python3
"""Build the CIM-vs-systolic comparison workbook from a parsed summary CSV.

Usage: build_comparison_workbook.py <summary_csv> <out_xlsx>

Fully data-driven: configurations, geometries and layers are read from the CSV
(produced by parse_sweep_logs.py), so any sweep design space renders without
editing this script. Within each geometry the systolic baseline sorts first and
every CIM variant is compared against it at matched K x N.
"""
import csv, sys
from collections import OrderedDict
from openpyxl import Workbook
from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
from openpyxl.utils import get_column_letter

SRC, OUT = sys.argv[1], sys.argv[2]
rows = list(csv.DictReader(open(SRC)))

FONT = "Arial"
H = Font(name=FONT, bold=True, color="FFFFFF", size=10)
HFILL = PatternFill("solid", fgColor="2F4F6F")
SUB = Font(name=FONT, bold=True, size=10)
N = Font(name=FONT, size=10)
TITLE = Font(name=FONT, bold=True, size=13)
WRAP = Alignment(wrap_text=True, vertical="top")
CTR = Alignment(wrap_text=True, horizontal="center", vertical="center")
THIN = Border(bottom=Side(style="thin", color="BFBFBF"))
SAFILL = PatternFill("solid", fgColor="E8F0F8")   # systolic
B8FILL = PatternFill("solid", fgColor="E6F2E6")   # CIM 8-bit cells
B4FILL = PatternFill("solid", fgColor="FDF0E4")   # CIM 4-bit cells

LAYER_SHORT = {
    "mobilebert_encoder_layer_0_ffn_0_output_dense_fused": "ffn_0_output_dense",
    "mobilebert_encoder_layer_0_output_bottleneck_dense_fused": "output_bottleneck_dense",
    "mobilebert_encoder_layer_0_attention_output_dense_fused": "attention_output_dense",
    "matmul_6_fused": "matmul_6 (attn ctx)",
    "matmul_2_fused": "matmul_2 (attn scores)",
}


def fill_for(r):
    if r["backend"] == "systolic":
        return SAFILL
    return B8FILL if r["cim_cell"] == "8b" else B4FILL


def fnum(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


sims = sorted({r["sim"] for r in rows})
# RTL cosimulation is the only performance source of truth
primary_sim = "rtl"

layers = list(OrderedDict((r["layer"], None) for r in rows).keys())
layers.sort(key=lambda L: -max((fnum(r["macs"]) or 0) for r in rows if r["layer"] == L))


def variant(r):
    """Identity of one design point. The same array behind a different external
    port width is a distinct point, so the port width is part of the key."""
    return (r["config"], r["ic_port_width_bits"], r["oc_port_width_bits"])


def port_label(r):
    ic, oc = r["ic_port_width_bits"], r["oc_port_width_bits"]
    w = ic if ic == oc else f"{ic}/{oc}"
    return f"{w}b matched" if r["bw_mode"] == "matched" else f"{w}b"


# A comparison is only meaningful between designs of the same size AND the same
# external interface, so both dimensions key the grouping.
groups = OrderedDict()
for r in rows:
    groups.setdefault((r["geometry"], port_label(r)), {})[variant(r)] = r
for k, cfgs in groups.items():
    def key(v, cfgs=cfgs):
        d = cfgs[v]
        return (d["backend"] != "systolic", d["cim_cell"],
                int(d["cim_ch_out"] or 0), int(d["cim_ch_in"] or 0))
    groups[k] = [cfgs[v] for v in sorted(cfgs, key=key)]
group_order = sorted(groups, key=lambda k: (int(k[0].split("x")[0]) * int(k[0].split("x")[1]),
                                            k[1]))

cell = {(variant(r), r["layer"], r["sim"]): r for r in rows}


def metric(v, layer, field, sim=None):
    r = cell.get((v, layer, sim or primary_sim))
    return r[field] if r else ""


wb = Workbook()

# ------------------------------------------------------------------ README ---
ws = wb.active
ws.title = "README"
ws.sheet_view.showGridLines = False
n_cfg = len({r["config"] for r in rows})
notes = [
    ("CIM vs. systolic matrix backend — MobileBERT matrix-unit layers", TITLE),
    ("", N),
    (f"Generated from {SRC.split('/')[-1]}: {len(rows)} runs, {n_cfg} configs, "
     f"{len(layers)} layers, sim(s) = {', '.join(sims)}.", N),
    ("", N),
    ("Pipeline", SUB),
    ("matrix_backend_sweep.sh <dir> -> per-run logs + manifest.csv; "
     "parse_sweep_logs.py <dir> summary.csv -> this CSV; "
     "build_comparison_workbook.py summary.csv out.xlsx -> this workbook. All "
     "three live in analysis/scripts/ and are fully manifest-driven.", N),
    ("", N),
    ("Reading the columns", SUB),
    ("runtime_cycles = Total Runtime / 5 ns clock (200 MHz). ideal_cycles = "
     "total MACs / (K x N), recomputed per config. utilization = ideal/runtime. "
     "II (cycles per ideal beat) = runtime/ideal, the effective initiation "
     "interval; 1.0 is perfect.", N),
    ("Every CIM point is compared against the systolic array of the SAME K x N. "
     "Config names: cim<A>b<B>b_<KxN>_co<CH_OUT>_ci<CH_IN>, encoding the macro "
     "cell widths and the CH_IN/CH_OUT construction. 8b cells match the INT8 "
     "datapath (native); 4b is the vanilla sub-word macro.", N),
    ("External bandwidth: idealised L2 model, one word/cycle/port (input, weight, "
     "bias, output). Each geometry is run at its MATCHED width — one full array "
     "row/column per cycle, so 64 B/cyc for a 64x64 INT8 array — and at a fixed "
     "width held constant as the array grows. Comparison blocks never mix the "
     "two: a design point is (geometry, port width), and the systolic row inside "
     "each block is that block's baseline.", N),
    ("", N),
    ("Primary comparison sim: rtl. The Comparison tab uses RTL cosim as the "
     "cycle-accurate source of truth. SystemC rows remain in Results for "
     "functional pass/fail and traffic counts only; their performance cells "
     "are blank. Any config missing an RTL row shows blank in Comparison.", N),
    ("CIM Stalls reports the three backend-specific counters as raw cycles and "
     "percent of core cycles. set_wait is time awaiting a completely filled "
     "resident set; completion_queue is a pending MAC blocked by reserved or "
     "occupied completion storage; result_path is the union of "
     "accumulation-channel stalls and a full final-output FIFO, so these "
     "categories can overlap other counters.", N),
]
r = 1
for text, font in notes:
    c = ws.cell(row=r, column=1, value=text); c.font = font; c.alignment = WRAP
    r += 1
ws.column_dimensions["A"].width = 118

# ---------------------------------------------------------- Configurations ---
ws = wb.create_sheet("Configurations")
ws.sheet_view.showGridLines = False
ws["A1"] = "Design points"; ws["A1"].font = TITLE
hdr = ["Config", "Backend", "Geometry (KxN)", "MACs", "Macro cell",
       "CH_IN", "CH_OUT", "Input-axis tiles", "Output-axis tiles", "Port width"]
for j, h in enumerate(hdr, 1):
    c = ws.cell(row=3, column=j, value=h); c.font = H; c.fill = HFILL; c.alignment = CTR
r = 4
seen = set()
for k in group_order:
    for d in groups[k]:
        if variant(d) in seen:
            continue
        seen.add(variant(d))
        vals = [d["config"], d["backend"], d["geometry"], int(float(d["macs"])),
                d["cim_cell"] or "-", d["cim_ch_in"] or "-", d["cim_ch_out"] or "-",
                d["input_axis_tiles"] or "-", d["output_axis_tiles"] or "-",
                port_label(d)]
        for j, v in enumerate(vals, 1):
            c = ws.cell(row=r, column=j, value=v); c.font = N; c.fill = fill_for(d)
            c.border = THIN
        r += 1
for j, w in enumerate([26, 10, 13, 9, 10, 8, 9, 15, 16, 14], 1):
    ws.column_dimensions[get_column_letter(j)].width = w

# ----------------------------------------------------- Comparison (by geom) --
ws = wb.create_sheet("Comparison")
ws.sheet_view.showGridLines = False
ws["A1"] = (f"Utilization and II by geometry and port width — {primary_sim}; "
            "each block's systolic row is its baseline")
ws["A1"].font = TITLE
r = 3
for k in group_order:
    g, plabel = k
    sa = next((d for d in groups[k] if d["backend"] == "systolic"), None)
    ws.cell(row=r, column=1, value=f"Geometry {g} — external port {plabel}").font = SUB
    r += 1
    hdr = (["Config", "cell", "CH_IN", "CH_OUT"]
           + [f"{LAYER_SHORT.get(L, L)}\nutil" for L in layers]
           + ["mean util", "mean II", "vs systolic\n(mean runtime)"])
    for j, h in enumerate(hdr, 1):
        c = ws.cell(row=r, column=j, value=h); c.font = H; c.fill = HFILL; c.alignment = CTR
    ws.row_dimensions[r].height = 42
    r += 1
    for d in groups[k]:
        cfg, v = d["config"], variant(d)
        utils = [fnum(metric(v, L, "utilization")) for L in layers]
        iis = [fnum(metric(v, L, "cycles_per_ideal_beat")) for L in layers]
        runs = [fnum(metric(v, L, "runtime_cycles")) for L in layers]
        sa_runs = [fnum(metric(variant(sa), L, "runtime_cycles")) for L in layers] if sa else []
        uvals = [u for u in utils if u is not None]
        ivals = [i for i in iis if i is not None]
        umean = sum(uvals) / len(uvals) if uvals else None
        imean = sum(ivals) / len(ivals) if ivals else None
        ratios = [sr / rr for sr, rr in zip(sa_runs, runs) if sr and rr]
        rmean = sum(ratios) / len(ratios) if ratios else None
        vals = [cfg, d["cim_cell"] or "-", d["cim_ch_in"] or "-", d["cim_ch_out"] or "-"]
        for j, v in enumerate(vals, 1):
            c = ws.cell(row=r, column=j, value=v); c.font = N; c.fill = fill_for(d); c.border = THIN
        for k, u in enumerate(utils):
            c = ws.cell(row=r, column=5 + k, value=u if u is not None else "")
            c.font = N; c.number_format = "0.0%"; c.fill = fill_for(d); c.border = THIN
        col = 5 + len(layers)
        for v, fmt in [(round(umean, 4) if umean is not None else "", "0.0%"),
                       (round(imean, 3) if imean is not None else "", "0.00"),
                       (round(rmean, 2) if rmean else "", "0.00x")]:
            c = ws.cell(row=r, column=col, value=v); c.font = N; c.number_format = fmt
            c.fill = fill_for(d); c.border = THIN; col += 1
        r += 1
    r += 1
ws.column_dimensions["A"].width = 26
for j in range(2, 5):
    ws.column_dimensions[get_column_letter(j)].width = 8
for j in range(5, 5 + len(layers) + 3):
    ws.column_dimensions[get_column_letter(j)].width = 12

# ------------------------------------------------------------ CIM Stalls -----
ws = wb.create_sheet("CIM Stalls")
ws.freeze_panes = "A3"
ws["A1"] = "CIM RTL stall attribution"
ws["A1"].font = TITLE
stall_fields = [
    ("config", "Config"),
    ("geometry", "Geometry"),
    ("layer", "Layer"),
    ("runtime_cycles", "Runtime"),
    ("core_cycles", "Core cycles"),
    ("cim_set_wait_cycles", "Set wait"),
    ("cim_set_wait_pct_of_core", "Set wait % core"),
    ("cim_completion_queue_stall_cycles", "Completion queue"),
    ("cim_completion_queue_stall_pct_of_core", "Completion queue % core"),
    ("cim_result_path_stall_cycles", "Result path"),
    ("cim_result_path_stall_pct_of_core", "Result path % core"),
    ("result_backpressure_cycles", "Raw-result backpressure"),
    ("output_fifo_full_cycles", "Output FIFO full"),
    ("output_backpressure_cycles", "Downstream output stall"),
]
for j, (_, heading) in enumerate(stall_fields, 1):
    c = ws.cell(row=2, column=j, value=heading)
    c.font = H
    c.fill = HFILL
    c.alignment = CTR
ws.row_dimensions[2].height = 34
cim_stalls = [
    d for d in rows if d["backend"] == "cim" and d["sim"] == primary_sim
]
cim_stalls.sort(key=lambda d: (d["geometry"], d["config"], d["layer"]))
for i, d in enumerate(cim_stalls, 3):
    for j, (field, _) in enumerate(stall_fields, 1):
        value = d.get(field, "")
        if field.endswith("_pct_of_core") and value not in ("", None):
            value = float(value)
        elif field.endswith("_cycles") and value not in ("", None):
            value = int(float(value))
        c = ws.cell(row=i, column=j, value=value)
        c.font = N
        c.fill = fill_for(d)
        c.border = THIN
        if field.endswith("_pct_of_core") and isinstance(value, float):
            c.number_format = '0.0"%"'
        elif field.endswith("_cycles") and isinstance(value, int):
            c.number_format = "#,##0"
for j, (field, heading) in enumerate(stall_fields, 1):
    ws.column_dimensions[get_column_letter(j)].width = max(
        11, min(32, max(len(field), len(heading)) + 2)
    )

# ---------------------------------------------------------------- Results ----
ws = wb.create_sheet("Results")
ws.freeze_panes = "B2"
fields = ["config", "sim", "bw_mode", "ic_port_width_bits", "oc_port_width_bits",
          "backend", "geometry",
          "macs", "cim_cell", "cim_ch_in", "cim_ch_out", "input_axis_tiles",
          "output_axis_tiles", "layer", "passed", "runtime_cycles", "runtime_us",
          "ideal_cycles", "utilization", "cycles_per_ideal_beat",
          "ext_read_GBps", "ext_write_GBps", "read_bw_pct_of_peak",
          "core_cycles", "array_resident_cycles", "array_issue_cycles",
          "input_unavailable_cycles", "input_backpressure_cycles",
          "weight_unavailable_cycles", "weight_backpressure_cycles",
          "result_backpressure_cycles", "accumulation_stall_cycles",
          "output_backpressure_cycles", "output_fifo_full_cycles",
          "cim_set_wait_cycles", "cim_set_wait_pct_of_core",
          "cim_completion_queue_stall_cycles",
          "cim_completion_queue_stall_pct_of_core",
          "cim_result_path_stall_cycles",
          "cim_result_path_stall_pct_of_core"]
for j, f in enumerate(fields, 1):
    c = ws.cell(row=1, column=j, value=f); c.font = H; c.fill = HFILL; c.alignment = CTR
ws.row_dimensions[1].height = 30
pct = {"utilization"}
percent_points = {
    "read_bw_pct_of_peak", "cim_set_wait_pct_of_core",
    "cim_completion_queue_stall_pct_of_core",
    "cim_result_path_stall_pct_of_core",
}
ints = {
    "runtime_cycles", "ideal_cycles", "macs", "core_cycles",
    "array_resident_cycles", "array_issue_cycles", "input_unavailable_cycles",
    "input_backpressure_cycles", "weight_unavailable_cycles",
    "weight_backpressure_cycles", "result_backpressure_cycles",
    "accumulation_stall_cycles", "output_backpressure_cycles",
    "output_fifo_full_cycles", "cim_set_wait_cycles",
    "cim_completion_queue_stall_cycles", "cim_result_path_stall_cycles",
}
srt = sorted(rows, key=lambda r: (r["geometry"], r["backend"] != "systolic",
             r["cim_cell"], r["config"], r["sim"], r["layer"]))
for i, d in enumerate(srt, 2):
    for j, f in enumerate(fields, 1):
        v = d.get(f, "")
        if f in pct and v not in ("", None):
            v = float(v)
        elif f in percent_points and v not in ("", None):
            v = float(v)
        elif f in ints and v not in ("", None):
            v = int(float(v))
        c = ws.cell(row=i, column=j, value=v); c.font = N; c.fill = fill_for(d); c.border = THIN
        if f in pct and isinstance(v, float):
            c.number_format = "0.0%"
        if f in percent_points and isinstance(v, float):
            c.number_format = '0.0"%"'
        if f in ints and isinstance(v, int):
            c.number_format = "#,##0"
for j, f in enumerate(fields, 1):
    ws.column_dimensions[get_column_letter(j)].width = max(9, min(24, len(f) + 2))

wb.calculation.fullCalcOnLoad = True
wb.save(OUT)
print(f"saved {OUT}: {len(wb.sheetnames)} sheets, {len(rows)} result rows, "
      f"primary_sim={primary_sim}, comparison blocks="
      + ", ".join(f"{g} @ {p}" for g, p in group_order))
