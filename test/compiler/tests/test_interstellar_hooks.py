# Verify opt-in evaluation hooks and preserve the existing symmetric SA objective
import contextlib
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "interstellar/src"))
import interstellar as interstellar
from interstellar.evaluation import CandidateEvaluation


# Construct a small three-level resource with independent operand costs
def resource(costs=None, partitions=None, capacity=64):
    return interstellar.Resource(
        [[1, 1, 1], [capacity] * 3, [4096]],
        costs or [[1, 1, 1], [10, 10, 10], [100]],
        [[0, 0, 0], [0, 0, 0], [0]], [4, 1, 1], mac_capacity=0,
        memory_partitions=partitions or [[0, 1, 2], [0, 1, 2], [0, 0, 0]],
        invalid_underutilized=False,
    )


# Keep the test layer small enough for real complete candidate enumeration
def layer():
    return interstellar.Layer(nifm=2, nofm=2, wofm=4, hofm=1, wfil=1, hfil=1)


# Capture the legacy optimizer's verbose-free diagnostic output during tests
def optimize(hardware=None, **kwargs):
    with contextlib.redirect_stdout(io.StringIO()):
        return interstellar.optimizer.opt_optimizer(hardware or resource(), layer(), **kwargs)


# Check complete evaluation ownership and safe replacement of SRAM filtering
class EvaluationHookTests(unittest.TestCase):
    # Preserve the exact pre-change symmetric SA winner, factorization and score
    def test_matched_sa_baseline(self):
        cost, runtime, mapping, perf = optimize()
        self.assertEqual((cost, perf), (2248, 4.0))
        self.assertEqual(mapping.loop_blockings[interstellar.le.OX], (1, 4, 1))
        self.assertEqual(mapping.loop_partitionings[interstellar.le.IC], (2, 1, 1))
        self.assertEqual(mapping.loop_partitionings[interstellar.le.OC], (2, 1, 1))
        self.assertEqual(cost, interstellar.cost_model.get_cost(resource(), mapping, layer())[0])
        self.assertIsInstance(mapping.evaluation, CandidateEvaluation)
        self.assertEqual(mapping.evaluation.runtime_cycles, perf)
        self.assertFalse(mapping.evaluation.rank_by_runtime)

    # Verify each complete candidate is evaluated once and the winner retains that object
    def test_evaluate_once_and_report_same_result(self):
        results = []

        # Use visibly backend-defined costs without any SA access calculation
        def evaluate_candidate(hardware, operation, mapping):
            value = mapping.loop_blockings[interstellar.le.OX][1]
            evaluation = CandidateEvaluation(True, runtime_cycles=10 - value, cost=value,
                                             traffic={"fresh_a": 16}, metadata={"target": "test"})
            results.append((mapping, evaluation))
            return evaluation

        with patch.object(interstellar.cost_model, "get_level_cost", side_effect=AssertionError("generic cost used")), \
             patch.object(interstellar.cost_model, "get_access", side_effect=AssertionError("generic traffic used")), \
             patch.object(interstellar.cost_model, "get_ideal_performance", side_effect=AssertionError("SA performance used")):
            cost, runtime, mapping, perf = optimize(candidate_evaluator=evaluate_candidate)
        self.assertGreater(len(results), 1)
        self.assertEqual(len({id(point) for point, _ in results}), len(results))
        expected = min((evaluation for _, evaluation in results), key=lambda value: value.rank())
        self.assertIs(mapping.evaluation, expected)
        self.assertEqual((runtime, cost), expected.rank())
        self.assertEqual(perf, runtime)

    # Skip illegal candidates even if they would otherwise have the best score
    def test_legality_precedes_ranking(self):
        rejected = []

        # Reject the largest inner tile to make legality affect the winning mapping
        def evaluate_candidate(hardware, operation, mapping):
            value = mapping.loop_blockings[interstellar.le.OX][1]
            if value == 4:
                rejected.append(mapping)
                return CandidateEvaluation(False, reasons=("directed restriction",))
            return CandidateEvaluation(True, runtime_cycles=10 - value, cost=1, traffic={})

        _, _, mapping, _ = optimize(candidate_evaluator=evaluate_candidate)
        self.assertTrue(rejected)
        self.assertNotEqual(mapping.loop_blockings[interstellar.le.OX][1], 4)

    # Replace generic memory tests at both pre-spatial and post-spatial stages
    def test_capacity_hook_reaches_both_stages(self):
        stages = set()

        # Defer uncertain capacity checks until the complete evaluator
        def capacity_filter(hardware, operation, mapping, stage):
            stages.add(stage)
            self.assertIsNone(mapping.loop_orders)
            return True

        with patch.object(interstellar.cost_model, "valid_blocking_size_current_level", side_effect=AssertionError("generic capacity used")), \
             patch.object(interstellar.cost_model, "valid_partitioning", side_effect=AssertionError("generic bank capacity used")):
            optimize(resource(capacity=0), capacity_filter=capacity_filter,
                     candidate_evaluator=lambda *args: CandidateEvaluation(True, runtime_cycles=1, cost=1, traffic={}))
        self.assertEqual(stages, {"blocking", "partitioned"})


# Ensure rank/report costs agree for asymmetric and absent memory partitions
class CostConsistencyTests(unittest.TestCase):
    # Use independent operand costs that expose the former input-cost-only total
    def test_asymmetric_rank_and_report(self):
        hardware = resource(costs=[[2, 3, 5], [7, 11, 13], [17]])
        cost, _, mapping, _ = optimize(hardware)
        total, coefficients, accesses = interstellar.cost_model.get_cost(hardware, mapping, layer())
        expected = sum(sum(access * coefficient for access, coefficient in zip(level_access, level_cost))
                       for level_access, level_cost in zip(accesses, coefficients))
        self.assertEqual(total, expected)
        self.assertEqual(cost, total)
        self.assertEqual(sum(interstellar.cost_model.get_level_costs(hardware, mapping, layer())), total)


if __name__ == "__main__":
    unittest.main()
