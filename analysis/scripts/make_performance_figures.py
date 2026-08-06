#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib>=3.8"]
# ///
"""Generate paper-quality RTL performance figures and supporting tables.

    analysis/scripts/make_performance_figures.py
    analysis/scripts/make_performance_figures.py --sweep b-sets
    analysis/scripts/make_performance_figures.py --sweep result-slots --only mobilebert_ffn_output
    analysis/scripts/make_performance_figures.py --only performance_resnet18_layer4_conv2_b_sets
    analysis/scripts/make_performance_figures.py --style slide --formats png

Both controlled-variable figure families read the same parsed sweep summary:
one varies result slots and one varies B-set count. Figures use vector SVG by
default; PNG and PDF are opt-in. Tables use CSV, Markdown, and booktabs LaTeX.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

ANALYSIS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from perf_figs import charts, style as S, tables  # noqa: E402
from perf_figs.data import (  # noqa: E402
    DEFAULT_PD_CONFIG,
    DEFAULT_PD_PORTS,
    DEFAULT_SUMMARY,
    LAYER_STEMS,
    geomean_speedup,
    load,
    point,
    reference,
    speedups,
)

FIGURE_KINDS = {
    "result-slots": ("result_slots", charts.fig_workload_result_slots),
    "b-sets": ("b_sets", charts.fig_workload_b_sets),
}


# Parse the reproducible figure-generation command line
def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--summary", type=Path, default=DEFAULT_SUMMARY)
    parser.add_argument("--outdir", type=Path, default=ANALYSIS_DIR / "figures")
    parser.add_argument("--style", choices=("paper", "slide", "both"), default="paper")
    parser.add_argument("--sweep", choices=("result-slots", "b-sets", "both"), default="both")
    parser.add_argument(
        "--formats",
        default="svg",
        help="comma-separated output formats (default: svg; add png or pdf explicitly)",
    )
    parser.add_argument(
        "--only",
        default="",
        help="comma-separated workload stems, layer IDs, or full figure names",
    )
    parser.add_argument("--no-tables", action="store_true")
    parser.add_argument("--pd-config", default=DEFAULT_PD_CONFIG)
    parser.add_argument("--pd-input-bits", type=int, default=DEFAULT_PD_PORTS[0])
    parser.add_argument("--pd-output-bits", type=int, default=DEFAULT_PD_PORTS[1])
    return parser.parse_args(argv)


# Resolve --only aliases into deterministic sweep/workload pairs
def _selected_figures(study, only: str, sweep: str) -> list[tuple[str, str]]:
    kinds = tuple(FIGURE_KINDS) if sweep == "both" else (sweep,)
    requested = [value.strip() for value in only.split(",") if value.strip()]
    if not requested:
        return [(kind, layer) for kind in kinds for layer in study.layers]

    aliases = {}
    for layer in study.layers:
        stem = LAYER_STEMS[layer]
        aliases[layer] = (None, layer)
        aliases[stem] = (None, layer)
        for kind, (suffix, _builder) in FIGURE_KINDS.items():
            aliases[f"performance_{stem}_{suffix}"] = (kind, layer)

    unknown = sorted(set(requested) - set(aliases))
    if unknown:
        raise SystemExit(f"unknown workload figure(s): {', '.join(unknown)}")

    selected = set()
    for value in requested:
        kind, layer = aliases[value]
        if kind is None:
            selected.update((candidate, layer) for candidate in kinds)
        elif kind not in kinds:
            raise SystemExit(f"{value} is excluded by --sweep {sweep}")
        else:
            selected.add((kind, layer))
    return [
        (kind, layer)
        for kind in kinds
        for layer in study.layers
        if (kind, layer) in selected
    ]


# Write reusable performance and workload-shape tables
def _write_tables(study, outdir: Path) -> list[Path]:
    artifacts = []
    content = {
        "performance_rankings.csv": tables.rankings_csv(study),
        "performance_rankings.md": tables.rankings_markdown(study),
        "performance_rankings.tex": tables.rankings_latex(study),
        "performance_pd_rslots8_workloads.csv": tables.pd_workloads_csv(study),
        "performance_pd_rslots8_workloads.md": tables.pd_workloads_markdown(study),
        "performance_pd_rslots8_workloads.tex": tables.pd_workloads_latex(study),
        "workload_operation_shapes.csv": tables.operation_shapes_csv(study),
        "workload_operation_shapes.md": tables.operation_shapes_markdown(study),
        "workload_operation_shapes.tex": tables.operation_shapes_latex(study),
    }
    for name, content_text in content.items():
        path = outdir / name
        path.write_text(content_text, encoding="utf-8")
        artifacts.append(path)
    return artifacts


# Generate both CSV-backed performance families and print a measured summary
def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    study = load(
        args.summary,
        pd_config=args.pd_config,
        pd_ports=(args.pd_input_bits, args.pd_output_bits),
    )
    args.outdir.mkdir(parents=True, exist_ok=True)
    formats = [value.strip() for value in args.formats.split(",") if value.strip()]
    figures = _selected_figures(study, args.only, args.sweep)
    styles = ("paper", "slide") if args.style == "both" else (args.style,)

    written: list[Path] = []
    for style_name in styles:
        style = S.apply(S.STYLES[style_name])
        for kind, layer in figures:
            suffix, builder = FIGURE_KINDS[kind]
            name = f"performance_{LAYER_STEMS[layer]}_{suffix}"
            figure = builder(study, style, layer)
            for output_format in formats:
                path = args.outdir / f"{name}_{style_name}.{output_format}"
                figure.savefig(path, format=output_format, bbox_inches=None)
                written.append(path)
        operation_shapes = charts.fig_operation_shapes(study, style)
        for output_format in formats:
            path = args.outdir / f"workload_operation_shapes_{style_name}.{output_format}"
            operation_shapes.savefig(path, format=output_format, bbox_inches=None)
            written.append(path)
    if not args.no_tables:
        written.extend(_write_tables(study, args.outdir))

    pd = point(study, study.pd_key)
    pd_reference = reference(study, pd)
    pd_speedups = speedups(study, pd)
    print(
        f"validated {len(study.points)} hardware points x {len(study.layers)} workloads; "
        f"PD={pd.config} ports={pd.input_port_bits}/{pd.output_port_bits} rslots={pd.slots}"
    )
    print(
        f"PD vs {pd_reference.config}: geomean={geomean_speedup(study, pd):.3f}x, "
        f"range={min(pd_speedups.values()):.3f}-{max(pd_speedups.values()):.3f}x"
    )
    if args.sweep in ("b-sets", "both"):
        _baseline, b_set_family = charts.b_set_comparison_family(study)
        print(
            f"B-set sweep: rslots={pd.slots}, "
            f"B sets={'/'.join(str(design.sets) for design in b_set_family)}"
        )
    print(f"wrote {len(written)} files to {args.outdir}")
    for path in written:
        print(f"  {path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
