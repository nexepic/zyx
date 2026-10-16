import csv
import json
from pathlib import Path
import tempfile
import unittest
from runner.baseline import verify_counts


class FinalCampaignCountsTest(unittest.TestCase):
    def test_independent_process_only_checks_its_registered_engine(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); dataset = root / "dataset"; dataset.mkdir()
            for name, fields, rows in (
                ("users.csv", ["id", "country", "age", "score"], [["user-000001", "CN", 30, 900]]),
                ("posts.csv", ["id", "score"], []),
                ("follows.csv", ["src", "dst", "weight"], []),
            ):
                with (dataset / name).open("w") as stream:
                    writer = csv.writer(stream); writer.writerow(fields); writer.writerows(rows)
            event = {"event": "result", "database": "zyx", "workload": "label_scan_filter", "iteration": 0, "value": 1}
            (root / "raw.jsonl").write_text(json.dumps(event) + "\n")
            self.assertEqual(verify_counts(root, ["label_scan_filter"], 1, ["zyx"])["failures"], [])
            self.assertTrue(verify_counts(root, ["label_scan_filter"], 1)["failures"])
            event["value"] = 2; (root / "raw.jsonl").write_text(json.dumps(event) + "\n")
            self.assertTrue(verify_counts(root, ["label_scan_filter"], 1, ["zyx"])["failures"])


class FinalCampaignPreparationOptionsTest(unittest.TestCase):
    def invoke(self, environment):
        from runner.adapters.zyx import ZyxAdapter
        from unittest.mock import patch
        from types import SimpleNamespace
        import os
        adapter = ZyxAdapter("zyx", Path("/tmp/dataset"), "smoke", threads=1)
        with patch.dict(os.environ, environment, clear=True), patch("runner.adapters.zyx.subprocess.run", return_value=SimpleNamespace(returncode=1, stdout="", stderr="test probe")) as run:
            adapter._run_workload("label_scan_filter", warmup=3, iterations=10, execution_mode="warm")
            return run.call_args.args[0]

    def test_bounded_preparation_is_an_explicit_option(self):
        args = self.invoke({"ZYX_COMPARE_LOAD_BATCH_ROWS": "1000"})
        self.assertEqual(args[args.index("--load-batch-rows") + 1], "1000")
        self.assertNotIn("--load-batch-rows", self.invoke({}))

    def test_invalid_batch_is_rejected_before_launch(self):
        for value in ("0", "-1", "invalid"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.invoke({"ZYX_COMPARE_LOAD_BATCH_ROWS": value})


if __name__ == "__main__":
    unittest.main()
