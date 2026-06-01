from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any

from runner.adapters.base import BenchmarkAdapter, DEFAULT_PROFILE, PROFILE_WORKLOADS, WorkloadResult
from runner.models import ProfileEvent, Sample


class ZyxAdapter(BenchmarkAdapter):
    def __init__(
        self,
        database: str,
        dataset_dir: Path,
        scale: str,
        profile: str = DEFAULT_PROFILE,
    ):
        super().__init__(database, dataset_dir, scale, profile)
        self.binary = Path(os.environ.get("ZYX_COMPARE_BENCH", "/usr/local/bin/zyx-compare-bench"))
        self.db_path = dataset_dir.parent / "zyx.db"
        self.timeout_seconds = float(os.environ.get("ZYX_COMPARE_TIMEOUT_SECONDS", "600"))
        self.profile_events: list[ProfileEvent] = []

    def run_all(self, warmup: int, iterations: int) -> list[WorkloadResult]:
        self.profile_events = []

        if warmup < 0:
            return [self._failed_result("run_all", "warmup must be >= 0")]
        if iterations <= 0:
            return [self._failed_result("run_all", "iterations must be > 0")]

        command = [
            str(self.binary),
            "--dataset",
            str(self.dataset_dir),
            "--db-path",
            str(self.db_path),
            "--scale",
            self.scale,
            "--profile",
            self.profile,
            "--warmup",
            str(warmup),
            "--iterations",
            str(iterations),
            "--emit-profile",
        ]
        try:
            completed = subprocess.run(
                command, check=False, capture_output=True, text=True, timeout=self.timeout_seconds
            )
        except subprocess.TimeoutExpired:
            return [self._failed_result("run_all", f"subprocess timed out after {self.timeout_seconds:g}s")]
        except OSError as exc:
            return [self._failed_result("run_all", str(exc))]

        workloads = PROFILE_WORKLOADS[self.profile]
        results = {
            workload: WorkloadResult("zyx", workload, self.scale, "ok", samples=[], equivalent_mode="api")
            for workload in workloads
        }
        errors: list[WorkloadResult] = []
        malformed: list[str] = []
        for stream_name, output in [("stdout", completed.stdout), ("stderr", completed.stderr)]:
            for line_number, line in enumerate(output.splitlines(), start=1):
                line = line.strip()
                if not line:
                    continue
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    malformed.append(f"{stream_name}:{line_number}: malformed JSONL: {line[:200]}")
                    continue
                if not isinstance(event, dict):
                    malformed.append(f"{stream_name}:{line_number}: JSONL event is not an object")
                    continue
                self._consume_event(event, results, errors, malformed, stream_name, line_number)

        if completed.returncode != 0 and not errors:
            detail = "; ".join(malformed) if malformed else (completed.stderr.strip() or completed.stdout.strip())
            errors.append(self._failed_result("run_all", detail or f"subprocess exited {completed.returncode}"))
        elif malformed:
            errors.append(self._failed_result("subprocess_output", "; ".join(malformed)))

        ok_results = [results[workload] for workload in workloads if results[workload].samples]
        if errors:
            return ok_results + errors

        incomplete = self._incomplete_workloads(results, workloads, iterations)
        if incomplete:
            return ok_results + [self._failed_result("run_all", "missing/incomplete samples for: " + "; ".join(incomplete))]
        return [results[workload] for workload in workloads]

    def _incomplete_workloads(
        self, results: dict[str, WorkloadResult], workloads: list[str], iterations: int
    ) -> list[str]:
        expected_iterations = set(range(iterations))
        incomplete: list[str] = []
        for workload in workloads:
            samples = results[workload].samples
            sample_iterations = {sample.iteration for sample in samples}
            if len(samples) != iterations or sample_iterations != expected_iterations:
                missing = sorted(expected_iterations - sample_iterations)
                duplicate_count = len(samples) - len(sample_iterations)
                details = f"{workload} expected {iterations} samples"
                if missing:
                    details += f" missing samples for iterations {missing}"
                if duplicate_count > 0:
                    details += f" duplicate iterations {duplicate_count}"
                incomplete.append(details)
        return incomplete

    def _consume_event(
        self,
        event: dict[str, Any],
        results: dict[str, WorkloadResult],
        errors: list[WorkloadResult],
        malformed: list[str],
        stream_name: str,
        line_number: int,
    ) -> None:
        event_type = event.get("event")
        workload = str(event.get("workload", "run_all"))
        if event_type == "sample":
            try:
                sample = Sample(
                    database=str(event.get("database", "zyx")),
                    workload=workload,
                    scale=str(event.get("scale", self.scale)),
                    iteration=int(event.get("iteration", 0)),
                    latency_ms=float(event.get("latency_ms", 0.0)),
                    status=str(event.get("status", "ok")),
                    equivalent_mode=str(event.get("equivalent_mode", "api")),
                )
            except (TypeError, ValueError) as exc:
                malformed.append(f"{stream_name}:{line_number}: invalid sample event: {exc}")
                return
            if workload not in results:
                malformed.append(
                    f"{stream_name}:{line_number}: unexpected workload for profile {self.profile!r}: {workload}"
                )
                return
            results[workload].samples.append(sample)
        elif event_type == "profile":
            try:
                profile_event = ProfileEvent(
                    database=str(event["database"]),
                    workload=workload,
                    scale=str(event["scale"]),
                    profile=str(event["profile"]),
                    iteration=int(event["iteration"]),
                    phase=str(event["phase"]),
                    total_time_ms=float(event["total_time_ms"]),
                    calls=int(event["calls"]),
                    equivalent_mode=str(event.get("equivalent_mode", "api")),
                )
            except (KeyError, TypeError, ValueError) as exc:
                malformed.append(f"{stream_name}:{line_number}: invalid profile event: {exc}")
                return
            if workload not in results:
                malformed.append(
                    f"{stream_name}:{line_number}: unexpected workload for profile {self.profile!r}: {workload}"
                )
                return
            self.profile_events.append(profile_event)
        elif event_type in {"error", "failure"}:
            errors.append(
                WorkloadResult(
                    database=str(event.get("database", "zyx")),
                    workload=workload,
                    scale=str(event.get("scale", self.scale)),
                    status="failed",
                    samples=[],
                    error=str(event.get("error", "subprocess reported failure")),
                    equivalent_mode=str(event.get("equivalent_mode", "api")),
                )
            )
        else:
            malformed.append(f"{stream_name}:{line_number}: unknown event type: {event_type!r}")

    def _failed_result(self, workload: str, error: str) -> WorkloadResult:
        return WorkloadResult("zyx", workload, self.scale, "failed", error=error, equivalent_mode="api")
