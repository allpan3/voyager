# Injected timing parameters

The [CIM timing profile](cim-n7-1ns-int8-double-accum.json)
supplies 11 fields to the compiler's timing model. Numeric timing values are
in cycles; `null` means unknown. These fields describe hardware behavior.
The bank-switch cost also configures the simulation harness.

The profile covers N7, 1 ns, INT8 builds with 2 or 18 CIM weight sets.
`double-accum` means `DOUBLE_BUFFERED_ACCUM_BUFFER=true`:
two accumulation SRAM banks alternate between computation and output drain.
The new measured correction is the 11-cycle output-bank drain. Other numeric
values retain earlier model inputs.

## Accelerator fields

These fields are under `accelerator`.

| Parameter | Value | Role | Hardware path and evidence |
| --- | --- | --- | --- |
| `kernel_launch_overhead_cycles` | 96 | Fixed overhead per vector pass: parameter dispatch, instructions, and pipeline fill/drain. | [VectorUnit::send_instructions](../src/vector_unit/main.h#L300), [output completion](../src/vector_unit/OutputController.h#L574), and [SoC launch/completion control](../soc/src/main/scala/Voyager.scala#L726). Reference measurement from Sphinx; an aggregate cost. |
| `sa_output_slack_cycles` | 24 | Slack subtracted from the SA output-stall estimate before a slower output path limits array progress. | [MatrixProcessor output FIFOs](../src/MatrixProcessor.h#L48) → [OutputController](../src/OutputController.h#L184) → [vector pipeline](../src/vector_unit/VectorPipeline.h#L135). Reference buffering/overlap correction from an earlier SA build. |
| `spmm_row_cycles` | 8 | Fixed turnaround per sparse row per output-channel pass; weight-gather traffic is counted separately. | [SpMMUnit::run_accumulation](../src/SpMMUnit.h#L1059) and [send_outputs](../src/SpMMUnit.h#L1165). Sphinx reference measurement, used by the SA sparse-correction model. The recorded CIM builds disable SpMM. |

## CIM fields

These fields are under `accelerator.cim`.

| Parameter | Value | Role | Hardware path and evidence |
| --- | --- | --- | --- |
| `output_cycles_per_vector` | 1 | Minimum output service time per vector. Payload width, SRAM traffic, and fused vector operations can increase it. | [CIMProcessor::write_back](../src/cim/CIMProcessor.h#L1634) → [OutputController](../src/OutputController.h#L184). Assumed minimum; the complete path has not been measured as one cycle. |
| `accumulation_feedback_cycles` | `null` | Required spacing for dependent accumulator SRAM updates. The compiler currently reports unknown feedback safety; this field does not insert hardware stalls. | [issue_accumulation_reads](../src/cim/CIMProcessor.h#L1393) → [complete_accumulation](../src/cim/CIMProcessor.h#L1505) ↔ [DualPortBuffer](../src/DualPortBuffer.h#L14). Write visibility before a dependent read remains unmeasured. |
| `weight_release_cycles` | 1 | Delay from the final modeled use of a resident weight set until it can be refilled. | [Release event](../src/cim/CIMProcessor.h#L1073) → [set_scoreboard: READY to FREE](../src/cim/CIMProcessor.h#L741) → [loader ownership check](../src/cim/CIMProcessor.h#L859). Derived from one registered transition; extra HLS stages are unmeasured. |
| `weight_ready_cycles` | 1 | Delay from completing a weight-set fill until its first MAC issue can begin. | [Ready event](../src/cim/CIMProcessor.h#L872) → [set_scoreboard: FREE to READY](../src/cim/CIMProcessor.h#L741) → [first-use readiness check](../src/cim/CIMProcessor.h#L1030). Derived from one registered transition. |
| `output_bank_drain_cycles` | 11 | Fixed overhead per completed output tile on the banked accumulation path, including the final non-overlapped drain. | [Tile drain and bank-return handshakes](../src/OutputController.h#L322) ↔ [SRAM ownership handoff](../src/DualPortBuffer.h#L64). Measured minimum RTL gap for the recorded CIM2/CIM18 builds. |
| `output_pipeline` | `null` | Optional output-stage capacities and forward/backpressure delays. Currently uses architectural storage without added measured stage delays. | [Matrix output buffering](../src/cim/CIMProcessor.h#L419) → [OutputController](../src/OutputController.h#L184) → [vector stages and channels](../src/vector_unit/VectorPipeline.h#L53). A complete stage timing record is unavailable. |

## SoC fields

These fields are under `soc`.

| Parameter | Value | Role | Hardware path and evidence |
| --- | --- | --- | --- |
| `bank_switch_cycles` | 8 | Uncovered delay when a request port switches scratchpad banks. Exported to the harness; independent ports use zero. | [requestFifo](../soc/src/main/scala/TLInterface.scala#L40) → [TLFIFOFixer](../soc/src/main/scala/Voyager.scala#L443) → [scratchpad fabric](../soc/src/main/scala/Scratchpad.scala#L25). Sphinx round-trip measurement, modeled in [ScratchpadTiming](../test/common/ScratchpadTiming.h#L151). |
| `memory_request_latency_cycles` | 0 | Additional initiation cost per CIM input, weight, or bias request. Zero adds no extra overhead; payload transfers still cost cycles. | [Request issue helpers](../src/Utils.h#L6) → [TileLink request handshake](../soc/src/main/scala/TLInterface.scala#L114). Assumed extra cost; this is not total SRAM response latency. |

## Optional output-pipeline record

No nested values are supplied while `output_pipeline` is `null`. A record
can contain:

- `elements_per_vector`, `vector_lanes`, `accum_bits`, `output_bits`, and
  `port_bits`: identify the lane counts and datapath widths for the stage record.
- `stages[].name`: stage identifier. `elements`: storage capacity, or the
  existing `matrix_output` or `vector_pipeline` capacity.
- `stages[].forward_cycles`: data transit delay. `feedback_cycles`:
  backpressure/credit-return delay. These describe output FIFOs, pipeline
  registers, and ready/valid feedback along the output path.

The [compiler output model](https://github.com/allpan3/voyager-compiler/blob/compiler-timing-profile/src/voyager_compiler/codegen/transform/tiling/timing/output.py)
validates the geometry and uses those stage values.

Build settings under `metadata.builds[].parameters` describe the measured
build; the timing loader does not inject them. Names and evidence text are
descriptive. DRAM timing remains in the existing compiler configuration.
Hardware links identify the relevant paths, not fresh latency measurements.
