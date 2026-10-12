import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("freeze", Path(__file__).with_name("freeze_optimization_source.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class FreezeSourceTest(unittest.TestCase):
    def test_dirty_deleted_untracked_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "repo"
            root.mkdir()
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / "edited.cpp").write_text("original")
            (root / "deleted.cpp").write_text("deleted")
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            (root / "edited.cpp").write_text("dirty")
            (root / "deleted.cpp").unlink()
            (root / "new.cpp").write_text("new")
            output = Path(directory) / "snapshot"
            check_output = subprocess.check_output
            def fixture_output(command, **kwargs):
                if command == ["git", "rev-parse", "HEAD"]:
                    return b"fixture-head\n"
                return check_output(command, **kwargs)
            with patch.object(module.subprocess, "check_output", side_effect=fixture_output):
                module.freeze(root, output, [])
            manifest = json.loads((output / "manifest.json").read_text())
            entries = {item["path"]: item for item in manifest["source"]}
            self.assertEqual((output / "source/edited.cpp").read_text(), "dirty")
            self.assertEqual(entries["deleted.cpp"]["state"], "deleted")
            self.assertEqual(entries["new.cpp"]["sha256"], module.digest(root / "new.cpp"))
            with self.assertRaises(FileExistsError):
                module.freeze(root, output, [])


if __name__ == "__main__":
    unittest.main()
