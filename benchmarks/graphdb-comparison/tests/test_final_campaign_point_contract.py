import random
import unittest
from runner.workload_preflight import point_probe_ids


class PointContractTest(unittest.TestCase):
    def test_full_profile_checks_thousand_hits_and_ten_percent_misses(self):
        values = point_probe_ids(10000, "contract", random.Random(42))
        hits = [value for value in values if value < 10000]
        misses = [value for value in values if value >= 10000]
        self.assertEqual(len(hits), 1000)
        self.assertEqual(len(misses), 100)
        self.assertEqual(len(set(values)), 1100)
        self.assertEqual(values, point_probe_ids(10000, "contract", random.Random(42)))

    def test_smoke_retains_a_bounded_hit_and_missing_oracle(self):
        values = point_probe_ids(32, "smoke", random.Random(42))
        self.assertEqual(sum(value < 32 for value in values), 10)
        self.assertEqual(sum(value >= 32 for value in values), 1)


if __name__ == "__main__":
    unittest.main()
