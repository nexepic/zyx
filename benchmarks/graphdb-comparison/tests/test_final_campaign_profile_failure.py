import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from runner.adapters.zyx import ZyxAdapter

class ProfileFailureEvidenceTest(unittest.TestCase):
    def test_process_error_preserves_original_detail_and_raw_streams(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"ZYX_COMPARE_PROFILE_PROCESS":"1"}):
            root=Path(tmp)
            error=json.dumps({"event":"error","workload":"run_all","error":"original admission detail"})+"\n"
            completed=subprocess.CompletedProcess([],1,stdout="",stderr=error)
            adapter=ZyxAdapter("zyx",root/"dataset","smoke",profile="scan")
            with patch("runner.adapters.zyx.subprocess.run",return_value=completed):
                results=adapter.run_all(0,1)
            self.assertEqual(results[0].status,"failed")
            self.assertIn("original admission detail",str(results[0]))
            self.assertEqual((root/"native-profile-stderr.jsonl").read_text(),error)
            self.assertEqual((root/"native-profile-stdout.jsonl").read_text(),"")
