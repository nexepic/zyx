#!/usr/bin/env python3
"""Run GoogleTest shards in isolated working directories on POSIX hosts."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from typing import Sequence


PROCESS_REAP_TIMEOUT_SECONDS = 5.0


def process_tree_is_active(process: subprocess.Popen[bytes]) -> bool:
    """Return whether a shard root or one of its POSIX group members remains."""
    if os.name == "nt":
        return process.poll() is None
    try:
        # Each shard starts a new session, so its PID is also its process-group
        # ID even after the shard root itself exits.
        os.killpg(process.pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def terminate_process_tree(process: subprocess.Popen[bytes], *, force: bool) -> None:
    """Stop one test shard and every descendant it created."""
    if os.name == "nt":
        if process.poll() is not None:
            return
        command = ["taskkill", "/PID", str(process.pid), "/T"]
        if force:
            command.append("/F")
        try:
            # Cleanup itself must be bounded: a stuck taskkill must not turn a
            # timed-out test run into a stuck test runner.
            subprocess.run(
                command,
                check=False,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=PROCESS_REAP_TIMEOUT_SECONDS,
            )
        except (OSError, subprocess.TimeoutExpired):
            process.kill() if force else process.terminate()
        return
    try:
        os.killpg(process.pid, signal.SIGKILL if force else signal.SIGTERM)
    except ProcessLookupError:
        pass
    except PermissionError:
        # The supervisor must still report the original timeout/interrupt when
        # an external sandbox owns the process group and rejects the signal.
        # Finalization retains that shard workspace for later diagnostics.
        pass


def terminate_and_reap_processes(
    processes: Sequence[tuple[int, subprocess.Popen[bytes], Path, Path]], *, reason: str
) -> list[tuple[int, int]]:
    """Bound cleanup so a stuck shard cannot indefinitely block the runner."""
    active = [(index, process) for index, process, _, _ in processes if process_tree_is_active(process)]
    if not active:
        return []

    for _, process in active:
        terminate_process_tree(process, force=False)

    deadline = time.monotonic() + PROCESS_REAP_TIMEOUT_SECONDS
    while active and time.monotonic() < deadline:
        active = [(index, process) for index, process in active if process_tree_is_active(process)]
        if active:
            time.sleep(0.1)

    for _, process in active:
        terminate_process_tree(process, force=True)

    deadline = time.monotonic() + PROCESS_REAP_TIMEOUT_SECONDS
    while active and time.monotonic() < deadline:
        active = [(index, process) for index, process in active if process_tree_is_active(process)]
        if active:
            time.sleep(0.1)

    if active:
        details = ", ".join(f"shard {index} (PID {process.pid})" for index, process in active)
        print(
            f"Test {details} remained active after {reason}; it may be in an "
            "uninterruptible kernel I/O wait and was left for the OS to reap.",
            file=sys.stderr,
        )
    return [(index, process.pid) for index, process in active]


def make_isolated_workdir(source_root: Path, shard_root: Path, index: int) -> tuple[Path, Path]:
    """Provide source-relative fixtures and a private system-temp replacement per shard."""
    workdir = shard_root / f"shard-{index}"
    workdir.mkdir()
    for entry in source_root.iterdir():
        target = workdir / entry.name
        try:
            target.symlink_to(entry, target_is_directory=entry.is_dir())
        except OSError as error:
            raise RuntimeError(f"unable to prepare isolated shard workspace for {entry.name}: {error}") from error
    temporary_directory = workdir / ".tmp"
    temporary_directory.mkdir()
    return workdir, temporary_directory


def run_shards(command: Sequence[str], jobs: int, timeout_seconds: float, failure_log_dir: Path | None) -> int:
    if not command:
        raise ValueError("a test command is required")
    if jobs <= 0:
        raise ValueError("jobs must be greater than zero")
    if jobs == 1:
        from run_test_process import run

        return run(command, timeout_seconds)
    if os.name == "nt":
        raise RuntimeError("parallel test shards require POSIX working-directory isolation; use ZYX_TEST_SHARDS=1 on Windows")

    source_root = Path.cwd().resolve()
    shard_root = Path(tempfile.mkdtemp(prefix="zyx-test-shards-"))
    processes: list[tuple[int, subprocess.Popen[bytes], Path, Path]] = []
    interrupted = False
    exit_code = 0

    def handle_signal(signum: int, _frame: object) -> None:
        nonlocal interrupted
        interrupted = True
        print(f"\nTest shards interrupted by signal {signum}; scheduling descendant cleanup...", file=sys.stderr)

    previous_handlers = {signum: signal.signal(signum, handle_signal) for signum in (signal.SIGINT, signal.SIGTERM)}
    try:
        for index in range(jobs):
            workdir, temporary_directory = make_isolated_workdir(source_root, shard_root, index)
            log_path = shard_root / f"shard-{index}.log"
            environment = os.environ.copy()
            environment["GTEST_TOTAL_SHARDS"] = str(jobs)
            environment["GTEST_SHARD_INDEX"] = str(index)
            # Fixtures use the platform temporary-directory API for databases and
            # test artifacts. A private directory prevents independently
            # sharded processes from deleting or reopening each other's artifacts.
            environment["TMPDIR"] = str(temporary_directory)
            environment["TMP"] = str(temporary_directory)
            environment["TEMP"] = str(temporary_directory)
            log_file = log_path.open("wb")
            process = subprocess.Popen(
                list(command),
                cwd=workdir,
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            log_file.close()
            processes.append((index, process, log_path, workdir))

        print(f"Running {jobs} isolated GoogleTest shards (timeout: {timeout_seconds:.0f}s per shard)...")
        deadline = time.monotonic() + timeout_seconds
        while any(process.poll() is None for _, process, _, _ in processes):
            if interrupted:
                terminate_and_reap_processes(processes, reason="an interrupt")
                exit_code = 130
                break
            if time.monotonic() >= deadline:
                print(f"Test shards exceeded {timeout_seconds:.0f}s; terminating process trees...", file=sys.stderr)
                terminate_and_reap_processes(processes, reason="the test timeout")
                exit_code = 124
                break
            time.sleep(0.1)

        if exit_code == 0:
            failed = [(index, process, log_path) for index, process, log_path, _ in processes if process.returncode != 0]
            if failed:
                if failure_log_dir is not None:
                    failure_log_dir.mkdir(parents=True, exist_ok=True)
                for index, process, log_path in failed:
                    print(f"\n--- Test shard {index} failed with exit code {process.returncode} ---", file=sys.stderr)
                    try:
                        contents = log_path.read_text(errors="replace")
                        print(contents, file=sys.stderr)
                        if failure_log_dir is not None:
                            (failure_log_dir / f"shard-{index}.log").write_text(contents)
                    except OSError as error:
                        print(f"Unable to read shard log {log_path}: {error}", file=sys.stderr)
                if failure_log_dir is not None:
                    print(f"Failed shard logs retained at: {failure_log_dir.resolve()}", file=sys.stderr)
                exit_code = 1
            else:
                print(f"All {jobs} isolated GoogleTest shards passed.")
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
        remaining = terminate_and_reap_processes(processes, reason="runner finalization")
        if remaining and exit_code == 0:
            exit_code = 125
        if all(not process_tree_is_active(process) for _, process, _, _ in processes):
            shutil.rmtree(shard_root, ignore_errors=True)
        else:
            # An uninterruptible kernel I/O wait cannot be reaped safely from
            # user space. Preserve its workspace so it does not lose fixtures.
            print(f"Retaining active shard workspace for diagnostics: {shard_root}", file=sys.stderr)
    return exit_code


def write_result(
    path: Path,
    exit_code: int,
    jobs: int,
    *,
    run_id: str | None,
    started_at: float,
    command: Sequence[str],
) -> None:
    """Publish an attributable result record for non-interactive CI harnesses."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    completed_at = time.time()
    temporary.write_text(
        json.dumps(
            {
                "command": list(command),
                "completed_at_unix_seconds": completed_at,
                "exit_code": exit_code,
                "jobs": jobs,
                "launcher_pid": os.getpid(),
                "run_id": run_id,
                "started_at_unix_seconds": started_at,
                "working_directory": str(Path.cwd().resolve()),
            },
            sort_keys=True,
        )
        + "\n"
    )
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=1, help="number of isolated GoogleTest shards")
    parser.add_argument("--timeout", type=float, default=1800.0, help="maximum wall time for the complete shard run")
    parser.add_argument("--result-file", type=Path, help="write the final exit code as JSON")
    parser.add_argument("--run-id", help="opaque caller-generated identifier recorded with the result")
    parser.add_argument("--failure-log-dir", type=Path, help="copy failed shard logs to this directory")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="test executable and arguments")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    started_at = time.time()
    try:
        exit_code = run_shards(command, args.jobs, args.timeout, args.failure_log_dir)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"Unable to run test shards: {error}", file=sys.stderr)
        exit_code = 127
    if args.result_file is not None:
        write_result(
            args.result_file,
            exit_code,
            args.jobs,
            run_id=args.run_id,
            started_at=started_at,
            command=command,
        )
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
