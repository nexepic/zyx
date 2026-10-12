#!/usr/bin/env python3
"""Freeze actual tracked working-tree bytes, including uncommitted edits.

Run before production changes. The output directory must not already exist.
Ignored build products are excluded; explicitly supplied binaries are copied.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import platform
import shutil
import subprocess
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def freeze(root: Path, output: Path, binaries: list[Path]) -> None:
    root = root.resolve()
    output = output.resolve()
    paths = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=root
    ).decode().split("\0")
    status = subprocess.check_output(["git", "status", "--porcelain=v1"], cwd=root).decode()
    # Validate before creating any output; never overwrite an existing baseline.
    for binary in binaries:
        if not binary.is_file():
            raise FileNotFoundError(binary)
    output.mkdir(parents=True, exist_ok=False)
    records = []
    for name in sorted(set(paths) - {""}):
        source = root / name
        if not source.is_file():
            records.append({"path": name, "state": "deleted"})
            continue
        target = output / "source" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        source_hash = digest(source)
        if digest(target) != source_hash:
            raise RuntimeError(f"source changed during freeze: {name}")
        records.append({"path": name, "sha256": source_hash, "bytes": source.stat().st_size})
    artifacts = []
    for index, binary in enumerate(binaries):
        target = output / "binaries" / str(index) / binary.name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(binary, target)
        artifacts.append({"original": str(binary.resolve()), "copy": str(target.relative_to(output)),
                          "sha256": digest(target)})
    manifest = {"task": "S0.1", "root": str(root), "head": subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=root).decode().strip(), "status": status,
        "platform": platform.platform(), "machine": platform.machine(),
        "source": records, "binaries": artifacts,
        "limitations": ["Build provenance must be collected separately; a copied binary is not proven to match source.",
                        "Ignored dependencies and build files are not source inputs in this manifest."]}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--binary", type=Path, action="append", default=[])
    args = parser.parse_args()
    freeze(args.root, args.output, args.binary)
