# Mapping reports

`make network-proto` writes `mapping-report.md` beside `mapping-evaluations.json`
and `tilings.txtpb`. The report contains a layer summary, selected L1/L2 loop
nests, target geometry, cycle estimates and energy availability, storage use, and skipped
operations.

For SA and CIM, the report is under the selected `build/…/tilings/<network>/`
directory. Use the same target arguments as the mapping run to inspect it:

```bash
MAPPING_BUILD_DIR="$PWD/$(make -s print-build-dir "${MAPPING_ARGS[@]}" | tail -n 1)"
less "$MAPPING_BUILD_DIR/tilings/resnet18/mapping-report.md"
```

- **Utilization:** dense ideal cycles divided by estimated runtime, including
  padding work
- **True utilization:** useful ideal cycles divided by estimated runtime,
  excluding modeled zero-padding work
- Both backends exclude modeled spatial convolution padding and compiler
  channel padding from useful work
- Energy is unavailable without hardware characterization; `n/a` does not mean zero
- L1 and L2 loops are shown inner to outer, including factors of one
- IC/OC factors count channel blocks; multiply by fixed array lanes for channels
- L2 factors repeat L1 tiles; all L1 loops execute inside the L2 loops
- Stage service totals overlap and must not be summed as runtime
- Search counts on reused mappings describe the original search

JSON metrics include `dense_ideal_cycles`, `useful_ideal_cycles`, `utilization`,
and `true_utilization`. The existing `ideal_cycles` and `effective_utilization`
fields retain the useful-work definitions. Rendering an existing JSON report
derives the new display metrics without modifying that JSON or rerunning search.
