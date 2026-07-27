#!/usr/bin/env python3
"""Build one RTL-only CIM-vs-systolic workbook from a parsed sweep CSV.

Usage: build_comparison_workbook.py <summary_csv> <out_xlsx>

The workbook is intentionally per-layer. A hardware point is performance-
reportable only when every expected layer has a passing RTL row with the
required hardware counters. SystemC rows help identify attempted points but
never contribute cycles, utilization, comparisons, or stall measurements.
"""

import csv
import sys
from collections import OrderedDict

from openpyxl import Workbook
from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
from openpyxl.utils import get_column_letter
from openpyxl.worksheet.table import Table, TableStyleInfo

SRC, OUT = sys.argv[1], sys.argv[2]
rows = list(csv.DictReader(open(SRC)))
if not rows:
    sys.exit("summary CSV is empty")

FONT = "Arial"
NAVY = "17324D"
TEAL = "0F6B78"
TEAL_LIGHT = "D9EEF0"
SA_BLUE = "DCEAF7"
CIM_GREEN = "E2F0D9"
PASS_GREEN = "C6E0B4"
FAIL_RED = "F4CCCC"
GRAY = "667085"
WHITE = "FFFFFF"
GRID = "D0D5DD"

TITLE = Font(name=FONT, bold=True, size=15, color=WHITE)
SECTION = Font(name=FONT, bold=True, size=11, color=WHITE)
HEADER = Font(name=FONT, bold=True, size=10, color=WHITE)
BODY = Font(name=FONT, size=10)
BODY_BOLD = Font(name=FONT, bold=True, size=10)
WRAP = Alignment(wrap_text=True, vertical="top")
CENTER = Alignment(wrap_text=True, horizontal="center", vertical="center")
RIGHT = Alignment(horizontal="right", vertical="center")
THIN_BOTTOM = Border(bottom=Side(style="thin", color=GRID))

LAYER_ORDER = [
    "mobilebert_encoder_layer_0_ffn_0_output_dense_fused",
    "mobilebert_encoder_layer_0_output_bottleneck_dense_fused",
    "mobilebert_encoder_layer_0_attention_output_dense_fused",
    "matmul_6_fused",
    "matmul_2_fused",
]
LAYER_SHORT = {
    "mobilebert_encoder_layer_0_ffn_0_output_dense_fused": "ffn_0_output_dense",
    "mobilebert_encoder_layer_0_output_bottleneck_dense_fused": "output_bottleneck_dense",
    "mobilebert_encoder_layer_0_attention_output_dense_fused": "attention_output_dense",
    "matmul_6_fused": "matmul_6 (attention context)",
    "matmul_2_fused": "matmul_2 (attention scores)",
}
REQUIRED_RTL_FIELDS = [
    "runtime_cycles",
    "matrix_unit_cycles",
    "ideal_cycles",
    "processor_active_cycles",
]
STALL_FIELDS = [
    ("input_unavailable_cycles", "Input unavailable"),
    ("input_backpressure_cycles", "Input backpressure"),
    ("weight_unavailable_cycles", "Weight unavailable"),
    ("weight_backpressure_cycles", "Weight backpressure"),
    ("result_backpressure_cycles", "Result backpressure"),
    ("accumulation_stall_cycles", "Accumulation stall"),
    ("output_fifo_full_cycles", "Output FIFO full"),
    ("output_backpressure_cycles", "Output backpressure"),
    ("cim_set_wait_cycles", "CIM set wait"),
    ("cim_completion_queue_stall_cycles", "CIM completion queue"),
]
STALL_DEFINITIONS = {
    "input_unavailable_cycles": (
        "Counted when the MatrixProcessor or CIMProcessor is ready to consume the next input/A beat, but "
        "InputController's window-buffer output has valid low.\n\n"
        "The beat may still be waiting for a memory response, input-buffer read, packing, transposition, or "
        "window formation. This counter identifies upstream input starvation but not which upstream stage caused it."
    ),
    "input_backpressure_cycles": (
        "SA: a valid input beat is waiting at the input skewer because the array-side consumer is not ready.\n\n"
        "CIM: a fully formed MAC request is valid, but CIMArray has ready low. The current CIM counter therefore "
        "matches CIM completion-queue stall cycles."
    ),
    "weight_unavailable_cycles": (
        "Counted when the processor's weight loader is ready for the next weight beat, but the upstream weight "
        "channel has valid low.\n\n"
        "SA may be waiting for WeightController, weight-buffer fill/read, memory response, packing, or transposition. "
        "CIM streams directly from WeightController and may be waiting for memory response, packing, transposition, "
        "or production of the next resident-set row beat."
    ),
    "weight_backpressure_cycles": (
        "SA: push_weights has already popped a valid weight beat and is presenting it to the serialized weight "
        "skewer, but one or more per-column skewer FIFOs cannot accept the vector while earlier weights drain into "
        "the array.\n\n"
        "CIM: load_weights has already popped a beat and is presenting a resident-set write request, but CIMArray "
        "has ready low. The current CIMArray write port is always ready outside reset, so this counter is expected "
        "to remain zero. Waiting for set_consumed happens before a request is presented and is not counted here."
    ),
    "result_backpressure_cycles": (
        "A raw array result is valid but the processor-side result consumer is not ready.\n\n"
        "Raw means the array's partial-sum output before bias addition, cross-tile accumulation, and final write-back. "
        "SA observes the psum output skewer. CIM observes CIMArray's CBeat result channel; it blocks when "
        "collect_results cannot forward the beat into the accumulation pipeline."
    ),
    "accumulation_stall_cycles": (
        "Union of accumulation-path blocking conditions for both SA and CIM: an accumulation-buffer read address is "
        "valid but not accepted; the accumulator expects read data that is not valid; a write request is valid but "
        "not accepted; or the accumulation-to-writeback FIFO cannot accept another completed sum.\n\n"
        "The component conditions can overlap, so this counter is not additive with other stalls."
    ),
    "output_fifo_full_cycles": (
        "Producer-side final-output blockage. write_back has a completed final accumulation and asserts enqueue "
        "valid, but the eight-entry final-output FIFO has no available slot.\n\n"
        "The FIFO drains through the processor output channel into MatrixUnit OutputController."
    ),
    "output_backpressure_cycles": (
        "Consumer-side final-output blockage. The final-output FIFO presents a valid entry on the processor output "
        "channel, but MatrixUnit OutputController is not ready to pop it.\n\n"
        "OutputController serializes the value and either sends it to memory or pushes it to the vector unit. This "
        "differs from Output FIFO full, which is measured at the FIFO's enqueue side."
    ),
    "cim_set_wait_cycles": (
        "CIM only. The MAC issue loop has reached a weight-set swap and is ready to pop set_filled, but the loader has "
        "not finished every row of that resident set. The loader may still be receiving weight beats or writing them "
        "into CIMArray."
    ),
    "cim_completion_queue_stall_cycles": (
        "CIM only. Each accepted MAC immediately reserves one entry in CIMArray's central completion queue because "
        "the fixed-latency tile pipeline cannot be stopped after issue.\n\n"
        "The entry first holds request metadata, then the captured tile results, and is released only after its final "
        "CBeat is accepted by CIMProcessor. This stall means all entries are reserved and no entry is being released "
        "on the current cycle; it does not refer to a speculative result."
    ),
}


# Convert a CSV value to an integer when present
def integer(value):
    if value in ("", None):
        return None
    return int(float(value))


# Interpret the parser's Boolean text
def passed(value):
    return str(value).lower() == "true"


# Return the stable identity of one hardware instance
def point_key(row):
    latency = row.get("cim_mac_latency", "") if row["backend"] == "cim" else ""
    return (
        row["config"],
        row["ic_port_width_bits"],
        row["oc_port_width_bits"],
        latency,
        row.get("clock_period_ns", ""),
    )


# Return a human-readable explicit port-width label
def port_label(row):
    ic = row["ic_port_width_bits"]
    oc = row["oc_port_width_bits"]
    return f"{ic}-bit" if ic == oc else f"{ic}/{oc}-bit"


# Return a complete instance label with CIM latency only where applicable
def instance_label(row):
    base = f"{'CIM' if row['backend'] == 'cim' else 'SA'} {row['geometry']} | {port_label(row)} ports"
    if row["backend"] == "cim":
        base += f" | MAC latency {row.get('cim_mac_latency') or '1'}"
    return base


# Return the configured array MAC count
def macs(row):
    return integer(row["macs"]) or 0


# Apply the common title band
def style_title(sheet, title, last_column):
    sheet.merge_cells(start_row=1, start_column=1, end_row=1, end_column=last_column)
    cell = sheet.cell(1, 1, title)
    cell.font = TITLE
    cell.fill = PatternFill("solid", fgColor=NAVY)
    cell.alignment = Alignment(vertical="center")
    sheet.row_dimensions[1].height = 30
    sheet.sheet_view.showGridLines = False


# Apply the common table header
def style_header(sheet, row, first_column, last_column):
    for column in range(first_column, last_column + 1):
        cell = sheet.cell(row, column)
        cell.font = HEADER
        cell.fill = PatternFill("solid", fgColor=TEAL)
        cell.alignment = CENTER
    sheet.row_dimensions[row].height = 38


# Apply the common body-row styling
def style_body_row(sheet, row, first_column, last_column, fill):
    for column in range(first_column, last_column + 1):
        cell = sheet.cell(row, column)
        cell.font = BODY
        cell.fill = PatternFill("solid", fgColor=fill)
        cell.border = THIN_BOTTOM
        cell.alignment = WRAP if column == first_column else RIGHT


# Add a filterable Excel table with a stable style
def add_table(sheet, name, first_row, last_row, first_column, last_column):
    if last_row < first_row:
        return
    ref = (
        f"{get_column_letter(first_column)}{first_row}:"
        f"{get_column_letter(last_column)}{last_row}"
    )
    table = Table(displayName=name, ref=ref)
    table.tableStyleInfo = TableStyleInfo(
        name="TableStyleMedium2",
        showFirstColumn=False,
        showLastColumn=False,
        showRowStripes=True,
        showColumnStripes=False,
    )
    sheet.add_table(table)


# Assign explicit bounded column widths
def set_widths(sheet, widths):
    for index, width in enumerate(widths, 1):
        sheet.column_dimensions[get_column_letter(index)].width = width


all_layers = list(OrderedDict((row["layer"], None) for row in rows))
layers = [layer for layer in LAYER_ORDER if layer in all_layers]
layers.extend(sorted(layer for layer in all_layers if layer not in layers))

point_rows = OrderedDict()
for row in rows:
    point_rows.setdefault(point_key(row), []).append(row)

points = []
for key, observations in point_rows.items():
    representative = observations[0]
    rtl = {row["layer"]: row for row in observations if row["sim"] == "rtl"}
    passing_layers = [
        layer
        for layer in layers
        if layer in rtl
        and passed(rtl[layer]["passed"])
        and all(rtl[layer].get(field) not in ("", None) for field in REQUIRED_RTL_FIELDS)
    ]
    reportable = len(passing_layers) == len(layers)
    if reportable:
        reason = ""
    elif not rtl:
        reason = "RTL generation or simulation did not produce layer results"
    else:
        reason = f"{len(passing_layers)}/{len(layers)} layers passed with complete RTL counters"
    points.append(
        {
            "key": key,
            "row": representative,
            "rtl": rtl,
            "reportable": reportable,
            "passing_layers": passing_layers,
            "reason": reason,
        }
    )

points.sort(
    key=lambda point: (
        macs(point["row"]),
        point["row"]["backend"] != "systolic",
        integer(point["row"]["ic_port_width_bits"]) or 0,
        integer(point["row"].get("cim_mac_latency")) or 0,
    )
)
point_by_key = {point["key"]: point for point in points}
reportable_points = [point for point in points if point["reportable"]]
sa_points = [point for point in points if point["row"]["backend"] == "systolic"]
cim_points = [point for point in points if point["row"]["backend"] == "cim"]

if not sa_points:
    sys.exit("no systolic design point found")

baseline_sa = min(sa_points, key=lambda point: macs(point["row"]))
baseline_geometry = baseline_sa["row"]["geometry"]
baseline_ports = (
    baseline_sa["row"]["ic_port_width_bits"],
    baseline_sa["row"]["oc_port_width_bits"],
)


# Locate one point by backend, geometry, ports, and optional CIM latency
def find_point(backend, geometry, ports, latency=None):
    for point in points:
        row = point["row"]
        if row["backend"] != backend or row["geometry"] != geometry:
            continue
        if (row["ic_port_width_bits"], row["oc_port_width_bits"]) != ports:
            continue
        if backend == "cim" and str(row.get("cim_mac_latency") or "1") != str(latency or 1):
            continue
        return point
    return None


comparisons = []
seen_comparisons = set()


# Add one explicit comparison without duplicating it
def add_comparison(kind, sa, cim):
    if not sa or not cim:
        return
    key = (kind, sa["key"], cim["key"])
    if key in seen_comparisons:
        return
    seen_comparisons.add(key)
    comparisons.append({"kind": kind, "sa": sa, "cim": cim})


for sa in sa_points:
    row = sa["row"]
    for cim in cim_points:
        cim_row = cim["row"]
        if cim_row["geometry"] != row["geometry"]:
            continue
        if (cim_row["ic_port_width_bits"], cim_row["oc_port_width_bits"]) != (
            row["ic_port_width_bits"],
            row["oc_port_width_bits"],
        ):
            continue
        add_comparison("Same array size and port width", sa, cim)

for cim in cim_points:
    row = cim["row"]
    if row["geometry"] == baseline_geometry or str(row.get("cim_mac_latency") or "1") != "1":
        continue
    if (row["ic_port_width_bits"], row["oc_port_width_bits"]) == baseline_ports:
        kind = "SA baseline vs larger CIM, baseline-matched ports"
    else:
        kind = "SA baseline vs larger CIM, CIM-dimension-matched ports"
    add_comparison(kind, baseline_sa, cim)

wb = Workbook()
wb.remove(wb.active)

# -------------------------------------------------------------- Comparisons
ws = wb.create_sheet("Comparisons")
style_title(ws, "CIM vs systolic — per-layer RTL comparison", 11)
ws["A2"] = (
    "Every row is one layer. No cycles are added across layers. "
    "Speedup is SA RTL cycles divided by CIM RTL cycles."
)
ws["A2"].font = Font(name=FONT, italic=True, color=GRAY)
ws.merge_cells("A2:K2")

comparison_headers = [
    "Comparison",
    "Comparison type",
    "Layer",
    "SA instance",
    "CIM instance",
    "SA RTL cycles",
    "CIM RTL cycles",
    "CIM speedup",
    "SA MatrixUnit utilization",
    "CIM MatrixUnit utilization",
    "Utilization delta",
]
for column, heading in enumerate(comparison_headers, 1):
    ws.cell(4, column, heading)
style_header(ws, 4, 1, len(comparison_headers))

comparison_start = 5
comparison_row = comparison_start
result_row_map = {}
pending_comparison_formulas = []
unavailable = []
for comparison in comparisons:
    sa = comparison["sa"]
    cim = comparison["cim"]
    label = f"{instance_label(sa['row'])} vs {instance_label(cim['row'])}"
    if not sa["reportable"] or not cim["reportable"]:
        unavailable.append((label, "FAILED"))
        continue
    for layer in layers:
        ws.cell(comparison_row, 1, label)
        ws.cell(comparison_row, 2, comparison["kind"])
        ws.cell(comparison_row, 3, LAYER_SHORT.get(layer, layer))
        ws.cell(comparison_row, 4, instance_label(sa["row"]))
        ws.cell(comparison_row, 5, instance_label(cim["row"]))
        pending_comparison_formulas.append(
            (comparison_row, sa["key"], cim["key"], layer)
        )
        style_body_row(ws, comparison_row, 1, len(comparison_headers), SA_BLUE)
        comparison_row += 1

comparison_last = comparison_row - 1
add_table(
    ws,
    "PerLayerComparisons",
    4,
    comparison_last,
    1,
    len(comparison_headers),
)
ws.freeze_panes = "F5"
set_widths(ws, [46, 38, 29, 38, 43, 14, 15, 13, 18, 19, 16])

if unavailable:
    failed_title_row = comparison_row + 2
    ws.cell(failed_title_row, 1, "Unavailable comparisons")
    ws.cell(failed_title_row, 1).font = SECTION
    ws.cell(failed_title_row, 1).fill = PatternFill("solid", fgColor=NAVY)
    ws.merge_cells(
        start_row=failed_title_row,
        start_column=1,
        end_row=failed_title_row,
        end_column=2,
    )
    for index, (label, status) in enumerate(unavailable, failed_title_row + 1):
        ws.cell(index, 1, label)
        ws.cell(index, 2, status)
        ws.cell(index, 2).fill = PatternFill("solid", fgColor=FAIL_RED)

# --------------------------------------------------------- Per-Layer Results
ws = wb.create_sheet("Per-Layer Results")
style_title(ws, "Passing RTL results — one row per instance and layer", 12)
ws["A2"] = (
    "MatrixUnit utilization = ideal MAC cycles / MatrixUnit start-to-done cycles. "
    "Failed hardware points are excluded and appear only in Design Points."
)
ws["A2"].font = Font(name=FONT, italic=True, color=GRAY)
ws.merge_cells("A2:L2")
result_headers = [
    "Instance",
    "Config",
    "Backend",
    "Array",
    "Input port bits",
    "Output port bits",
    "CIM MAC latency",
    "Layer",
    "RTL cycles",
    "MatrixUnit cycles",
    "Ideal MAC cycles",
    "MatrixUnit utilization",
]
for column, heading in enumerate(result_headers, 1):
    ws.cell(4, column, heading)
style_header(ws, 4, 1, len(result_headers))

result_row = 5
for point in reportable_points:
    row = point["row"]
    fill = SA_BLUE if row["backend"] == "systolic" else CIM_GREEN
    for layer in layers:
        rtl = point["rtl"][layer]
        values = [
            instance_label(row),
            row["config"],
            row["backend"],
            row["geometry"],
            integer(row["ic_port_width_bits"]),
            integer(row["oc_port_width_bits"]),
            integer(row.get("cim_mac_latency")) if row["backend"] == "cim" else None,
            LAYER_SHORT.get(layer, layer),
            integer(rtl["runtime_cycles"]),
            integer(rtl["matrix_unit_cycles"]),
            integer(rtl["ideal_cycles"]),
        ]
        for column, value in enumerate(values, 1):
            ws.cell(result_row, column, value)
        ws.cell(result_row, 12, f"=IFERROR(K{result_row}/J{result_row},\"\")")
        ws.cell(result_row, 12).number_format = "0.0%"
        for column in range(5, 12):
            ws.cell(result_row, column).number_format = "#,##0"
        style_body_row(ws, result_row, 1, len(result_headers), fill)
        result_row_map[(point["key"], layer)] = result_row
        result_row += 1
add_table(
    ws,
    "PerLayerRtlResults",
    4,
    result_row - 1,
    1,
    len(result_headers),
)
ws.freeze_panes = "I5"
set_widths(ws, [43, 31, 11, 11, 14, 15, 16, 29, 12, 19, 16, 18])

# Link comparison formulas to the auditable per-layer results
comparison_ws = wb["Comparisons"]
for row, sa_key, cim_key, layer in pending_comparison_formulas:
    sa_result_row = result_row_map[(sa_key, layer)]
    cim_result_row = result_row_map[(cim_key, layer)]
    comparison_ws.cell(row, 6, f"='Per-Layer Results'!I{sa_result_row}")
    comparison_ws.cell(row, 7, f"='Per-Layer Results'!I{cim_result_row}")
    comparison_ws.cell(row, 8, f'=IFERROR(F{row}/G{row},"")')
    comparison_ws.cell(row, 9, f"='Per-Layer Results'!L{sa_result_row}")
    comparison_ws.cell(row, 10, f"='Per-Layer Results'!L{cim_result_row}")
    comparison_ws.cell(row, 11, f'=IFERROR(J{row}-I{row},"")')
    comparison_ws.cell(row, 8).number_format = "0.00x"
    for column in (9, 10, 11):
        comparison_ws.cell(row, column).number_format = "0.0%"

# ------------------------------------------------------------- Stall Analysis
ws = wb.create_sheet("Stall Analysis")
stall_headers = [
    "Instance",
    "Layer",
    "Processor active cycles",
    *[heading for _, heading in STALL_FIELDS],
]
style_title(ws, "Passing RTL stall counters — SA and CIM", len(stall_headers))
ws["A2"] = (
    "Counters are simultaneous conditions and may overlap. "
    "Complete signal-level definitions are in the final Definition sheet."
)
ws["A2"].font = Font(name=FONT, italic=True, color=GRAY)
ws.merge_cells(start_row=2, start_column=1, end_row=2, end_column=len(stall_headers))
for column, heading in enumerate(stall_headers, 1):
    ws.cell(4, column, heading)
style_header(ws, 4, 1, len(stall_headers))

stall_row = 5
for point in reportable_points:
    row = point["row"]
    fill = SA_BLUE if row["backend"] == "systolic" else CIM_GREEN
    for layer in layers:
        rtl = point["rtl"][layer]
        values = [
            instance_label(row),
            LAYER_SHORT.get(layer, layer),
            integer(rtl["processor_active_cycles"]),
            *[
                integer(rtl.get(field))
                if row["backend"] == "cim" or not field.startswith("cim_")
                else None
                for field, _ in STALL_FIELDS
            ],
        ]
        for column, value in enumerate(values, 1):
            ws.cell(stall_row, column, value)
            if column >= 3:
                ws.cell(stall_row, column).number_format = "#,##0"
        style_body_row(ws, stall_row, 1, len(stall_headers), fill)
        stall_row += 1
add_table(
    ws,
    "RtlStallAnalysis",
    4,
    stall_row - 1,
    1,
    len(stall_headers),
)
ws.freeze_panes = "D5"
set_widths(ws, [43, 29, 19] + [18] * len(STALL_FIELDS))

# -------------------------------------------------------------- Design Points
ws = wb.create_sheet("Design Points")
design_headers = [
    "Instance",
    "Config",
    "Backend",
    "Array",
    "Input port bits",
    "Output port bits",
    "Clock period (ns)",
    "CIM MAC latency",
    "Macro native width",
    "CH_IN",
    "CH_OUT",
    "RTL status",
    "Passing RTL layers",
    "Failure reason",
]
style_title(ws, "Sweep design points and RTL status", len(design_headers))
ws["A2"] = (
    "MAC latency is a CIM-only parameter. SA cells are intentionally blank. "
    "A failed point has no performance or comparison rows."
)
ws["A2"].font = Font(name=FONT, italic=True, color=GRAY)
ws.merge_cells(start_row=2, start_column=1, end_row=2, end_column=len(design_headers))
for column, heading in enumerate(design_headers, 1):
    ws.cell(4, column, heading)
style_header(ws, 4, 1, len(design_headers))
design_row = 5
for point in points:
    row = point["row"]
    values = [
        instance_label(row),
        row["config"],
        row["backend"],
        row["geometry"],
        integer(row["ic_port_width_bits"]),
        integer(row["oc_port_width_bits"]),
        float(row["clock_period_ns"]),
        integer(row.get("cim_mac_latency")) if row["backend"] == "cim" else None,
        row.get("cim_macro_native_width") or None,
        integer(row.get("cim_ch_in")),
        integer(row.get("cim_ch_out")),
        "PASS" if point["reportable"] else "FAILED",
        f"{len(point['passing_layers'])}/{len(layers)}",
        point["reason"],
    ]
    for column, value in enumerate(values, 1):
        ws.cell(design_row, column, value)
    fill = SA_BLUE if row["backend"] == "systolic" else CIM_GREEN
    style_body_row(ws, design_row, 1, len(design_headers), fill)
    ws.cell(design_row, 12).fill = PatternFill(
        "solid", fgColor=PASS_GREEN if point["reportable"] else FAIL_RED
    )
    ws.cell(design_row, 12).font = BODY_BOLD
    design_row += 1
add_table(
    ws,
    "SweepDesignPoints",
    4,
    design_row - 1,
    1,
    len(design_headers),
)
ws.freeze_panes = "E5"
set_widths(ws, [43, 31, 11, 11, 14, 15, 16, 16, 18, 10, 10, 12, 17, 54])

# ---------------------------------------------------------------- Definition
ws = wb.create_sheet("Definition")
style_title(ws, "Definition", 3)
method_rows = [
    (
        "MatrixUnit cycles",
        "Harness Matrix Unit Runtime / clock period",
        "Measured from the MatrixUnit start handshake through the MatrixUnit done handshake. It includes "
        "OutputController drain and excludes any later vector-unit tail. The clock period comes from the sweep "
        "manifest.",
    ),
    (
        "Ideal MAC cycles",
        "Total MAC work / array MACs per cycle",
        "Minimum full-array cycles implied by the layer's MAC count and configured compute resources.",
    ),
    (
        "MatrixUnit utilization",
        "Ideal MAC cycles / MatrixUnit cycles",
        "The workbook's utilization metric for matrix operations.",
    ),
    (
        "Processor active cycles",
        "Hardware processor_active_cycles counter",
        "Internal diagnostic window from processor parameter acceptance through processor write-back completion. "
        "Retained on Stall Analysis but not used for utilization.",
    ),
    (
        "Stall counters",
        "SA and CIM",
        "Stall conditions may overlap and must not be added. Complete signal-level definitions follow below.",
    ),
]
method_rows.extend(
    [
        *[
            (
                heading,
                "CIM only" if field.startswith("cim_") else "SA and CIM",
                STALL_DEFINITIONS[field].replace("\n\n", " "),
            )
            for field, heading in STALL_FIELDS
        ],
    ]
)
for column, heading in enumerate(["Field", "Definition", "Notes"], 1):
    ws.cell(3, column, heading)
style_header(ws, 3, 1, 3)
for row_index, values in enumerate(method_rows, 4):
    for column, value in enumerate(values, 1):
        ws.cell(row_index, column, value)
        ws.cell(row_index, column).font = BODY_BOLD if column == 1 else BODY
        ws.cell(row_index, column).alignment = WRAP
        ws.cell(row_index, column).border = THIN_BOTTOM
    ws.cell(row_index, 1).fill = PatternFill("solid", fgColor=TEAL_LIGHT)
    ws.row_dimensions[row_index].height = max(34, 15 * (1 + len(str(values[2])) // 82))
add_table(ws, "DefinitionTable", 3, 3 + len(method_rows), 1, 3)
set_widths(ws, [28, 38, 92])

for sheet in wb.worksheets:
    sheet.sheet_properties.pageSetUpPr.fitToPage = True
    sheet.page_setup.fitToWidth = 1
    sheet.page_setup.fitToHeight = 0
    sheet.sheet_view.zoomScale = 90

wb.calculation.fullCalcOnLoad = True
wb.calculation.forceFullCalc = True
wb.calculation.calcMode = "auto"
wb.save(OUT)

print(
    f"saved {OUT}: {len(wb.sheetnames)} sheets, "
    f"{len(points)} hardware points, {len(reportable_points)} RTL-pass points, "
    f"{len(comparisons)} comparison views, {len(layers)} layers"
)
