#!/usr/bin/env python3
"""Run selected query-scratch crash/recovery checks in independent processes."""

import argparse
import json
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", type=pathlib.Path, required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    worker = args.worker.resolve()
    if not worker.is_file():
        parser.error("build the selected zyx_query_scratch_crash_worker target first")
    if not 1 <= args.repetitions <= 10000:
        parser.error("repetitions must be between 1 and 10000")
    results = []
    with tempfile.TemporaryDirectory(prefix="zyx_query_scratch_crash_") as directory:
        for repetition in range(args.repetitions):
            path = pathlib.Path(directory) / (str(repetition) + ".dat")
            for phase, expected in (("crash", 86), ("recover", 0)):
                result = subprocess.run(
                    [str(worker), phase, str(path)], capture_output=True,
                    text=True, timeout=60, check=False,
                )
                results.append({"repetition": repetition, "phase": phase,
                                "exit_code": result.returncode,
                                "stdout": result.stdout.strip(),
                                "stderr": result.stderr.strip()})
                if result.returncode != expected:
                    print(json.dumps({"passed": False, "results": results}, indent=2))
                    return 1
            path.unlink()
    print(json.dumps({"passed": True, "repetitions": args.repetitions,
                      "results": results}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
