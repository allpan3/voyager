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
Its optional `accelerator.sa_output_pipeline` records synthesized stage
capacities, forward delays, writeback restart and command start/done delays.
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

Two optional CIM refinements have explicit unknown values:

- `accumulation_feedback_cycles: null` preserves the existing unvalidated
  feedback-safety result. It does not prove RAW safety.
- `output_pipeline: null` retains architectural output storage without
  additional scheduled pipeline stages.

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
has RTL evidence. The former standalone-vector and sparse-row reference
allowances are now `null`, because they were not measurements of these builds.
Using an unknown delay emits a warning: the estimate omits that physical
delay and remains a lower bound for that path.
The record does not claim that all paths were calibrated on these builds.

Prepare new values from existing reports or targeted measurements when
needed. Store the results in JSON for reuse. Validate mapping selection
against alternative legal mappings on the same RTL. K128 is a regression
case; absolute prediction error alone is not enough.
