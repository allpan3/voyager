#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib>=3.8", "openpyxl>=3.1"]
# ///
"""Generate the MatrixUnit SA-vs-CIM area figures and tables.

    uv run analysis/scripts/make_area_figures.py                    # paper + slide, SVG
    uv run analysis/scripts/make_area_figures.py --formats svg,pdf  # add vector PDF for LaTeX
    uv run analysis/scripts/make_area_figures.py --style slide      # 16:9 sans-serif only
    uv run analysis/scripts/make_area_figures.py --rslots 6         # a different CIM point

Output lands in ``analysis/figures/``. SVG by default; ask for ``pdf`` (LaTeX)
or ``png`` (slides, quick looks) explicitly. Tables are always written as
LaTeX/Markdown/CSV/text alongside the figures.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

ANALYSIS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from area_figs import charts, style as S, tables  # noqa: E402
from area_figs.data import DEFAULT_WORKBOOK, load  # noqa: E402

FIGURES = {
    "area_stacked": (charts.fig_stacked, "absolute stacked breakdown"),
    "area_ratio": (charts.fig_ratio, "SA/CIM area-reduction dot plot"),
    "area_table": (charts.fig_table, "typeset breakdown table"),
}


# Parse the area-figure command line
def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--workbook", type=Path, default=DEFAULT_WORKBOOK)
    p.add_argument("--outdir", type=Path, default=ANALYSIS_DIR / "figures")
    p.add_argument("--rslots", type=int, default=8, choices=(2, 6, 8))
    p.add_argument(
        "--style",
        choices=("paper", "slide", "both"),
        default="both",
        help="paper = single-column serif; slide = 16:9 sans-serif",
    )
    p.add_argument(
        "--formats",
        default="svg",
        help="comma-separated output formats (default: svg; add png or pdf explicitly)",
    )
    p.add_argument("--only", default="", help="comma-separated figure names to build")
    p.add_argument("--no-tables", action="store_true")
    return p.parse_args(argv)


# Generate the requested area figures and tables
def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    model = load(args.workbook, rslots=args.rslots)
    args.outdir.mkdir(parents=True, exist_ok=True)

    formats = [f.strip() for f in args.formats.split(",") if f.strip()]
    wanted = [n.strip() for n in args.only.split(",") if n.strip()] or list(FIGURES)
    unknown = set(wanted) - set(FIGURES)
    if unknown:
        raise SystemExit(f"unknown figure(s): {', '.join(sorted(unknown))}")
    styles = ("paper", "slide") if args.style == "both" else (args.style,)

    written: list[Path] = []
    for style_name in styles:
        st = S.apply(S.STYLES[style_name])
        for name in wanted:
            builder, _ = FIGURES[name]
            fig = builder(model, st)
            for fmt in formats:
                path = args.outdir / f"{name}_r{model.rslots}_{style_name}.{fmt}"
                fig.savefig(path, format=fmt, bbox_inches=None)
                written.append(path)
            fig.clear()

    if not args.no_tables:
        stem = f"area_table_r{model.rslots}"
        for suffix, text in (
            ("tex", tables.to_latex(model)),
            ("md", tables.to_markdown(model)),
            ("csv", tables.to_csv(model)),
            ("txt", tables.to_text(model)),
        ):
            path = args.outdir / f"{stem}.{suffix}"
            path.write_text(text, encoding="utf-8")
            written.append(path)
        print(tables.to_text(model))

    print(f"wrote {len(written)} files to {args.outdir}")
    for path in written:
        print(f"  {path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
