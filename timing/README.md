# Recorded compiler timing

Select a JSON timing file when compiling a model. The compiler reads its
values before mapping search and uses them in the existing timing equations.
Normal compilation does not parse HLS reports or run RTL calibration.

For the existing Make model targets, add this setting to the usual command:

```sh
TIMING_PROFILE=timing/cim-final-n7-1ns-int8-double.json
```

Make forwards the file through `--timing_profile`. Direct compiler callers
can pass that argument or set `AcceleratorConfig(timing_profile=path)`.
Select `jeffrey-reference` explicitly to use the historical reference values.
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

- `accelerator` holds SA, vector, and CIM timing inputs, in cycles.
- `soc` holds bank-switch and per-request costs. DRAM settings remain in
  the existing compiler configuration.
- `metadata` records the hardware source and build conditions.
- `evidence` identifies each value as measured, derived, reference, assumed,
  or unknown, and gives its source.

The compiler exports the same bank-switch value to `memory_config.txt`.
Independent ports still have zero bank-switch cost. The harness does not
add the recorded accelerator pipeline costs again.

Two optional CIM refinements have explicit unknown values:

- `accumulation_feedback_cycles: null` preserves the existing unvalidated
  feedback-safety result. It does not prove RAW safety.
- `output_pipeline: null` retains architectural output storage without
  additional scheduled pipeline stages.

Missing fields cause an error. An unknown value is not a measured zero.
Keep architectural formulas and work counts in the model.

## Current evidence

The CIM record includes the two tested N7, 1 ns, INT8, double-buffered
configurations with 2 and 18 weight sets. The 11-cycle minimum bank drain
has RTL evidence. Other values retain the labeled reference assumptions.
The record does not claim that all paths were calibrated on these builds.

`jeffrey-reference` preserves Jeffrey's SA/vector values. Its CIM fields
retain the earlier ideal assumptions, including zero bank drain.

Prepare new values from existing reports or targeted measurements when
needed. Store the results in JSON for reuse. Validate mapping selection
against alternative legal mappings on the same RTL. K128 is a regression
case; absolute prediction error alone is not enough.
