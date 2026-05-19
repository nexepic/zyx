#!/usr/bin/env python3
"""Read the canonical ZYX version from root CMakeLists.txt."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CMAKE_LISTS = ROOT / "CMakeLists.txt"
VERSION_RE = re.compile(
    r"project\s*\(\s*zyx\b(?P<body>.*?)\)",
    re.IGNORECASE | re.DOTALL,
)
VALUE_RE = re.compile(r"\bVERSION\s+([0-9]+\.[0-9]+\.[0-9]+(?:[-+A-Za-z0-9.]*)?)")


def read_project_version() -> str:
    text = CMAKE_LISTS.read_text(encoding="utf-8")
    project_match = VERSION_RE.search(text)
    if not project_match:
        raise SystemExit("Error: could not find project(zyx ... VERSION ...) in CMakeLists.txt")
    version_match = VALUE_RE.search(project_match.group("body"))
    if not version_match:
        raise SystemExit("Error: could not find VERSION in project(zyx ...) declaration")
    return version_match.group(1)


def main() -> None:
    sys.stdout.write(read_project_version() + "\n")


if __name__ == "__main__":
    main()
