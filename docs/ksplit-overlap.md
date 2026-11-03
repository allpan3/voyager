# K-split output ordering and overlap

The first bare matrix partial of an asynchronous K-split reduction now passes
through the vector unit without arithmetic. Its completion gates the next
vector read, while the next matrix pass can overlap the drain. This replaces
the whole-dispatch wait for logical `bfloat16`, `float32`, and `fp8_e4m3` outputs.
Integer and unknown logical types retain the conservative wait.

The compiler emits `output_via_vector` on the matrix operation; the mapper must
honor it. **Integrate both changes together.** An older mapper silently ignores
the marker after the new compiler removes the wait, reintroducing the race.
The updated mapper remains compatible with older compiler output and rejects
marked output types absent from its build's `OUTPUT_DATATYPES`. No RTL, firmware,
proto schema, or tiling-search changes are required.

## Focused regressions

From the accelerator checkout with its normal build environment loaded:

```bash
source ./.envrc
export PYTHONPATH="$PWD/voyager-compiler/src:$PWD/voyager-compiler/test${PYTHONPATH:+:$PYTHONPATH}"
PYTHONDONTWRITEBYTECODE=1 python voyager-compiler/test/test_ksplit_overlap.py -v
make test-output-routing DATATYPE=MXNF4 IC_DIMENSION=64 OC_DIMENSION=64 \
  DOUBLE_BUFFERED_ACCUM_BUFFER=true SUPPORT_MVM=true SUPPORT_SPMM=true CLOCK_PERIOD=5
make test-output-routing DATATYPE=INT8 IC_DIMENSION=16 OC_DIMENSION=16 \
  DOUBLE_BUFFERED_ACCUM_BUFFER=false SUPPORT_MVM=false SUPPORT_SPMM=false CLOCK_PERIOD=5
```

The eight compiler tests cover 2/3/4/6 reduction rounds, reused 3 x 3 output grids,
bare and fused tails, convolution aliases, logical datatype selection, FP32
scratch widening, leading dequantize, unsplit/synchronous paths, GEMV, and
integer/unknown fallback. Floating cases check eager values and emitted IR.
The fallback checks the real kernel's semaphore dependency directly: Torch
2.12 rejects aliased semaphore views during symbolic export on the unchanged
base as well. The mapper test checks absent/false markers, vector routing and
output destination, and rejection of an unsupported marked integer output.
These tests do not require RTL simulation; the mapper build needs Catapult's
C++ compiler and SystemC library.

## Completed RTL validation

On 2026-11-02, nine complete MXNF4 64 x 64 cases at 5 ns passed with 2 MiB
scratchpad, `MAX_TILES=0`, and `RTOL=ATOL=0`: **141 matrix dispatches and 196,608
final outputs, all exact**. Interstellar selects L1 mappings in every case.

| Case | K rounds | Tile M x K x N | Output tiles |
|---|---:|---|---:|
| Automatic unsplit | 1 | 128 x 512 x 128 | 1 |
| Two-round grid | 2 | 64 x 256 x 64 | 8 |
| Three-round grid, two input seeds | 3 | 64 x 256 x 64 | 6 each |
| FP8 output | 4 | 64 x 256 x 64 | 4 |
| INT4 input | 4 | 64 x 256 x 64 | 4 |
| Wide output | 4 | 64 x 256 x 128 | 4 |
| Fused ReLU | 8 | 64 x 128 x 64 | 4 |
| Automatic large K | 8 | 128 x 1024 x 128 | 1 |

The automatic cases leave tile selection unconstrained. Other cases constrain
M/N/K tiles to exercise different reduction depths and buffer reuse. Unless
specified otherwise, inputs are INT6, scales are FP8 E5M3 in blocks of 64, and
outputs are BF16. Independent integer-dot/BF16-rounding reconstruction matches
all 180,224 outputs of the eight BF16 cases. FP8 output matches hardware gold.

Removing only the routing marker from the passing three-round case, without
restoring the wait, produces **3,452 wrong final outputs out of 24,576**, plus
576 scratch mismatches. The same 18 matrix dispatches and input files pass
with the fix. The RTL wrapper returns zero even for this negative control;
validation must check `Error count: 0` and full dispatch/output coverage.

Raw results and reproduction scripts remain local under
`build/.checks/ksplit-overlap/`: `tiling-suite-verification.json`,
`tiling-independent-math.json`, `tiling-negative-control.json`, and the
corresponding `compile_tiling_suite.py`, `run_tiling_suite.py`,
`verify_tiling_suite.py`, and `check_tiling_math.py`. They are investigation
artifacts, not files supplied by this repository. The focused regressions above
are tracked and passed again during PR preparation in both mapper builds.
The completed RTL suite was not repeated.

## Historical performance and limits

For the complete BF16 P@V operation (M=1024, K=1024, N=128), current automatic
unsplit tiling takes 172.2 us in RTL. The pre-migration stack forced to that same
tiling reproduces all eight completion timestamps and all 131,072 outputs.
Its old search did not select that tiling. The unsplit case does not use this
fix, so its speed is not an improvement caused by K-split routing.

With identical K-split tiles (512 x 512 x 128), L1 mapping, inputs, and RTL,
current matrix stages take 193.390 us versus historical 222.885 us: 13.2% less
active RTL time, with exact output agreement. Historical input slicing is
excluded because the old vector copy path cannot handle INT6 input. This is
not a full-network comparison. Fast SystemC absolute timing is not hardware
performance evidence.

Full-SoC write-response visibility remains unvalidated: unit completion is not
a write acknowledgement. A cycle-accurate SystemC E115 conflict also reproduces
on the base; the reported hardware checks use RTL. These results support the
tested schedules, not a claim that every workload reaches peak hardware speed.
