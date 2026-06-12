from __future__ import annotations

import argparse
import csv
import json
from datetime import datetime, timezone
from pathlib import Path

from dataset.generate import SCALES
from runner.adapters.base import DEFAULT_PROFILE, EXECUTION_MODES, PROFILE_WORKLOADS, WARM_EXECUTION_MODE
from runner.run import (
    DATABASE_CHOICES,
    DEFAULT_DATABASES,
    DEFAULT_ITERATIONS,
    DEFAULT_OUTPUT_ROOT,
    DEFAULT_WARMUP,
    run_benchmark,
)

DEFAULT_THREAD_COUNTS = (1, 8)
DEFAULT_SCALE = "medium"


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")


def _read_summary(
    path: Path,
    expected_scale: str,
    expected_databases: list[str],
) -> dict[tuple[str, str], dict[str, object]]:
    rows: dict[tuple[str, str], dict[str, object]] = {}
    expected_database_set = set(expected_databases)
    with path.open(newline="") as handle:
        for payload in csv.DictReader(handle):
            scale = payload.get("scale", "")
            if scale != expected_scale:
                raise ValueError(f"{path} contains scale {scale!r}, expected {expected_scale!r}")
            database = payload["database"]
            if database not in expected_database_set:
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


def _summary_cell(
    summaries: dict[tuple[str, int], dict[tuple[str, str], dict[str, object]]],
    scale: str,
    thread_count: int,
    database: str,
    workload: str,
) -> dict[str, object] | None:
    return summaries.get((scale, thread_count), {}).get((database, workload))


def _p50(
    summaries: dict[tuple[str, int], dict[tuple[str, str], dict[str, object]]],
    scale: str,
    thread_count: int,
    database: str,
    workload: str,
) -> float | None:
    cell = _summary_cell(summaries, scale, thread_count, database, workload)
    if cell is None or cell.get("status") != "ok":
        return None
    return cell.get("p50_ms")  # type: ignore[return-value]


def _status_or_ms(
    summaries: dict[tuple[str, int], dict[tuple[str, str], dict[str, object]]],
    scale: str,
    thread_count: int,
    database: str,
    workload: str,
) -> str:
    cell = _summary_cell(summaries, scale, thread_count, database, workload)
    if cell is None:
        return ""
    status = cell.get("status")
    if status != "ok":
        return str(status)
    return _format_ms(cell.get("p50_ms"))  # type: ignore[arg-type]


def _speedup(baseline_p50_ms: float | None, current_p50_ms: float | None) -> float | None:
    if baseline_p50_ms is None or current_p50_ms is None or current_p50_ms <= 0:
        return None
    return baseline_p50_ms / current_p50_ms


def _workload_order(profile: str) -> list[str]:
    return list(PROFILE_WORKLOADS[profile])


def build_thread_scaling_rows(
    summary_paths: dict[tuple[str, int], Path],
    databases: list[str],
    scales: list[str],
    thread_counts: list[int],
    profile: str,
    execution_mode: str,
) -> list[dict[str, object]]:
    baseline_thread_count = thread_counts[0]
    summaries = {
        (scale, thread_count): _read_summary(path, expected_scale=scale, expected_databases=databases)
        for (scale, thread_count), path in summary_paths.items()
    }

    rows: list[dict[str, object]] = []
    for scale in scales:
        for workload in _workload_order(profile):
            for database in databases:
                baseline_p50_ms = _p50(summaries, scale, baseline_thread_count, database, workload)
                for thread_count in thread_counts:
                    cell = _summary_cell(summaries, scale, thread_count, database, workload)
                    if cell is None:
                        continue
                    p50_ms = cell.get("p50_ms") if cell.get("status") == "ok" else None
                    rows.append(
                        {
                            "scale": scale,
                            "profile": profile,
                            "execution_mode": execution_mode,
                            "workload": workload,
                            "database": database,
                            "thread_count": thread_count,
                            "status": cell.get("status", ""),
                            "p50_ms": p50_ms,
                            "baseline_thread_count": baseline_thread_count,
                            "baseline_p50_ms": baseline_p50_ms,
                            "speedup_vs_baseline": _speedup(baseline_p50_ms, p50_ms),  # type: ignore[arg-type]
                        }
                    )
    return rows


def write_thread_scaling_csv(rows: list[dict[str, object]], path: Path) -> None:
    fields = [
        "scale",
        "profile",
        "execution_mode",
        "workload",
        "database",
        "thread_count",
        "status",
        "p50_ms",
        "baseline_thread_count",
        "baseline_p50_ms",
        "speedup_vs_baseline",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def write_thread_scaling_markdown(
    summary_paths: dict[tuple[str, int], Path],
    databases: list[str],
    scales: list[str],
    thread_counts: list[int],
    profile: str,
    execution_mode: str,
    warmup: int,
    iterations: int,
    path: Path,
) -> None:
    baseline_thread_count = thread_counts[0]
    target_thread_count = thread_counts[-1]
    summaries = {
        (scale, thread_count): _read_summary(path, expected_scale=scale, expected_databases=databases)
        for (scale, thread_count), path in summary_paths.items()
    }

    lines = [
        "# Thread Scaling Performance",
        "",
        "Purpose: compare single-operation execution with explicit thread budgets. Unit: p50 ms; lower is better.",
        "",
        "| Field | Value |",
        "| --- | --- |",
        f"| scales | {', '.join(scales)} |",
        f"| profile | {profile} |",
        f"| execution mode | {execution_mode} |",
        f"| warmup | {warmup} |",
        f"| iterations | {iterations} |",
        f"| threads | {', '.join(str(value) for value in thread_counts)} |",
        f"| baseline thread count | {baseline_thread_count} |",
        f"| target thread count | {target_thread_count} |",
        f"| databases | {', '.join(databases)} |",
        "",
        "A speedup above `1.0` means the target thread count is faster than the baseline for the same database/workload.",
        "",
    ]

    for scale in scales:
        lines.extend(
            [
                f"## {scale}",
                "",
                "| workload | "
                + " | ".join(
                    f"{database}@{baseline_thread_count} | {database}@{target_thread_count} | {database} speedup"
                    for database in databases
                )
                + " | ZYX/best competitor @target |",
                "| --- | " + " | ".join(["---: | ---: | ---:"] * len(databases)) + " | ---: |",
            ]
        )

        for workload in _workload_order(profile):
            cells: list[str] = []
            for database in databases:
                baseline_p50_ms = _p50(summaries, scale, baseline_thread_count, database, workload)
                target_p50_ms = _p50(summaries, scale, target_thread_count, database, workload)
                cells.extend(
                    [
                        _status_or_ms(summaries, scale, baseline_thread_count, database, workload),
                        _status_or_ms(summaries, scale, target_thread_count, database, workload),
                        _format_ratio(_speedup(baseline_p50_ms, target_p50_ms)),
                    ]
                )

            zyx_target = _p50(summaries, scale, target_thread_count, "zyx", workload)
            competitor_targets = [
                _p50(summaries, scale, target_thread_count, database, workload)
                for database in databases
                if database != "zyx"
            ]
            competitor_targets = [value for value in competitor_targets if value is not None]
            best_competitor = min(competitor_targets) if competitor_targets else None
            zyx_ratio = zyx_target / best_competitor if zyx_target is not None and best_competitor else None
            lines.append("| " + " | ".join([f"`{workload}`", *cells, _format_ratio(zyx_ratio)]) + " |")
        lines.append("")

    lines.extend(
        [
            "## Notes",
            "",
            "- Thread counts are passed into adapters explicitly instead of relying on each engine's default.",
            "- `0` means adapter/engine auto-detected thread count when that adapter supports it.",
            "- Detailed long-form results for every configured thread count are in `thread_scaling.csv`.",
        ]
    )
    path.write_text("\n".join(lines) + "\n")


def write_thread_scaling_reports(
    summary_paths: dict[tuple[str, int], Path],
    databases: list[str],
    scales: list[str],
    thread_counts: list[int],
    profile: str,
    execution_mode: str,
    warmup: int,
    iterations: int,
    output_dir: Path,
) -> dict[str, Path]:
    rows = build_thread_scaling_rows(summary_paths, databases, scales, thread_counts, profile, execution_mode)
    csv_path = output_dir / "thread_scaling.csv"
    markdown_path = output_dir / "thread_scaling.md"
    write_thread_scaling_csv(rows, csv_path)
    write_thread_scaling_markdown(
        summary_paths, databases, scales, thread_counts, profile, execution_mode, warmup, iterations, markdown_path
    )
    return {"csv": csv_path, "markdown": markdown_path}


def run_thread_scaling(
    databases: list[str],
    scales: list[str],
    thread_counts: list[int],
    seed: int,
    output_root: Path,
    warmup: int,
    iterations: int,
    profile: str = DEFAULT_PROFILE,
    execution_mode: str = WARM_EXECUTION_MODE,
    keep_db_artifacts: bool = False,
) -> Path:
    if not databases:
        raise ValueError("at least one database is required")
    if not scales:
        raise ValueError("at least one scale is required")
    if not thread_counts:
        raise ValueError("at least one thread count is required")
    for scale in scales:
        if scale not in SCALES:
            raise ValueError(f"unsupported scale: {scale}")
    for thread_count in thread_counts:
        if thread_count < 0:
            raise ValueError("thread counts must be >= 0")
    if profile not in PROFILE_WORKLOADS:
        raise ValueError(f"unsupported profile: {profile}")
    if execution_mode not in EXECUTION_MODES:
        raise ValueError(f"execution mode must be one of: {', '.join(EXECUTION_MODES)}")
    if warmup < 0:
        raise ValueError("warmup must be >= 0")
    if iterations <= 0:
        raise ValueError("iterations must be > 0")

    output_root.mkdir(parents=True, exist_ok=True)
    runs: list[dict[str, object]] = []
    summary_paths: dict[tuple[str, int], Path] = {}
    for scale in scales:
        for thread_count in thread_counts:
            result_dir = run_benchmark(
                databases=databases,
                scale=scale,
                seed=seed,
                output_root=output_root,
                warmup=warmup,
                iterations=iterations,
                profile=profile,
                execution_mode=execution_mode,
                keep_db_artifacts=keep_db_artifacts,
                threads=thread_count,
            )
            status_path = result_dir / "run_status.json"
            run_status = json.loads(status_path.read_text()) if status_path.exists() else {}
            summary_paths[(scale, thread_count)] = result_dir / "summary.csv"
            runs.append(
                {
                    "scale": scale,
                    "thread_count": thread_count,
                    "execution_mode": execution_mode,
                    "result_dir": str(result_dir),
                    "summary": str(result_dir / "summary.csv"),
                    "comparison": str(result_dir / "comparison.csv"),
                    "quality_gates": str(result_dir / "quality_gates.json"),
                    "run_status": run_status,
                }
            )

    report_paths = write_thread_scaling_reports(
        summary_paths, databases, scales, thread_counts, profile, execution_mode, warmup, iterations, output_root
    )
    manifest = {
        "thread_scaling_schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "databases": databases,
        "scales": scales,
        "thread_counts": thread_counts,
        "baseline_thread_count": thread_counts[0],
        "seed": seed,
        "warmup": warmup,
        "iterations": iterations,
        "profile": profile,
        "execution_mode": execution_mode,
        "keep_db_artifacts": keep_db_artifacts,
        "report_csv": str(report_paths["csv"]),
        "report_markdown": str(report_paths["markdown"]),
        "runs": runs,
    }
    manifest_path = output_root / f"{_utc_timestamp()}-{profile}-{execution_mode}-thread-scaling.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    (output_root / "latest-thread-scaling.txt").write_text(manifest_path.name + "\n")
    return manifest_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Run graph database benchmark thread-scaling reports")
    parser.add_argument("--database", action="append", choices=DATABASE_CHOICES, dest="databases")
    parser.add_argument("--scale", action="append", choices=sorted(SCALES), dest="scales")
    parser.add_argument("--thread-count", action="append", type=int, dest="thread_counts")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--profile", choices=sorted(PROFILE_WORKLOADS), default=DEFAULT_PROFILE)
    parser.add_argument("--execution-mode", choices=EXECUTION_MODES, default=WARM_EXECUTION_MODE)
    parser.add_argument("--warmup", type=int, default=DEFAULT_WARMUP)
    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS)
    parser.add_argument("--keep-db-artifacts", action="store_true")
    args = parser.parse_args()

    manifest_path = run_thread_scaling(
        databases=args.databases if args.databases else list(DEFAULT_DATABASES),
        scales=args.scales if args.scales else [DEFAULT_SCALE],
        thread_counts=args.thread_counts if args.thread_counts else list(DEFAULT_THREAD_COUNTS),
        seed=args.seed,
        output_root=args.output_root,
        warmup=args.warmup,
        iterations=args.iterations,
        profile=args.profile,
        execution_mode=args.execution_mode,
        keep_db_artifacts=args.keep_db_artifacts,
    )
    print(manifest_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
