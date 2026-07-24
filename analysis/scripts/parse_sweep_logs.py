#!/usr/bin/env python3
"""Parse one sweep directory into a flat CSV of per-run metrics.

Usage: parse_sweep_logs.py <sweep_dir> <out_csv>

Config metadata comes entirely from <sweep_dir>/manifest.csv (emitted by
matrix_backend_sweep.sh), so nothing about the design space is hardcoded here.
Log files are named
    <config>[_pw<bits>]__<layer>.log         SystemC functional run
    <config>[_pw<bits>]__rtl__<layer>.log    RTL cosim (cycle-accurate + counters)
One CSV row per (config, port width, sim, layer).
"""
import csv, os, re, sys

SWEEP_DIR = sys.argv[1]
OUT = sys.argv[2]
CLK_NS = 5.0
FREQ_HZ = 200e6
INT8_BITS = 8

# MatrixPerfHardware counters carried through to the CSV (present in RTL logs)
PERF_COUNTERS = [
    "core_cycles", "array_resident_cycles", "array_issue_cycles",
    "input_unavailable_cycles", "input_backpressure_cycles",
    "weight_unavailable_cycles", "weight_backpressure_cycles",
    "result_backpressure_cycles", "accumulation_stall_cycles",
    "output_backpressure_cycles", "output_fifo_full_cycles",
]

manifest_path = os.path.join(SWEEP_DIR, "manifest.csv")
if not os.path.exists(manifest_path):
    sys.exit(f"no manifest at {manifest_path}")

CFG = {}
for m in csv.DictReader(open(manifest_path)):
    is_cim = m["backend"] == "1"
    CFG[m["config"]] = dict(
        backend="cim" if is_cim else "systolic",
        K=int(m["K"]), N=int(m["N"]),
        # Width the ports take when left unpinned: one array row/column per
        # cycle. Older manifests predate the columns, so fall back to INT8.
        ic_matched=int(m.get("ic_matched_port_bits") or int(m["K"]) * INT8_BITS),
        oc_matched=int(m.get("oc_matched_port_bits") or int(m["N"]) * INT8_BITS),
        cell=(m["cell_bits"] + "b") if (is_cim and m["cell_bits"]) else "",
        ch_in=m["ch_in"], ch_out=m["ch_out"],
        input_axis_tiles=m["input_axis_tiles"] or "",
        output_axis_tiles=m["output_axis_tiles"] or "",
    )

rows = []
for fn in sorted(os.listdir(SWEEP_DIR)):
    if "__" not in fn or not fn.endswith(".log"):
        continue
    tag, layer = fn[:-4].split("__", 1)
    sim = "systemc"
    if layer.startswith("rtl__"):
        sim, layer = "rtl", layer[len("rtl__"):]
    # _pw<bits> pins both ports; _pw<ic>x<oc> pins them separately (asymmetric
    # geometries). No suffix means the widths were left derived, i.e. matched.
    mm = re.match(r"^(.*)_pw(\d+)(?:x(\d+))?$", tag)
    if mm:
        cfg, pinned = mm.group(1), True
        pw_ic = int(mm.group(2))
        pw_oc = int(mm.group(3) or mm.group(2))
    else:
        cfg, pinned, pw_ic, pw_oc = tag, False, None, None
    if cfg not in CFG:
        continue

    txt = open(os.path.join(SWEEP_DIR, fn), errors="replace").read()
    meta = CFG[cfg]
    ic, oc = meta["K"], meta["N"]

    def grab(pat):
        g = re.search(pat, txt, re.M | re.I)
        return int(g.group(1)) if g else None

    ok = bool(re.search(r"Error\s+count:\s+0\b", txt))
    total_ns = grab(r"^Total Runtime:\s+(\d+)\s*ns")
    ideal_ns = grab(r"matrix unit ideal runtime:\s+(\d+)\s*ns")
    rd = grab(r"^harness:\s+(\d+)") or 0
    wr = grab(r"^harness_outputs:\s+(\d+)") or 0
    perf = {}
    pm = re.search(r"^MatrixPerfHardware:(.*)$", txt, re.M)
    if pm:
        perf = dict(kv.split("=") for kv in pm.group(1).split())

    # Only RTL cosimulation is performance-reportable
    cyc = total_ns / CLK_NS if (sim == "rtl" and total_ns) else None
    ideal_cyc = ideal_ns / CLK_NS if (sim == "rtl" and ideal_ns) else None
    # Idealised L2 model: one word/cycle/port. Unpinned runs sit at the matched
    # width (one array row/column per cycle); a pinned width overrides both.
    ic_pw = pw_ic if pinned else meta["ic_matched"]
    oc_pw = pw_oc if pinned else meta["oc_matched"]
    peak_rd_bpc = (ic_pw + 2 * oc_pw) / 8
    peak_wr_bpc = oc_pw / 8
    rd_bpc = rd / cyc if cyc else None
    wr_bpc = wr / cyc if cyc else None

    rows.append(dict(
        config=cfg, sim=sim, bw_mode="pinned" if pinned else "matched",
        ic_port_width_bits=ic_pw, oc_port_width_bits=oc_pw,
        backend=meta["backend"], geometry=f"{ic}x{oc}", IC=ic, OC=oc, macs=ic * oc,
        cim_cell=meta["cell"], cim_ch_in=meta["ch_in"], cim_ch_out=meta["ch_out"],
        input_axis_tiles=meta["input_axis_tiles"],
        output_axis_tiles=meta["output_axis_tiles"],
        layer=layer, passed=ok,
        runtime_cycles=int(cyc) if cyc else "",
        runtime_us=round(total_ns / 1000.0, 3) if cyc else "",
        ideal_cycles=int(ideal_cyc) if ideal_cyc else "",
        utilization=round(ideal_cyc / cyc, 4) if (cyc and ideal_cyc) else "",
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

if not rows:
    sys.exit("no matching logs found for any manifest config")

rows.sort(key=lambda r: (r["sim"], r["layer"], r["macs"], r["config"]))
with open(OUT, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
    w.writeheader()
    w.writerows(rows)
print(f"wrote {OUT} ({len(rows)} rows, "
      f"{len({r['config'] for r in rows})} configs, "
      f"sims={sorted({r['sim'] for r in rows})})")
