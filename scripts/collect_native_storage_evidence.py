#!/usr/bin/env python3
"""Create one checksummed, machine-readable native-storage CI evidence manifest."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import sys
from typing import Any, Dict, Optional


MANIFEST_SCHEMA_VERSION = 1
VECTOR_CALIBRATION_SCHEMA_VERSION = 2
LARGE_VALUE_CALIBRATION_SCHEMA_VERSION = 1
MAXIMUM_REPETITIONS = 9999
CI_ENVIRONMENT_KEYS = (
    "GITHUB_WORKFLOW",
    "GITHUB_RUN_ID",
    "GITHUB_RUN_ATTEMPT",
    "GITHUB_SHA",
    "RUNNER_OS",
    "RUNNER_ARCH",
)


def _fail(message: str) -> None:
    raise ValueError(message)


def _require_string(value: Any, description: str) -> str:
    if not isinstance(value, str) or not value:
        _fail(f"{description} must be a non-empty string")
    return value


def _require_nonnegative_integer(value: Any, description: str) -> int:
    if type(value) is not int or value < 0:
        _fail(f"{description} must be a non-negative integer")
    return value


def _require_nonnegative_number(value: Any, description: str) -> float:
    if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
        _fail(f"{description} must be a non-negative number")
    return float(value)


def _read_json_report(path: Path, kind: str) -> Dict[str, Any]:
    try:
        decoded = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        _fail(f"could not read {kind} calibration report: {error}")
    except json.JSONDecodeError as error:
        _fail(f"{kind} calibration report is not valid JSON: {error}")
    if not isinstance(decoded, dict):
        _fail(f"{kind} calibration report root must be an object")
    return decoded


def _require_passed_gate(report: Dict[str, Any], kind: str) -> Dict[str, Any]:
    gate = report.get("gate")
    if not isinstance(gate, dict):
        _fail(f"{kind} calibration report has no gate result")
    if gate.get("enforced") is not True or gate.get("passed") is not True:
        _fail(f"{kind} calibration report does not contain a passed enforced gate")
    if not isinstance(gate.get("violations"), list):
        _fail(f"{kind} calibration report has an incomplete gate result")
    if gate["violations"]:
        _fail(f"{kind} calibration report names gate violations despite passing")
    return gate


def _read_vector_report(path: Path, kind: str) -> Dict[str, Any]:
    decoded = _read_json_report(path, kind)
    if decoded.get("schema_version") != VECTOR_CALIBRATION_SCHEMA_VERSION:
        _fail(f"{kind} calibration report has an unsupported schema version")
    dataset = decoded.get("dataset")
    if not isinstance(dataset, dict):
        _fail(f"{kind} calibration report has no dataset descriptor")
    _require_string(dataset.get("label"), f"{kind} dataset label")
    _require_nonnegative_integer(dataset.get("bytes"), f"{kind} dataset byte count")
    crc32 = _require_nonnegative_integer(dataset.get("crc32"), f"{kind} dataset CRC32")
    if crc32 > 0xFFFFFFFF:
        _fail(f"{kind} dataset CRC32 exceeds 32 bits")
    _require_string(decoded.get("hardware_label"), f"{kind} hardware label")
    gate = _require_passed_gate(decoded, kind)
    if not isinstance(gate.get("thresholds"), dict):
        _fail(f"{kind} calibration report has an incomplete gate result")
    return decoded


def _read_large_value_report(path: Path) -> Dict[str, Any]:
    kind = "LargeValue"
    decoded = _read_json_report(path, kind)
    if decoded.get("schema_version") != LARGE_VALUE_CALIBRATION_SCHEMA_VERSION:
        _fail("LargeValue calibration report has an unsupported schema version")
    dataset = decoded.get("dataset")
    if not isinstance(dataset, dict):
        _fail("LargeValue calibration report has no dataset descriptor")
    _require_string(dataset.get("label"), "LargeValue dataset label")
    if dataset.get("generated") is not True:
        _fail("LargeValue dataset must be declared generated")
    unique_values = _require_nonnegative_integer(dataset.get("unique_values"), "LargeValue unique value count")
    value_bytes = _require_nonnegative_integer(dataset.get("value_bytes"), "LargeValue value byte count")
    logical_bytes = _require_nonnegative_integer(dataset.get("logical_bytes"), "LargeValue logical byte count")
    if unique_values == 0 or value_bytes == 0 or logical_bytes != unique_values * value_bytes:
        _fail("LargeValue dataset logical byte count must match its non-empty generated values")
    _require_string(decoded.get("hardware_label"), "LargeValue hardware label")

    storage = decoded.get("storage")
    if not isinstance(storage, dict) or storage.get("single_file") is not True:
        _fail("LargeValue calibration report must attest to one database file")
    for field in ("database_bytes", "file_growth_bytes", "space_amplification"):
        _require_nonnegative_number(storage.get(field), f"LargeValue storage {field}")

    unique_write = decoded.get("unique_write")
    if not isinstance(unique_write, dict):
        _fail("LargeValue calibration report has no unique-write metrics")
    for field in ("seconds", "values_per_second", "submitted_write_bytes", "submitted_write_amplification"):
        _require_nonnegative_number(unique_write.get(field), f"LargeValue unique-write {field}")

    dedup = decoded.get("dedup")
    if not isinstance(dedup, dict):
        _fail("LargeValue calibration report has no deduplication metrics")
    attempts = _require_nonnegative_integer(dedup.get("attempts"), "LargeValue dedup attempts")
    hits = _require_nonnegative_integer(dedup.get("hits"), "LargeValue dedup hits")
    misses = _require_nonnegative_integer(dedup.get("misses"), "LargeValue dedup misses")
    _require_nonnegative_integer(dedup.get("candidates_scanned"), "LargeValue dedup candidates scanned")
    _require_nonnegative_number(dedup.get("seconds"), "LargeValue dedup seconds")
    hit_rate = _require_nonnegative_number(dedup.get("hit_rate"), "LargeValue dedup hit rate")
    if attempts == 0 or attempts != hits + misses or hit_rate > 1.0:
        _fail("LargeValue deduplication metrics are internally inconsistent")

    range_read = decoded.get("range_read")
    if not isinstance(range_read, dict):
        _fail("LargeValue calibration report has no range-read metrics")
    for field in ("operations", "bytes", "submitted_read_bytes"):
        _require_nonnegative_integer(range_read.get(field), f"LargeValue range-read {field}")
    for field in ("seconds", "bytes_per_second"):
        _require_nonnegative_number(range_read.get(field), f"LargeValue range-read {field}")
    latency = range_read.get("latency_us")
    if not isinstance(latency, dict):
        _fail("LargeValue range-read metrics have no latency distribution")
    for percentile in ("p50", "p95", "p99"):
        _require_nonnegative_number(latency.get(percentile), f"LargeValue range-read latency {percentile}")
    _require_nonnegative_integer(decoded.get("peak_rss_bytes"), "LargeValue peak RSS")
    _require_passed_gate(decoded, kind)
    return decoded


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        _fail(f"could not hash calibration report: {error}")
    return digest.hexdigest()


def _evidence_file_summary(path: Path, description: str, *, require_nonempty: bool = False) -> Dict[str, Any]:
    """Return a bounded manifest entry only for a readable regular artifact."""
    try:
        metadata = path.stat()
    except OSError as error:
        _fail(f"could not stat {description}: {error}")
    if not path.is_file():
        _fail(f"{description} must be a regular file")
    if require_nonempty and metadata.st_size == 0:
        _fail(f"{description} must not be empty")
    return {
        "file_name": path.name,
        "bytes": metadata.st_size,
        "sha256": _sha256(path),
    }


def _vector_report_summary(path: Path, report: Dict[str, Any]) -> Dict[str, Any]:
    dataset = report["dataset"]
    return {
        "file_name": path.name,
        "sha256": _sha256(path),
        "schema_version": report["schema_version"],
        "dataset": {
            "label": dataset["label"],
            "bytes": dataset["bytes"],
            "crc32": dataset["crc32"],
        },
        "hardware_label": report["hardware_label"],
        "gate": {
            "enforced": report["gate"]["enforced"],
            "passed": report["gate"]["passed"],
            "thresholds": report["gate"]["thresholds"],
        },
    }


def _large_value_report_summary(path: Path, report: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "file_name": path.name,
        "sha256": _sha256(path),
        "schema_version": report["schema_version"],
        "dataset": report["dataset"],
        "hardware_label": report["hardware_label"],
        "storage": report["storage"],
        "unique_write": report["unique_write"],
        "dedup": report["dedup"],
        "range_read": report["range_read"],
        "peak_rss_bytes": report["peak_rss_bytes"],
        "gate": {
            "enforced": report["gate"]["enforced"],
            "passed": report["gate"]["passed"],
        },
    }


def _validate_repetitions(value: int, description: str, minimum: int) -> int:
    if value < minimum or value > MAXIMUM_REPETITIONS:
        _fail(f"{description} must be between {minimum} and {MAXIMUM_REPETITIONS}")
    return value


def collect(build_report_path: Path, query_report_path: Path, large_value_report_path: Path, output_path: Path,
            recovery_repetitions: int, endurance_repetitions: int, *, native_lifecycle_log_path: Path,
            recovery_maintenance_log_path: Path, vector_smoke_data_path: Path, large_value_smoke_data_path: Path,
            endurance_log_path: Optional[Path] = None) -> Dict[str, Any]:
    """Validate passed native-storage reports and publish an immutable audit manifest."""
    recovery_repetitions = _validate_repetitions(recovery_repetitions, "recovery repetitions", 1)
    endurance_repetitions = _validate_repetitions(endurance_repetitions, "endurance repetitions", 0)
    build_report = _read_vector_report(build_report_path, "build")
    query_report = _read_vector_report(query_report_path, "query")
    large_value_report = _read_large_value_report(large_value_report_path)
    if build_report["dataset"]["label"] != query_report["dataset"]["label"]:
        _fail("build and query calibration reports must name the same frozen dataset label")
    if build_report["hardware_label"] != query_report["hardware_label"]:
        _fail("build and query calibration reports must name the same hardware label")

    evidence_files = {
        "native_lifecycle_log": _evidence_file_summary(
            native_lifecycle_log_path, "native lifecycle transcript", require_nonempty=True
        ),
        "recovery_maintenance_log": _evidence_file_summary(
            recovery_maintenance_log_path, "recovery/maintenance transcript", require_nonempty=True
        ),
        "vector_smoke_data_file": _evidence_file_summary(
            vector_smoke_data_path, "vector smoke database data file"
        ),
        "large_value_smoke_data_file": _evidence_file_summary(
            large_value_smoke_data_path, "LargeValue smoke database data file"
        ),
    }
    if endurance_repetitions == 0:
        if endurance_log_path is not None:
            _fail("endurance transcript is invalid when manual endurance repetitions are zero")
    else:
        if endurance_log_path is None:
            _fail("endurance transcript is required when manual endurance repetitions are nonzero")
        evidence_files["endurance_log"] = _evidence_file_summary(
            endurance_log_path, "endurance transcript", require_nonempty=True
        )

    output_path = output_path.resolve()
    if output_path.exists():
        _fail("native-storage evidence manifest already exists and will not be overwritten")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema_version": MANIFEST_SCHEMA_VERSION,
        "evidence_classification": "ci-smoke",
        "generated_at_utc": datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"),
        "calibration": {
            "vector": {
                "build": _vector_report_summary(build_report_path, build_report),
                "query": _vector_report_summary(query_report_path, query_report),
            },
            "large_value": _large_value_report_summary(large_value_report_path, large_value_report),
        },
        "executed_repetitions": {
            "recovery_maintenance": recovery_repetitions,
            "manual_endurance": endurance_repetitions,
        },
        "evidence_files": evidence_files,
        "ci_environment": {key: os.environ[key] for key in CI_ENVIRONMENT_KEYS if os.environ.get(key)},
        "limitations": [
            "CI smoke evidence verifies pipeline wiring only and is not production performance approval.",
            "Production approval requires declared target hardware, frozen production-scale input, reviewed thresholds, "
            "and an external review record.",
        ],
    }
    temporary_path = output_path.with_name(output_path.name + ".tmp")
    try:
        temporary_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        temporary_path.replace(output_path)
    except OSError as error:
        _fail(f"could not write native-storage evidence manifest: {error}")
    return manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-report", type=Path, required=True, help="passed vector-build calibration JSON")
    parser.add_argument("--query-report", type=Path, required=True, help="passed vector-query calibration JSON")
    parser.add_argument("--large-value-report", type=Path, required=True, help="passed LargeValue calibration JSON")
    parser.add_argument("--output", type=Path, required=True, help="new evidence-manifest JSON path")
    parser.add_argument("--recovery-repetitions", type=int, required=True, help="completed recovery/maintenance repeats")
    parser.add_argument("--endurance-repetitions", type=int, default=0, help="optional completed manual endurance repeats")
    parser.add_argument("--native-lifecycle-log", type=Path, required=True, help="completed native lifecycle transcript")
    parser.add_argument("--recovery-maintenance-log", type=Path, required=True,
                        help="completed recovery/maintenance transcript")
    parser.add_argument("--vector-smoke-data", type=Path, required=True, help="generated vector smoke database data file")
    parser.add_argument("--large-value-smoke-data", type=Path, required=True,
                        help="generated LargeValue smoke database data file")
    parser.add_argument("--endurance-log", type=Path, help="completed optional manual endurance transcript")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        manifest = collect(
            args.build_report, args.query_report, args.large_value_report, args.output, args.recovery_repetitions,
            args.endurance_repetitions, native_lifecycle_log_path=args.native_lifecycle_log,
            recovery_maintenance_log_path=args.recovery_maintenance_log,
            vector_smoke_data_path=args.vector_smoke_data, large_value_smoke_data_path=args.large_value_smoke_data,
            endurance_log_path=args.endurance_log,
        )
    except ValueError as error:
        print(f"Unable to collect native-storage evidence: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"output": str(args.output), "schema_version": manifest["schema_version"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
