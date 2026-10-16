from pathlib import Path
from types import SimpleNamespace
import os
import unittest
from unittest.mock import patch
from runner.adapters.kuzu import KuzuAdapter


class KuzuResourceOptionsTest(unittest.TestCase):
    def setUp(self):
        self.adapter = KuzuAdapter("kuzu", Path("/tmp/dataset"), "smoke", threads=1)

    def test_explicit_pool_and_threads_are_both_forwarded(self):
        calls = []
        module = SimpleNamespace(Database=lambda path, **options: calls.append((path, options)))
        with patch.dict(os.environ, {"KUZU_BENCH_BUFFER_POOL_BYTES": "67108864"}):
            self.adapter._open_database(module)
        self.assertEqual(calls, [("/tmp/kuzu.db", {"buffer_pool_size": 67108864, "max_num_threads": 1})])

    def test_unsupported_explicit_cap_does_not_fall_back(self):
        module = SimpleNamespace(Database=lambda path: object())
        with patch.dict(os.environ, {"KUZU_BENCH_BUFFER_POOL_BYTES": "67108864"}):
            with self.assertRaises(TypeError):
                self.adapter._open_database(module)

    def test_invalid_limits_fail_before_opening(self):
        calls = []
        module = SimpleNamespace(Database=lambda path, **options: calls.append(path))
        for value in ("0", "-1", "invalid"):
            with self.subTest(value=value), patch.dict(os.environ, {"KUZU_BENCH_BUFFER_POOL_BYTES": value}):
                with self.assertRaises(ValueError):
                    self.adapter._open_database(module)
        self.assertEqual(calls, [])

    def test_virtual_database_mapping_cap_is_forwarded_with_pool_cap(self):
        calls = []
        module = SimpleNamespace(Database=lambda path, **options: calls.append(options))
        with patch.dict(os.environ, {"KUZU_BENCH_BUFFER_POOL_BYTES": "67108864", "KUZU_BENCH_MAX_DB_BYTES": "2147483648"}):
            self.adapter._open_database(module)
        self.assertEqual(calls, [{"buffer_pool_size": 67108864, "max_db_size": 2147483648, "max_num_threads": 1}])

    def test_invalid_mapping_limit_fails_before_opening(self):
        module = SimpleNamespace(Database=lambda path, **options: self.fail("opened invalid database"))
        with patch.dict(os.environ, {"KUZU_BENCH_MAX_DB_BYTES": "0"}):
            with self.assertRaises(ValueError):
                self.adapter._open_database(module)

    def test_legacy_thread_fallback_remains_available_without_explicit_cap(self):
        module = SimpleNamespace(Database=lambda path: path)
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(self.adapter._open_database(module), "/tmp/kuzu.db")


if __name__ == "__main__":
    unittest.main()
