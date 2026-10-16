import collections
import json
from pathlib import Path
import re
import tempfile
import unittest
from runner.workload_preflight import ContractFamilyProgress, edge_setup_queries


class FamilyContractTest(unittest.TestCase):
    def test_thirty_minute_limit_is_checked_per_family(self):
        with tempfile.TemporaryDirectory() as directory:
            now = [0.0]
            budget = ContractFamilyProgress(Path(directory), ("point", "scan"), lambda: now[0])
            now[0] = 1799.0
            self.assertEqual(budget.remaining(), 1.0)
            budget.append("point")
            now[0] = 1801.0
            self.assertEqual(budget.remaining(), 1798.0)
            now[0] = 3599.0
            with self.assertRaisesRegex(TimeoutError, "scan"):
                budget.remaining()

    def test_progress_rejects_wrong_family_and_retains_completed_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            budget = ContractFamilyProgress(Path(directory), ("point", "scan"))
            with self.assertRaises(AssertionError):
                budget.append("scan")
            budget.append("point")
            event = json.loads((Path(directory) / "family-progress.jsonl").read_text())
            self.assertEqual((event["family"], event["completed"], event["total"]), ("point", 1, 2))

    def test_bounded_graph_setup_preserves_parallel_edges_self_loops_and_direction(self):
        edges = [(0, 1), (0, 1), (2, 2), *[(i, i + 1) for i in range(100)]]
        reconstructed = []
        for query in edge_setup_queries(edges):
            bindings = dict(re.findall(r"(n\d+):User \{id:(\d+)\}", query))
            links = re.findall(r"\((n\d+)\)-\[:LINK \{weight:1\}\]->\((n\d+)\)", query)
            self.assertLessEqual(len(links), 32)
            self.assertLessEqual(len(bindings), 64)
            reconstructed.extend((int(bindings[source]), int(bindings[target])) for source, target in links)
        self.assertEqual(collections.Counter(reconstructed), collections.Counter(edges))


if __name__ == "__main__":
    unittest.main()
