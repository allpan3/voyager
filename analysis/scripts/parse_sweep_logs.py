#!/usr/bin/env python3
"""Parse one sweep directory into a flat CSV of per-run metrics.

Usage: parse_sweep_logs.py <sweep_dir> <out_csv>

Config metadata comes entirely from <sweep_dir>/manifest.csv (emitted by
matrix_backend_sweep.py), so nothing about the design space is hardcoded here.
Log files are named
    <config>[_pw<bits>]__<layer>.log         SystemC functional run
    <config>[_pw<bits>]__rtl__<layer>.log    RTL cosim (cycle-accurate + counters)
One CSV row per (config, port width, sim, layer).
"""
import csv, os, re, sys

SWEEP_DIR = sys.argv[1]
OUT = sys.argv[2]

# MatrixPerfHardware counters carried through to the CSV (present in RTL logs)
PERF_COUNTERS = [
    "processor_active_cycles", "array_resident_cycles", "array_issue_cycles",
    "input_unavailable_cycles", "input_backpressure_cycles",
    "weight_unavailable_cycles", "weight_backpressure_cycles",
    "result_backpressure_cycles", "accumulation_stall_cycles",
    "output_backpressure_cycles", "output_fifo_full_cycles",
    "mac_wait_weight_set_load_cycles", "cim_completion_storage_stall_cycles",
    "cim_result_slot_stall_cycles",
    "cim_completion_descriptor_stall_cycles",
]
CIM_STALL_COUNTERS = [
    "mac_wait_weight_set_load_cycles",
    "cim_completion_storage_stall_cycles",
    "cim_result_slot_stall_cycles",
    "cim_completion_descriptor_stall_cycles",
]


# Return ceiling division for positive hardware geometry values
def ceil_div(dividend, divisor):
    return (dividend + divisor - 1) // divisor


# Derive the exact cycles between logical MAC issues from CIM parameters
def cim_mac_issue_cycles(meta):
    if meta["backend"] != "cim":
        return 1
    logical_a_width = meta["logical_a_width_bits"]
    base_a_width = meta["base_a_width_bits"]
    if meta["mode"] == "0":
        return ceil_div(logical_a_width, base_a_width)
    guard_width = max(1, (meta["ch_in_value"] - 1).bit_length())
    serial_max_slice_width = meta["base_c_width_bits"] - meta["base_b_width_bits"] - guard_width
    if serial_max_slice_width <= 0:
        raise ValueError(f"{meta['config']} has no legal bit-serial A slice")
    serial_slice_width = min(logical_a_width, serial_max_slice_width)
    num_slices = ceil_div(logical_a_width, serial_slice_width)
    slice_interval = ceil_div(serial_slice_width, base_a_width) * base_a_width
    return num_slices * slice_interval


manifest_path = os.path.join(SWEEP_DIR, "manifest.csv")
if not os.path.exists(manifest_path):
    sys.exit(f"no manifest at {manifest_path}")

workload_path = os.path.join(SWEEP_DIR, "workloads.csv")
WORKLOAD_NETWORK = {}
if os.path.exists(workload_path):
    for workload in csv.DictReader(open(workload_path)):
        WORKLOAD_NETWORK[workload["layer"]] = workload["network"]

CFG = {}
for m in csv.DictReader(open(manifest_path)):
    is_cim = m["backend"] == "1"
    clock_period = m.get("clock_period_ns") or os.environ.get("CLOCK_PERIOD")
    if not clock_period:
        sys.exit(
            "manifest lacks clock_period_ns; set CLOCK_PERIOD when parsing this "
            "legacy sweep"
        )
    # macro_native_width_bits was called cell_bits before the terminology was
    # settled; read either so older sweep directories still parse.
    native = m.get("macro_native_width_bits") or m.get("cell_bits") or ""
    ic_matched = m.get("ic_matched_port_bits")
    oc_matched = m.get("oc_matched_port_bits")
    if not ic_matched or not oc_matched:
        sys.exit(f"{m['config']} lacks matched port widths")
    logical_a_width = int(m.get("logical_a_width_bits") or int(ic_matched) // int(m["K"]))
    base_a_width = m.get("macro_base_a_width_bits") or native
    base_b_width = m.get("macro_base_b_width_bits") or native
    base_c_width = m.get("macro_base_c_width_bits")
    if is_cim and (not base_a_width or not base_b_width):
        sys.exit(f"{m['config']} lacks CIM base A/B widths")
    if is_cim and (m.get("cim_mode") or "0") == "1" and not base_c_width:
        sys.exit(f"{m['config']} lacks CIM base C width required for bit-serial normalization")
    CFG[m["config"]] = dict(
        config=m["config"],
        backend="cim" if is_cim else "systolic",
        K=int(m["K"]), N=int(m["N"]),
        # One matched port beat carries one logical array row or column
        ic_matched=int(ic_matched),
        oc_matched=int(oc_matched),
        logical_a_width_bits=logical_a_width,
        base_a_width_bits=int(base_a_width) if is_cim else None,
        base_b_width_bits=int(base_b_width) if is_cim else None,
        base_c_width_bits=int(base_c_width) if (is_cim and base_c_width) else None,
        # The array every point is judged against. Absent in older manifests;
        # the workbook falls back to inferring it from the systolic rows.
        baseline_geometry=m.get("baseline_geometry") or "",
        technology=m.get("technology") or "",
        datatype=m.get("datatype") or "",
        clock_period_ns=float(clock_period),
        native_width=(native + "b") if (is_cim and native) else "",
        native_width_bits=int(native) if (is_cim and native) else None,
        mac_latency=(m.get("cim_mac_latency") or "1") if is_cim else "",
        mode=(m.get("cim_mode") or "0") if is_cim else "",
        b_sets=(m.get("cim_b_sets") or "2") if is_cim else "",
        result_slots=(m.get("cim_result_slots_per_output_lane") or "") if is_cim else "",
        ch_in=m["ch_in"], ch_out=m["ch_out"],
        ch_in_value=int(m["ch_in"]) if is_cim else None,
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
    # Treat workloads.csv as the reportable workload allowlist
    if WORKLOAD_NETWORK and layer not in WORKLOAD_NETWORK:
        continue
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

    def grab_number(pat):
        g = re.search(pat, txt, re.M | re.I)
        return float(g.group(1)) if g else None

    def grab_numbers(pat):
        return [float(value) for value in re.findall(pat, txt, re.M | re.I)]

    ok = bool(re.search(r"Error\s+count:\s+0\b", txt))
    number = r"([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)"
    total_ns = grab_number(rf"^Total Runtime:\s+{number}\s*ns")
    matrix_unit_values = grab_numbers(rf"^Matrix Unit Runtime:\s+{number}\s*ns")
    matrix_unit_ns = sum(matrix_unit_values) if matrix_unit_values else None
    ideal_cycle_values = grab_numbers(
        r"matrix unit ideal cycles:\s+(\d+)"
    )
    ideal_runtime_values = grab_numbers(rf"matrix unit ideal runtime:\s+{number}\s*ns")
    # SCVerify setup may repeat ideal metrics before the measured RTL intervals
    interval_count = len(matrix_unit_values)
    ideal_cycles_logged = (
        sum(ideal_cycle_values[-interval_count:])
        if interval_count and ideal_cycle_values else None
    )
    ideal_ns = (
        sum(ideal_runtime_values[-interval_count:])
        if interval_count and ideal_runtime_values else None
    )
    rd = grab(r"^harness:\s+(\d+)") or 0
    wr = grab(r"^harness_outputs:\s+(\d+)") or 0
    perf = {}
    pm = re.search(r"^MatrixPerfHardware:(.*)$", txt, re.M)
    if pm:
        perf = dict(kv.split("=") for kv in pm.group(1).split())
        if "processor_active_cycles" not in perf and "core_cycles" in perf:
            perf["processor_active_cycles"] = perf["core_cycles"]
        if "cim_completion_storage_stall_cycles" not in perf:
            perf["cim_completion_storage_stall_cycles"] = perf.get(
                "cim_completion_queue_stall_cycles",
                perf.get("cim_array_credit_stall_cycles", ""),
            )

    # Normalize one synthesized stall counter against its snapshot duration
    def perf_pct_of_processor(name):
        try:
            return round(
                100 * int(perf[name]) / int(perf["processor_active_cycles"]), 1
            )
        except (KeyError, TypeError, ValueError, ZeroDivisionError):
            return ""

    # Only passing RTL cosimulation is performance-reportable
    clock_ns = meta["clock_period_ns"]
    freq_hz = 1e9 / clock_ns
    cyc = total_ns / clock_ns if (sim == "rtl" and ok and total_ns) else None
    matrix_unit_cyc = (
        matrix_unit_ns / clock_ns
        if (sim == "rtl" and ok and matrix_unit_ns)
        else None
    )
    if sim == "rtl" and ok:
        raw_ideal_cyc = (
            ideal_cycles_logged
            if ideal_cycles_logged is not None
            else ideal_ns / clock_ns if ideal_ns is not None else None
        )
    else:
        raw_ideal_cyc = None
    mac_issue_cycles = cim_mac_issue_cycles(meta)
    ideal_cyc = raw_ideal_cyc * mac_issue_cycles if raw_ideal_cyc is not None else None
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
        clock_period_ns=clock_ns,
        technology=meta["technology"],
        backend=meta["backend"], geometry=f"{ic}x{oc}", IC=ic, OC=oc, macs=ic * oc,
        baseline_geometry=meta["baseline_geometry"],
        datatype=meta["datatype"],
        cim_macro_native_width=meta["native_width"],
        cim_mac_latency=meta["mac_latency"],
        cim_mode="bit-serial" if meta["mode"] == "1" else ("bit-parallel" if meta["mode"] == "0" else ""),
        cim_b_sets=meta["b_sets"],
        cim_result_slots_per_output_lane=meta["result_slots"],
        cim_ch_in=meta["ch_in"], cim_ch_out=meta["ch_out"],
        input_axis_tiles=meta["input_axis_tiles"],
        output_axis_tiles=meta["output_axis_tiles"],
        cim_tile_input_axis_elements=meta["tile_in_elems"],
        cim_tile_output_axis_elements=meta["tile_out_elems"],
        network=WORKLOAD_NETWORK.get(layer, ""), layer=layer, passed=ok,
        runtime_cycles=round(cyc) if cyc else "",
        runtime_us=round(total_ns / 1000.0, 3) if cyc else "",
        matrix_unit_cycles=round(matrix_unit_cyc) if matrix_unit_cyc else "",
        raw_ideal_cycles=round(raw_ideal_cyc) if raw_ideal_cyc else "",
        logical_a_width_bits=meta["logical_a_width_bits"],
        logical_mac_issue_cycles=mac_issue_cycles if raw_ideal_cyc else "",
        ideal_cycles=round(ideal_cyc) if ideal_cyc else "",
        matrix_utilization=round(ideal_cyc / matrix_unit_cyc, 4)
        if (ideal_cyc and matrix_unit_cyc) else "",
        ext_read_bytes=rd, ext_write_bytes=wr,
        ext_read_GBps=round(rd_bpc * freq_hz / 1e9, 2) if rd_bpc else "",
        ext_write_GBps=round(wr_bpc * freq_hz / 1e9, 2) if wr_bpc else "",
        peak_read_GBps=round(peak_rd_bpc * freq_hz / 1e9, 2),
        peak_write_GBps=round(peak_wr_bpc * freq_hz / 1e9, 2),
        read_bw_pct_of_peak=round(100 * rd_bpc / peak_rd_bpc, 1) if rd_bpc else "",
        write_bw_pct_of_peak=round(100 * wr_bpc / peak_wr_bpc, 1) if wr_bpc else "",
        **{name: perf.get(name, "") for name in PERF_COUNTERS},
        **{
            name[:-len("_cycles")] + "_pct_of_processor": perf_pct_of_processor(name)
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
