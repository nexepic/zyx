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

MESON_BUILD = ROOT / "meson.build"

PACKAGE_JSONS = [
    ROOT / "docs" / "package.json",
    ROOT / "docs" / "apps" / "docs" / "package.json",
    ROOT / "docs" / "packages" / "core" / "package.json",
    ROOT / "docs" / "packages" / "config" / "package.json",
]

VERSION_RE = re.compile(r"^v(\d+\.\d+\.\d+)$")
MESON_VERSION_RE = re.compile(r"(version\s*:\s*')(\d+\.\d+\.\d+)(')")


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


def update_meson_build(version: str) -> None:
    text = MESON_BUILD.read_text(encoding="utf-8")
    new_text, count = MESON_VERSION_RE.subn(rf"\g<1>{version}\g<3>", text, count=1)
    if count == 0:
        print("Error: could not find version in meson.build", file=sys.stderr)
        sys.exit(1)
    MESON_BUILD.write_text(new_text, encoding="utf-8")


def update_package_json(path: Path, version: str) -> None:
    data = json.loads(path.read_text(encoding="utf-8"))
    data["version"] = version
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def read_meson_version() -> str:
    text = MESON_BUILD.read_text(encoding="utf-8")
    m = MESON_VERSION_RE.search(text)
    if not m:
        print("Error: could not read version from meson.build", file=sys.stderr)
        sys.exit(1)
    return m.group(2)


def read_package_json_version(path: Path) -> str:
    data = json.loads(path.read_text(encoding="utf-8"))
    return data["version"]


def verify_consistency(version: str) -> None:
    errors = []
    actual = read_meson_version()
    if actual != version:
        errors.append(f"  meson.build: expected {version}, got {actual}")
    for path in PACKAGE_JSONS:
        actual = read_package_json_version(path)
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

    # Update files
    update_meson_build(version)
    for path in PACKAGE_JSONS:
        update_package_json(path, version)

    # Verify consistency
    verify_consistency(version)
    print("All files updated and verified.")

    # Git commit + tag
    changed_files = [MESON_BUILD] + PACKAGE_JSONS
    if args.no_commit:
        print("Skipping git commit and tag (--no-commit).")
    else:
        git_commit_and_tag(tag, changed_files)
        print(f"Committed and tagged {tag}.")
        print(f"Done! Run 'git push && git push --tags' to publish.")


if __name__ == "__main__":
    main()
