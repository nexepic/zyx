import io
import json
from types import SimpleNamespace
from concurrent.futures import ThreadPoolExecutor
import unittest
from runner.workload_preflight import NativeWorker


class FinalCampaignQuotaErrorsTest(unittest.TestCase):
    def invoke(self, error, ok=False):
        worker = object.__new__(NativeWorker)
        worker.process = SimpleNamespace(stdin=io.StringIO(), stdout=io.StringIO(json.dumps({"ok": ok, "error": error}) + "\n"), stderr=io.StringIO())
        worker.reader = ThreadPoolExecutor(max_workers=1); worker.trace = []; worker.evidence = None
        try:
            return worker.call("QUERY", "RETURN 1", error_contains="byte")
        finally:
            worker.reader.shutdown()

    def test_coordinated_resource_denial_is_the_same_defined_quota_outcome(self):
        self.assertFalse(self.invoke("hierarchical resource admission denied")["ok"])

    def test_existing_materialized_byte_error_remains_accepted(self):
        self.assertFalse(self.invoke("shared materialization exceeded its byte bound")["ok"])

    def test_unrelated_failures_and_success_are_never_treated_as_quota_denial(self):
        for error, ok in [("unknown property", False), ("hierarchical resource admission denied", True), ("", True)]:
            with self.subTest(error=error, ok=ok), self.assertRaises(AssertionError):
                self.invoke(error, ok)


if __name__ == "__main__":
    unittest.main()
