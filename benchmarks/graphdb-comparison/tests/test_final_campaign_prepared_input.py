import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from runner.adapters.zyx import ZyxAdapter


class FinalCampaignPreparedInputTest(unittest.TestCase):
    def invoke(self, changes=None, workload="label_scan_filter", profile="scan"):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);dataset=root/"dataset";dataset.mkdir()
            (dataset/"users.csv").write_text("id,score\nu1,1\n")
            source=root/"prepared.zyx";source.write_bytes(b"immutable prepared fixture")
            binary=root/"benchmark";binary.write_bytes(b"frozen consumer binary")
            manifest={"scale":"smoke","consumer_binary_sha256":ZyxAdapter._digest(binary),
                      "database_sha256":ZyxAdapter._digest(source),
                      "dataset_sha256":{"users.csv":ZyxAdapter._digest(dataset/"users.csv")}}
            manifest.update(changes or {})
            Path(str(source)+".json").write_text(json.dumps(manifest))
            with patch.dict(os.environ,{"ZYX_COMPARE_BENCH":str(binary),"ZYX_COMPARE_PREPARED_DB":str(source)},clear=True), \
                 patch("runner.adapters.zyx.subprocess.run",return_value=SimpleNamespace(returncode=1,stdout="",stderr="probe")) as run:
                adapter=ZyxAdapter("zyx",dataset,"smoke",profile=profile)
                try:
                    adapter._run_workload(workload,0,1,"warm")
                except (ValueError,OSError):
                    run.assert_not_called()
                    raise
                return run.call_args.args[0],json.loads((root/"prepared-input.json").read_text())

    def test_exact_frozen_fixture_is_forwarded_with_provenance(self):
        command,proof=self.invoke()
        self.assertIn("--prepared-db",command)
        self.assertEqual(proof["manifest"]["scale"],"smoke")
        self.assertEqual(len(proof["manifest"]["database_sha256"]),64)

    def test_mismatched_binary_dataset_database_and_scale_fail_before_launch(self):
        for key,value in [("consumer_binary_sha256","wrong"),("database_sha256","wrong"),
                          ("dataset_sha256",{}),("scale","other")]:
            with self.subTest(key=key),self.assertRaises(ValueError):
                self.invoke({key:value})

    def test_import_cannot_use_a_prepared_fixture(self):
        with self.assertRaises(ValueError):self.invoke(workload="load_nodes_edges")

    def test_mutation_profile_cannot_use_a_prepared_fixture(self):
        with self.assertRaises(ValueError):self.invoke(workload="point_create_node",profile="write")


if __name__=="__main__":unittest.main()
