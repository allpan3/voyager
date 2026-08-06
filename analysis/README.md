# CIM versus systolic analysis

This directory contains generated analysis workbooks, reusable plotting modules, and generated artifacts. Runnable
entry points live together under `scripts/`.

```text
analysis/
├── scripts/       runnable generators and their private support modules
├── figures/       generated paper and slide artifacts
└── *.xlsx         canonical and supporting workbooks
```

## Area comparison figures (SA vs CIM, 64x64)

`scripts/make_area_figures.py` turns `matrixunit_sa_vs_cim_64x64_area_comparison_1ns.xlsx` into paper-quality figures and
tables. Matplotlib is the tool of choice here: it emits real vector output with embedded Type-42 fonts, which is
what a LaTeX submission wants, and the same code re-typesets for slides.

```bash
uv run analysis/scripts/make_area_figures.py
```

No environment setup is needed — the script carries [PEP 723](https://peps.python.org/pep-0723/) inline
dependencies, so `uv` builds a throwaway venv on the fly. Everything lands in `analysis/figures/`.

Three figures, each in two presets (`{name}_r8_{paper,slide}.svg`):

| Figure | What it shows |
| --- | --- |
| `area_stacked` | Absolute stacked breakdown, totals, and the reduction bracket. The headline figure. |
| `area_ratio` | SA/CIM area reduction per component, as a log-scale dot plot growing from parity. |
| `area_table` | The breakdown typeset as a booktabs-style table, rendered as a figure. |

`area_stacked` and `area_ratio` each carry their own title and are sized to sit side by side: heights match, but
each gets the width its form wants (a two-column bar chart is narrow, a six-row dot plot is wide), and the widths
sum to just inside a 9.30 in slide content box. Pass `title=None` to a builder for an untitled figure.

Presets: `paper` is a single-column IEEE/ACM figure in Times at 8 pt; `slide` is the same chart in
Helvetica at 13 pt for projection. Pick with `--style paper|slide|both`, and `--rslots 2|6|8` for a different
CIM point (the workbook holds all three; 8 is the default). `--formats` defaults to `svg`; ask for `pdf`
(LaTeX) or `png` (slides, quick looks) explicitly, e.g. `--formats svg,pdf`.

Two deliberate presentation choices: the result-slot count is **not** printed on any chart, table, or slide —
it is only in the filenames, so it can be stated verbally — and ratios are reported one way round only, as
**SA/CIM** ("CIM is N times smaller").

The same numbers are also written as `area_table_r8.{tex,md,csv,txt}` — `.tex` is booktabs and drops straight
into a paper.

### How the numbers are derived

`scripts/area_figs/data.py` is the single source of truth; figures, tables, and slides all read it, so a number in a
plot cannot drift from the same number in a table. A MatrixUnit DC report counts its compute-array child once as
a reduced block abstraction, so the full-design area is `parent report - embedded abstraction + standalone
mapped child`, applied recursively (4,096 ProcessingElements for SA, one CIMArray for CIM). Every derived value
is cross-checked against the workbook's own `Comparison` sheet on load and raises `WorkbookMismatch` if it
disagrees.

## RTL performance figures (PD-family CIM versus SA)

`scripts/make_performance_figures.py` is the single generator for the two RTL performance figure families. It reads
the completed sweep `summary.csv`, validates exactly 71 hardware points and 568 results across the eight declared
matrix workloads, and emits two independently readable paper figures per workload in vector SVG:

```bash
uv run analysis/scripts/make_performance_figures.py
```

Generate only the B-set comparison figures from the same parsed sweep summary with:

```bash
uv run analysis/scripts/make_performance_figures.py \
  --sweep b-sets --style both --no-tables
```

| Workload stem | Workload |
| --- | --- |
| `mobilebert_ffn_output` | MobileBERT FFN output |
| `mobilebert_bottleneck` | MobileBERT output bottleneck |
| `mobilebert_attention_output` | MobileBERT attention output |
| `mobilebert_attention_context` | MobileBERT attention context matmul |
| `mobilebert_attention_scores` | MobileBERT attention scores matmul |
| `resnet18_layer1_conv1` | ResNet-18 layer 1 conv1 |
| `resnet18_layer2_conv1` | ResNet-18 layer 2 conv1 |
| `resnet18_layer4_conv2` | ResNet-18 layer 4 conv2 |

Each workload has two controlled-variable figures:

| Filename suffix | Controlled variable | Fixed CIM value | CIM bars |
| --- | --- | --- | --- |
| `_result_slots` | Result slots per output lane | 18 B sets | rslots 1, 2, 6, and 8 |
| `_b_sets` | B-set count | rslots 8 | 2, 8, and 18 B sets |

Every chart uses raw end-to-end RTL cycles (lower is better) and compares the matched `sa_64x64` 512/512-bit
baseline with one exact CIM family: 64x64 array, 512-bit input/output ports, native 8-bit bit-parallel macros,
`CH_IN/CH_OUT=64/8`, MAC latency 3, INT8, tsmc7, and 2 ns. The result-slot charts fix 18 B sets. The B-set charts
fix rslots at 8 because the 2-set, 8-set, and 18-set configurations have source-matched rslots=8 completions; no
lower common slot count is needed. All fixed parameters and the sole controlled variable are printed on every
chart so each figure remains self-contained when copied into a paper or slide.

The SA bar is labeled only with its measured cycle count; it has no meaningless SA-versus-SA speedup. Each CIM
bar reports its measured cycles and speedup relative to SA. The 18-set, rslots=8 CIM bar is the physical-design
point.

The default PD point is the 512-bit instance of
`cim8b8b_64x64_co8_ci64_sets18_rslots8`. Override `--pd-config`, `--pd-input-bits`, and `--pd-output-bits` only
when intentionally studying another physical-design target. `--style slide` re-typesets the same plots for a
presentation. `--sweep result-slots|b-sets|both` selects the controlled-variable family; both are generated by default.
`--only` accepts a workload stem (for example, `mobilebert_ffn_output`), an exact layer ID, or a full figure name;
`--formats` selects output formats. The default is `svg`; PNG and PDF are opt-in, for example with
`--formats pdf,png,svg`.

The generator also writes `performance_rankings.{csv,md,tex}` and the exact PD workload breakdown as
`performance_pd_rslots8_workloads.{csv,md,tex}`.
End-to-end speedup is the closest available SA reference's fused-layer `runtime_cycles` divided by the CIM point's
`runtime_cycles`; the eight-workload aggregate is a geometric mean. MatrixUnit utilization and each stall counter
remain separate measured quantities. Stall counters can overlap and are never summed.

### Workload operation shapes

The same command writes `workload_operation_shapes.{csv,md,tex}` and a typeset
`workload_operation_shapes_{paper,slide}.svg` table by default. It reports the logical input, weight/right-hand-side,
and output tensor shapes, the equivalent `M×K × K×N` GEMM, and the logical MAC count for every measured layer.
For convolutions, `M` is the number of output spatial positions, `K` is kernel height × kernel width × input
channels, and `N` is output channels. Shapes intentionally exclude the compiler's extra fused bias/padding lane;
the MAC counts are cross-checked against the measured 64×64 SA ideal-cycle metadata.

## Area-normalized RTL performance

`scripts/make_area_normalized_performance.py` is deliberately separate because it joins two source artifacts: measured
RTL cycles from `summary.csv` and hierarchy-corrected area from
`matrixunit_sa_vs_cim_64x64_area_comparison_1ns.xlsx`. It compares the SA 64×64 baseline with the single fixed CIM
physical-design point:

```bash
uv run analysis/scripts/make_area_normalized_performance.py
```

The generator writes `area_normalized_performance.{csv,md,tex}` and
`area_normalized_performance_{paper,slide}.svg`. The long-form CSV is a published result table, not an intermediate
input; it retains cycles, time, area, absolute throughput, absolute throughput density, speedup, and throughput density
relative to SA for every workload.

For one fixed workload and the shared 2 ns clock:

```text
throughput [GMAC/s] = logical MACs / runtime [ns]
throughput density [GMAC/s/mm²] = throughput / hierarchy-corrected MatrixUnit area [mm²]
density relative to SA = (SA cycles / design cycles) × (SA area / design area)
```

This is end-to-end fused-layer throughput density, not isolated array occupancy: `runtime_cycles` includes the
measured data movement, control, and fused work in the RTL interval. Area is the workbook's hierarchy-corrected
full-design cell area, including SRAM and the supplied CIM macro area but excluding routing/interconnect area.

### Data flow

`analysis/scripts/parse_sweep_logs.py` is the only log parser. It reads the manifest, workload declaration, and retained
RTL simulation logs and writes `summary.csv`. The performance generator reads that CSV directly for both controlled-
variable figure families. The area-normalized generator reads the same CSV plus the area workbook. Neither plotting
script dumps or reparses an intermediate CSV; their CSV outputs are final reusable tables generated from the same
validated in-memory objects as the figures.

## Comparison workbook

The current comparison uses 71 hardware points and eight matrix-unit workloads from:

```text
cmp_results/runs/019fc8ce-df84-7d40-be91-180a41d1ff07/sweep
```

Reusable Catapult and SCVerify state stays under `build/`. New run directories under `cmp_results/runs/` contain the
cross-build manifests, resume stamps, captured console logs, and parsed results consumed here; `analysis/` contains the
scripts and curated workbooks, figures, and tables derived from those records. Legacy `*_hls` report copies have been
removed, and the sweep no longer creates or consumes them.

`fc_1` is retained in the sweep logs for provenance but excluded from the workbook because it uses vector and
reduction instructions rather than the matrix unit.

Result-slot counts are explicit sweep inputs, not values derived from a sufficiency equation. Parallel native-8
families include slots=8 and parallel native-4 families include slots=6, while previously completed comparison counts
remain in the design space. The workbook makes no general capacity claim for a slot count; use the measured result-slot
and completion-storage stall counters for each hardware and workload combination.

## Reproducible workbook

Python owns log parsing and the canonical workbook schema, formulas, tables, formatting, and methodology. The
required `openpyxl` dependency is declared in `environment.yml`.

From the repository root:

```bash
SWEEP=cmp_results/runs/019fc8ce-df84-7d40-be91-180a41d1ff07/sweep
python3 analysis/scripts/parse_sweep_logs.py "$SWEEP" "$SWEEP/summary.csv"
python3 analysis/scripts/build_comparison_workbook.py \
  "$SWEEP/summary.csv" analysis/sa_cim_rtl_performance_comparison.xlsx
```

The parser reads `manifest.csv`, `workloads.csv`, and the simulation logs. The workbook generator reads the sibling
`workloads.csv` when present to preserve the declared workload order. This path does not require Codex or OpenAI
software.
