import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runner.workload_preflight import compare_rows, cypher, typed
from dataset.generate import SCALES, generate_graph, write_dataset


class WorkloadPreflightTest(unittest.TestCase):
    def test_full_oracle_rejects_correct_count_wrong_values_types_and_multiplicity(self):
        for actual in ([[typed(2)]], [[typed("1")]], [[typed(True)]], [[typed(1)], [typed(1)]]):
            with self.assertRaises(AssertionError):
                compare_rows(actual, [[1]])

    def test_unordered_oracle_preserves_duplicates_and_ordered_oracle_checks_sequence(self):
        compare_rows([[typed(2)], [typed(1)], [typed(1)]], [[1], [2], [1]])
        with self.assertRaises(AssertionError):
            compare_rows([[typed(2)], [typed(1)]], [[1], [2]], ordered=True)

    def test_map_oracle_distinguishes_absent_key_from_explicit_null(self):
        with self.assertRaises(AssertionError):
            compare_rows([[typed({})]], [[{"missing": None}]])
        compare_rows([[typed({"id": 1, "value": [None, True, "x"]})]], [[{"value": [None, True, "x"], "id": 1}]])

    def test_literal_encoder_preserves_identifier_and_string_source_spelling(self):
        self.assertEqual(cypher({"id": 1, "body": "a'b\\c"}), "{id:1,body:'a\\'b\\\\c'}")
        self.assertEqual(cypher({"tick`key": 1, "with space": 2}), "{`tick``key`:1,`with space`:2}")

    def test_holdout_profile_matches_registered_dimensions(self):
        config = SCALES["holdout"]
        self.assertEqual((config.users, config.posts, config.tags, config.follows_per_user), (73000, 87000, 3000, 7))

    def test_manifest_authenticates_every_csv_and_schema(self):
        from dataset.generate import ScaleConfig
        graph = generate_graph(ScaleConfig("unit", 3, 2, 1, 2, 1), seed=314159)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); manifest = write_dataset(graph, root)
            self.assertEqual(manifest["schema"]["User"]["score"], "double")
            self.assertEqual(len(manifest["files"]), 6)
            for name, proof in manifest["files"].items():
                self.assertEqual(proof["sha256"], hashlib.sha256((root / name).read_bytes()).hexdigest())
                self.assertEqual(proof["bytes"], (root / name).stat().st_size)
            self.assertEqual(json.loads((root / "manifest.json").read_text()), manifest)


if __name__ == "__main__":
    unittest.main()
