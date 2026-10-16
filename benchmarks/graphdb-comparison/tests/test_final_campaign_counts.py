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


if __name__ == "__main__":
    unittest.main()
