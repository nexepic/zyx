"""Reproduce only the R3.2 payload/gate checks; requires CMake and GTest.

Usage: python run-tests.py BUILD_DIR [extra CMake configure arguments...]
The configured CXX/GTest installation and optional sanitizers belong to the
caller. Native storage, whole-suite tests and latency measurements are excluded.
"""
from pathlib import Path
import json
import subprocess
import sys

root = Path(__file__).resolve().parents[4]
build = Path(sys.argv[1]).resolve()
subprocess.run(["cmake", "-S", str(root / "benchmarks/architecture-design/property-subblock"),
                "-B", str(build), *sys.argv[2:]], check=True)
subprocess.run(["cmake", "--build", str(build), "--config", "Debug"], check=True)
subprocess.run(["ctest", "--test-dir", str(build), "-C", "Debug", "--output-on-failure"], check=True)
(build / "scope.json").write_text(json.dumps({
    "parent_item": "R3.2", "test_suite": "PropertySubblockCandidateTest",
    "production_format_changed": False, "full_lifecycle_accepted": False,
    "formal_performance_run": False
}, indent=2) + "\n")
