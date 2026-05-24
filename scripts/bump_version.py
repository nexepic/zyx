#!/usr/bin/env python3
"""Bump project version across all tracked files.

Usage:
    python scripts/bump_version.py v1.0.0
    python scripts/bump_version.py v1.0.0 --no-commit
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

CMAKE_LISTS = ROOT / "CMakeLists.txt"

PACKAGE_JSONS = [
    ROOT / "docs" / "package.json",
    ROOT / "docs" / "apps" / "docs" / "package.json",
    ROOT / "docs" / "packages" / "core" / "package.json",
    ROOT / "docs" / "packages" / "config" / "package.json",
    ROOT / "bindings" / "nodejs" / "package.json",
    ROOT / "bindings" / "nodejs" / "npm" / "linux-x64-gnu" / "package.json",
    ROOT / "bindings" / "nodejs" / "npm" / "darwin-arm64" / "package.json",
    ROOT / "bindings" / "nodejs" / "npm" / "win32-x64-msvc" / "package.json",
]

RUST_MANIFESTS = [
    ROOT / "bindings" / "rust" / "zyxdb" / "Cargo.toml",
    ROOT / "bindings" / "rust" / "zyxdb-sys" / "Cargo.toml",
]
RUST_LOCKFILE = ROOT / "bindings" / "rust" / "Cargo.lock"

VERSION_RE = re.compile(r"^v(\d+\.\d+\.\d+)$")
CMAKE_PROJECT_RE = re.compile(
    r"(project\s*\(\s*zyx\b(?:(?!\)).)*?\bVERSION\s+)(\d+\.\d+\.\d+)((?:(?!\)).)*\))",
    re.IGNORECASE | re.DOTALL,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Bump ZYX version")
    parser.add_argument("version", help="Version tag, e.g. v1.0.0")
    parser.add_argument(
        "--no-commit",
        action="store_true",
        help="Only modify files, skip git commit and tag",
    )
    return parser.parse_args()


def validate_version(tag: str) -> str:
    """Validate vX.Y.Z format and return the numeric part."""
    m = VERSION_RE.match(tag)
    if not m:
        print(f"Error: version must match vX.Y.Z (got '{tag}')", file=sys.stderr)
        sys.exit(1)
    return m.group(1)


def update_cmake_lists(version: str) -> None:
    text = CMAKE_LISTS.read_text(encoding="utf-8")
    new_text, count = CMAKE_PROJECT_RE.subn(rf"\g<1>{version}\g<3>", text, count=1)
    if count == 0:
        print("Error: could not find project(zyx VERSION ...) in CMakeLists.txt", file=sys.stderr)
        sys.exit(1)
    CMAKE_LISTS.write_text(new_text, encoding="utf-8")


def update_package_json(path: Path, version: str) -> None:
    data = json.loads(path.read_text(encoding="utf-8"))
    data["version"] = version
    if "optionalDependencies" in data:
        for dep in data["optionalDependencies"]:
            data["optionalDependencies"][dep] = version
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def update_cargo_toml(path: Path, version: str) -> None:
    text = path.read_text(encoding="utf-8")
    new_text, count = re.subn(
        r'(?m)^(version\s*=\s*)"[^"]+"',
        rf'\1"{version}"',
        text,
        count=1,
    )
    if count == 0:
        print(f"Error: could not find package version in {path.relative_to(ROOT)}", file=sys.stderr)
        sys.exit(1)
    new_text = re.sub(
        r'(zyxdb-sys\s*=\s*\{[^}]*version\s*=\s*)"[^"]+"',
        rf'\1"{version}"',
        new_text,
        count=1,
    )
    path.write_text(new_text, encoding="utf-8")


def update_cargo_lock(version: str) -> None:
    if not RUST_LOCKFILE.exists():
        return
    text = RUST_LOCKFILE.read_text(encoding="utf-8")
    for package in ("zyxdb", "zyxdb-sys"):
        pattern = re.compile(
            rf'(\[\[package\]\]\nname = "{re.escape(package)}"\nversion = )"[^"]+"',
            re.MULTILINE,
        )
        text, count = pattern.subn(rf'\1"{version}"', text, count=1)
        if count == 0:
            print(f"Error: could not find {package} in {RUST_LOCKFILE.relative_to(ROOT)}", file=sys.stderr)
            sys.exit(1)
    RUST_LOCKFILE.write_text(text, encoding="utf-8")


def read_cargo_toml_version(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    m = re.search(r'(?m)^version\s*=\s*"([^"]+)"', text)
    if not m:
        print(f"Error: could not read package version from {path.relative_to(ROOT)}", file=sys.stderr)
        sys.exit(1)
    return m.group(1)


def read_cmake_version() -> str:
    text = CMAKE_LISTS.read_text(encoding="utf-8")
    m = CMAKE_PROJECT_RE.search(text)
    if not m:
        print("Error: could not read version from CMakeLists.txt", file=sys.stderr)
        sys.exit(1)
    return m.group(2)


def read_package_json_version(path: Path) -> str:
    data = json.loads(path.read_text(encoding="utf-8"))
    return data["version"]


def verify_consistency(version: str) -> None:
    errors = []
    actual = read_cmake_version()
    if actual != version:
        errors.append(f"  CMakeLists.txt: expected {version}, got {actual}")
    for path in PACKAGE_JSONS:
        actual = read_package_json_version(path)
        if actual != version:
            errors.append(f"  {path.relative_to(ROOT)}: expected {version}, got {actual}")
    for path in RUST_MANIFESTS:
        actual = read_cargo_toml_version(path)
        if actual != version:
            errors.append(f"  {path.relative_to(ROOT)}: expected {version}, got {actual}")
    if errors:
        print("Version consistency check FAILED:", file=sys.stderr)
        for e in errors:
            print(e, file=sys.stderr)
        sys.exit(1)


def git_commit_and_tag(tag: str, files: list[Path]) -> None:
    rel_paths = [str(p.relative_to(ROOT)) for p in files]
    subprocess.run(["git", "add"] + rel_paths, cwd=ROOT, check=True)
    subprocess.run(
        ["git", "commit", "-m", f"Bump version to {tag}"],
        cwd=ROOT,
        check=True,
    )
    subprocess.run(["git", "tag", tag], cwd=ROOT, check=True)


def main() -> None:
    args = parse_args()
    tag = args.version
    version = validate_version(tag)

    print(f"Bumping version to {tag} ({version}) ...")

    update_cmake_lists(version)
    for path in PACKAGE_JSONS:
        update_package_json(path, version)
    for path in RUST_MANIFESTS:
        update_cargo_toml(path, version)
    update_cargo_lock(version)

    verify_consistency(version)
    print("All files updated and verified.")

    changed_files = [CMAKE_LISTS] + PACKAGE_JSONS + RUST_MANIFESTS
    if RUST_LOCKFILE.exists():
        changed_files.append(RUST_LOCKFILE)
    if args.no_commit:
        print("Skipping git commit and tag (--no-commit).")
    else:
        git_commit_and_tag(tag, changed_files)
        print(f"Committed and tagged {tag}.")
        print("Done! Run 'git push && git push --tags' to publish.")


if __name__ == "__main__":
    main()
