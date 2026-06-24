from __future__ import annotations

import argparse
import csv
import json
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from dataset.generate import SCALES
from runner.adapters.base import WARM_EXECUTION_MODE
from runner.run import DATABASE_CHOICES, DEFAULT_DATABASES, DEFAULT_ITERATIONS, DEFAULT_OUTPUT_ROOT, DEFAULT_WARMUP, run_benchmark

STEADY_STATE_PROFILE = "operational_steady_state"
STEADY_STATE_PROFILES = ("scan", "indexed", "retrieval", "multihop", "write", "operational_dynamic")
DEFAULT_SCALE = "medium"


@dataclass(frozen=True)
class OperationSpec:
    category: str
    profile: str
    workload: str
    meaning: str


OPERATION_SPECS = [
    OperationSpec("load/index build", "scan", "load_nodes_edges", "base graph load + primary id index"),
    OperationSpec("load/index build", "indexed", "load_nodes_edges", "graph load + benchmark property indexes"),
    OperationSpec("load/index build", "operational_dynamic", "load_nodes_edges", "post-load dynamic workload base graph"),
    OperationSpec("property filter", "scan", "label_scan_filter", "label + single-property filter"),
    OperationSpec("property filter", "scan", "all_nodes_property_filter", "all-node property filter"),
    OperationSpec("property filter", "scan", "label_multi_property_filter", "label + multi-property filter"),
    OperationSpec("read scan", "scan", "relationship_type_scan", "relationship type count"),
    OperationSpec("property filter", "scan", "relationship_property_filter", "relationship property filter"),
    OperationSpec("adjacency expand", "scan", "one_hop_expand", "anchored one-hop expansion count"),
    OperationSpec("adjacency expand", "scan", "two_hop_expand", "anchored two-hop expansion count"),
    OperationSpec("adjacency expand", "scan", "shortest_path_chain", "short bounded path existence"),
    OperationSpec("topk/group", "scan", "aggregation_group_by", "distinct group count"),
    OperationSpec("topk/group", "scan", "aggregation_count_by_group", "group by + count"),
    OperationSpec("topk/group", "scan", "topk_property_sort", "property sort TopK"),
    OperationSpec("property filter", "indexed", "point_lookup_indexed", "indexed point lookup"),
    OperationSpec("property filter", "indexed", "property_equality_indexed", "indexed equality predicate"),
    OperationSpec("property filter", "indexed", "property_range_indexed", "indexed range predicate"),
    OperationSpec("data retrieval", "retrieval", "point_node_fetch_by_id", "fetch one node projection by id"),
    OperationSpec("data retrieval", "retrieval", "point_edge_fetch_by_endpoints", "fetch one relationship projection by endpoints"),
    OperationSpec("data retrieval", "retrieval", "batch_node_fetch_100", "fetch 100 node projections"),
    OperationSpec("data retrieval", "retrieval", "one_hop_fetch_neighbor_ids", "fetch one-hop neighbor ids"),
    OperationSpec("data retrieval", "retrieval", "one_hop_fetch_neighbor_records", "fetch one-hop neighbor projections"),
    OperationSpec("data retrieval", "retrieval", "property_index_fetch_users_by_country", "fetch projected users through equality index"),
    OperationSpec("data retrieval", "retrieval", "range_index_fetch_user_projection", "fetch projected users through range index"),
    OperationSpec("data retrieval", "retrieval", "relationship_property_fetch", "fetch relationship-property projections"),
    OperationSpec("adjacency expand", "multihop", "reachable_within_6", "bounded reachability <= 6 hops"),
    OperationSpec("adjacency expand", "multihop", "reachable_within_12", "bounded reachability <= 12 hops"),
    OperationSpec("adjacency expand", "multihop", "reachable_within_24", "bounded reachability <= 24 hops"),
    OperationSpec("adjacency expand", "multihop", "reachable_within_30", "bounded reachability <= 30 hops"),
    OperationSpec("adjacency expand", "multihop", "varlength_frontier_count", "batched variable-length expansion count"),
    OperationSpec("point write", "write", "point_create_node", "create one node"),
    OperationSpec("point write", "write", "point_create_edge", "create one relationship"),
    OperationSpec("point write", "write", "point_update_node_property", "update one node property"),
    OperationSpec("point write", "write", "point_update_edge_property", "update one relationship property"),
    OperationSpec("point write", "write", "point_create_delete_edge", "create then delete one relationship"),
    OperationSpec("point write", "write", "write_then_read_edge", "write one relationship then read it"),
    OperationSpec("adjacency expand", "operational_dynamic", "index_seek_then_one_hop_expand", "indexed seed + one-hop expansion"),
    OperationSpec("adjacency expand", "operational_dynamic", "index_seek_then_two_hop_expand", "indexed seed + two-hop expansion"),
    OperationSpec("point write", "operational_dynamic", "post_persist_create_node", "post-load create one node"),
    OperationSpec("point write", "operational_dynamic", "post_persist_create_edge", "post-load create one relationship"),
    OperationSpec("adjacency expand", "operational_dynamic", "write_then_one_hop_expand", "write then local one-hop count"),
    OperationSpec("batch write", "operational_dynamic", "batch_create_edges_100", "append 100 relationships"),
    OperationSpec("batch write", "operational_dynamic", "batch_create_edges_1000", "append 1000 relationships"),
    OperationSpec("batch write", "operational_dynamic", "batch_create_edges_10000", "append 10000 relationships"),
    OperationSpec("batch write", "operational_dynamic", "batch_create_edges_100_then_one_hop_expand", "append 100 relationships then local count"),
    OperationSpec("batch write", "operational_dynamic", "batch_create_edges_10000_then_one_hop_expand", "append 10000 relationships then local count"),
]


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")


def _read_summary(
    path: Path,
    expected_scale: str | None = None,
    expected_databases: list[str] | None = None,
) -> dict[tuple[str, str], dict[str, object]]:
    rows: dict[tuple[str, str], dict[str, object]] = {}
    expected_database_set = set(expected_databases or [])
    with path.open(newline="") as handle:
        for payload in csv.DictReader(handle):
            scale = payload.get("scale", "")
            if expected_scale is not None and scale != expected_scale:
                raise ValueError(f"{path} contains scale {scale!r}, expected {expected_scale!r}")
            database = payload["database"]
            if expected_database_set and database not in expected_database_set:
                raise ValueError(f"{path} contains unexpected database {database!r}")
            status = payload.get("status", "ok")
            rows[(database, payload["workload"])] = {
                "status": status,
                "p50_ms": float(payload["p50_ms"]) if status == "ok" else None,
            }
    return rows


def _format_ms(value: float | None) -> str:
    return "" if value is None else f"{value:.3f}"


def _format_ratio(value: float | None) -> str:
    if value is None:
        return ""
    if value < 0.01:
        return f"{value:.4f}"
    return f"{value:.2f}"


def _format_database_cell(row: dict[str, object], database: str) -> str:
    status = row.get(f"{database}_status")
    if status and status != "ok":
        return str(status)
    return _format_ms(row.get(f"{database}_p50_ms"))  # type: ignore[arg-type]


def _category_order() -> dict[str, int]:
    return {spec.category: index for index, spec in enumerate(OPERATION_SPECS)}


def build_operational_rows(
    profile_summary_paths: dict[str, Path],
    databases: list[str],
    primary_database: str = "zyx",
    expected_scale: str | None = None,
) -> list[dict[str, object]]:
    summaries = {
        profile: _read_summary(path, expected_scale=expected_scale, expected_databases=databases)
        for profile, path in profile_summary_paths.items()
    }
    rows: list[dict[str, object]] = []
    for spec in OPERATION_SPECS:
        summary = summaries.get(spec.profile, {})
        cells_by_database = {database: summary.get((database, spec.workload)) for database in databases}
        if not any(cells_by_database.values()):
            continue
        p50_by_database = {
            database: (cell.get("p50_ms") if cell is not None and cell.get("status") == "ok" else None)
            for database, cell in cells_by_database.items()
        }
        status_by_database = {
            database: (str(cell.get("status", "")) if cell is not None else "") for database, cell in cells_by_database.items()
        }
        present = [(database, value) for database, value in p50_by_database.items() if value is not None]
        if not present:
            continue

        ranked = sorted(present, key=lambda item: (item[1], item[0]))
        fastest_database, fastest_p50 = ranked[0]
        non_primary_ranked = [(database, value) for database, value in ranked if database != primary_database]
        fastest_non_primary = non_primary_ranked[0] if non_primary_ranked else (None, None)
        primary_p50 = p50_by_database.get(primary_database)
        primary_rank = None
        primary_vs_fastest = None
        primary_vs_fastest_non_primary = None
        if primary_p50 is not None:
            primary_rank = 1 + sum(1 for _, value in ranked if value < primary_p50)
            primary_vs_fastest = primary_p50 / fastest_p50 if fastest_p50 > 0 else None
            non_primary_p50 = fastest_non_primary[1]
            primary_vs_fastest_non_primary = (
                primary_p50 / non_primary_p50 if non_primary_p50 is not None and non_primary_p50 > 0 else None
            )

        row: dict[str, object] = {
            "category": spec.category,
            "profile": spec.profile,
            "workload": spec.workload,
            "meaning": spec.meaning,
            "fastest_database": fastest_database,
            "fastest_p50_ms": fastest_p50,
            "fastest_non_zyx_database": fastest_non_primary[0],
            "fastest_non_zyx_p50_ms": fastest_non_primary[1],
            "zyx_p50_ms": primary_p50,
            "zyx_rank": primary_rank,
            "zyx_vs_fastest": primary_vs_fastest,
            "zyx_vs_fastest_non_zyx": primary_vs_fastest_non_primary,
            "primary_database": primary_database,
            "primary_p50_ms": primary_p50,
            "primary_rank": primary_rank,
            "primary_vs_fastest": primary_vs_fastest,
            "primary_vs_fastest_non_primary": primary_vs_fastest_non_primary,
            "fastest_non_primary_database": fastest_non_primary[0],
            "fastest_non_primary_p50_ms": fastest_non_primary[1],
        }
        for database, value in p50_by_database.items():
            row[f"{database}_p50_ms"] = value
            row[f"{database}_status"] = status_by_database[database]
        rows.append(row)
    return rows


def write_operational_csv(rows: list[dict[str, object]], databases: list[str], path: Path) -> None:
    fields = [
        "category",
        "profile",
        "workload",
        "meaning",
        *[f"{database}_p50_ms" for database in databases],
        *[f"{database}_status" for database in databases],
        "fastest_database",
        "fastest_p50_ms",
        "fastest_non_primary_database",
        "fastest_non_primary_p50_ms",
        "primary_database",
        "primary_p50_ms",
        "primary_rank",
        "primary_vs_fastest",
        "primary_vs_fastest_non_primary",
    ]
    if len(fields) != len(set(fields)):
        duplicates = sorted({field for field in fields if fields.count(field) > 1})
        raise ValueError("operational CSV field names must be unique: " + ", ".join(duplicates))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def _category_summary(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    grouped: dict[str, list[dict[str, object]]] = {}
    for row in rows:
        grouped.setdefault(str(row["category"]), []).append(row)

    summary: list[dict[str, object]] = []
    for category in sorted(grouped, key=lambda value: _category_order().get(value, 10_000)):
        category_rows = grouped[category]
        ratios = [
            float(row["zyx_vs_fastest_non_zyx"])
            for row in category_rows
            if row.get("zyx_vs_fastest_non_zyx") is not None
        ]
        wins = sum(
            1
            for row in category_rows
            if row.get("fastest_database") == "zyx" and row.get("fastest_non_zyx_database") is not None
        )
        non_zyx_wins = sum(
            1
            for row in category_rows
            if row.get("fastest_database") not in {None, "zyx"} and row.get("zyx_p50_ms") is not None
        )
        not_comparable = len(category_rows) - wins - non_zyx_wins
        geo = None
        if ratios:
            product = 1.0
            for ratio in ratios:
                product *= ratio
            geo = product ** (1.0 / len(ratios))
        summary.append(
            {
                "category": category,
                "workloads": len(category_rows),
                "zyx_wins": wins,
                "non_zyx_wins": non_zyx_wins,
                "not_comparable": not_comparable,
                "geomean_zyx_vs_best_competitor": geo,
            }
        )
    return summary


def write_operational_markdown(
    rows: list[dict[str, object]],
    databases: list[str],
    scale: str,
    warmup: int,
    iterations: int,
    threads: int | None,
    path: Path,
) -> None:
    lines = [
        "# Operational Steady-State Performance",
        "",
        "Primary dimension: medium-scale steady-state p50 latency by default. Unit: ms; lower is better.",
        "",
        "| Field | Value |",
        "| --- | --- |",
        f"| scale | {scale} |",
        "| execution mode | operational steady-state (`warm`) |",
        "| statistic | p50 latency |",
        f"| warmup | {warmup} |",
        f"| iterations | {iterations} |",
        f"| threads | {'adapter default' if threads is None else threads} |",
        f"| databases | {', '.join(databases)} |",
        "| write semantics | statement-return latency; durability barriers are intentionally excluded from this main report |",
        "",
        "## Category Summary",
        "",
        "| category | workloads | ZYX wins | non-ZYX wins | not comparable | geomean ZYX/best competitor |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in _category_summary(rows):
        lines.append(
            "| "
            + " | ".join(
                [
                    str(row["category"]),
                    str(row["workloads"]),
                    str(row["zyx_wins"]),
                    str(row["non_zyx_wins"]),
                    str(row["not_comparable"]),
                    _format_ratio(row.get("geomean_zyx_vs_best_competitor")),  # type: ignore[arg-type]
                ]
            )
            + " |"
        )

    current_category: str | None = None
    for row in rows:
        category = str(row["category"])
        if category != current_category:
            current_category = category
            lines.extend(
                [
                    "",
                    f"## {category}",
                    "",
                    "| workload | meaning | "
                    + " | ".join(f"{database} p50/status" for database in databases)
                    + " | fastest | best competitor | ZYX/best competitor | ZYX rank |",
                    "| --- | --- | "
                    + " | ".join(["---:"] * len(databases))
                    + " | --- | --- | ---: | ---: |",
                ]
            )
        database_cells = [_format_database_cell(row, database) for database in databases]
        lines.append(
            "| "
            + " | ".join(
                [
                    f"`{row['workload']}`",
                    str(row["meaning"]),
                    *database_cells,
                    str(row["fastest_database"]),
                    "" if row.get("fastest_non_zyx_database") is None else str(row["fastest_non_zyx_database"]),
                    _format_ratio(row.get("zyx_vs_fastest_non_zyx")),  # type: ignore[arg-type]
                    "" if row.get("zyx_rank") is None else str(row["zyx_rank"]),
                ]
            )
            + " |"
        )

    lines.extend(
        [
            "",
            "## Interpretation",
            "",
            "- `ZYX/best competitor < 1.0` means ZYX is faster than every non-ZYX competitor for that workload.",
            "- `ZYX/best competitor > 1.0` means ZYX is slower than the fastest non-ZYX competitor.",
            "- This report intentionally excludes startup/first-operation and explicit durability-barrier measurements.",
        ]
    )
    path.write_text("\n".join(lines) + "\n")


def write_operational_reports(
    profile_summary_paths: dict[str, Path],
    databases: list[str],
    scale: str,
    warmup: int,
    iterations: int,
    threads: int | None,
    output_dir: Path,
) -> dict[str, Path]:
    rows = build_operational_rows(profile_summary_paths, databases, expected_scale=scale)
    csv_path = output_dir / "operational_steady_state.csv"
    markdown_path = output_dir / "operational_steady_state.md"
    write_operational_csv(rows, databases, csv_path)
    write_operational_markdown(rows, databases, scale, warmup, iterations, threads, markdown_path)
    return {"csv": csv_path, "markdown": markdown_path}


def run_operational_steady_state(
    databases: list[str],
    scale: str,
    seed: int,
    output_root: Path,
    warmup: int,
    iterations: int,
    keep_db_artifacts: bool = False,
    threads: int | None = None,
) -> Path:
    if not databases:
        raise ValueError("at least one database is required")
    if scale not in SCALES:
        raise ValueError(f"unsupported scale: {scale}")
    if warmup < 0:
        raise ValueError("warmup must be >= 0")
    if iterations <= 0:
        raise ValueError("iterations must be > 0")
    if threads is not None and threads < 0:
        raise ValueError("threads must be >= 0")

    output_root.mkdir(parents=True, exist_ok=True)
    runs: list[dict[str, object]] = []
    profile_summary_paths: dict[str, Path] = {}
    for profile in STEADY_STATE_PROFILES:
        result_dir = run_benchmark(
            databases=databases,
            scale=scale,
            seed=seed,
            output_root=output_root,
            warmup=warmup,
            iterations=iterations,
            profile=profile,
            execution_mode=WARM_EXECUTION_MODE,
            keep_db_artifacts=keep_db_artifacts,
            threads=threads,
        )
        status_path = result_dir / "run_status.json"
        run_status = json.loads(status_path.read_text()) if status_path.exists() else {}
        profile_summary_paths[profile] = result_dir / "summary.csv"
        runs.append(
            {
                "profile": profile,
                "execution_mode": WARM_EXECUTION_MODE,
                "result_dir": str(result_dir),
                "summary": str(result_dir / "summary.csv"),
                "comparison": str(result_dir / "comparison.csv"),
                "quality_gates": str(result_dir / "quality_gates.json"),
                "run_status": run_status,
            }
        )

    report_paths = write_operational_reports(profile_summary_paths, databases, scale, warmup, iterations, threads, output_root)
    manifest = {
        "operational_steady_state_schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "databases": databases,
        "scale": scale,
        "seed": seed,
        "warmup": warmup,
        "iterations": iterations,
        "threads": threads,
        "execution_mode": WARM_EXECUTION_MODE,
        "profile": STEADY_STATE_PROFILE,
        "component_profiles": list(STEADY_STATE_PROFILES),
        "keep_db_artifacts": keep_db_artifacts,
        "report_csv": str(report_paths["csv"]),
        "report_markdown": str(report_paths["markdown"]),
        "runs": runs,
    }
    manifest_path = output_root / f"{_utc_timestamp()}-{scale}-{STEADY_STATE_PROFILE}-matrix.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    (output_root / "latest-operational-steady-state.txt").write_text(manifest_path.name + "\n")
    return manifest_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the Operational Steady-State graph database benchmark report")
    parser.add_argument("--database", action="append", choices=DATABASE_CHOICES, dest="databases")
    parser.add_argument("--scale", choices=sorted(SCALES), default=DEFAULT_SCALE)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--warmup", type=int, default=DEFAULT_WARMUP)
    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS)
    parser.add_argument("--keep-db-artifacts", action="store_true")
    parser.add_argument("--threads", type=int)
    args = parser.parse_args()

    manifest_path = run_operational_steady_state(
        databases=args.databases if args.databases else list(DEFAULT_DATABASES),
        scale=args.scale,
        seed=args.seed,
        output_root=args.output_root,
        warmup=args.warmup,
        iterations=args.iterations,
        keep_db_artifacts=args.keep_db_artifacts,
        threads=args.threads,
    )
    print(manifest_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
