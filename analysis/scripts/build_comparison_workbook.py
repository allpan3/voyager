#!/usr/bin/env python3
"""Build the CIM-vs-systolic comparison workbook.

Usage: cmp_build_xlsx.py <summary_csv> <out_xlsx>

Baseline convention: every CIM point is compared against the systolic array of
the SAME K x N geometry. The native CIM comparison point is the one whose macro
cell width matches the systolic datapath width (8-bit cells for INT8); the
narrower 4-bit vanilla macro is reported as an additional point.
"""
import csv, sys
from openpyxl import Workbook
from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
from openpyxl.utils import get_column_letter

SRC, OUT = sys.argv[1], sys.argv[2]
rows = list(csv.DictReader(open(SRC)))

FONT = "Arial"
H = Font(name=FONT, bold=True, color="FFFFFF", size=10)
HFILL = PatternFill("solid", fgColor="2F4F6F")
SUBH = Font(name=FONT, bold=True, size=10)
N = Font(name=FONT, size=10)
TITLE = Font(name=FONT, bold=True, size=13)
WRAP = Alignment(wrap_text=True, vertical="top")
CTR = Alignment(wrap_text=True, horizontal="center", vertical="center")
THIN = Border(bottom=Side(style="thin", color="BFBFBF"))
SAFILL = PatternFill("solid", fgColor="E8F0F8")     # systolic baseline
B8FILL = PatternFill("solid", fgColor="E6F2E6")     # CIM, native 8-bit cells
B4FILL = PatternFill("solid", fgColor="FDF0E4")     # CIM, narrow 4-bit cells

LAYER_SHORT = {
    "mobilebert_encoder_layer_0_ffn_0_output_dense_fused": "ffn_0_output_dense",
    "mobilebert_encoder_layer_0_output_bottleneck_dense_fused": "output_bottleneck_dense",
    "mobilebert_encoder_layer_0_attention_output_dense_fused": "attention_output_dense",
    "matmul_6_fused": "matmul_6 (attn ctx)",
    "matmul_2_fused": "matmul_2 (attn scores)",
}
LAYER_SHAPE = {
    "mobilebert_encoder_layer_0_ffn_0_output_dense_fused": "M=128, K=512, N=128",
    "mobilebert_encoder_layer_0_output_bottleneck_dense_fused": "M=128, K=128, N=512",
    "mobilebert_encoder_layer_0_attention_output_dense_fused": "M=512, K=128, N=128",
    "matmul_6_fused": "M=128, K=128, N=32 (act x act)",
    "matmul_2_fused": "M=128, K=32, N=128 (act x act)",
}
LAYER_ORDER = [
    "mobilebert_encoder_layer_0_ffn_0_output_dense_fused",
    "mobilebert_encoder_layer_0_output_bottleneck_dense_fused",
    "mobilebert_encoder_layer_0_attention_output_dense_fused",
    "matmul_6_fused",
    "matmul_2_fused",
]
GEOM_ORDER = ["32x32", "32x64", "64x32", "64x64"]
# geometry -> (systolic, CIM 8-bit cells, CIM 4-bit cells, extra 4-bit variants)
GROUPS = {
    "32x32": ("sa_32x32", "cimb8_32x32", "cim_32x32", []),
    "32x64": ("sa_32x64", "cimb8_32x64", "cim_32x64", []),
    "64x32": ("sa_64x32", "cimb8_64x32", "cim_64x32", []),
    "64x64": ("sa_64x64", "cimb8_64x64", "cim_64x64", ["cimflat_64x64"]),
}

def fill_for(cfg):
    if cfg.startswith("sa_"):
        return SAFILL
    return B8FILL if cfg.startswith("cimb8") else B4FILL

def get(cfg, layer):
    return next(r for r in rows if r["config"] == cfg and r["layer"] == layer)

wb = Workbook()

# ---------------------------------------------------------------- README ----
ws = wb.active
ws.title = "README"
ws.sheet_view.showGridLines = False
notes = [
    ("Voyager: CIM vs. systolic matrix backend — MobileBERT matrix-unit layers", TITLE),
    ("", N),
    ("Baseline convention", SUBH),
    ("Every CIM point is compared against the SYSTOLIC ARRAY OF THE SAME K x N GEOMETRY. "
     "The 'Same-size comparison' tab is the primary result; each row's systolic column is that "
     "row's own baseline.", N),
    ("The native CIM comparison point is the macro whose CELL WIDTH MATCHES THE SYSTOLIC "
     "DATAPATH WIDTH — 8-bit cells for INT8 (green columns). One cell holds one INT8 weight and "
     "consumes one INT8 activation in a single slice, so it is the like-for-like machine.", N),
    ("The 4-bit vanilla macro (orange columns) is reported as an ADDITIONAL, narrower-cell point: "
     "it composes two cells per INT8 weight and takes two A nibble-slices per MAC, so it carries a "
     "structural penalty the systolic array does not have.", N),
    ("A secondary 'scaling view' at the bottom of that tab keeps the iso-area framing — CIM given "
     "more MACs to spend the weight SRAM it omits — but it is deliberately not the headline.", N),
    ("", N),
    ("Run", SUBH),
    ("Host / worktree: n7, /sim/allpan/voyager/cim (branch cim, HEAD 79b52302)", N),
    ("Simulator: SystemC, cycle-accurate Connections (make sim / TestRunner). Not fast-sim: "
     "CIMArray's signal-level admission is incompatible with MatchLib's fast TLM mode, so both "
     "backends were run in the accurate mode for an apples-to-apples cycle count.", N),
    ("All 65 runs were checked against the gold model first and reported \"Error count: 0\". "
     "No result here comes from a functionally failing run.", N),
    ("Datatype INT8 (activations + weights), bias int24, accumulate/vector bf16.", N),
    ("Technology: generic. Clock 200 MHz (CLOCK_PERIOD=5 ns) — the SystemC clock period, so "
     "runtime in us is real time at 200 MHz. Utilization is clock-independent.", N),
    ("Buffers held constant: input 1024, weight 1024, accum 1024 entries; single-banked accum.", N),
    ("", N),
    ("Metric definitions", SUBH),
    ("runtime_cycles — Harness \"Total Runtime\" for the layer, divided by the 5 ns clock.", N),
    ("ideal_cycles — the model's own peak-rate bound: total MACs / (IC_DIMENSION x OC_DIMENSION), "
     "recomputed per configuration. Utilization is therefore always relative to that "
     "configuration's own peak, never to a fixed reference array.", N),
    ("utilization = ideal_cycles / runtime_cycles.", N),
    ("cycles_per_ideal_beat (II) = runtime_cycles / ideal_cycles — the effective initiation "
     "interval per array operation. 1.0 would be perfect. This is the most diagnostic column: "
     "systolic ~2.0, CIM 8-bit cells 5.0, CIM 4-bit cells 6.0, flat across every geometry.", N),
    ("", N),
    ("External bandwidth assumed", SUBH),
    ("The harness memory model is deliberately idealised: zero latency, no DRAM timing, no "
     "bank conflicts, no contention between ports. Each port delivers one word per cycle, and a "
     "word is the port width. This is the assumption behind every number here.", N),
    ("Matrix-unit ports (INT8): input = IC bytes/cycle; weight = OC bytes/cycle; bias = OC "
     "bytes/cycle; output = OC bytes/cycle. Port widths scale with the array and are NOT held "
     "constant across geometries — see the Configurations tab for the resulting GB/s at 200 MHz.", N),
    ("Measured traffic comes from the harness AccessCounter: \"harness\" counts all bytes read "
     "across the input/weight/bias ports (one shared counter, so read pressure is aggregate only), "
     "\"harness_outputs\" counts bytes written.", N),
    ("At matched geometry the two backends read exactly the same number of bytes — same tiling, "
     "same work — so the runtime gap is compute-issue efficiency, not memory traffic.", N),
    ("", N),
    ("Caveats — read before quoting these numbers", SUBH),
    ("1. Not attempted: a 64x128 (8192-MAC) point. The Interstellar tiler finds no valid mapping "
     "for matmul_6_fused at OC=128 (that layer has only 64 output channels) and aborts the whole "
     "tiling file, so no OC=128 configuration exists for either backend.", N),
    ("2. CIM K (input axis) is capped at 64 by a static assert in CIMProcessor.h, so wider CIM "
     "arrays can only grow along N.", N),
    ("3. Tiling is generated with weight_buffer_size=1024 for both backends, so both run the same "
     "schedule and do the same work. That schedule is tuned for a weight-buffered machine; a "
     "CIM-aware tiling was not explored.", N),
    ("4. Configurations reaching 64 in either dimension need their own compiler corpus: the "
     "compiler disables reshape fusion once an unroll dimension hits 64, so >=64 runs use "
     "test/compiler64 and <64 runs use test/compiler. The 32-unroll corpus was verified "
     "byte-identical to the repo's existing one.", N),
    ("5. resnet18 was not run — it is not set up in this worktree.", N),
    ("6. No LibreOffice was available on either host, so the formulas below ship without cached "
     "values; the workbook is flagged to recalculate on open. summary.csv has the same numbers "
     "already materialised.", N),
]
r = 1
for text, font in notes:
    c = ws.cell(row=r, column=1, value=text)
    c.font = font
    c.alignment = WRAP
    r += 1
ws.column_dimensions["A"].width = 118

# -------------------------------------------------------- Configurations ----
ws = wb.create_sheet("Configurations")
ws.sheet_view.showGridLines = False
ws["A1"] = "Design points — grouped by array geometry, systolic first in each group"
ws["A1"].font = TITLE
hdr = ["Geometry", "Config", "Backend", "Macro cell", "Role", "IC (K)", "OC (N)", "MACs",
       "CIM array organization", "Weight SRAM (KB)", "On-array resident weights (KB)",
       "Input port (B/cyc)", "Weight port (B/cyc)", "Bias port (B/cyc)", "Output port (B/cyc)",
       "Peak read BW @200MHz (GB/s)", "Peak write BW @200MHz (GB/s)"]
for j, h in enumerate(hdr, 1):
    c = ws.cell(row=3, column=j, value=h)
    c.font = H; c.fill = HFILL; c.alignment = CTR
ws.row_dimensions[3].height = 42

r = 4
for g in GEOM_ORDER:
    sa, b8, b4, extra = GROUPS[g]
    for cfg in [sa, b8, b4] + extra:
        d = get(cfg, LAYER_ORDER[0])
        ic, oc = int(d["IC"]), int(d["OC"])
        is_cim = d["backend"] == "cim"
        if not is_cim:
            role = "baseline"
        elif d["cim_cell"] == "8b":
            role = "native CIM (cell width = datapath width)"
        elif cfg.startswith("cimflat"):
            role = "additional: 4b cells, flat organization"
        else:
            role = "additional: narrow 4b cells"
        vals = [g, cfg, "CIM" if is_cim else "systolic", d["cim_cell"] or "—", role, ic, oc,
                f"=F{r}*G{r}", d["cim_org"] or "—",
                0 if is_cim else f"=1024*G{r}/1024",
                f"=2*F{r}*G{r}/1024" if is_cim else 0,
                f"=F{r}", f"=G{r}", f"=G{r}", f"=G{r}",
                f"=(L{r}+M{r}+N{r})*0.2", f"=O{r}*0.2"]
        for j, v in enumerate(vals, 1):
            c = ws.cell(row=r, column=j, value=v)
            c.font = N; c.fill = fill_for(cfg); c.border = THIN
            if j in (10, 11, 16, 17):
                c.number_format = "#,##0.0"
        r += 1
for j, w in enumerate([10, 15, 10, 11, 34, 8, 8, 9, 24, 12, 14, 11, 12, 11, 12, 14, 14], 1):
    ws.column_dimensions[get_column_letter(j)].width = w

nr = r + 1
for txt in [
    "Weight SRAM = WEIGHT_BUFFER_SIZE (1024 entries) x OC bytes. The CIM backend instantiates none: "
    "MatrixUnit.h guards weight_buffer and WeightController out under MATRIX_BACKEND=CIM.",
    "On-array resident weights = CIM_B_SETS (2) x K x N bytes. CIMProcessor ping-pongs exactly two "
    "resident sets, so B_SETS above 2 buys nothing today (the real vanilla macro has 18).",
    "Port widths are IC/OC-derived (ArchitectureParams.h: IC_PORT_WIDTH = IC x 8, OC_PORT_WIDTH = "
    "OC x 8 for INT8), so larger arrays are also given proportionally more external bandwidth.",
    "Macro cell 8b = BASE_A_WIDTH/BASE_B_WIDTH 8, BASE_C_WIDTH 24: one cell per INT8 weight, one A "
    "slice per MAC. 4b = the vanilla macro: two cells composed per INT8 weight, two A nibble-slices "
    "per MAC, which costs one extra cycle of element issue window.",
]:
    c = ws.cell(row=nr, column=1, value=txt); c.font = N; c.alignment = WRAP
    ws.merge_cells(start_row=nr, start_column=1, end_row=nr, end_column=17)
    ws.row_dimensions[nr].height = 26
    nr += 1

# ---------------------------------------------------------------- Results ---
ws = wb.create_sheet("Results")
ws.freeze_panes = "D2"
hdr = ["Geometry", "Config", "Backend", "Macro cell", "Layer", "Shape", "IC (K)", "OC (N)",
       "MACs", "Gold check", "Runtime (cycles)", "Runtime (us @200MHz)", "Ideal (cycles)",
       "Utilization", "II (cycles per ideal beat)", "Ext read (bytes)", "Ext write (bytes)",
       "Ext read (GB/s)", "Ext write (GB/s)", "Peak read (GB/s)", "Peak write (GB/s)",
       "Read % of peak", "Write % of peak"]
for j, h in enumerate(hdr, 1):
    c = ws.cell(row=1, column=j, value=h)
    c.font = H; c.fill = HFILL; c.alignment = CTR
ws.row_dimensions[1].height = 42

idx = {}
r = 2
for layer in LAYER_ORDER:
    for g in GEOM_ORDER:
        sa, b8, b4, extra = GROUPS[g]
        for cfg in [sa, b8, b4] + extra:
            d = get(cfg, layer)
            idx[(cfg, layer)] = r
            vals = [g, cfg, "CIM" if d["backend"] == "cim" else "systolic", d["cim_cell"] or "—",
                    LAYER_SHORT[layer], LAYER_SHAPE[layer], int(d["IC"]), int(d["OC"]),
                    f"=G{r}*H{r}", "PASS" if d["passed"] == "True" else "FAIL",
                    int(d["runtime_cycles"]), f"=K{r}*5/1000", int(d["ideal_cycles"]),
                    f"=M{r}/K{r}", f"=K{r}/M{r}",
                    int(d["ext_read_bytes"]), int(d["ext_write_bytes"]),
                    f"=P{r}/K{r}*0.2", f"=Q{r}/K{r}*0.2",
                    f"=(G{r}+2*H{r})*0.2", f"=H{r}*0.2",
                    f"=R{r}/T{r}", f"=S{r}/U{r}"]
            for j, v in enumerate(vals, 1):
                c = ws.cell(row=r, column=j, value=v)
                c.font = N; c.fill = fill_for(cfg); c.border = THIN
                if j in (11, 13, 16, 17): c.number_format = "#,##0"
                if j == 12: c.number_format = "#,##0.00"
                if j in (14, 22, 23): c.number_format = "0.0%"
                if j == 15: c.number_format = "0.00"
                if j in (18, 19, 20, 21): c.number_format = "#,##0.00"
            r += 1
for j, w in enumerate([10, 15, 10, 11, 24, 30, 8, 8, 9, 10, 13, 13, 12, 11, 13,
                       13, 13, 11, 11, 11, 11, 11, 11], 1):
    ws.column_dimensions[get_column_letter(j)].width = w

# ------------------------------------------------- Same-size comparison -----
ws = wb.create_sheet("Same-size comparison")
ws.sheet_view.showGridLines = False
ws["A1"] = "Same-size comparison — each row's systolic column is that row's baseline"
ws["A1"].font = TITLE
ws["A2"] = ("Green = native CIM (8-bit cells, matching the INT8 datapath). "
            "Orange = additional narrow-cell point (4-bit vanilla macro). "
            "Ratios below 1.00x mean the CIM array is slower than the systolic array of the "
            "same K x N.")
ws["A2"].font = N; ws["A2"].alignment = WRAP
ws.merge_cells("A2:M2"); ws.row_dimensions[2].height = 28

hdr = ["Layer", "Shape", "Array", "MACs",
       "Systolic cycles", "CIM 8b cycles", "CIM 4b cycles",
       "Systolic util", "CIM 8b util", "CIM 4b util",
       "CIM 8b vs systolic", "CIM 4b vs systolic", "Systolic II", "CIM 8b II", "CIM 4b II"]
for j, h in enumerate(hdr, 1):
    c = ws.cell(row=4, column=j, value=h)
    c.font = H; c.fill = HFILL; c.alignment = CTR
ws.row_dimensions[4].height = 40

r = 5
for layer in LAYER_ORDER:
    for g in GEOM_ORDER:
        sa, b8, b4, _ = GROUPS[g]
        rs, r8, r4 = idx[(sa, layer)], idx[(b8, layer)], idx[(b4, layer)]
        ws.cell(row=r, column=1, value=LAYER_SHORT[layer]).font = N
        ws.cell(row=r, column=2, value=LAYER_SHAPE[layer]).font = N
        ws.cell(row=r, column=3, value=g).font = SUBH
        ws.cell(row=r, column=4, value=f"=Results!I{rs}").font = N
        pairs = [
            (5, f"=Results!K{rs}", "#,##0", SAFILL),
            (6, f"=Results!K{r8}", "#,##0", B8FILL),
            (7, f"=Results!K{r4}", "#,##0", B4FILL),
            (8, f"=Results!N{rs}", "0.0%", SAFILL),
            (9, f"=Results!N{r8}", "0.0%", B8FILL),
            (10, f"=Results!N{r4}", "0.0%", B4FILL),
            (11, f"=E{r}/F{r}", "0.00x", B8FILL),
            (12, f"=E{r}/G{r}", "0.00x", B4FILL),
            (13, f"=Results!O{rs}", "0.00", SAFILL),
            (14, f"=Results!O{r8}", "0.00", B8FILL),
            (15, f"=Results!O{r4}", "0.00", B4FILL),
        ]
        for col, formula, fmt, fill in pairs:
            c = ws.cell(row=r, column=col, value=formula)
            c.font = N; c.number_format = fmt; c.fill = fill; c.border = THIN
        for col in (1, 2, 3, 4):
            ws.cell(row=r, column=col).border = THIN
        r += 1

# ---- secondary: iso-area scaling view
r += 1
ws.cell(row=r, column=1, value="Secondary view — CIM given more MACs to spend the weight SRAM it omits").font = SUBH
r += 1
ws.cell(row=r, column=1, value=(
    "The systolic 32x32 baseline carries a 32 KB weight SRAM that the CIM backend does not "
    "instantiate at all. This block spends that area on MACs instead: a 64x64 CIM array (4x the "
    "MACs) against the 32x32 systolic array. It is the most favourable honest framing for CIM, "
    "and is kept separate from the same-size result above.")).font = N
ws.cell(row=r, column=1).alignment = WRAP
ws.merge_cells(start_row=r, start_column=1, end_row=r, end_column=13)
ws.row_dimensions[r].height = 30
r += 1
hdr2 = ["Layer", "Shape", "MAC ratio", "Systolic 32x32 cycles", "CIM 8b 64x64 cycles",
        "CIM 4b 64x64 cycles", "CIM 8b vs systolic 32x32", "CIM 4b vs systolic 32x32"]
for j, h in enumerate(hdr2, 1):
    c = ws.cell(row=r, column=j, value=h)
    c.font = H; c.fill = HFILL; c.alignment = CTR
ws.row_dimensions[r].height = 40
r += 1
for layer in LAYER_ORDER:
    rs, r8, r4 = idx[("sa_32x32", layer)], idx[("cimb8_64x64", layer)], idx[("cim_64x64", layer)]
    ws.cell(row=r, column=1, value=LAYER_SHORT[layer]).font = N
    ws.cell(row=r, column=2, value=LAYER_SHAPE[layer]).font = N
    for col, formula, fmt, fill in [
        (3, f"=Results!I{r8}/Results!I{rs}", "0.0x", B8FILL),
        (4, f"=Results!K{rs}", "#,##0", SAFILL),
        (5, f"=Results!K{r8}", "#,##0", B8FILL),
        (6, f"=Results!K{r4}", "#,##0", B4FILL),
        (7, f"=D{r}/E{r}", "0.00x", B8FILL),
        (8, f"=D{r}/F{r}", "0.00x", B4FILL),
    ]:
        c = ws.cell(row=r, column=col, value=formula)
        c.font = N; c.number_format = fmt; c.fill = fill; c.border = THIN
    r += 1

# ---- organization control
r += 1
ws.cell(row=r, column=1, value="Control — array organization at fixed 64x64, 4-bit cells").font = SUBH
r += 1
hdr3 = ["Layer", "Shape", "4 tiles x 4 elem (cycles)", "16 tiles x 1 elem (cycles)", "Difference"]
for j, h in enumerate(hdr3, 1):
    c = ws.cell(row=r, column=j, value=h)
    c.font = H; c.fill = HFILL; c.alignment = CTR
ws.row_dimensions[r].height = 30
r += 1
for layer in LAYER_ORDER:
    ra, rb = idx[("cim_64x64", layer)], idx[("cimflat_64x64", layer)]
    ws.cell(row=r, column=1, value=LAYER_SHORT[layer]).font = N
    ws.cell(row=r, column=2, value=LAYER_SHAPE[layer]).font = N
    for col, formula, fmt in [(3, f"=Results!K{ra}", "#,##0"),
                              (4, f"=Results!K{rb}", "#,##0"),
                              (5, f"=D{r}-C{r}", "#,##0")]:
        c = ws.cell(row=r, column=col, value=formula)
        c.font = N; c.number_format = fmt; c.fill = B4FILL; c.border = THIN
    r += 1

r += 1
for txt in [
    "Reading the II columns is the fastest way to see the result: the systolic array settles at "
    "~2.0 cycles per array operation, the native 8-bit CIM at exactly 5.0, the 4-bit CIM at "
    "exactly 6.0 — identical across every geometry and every layer. Ideal is 1.0.",
    "The 4b-to-8b gap of exactly 1.0 cycle is the extra A nibble-slice: CIMElement::issue_window() "
    "is ceil(A_WIDTH / BASE_A_WIDTH) in bit-parallel mode, so 4-bit cells need two slices per INT8 "
    "MAC. CIMTile::issue_window() adds the latch stage, making the array's admission floor 3 "
    "cycles (4b) or 2 (8b) — measured II sits exactly 3 cycles above that floor in both cases, so "
    "~3 cycles per operation are CIMProcessor controller overhead independent of the macro.",
    "Because utilization is measured against each configuration's OWN peak, a flat CIM utilization "
    "across geometries means CIM throughput scaled linearly with array size while its efficiency "
    "never moved. Growing the array does not close the gap; shortening the initiation interval "
    "would.",
]:
    c = ws.cell(row=r, column=1, value=txt); c.font = N; c.alignment = WRAP
    ws.merge_cells(start_row=r, start_column=1, end_row=r, end_column=15)
    ws.row_dimensions[r].height = 44
    r += 1

ws.column_dimensions["A"].width = 24
ws.column_dimensions["B"].width = 30
for j in range(3, 16):
    ws.column_dimensions[get_column_letter(j)].width = 14

# No LibreOffice on either host, so formulas ship without cached values;
# force the reader to compute them on open.
wb.calculation.fullCalcOnLoad = True
wb.save(OUT)
print("saved", OUT)
