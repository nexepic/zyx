from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, TextIO

from dataset.generate import SCALES, generate_graph, write_dataset
from runner.adapters.base import (
    BenchmarkAdapter,
    DEFAULT_PROFILE,
    EXECUTION_MODES,
    MUTATING_WORKLOADS,
    PROFILE_WORKLOADS,
    WARM_EXECUTION_MODE,
)
from runner.adapters.fake import FakeAdapter
from runner.models import FailureEvent
from runner.summarize import write_summary_outputs

DATABASE_CHOICES = ("fake", "zyx", "neo4j", "memgraph", "kuzu")
DEFAULT_DATABASES = ["zyx", "neo4j", "memgraph", "kuzu"]
DEFAULT_OUTPUT_ROOT = Path("benchmarks/graphdb-comparison/results")
DEFAULT_WARMUP = 20
DEFAULT_ITERATIONS = 100


def _measurement_semantics(profile: str, execution_mode: str) -> dict[str, object]:
    execution_semantics = {
        "warm": (
            "Measure operations on one loaded/open database handle after the configured query warmup. "
            "This is a steady-state latency view."
        ),
        "opened": (
            "Measure operations on one loaded/open database handle without explicit query warmup. "
            "first_ms is the first operation after open; later samples reuse the same handle."
        ),
        "cold-ish": (
            "Use a fresh prepared adapter/database handle for each measured query. The measured sample excludes "
            "data load and handle open/close time, and does not flush OS or service page caches."
        ),
    }
    profile_semantics: dict[str, object] = {
        "latency_contract": "read_query_or_load_latency",
        "durability_contract": "not_applicable",
        "phase_contract": "profile outputs classify raw timers into operation families and semantic phases when available.",
    }
    if profile == "write":
        profile_semantics = {
            "latency_contract": "statement_return_latency",
            "durability_contract": (
                "No adapter-added checkpoint is forced. Each database is measured at the point where its normal "
                "statement/autocommit API returns."
            ),
            "phase_contract": (
                "Statement latency, commit wrapper work, WAL fsync timers, close/checkpoint timers, and load/import "
                "timers are reported separately in phase_category_summary.* when the adapter emits profile events."
            ),
        }
    elif profile == "write_durable":
        profile_semantics = {
            "latency_contract": "durable_after_each_write_latency",
            "durability_contract": (
                "Each measured write includes an adapter-defined durability barrier before the timer stops."
            ),
            "barrier_by_database": {
                "zyx": "No extra adapter call: ZYX auto-commit fsyncs the WAL commit record; the main database checkpoint may be deferred until the WAL threshold or clean close.",
                "kuzu": "CHECKPOINT after each measured write.",
                "neo4j": "No adapter-added barrier; service autocommit durability is engine/configuration defined.",
                "memgraph": "No adapter-added barrier; service autocommit durability is engine/configuration defined.",
            },
            "phase_contract": (
                "Durability barriers are included in sample latency; raw profile timers are additionally grouped into "
                "statement_latency, durable_wal_fsync, close_checkpoint, and load_import categories."
            ),
        }
    elif profile == "operational_dynamic":
        profile_semantics = {
            "latency_contract": "post_persist_incremental_write_and_local_traversal_latency",
            "durability_contract": (
                "The base graph is loaded and persisted before measuring incremental operations. "
                "Single-statement writes use each adapter's normal statement-return semantics; batch workloads use "
                "the engine's public batch-friendly path and report affected-edge counts."
            ),
            "phase_contract": (
                "Operational dynamic keeps post-load local traversal, point writes, batch writes, and close checkpoint "
                "timers visible as separate operation and phase summaries."
            ),
        }
    return {
        "execution_mode": execution_mode,
        "execution_contract": execution_semantics.get(execution_mode, "unknown"),
        "profile": profile,
        "profile_contract": profile_semantics,
        "workload_isolation": {
            "load_nodes_edges": "fresh adapter/database per warmup and measured load sample",
            "cold-ish": "fresh loaded adapter/database per measured non-load workload sample",
            "mutating_warm_opened": "fresh loaded adapter/database per mutating workload, excluding load/open from measured latency",
        },
    }


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")


def _create_result_dir(output_root: Path, databases: list[str], scale: str, profile: str) -> Path:
    base_name = f"{_utc_timestamp()}-{scale}-{profile}-{'-'.join(databases)}"
    for suffix in [""] + [f"-{attempt}" for attempt in range(1, 100)]:
        result_dir = output_root / f"{base_name}{suffix}"
        try:
            result_dir.mkdir(parents=True, exist_ok=False)
            return result_dir
        except FileExistsError:
            continue
    raise FileExistsError(f"could not create unique result directory under {output_root}")


def _write_jsonl(handle: TextIO, event: dict[str, object]) -> None:
    handle.write(json.dumps(event, sort_keys=True) + "\n")


def _error_event(failure: FailureEvent) -> dict[str, object]:
    event = failure.to_event()
    event["event"] = "error"
    return event


def _write_failure(raw_handle: TextIO, errors_handle: TextIO, failure: FailureEvent) -> None:
    _write_jsonl(errors_handle, _error_event(failure))
    _write_jsonl(raw_handle, failure.to_event())


def _write_profile_events(result_dir: Path, events: Iterable[object]) -> None:
    serialized: list[dict[str, object]] = []
    for event in events:
        to_event = getattr(event, "to_event", None)
        if callable(to_event):
            serialized.append(to_event())
        elif isinstance(event, dict):
            serialized.append(event)
        else:
            raise TypeError(f"unsupported profile event type: {type(event).__name__}")
    if not serialized:
        return
    profile_path = result_dir / "zyx_profiles.jsonl"
    with profile_path.open("a") as handle:
        for event in serialized:
            _write_jsonl(handle, event)


def _cleanup_adapter_artifacts(adapter: BenchmarkAdapter) -> list[str]:
    cleanup = getattr(adapter, "cleanup_artifacts", None)
    if not callable(cleanup):
        return []
    removed = cleanup()
    return [str(path) for path in removed]


def _git_metadata() -> dict[str, object]:
    repo_root = Path(__file__).resolve().parents[3]

    def run_git(*args: str) -> str | None:
        try:
            completed = subprocess.run(
                ["git", *args],
                cwd=repo_root,
                check=False,
                capture_output=True,
                text=True,
                timeout=5,
            )
        except (OSError, subprocess.TimeoutExpired):
            return None
        if completed.returncode != 0:
            return None
        return completed.stdout.strip()

    commit = run_git("rev-parse", "HEAD") or "unknown"
    branch = run_git("rev-parse", "--abbrev-ref", "HEAD") or "unknown"
    status = run_git("status", "--porcelain")
    return {
        "git_branch": branch,
        "git_commit": commit,
        "git_dirty": None if status is None else bool(status),
    }


def _environment(
    databases: Iterable[str],
    scale: str,
    seed: int,
    warmup: int,
    iterations: int,
    profile: str,
    execution_mode: str,
    keep_db_artifacts: bool,
    threads: int | None,
) -> dict[str, object]:
    thread_contract = (
        "adapter default thread configuration"
        if threads is None
        else ("adapter auto-detected thread count" if threads == 0 else f"fixed {threads} execution thread(s)")
    )
    environment = {
        "benchmark_schema_version": 1,
        "event": "environment",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "databases": list(databases),
        "scale": scale,
        "profile": profile,
        "keep_db_artifacts": keep_db_artifacts,
        "measurement_semantics": _measurement_semantics(profile, execution_mode),
        "required_workloads": PROFILE_WORKLOADS[profile],
        "mutating_workloads": [workload for workload in PROFILE_WORKLOADS[profile] if workload in MUTATING_WORKLOADS],
        "seed": seed,
        "warmup": warmup,
        "iterations": iterations,
        "execution_mode": execution_mode,
        "threads": threads,
        "thread_contract": thread_contract,
        "machine": platform.machine(),
        "platform": platform.platform(),
        "python": platform.python_version(),
        "system": platform.system(),
        "zyx_compare_bench": os.environ.get("ZYX_COMPARE_BENCH", "/usr/local/bin/zyx-compare-bench"),
    }
    environment.update(_git_metadata())
    return environment


def _adapter_for(
    database: str,
    dataset_dir: Path,
    scale: str,
    profile: str,
    threads: int | None = None,
) -> BenchmarkAdapter:
    if database == "fake":
        return FakeAdapter(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile, threads=threads)
    if database == "kuzu":
        from runner.adapters.kuzu import KuzuAdapter

        return KuzuAdapter(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile, threads=threads)
    if database == "neo4j":
        from runner.adapters.neo4j import Neo4jAdapter

        return Neo4jAdapter(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile, threads=threads)
    if database == "memgraph":
        from runner.adapters.memgraph import MemgraphAdapter

        return MemgraphAdapter(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile, threads=threads)
    if database == "zyx":
        from runner.adapters.zyx import ZyxAdapter

        return ZyxAdapter(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile, threads=threads)
    raise ValueError(f"unsupported database: {database}")


def run_benchmark(
    databases: list[str],
    scale: str,
    seed: int,
    output_root: Path,
    warmup: int,
    iterations: int,
    profile: str = DEFAULT_PROFILE,
    execution_mode: str = WARM_EXECUTION_MODE,
    baseline_summary: Path | None = None,
    max_regression_ratio: float = 1.10,
    keep_db_artifacts: bool = False,
    threads: int | None = None,
) -> Path:
    if scale not in SCALES:
        raise ValueError(f"unsupported scale: {scale}")
    if profile not in PROFILE_WORKLOADS:
        raise ValueError(f"unsupported profile: {profile}")
    if warmup < 0:
        raise ValueError("warmup must be >= 0")
    if iterations <= 0:
        raise ValueError("iterations must be > 0")
    if execution_mode not in EXECUTION_MODES:
        raise ValueError(f"execution mode must be one of: {', '.join(EXECUTION_MODES)}")
    if max_regression_ratio < 1.0:
        raise ValueError("max regression ratio must be >= 1.0")
    if threads is not None and threads < 0:
        raise ValueError("threads must be >= 0")
    if baseline_summary is not None and not baseline_summary.exists():
        raise ValueError(f"baseline summary does not exist: {baseline_summary}")

    result_dir = _create_result_dir(output_root, databases, scale, profile)
    dataset_dir = result_dir / "dataset"

    graph = generate_graph(SCALES[scale], seed=seed)
    manifest = write_dataset(graph, dataset_dir)
    environment = _environment(
        databases, scale, seed, warmup, iterations, profile, execution_mode, keep_db_artifacts, threads
    )
    environment["dataset_manifest"] = manifest
    if baseline_summary is not None:
        environment["baseline_summary"] = str(baseline_summary)
        environment["max_regression_ratio"] = max_regression_ratio

    raw_path = result_dir / "raw.jsonl"
    errors_path = result_dir / "errors.jsonl"
    failure_count = 0
    with raw_path.open("w") as raw_handle, errors_path.open("w") as errors_handle:
        _write_jsonl(raw_handle, environment)
        for database in databases:
            try:
                adapter = _adapter_for(database, dataset_dir, scale, profile, threads)
            except Exception as exc:
                failure = FailureEvent(
                    database=database,
                    workload="adapter_setup",
                    scale=scale,
                    status="failed",
                    error=str(exc),
                )
                _write_failure(raw_handle, errors_handle, failure)
                failure_count += 1
                continue

            results = None
            try:
                results = adapter.run_all(warmup=warmup, iterations=iterations, execution_mode=execution_mode)
                _write_profile_events(result_dir, getattr(adapter, "profile_events", []))
            except Exception as exc:
                failure = FailureEvent(
                    database=database,
                    workload="run_all",
                    scale=scale,
                    status="failed",
                    error=str(exc),
                )
                _write_failure(raw_handle, errors_handle, failure)
                failure_count += 1
            finally:
                if not keep_db_artifacts:
                    try:
                        removed = _cleanup_adapter_artifacts(adapter)
                    except Exception as exc:
                        failure = FailureEvent(
                            database=database,
                            workload="artifact_cleanup",
                            scale=scale,
                            status="failed",
                            error=str(exc),
                        )
                        _write_failure(raw_handle, errors_handle, failure)
                        failure_count += 1
                    else:
                        if removed:
                            _write_jsonl(
                                raw_handle,
                                {
                                    "event": "artifact_cleanup",
                                    "database": database,
                                    "scale": scale,
                                    "removed": removed,
                                },
                            )
            if results is None:
                continue

            for result in results:
                if result.status != "ok":
                    failure = FailureEvent(
                        database=result.database,
                        workload=result.workload,
                        scale=result.scale,
                        status=result.status,
                        error=result.error,
                        equivalent_mode=result.equivalent_mode,
                    )
                    _write_failure(raw_handle, errors_handle, failure)
                    if result.status == "failed":
                        failure_count += 1
                    continue
                for sample in result.samples:
                    _write_jsonl(raw_handle, sample.to_event())

    profile_path = result_dir / "zyx_profiles.jsonl"
    outputs = write_summary_outputs(
        raw_path,
        result_dir,
        environment,
        profile_path if profile_path.exists() else None,
        baseline_summary,
        max_regression_ratio,
    )
    quality_report = json.loads(outputs["quality"].read_text())
    quality_failure_count = int(quality_report.get("failure_count", 0))
    (result_dir / "run_status.json").write_text(
        json.dumps(
            {"failure_count": failure_count, "quality_failure_count": quality_failure_count},
            indent=2,
            sort_keys=True,
        )
        + "\n"
    )
    latest_path = output_root / "latest.txt"
    latest_path.write_text(result_dir.name + "\n")
    return result_dir


def main() -> int:
    parser = argparse.ArgumentParser(description="Run graph database comparison benchmarks")
    parser.add_argument("--database", action="append", choices=DATABASE_CHOICES, dest="databases")
    parser.add_argument("--scale", choices=sorted(SCALES), default="smoke")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--profile", choices=sorted(PROFILE_WORKLOADS), default=DEFAULT_PROFILE)
    parser.add_argument("--warmup", type=int, default=DEFAULT_WARMUP)
    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS)
    parser.add_argument("--execution-mode", choices=EXECUTION_MODES, default=WARM_EXECUTION_MODE)
    parser.add_argument("--baseline-summary", type=Path)
    parser.add_argument("--max-regression-ratio", type=float, default=1.10)
    parser.add_argument("--keep-db-artifacts", action="store_true")
    parser.add_argument("--threads", type=int)
    args = parser.parse_args()

    databases = args.databases if args.databases else DEFAULT_DATABASES
    result_dir = run_benchmark(
        databases=databases,
        scale=args.scale,
        seed=args.seed,
        output_root=args.output_root,
        warmup=args.warmup,
        iterations=args.iterations,
        profile=args.profile,
        execution_mode=args.execution_mode,
        baseline_summary=args.baseline_summary,
        max_regression_ratio=args.max_regression_ratio,
        keep_db_artifacts=args.keep_db_artifacts,
        threads=args.threads,
    )
    print(result_dir)
    status_path = result_dir / "run_status.json"
    status = json.loads(status_path.read_text())
    failure_count = status.get("failure_count", 0)
    quality_failure_count = status.get("quality_failure_count", 0)
    return 1 if failure_count or quality_failure_count else 0


if __name__ == "__main__":
    raise SystemExit(main())
