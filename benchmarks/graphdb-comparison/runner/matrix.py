from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path

from dataset.generate import SCALES
from runner.adapters.base import DEFAULT_PROFILE, EXECUTION_MODES, PROFILE_WORKLOADS
from runner.run import DATABASE_CHOICES, DEFAULT_DATABASES, DEFAULT_ITERATIONS, DEFAULT_OUTPUT_ROOT, DEFAULT_WARMUP, run_benchmark

DEFAULT_MATRIX_SCALES = ("small", "medium")
DEFAULT_MATRIX_EXECUTION_MODES = ("cold-ish", "opened", "warm")


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")


def run_matrix(
    databases: list[str],
    scales: list[str],
    execution_modes: list[str],
    seed: int,
    output_root: Path,
    warmup: int,
    iterations: int,
    profile: str = DEFAULT_PROFILE,
    baseline_summary: Path | None = None,
    max_regression_ratio: float = 1.10,
    keep_db_artifacts: bool = False,
    threads: int | None = None,
) -> Path:
    if not databases:
        raise ValueError("at least one database is required")
    if not scales:
        raise ValueError("at least one scale is required")
    if not execution_modes:
        raise ValueError("at least one execution mode is required")
    for scale in scales:
        if scale not in SCALES:
            raise ValueError(f"unsupported scale: {scale}")
    for execution_mode in execution_modes:
        if execution_mode not in EXECUTION_MODES:
            raise ValueError(f"unsupported execution mode: {execution_mode}")
    if profile not in PROFILE_WORKLOADS:
        raise ValueError(f"unsupported profile: {profile}")
    if threads is not None and threads < 0:
        raise ValueError("threads must be >= 0")

    output_root.mkdir(parents=True, exist_ok=True)
    runs: list[dict[str, object]] = []
    for scale in scales:
        for execution_mode in execution_modes:
            result_dir = run_benchmark(
                databases=databases,
                scale=scale,
                seed=seed,
                output_root=output_root,
                warmup=warmup,
                iterations=iterations,
                profile=profile,
                execution_mode=execution_mode,
                baseline_summary=baseline_summary,
                max_regression_ratio=max_regression_ratio,
                keep_db_artifacts=keep_db_artifacts,
                threads=threads,
            )
            runs.append(
                {
                    "scale": scale,
                    "execution_mode": execution_mode,
                    "result_dir": str(result_dir),
                    "summary": str(result_dir / "summary.csv"),
                    "comparison": str(result_dir / "comparison.csv"),
                    "quality_gates": str(result_dir / "quality_gates.json"),
                }
            )

    manifest = {
        "benchmark_matrix_schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "databases": databases,
        "scales": scales,
        "execution_modes": execution_modes,
        "seed": seed,
        "warmup": warmup,
        "iterations": iterations,
        "profile": profile,
        "threads": threads,
        "keep_db_artifacts": keep_db_artifacts,
        "runs": runs,
    }
    if baseline_summary is not None:
        manifest["baseline_summary"] = str(baseline_summary)
        manifest["max_regression_ratio"] = max_regression_ratio

    manifest_path = output_root / f"{_utc_timestamp()}-matrix.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    (output_root / "latest-matrix.txt").write_text(manifest_path.name + "\n")
    return manifest_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the graph database comparison benchmark matrix")
    parser.add_argument("--database", action="append", choices=DATABASE_CHOICES, dest="databases")
    parser.add_argument("--scale", action="append", choices=sorted(SCALES), dest="scales")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--profile", choices=sorted(PROFILE_WORKLOADS), default=DEFAULT_PROFILE)
    parser.add_argument("--execution-mode", action="append", choices=EXECUTION_MODES, dest="execution_modes")
    parser.add_argument("--warmup", type=int, default=DEFAULT_WARMUP)
    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS)
    parser.add_argument("--baseline-summary", type=Path)
    parser.add_argument("--max-regression-ratio", type=float, default=1.10)
    parser.add_argument("--keep-db-artifacts", action="store_true")
    parser.add_argument("--threads", type=int)
    args = parser.parse_args()

    manifest_path = run_matrix(
        databases=args.databases if args.databases else list(DEFAULT_DATABASES),
        scales=args.scales if args.scales else list(DEFAULT_MATRIX_SCALES),
        execution_modes=args.execution_modes if args.execution_modes else list(DEFAULT_MATRIX_EXECUTION_MODES),
        seed=args.seed,
        output_root=args.output_root,
        warmup=args.warmup,
        iterations=args.iterations,
        profile=args.profile,
        baseline_summary=args.baseline_summary,
        max_regression_ratio=args.max_regression_ratio,
        keep_db_artifacts=args.keep_db_artifacts,
        threads=args.threads,
    )
    print(manifest_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
