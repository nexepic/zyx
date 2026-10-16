"""Bounded read baseline using the existing benchmark adapters.

Run this module in an isolated container, once per independent round. Database
files and CSV inputs live in a temporary directory; only measurements survive.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import platform
import csv
from collections import Counter

from runner.adapters.base import PROFILE_WORKLOADS
from runner.run import run_benchmark
from runner.semantic_oracle import verify_observations


BASELINE_WORKLOADS = {
    "scan": ["label_scan_filter", "all_nodes_property_filter", "label_multi_property_filter",
             "relationship_type_scan", "relationship_property_filter", "one_hop_expand",
             "two_hop_expand", "aggregation_group_by", "aggregation_count_by_group", "topk_property_sort"],
    "indexed": ["point_lookup_indexed", "property_equality_indexed", "property_range_indexed"],
    "retrieval": ["point_node_fetch_by_id", "point_edge_fetch_by_endpoints", "batch_node_fetch_100",
                  "one_hop_fetch_neighbor_ids", "one_hop_fetch_neighbor_records"],
}


def verify_counts(result: Path, workloads: list[str], iterations: int, databases=("zyx", "kuzu")) -> dict:
    """Check scalar/count observations independently of engine execution."""
    dataset = result / "dataset"
    with (dataset / "users.csv").open() as stream:
        users = list(csv.DictReader(stream))
    countries = {row["country"] for row in users}
    with (dataset / "follows.csv").open() as stream:
        follows = list(csv.DictReader(stream))
    first = Counter(row["dst"] for row in follows if row["src"] == "user-000001")
    expected = {
        "label_scan_filter": sum(row["country"] == "CN" for row in users),
        "all_nodes_property_filter": sum(float(row["score"]) >= 900 for row in users),
        "label_multi_property_filter": sum(row["country"] == "CN" and int(row["age"]) >= 30 for row in users),
        "relationship_type_scan": len(follows),
        "relationship_property_filter": sum(int(row["weight"]) == 1 for row in follows),
        "one_hop_expand": sum(first.values()),
        "two_hop_expand": sum(first[row["src"]] for row in follows),
        "aggregation_group_by": len(countries),
        "aggregation_count_by_group": len(countries),
        "topk_property_sort": min(100, len(users)),
        "point_lookup_indexed": sum(row["id"] == "user-000001" for row in users),
        "property_equality_indexed": sum(row["country"] == "CN" for row in users),
        "property_range_indexed": sum(30 <= int(row["age"]) < 40 for row in users),
        "point_node_fetch_by_id": 1,
        "point_edge_fetch_by_endpoints": 1,
        "batch_node_fetch_100": min(100, len(users)),
        "one_hop_fetch_neighbor_ids": sum(first.values()),
        "one_hop_fetch_neighbor_records": sum(first.values()),
    }
    with (dataset / "posts.csv").open() as stream:
        expected["all_nodes_property_filter"] += sum(float(row["score"]) >= 900 for row in csv.DictReader(stream))
    events = [json.loads(line) for line in (result / "raw.jsonl").read_text().splitlines()]
    unsupported = {(event["database"], event["workload"]) for event in events
                   if event.get("status") == "unsupported"}
    failures = []
    for database in databases:
        for workload in workloads:
            if (database, workload) in unsupported:
                continue
            observations = [event for event in events if event.get("event") == "result"
                            and event.get("database") == database and event.get("workload") == workload]
            if len(observations) != iterations or {event["iteration"] for event in observations} != set(range(iterations)):
                failures.append(f"{database}/{workload}: missing or duplicate observations")
            elif any(event["value"] != expected[workload] for event in observations):
                failures.append(f"{database}/{workload}: expected {expected[workload]}, observed {[e['value'] for e in observations]}")
    return {"expected": {name: expected[name] for name in workloads}, "failures": failures,
            "scope": "scalar values and projected row counts; projected values, types, ordering and duplicates need separate semantic tests"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scale", choices=["smoke", "small", "medium"], default="medium")
    parser.add_argument("--round", type=int, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--profile", choices=list(BASELINE_WORKLOADS), action="append")
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--iterations", type=int, default=10)
    parser.add_argument("--require-full-results", action="store_true",
                        help="Require per-iteration aggregate/Top-K rows; count-only observers fail this gate")
    args = parser.parse_args()
    if args.round < 1:
        parser.error("--round must be positive")
    if args.warmup < 0 or args.iterations <= 0:
        parser.error("--warmup must be nonnegative and --iterations must be positive")
    destination = args.output_root / f"{args.scale}-round-{args.round}"
    destination.mkdir(parents=True, exist_ok=False)
    failures = 0
    # Alternate order across independent rounds to expose ordering effects.
    databases = ["zyx", "kuzu"] if args.round % 2 else ["kuzu", "zyx"]
    for profile in args.profile or list(BASELINE_WORKLOADS):
        original = PROFILE_WORKLOADS[profile]
        PROFILE_WORKLOADS[profile] = BASELINE_WORKLOADS[profile]
        try:
            with tempfile.TemporaryDirectory(prefix="zyx-baseline-") as temporary:
                result = run_benchmark(databases, args.scale, 42, Path(temporary), args.warmup,
                                       args.iterations, profile=profile, threads=1,
                                       observe_result_rows=args.require_full_results)
                hashes = {file.name: hashlib.sha256(file.read_bytes()).hexdigest()
                          for file in (result / "dataset").iterdir() if file.is_file()}
                (result / "dataset-hashes.json").write_text(json.dumps(hashes, indent=2) + "\n")
                correctness = verify_counts(result, BASELINE_WORKLOADS[profile], args.iterations)
                (result / "correctness.json").write_text(json.dumps(correctness, indent=2) + "\n")
                failures += len(correctness["failures"])
                if args.require_full_results:
                    with (result / "dataset/users.csv").open() as stream:
                        users = list(csv.DictReader(stream))
                    events = [json.loads(line) for line in (result / "raw.jsonl").read_text().splitlines()]
                    semantics = verify_observations(users, events, databases,
                                                    BASELINE_WORKLOADS[profile], args.iterations)
                    (result / "full-result-correctness.json").write_text(json.dumps(semantics, indent=2) + "\n")
                    failures += len(semantics["failures"])
                if platform.system() == "Linux":
                    import resource
                    resources = {
                        "parent_peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                        "child_peak_rss_kib": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
                        "scope": "cumulative process high-water marks, including load/setup; parent includes Python orchestration, validation and Kuzu, children include ZYX",
                    }
                    for name in ("memory.peak", "memory.max", "cpu.max", "io.stat"):
                        file = Path("/sys/fs/cgroup") / name
                        if file.is_file():
                            resources[name] = file.read_text().strip()
                    (result / "resources.json").write_text(json.dumps(resources, indent=2) + "\n")
                shutil.copytree(result, destination / profile,
                                ignore=shutil.ignore_patterns("dataset", "*.db", "*.db.*"))
                status = json.loads((result / "run_status.json").read_text())
                failures += status["failure_count"] + status["quality_failure_count"]
                print(f"{args.scale} round {args.round} {profile}: {status}", flush=True)
        finally:
            PROFILE_WORKLOADS[profile] = original
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
