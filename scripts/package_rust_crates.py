#!/usr/bin/env python3
"""Verify unpublished sibling crates together without publishing or editing the checkout.

The temporary workspace changes only workspace membership. Cargo normalizes it
out of published manifests, stages local dependencies, and verifies each archive.
The extracted SDK tests additionally link/run against the already-tested native
library. This validates the system feature, not a bundled native rebuild.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import tomllib

PACKAGES = {"zyx-src": "", "zyxdb-sys": "bindings/rust/zyxdb-sys", "zyxdb": "bindings/rust/zyxdb"}
ROOTS = ("Cargo.toml", "README.md", "LICENSE", "CMakeLists.txt", "conanfile.py", "cmake", "include", "src", "lib/inputxx", "crates/zyx-src", "bindings/rust")


def stage_workspace(root, stage):
    tracked = subprocess.check_output(["git", "ls-files", "-z", "--", *ROOTS], cwd=root).decode().split("\0")
    for name in filter(None, tracked):
        source, destination = root / name, stage / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
    manifest = stage / "Cargo.toml"
    if "workspace" in tomllib.loads(manifest.read_text()):
        raise ValueError("root workspace layout changed; review staging")
    nested = stage / "bindings/rust/Cargo.toml"
    if tomllib.loads(nested.read_text()) != {"workspace": {"members": ["zyxdb-sys", "zyxdb"], "resolver": "2"}}:
        raise ValueError("nested workspace layout changed; review staging")
    nested.unlink()
    manifest.write_text(manifest.read_text() + '\n[workspace]\nmembers = ["bindings/rust/zyxdb-sys", "bindings/rust/zyxdb"]\nresolver = "2"\n')
    shutil.copyfile(stage / "bindings/rust/Cargo.lock", stage / "Cargo.lock")
    versions = {name: tomllib.loads((stage / path / "Cargo.toml").read_text())["package"]["version"] for name, path in PACKAGES.items()}
    if len(set(versions.values())) != 1:
        raise ValueError("crate versions must agree")
    return versions


def validate(root, output, lib_dir):
    if not (lib_dir / "libzyx.so").is_file():
        raise ValueError("Linux package validation requires the tested libzyx.so")
    if output.exists():
        raise ValueError("output must be a new directory, to exclude stale packages")
    with tempfile.TemporaryDirectory(prefix="zyx-rust-package-") as temporary:
        stage = Path(temporary)
        versions = stage_workspace(root, stage)
        env = dict(os.environ, ZYX_LIB_DIR=str(lib_dir), CARGO_TARGET_DIR=str(stage / "target"))
        subprocess.run(["cargo", "package", "--workspace", "--no-default-features", "--features", "zyxdb/system,zyxdb-sys/system"], cwd=stage, env=env, check=True)
        extracted = stage / "target/package"
        patches = []
        for name in ("zyx-src", "zyxdb-sys"):
            patches += ["--config", f'patch.crates-io.{name}.path={json.dumps(str(extracted / (name + "-" + versions[name])))}']
        subprocess.run(["cargo", "test", "--manifest-path", str(extracted / ("zyxdb-" + versions["zyxdb"]) / "Cargo.toml"), "--no-default-features", "--features", "system", *patches], cwd=stage, env=env, check=True)
        output.mkdir(parents=True)
        for name, version in versions.items():
            archive = extracted / f"{name}-{version}.crate"
            if not archive.is_file():
                raise ValueError(f"missing verified archive: {archive}")
            shutil.copyfile(archive, output / archive.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lib-dir", type=Path, required=True)
    args = parser.parse_args()
    validate(Path(__file__).resolve().parents[1], args.output.resolve(), args.lib_dir.resolve())


if __name__ == "__main__":
    main()
