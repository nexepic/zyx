from __future__ import annotations

import json
import hashlib
import math
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any

from runner.adapters.base import (
    BenchmarkAdapter,
    DEFAULT_PROFILE,
    EXECUTION_MODES,
    PROFILE_WORKLOADS,
    WorkloadResult,
)
from runner.models import ProfileEvent, Sample


class ZyxAdapter(BenchmarkAdapter):
    def __init__(
        self,
        database: str,
        dataset_dir: Path,
        scale: str,
        profile: str = DEFAULT_PROFILE,
        threads: int | None = None,
    ):
        super().__init__(database, dataset_dir, scale, profile, threads)
        self.binary = Path(os.environ.get("ZYX_COMPARE_BENCH", "/usr/local/bin/zyx-compare-bench"))
        self.db_path = dataset_dir.parent / "zyx.db"
        self.timeout_seconds = float(os.environ.get("ZYX_COMPARE_TIMEOUT_SECONDS", "600"))
        profile_setting = os.environ.get("ZYX_COMPARE_EMIT_PROFILE", "1")
        if profile_setting not in ("0", "1"):
            raise ValueError("ZYX_COMPARE_EMIT_PROFILE must be 0 or 1")
        self.emit_profile = profile_setting == "1"
        self.profile_events: list[ProfileEvent] = []
        self.result_events: list[dict[str, Any]] = []
        self._prepared_checked = False
        self._prepared_source: Path | None = None

    def run_all(self, warmup: int, iterations: int, execution_mode: str = "warm") -> list[WorkloadResult]:
        self.profile_events = []
        self.result_events = []
        self._prepared_checked = False
        self._prepared_source = None

        if warmup < 0:
            return [self._failed_result("run_all", "warmup must be >= 0")]
        if iterations <= 0:
            return [self._failed_result("run_all", "iterations must be > 0")]
        if execution_mode not in EXECUTION_MODES:
            return [self._failed_result("run_all", f"execution mode must be one of: {', '.join(EXECUTION_MODES)}")]

        configured_profile_process = os.environ.get("ZYX_COMPARE_PROFILE_PROCESS", "0")
        if configured_profile_process not in ("0", "1"):
            raise ValueError("ZYX_COMPARE_PROFILE_PROCESS must be 0 or 1")
        profile_process = configured_profile_process == "1"
        self._profile_completed = None
        workloads = PROFILE_WORKLOADS[self.profile]
        results: list[WorkloadResult] = []
        for workload in workloads:
            result, profile_events = self._run_workload(
                workload, warmup=warmup, iterations=iterations, execution_mode=execution_mode, profile_process=profile_process
            )
            results.append(result)
            if result.status != "ok":
                break
            self.profile_events.extend(profile_events)
        return results

    @staticmethod
    def _digest(path: Path) -> str:
        value = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                value.update(chunk)
        return value.hexdigest()

    def prepared_database(self) -> Path | None:
        if self._prepared_checked:
            return self._prepared_source
        configured = os.environ.get("ZYX_COMPARE_PREPARED_DB")
        if configured is None:
            self._prepared_checked = True
            return None
        if self.profile not in ("scan", "indexed", "retrieval"):
            raise ValueError("prepared input is limited to read profiles")
        if not configured:
            raise ValueError("prepared database path must be nonempty")
        source = Path(configured)
        manifest = json.loads(Path(str(source) + ".json").read_text())
        csv_hashes = {path.name: self._digest(path) for path in sorted(self.dataset_dir.glob("*.csv"))}
        if not csv_hashes:
            raise ValueError("prepared input requires the exact dataset CSV files")
        expected = {"consumer_binary_sha256": self._digest(self.binary),
                    "database_sha256": self._digest(source), "dataset_sha256": csv_hashes,
                    "scale": self.scale}
        if any(manifest.get(key) != value for key, value in expected.items()):
            raise ValueError("prepared database provenance does not match binary, dataset or scale")
        (self.dataset_dir.parent / "prepared-input.json").write_text(
            json.dumps({"source": str(source), "manifest": manifest}, indent=2) + "\n")
        self._prepared_source = source
        self._prepared_checked = True
        return source

    def _run_workload(
        self,
        workload: str,
        warmup: int,
        iterations: int,
        execution_mode: str,
        profile_process: bool = False,
    ) -> tuple[WorkloadResult, list[ProfileEvent]]:
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
            "--execution-mode",
            execution_mode,
            "--workloads" if profile_process else "--workload",
            ",".join(PROFILE_WORKLOADS[self.profile]) if profile_process else workload,
        ]
        prepared = self.prepared_database()
        if prepared is not None:
            if workload == "load_nodes_edges":
                raise ValueError("prepared input cannot measure database import")
            command.extend(["--prepared-db", str(prepared)])
        if self.threads is not None:
            command.extend(["--threads", str(self.threads)])
        configured_batch = os.environ.get("ZYX_COMPARE_LOAD_BATCH_ROWS")
        if configured_batch is not None:
            batch_rows = int(configured_batch)
            if batch_rows <= 0:
                raise ValueError("benchmark preparation batch rows must be positive")
            command.extend(["--load-batch-rows", str(batch_rows)])
        if self.emit_profile:
            command.append("--emit-profile")
        if self.emit_result_rows:
            command.append("--emit-result-rows")

        try:
            # COSEC: an argument vector bypasses shell parsing, so paths and workload names cannot inject commands.
            if profile_process and self._profile_completed is not None:
                completed = self._profile_completed
            else:
                completed = subprocess.run(
                    command, check=False, capture_output=True, text=True, timeout=self.timeout_seconds
                )
                if profile_process:
                    self._profile_completed = completed
        except subprocess.TimeoutExpired:
            return self._failed_result(
                workload, f"subprocess for {workload} timed out after {self.timeout_seconds:g}s"
            ), []
        except OSError as exc:
            return self._failed_result(workload, str(exc)), []

        result = WorkloadResult("zyx", workload, self.scale, "ok", samples=[], equivalent_mode="api")
        profile_events: list[ProfileEvent] = []
        reported_errors: list[str] = []
        malformed: list[str] = []
        for stream_name, output in (("stdout", completed.stdout), ("stderr", completed.stderr)):
            for line_number, raw_line in enumerate(output.splitlines(), start=1):
                line = raw_line.strip()
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
                if profile_process and event.get("workload") != workload and event.get("workload") in PROFILE_WORKLOADS[self.profile]:
                    continue
                self._consume_event(
                    event,
                    workload,
                    result,
                    profile_events,
                    reported_errors,
                    malformed,
                    stream_name,
                    line_number,
                )

        if malformed:
            return self._failed_result(workload, "; ".join(malformed)), []
        if reported_errors:
            return self._failed_result(workload, "; ".join(reported_errors)), []
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            return self._failed_result(workload, detail[:2000] or f"subprocess exited {completed.returncode}"), []

        incomplete = self._incomplete_samples(result.samples, workload, iterations)
        if incomplete:
            return self._failed_result(workload, incomplete), []
        return result, profile_events

    def cleanup_artifacts(self) -> list[Path]:
        removed: list[Path] = []
        if not self.db_path.parent.exists():
            return removed

        # Native ZYX owns one database file. Only remove files created by this
        # adapter, not retired WAL-style sidecars sharing the same prefix.
        paths = [(self.db_path, False)] + [
            (path, True)
            for path in sorted(self.db_path.parent.glob(f"{self.db_path.name}.load-iteration-*"))
        ]
        paths += [
            (path, True)
            for path in sorted(self.db_path.parent.glob(f"{self.db_path.name}.workload-*"))
        ]
        paths += [
            (path, True)
            for path in sorted(self.db_path.parent.glob(f"{self.db_path.name}.coldish-*"))
        ]
        paths += [(self.db_path.parent / f"{self.db_path.name}.benchmark-baseline", False)]
        for path, allow_directory in paths:
            if path.is_symlink() or path.is_file():
                path.unlink()
            elif allow_directory and path.is_dir():
                shutil.rmtree(path)
            else:
                continue
            removed.append(path)
        return removed

    @staticmethod
    def _incomplete_samples(samples: list[Sample], workload: str, iterations: int) -> str:
        expected_iterations = set(range(iterations))
        sample_iterations = {sample.iteration for sample in samples}
        if len(samples) == iterations and sample_iterations == expected_iterations:
            return ""
        missing = sorted(expected_iterations - sample_iterations)
        unexpected = sorted(sample_iterations - expected_iterations)
        duplicate_count = len(samples) - len(sample_iterations)
        details = f"{workload} expected {iterations} samples"
        if missing:
            details += f"; missing iterations {missing}"
        if unexpected:
            details += f"; unexpected iterations {unexpected}"
        if duplicate_count > 0:
            details += f"; duplicate iterations {duplicate_count}"
        return details

    def _consume_event(
        self,
        event: dict[str, Any],
        expected_workload: str,
        result: WorkloadResult,
        profile_events: list[ProfileEvent],
        reported_errors: list[str],
        malformed: list[str],
        stream_name: str,
        line_number: int,
    ) -> None:
        event_type = event.get("event")
        workload = event.get("workload")
        location = f"{stream_name}:{line_number}"
        if workload != expected_workload:
            malformed.append(
                f"{location}: unexpected workload for isolated subprocess {expected_workload!r}: {workload!r}"
            )
            return
        if event_type == "sample":
            try:
                sample = Sample(
                    database=str(event["database"]),
                    workload=str(workload),
                    scale=str(event["scale"]),
                    iteration=int(event["iteration"]),
                    latency_ms=float(event["latency_ms"]),
                    status=str(event["status"]),
                    equivalent_mode=str(event["equivalent_mode"]),
                )
            except (KeyError, TypeError, ValueError) as exc:
                malformed.append(f"{location}: invalid sample event: {exc}")
                return
            if (
                sample.database != "zyx"
                or sample.scale != self.scale
                or sample.status != "ok"
                or sample.equivalent_mode != "api"
                or not math.isfinite(sample.latency_ms)
                or sample.latency_ms < 0.0
            ):
                malformed.append(f"{location}: sample metadata or latency violates the ZYX benchmark contract")
                return
            result.samples.append(sample)
        elif event_type == "result":
            iteration = event.get("iteration")
            value = event.get("value")
            if type(iteration) is not int or iteration < 0 or type(value) is not int or value < 0:
                malformed.append(f"{location}: invalid result event")
                return
            if "rows" in event and (not isinstance(event["rows"], list)
                                    or any(not isinstance(row, list) for row in event["rows"])):
                malformed.append(f"{location}: invalid result rows")
                return
            self.result_events.append(event.copy())
        elif event_type == "profile":
            try:
                profile_event = ProfileEvent(
                    database=str(event["database"]),
                    workload=str(workload),
                    scale=str(event["scale"]),
                    profile=str(event["profile"]),
                    iteration=int(event["iteration"]),
                    phase=str(event["phase"]),
                    total_time_ms=float(event["total_time_ms"]),
                    calls=int(event["calls"]),
                    equivalent_mode=str(event.get("equivalent_mode", "api")),
                    value_total=int(event["value_total"]) if "value_total" in event else None,
                    value_calls=int(event["value_calls"]) if "value_calls" in event else None,
                    value_avg=float(event["value_avg"]) if "value_avg" in event else None,
                )
            except (KeyError, TypeError, ValueError) as exc:
                malformed.append(f"{location}: invalid profile event: {exc}")
                return
            if (
                profile_event.database != "zyx"
                or profile_event.scale != self.scale
                or profile_event.profile != self.profile
                or profile_event.equivalent_mode != "api"
                or profile_event.iteration < 0
                or not math.isfinite(profile_event.total_time_ms)
                or profile_event.total_time_ms < 0.0
                or profile_event.calls < 0
            ):
                malformed.append(f"{location}: profile metadata or counters violate the ZYX benchmark contract")
                return
            profile_events.append(profile_event)
        elif event_type in {"error", "failure"}:
            reported_errors.append(str(event.get("error", "subprocess reported failure")))
        else:
            malformed.append(f"{location}: unknown event type: {event_type!r}")

    def _failed_result(self, workload: str, error: str) -> WorkloadResult:
        return WorkloadResult("zyx", workload, self.scale, "failed", error=error, equivalent_mode="api")
