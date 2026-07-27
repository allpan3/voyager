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
    "cim_set_wait_cycles", "cim_completion_queue_stall_cycles",
    "cim_result_path_stall_cycles",
]
CIM_STALL_COUNTERS = [
    "cim_set_wait_cycles",
    "cim_completion_queue_stall_cycles",
    "cim_result_path_stall_cycles",
]

manifest_path = os.path.join(SWEEP_DIR, "manifest.csv")
if not os.path.exists(manifest_path):
    sys.exit(f"no manifest at {manifest_path}")

CFG = {}
for m in csv.DictReader(open(manifest_path)):
    is_cim = m["backend"] == "1"
    # macro_native_width_bits was called cell_bits before the terminology was
    # settled; read either so older sweep directories still parse.
    native = m.get("macro_native_width_bits") or m.get("cell_bits") or ""
    CFG[m["config"]] = dict(
        backend="cim" if is_cim else "systolic",
        K=int(m["K"]), N=int(m["N"]),
        # Width the ports take when left unpinned: one array row/column per
        # cycle. Older manifests predate the columns, so fall back to INT8.
        ic_matched=int(m.get("ic_matched_port_bits") or int(m["K"]) * INT8_BITS),
        oc_matched=int(m.get("oc_matched_port_bits") or int(m["N"]) * INT8_BITS),
        # The array every point is judged against. Absent in older manifests;
        # the workbook falls back to inferring it from the systolic rows.
        baseline_geometry=m.get("baseline_geometry") or "",
        native_width=(native + "b") if (is_cim and native) else "",
        mac_latency=(m.get("cim_mac_latency") or "1") if is_cim else "",
        ch_in=m["ch_in"], ch_out=m["ch_out"],
        input_axis_tiles=m["input_axis_tiles"] or "",
        output_axis_tiles=m["output_axis_tiles"] or "",
        # CIMTile organization: elements per tile along each axis. Manifests
        # predating the columns come from a sweep that pinned both to 1, so
        # that is the fallback rather than an unknown.
        tile_in_elems=(m.get("tile_input_axis_elements") or "1") if is_cim else "",
        tile_out_elems=(m.get("tile_output_axis_elements") or "1") if is_cim else "",
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
        if "cim_completion_queue_stall_cycles" not in perf:
            perf["cim_completion_queue_stall_cycles"] = perf.get(
                "cim_array_credit_stall_cycles", ""
            )

    # Normalize one synthesized stall counter against its snapshot duration
    def perf_pct_of_core(name):
        try:
            return round(100 * int(perf[name]) / int(perf["core_cycles"]), 1)
        except (KeyError, TypeError, ValueError, ZeroDivisionError):
            return ""

    # Only passing RTL cosimulation is performance-reportable
    cyc = total_ns / CLK_NS if (sim == "rtl" and ok and total_ns) else None
    ideal_cyc = ideal_ns / CLK_NS if (sim == "rtl" and ok and ideal_ns) else None
    core_cyc = int(perf["core_cycles"]) if (ok and perf.get("core_cycles")) else None
    # Idealised L2 model: one word/cycle/port. Unpinned runs sit at the matched
    # width (one array row/column per cycle); pinned widths override both.
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
        baseline_geometry=meta["baseline_geometry"],
        cim_macro_native_width=meta["native_width"],
        cim_mac_latency=meta["mac_latency"],
        cim_ch_in=meta["ch_in"], cim_ch_out=meta["ch_out"],
        input_axis_tiles=meta["input_axis_tiles"],
        output_axis_tiles=meta["output_axis_tiles"],
        cim_tile_input_axis_elements=meta["tile_in_elems"],
        cim_tile_output_axis_elements=meta["tile_out_elems"],
        layer=layer, passed=ok,
        runtime_cycles=int(cyc) if cyc else "",
        runtime_us=round(total_ns / 1000.0, 3) if cyc else "",
        ideal_cycles=int(ideal_cyc) if ideal_cyc else "",
        matrix_utilization=round(ideal_cyc / core_cyc, 4)
        if (ideal_cyc and core_cyc) else "",
        ext_read_bytes=rd, ext_write_bytes=wr,
        ext_read_GBps=round(rd_bpc * FREQ_HZ / 1e9, 2) if rd_bpc else "",
        ext_write_GBps=round(wr_bpc * FREQ_HZ / 1e9, 2) if wr_bpc else "",
        peak_read_GBps=round(peak_rd_bpc * FREQ_HZ / 1e9, 2),
        peak_write_GBps=round(peak_wr_bpc * FREQ_HZ / 1e9, 2),
        read_bw_pct_of_peak=round(100 * rd_bpc / peak_rd_bpc, 1) if rd_bpc else "",
        write_bw_pct_of_peak=round(100 * wr_bpc / peak_wr_bpc, 1) if wr_bpc else "",
        **{name: perf.get(name, "") for name in PERF_COUNTERS},
        **{
            name.removesuffix("_cycles") + "_pct_of_core": perf_pct_of_core(name)
            for name in CIM_STALL_COUNTERS
        },
    ))

if not rows:
    sys.exit("no matching logs found for any manifest config")

rows.sort(key=lambda r: (r["sim"], r["layer"], r["macs"], r["config"]))
with open(OUT, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
    w.writeheader()
    w.writerows(rows)
print(f"wrote {OUT} ({len(rows)} rows, "
      f"{len({(r['config'], r['ic_port_width_bits'], r['oc_port_width_bits'], r['cim_mac_latency']) for r in rows})} "
      "hardware points, "
      f"sims={sorted({r['sim'] for r in rows})})")
