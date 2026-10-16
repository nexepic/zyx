import json
import os
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from runner.adapters.zyx import ZyxAdapter


class FinalCampaignProfileProcessTest(unittest.TestCase):
    names = ["label_scan_filter", "aggregation_count_by_group"]

    def invoke(self, setting="1", extra=None):
        events = []
        for name in self.names:
            events.extend([{"event": "sample", "database": "zyx", "workload": name,
                            "scale": "smoke", "iteration": 0, "latency_ms": 1.0,
                            "status": "ok", "equivalent_mode": "api"},
                           {"event": "result", "workload": name, "iteration": 0, "value": 2}])
        if extra is not None:
            events.append(extra)
        completed = SimpleNamespace(returncode=0, stdout="\n".join(map(json.dumps, events)), stderr="")
        with patch.dict(os.environ, {"ZYX_COMPARE_PROFILE_PROCESS": setting}, clear=True), \
             patch.dict("runner.adapters.zyx.PROFILE_WORKLOADS", {"scan": self.names}), \
             patch("runner.adapters.zyx.subprocess.run", return_value=completed) as run:
            adapter = ZyxAdapter("zyx", Path("/tmp/dataset"), "smoke")
            results = adapter.run_all(3, 1)
            return results, run, adapter

    def test_one_preparation_keeps_every_workload_sample_and_observation(self):
        results, run, adapter = self.invoke()
        self.assertEqual(run.call_count, 1)
        self.assertEqual([r.workload for r in results], self.names)
        self.assertTrue(all(r.status == "ok" and len(r.samples) == 1 for r in results))
        self.assertEqual(len(adapter.result_events), 2)
        args = run.call_args.args[0]
        self.assertEqual(args[args.index("--workloads") + 1], ",".join(self.names))
        self.assertNotIn("--workload", args)

    def test_later_workload_error_is_not_hidden(self):
        results, run, _ = self.invoke(extra={"event": "error", "workload": self.names[1], "error": "injected"})
        self.assertEqual(run.call_count, 1)
        self.assertEqual([r.status for r in results], ["ok", "failed"])

    def test_unregistered_events_are_rejected(self):
        results, _, _ = self.invoke(extra={"event": "result", "workload": "unknown", "iteration": 0, "value": 2})
        self.assertEqual(results[0].status, "failed")

    def test_ambiguous_mode_is_rejected(self):
        with self.assertRaises(ValueError):
            self.invoke(setting="yes")


if __name__ == "__main__":
    unittest.main()
