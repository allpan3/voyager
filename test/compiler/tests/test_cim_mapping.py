# Check current-CIM source-derived policy with directed and exhaustive small cases
from dataclasses import replace
from itertools import permutations, product
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from voyager_compiler.mapping.schedule import LOOPS, Schedule, TemporalLevel
from voyager_compiler.mapping.target import CIMTarget
from voyager_compiler.mapping.models.cim_weight_policy import WeightFetch, descriptor_trace, weight_policy

from voyager_compiler.mapping.workload import Workload
from voyager_compiler.mapping.models.cim import evaluate as evaluate_command


# Supply an explicit unpadded stride-one workload to the policy checks
def evaluate(target, schedule, fetch=None):
    extent = {loop: schedule.l1.bound(loop) * schedule.l2.bound(loop) for loop in LOOPS}
    workload = Workload(extent["OX"] + extent["FX"] - 1,
                        extent["OY"] + extent["FY"] - 1,
                        extent["IC"] * target.k, extent["OC"] * target.n,
                        extent["FX"], extent["FY"])
    return evaluate_command(target, schedule, workload, fetch)


TARGET_PATH = Path(__file__).resolve().parents[1] / "targets/cim-test.json"


# Build the unit-test 8-by-8 hierarchy using the current physical field names
def small_target(**changes):
    target = replace(CIMTarget.load(TARGET_PATH), ch_in=2, ch_out=2,
                     tile_input_axis_elements=2, tile_output_axis_elements=1,
                     input_axis_tiles=2, output_axis_tiles=8, a_port_tiles=2,
                     b_port_tiles=1, c_port_tiles=8, result_slots_per_output_lane=2,
                     b_sets=8, input_buffer_words=16, accum_buffer_words=16,
                     ic_port_bits=64, oc_port_bits=64, base_c_width=12)
    target = replace(target, **changes)
    return replace(target, vector_config=dict(lanes=target.n),
                   output_storage=dict(matrix_results=target.n * target.result_slots_per_output_lane,
                                       accumulation_metadata=target.n * 2, accumulation_writeback=target.n,
                                       matrix_output=target.n * 8, vector_pipeline=0))


# Enumerate expected per-MAC logical weights directly from semantic loop nesting
def semantic_weight_trace(schedule):
    levels = (schedule.l2, schedule.l1)
    dimensions = [(level, loop) for level in range(2) for loop in reversed(levels[level].order)]
    for values in product(*(range(levels[level].bound(loop)) for level, loop in dimensions)):
        counters = dict(zip(dimensions, values))
        yield tuple(counters[(0, loop)] for loop in ("OC", "IC", "FY")) + tuple(
            counters[(1, loop)] for loop in ("IC", "OC", "FX", "FY"))


# Check target configuration and physical geometry
class TargetTests(unittest.TestCase):
    # Check serial slice rounding and separate result latency
    def test_serial_macro_cadence(self):
        target = small_target(mode=1, base_c_width=8, mac_latency=5)
        self.assertEqual(target.issue_interval, 12)  # Three 3-bit slices use 4 cycles each
        self.assertEqual(target.macro_result_latency, 16)


# Exercise exact weight descriptor boundaries and full-set traffic
class WeightPolicyTests(unittest.TestCase):
    # Verify the required three-set/four-replay capacity boundary
    def test_three_sets_four_uses(self):
        schedule = Schedule(TemporalLevel.make(IC=3), TemporalLevel.make(OX=4))
        for capacity, descriptors, loads in ((3, 1, 3), (2, 12, 12)):
            result = evaluate(small_target(b_sets=capacity), schedule)
            self.assertTrue(result.legal, result.reasons)
            self.assertEqual((result.traffic.descriptors, result.traffic.full_set_loads,
                              result.traffic.set_uses, result.traffic.a_beats),
                             (descriptors, loads, 12, 12))
            self.assertEqual(result.traffic.b_writes, loads * 64)
            self.assertEqual(result.traffic.b_write_bytes, loads * 64)
        trace = list(descriptor_trace(small_target(b_sets=3), schedule))
        self.assertEqual((len(trace), len(trace[0][0]), trace[0][1]), (1, 3, 4))


    # Match per-MAC weight identities over small independent loop orders
    def test_exhaustive_small_weight_trace(self):
        for inner_order, outer_order, capacity in product(permutations(("OX", "IC", "OC")),
                                                         permutations(("OX", "IC", "OC")), (1, 2, 8)):
            schedule = Schedule(TemporalLevel.make(inner_order, OX=2, IC=2, OC=2),
                                TemporalLevel.make(outer_order, OX=2, IC=2, OC=2))
            target = small_target(b_sets=capacity)
            policy = weight_policy(target, schedule)
            actual = [key for keys, replays in descriptor_trace(target, schedule)
                      for _ in range(replays) for key in keys
                      for _ in range(policy.macs_per_set_use)]
            self.assertEqual(actual, list(semantic_weight_trace(schedule)), (inner_order, outer_order, capacity))

    # Keep useful source bytes, rounded external transfers, and padded writes separate
    def test_partial_weight_payload(self):
        result = evaluate(small_target(), Schedule(), WeightFetch(3, 5, 5))
        self.assertTrue(result.legal, result.reasons)
        traffic = result.traffic
        self.assertEqual((traffic.weight_unique_useful_bytes, traffic.weight_requested_bytes,
                          traffic.weight_external_transfer_bytes, traffic.b_write_bytes),
                         (15, 15, 24, 64))
        self.assertEqual((traffic.weight_external_beats, traffic.b_writes), (3, 64))
        self.assertEqual((traffic.useful_scalar_macs, traffic.compute_scalar_macs), (15, 64))

    # Account for the current reader refetching each packed source row per set
    def test_packed_weight_rows_are_not_cached(self):
        schedule = Schedule(TemporalLevel.make(OC=2))
        traffic = evaluate(small_target(oc_port_bits=128), schedule, WeightFetch(8, 8, 16, 2)).traffic
        self.assertEqual(traffic.weight_unique_useful_bytes, 128)
        self.assertEqual(traffic.weight_requested_bytes, 256)
        self.assertEqual(traffic.weight_external_beats, 16)
        self.assertEqual(traffic.b_write_bytes, 128)


# Count local ownership independently by admitting first contributions into free slots
def accumulation_trace_counts(schedule, contexts):
    levels = (schedule.l2, schedule.l1)
    dimensions = [(level, loop) for level in range(2) for loop in reversed(levels[level].order)]
    owners, resident, live = {}, set(), set()
    updates = reads = writes = peak = 0
    for values in product(*(range(levels[level].bound(loop)) for level, loop in dimensions)):
        counters = dict(zip(dimensions, values))
        row = tuple(counters[level, loop] for level in range(2) for loop in ("OC", "OY", "OX"))
        reductions = [(level, loop) for level in range(2) for loop in ("IC", "FX", "FY")]
        first = all(counters[d] == 0 for d in reductions)
        final = all(counters[level, loop] == levels[level].bound(loop) - 1 for level, loop in reductions)
        if first:
            owners[row] = len(resident) < contexts
            if owners[row]:
                resident.add(row)
        local = owners[row]
        updates += local
        reads += not first and not local
        writes += not final and not local
        if final:
            resident.discard(row)
            live.discard(row)
            del owners[row]
        else:
            live.add(row)
            peak = max(peak, len(live))
    assert not owners and not resident and not live
    return updates, reads, writes, max(1, peak)


# Check lifetime-based context allocation and actual local/buffer retirement choices
class AccumulationTests(unittest.TestCase):
    # Reuse one feedback register across four complete rows without changing final addresses
    def test_consecutive_rows_reuse_one_context(self):
        target = small_target(local_accum_contexts=1, double_buffered_accum=True)
        for banked in (False, True):
            schedule = Schedule(TemporalLevel.make(order=("IC", "OX"), OX=4, IC=4),
                                write_output_to_accum_buffer=banked)
            result = evaluate(target, schedule)
            self.assertTrue(result.legal)
            self.assertEqual(result.accumulation_footprint, 4)
            t = result.traffic
            self.assertEqual((t.buffer_accum_reads, t.buffer_accum_intermediate_writes,
                              t.buffer_accum_final_writes, t.direct_output_vectors),
                             (0, 0, 4 if banked else 0, 0 if banked else 4))
            self.assertEqual(t.local_accum_updates, 16)
            self.assertEqual(result.local_accum_footprint, 1)
            self.assertEqual(result.timing.resource_cycles["accumulation"], 16)
            self.assertEqual(result.timing.resource_cycles["accumulation_sram_reads"], 4 if banked else 0)
            self.assertEqual(result.timing.resource_cycles["accumulation_sram_writes"], 4 if banked else 0)

    # Compare every inner order and representative two-level reductions with a live-row oracle
    def test_exhaustive_accumulation_lifetimes(self):
        schedules = [Schedule(TemporalLevel.make(order=order, **dict.fromkeys(LOOPS, 2)))
                     for order in permutations(LOOPS)]
        for reduction in ("IC", "FY"):
            for outer in permutations(("OX", "OY", reduction)):
                for inner in permutations(("OC", "OX", "FX")):
                    schedules.append(Schedule(
                        TemporalLevel.make(order=inner, OC=2, OX=2, FX=2),
                        TemporalLevel.make(order=outer, OX=2, OY=2, **{reduction: 2})))
        for schedule in schedules:
            for contexts in (1, 3, 4):
                result = evaluate(small_target(local_accum_contexts=contexts,
                                                input_buffer_words=256), schedule)
                self.assertTrue(result.legal, (schedule, result.reasons))
                t = result.traffic
                self.assertEqual((t.local_accum_updates, t.buffer_accum_reads,
                                  t.buffer_accum_intermediate_writes, result.local_accum_footprint),
                                 accumulation_trace_counts(schedule, contexts), schedule)

    # Retain the exact outer-OC, banked-context, ABI, and counter-width guards
    def test_schedule_legality_guards(self):
        target = small_target(double_buffered_accum=True)
        schedules = (
            Schedule(l2=TemporalLevel.make(FX=2)),
            Schedule(l0_temporal=(2, 1, 1, 1, 1, 1)),
            Schedule(l1=TemporalLevel.make(OX=17)),
            Schedule(l1=TemporalLevel.make(IC=1024)),
            Schedule(l1=TemporalLevel.make(FY=16)),
            Schedule(l2=TemporalLevel.make(order=("OC", "IC"), OC=2, IC=2)),
            Schedule(l2=TemporalLevel.make(OX=2, IC=2), write_output_to_accum_buffer=True),
            Schedule(l2=TemporalLevel.make(OX=256, OY=256)),
        )
        for schedule in schedules:
            with self.subTest(schedule=schedule):
                result = evaluate(target, schedule)
                self.assertFalse(result.legal)
                self.assertTrue(result.reasons)
                self.assertIsNone(result.runtime_cycles)


if __name__ == "__main__":
    unittest.main()
