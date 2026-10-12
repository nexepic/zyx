"""Adversarial result checks that row-count-only validation cannot detect."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runner.semantic_oracle import verify_groups, verify_topk, verify_observations


USERS = [{"id": "a", "country": "CN", "score": 9},
         {"id": "b", "country": "US", "score": 8},
         {"id": "c", "country": "CN", "score": 8},
         {"id": "d", "country": "US", "score": 1}]


class SemanticOracleTest(unittest.TestCase):
    def test_groups_accept_any_group_order(self):
        self.assertEqual(verify_groups(USERS, [["US", 2], ["CN", 2]]), [])

    def test_same_count_wrong_groups_fail(self):
        for rows in ([["US", 1], ["CN", 3]], [["US", 2], ["US", 2]],
                     [["US", 2], ["XX", 2]], [["US", 2.0], ["CN", 2]],
                     [["US", True], ["CN", 2]], [[None, 2], ["CN", 2]]):
            with self.subTest(rows=rows):
                self.assertTrue(verify_groups(USERS, rows))

    def test_topk_accepts_boundary_ties_without_fixed_id_order(self):
        self.assertEqual(verify_topk(USERS, [["a"], ["b"]], 2), [])
        self.assertEqual(verify_topk(USERS, [["a"], ["c"]], 2), [])
        self.assertEqual(verify_topk(USERS, [["a"], ["c"], ["b"]], 3), [])

    def test_same_count_wrong_topk_fails(self):
        for rows in ([["a"], ["d"]], [["b"], ["a"]], [["a"], ["a"]],
                     [["a"], ["unknown"]], [["a"], [8]], [["a", 9], ["b", 8]]):
            with self.subTest(rows=rows):
                self.assertTrue(verify_topk(USERS, rows, 2))

    def test_empty_and_limit_above_population(self):
        self.assertEqual(verify_groups([], []), [])
        self.assertEqual(verify_topk(USERS, [], 0), [])
        self.assertEqual(verify_topk([], [], 100), [])
        self.assertEqual(verify_topk(USERS, [["a"], ["b"], ["c"], ["d"]]), [])

    def test_missing_rows_are_not_success(self):
        self.assertTrue(verify_groups(USERS, None))
        self.assertTrue(verify_topk(USERS, None))

    def test_invalid_dataset_and_limit_fail(self):
        for users in (USERS + [USERS[0]], [{"id": "a", "score": float("nan")}],
                      [{"id": "a", "score": float("inf")}]):
            with self.assertRaises(ValueError):
                verify_topk(users, [])
        for limit in (-1, True, 0.5):
            with self.assertRaises(ValueError):
                verify_topk(USERS, [], limit)

    def test_every_iteration_requires_full_output(self):
        events = [{"event": "result", "database": "zyx", "workload": "aggregation_count_by_group",
                   "iteration": i, "rows": [["CN", 2], ["US", 2]]} for i in range(2)]
        args = (USERS, events, ["zyx"], ["aggregation_count_by_group"], 2)
        self.assertEqual(verify_observations(*args)["failures"], [])
        events[1]["iteration"] = 0
        self.assertTrue(verify_observations(*args)["failures"])
        events[1]["iteration"] = True
        self.assertTrue(verify_observations(*args)["failures"])
        events[1]["iteration"] = 1
        del events[1]["rows"]
        self.assertTrue(verify_observations(*args)["failures"])

    def test_missing_database_or_observation_fails(self):
        result = verify_observations(USERS, [], ["zyx", "kuzu"], ["topk_property_sort"], 1)
        self.assertEqual(len(result["failures"]), 2)

    def test_count_only_payload_cannot_pass_semantic_gate(self):
        event = {"event": "result", "database": "zyx", "workload": "topk_property_sort",
                 "iteration": 0, "value": 4}
        result = verify_observations(USERS, [event], ["zyx"], ["topk_property_sort"], 1)
        self.assertEqual(result["failures"], ["zyx/topk_property_sort/0: missing Top-K rows"])

    def test_exact_wrong_groups_fail_at_iteration_location(self):
        event = {"event": "result", "database": "zyx", "workload": "aggregation_count_by_group",
                 "iteration": 0, "rows": [["CN", 1], ["US", 3]]}
        result = verify_observations(USERS, [event], ["zyx"], ["aggregation_count_by_group"], 1)
        self.assertEqual(result["failures"], ["zyx/aggregation_count_by_group/0: aggregate keys or counts differ"])


if __name__ == "__main__":
    unittest.main()
