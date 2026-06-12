from __future__ import annotations

import csv
import json
import math
import platform
from collections import defaultdict
from dataclasses import asdict
from pathlib import Path
from typing import Iterable

from runner.models import FailureEvent, ProfileEvent, ProfileSummaryRow, Sample, SummaryRow
from runner.stats import percentile, summarize_samples

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

PROFILE_SUMMARY_FIELDS = [
    "database",
    "workload",
    "scale",
    "profile",
    "phase",
    "samples",
    "total_calls",
    "avg_calls",
    "first_ms",
    "min_ms",
    "avg_ms",
    "p50_ms",
    "p95_ms",
    "p99_ms",
    "max_ms",
    "equivalent_mode",
]

OPERATION_SUMMARY_FIELDS = [
    "operation_type",
    "database",
    "scale",
    "workload_count",
    "workloads",
    "ok_workloads",
    "unsupported_workloads",
    "failed_workloads",
    "samples",
    "geomean_p50_ms",
    "avg_p50_ms",
    "min_p50_ms",
    "max_p50_ms",
]

PHASE_CATEGORY_SUMMARY_FIELDS = [
    "database",
    "workload",
    "scale",
    "profile",
    "semantic_phase",
    "raw_phases",
    "samples",
    "total_calls",
    "avg_calls",
    "first_ms",
    "min_ms",
    "avg_ms",
    "p50_ms",
    "p95_ms",
    "p99_ms",
    "max_ms",
    "equivalent_mode",
]

LIMITATIONS = [
    "Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.",
    "Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.",
]

LOAD_INDEX_BUILD_WORKLOADS = {"load_nodes_edges"}
READ_SCAN_WORKLOADS = {"relationship_type_scan"}
PROPERTY_FILTER_WORKLOADS = {
    "label_scan_filter",
    "all_nodes_property_filter",
    "label_multi_property_filter",
    "relationship_property_filter",
    "point_lookup_indexed",
    "property_equality_indexed",
    "property_range_indexed",
}
ADJACENCY_EXPAND_WORKLOADS = {
    "one_hop_expand",
    "two_hop_expand",
    "shortest_path_chain",
    "index_seek_then_one_hop_expand",
    "index_seek_then_two_hop_expand",
    "write_then_one_hop_expand",
    "batch_create_edges_100_then_one_hop_expand",
    "batch_create_edges_10000_then_one_hop_expand",
}
TOPK_GROUP_WORKLOADS = {
    "aggregation_group_by",
    "aggregation_count_by_group",
    "topk_property_sort",
}
POINT_WRITE_WORKLOADS = {
    "point_create_node",
    "point_create_edge",
    "point_update_node_property",
    "point_update_edge_property",
    "point_create_delete_edge",
    "write_then_read_edge",
    "point_create_node_durable",
    "point_create_edge_durable",
    "point_update_node_property_durable",
    "point_update_edge_property_durable",
    "point_create_delete_edge_durable",
    "write_then_read_edge_durable",
    "post_persist_create_node",
    "post_persist_create_edge",
}
BATCH_WRITE_WORKLOADS = {
    "batch_create_edges_100",
    "batch_create_edges_1000",
    "batch_create_edges_10000",
}


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


def read_summary_rows(path: Path | None) -> list[SummaryRow]:
    if path is None or not path.exists():
        return []
    rows: list[SummaryRow] = []
    with path.open(newline="") as handle:
        for payload in csv.DictReader(handle):
            rows.append(
                SummaryRow(
                    database=payload["database"],
                    workload=payload["workload"],
                    scale=payload["scale"],
                    samples=int(payload["samples"]),
                    first_ms=float(payload["first_ms"]),
                    min_ms=float(payload["min_ms"]),
                    avg_ms=float(payload["avg_ms"]),
                    p50_ms=float(payload["p50_ms"]),
                    p95_ms=float(payload["p95_ms"]),
                    p99_ms=float(payload["p99_ms"]),
                    max_ms=float(payload["max_ms"]),
                    ops_per_sec=float(payload["ops_per_sec"]),
                    status=payload.get("status", "ok"),
                    equivalent_mode=payload.get("equivalent_mode", "cypher"),
                )
            )
    return rows


def read_profile_events(path: Path | None) -> list[ProfileEvent]:
    if path is None or not path.exists():
        return []
    return [ProfileEvent(**payload) for payload in _read_events(path, "profile", ProfileEvent.__dataclass_fields__)]


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
            "- `zyx_first/p50` highlights first measured iteration cost; use `execution_mode=opened` for first-query-after-open samples and `execution_mode=cold-ish` for less cache-amortized query samples.",
            "- The comparison uses p50 latency and only includes successful rows with at least one sample.",
        ]
    )
    path.write_text("\n".join(lines) + "\n")


def build_profile_summary_rows(events: Iterable[ProfileEvent]) -> list[ProfileSummaryRow]:
    grouped: dict[tuple[str, str, str, str, str, str], list[ProfileEvent]] = defaultdict(list)
    for event in events:
        key = (event.database, event.workload, event.scale, event.profile, event.phase, event.equivalent_mode)
        grouped[key].append(event)

    rows: list[ProfileSummaryRow] = []
    for key in sorted(grouped):
        group = grouped[key]
        durations = [event.total_time_ms for event in group]
        total_calls = sum(event.calls for event in group)
        first = min(group, key=lambda event: event.iteration)
        sample_count = len(group)
        rows.append(
            ProfileSummaryRow(
                database=key[0],
                workload=key[1],
                scale=key[2],
                profile=key[3],
                phase=key[4],
                samples=sample_count,
                total_calls=total_calls,
                avg_calls=total_calls / sample_count,
                first_ms=first.total_time_ms,
                min_ms=min(durations),
                avg_ms=sum(durations) / sample_count,
                p50_ms=percentile(durations, 50),
                p95_ms=percentile(durations, 95),
                p99_ms=percentile(durations, 99),
                max_ms=max(durations),
                equivalent_mode=key[5],
            )
        )
    return sorted(
        rows,
        key=lambda row: (row.database, row.scale, row.profile, row.workload, -row.p50_ms, row.phase),
    )


def write_profile_summary_csv(rows: Iterable[ProfileSummaryRow], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=PROFILE_SUMMARY_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def write_profile_summary_markdown(rows: Iterable[ProfileSummaryRow], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Benchmark Profile Breakdown",
        "",
        "## Phase Summary",
        "",
        "| " + " | ".join(PROFILE_SUMMARY_FIELDS) + " |",
        "| " + " | ".join(["---"] * len(PROFILE_SUMMARY_FIELDS)) + " |",
    ]
    for row in rows:
        data = asdict(row)
        lines.append(
            "| "
            + " | ".join(
                _format_float(data[field]) if isinstance(data[field], float) else str(data[field])
                for field in PROFILE_SUMMARY_FIELDS
            )
            + " |"
        )
    lines.extend(
        [
            "",
            "## Reading Guide",
            "",
            "- Each row aggregates one emitted profile phase across measured iterations.",
            "- Rows are ordered by database, scale, profile, workload, and descending `p50_ms` so the slowest phases appear first within each workload.",
            "- `first_ms` helps diagnose cold-ish first-iteration costs; `p50_ms` and `p95_ms` highlight steady-state and tail behavior.",
            "- `total_calls` and `avg_calls` distinguish expensive phases from frequently repeated small operations.",
            "- ZYX currently emits internal profile phases; other databases may only have top-level sample latency unless their adapters add phase events.",
        ]
    )
    path.write_text("\n".join(lines) + "\n")


def operation_type_for_workload(workload: str) -> str:
    if workload in LOAD_INDEX_BUILD_WORKLOADS:
        return "load/index build"
    if workload in READ_SCAN_WORKLOADS:
        return "read scan"
    if workload in PROPERTY_FILTER_WORKLOADS:
        return "property filter"
    if workload in ADJACENCY_EXPAND_WORKLOADS or workload.startswith("reachable_within_"):
        return "adjacency expand"
    if workload in TOPK_GROUP_WORKLOADS:
        return "topk/group"
    if workload in POINT_WRITE_WORKLOADS:
        return "point write"
    if workload in BATCH_WRITE_WORKLOADS:
        return "batch write"
    return "other"


def _geomean(values: list[float]) -> float | None:
    positive = [value for value in values if value > 0]
    if not positive:
        return None
    return math.exp(sum(math.log(value) for value in positive) / len(positive))


def build_operation_summary_rows(rows: Iterable[SummaryRow]) -> list[dict[str, object]]:
    grouped: dict[tuple[str, str, str], list[SummaryRow]] = defaultdict(list)
    for row in rows:
        grouped[(operation_type_for_workload(row.workload), row.database, row.scale)].append(row)

    summary_rows: list[dict[str, object]] = []
    for operation_type, database, scale in sorted(grouped):
        group = grouped[(operation_type, database, scale)]
        ok_rows = [row for row in group if row.status == "ok" and row.samples > 0]
        unsupported = [row.workload for row in group if row.status == "unsupported"]
        failed = [row.workload for row in group if row.status not in {"ok", "unsupported"}]
        p50_values = [row.p50_ms for row in ok_rows if row.p50_ms > 0]
        summary_rows.append(
            {
                "operation_type": operation_type,
                "database": database,
                "scale": scale,
                "workload_count": len(group),
                "workloads": ";".join(sorted(row.workload for row in group)),
                "ok_workloads": len(ok_rows),
                "unsupported_workloads": len(unsupported),
                "failed_workloads": len(failed),
                "samples": sum(row.samples for row in ok_rows),
                "geomean_p50_ms": _geomean(p50_values),
                "avg_p50_ms": None if not p50_values else sum(p50_values) / len(p50_values),
                "min_p50_ms": None if not p50_values else min(p50_values),
                "max_p50_ms": None if not p50_values else max(p50_values),
            }
        )
    return summary_rows


def write_operation_summary_csv(rows: Iterable[dict[str, object]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=OPERATION_SUMMARY_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in OPERATION_SUMMARY_FIELDS})


def write_operation_summary_markdown(rows: Iterable[dict[str, object]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Operation Type Summary",
        "",
        "Rows group benchmark workloads by database operation family. Unit: ms; lower p50-derived values are better.",
        "",
        "| " + " | ".join(OPERATION_SUMMARY_FIELDS) + " |",
        "| " + " | ".join(["---"] * len(OPERATION_SUMMARY_FIELDS)) + " |",
    ]
    for row in rows:
        lines.append(
            "| "
            + " | ".join(
                _format_float(row.get(field)) if isinstance(row.get(field), float) else str(row.get(field, ""))
                for field in OPERATION_SUMMARY_FIELDS
            )
            + " |"
        )
    lines.extend(
        [
            "",
            "## Operation Families",
            "",
            "- `read scan`: scans without scalar property predicates, such as relationship type counts.",
            "- `adjacency expand`: fixed-hop, bounded-reachability, and write-then-local-traversal workloads.",
            "- `property filter`: node/relationship property predicates and indexed scalar lookup workloads.",
            "- `topk/group`: sort/TopK and aggregation/grouping workloads.",
            "- `point write`, `batch write`, `load/index build`: mutation and ingest phases kept separate from read latency.",
        ]
    )
    path.write_text("\n".join(lines) + "\n")


def measurement_phase_for_profile_phase(phase: str) -> str:
    lowered = phase.lower()
    if lowered.endswith(".operation"):
        return "statement_latency"
    if (
        "checkpoint" in lowered
        or lowered.startswith("db.close")
        or lowered.startswith("db.flush")
        or lowered.startswith("save.")
        or lowered.startswith("storage.close")
        or lowered == "txn.save"
        or lowered.endswith(".db_close")
    ):
        return "close_checkpoint"
    if lowered.startswith("wal."):
        return "durable_wal_fsync"
    if lowered.startswith("load.") or lowered.startswith("datamanager.add_"):
        return "load_import"
    if lowered.startswith("property.store_batch") or lowered.startswith("index."):
        return "load_import"
    if "commit" in lowered:
        return "statement_commit"
    return "other"


def build_phase_category_summary_rows(events: Iterable[ProfileEvent]) -> list[dict[str, object]]:
    per_iteration: dict[tuple[str, str, str, str, str, str, int], dict[str, object]] = {}
    for event in events:
        semantic_phase = measurement_phase_for_profile_phase(event.phase)
        key = (
            event.database,
            event.workload,
            event.scale,
            event.profile,
            semantic_phase,
            event.equivalent_mode,
            event.iteration,
        )
        bucket = per_iteration.setdefault(key, {"duration": 0.0, "calls": 0, "raw_phases": set()})
        bucket["duration"] = float(bucket["duration"]) + event.total_time_ms
        bucket["calls"] = int(bucket["calls"]) + event.calls
        raw_phases = bucket["raw_phases"]
        if isinstance(raw_phases, set):
            raw_phases.add(event.phase)

    grouped: dict[tuple[str, str, str, str, str, str], list[tuple[int, float, int, set[str]]]] = defaultdict(list)
    for key, bucket in per_iteration.items():
        database, workload, scale, profile, semantic_phase, equivalent_mode, iteration = key
        raw_phases = bucket["raw_phases"]
        grouped[(database, workload, scale, profile, semantic_phase, equivalent_mode)].append(
            (
                iteration,
                float(bucket["duration"]),
                int(bucket["calls"]),
                set(raw_phases) if isinstance(raw_phases, set) else set(),
            )
        )

    rows: list[dict[str, object]] = []
    for key in sorted(grouped):
        group = sorted(grouped[key], key=lambda item: item[0])
        durations = [item[1] for item in group]
        calls = [item[2] for item in group]
        raw_phase_names = sorted({phase for item in group for phase in item[3]})
        sample_count = len(group)
        rows.append(
            {
                "database": key[0],
                "workload": key[1],
                "scale": key[2],
                "profile": key[3],
                "semantic_phase": key[4],
                "raw_phases": ";".join(raw_phase_names),
                "samples": sample_count,
                "total_calls": sum(calls),
                "avg_calls": sum(calls) / sample_count,
                "first_ms": group[0][1],
                "min_ms": min(durations),
                "avg_ms": sum(durations) / sample_count,
                "p50_ms": percentile(durations, 50),
                "p95_ms": percentile(durations, 95),
                "p99_ms": percentile(durations, 99),
                "max_ms": max(durations),
                "equivalent_mode": key[5],
            }
        )
    return sorted(
        rows,
        key=lambda row: (
            row["database"],
            row["scale"],
            row["profile"],
            row["workload"],
            row["semantic_phase"],
        ),
    )


def write_phase_category_summary_csv(rows: Iterable[dict[str, object]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=PHASE_CATEGORY_SUMMARY_FIELDS)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in PHASE_CATEGORY_SUMMARY_FIELDS})


def write_phase_category_summary_markdown(rows: Iterable[dict[str, object]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Measurement Phase Summary",
        "",
        "Rows group raw profile timers into benchmark semantics such as statement latency, WAL fsync, close checkpoint, and load/import.",
        "",
        "| " + " | ".join(PHASE_CATEGORY_SUMMARY_FIELDS) + " |",
        "| " + " | ".join(["---"] * len(PHASE_CATEGORY_SUMMARY_FIELDS)) + " |",
    ]
    for row in rows:
        lines.append(
            "| "
            + " | ".join(
                _format_float(row.get(field)) if isinstance(row.get(field), float) else str(row.get(field, ""))
                for field in PHASE_CATEGORY_SUMMARY_FIELDS
            )
            + " |"
        )
    lines.extend(
        [
            "",
            "## Semantic Phases",
            "",
            "- `statement_latency`: user-visible operation latency measured by the workload sample timer.",
            "- `durable_wal_fsync`: WAL commit/sync work emitted by storage profile timers.",
            "- `close_checkpoint`: clean close, checkpoint, and deferred database flush work.",
            "- `load_import`: data loading, index build, and batch storage ingest work.",
            "- `statement_commit`: commit wrapper work that is not precise enough to classify as WAL-only.",
        ]
    )
    path.write_text("\n".join(lines) + "\n")


def _quality_gate(name: str, status: str, details: str) -> dict[str, str]:
    return {"name": name, "status": status, "details": details}


def build_quality_gate_report(
    rows: Iterable[SummaryRow],
    environment: dict[str, object] | None,
    profile_events: Iterable[ProfileEvent] = (),
    baseline_rows: Iterable[SummaryRow] = (),
    max_regression_ratio: float = 1.10,
) -> dict[str, object]:
    rows = list(rows)
    environment = environment or {}
    gates: list[dict[str, str]] = []

    required_environment_fields = [
        "benchmark_schema_version",
        "databases",
        "execution_mode",
        "git_commit",
        "git_dirty",
        "iterations",
        "profile",
        "required_workloads",
        "scale",
        "seed",
        "warmup",
    ]
    if "benchmark_schema_version" not in environment:
        gates.append(
            _quality_gate(
                "benchmark_environment_metadata",
                "skipped",
                "environment did not come from the benchmark runner schema",
            )
        )
    else:
        missing = [field for field in required_environment_fields if field not in environment]
        gates.append(
            _quality_gate(
                "benchmark_environment_metadata",
                "passed" if not missing else "failed",
                "all required environment fields are present"
                if not missing
                else "missing fields: " + ", ".join(missing),
            )
        )

    failed_rows = [row for row in rows if row.status not in {"ok", "unsupported"}]
    gates.append(
        _quality_gate(
            "no_workload_failures",
            "passed" if not failed_rows else "failed",
            "no failed workload rows"
            if not failed_rows
            else f"{len(failed_rows)} failed workload row(s) in summary",
        )
    )
    unsupported_rows = [row for row in rows if row.status == "unsupported"]
    gates.append(
        _quality_gate(
            "unsupported_workloads_declared",
            "skipped" if not unsupported_rows else "passed",
            "no unsupported workload rows"
            if not unsupported_rows
            else f"{len(unsupported_rows)} unsupported workload row(s) explicitly marked",
        )
    )

    databases = [str(value) for value in environment.get("databases", [])] if isinstance(environment.get("databases"), list) else []
    required_workloads = (
        [str(value) for value in environment.get("required_workloads", [])]
        if isinstance(environment.get("required_workloads"), list)
        else []
    )
    iterations = environment.get("iterations")
    expected_iterations = iterations if isinstance(iterations, int) and iterations > 0 else None
    if databases and required_workloads and expected_iterations is not None:
        row_by_key = {(row.database, row.workload): row for row in rows}
        incomplete: list[str] = []
        for database in databases:
            for workload in required_workloads:
                row = row_by_key.get((database, workload))
                if row is None:
                    incomplete.append(f"{database}/{workload}: missing")
                elif row.status == "unsupported":
                    continue
                elif row.status != "ok":
                    incomplete.append(f"{database}/{workload}: {row.status}")
                elif row.samples != expected_iterations:
                    incomplete.append(f"{database}/{workload}: expected {expected_iterations}, got {row.samples}")
        gates.append(
            _quality_gate(
                "expected_sample_counts",
                "passed" if not incomplete else "failed",
                "all configured database/workload rows have the expected sample count"
                if not incomplete
                else "; ".join(incomplete[:10]),
            )
        )
    else:
        gates.append(
            _quality_gate(
                "expected_sample_counts",
                "skipped",
                "environment does not include databases, required_workloads, and iterations",
            )
        )

    if "zyx" in databases and required_workloads:
        zyx_ok_workloads = {row.workload for row in rows if row.database == "zyx" and row.status == "ok" and row.samples > 0}
        missing_zyx = [workload for workload in required_workloads if workload not in zyx_ok_workloads]
        gates.append(
            _quality_gate(
                "zyx_primary_rows_present",
                "passed" if not missing_zyx else "failed",
                "ZYX has successful rows for every configured workload"
                if not missing_zyx
                else "missing ZYX rows: " + ", ".join(missing_zyx),
            )
        )
    else:
        gates.append(
            _quality_gate(
                "zyx_primary_rows_present",
                "skipped",
                "ZYX is not part of this run or required workloads are not declared",
            )
        )

    profile_workloads = {event.workload for event in profile_events if event.database == "zyx"}
    if "zyx" in databases:
        zyx_ok_workloads = {row.workload for row in rows if row.database == "zyx" and row.status == "ok" and row.samples > 0}
        missing_profiles = sorted(zyx_ok_workloads - profile_workloads)
        gates.append(
            _quality_gate(
                "zyx_profile_coverage",
                "passed" if not missing_profiles else "failed",
                "ZYX profile events cover every successful ZYX workload"
                if not missing_profiles
                else "missing profile events for: " + ", ".join(missing_profiles),
            )
        )
    else:
        gates.append(
            _quality_gate("zyx_profile_coverage", "skipped", "ZYX is not part of this run")
        )

    baseline_by_key = {
        (row.database, row.workload, row.scale): row
        for row in baseline_rows
        if row.status == "ok" and row.samples > 0 and row.p50_ms > 0
    }
    if baseline_by_key:
        regressions: list[str] = []
        common_rows = 0
        for row in rows:
            if row.status != "ok" or row.samples <= 0:
                continue
            baseline = baseline_by_key.get((row.database, row.workload, row.scale))
            if baseline is None:
                continue
            common_rows += 1
            ratio = row.p50_ms / baseline.p50_ms
            if ratio > max_regression_ratio:
                regressions.append(
                    f"{row.database}/{row.workload}: {ratio:.3f}x "
                    f"({baseline.p50_ms:.6g} -> {row.p50_ms:.6g} ms)"
                )
        gates.append(
            _quality_gate(
                "p50_regression_against_baseline",
                "passed" if not regressions else "failed",
                f"{common_rows} comparable row(s), max allowed ratio {max_regression_ratio:.3f}"
                if not regressions
                else "; ".join(regressions[:10]),
            )
        )
    else:
        gates.append(
            _quality_gate(
                "p50_regression_against_baseline",
                "skipped",
                "no baseline summary rows were provided",
            )
        )

    failure_count = sum(1 for gate in gates if gate["status"] == "failed")
    return {
        "status": "passed" if failure_count == 0 else "failed",
        "failure_count": failure_count,
        "gates": gates,
    }


def write_quality_gate_report(report: dict[str, object], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")


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
    profile_events_path: Path | None = None,
    baseline_summary_path: Path | None = None,
    max_regression_ratio: float = 1.10,
) -> dict[str, Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    rows = summarize_samples(read_samples(events_path)) + failure_summary_rows(read_failures(events_path))
    csv_path = output_dir / "summary.csv"
    markdown_path = output_dir / "summary.md"
    comparison_csv_path = output_dir / "comparison.csv"
    comparison_markdown_path = output_dir / "comparison.md"
    profile_summary_csv_path = output_dir / "profile_summary.csv"
    profile_summary_markdown_path = output_dir / "profile_summary.md"
    operation_summary_csv_path = output_dir / "operation_summary.csv"
    operation_summary_markdown_path = output_dir / "operation_summary.md"
    phase_category_summary_csv_path = output_dir / "phase_category_summary.csv"
    phase_category_summary_markdown_path = output_dir / "phase_category_summary.md"
    environment_path = output_dir / "environment.json"
    quality_path = output_dir / "quality_gates.json"

    write_summary_csv(rows, csv_path)
    write_summary_markdown(rows, markdown_path)
    comparison_rows = build_comparison_rows(rows)
    write_comparison_csv(comparison_rows, comparison_csv_path)
    write_comparison_markdown(comparison_rows, comparison_markdown_path)
    operation_summary_rows = build_operation_summary_rows(rows)
    write_operation_summary_csv(operation_summary_rows, operation_summary_csv_path)
    write_operation_summary_markdown(operation_summary_rows, operation_summary_markdown_path)
    profile_events = read_profile_events(profile_events_path)
    profile_summary_rows = build_profile_summary_rows(profile_events)
    write_profile_summary_csv(profile_summary_rows, profile_summary_csv_path)
    write_profile_summary_markdown(profile_summary_rows, profile_summary_markdown_path)
    phase_category_summary_rows = build_phase_category_summary_rows(profile_events)
    write_phase_category_summary_csv(phase_category_summary_rows, phase_category_summary_csv_path)
    write_phase_category_summary_markdown(phase_category_summary_rows, phase_category_summary_markdown_path)
    environment_payload = environment if environment is not None else _default_environment()
    environment_path.write_text(json.dumps(environment_payload, indent=2, sort_keys=True) + "\n")
    write_quality_gate_report(
        build_quality_gate_report(
            rows,
            environment_payload,
            profile_events,
            read_summary_rows(baseline_summary_path),
            max_regression_ratio,
        ),
        quality_path,
    )

    return {
        "csv": csv_path,
        "markdown": markdown_path,
        "comparison_csv": comparison_csv_path,
        "comparison_markdown": comparison_markdown_path,
        "profile_summary_csv": profile_summary_csv_path,
        "profile_summary_markdown": profile_summary_markdown_path,
        "operation_summary_csv": operation_summary_csv_path,
        "operation_summary_markdown": operation_summary_markdown_path,
        "phase_category_summary_csv": phase_category_summary_csv_path,
        "phase_category_summary_markdown": phase_category_summary_markdown_path,
        "environment": environment_path,
        "quality": quality_path,
    }
