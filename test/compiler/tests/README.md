# Mapping tests

Run `make mapping-tests` from the accelerator root with the project Python environment active.

The suite checks controller-compatible tiling export, candidate selection, CIM weight reuse and accumulation lifetimes, and timing regressions for input loading, output buffering, and vector operations. Independent small traces check weight order and accumulation ownership. The target JSON supplies explicit test hardware parameters; hardware builds use their exported mapping targets.

Integration checks cover exported epilogue metadata, accepted tiling dimensions, and result publication. They do not fix the total cycles of a full mapping search. Small timing tests use independent reference schedules or explicit port and buffer constraints. RTL cycle comparisons are needed to assess prediction accuracy.

The model-generation command `voyager-compiler/test/test_codegen.py` requires model and dataset inputs and is not part of this pytest suite. RTL checks use generated tilings and compare accelerator outputs with the software Gold Model or stored network reference tensors.
