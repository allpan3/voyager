#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib>=3.8", "openpyxl>=3.1"]
# ///
"""Generate area-normalized RTL performance for the physical-design point.

    analysis/scripts/make_area_normalized_performance.py
    analysis/scripts/make_area_normalized_performance.py --style both --formats svg,png,pdf

The sweep summary supplies measured RTL cycles and operation counts. The area
workbook supplies hierarchy-corrected MatrixUnit areas. No intermediate CSV is
read: the output CSV is a published result table from the same in-memory model.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

ANALYSIS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from perf_figs import area_normalized, style as S  # noqa: E402
from perf_figs.data import (  # noqa: E402
    DEFAULT_PD_CONFIG,
    DEFAULT_PD_PORTS,
    DEFAULT_SUMMARY,
    load,
)


# Parse the standalone area-normalized performance command line
def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--summary", type=Path, default=DEFAULT_SUMMARY)
    parser.add_argument(
        "--area-workbook",
        type=Path,
        default=area_normalized.DEFAULT_AREA_WORKBOOK,
    )
    parser.add_argument("--outdir", type=Path, default=ANALYSIS_DIR / "figures")
    parser.add_argument("--style", choices=("paper", "slide", "both"), default="paper")
    parser.add_argument(
        "--formats",
        default="svg",
        help="comma-separated output formats (default: svg; add png or pdf explicitly)",
    )
    parser.add_argument("--no-tables", action="store_true")
    parser.add_argument("--pd-config", default=DEFAULT_PD_CONFIG)
    parser.add_argument("--pd-input-bits", type=int, default=DEFAULT_PD_PORTS[0])
    parser.add_argument("--pd-output-bits", type=int, default=DEFAULT_PD_PORTS[1])
    return parser.parse_args(argv)


# Write the long-form and paper-ready throughput-density tables
def _write_tables(study, outdir: Path, area_workbook: Path) -> list[Path]:
    content = {
        "area_normalized_performance.csv": area_normalized.area_normalized_performance_csv(
            study, area_workbook
        ),
        "area_normalized_performance.md": area_normalized.area_normalized_performance_markdown(
            study, area_workbook
        ),
        "area_normalized_performance.tex": area_normalized.area_normalized_performance_latex(
            study, area_workbook
        ),
    }
    written = []
    for name, content_text in content.items():
        path = outdir / name
        path.write_text(content_text, encoding="utf-8")
        written.append(path)
    return written


# Generate the area-normalized figure and tables from their two source artifacts
def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    study = load(
        args.summary,
        pd_config=args.pd_config,
        pd_ports=(args.pd_input_bits, args.pd_output_bits),
    )
    args.outdir.mkdir(parents=True, exist_ok=True)
    formats = [value.strip() for value in args.formats.split(",") if value.strip()]
    styles = ("paper", "slide") if args.style == "both" else (args.style,)

    written = []
    for style_name in styles:
        style = S.apply(S.STYLES[style_name])
        figure = area_normalized.fig_area_normalized_performance(
            study, style, args.area_workbook
        )
        for output_format in formats:
            path = args.outdir / f"area_normalized_performance_{style_name}.{output_format}"
            figure.savefig(path, format=output_format, bbox_inches=None)
            written.append(path)
    if not args.no_tables:
        written.extend(_write_tables(study, args.outdir, args.area_workbook))

    rows = area_normalized.area_normalized_performance_rows(study, args.area_workbook)
    cim_rows = [row for row in rows if row.result_slots == 8]
    print(
        f"validated {len(study.points)} hardware points x {len(study.layers)} workloads; "
        f"CIM rslots=8 PD throughput density range="
        f"{min(row.throughput_density_vs_sa for row in cim_rows):.3f}-"
        f"{max(row.throughput_density_vs_sa for row in cim_rows):.3f}x SA"
    )
    print(f"wrote {len(written)} files to {args.outdir}")
    for path in written:
        print(f"  {path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
