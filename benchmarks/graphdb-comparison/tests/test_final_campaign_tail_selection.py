import unittest

from runner.acceptance_sampling import select_read_supplements, select_write_supplements


class ReadTailSelectionTest(unittest.TestCase):
    def report(self, *, control_tail=(10, 10, 10), current_tail=(7, 7, 7),
               current_p50=7, workload="point_node_fetch_by_id", count=3):
        row = {"scale": "medium", "workload": workload, "control_processes": count,
               "current_processes": 3, "control_median_process_p50_ms": 10,
               "current_median_process_p50_ms": current_p50,
               "control_process_p50_min_ms": 10, "control_process_p50_max_ms": 10}
        processes = [{"scale": "medium", "workload": workload, "profile": "retrieval",
                      "engine": engine, "p95_ms": value}
                     for engine, values in [("control", control_tail), ("current", current_tail)]
                     for value in values]
        return {"comparisons": [row], "processes": processes}

    def test_stable_improvement_does_not_spend_supplement_budget(self):
        self.assertEqual(select_read_supplements(self.report(), set()), ({}, []))

    def test_near_p95_protection_gate_is_selected_despite_safe_p50(self):
        selected, reasons = select_read_supplements(self.report(current_tail=(10.6,) * 3), set())
        self.assertEqual(selected, {("medium", "retrieval"): [("point_node_fetch_by_id", 1.03)]})
        self.assertTrue(reasons[0]["near_p95_gate"])
        self.assertFalse(reasons[0]["near_gate"])

    def test_either_sources_unstable_tail_is_selected(self):
        for changes in [{"current_tail": (6, 7, 9)}, {"control_tail": (8, 10, 12)}]:
            with self.subTest(changes=changes):
                _, reasons = select_read_supplements(self.report(**changes), set())
                self.assertTrue(reasons[0]["unstable_tail"])

    def test_primary_p50_target_selection_is_preserved(self):
        _, reasons = select_read_supplements(
            self.report(workload="label_scan_filter", current_p50=9), {"label_scan_filter"})
        self.assertTrue(reasons[0]["near_gate"])

    def test_incomplete_base_is_not_repeated_as_a_supplement(self):
        self.assertEqual(select_read_supplements(self.report(count=2, current_tail=(8, 10, 12)), set()), ({}, []))

    def test_missing_or_invalid_tail_evidence_is_rejected(self):
        for values in [(10, 10), (10, 10, float("nan")), (0, 10, 10)]:
            with self.subTest(values=values), self.assertRaises(ValueError):
                select_read_supplements(self.report(current_tail=values), set())

    def test_one_workload_cannot_silently_span_profiles(self):
        report = self.report()
        report["processes"][0]["profile"] = "scan"
        with self.assertRaises(ValueError):
            select_read_supplements(report, set())


class WriteTailSelectionTest(unittest.TestCase):
    def report(self, current_tail=(7, 7, 7), current_p50=(7, 7, 7)):
        return {"rows": [{"workload": "update_large", "processes": {
            "control": [{"p50_ms": 10, "p95_ms": 10} for _ in range(3)],
            "current": [{"p50_ms": p50, "p95_ms": p95}
                        for p50, p95 in zip(current_p50, current_tail)]}}]}

    def test_safe_p50_does_not_hide_near_threshold_p95(self):
        reasons = select_write_supplements(self.report(current_tail=(10.6,) * 3))
        self.assertEqual(reasons[0]["workload"], "update_large")
        self.assertTrue(reasons[0]["near_p95_gate"])

    def test_stable_improvement_uses_no_extra_process(self):
        self.assertEqual(select_write_supplements(self.report()), [])

    def test_unstable_tail_is_selected(self):
        self.assertTrue(select_write_supplements(self.report(current_tail=(6, 7, 9)))[0]["unstable_tail"])

    def test_near_p50_protection_gate_is_preserved(self):
        self.assertTrue(select_write_supplements(self.report(current_p50=(10.5,) * 3))[0]["near_gate"])

    def test_incomplete_base_is_not_retried(self):
        self.assertEqual(select_write_supplements(self.report(current_tail=(7, 8))), [])

    def test_invalid_measurements_are_rejected(self):
        for report in [self.report(current_tail=(7, 7, float("nan"))),
                       self.report(current_p50=(7, 7, 0))]:
            with self.subTest(report=report), self.assertRaises(ValueError):
                select_write_supplements(report)


if __name__ == "__main__":
    unittest.main()
