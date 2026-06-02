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
    "first_ms",
    "min_ms",
    "avg_ms",
    "p50_ms",
    "p95_ms",
    "p99_ms",
    "max_ms",
    "ops_per_sec",
    "status",
    "equivalent_mode",
]

COMPARISON_FIELDS = [
    "workload",
    "scale",
    "best_database",
    "best_p50_ms",
    "zyx_p50_ms",
    "zyx_first_ms",
    "zyx_rank_by_p50",
    "zyx_vs_best_p50",
    "zyx_p95_ms",
    "zyx_p95_to_p50",
    "zyx_first_to_p50",
    "fastest_non_zyx_database",
    "fastest_non_zyx_p50_ms",
    "zyx_vs_fastest_non_zyx_p50",
]

LIMITATIONS = [
    "Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.",
    "Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.",
]


def _format_float(value: float | None) -> str:
    if value is None:
        return ""
    return f"{value:.6g}"


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
            first_ms=0.0,
            min_ms=0.0,
            avg_ms=0.0,
            p50_ms=0.0,
            p95_ms=0.0,
            p99_ms=0.0,
            max_ms=0.0,
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


def build_comparison_rows(rows: Iterable[SummaryRow], primary_database: str = "zyx") -> list[dict[str, object]]:
    grouped: dict[tuple[str, str], list[SummaryRow]] = {}
    for row in rows:
        if row.status != "ok" or row.samples <= 0:
            continue
        grouped.setdefault((row.scale, row.workload), []).append(row)

    comparison_rows: list[dict[str, object]] = []
    for scale, workload in sorted(grouped):
        candidates = sorted(grouped[(scale, workload)], key=lambda row: (row.p50_ms, row.database))
        best = candidates[0]
        primary = next((row for row in candidates if row.database == primary_database), None)
        non_primary = [row for row in candidates if row.database != primary_database]
        fastest_non_primary = non_primary[0] if non_primary else None

        primary_p50 = primary.p50_ms if primary is not None else None
        primary_rank = None
        if primary is not None:
            primary_rank = 1 + sum(1 for row in candidates if row.p50_ms < primary.p50_ms)

        comparison_rows.append(
            {
                "workload": workload,
                "scale": scale,
                "best_database": best.database,
                "best_p50_ms": best.p50_ms,
                "zyx_p50_ms": primary_p50,
                "zyx_first_ms": None if primary is None else primary.first_ms,
                "zyx_rank_by_p50": primary_rank,
                "zyx_vs_best_p50": None if primary_p50 is None or best.p50_ms <= 0 else primary_p50 / best.p50_ms,
                "zyx_p95_ms": None if primary is None else primary.p95_ms,
                "zyx_p95_to_p50": None
                if primary is None or primary.p50_ms <= 0
                else primary.p95_ms / primary.p50_ms,
                "zyx_first_to_p50": None
                if primary is None or primary.p50_ms <= 0
                else primary.first_ms / primary.p50_ms,
                "fastest_non_zyx_database": None if fastest_non_primary is None else fastest_non_primary.database,
                "fastest_non_zyx_p50_ms": None if fastest_non_primary is None else fastest_non_primary.p50_ms,
                "zyx_vs_fastest_non_zyx_p50": None
                if primary_p50 is None or fastest_non_primary is None or fastest_non_primary.p50_ms <= 0
                else primary_p50 / fastest_non_primary.p50_ms,
            }
        )
    return comparison_rows


def write_comparison_csv(rows: Iterable[dict[str, object]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=COMPARISON_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in COMPARISON_FIELDS})


def write_comparison_markdown(rows: Iterable[dict[str, object]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# 横向性能差距分析",
        "",
        "## ZYX vs Best",
        "",
        "| workload | scale | best | best_p50_ms | zyx_p50_ms | zyx_first_ms | zyx_rank | zyx_vs_best | fastest_non_zyx | zyx_vs_fastest_non_zyx | zyx_p95_ms | zyx_p95/p50 | zyx_first/p50 |",
        "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: | ---: |",
    ]
    for row in rows:
        lines.append(
            "| "
            + " | ".join(
                [
                    str(row["workload"]),
                    str(row["scale"]),
                    str(row["best_database"]),
                    _format_float(row.get("best_p50_ms")),  # type: ignore[arg-type]
                    _format_float(row.get("zyx_p50_ms")),  # type: ignore[arg-type]
                    _format_float(row.get("zyx_first_ms")),  # type: ignore[arg-type]
                    "" if row.get("zyx_rank_by_p50") is None else str(row["zyx_rank_by_p50"]),
                    _format_float(row.get("zyx_vs_best_p50")),  # type: ignore[arg-type]
                    "" if row.get("fastest_non_zyx_database") is None else str(row["fastest_non_zyx_database"]),
                    _format_float(row.get("zyx_vs_fastest_non_zyx_p50")),  # type: ignore[arg-type]
                    _format_float(row.get("zyx_p95_ms")),  # type: ignore[arg-type]
                    _format_float(row.get("zyx_p95_to_p50")),  # type: ignore[arg-type]
                    _format_float(row.get("zyx_first_to_p50")),  # type: ignore[arg-type]
                ]
            )
            + " |"
        )
    lines.extend(
        [
            "",
            "## Reading Guide",
            "",
            "- `zyx_vs_best` is 1 when ZYX is the fastest row for the workload; > 1 means ZYX is slower than the best row.",
            "- `zyx_vs_fastest_non_zyx` compares ZYX with the fastest other database; < 1 means ZYX is faster than every non-ZYX competitor.",
            "- `zyx_p95/p50` highlights tail-latency volatility for ZYX within this run.",
            "- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=cold-ish` for less cache-amortized query samples.",
            "- The comparison uses p50 latency and only includes successful rows with at least one sample.",
        ]
    )
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
    comparison_csv_path = output_dir / "comparison.csv"
    comparison_markdown_path = output_dir / "comparison.md"
    environment_path = output_dir / "environment.json"

    write_summary_csv(rows, csv_path)
    write_summary_markdown(rows, markdown_path)
    comparison_rows = build_comparison_rows(rows)
    write_comparison_csv(comparison_rows, comparison_csv_path)
    write_comparison_markdown(comparison_rows, comparison_markdown_path)
    environment_payload = environment if environment is not None else _default_environment()
    environment_path.write_text(json.dumps(environment_payload, indent=2, sort_keys=True) + "\n")

    return {
        "csv": csv_path,
        "markdown": markdown_path,
        "comparison_csv": comparison_csv_path,
        "comparison_markdown": comparison_markdown_path,
        "environment": environment_path,
    }
