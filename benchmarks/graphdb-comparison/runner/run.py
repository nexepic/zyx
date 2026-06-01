from __future__ import annotations

import argparse
import json
import platform
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, TextIO

from dataset.generate import SCALES, generate_graph, write_dataset
from runner.adapters.base import BenchmarkAdapter, DEFAULT_PROFILE, PROFILE_WORKLOADS
from runner.adapters.fake import FakeAdapter
from runner.models import FailureEvent
from runner.summarize import write_summary_outputs

DATABASE_CHOICES = ("fake", "zyx", "neo4j", "memgraph", "kuzu")
DEFAULT_DATABASES = ["zyx", "neo4j", "memgraph", "kuzu"]
DEFAULT_OUTPUT_ROOT = Path("benchmarks/graphdb-comparison/results")
DEFAULT_WARMUP = 20
DEFAULT_ITERATIONS = 100


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


def _environment(
    databases: Iterable[str],
    scale: str,
    seed: int,
    warmup: int,
    iterations: int,
    profile: str,
) -> dict[str, object]:
    return {
        "event": "environment",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "databases": list(databases),
        "scale": scale,
        "profile": profile,
        "seed": seed,
        "warmup": warmup,
        "iterations": iterations,
        "machine": platform.machine(),
        "platform": platform.platform(),
        "python": platform.python_version(),
        "system": platform.system(),
    }


def _adapter_for(database: str, dataset_dir: Path, scale: str, profile: str) -> BenchmarkAdapter:
    if database == "fake":
        return FakeAdapter(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile)
    if database == "kuzu":
        from runner.adapters.kuzu import KuzuAdapter

        return KuzuAdapter(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile)
    if database == "neo4j":
        from runner.adapters.neo4j import Neo4jAdapter

        return Neo4jAdapter(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile)
    if database == "memgraph":
        from runner.adapters.memgraph import MemgraphAdapter

        return MemgraphAdapter(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile)
    if database == "zyx":
        from runner.adapters.zyx import ZyxAdapter

        return ZyxAdapter(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile)
    raise ValueError(f"unsupported database: {database}")


def run_benchmark(
    databases: list[str],
    scale: str,
    seed: int,
    output_root: Path,
    warmup: int,
    iterations: int,
    profile: str = DEFAULT_PROFILE,
) -> Path:
    if scale not in SCALES:
        raise ValueError(f"unsupported scale: {scale}")
    if profile not in PROFILE_WORKLOADS:
        raise ValueError(f"unsupported profile: {profile}")
    if warmup < 0:
        raise ValueError("warmup must be >= 0")
    if iterations <= 0:
        raise ValueError("iterations must be > 0")

    result_dir = _create_result_dir(output_root, databases, scale, profile)
    dataset_dir = result_dir / "dataset"

    graph = generate_graph(SCALES[scale], seed=seed)
    manifest = write_dataset(graph, dataset_dir)
    environment = _environment(databases, scale, seed, warmup, iterations, profile)
    environment["dataset_manifest"] = manifest

    raw_path = result_dir / "raw.jsonl"
    errors_path = result_dir / "errors.jsonl"
    failure_count = 0
    with raw_path.open("w") as raw_handle, errors_path.open("w") as errors_handle:
        _write_jsonl(raw_handle, environment)
        for database in databases:
            try:
                adapter = _adapter_for(database, dataset_dir, scale, profile)
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

            try:
                results = adapter.run_all(warmup=warmup, iterations=iterations)
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
                continue

            for result in results:
                if result.status == "failed":
                    failure = FailureEvent(
                        database=result.database,
                        workload=result.workload,
                        scale=result.scale,
                        status=result.status,
                        error=result.error,
                        equivalent_mode=result.equivalent_mode,
                    )
                    _write_failure(raw_handle, errors_handle, failure)
                    failure_count += 1
                    continue
                for sample in result.samples:
                    _write_jsonl(raw_handle, sample.to_event())

    write_summary_outputs(raw_path, result_dir, environment)
    (result_dir / "run_status.json").write_text(json.dumps({"failure_count": failure_count}, indent=2, sort_keys=True) + "\n")
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
    )
    print(result_dir)
    status_path = result_dir / "run_status.json"
    failure_count = json.loads(status_path.read_text()).get("failure_count", 0)
    return 1 if failure_count else 0


if __name__ == "__main__":
    raise SystemExit(main())
