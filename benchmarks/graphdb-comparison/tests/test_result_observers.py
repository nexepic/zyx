"""Exercise real adapter observation paths with deterministic engine cursors."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runner.adapters.kuzu import KuzuAdapter
from runner.adapters.zyx import ZyxAdapter
from runner.adapters.base import WorkloadResult
from runner.run import run_benchmark
from runner.adapters.base import PROFILE_WORKLOADS


class Cursor:
    def __init__(self, rows):
        self.rows = iter(rows)
        self.current = next(self.rows, None)
        self.reused = []

    def has_next(self):
        return self.current is not None

    def get_next(self):
        self.reused[:] = self.current
        self.current = next(self.rows, None)
        return self.reused


class ResultObserverTest(unittest.TestCase):
    def test_kuzu_retains_each_iteration_actual_rows_not_reused_cursor_buffer(self):
        adapter = KuzuAdapter("kuzu", Path("unused"), "smoke")
        adapter.emit_result_rows = True
        with patch.object(adapter, "_ensure_loaded"), patch.object(adapter, "validate"), \
             patch.object(adapter, "_execute", side_effect=lambda query: Cursor([["CN", 2], ["US", 1]])):
            result = adapter.run_workload("aggregation_count_by_group", 1, 2)
        self.assertEqual(result.status, "ok")
        self.assertEqual([event["rows"] for event in adapter.result_events],
                         [[["CN", 2], ["US", 1]], [["CN", 2], ["US", 1]]])
        self.assertIsNone(adapter.observed_rows)
        self.assertEqual([event["iteration"] for event in adapter.result_events], [0, 1])

    def test_default_kuzu_mode_does_not_retain_rows(self):
        adapter = KuzuAdapter("kuzu", Path("unused"), "smoke")
        with patch.object(adapter, "_ensure_loaded"), patch.object(adapter, "validate"), \
             patch.object(adapter, "_execute", return_value=Cursor([["a"], ["b"]])):
            result = adapter.run_workload("topk_property_sort", 0, 1)
        self.assertEqual(result.status, "ok")
        self.assertNotIn("rows", adapter.result_events[0])

    def test_kuzu_observation_refuses_count_only_fallback(self):
        adapter = KuzuAdapter("kuzu", Path("unused"), "smoke")
        with patch.object(adapter, "_execute", return_value=[["CN", 2]]):
            with self.assertRaisesRegex(TypeError, "row cursor"):
                adapter._row_count("query", observe=True)

    def test_zyx_flag_and_native_rows_survive_subprocess_parser(self):
        for enabled in (False, True):
            adapter = ZyxAdapter("zyx", Path("unused"), "smoke")
            adapter.emit_result_rows = enabled
            event = {"event": "sample", "database": "zyx", "workload": "topk_property_sort",
                     "scale": "smoke", "iteration": 0, "latency_ms": 1.0, "status": "ok",
                     "equivalent_mode": "api"}
            result_event = {"event": "result", "workload": "topk_property_sort", "iteration": 0, "value": 1}
            if enabled:
                result_event["rows"] = [["a"]]
            stdout = json.dumps(event) + "\n" + json.dumps(result_event)
            completed = subprocess.CompletedProcess([], 0, stdout, "")
            with patch("runner.adapters.zyx.subprocess.run", return_value=completed) as run:
                result, _ = adapter._run_workload("topk_property_sort", 0, 1, "warm")
            self.assertEqual(result.status, "ok")
            self.assertEqual("--emit-result-rows" in run.call_args.args[0], enabled)
            self.assertEqual(adapter.result_events, [result_event])

    def test_native_malformed_rows_rejected(self):
        adapter = ZyxAdapter("zyx", Path("unused"), "smoke")
        for rows in (None, {}, ["a"]):
            malformed = []
            event = {"event": "result", "workload": "topk_property_sort", "iteration": 0, "value": 1,
                     "rows": rows}
            adapter._consume_event(event, "topk_property_sort",
                                   WorkloadResult("zyx", "topk_property_sort", "smoke", "ok"),
                                   [], [], malformed, "stdout", 1)
            self.assertTrue(malformed)
        self.assertEqual(adapter.result_events, [])

    def test_runner_discloses_and_rejects_mixed_observation_contracts(self):
        with tempfile.TemporaryDirectory() as directory, \
             patch.dict(PROFILE_WORKLOADS, {"scan": ["label_scan_filter"]}):
            root = Path(directory)
            previous = run_benchmark(["fake"], "smoke", 42, root / "control", 0, 1)
            current = run_benchmark(["fake"], "smoke", 42, root / "current", 0, 1,
                                    baseline_summary=previous / "summary.csv", observe_result_rows=True)
            environment = json.loads((current / "environment.json").read_text())
            self.assertEqual(environment["result_observation"],
                             "full_rows_retained_inside_timing_serialized_afterward")
            report = json.loads((current / "quality_gates.json").read_text())
            gate = next(gate for gate in report["gates"] if gate["name"] == "baseline_environment_provenance")
            self.assertEqual(gate["status"], "failed")
            self.assertIn("result_observation", gate["details"])


if __name__ == "__main__":
    unittest.main()
