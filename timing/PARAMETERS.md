# Injected timing parameters

The [CIM timing profile](cim-n7-1ns-int8-double-accum.json)
supplies timing inputs to the compiler's timing model. Numeric values are
in cycles; `null` means unknown. These fields describe hardware behavior.
Inherited reference measurements are explicitly labeled; they are not fresh
measurements of each build.
The bank-switch cost also configures the simulation harness.

The profile covers N7, 1 ns, INT8 builds with 2 or 18 CIM weight sets.
`double-accum` means `DOUBLE_BUFFERED_ACCUM_BUFFER=true`:
two accumulation SRAM banks alternate between computation and output drain.
The 11-cycle output-bank drain is measured. Source-derived transitions and
workload-dependent waits belong to the compiler, not this record.

## Accelerator fields

These fields are under `accelerator`.

| Parameter | Value | Role | Hardware path and evidence |
| --- | --- | --- | --- |
| `kernel_launch_overhead_cycles` | 96 | Existing compiler reference for parameter dispatch, instruction issue and pipeline fill/drain per standalone vector pass. | [VectorUnit::send_instructions](../src/vector_unit/main.h#L300), [output completion](../src/vector_unit/OutputController.h#L574), and [SoC launch/completion control](../soc/src/main/scala/Voyager.scala#L726). Inherited Sphinx measurement; no separate measurement for this build. |
| `spmm_row_cycles` | 8 | Existing compiler reference for sparse-row turnaround. Weight-gather traffic is counted separately. | [SpMMUnit::run_accumulation](../src/SpMMUnit.h#L1059) and [send_outputs](../src/SpMMUnit.h#L1165). Inherited Sphinx measurement of 7.3–7.5 cycles, rounded to 8. The recorded builds disable SpMM. |

The compiler derives the eight-vector output FIFO, payload beat count and
reduction-completion sequence in code. The former `sa_output_slack_cycles: 24`
and unmeasured draft delays are removed. Wide outputs parked in accumulation
SRAM still use the existing banked model.

The optional `accelerator.sa_output_pipeline` in the
[SA build record](sa-n7-1ns-int8-single-accum.json) describes synthesized
storage and latency. `target` identifies its geometry and datatype widths.
Each stage has a capacity in complete vectors and an unstalled forward delay:

| Stage | Capacity (vectors) | Forward delay (cycles) |
| --- | ---: | ---: |
| Matrix output controller | 2 | 1 |
| Vector input router | 8 | 7 |
| Vector stage 0 | 7 | 6 |
| Vector stage 1 | 8 | 7 |
| Vector stage 2 | 7 | 6 |
| Vector stage 3 | 10 | 9 |
| Vector output routing | 2 | 1 |
| Store controller | 2 | 3 |

The store delay ends at its first beat. The compiler calculates extra beats
from output width and port width. `direct_store_forward_cycles: 2` measures
from a direct-output FIFO dequeue to its first accepted memory beat. This
per-vector interval places recurring stores relative to shared-bank reads.

`writeback_restart_cycles: 18` is the minimum gap from one command's last
output enqueue to the next command's first output enqueue. It combines the
16-cycle controller restart and 2-cycle forwarding measurements in one record.
The scheduler applies this repeated constraint alongside data readiness, so
input/DMA waits can overlap it. It is not added unconditionally to every tile.

The scheduler omits small parameter-forwarding, vector-start and completion
delays. It approximates completion at the final accepted store beat, while
retaining command ordering, DMA completion and output-slot ownership constraints.

The record covers the 64×64 INT8/INT24, BF16 vector, 512-bit-port,
single-accumulation-bank build with performance counters enabled. It applies
to dense direct INT24 stores and riding BF16 vector tails without fetched
operands. The scheduler follows DMA completion, parameter dispatch, the
compiler overlap window, local-buffer refills and output-slot reuse. It
preserves the planner's separate ping-pong regions, including a retained
operand that occupies only the first region. Bank reservations approximate
requester arbitration; array propagation does not simulate every PE's flush
pipeline. These limits affect estimate accuracy, not the recorded constants.

The optional `accelerator.sa_input_pipeline` records local-buffer and data-path timing:

| Parameter | Cycles | Measured interval |
| --- | ---: | --- |
| `input_buffer_forward_cycles` | 9 | Final input fill response to the first local-buffer output. |
| `weight_buffer_forward_cycles` | 9 | Final weight fill response to the first local-buffer output. |
| `input_buffer_read_cycles` | 4 | Input-buffer read request to its array-input handshake, without backpressure. |
| `weight_buffer_read_cycles` | 3 | Weight-buffer read request to its array-input handshake, without backpressure. |
| `array_forward_cycles` | 8 | Input-to-deskewed-result delay beyond array-dimension propagation. |
| `accumulation_forward_cycles` | 10 | Deskewed partial sum to final-output enqueue, without waiting. |

The estimate omits short parameter-forwarding and request-start delays.
Buffer forwarding and read timing remain because they control repeated local
refills and slot release. Array/accumulation forwarding places output traffic
relative to those reads, affecting shared-bank contention during execution.

The compiler counts serialized parameter words from the source layout and
payload beats from hardware widths. It places each local-buffer fill after its
required DMA and buffer availability. Read forwarding relates array
consumption to the release of a local ping-pong slot. Refill duration, slot
reuse and shared-bank waits are calculated in code; none is a recorded
per-workload stall penalty.

## CIM fields

These fields are under `accelerator.cim`.

| Parameter | Value | Role | Hardware path and evidence |
| --- | --- | --- | --- |
| `accumulation_feedback_cycles` | `null` | Required spacing for dependent accumulator SRAM updates. The compiler currently reports unknown feedback safety; this field does not insert hardware stalls. | [issue_accumulation_reads](../src/cim/CIMProcessor.h#L1393) → [complete_accumulation](../src/cim/CIMProcessor.h#L1505) ↔ [DualPortBuffer](../src/DualPortBuffer.h#L14). Write visibility before a dependent read remains unmeasured. |
| `output_bank_drain_cycles` | 11 | Fixed overhead per completed output tile on the banked accumulation path, including the final non-overlapped drain. | [Tile drain and bank-return handshakes](../src/OutputController.h#L322) ↔ [SRAM ownership handoff](../src/DualPortBuffer.h#L64). Measured minimum RTL gap for the recorded CIM2/CIM18 builds. |
| `output_pipeline` | `null` | Optional output-stage capacities and forward/backpressure delays. Currently uses architectural storage without added measured stage delays. | [Matrix output buffering](../src/cim/CIMProcessor.h#L419) → [OutputController](../src/OutputController.h#L184) → [vector stages and channels](../src/vector_unit/VectorPipeline.h#L53). A complete stage timing record is unavailable. |

The output controller's initiation interval and the registered resident-set
ready/release transitions are defined in hardware source and modeled in code.
Transfer cycles follow payload width and port width. The former extra
per-request allowance is removed; startup and bank-switch waits are distinct.

## SoC fields

These fields are under `soc`.

| Parameter | Value | Role | Hardware path and evidence |
| --- | --- | --- | --- |
| `bank_switch_cycles` | 8 | Uncovered delay when a request port switches scratchpad banks. Exported to the harness; independent ports use zero. | [requestFifo](../soc/src/main/scala/TLInterface.scala#L40) → [TLFIFOFixer](../soc/src/main/scala/Voyager.scala#L443) → [scratchpad fabric](../soc/src/main/scala/Scratchpad.scala#L25). Sphinx round-trip measurement, modeled in [ScratchpadTiming](../test/common/ScratchpadTiming.h#L151). |

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

Build conditions under `metadata` describe the measured hardware; the timing
loader does not inject them. Names and evidence text are
descriptive. DRAM timing remains in the existing compiler configuration.
Hardware links identify the relevant paths, not fresh latency measurements.
