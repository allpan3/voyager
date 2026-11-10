# Recorded compiler timing

Select a JSON timing file when compiling a model. The compiler reads its
values before mapping search and uses them in the existing timing equations.
Normal compilation does not parse HLS reports or run RTL calibration.

For the existing Make model targets, add this setting to the usual command:

```sh
TIMING_PROFILE=timing/cim-n7-1ns-int8-double-accum.json
```

`double-accum` means `DOUBLE_BUFFERED_ACCUM_BUFFER=true`:
two accumulation SRAM banks alternate between computation and output drain.

Make forwards the file through `--timing_profile`. Direct compiler callers
can pass that argument or set `AcceleratorConfig(timing_profile=path)`.
A missing file or an incomplete record causes an error.

The hardware build, model paths, and loader behavior stay unchanged.
Hardware and build settings in `metadata` describe the measurements.
The compiler does not enforce them as compatibility rules. Select the record
that fits the intended experiment. To try a different latency, copy the JSON,
edit the value and its evidence, then pass that copy. No special override flag
or permanent table entry is needed.

The compiler writes the selected record to `timing_profile.json` beside
`model.txt`. This copy records the values used for search and reporting.
It is not a runtime compatibility gate. Existing Make timestamp behavior
still applies; regenerate a model when changing its compilation settings.

## Record contents

Keep records focused on measured properties that materially affect runtime
estimates or mapping decisions. A measurable delay does not by itself justify
a profile field; the estimator may omit small effects.
The SA record omits small parameter-forwarding, vector-start and completion
corrections. It retains writeback turnaround and timing that governs recurring
fills, stores, resource reuse and stalls.

See [the parameter reference](PARAMETERS.md) for each value's role and hardware path.

- `accelerator` holds SA, vector, and CIM timing inputs, in cycles.
- `soc` holds the measured bank-switch delay. DRAM settings remain in
  the existing compiler configuration.
- `metadata` records the hardware source and build conditions.
- `evidence` identifies each value as measured, derived, reference, assumed,
  or unknown, and gives its source.

The compiler exports the same bank-switch value to `memory_config.txt`.
Independent ports still have zero bank-switch cost. The harness does not
add the recorded accelerator pipeline costs again.

For the characterized 64×64 INT8 SA with single-bank accumulation, select
[`sa-n7-1ns-int8-single-accum.json`](sa-n7-1ns-int8-single-accum.json).
Its `accelerator.sa_output_pipeline` records synthesized stage capacities,
forward delays and writeback restart. Supply it together with
`accelerator.sa_input_pipeline` for the combined output/refill schedule;
omit both records to use the architectural estimate.
The compiler derives output beats and reduction completion from the mapping,
then schedules stage occupancy, output retirement and the compiler's
two-command overlap. It does not store workload stalls in JSON.

The scheduler keeps repeated loops compact. It evaluates initial transients,
then batches repetitions only when the complete relative timing state recurs.
This preserves the detailed schedule's result without visiting every vector.

This refinement covers dense direct INT24 stores and riding BF16 vector tails
with no fetched operands, including timed DRAM. DMA completion gates local
fills; output-slot reuse waits for its store. Local input and weight buffers
release their slots through the controller loop dependencies. Shared-bank
service uses the actual command slot, so a retained weight does not occupy
both ping-pong regions. Repeated command, tile and output schedules are batched
only when their complete relevant timing state repeats.

Bank reservations approximate requester arbitration, and array propagation
uses the architectural skew rather than every PE's flush pipeline. A group
spanning several physical banks retains a conservative region resource bound.
Other hardware paths retain their architectural estimates. The target geometry
and widths restrict the measured refinement; `metadata` remains descriptive.

Remove the obsolete `sa_output_slack_cycles` and unmeasured draft `sa_output`
entries from older records. The explicit eight-vector FIFO remains derived
in code, not a measured-profile constant.

`accumulation_feedback_cycles: 4` makes the compiler reject CIM mappings
whose dependent accumulation SRAM updates are closer than the measured
3-cycle read-to-store path. Output stalls are covered by the context count:
at most five younger operations can be in flight, and five local contexts
keep them in registers.

`output_pipeline: null` retains architectural storage for the uncharacterized
streaming output route.

SA and CIM share the source-defined registered FIFO rules and the compiler's
command dispatch, DMA and retirement rules. The optional CIM `banked_pipeline`
record adds measured producer handoff, forwarding, vector-stage timing, and
operand read-ahead. Read-ahead is the number of operand beats each input or
weight controller accepts before its buffer slot frees. It counts only
accelerator storage; system and test-harness response queues are excluded.
Accepted beats shorten later scratchpad port service but do not make a tile
ready before its slot frees and the fill completes.
Its geometry and width fields restrict where these measurements apply; they
do not supply workload timing. The compiler derives tile completion, operand
reuse, port service, and waits from the hardware configuration and mapping.

For characterized banked CIM routes, operand fills, accumulation-bank reuse,
vector completion and DMA now share one timeline. A bank can become reusable
before its vectors finish writing to memory. The next vector command still
waits for the prior command's output completion. Source-defined storage stays
in code; recorded stage capacities describe synthesized pipeline retention.
Single-bank output, split reductions, fetched vector operands, bias/scales,
nonuniform input fills, and bank-switching paths retain resource bounds.
Alignment therefore remains partial, and code sharing does not establish
equal accuracy.

Search first finds a candidate with the inexpensive resource model, then ranks
candidates with the combined schedule. It stops evaluating a candidate only
when an optimistic completion bound exceeds the best runtime (including any
configured tolerance). Exact ties remain eligible. Repeated timing states
are batched without changing the schedule. Reporting uses the same local
one-command timing, but its whole-graph scalar schedule does not reproduce
all cross-command overlap modeled during search.

Missing required fields cause an error. An unknown value is not a measured
zero. Source-defined output issue rate and weight ownership transitions are
calculated in code, along with workload-dependent waits. The SA record no
longer needs unused CIM entries. Remove `output_cycles_per_vector`,
`weight_release_cycles`, `weight_ready_cycles`, and
`memory_request_latency_cycles` from older profiles; these are no longer
recorded build measurements.

## Current evidence

The CIM record includes the two tested N7, 1 ns, INT8, double-buffered
configurations with 2 and 18 weight sets. The 11-cycle minimum bank drain
has RTL evidence. The existing compiler references for standalone-vector
startup (96 cycles per pass) and sparse-row turnaround (8 cycles) are retained
with `reference` status. They are inherited Sphinx measurements, not fresh
characterizations of these builds. A record may instead mark an uncharacterized
delay as `null`; consuming such a delay emits a warning and omits it from the
estimate.
The record does not claim that all paths were calibrated on these builds.

Prepare new values from existing reports or targeted measurements when
needed. Store the results in JSON for reuse. Validate mapping selection
against alternative legal mappings on the same RTL. K128 is a regression
case; absolute prediction error alone is not enough.
