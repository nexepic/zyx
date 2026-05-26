from __future__ import annotations

import csv
import json
import platform
from dataclasses import asdict
from pathlib import Path
from typing import Iterable

from runner.models import FailureEvent, Sample, SummaryRow
from runner.stats import summarize_samples

SUMMARY_FIELDS = [
    "database",
    "workload",
    "scale",
    "samples",
    "avg_ms",
    "p50_ms",
    "p95_ms",
    "p99_ms",
    "ops_per_sec",
    "status",
    "equivalent_mode",
]

LIMITATIONS = [
    "Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.",
    "Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.",
]


def _read_events(path: Path, event_name: str, fields: object) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    with path.open() as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            event = json.loads(line)
            if event.get("event") != event_name:
                continue
            events.append({key: event[key] for key in fields if key in event})
    return events


def read_samples(path: Path) -> list[Sample]:
    return [Sample(**payload) for payload in _read_events(path, "sample", Sample.__dataclass_fields__)]


def read_failures(path: Path) -> list[FailureEvent]:
    return [FailureEvent(**payload) for payload in _read_events(path, "failure", FailureEvent.__dataclass_fields__)]


def failure_summary_rows(failures: Iterable[FailureEvent]) -> list[SummaryRow]:
    rows: dict[tuple[str, str, str, str, str], SummaryRow] = {}
    for failure in failures:
        key = (failure.database, failure.workload, failure.scale, failure.status, failure.equivalent_mode)
        rows[key] = SummaryRow(
            database=failure.database,
            workload=failure.workload,
            scale=failure.scale,
            samples=0,
            avg_ms=0.0,
            p50_ms=0.0,
            p95_ms=0.0,
            p99_ms=0.0,
            ops_per_sec=0.0,
            status=failure.status,
            equivalent_mode=failure.equivalent_mode,
        )
    return [rows[key] for key in sorted(rows)]


def write_summary_csv(rows: Iterable[SummaryRow], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=SUMMARY_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def write_summary_markdown(rows: Iterable[SummaryRow], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# 图数据库性能对比报告",
        "",
        "## Summary",
        "",
        "| " + " | ".join(SUMMARY_FIELDS) + " |",
        "| " + " | ".join(["---"] * len(SUMMARY_FIELDS)) + " |",
    ]
    for row in rows:
        data = asdict(row)
        lines.append("| " + " | ".join(str(data[field]) for field in SUMMARY_FIELDS) + " |")
    lines.extend(["", "## Limitations", ""] + [f"- {limitation}" for limitation in LIMITATIONS])
    path.write_text("\n".join(lines) + "\n")


def _default_environment() -> dict[str, str]:
    return {
        "machine": platform.machine(),
        "platform": platform.platform(),
        "python": platform.python_version(),
        "system": platform.system(),
    }


def write_summary_outputs(
    events_path: Path,
    output_dir: Path,
    environment: dict[str, object] | None = None,
) -> dict[str, Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    rows = summarize_samples(read_samples(events_path)) + failure_summary_rows(read_failures(events_path))
    csv_path = output_dir / "summary.csv"
    markdown_path = output_dir / "summary.md"
    environment_path = output_dir / "environment.json"

    write_summary_csv(rows, csv_path)
    write_summary_markdown(rows, markdown_path)
    environment_payload = environment if environment is not None else _default_environment()
    environment_path.write_text(json.dumps(environment_payload, indent=2, sort_keys=True) + "\n")

    return {"csv": csv_path, "markdown": markdown_path, "environment": environment_path}
