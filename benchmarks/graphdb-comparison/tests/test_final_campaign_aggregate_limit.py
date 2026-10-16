import re
import unittest
from runner.workload_preflight import validate_high_cardinality_aggregate


class AggregateLimitTest(unittest.TestCase):
    def test_success_keeps_full_output_oracle(self):
        class Worker:
            def call(self, command, query, expected):
                self.expected = expected
        worker = Worker()
        unsupported = []
        validate_high_cardinality_aggregate(worker, 32, unsupported)
        self.assertEqual(worker.expected, [[i, 1] for i in range(32)])
        self.assertEqual(unsupported, [])

    def test_known_limit_is_na_and_every_group_is_checked_in_bounded_partitions(self):
        seen = []
        outer = self
        class Worker:
            calls = 0
            def call(self, command, query, expected):
                self.calls += 1
                if self.calls == 1:
                    raise RuntimeError('hierarchical resource admission denied')
                if self.calls == 2:
                    outer.assertEqual(expected, [[100000]])
                    return
                lower, upper = map(int, re.search(r'n.id >= (\d+) AND n.id < (\d+)', query).groups())
                outer.assertEqual(expected, [[i, 1] for i in range(lower, upper)])
                seen.extend(row[0] for row in expected)
        worker = Worker()
        unsupported = []
        validate_high_cardinality_aggregate(worker, 100000, unsupported)
        self.assertEqual(worker.calls, 102)
        self.assertEqual(seen, list(range(100000)))
        self.assertEqual(unsupported[0]['latency_comparison'], 'N/A')
        self.assertEqual(unsupported[0]['status'], 'unsupported_at_fixed_query_budget')

    def test_other_failures_and_small_group_denials_remain_failures(self):
        class Worker:
            def __init__(self, error): self.error = error
            def call(self, *args, **kwargs): raise RuntimeError(self.error)
        for count, message in [(32, 'hierarchical resource admission denied'),
                               (100000, 'bad allocation'), (100000, 'timeout'),
                               (100000, 'page authentication failed')]:
            with self.subTest(count=count, message=message):
                unsupported = []
                with self.assertRaisesRegex(RuntimeError, message):
                    validate_high_cardinality_aggregate(Worker(message), count, unsupported)
                self.assertEqual(unsupported, [])

    def test_partition_oracle_failure_is_not_accepted(self):
        class Worker:
            calls = 0
            def call(self, *args, **kwargs):
                self.calls += 1
                if self.calls == 1: raise RuntimeError('hierarchical resource admission denied')
                if self.calls == 3: raise AssertionError('wrong group count')
        with self.assertRaisesRegex(AssertionError, 'wrong group count'):
            validate_high_cardinality_aggregate(Worker(), 100000, [])
