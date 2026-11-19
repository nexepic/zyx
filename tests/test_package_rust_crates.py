import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("package_rust_crates", Path(__file__).resolve().parents[1] / "scripts/package_rust_crates.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class PackageRustCratesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "root"
        self.root.mkdir()
        self.files = {
            "Cargo.toml": '[package]\nname="zyx-src"\nversion="0.1.29"\n',
            "bindings/rust/Cargo.toml": '[workspace]\nmembers=["zyxdb-sys","zyxdb"]\nresolver="2"\n',
            "bindings/rust/Cargo.lock": 'version = 4\n',
            "bindings/rust/zyxdb-sys/Cargo.toml": '[package]\nname="zyxdb-sys"\nversion="0.1.29"\n',
            "bindings/rust/zyxdb/Cargo.toml": '[package]\nname="zyxdb"\nversion="0.1.29"\n',
        }
        for name, content in self.files.items():
            p = self.root / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content)
        self.mock = patch.object(m.subprocess, "check_output", return_value=("\0".join(self.files) + "\0").encode())
        self.mock.start()
        self.addCleanup(self.mock.stop)

    def test_staging_does_not_change_checkout(self):
        stage = Path(self.tmp.name) / "stage"
        stage.mkdir()
        versions = m.stage_workspace(self.root, stage)
        self.assertEqual(set(versions.values()), {"0.1.29"})
        self.assertFalse((stage / "bindings/rust/Cargo.toml").exists())
        self.assertIn("[workspace]", (stage / "Cargo.toml").read_text())
        for name, content in self.files.items():
            self.assertEqual((self.root / name).read_text(), content)

    def test_layout_changes_fail_closed(self):
        (self.root / "bindings/rust/Cargo.toml").write_text('[workspace]\nmembers=[]\n')
        with self.assertRaisesRegex(ValueError, "layout changed"):
            m.stage_workspace(self.root, Path(self.tmp.name) / "stage")

    def test_missing_native_library_is_not_a_pass(self):
        with self.assertRaisesRegex(ValueError, "tested libzyx.so"):
            m.validate(self.root, Path(self.tmp.name) / "out", self.root)

    def test_verification_failure_publishes_no_artifacts(self):
        (self.root / "libzyx.so").touch()
        with patch.object(m.subprocess, "run", side_effect=m.subprocess.CalledProcessError(1, "cargo")) as run:
            output = Path(self.tmp.name) / "out"
            with self.assertRaises(m.subprocess.CalledProcessError):
                m.validate(self.root, output, self.root)
            self.assertFalse(output.exists())
            cmd = run.call_args.args[0]
            self.assertEqual(cmd[:3], ["cargo", "package", "--workspace"])
            self.assertNotIn("--no-verify", cmd)
            self.assertTrue(run.call_args.kwargs["check"])


    def test_success_requires_archive_tests_before_export(self):
        (self.root / "libzyx.so").touch()
        calls = []
        def run(command, *, cwd, env, check):
            self.assertTrue(check)
            calls.append(command)
            if command[1] == "package":
                packages = Path(env["CARGO_TARGET_DIR"]) / "package"
                packages.mkdir(parents=True)
                for name in m.PACKAGES:
                    (packages / f"{name}-0.1.29.crate").write_bytes(b"fixture")
        output = Path(self.tmp.name) / "out"
        with patch.object(m.subprocess, "run", side_effect=run):
            m.validate(self.root, output, self.root)
        self.assertEqual([c[1] for c in calls], ["package", "test"])
        self.assertEqual(calls[1][-2:], ["--", "--test-threads=1"])
        self.assertIn("target/package/zyxdb-0.1.29/Cargo.toml", calls[1][3])
        self.assertEqual(len(list(output.glob("*.crate"))), 3)

    def test_existing_output_is_rejected(self):
        (self.root / "libzyx.so").touch()
        with self.assertRaisesRegex(ValueError, "new directory"):
            m.validate(self.root, self.root, self.root)


if __name__ == "__main__":
    unittest.main()
