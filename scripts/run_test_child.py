#!/usr/bin/env python3
"""Start a test command after the Windows supervisor releases its control pipe."""

from __future__ import annotations

import subprocess
import sys
from typing import Sequence


def run(command: Sequence[str]) -> int:
    if not command:
        raise ValueError("a test command is required")
    # The parent writes only after it has assigned this launcher to a Job Object.
    if sys.stdin.buffer.read(1) != b"\n":
        return 127
    return subprocess.call(list(command), stdin=subprocess.DEVNULL)


def main() -> int:
    command = sys.argv[1:]
    if command[:1] == ["--"]:
        command = command[1:]
    try:
        return run(command)
    except (OSError, ValueError) as error:
        print(f"Unable to start test child: {error}", file=sys.stderr)
        return 127


if __name__ == "__main__":
    raise SystemExit(main())
