"""Contract admission checks; this module uses only the Python standard library."""
import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runner.optimization_contract import validate_contract


CONTRACT = (Path(__file__).resolve().parents[2] / "architecture-design" / "decisions" /
            "20261011-s0-workload-contract" / "contract.json")


class OptimizationContractTest(unittest.TestCase):
    def setUp(self):
        self.contract = json.loads(CONTRACT.read_text())

    def test_preregistered_contract_covers_all_required_families(self):
        self.assertEqual(validate_contract(self.contract), [])

    def test_removing_a_family_is_rejected(self):
        for family in {item["family"] for item in self.contract["workloads"]}:
            with self.subTest(family=family):
                contract = copy.deepcopy(self.contract)
                contract["workloads"] = [item for item in contract["workloads"] if item["family"] != family]
                self.assertTrue(any("missing families" in error for error in validate_contract(contract)))

    def test_duplicate_id_is_rejected(self):
        self.contract["workloads"].append(self.contract["workloads"][0])
        self.assertTrue(any("duplicate" in error for error in validate_contract(self.contract)))

    def test_resource_limits_reject_boolean_zero_negative_and_fraction(self):
        for key in ("container_memory_bytes", "query_materialization_bytes", "threads", "cpu_quota"):
            for invalid in (True, 0, -1, 0.5, None):
                with self.subTest(key=key, invalid=invalid):
                    contract = copy.deepcopy(self.contract)
                    contract["resources"][key] = invalid
                    self.assertTrue(validate_contract(contract))

    def test_sampling_cannot_silently_lower_formal_minimum(self):
        for key, value in (("processes", 2), ("warmups", 2), ("measurements", 9)):
            contract = copy.deepcopy(self.contract)
            contract["sampling"][key] = value
            self.assertTrue(validate_contract(contract))

    def test_missing_oracle_and_timing_are_rejected(self):
        for field in ("oracle", "timing", "inputs", "operation", "metrics"):
            contract = copy.deepcopy(self.contract)
            del contract["workloads"][0][field]
            self.assertTrue(validate_contract(contract))

    def test_existing_workload_requires_execution_entry(self):
        self.contract["workloads"][0]["entry"] = ""
        self.assertTrue(validate_contract(self.contract))

    def test_pending_is_valid_but_not_converted_to_existing(self):
        before = copy.deepcopy(self.contract)
        self.assertTrue(any(item["implementation"] == "pending" for item in before["workloads"]))
        self.assertEqual(validate_contract(self.contract), [])
        self.assertEqual(before, self.contract)

    def test_malformed_containers_report_errors(self):
        for field in ("resources", "sampling", "workloads"):
            contract = copy.deepcopy(self.contract)
            contract[field] = None
            self.assertTrue(validate_contract(contract))


if __name__ == "__main__":
    unittest.main()
