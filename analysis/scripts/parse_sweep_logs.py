#!/usr/bin/env python3
"""Parse sweep logs into a CSV of runtime / utilization / external traffic.

Usage: cmp_parse.py <logs_dir> <out_csv>

Handles both naming schemes:
  <config>__<layer>.log            derived port width (scales with the array)
  <config>_pw<bits>__<layer>.log   pinned external (L2) port width
"""
import csv, os, re, sys

LOGS = sys.argv[1]
OUT = sys.argv[2]
CLK_NS = 5.0
FREQ_HZ = 200e6
READ_PORTS = 3          # input, weight, bias each get their own port
INT8_BITS = 8

# config -> (backend, IC, OC, cell width, array organization)
# "cell" is the CIM macro cell width: 8b matches the INT8 systolic datapath
# (native comparison point), 4b is the vanilla macro (two cells per INT8 weight,
# two A nibble-slices per MAC).
CFG = {
    "sa_32x32":      ("systolic", 32, 32, "",   ""),
    "sa_32x64":      ("systolic", 32, 64, "",   ""),
    "sa_64x32":      ("systolic", 64, 32, "",   ""),
    "sa_64x64":      ("systolic", 64, 64, "",   ""),
    "cimb8_32x32":   ("cim", 32, 32, "8b", "4 elem = 1 tile x 4 elem"),
    "cimb8_32x64":   ("cim", 32, 64, "8b", "8 elem = 2 tiles x 4 elem"),
    "cimb8_64x32":   ("cim", 64, 32, "8b", "4 elem = 1 tile x 4 elem"),
    "cimb8_64x64":   ("cim", 64, 64, "8b", "8 elem = 2 tiles x 4 elem"),
    "cim_32x32":     ("cim", 32, 32, "4b", "8 elem = 4 tiles x 2 elem"),
    "cim_32x64":     ("cim", 32, 64, "4b", "16 elem = 4 tiles x 4 elem"),
    "cim_64x32":     ("cim", 64, 32, "4b", "8 elem = 4 tiles x 2 elem"),
    "cim_64x64":     ("cim", 64, 64, "4b", "16 elem = 4 tiles x 4 elem"),
    "cimflat_64x64": ("cim", 64, 64, "4b", "16 elem = 16 tiles x 1 elem"),
}

rows = []
# Names of the MatrixPerfHardware counters carried through to the CSV
PERF_COUNTERS = [
    "core_cycles", "array_resident_cycles", "array_issue_cycles",
    "input_unavailable_cycles", "input_backpressure_cycles",
    "weight_unavailable_cycles", "weight_backpressure_cycles",
    "result_backpressure_cycles", "accumulation_stall_cycles",
    "output_backpressure_cycles", "output_fifo_full_cycles",
]


# Return a rounded ratio only when both inputs are usable
def ratio(numerator, denominator):
    if numerator is None or denominator in (None, 0):
        return ""
    return round(numerator / denominator, 4)


# Convert one optional hardware counter to an integer
def counter_value(perf, name):
    value = perf.get(name)
    return int(value) if value is not None else None


for fn in sorted(os.listdir(LOGS)):
    if "__" not in fn or not fn.endswith(".log"):
        continue
    tag, layer = fn[:-4].split("__", 1)
    # RTL cosim logs are tagged <config>__rtl__<layer>.log
    sim = "systemc"
    if layer.startswith("rtl__"):
        sim = "rtl"
        layer = layer[len("rtl__"):]
    m = re.match(r"^(.*)_pw(\d+)$", tag)
    if m:
        cfg, pw = m.group(1), int(m.group(2))
        pinned = True
    else:
        cfg, pw, pinned = tag, None, False
    if cfg not in CFG:
        continue
    txt = open(os.path.join(LOGS, fn), errors="replace").read()
    backend, ic, oc, cell, org = CFG[cfg]

    # Derived widths when not pinned: input port tracks IC, weight/bias track OC
    ic_pw = pw if pinned else ic * INT8_BITS
    oc_pw = pw if pinned else oc * INT8_BITS

    def grab(pat):
        mm = re.search(pat, txt, re.M | re.I)
        return int(mm.group(1)) if mm else None

    ok = bool(re.search(r"Error count:\s+0\b", txt))
    total_ns = grab(r"^Total Runtime:\s+(\d+)\s*ns")
    ideal_ns = grab(r"matrix unit ideal runtime:\s+(\d+)\s*ns")
    rd = grab(r"^harness:\s+(\d+)") or 0
    wr = grab(r"^harness_outputs:\s+(\d+)") or 0

    cyc = total_ns / CLK_NS if total_ns else None
    ideal_cyc = ideal_ns / CLK_NS if ideal_ns else None

    # Idealised L2 model: one word per cycle per port, zero latency, no
    # contention. Aggregate read budget is the input port plus the weight and
    # bias ports.
    peak_rd_bpc = (ic_pw + 2 * oc_pw) / 8
    peak_wr_bpc = oc_pw / 8
    rd_bpc = rd / cyc if cyc else None
    wr_bpc = wr / cyc if cyc else None

    # Hardware counter snapshot, present only in perf-counter RTL cosim logs
    perf = {}
    pm = re.search(r"^MatrixPerfHardware:(.*)$", txt, re.M)
    if pm:
        perf = dict(kv.split("=", 1) for kv in pm.group(1).split())

    runtime_cycles = int(cyc) if cyc else None
    core_cycles = counter_value(perf, "core_cycles")
    resident_cycles = counter_value(perf, "array_resident_cycles")
    issue_cycles = counter_value(perf, "array_issue_cycles")

    rows.append(dict(
        # Matrix ideal work divided by the whole fused-layer runtime
        fused_layer_matrix_efficiency=ratio(ideal_cyc, cyc),
        # Array-active means at least one MAC is in flight; array-issue means a
        # new MAC request was accepted
        array_active_utilization=ratio(resident_cycles, runtime_cycles),
        array_issue_utilization=ratio(issue_cycles, runtime_cycles),
        array_active_fraction_of_matrix=ratio(resident_cycles, core_cycles),
        array_issue_fraction_of_matrix=ratio(issue_cycles, core_cycles),
        config=cfg, sim=sim, bw_mode="pinned" if pinned else "derived",
        port_width_bits=pw if pinned else "",
        backend=backend, geometry=f"{ic}x{oc}", IC=ic, OC=oc, macs=ic * oc,
        cim_cell=cell, cim_org=org, layer=layer, passed=ok,
        runtime_cycles=runtime_cycles if runtime_cycles else "",
        runtime_us=round(total_ns / 1000.0, 3) if total_ns else "",
        ideal_cycles=int(ideal_cyc) if ideal_cyc else "",
        cycles_per_ideal_beat=round(cyc / ideal_cyc, 3) if (cyc and ideal_cyc) else "",
        ext_read_bytes=rd, ext_write_bytes=wr,
        ext_read_GBps=round(rd_bpc * FREQ_HZ / 1e9, 2) if rd_bpc else "",
        ext_write_GBps=round(wr_bpc * FREQ_HZ / 1e9, 2) if wr_bpc else "",
        peak_read_GBps=round(peak_rd_bpc * FREQ_HZ / 1e9, 2),
        peak_write_GBps=round(peak_wr_bpc * FREQ_HZ / 1e9, 2),
        read_bw_pct_of_peak=round(100 * rd_bpc / peak_rd_bpc, 1) if rd_bpc else "",
        write_bw_pct_of_peak=round(100 * wr_bpc / peak_wr_bpc, 1) if wr_bpc else "",
        **{name: perf.get(name, "") for name in PERF_COUNTERS},
    ))

rows.sort(key=lambda r: (r["sim"], r["bw_mode"], str(r["port_width_bits"]),
                         r["layer"], r["macs"], r["config"]))
with open(OUT, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
    w.writeheader()
    w.writerows(rows)
print(f"wrote {OUT} ({len(rows)} rows)")
