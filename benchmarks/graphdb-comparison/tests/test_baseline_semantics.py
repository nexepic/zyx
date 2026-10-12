"""Targeted integration tests for the full-output baseline gate, without engines."""
from collections import Counter
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from dataset.generate import generate_graph, SCALES, write_dataset
from runner import baseline


class BaselineSemanticsTest(unittest.TestCase):
    def run_round(self, output: Path, mode: str) -> tuple[int, list[Path]]:
        temporary = []

        def run(databases, scale, seed, output_root, warmup, iterations, **kwargs):
            temporary.append(output_root)
            result = output_root / "result"
            graph = generate_graph(SCALES["smoke"], seed)
            write_dataset(graph, result / "dataset")
            groups = [[key, count] for key, count in Counter(user["country"] for user in graph.users).items()]
            topk = [[user["id"]] for user in sorted(graph.users, key=lambda user: user["score"], reverse=True)[:100]]
            events = []
            for database in databases:
                for workload, rows in (("aggregation_count_by_group", groups), ("topk_property_sort", topk)):
                    event = {"event": "result", "database": database, "workload": workload,
                             "iteration": 0, "value": len(rows)}
                    if mode != "count-only":
                        event["rows"] = [list(row) for row in rows]
                    if mode == "wrong-groups" and workload == "aggregation_count_by_group":
                        event["rows"][0][1] += 1
                    events.append(event)
            (result / "raw.jsonl").write_text("\n".join(json.dumps(event) for event in events))
            (result / "run_status.json").write_text(json.dumps({"failure_count": 0, "quality_failure_count": 0}))
            return result

        arguments = ["baseline", "--round", "1", "--scale", "smoke", "--profile", "scan",
                     "--iterations", "1", "--warmup", "0", "--output-root", str(output),
                     "--require-full-results"]
        with patch.object(sys, "argv", arguments), patch.object(baseline, "run_benchmark", side_effect=run), \
             patch.dict(baseline.BASELINE_WORKLOADS, {"scan": ["aggregation_count_by_group", "topk_property_sort"]}), \
             contextlib.redirect_stdout(io.StringIO()):
            return baseline.main(), temporary

    def test_full_output_gate_passes_and_exports_only_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            status, temporary = self.run_round(output, "valid")
            self.assertEqual(status, 0)
            exported = output / "smoke-round-1/scan"
            self.assertEqual(json.loads((exported / "full-result-correctness.json").read_text())["failures"], [])
            self.assertFalse((exported / "dataset").exists())
            self.assertTrue(all(not path.exists() for path in temporary))

    def test_count_correct_but_semantics_wrong_fails_and_cleans(self):
        for mode in ("wrong-groups", "count-only"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                output = Path(directory)
                status, temporary = self.run_round(output, mode)
                self.assertEqual(status, 1)
                exported = output / "smoke-round-1/scan"
                self.assertEqual(json.loads((exported / "correctness.json").read_text())["failures"], [])
                self.assertTrue(json.loads((exported / "full-result-correctness.json").read_text())["failures"])
                self.assertTrue(all(not path.exists() for path in temporary))

    def test_invalid_sampling_fails_before_output_or_engine_creation(self):
        for flag, value in (("--iterations", "0"), ("--iterations", "-1"), ("--warmup", "-1")):
            with tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "absent"
                args = ["baseline", "--round", "1", "--output-root", str(output), flag, value]
                with patch.object(sys, "argv", args), patch.object(baseline, "run_benchmark") as run, \
                     contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    baseline.main()
                run.assert_not_called()
                self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
