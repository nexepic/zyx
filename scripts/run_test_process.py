#!/usr/bin/env python3
"""Run one test process with portable timeout and process-tree cleanup."""

from __future__ import annotations

import argparse
import ctypes
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Sequence


PROCESS_REAP_TIMEOUT_SECONDS = 5.0


class _WindowsProcessJob:
    """Keep all Windows test descendants in a job that dies with the runner."""

    _JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
    _JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000

    def __init__(self, process: subprocess.Popen[bytes]) -> None:
        if os.name != "nt":
            raise RuntimeError("Windows process jobs are only available on Windows")

        from ctypes import wintypes

        class _IoCounters(ctypes.Structure):
            _fields_ = [
                ("read_operation_count", ctypes.c_ulonglong),
                ("write_operation_count", ctypes.c_ulonglong),
                ("other_operation_count", ctypes.c_ulonglong),
                ("read_transfer_count", ctypes.c_ulonglong),
                ("write_transfer_count", ctypes.c_ulonglong),
                ("other_transfer_count", ctypes.c_ulonglong),
            ]

        class _BasicLimitInformation(ctypes.Structure):
            _fields_ = [
                ("per_process_user_time_limit", ctypes.c_longlong),
                ("per_job_user_time_limit", ctypes.c_longlong),
                ("limit_flags", wintypes.DWORD),
                ("minimum_working_set_size", ctypes.c_size_t),
                ("maximum_working_set_size", ctypes.c_size_t),
                ("active_process_limit", wintypes.DWORD),
                ("affinity", ctypes.c_size_t),
                ("priority_class", wintypes.DWORD),
                ("scheduling_class", wintypes.DWORD),
            ]

        class _ExtendedLimitInformation(ctypes.Structure):
            _fields_ = [
                ("basic_limit_information", _BasicLimitInformation),
                ("io_info", _IoCounters),
                ("process_memory_limit", ctypes.c_size_t),
                ("job_memory_limit", ctypes.c_size_t),
                ("peak_process_memory_used", ctypes.c_size_t),
                ("peak_job_memory_used", ctypes.c_size_t),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.SetInformationJobObject.argtypes = [
            wintypes.HANDLE,
            wintypes.DWORD,
            ctypes.c_void_p,
            wintypes.DWORD,
        ]
        kernel32.SetInformationJobObject.restype = wintypes.BOOL
        kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL

        handle = kernel32.CreateJobObjectW(None, None)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())

        self._kernel32 = kernel32
        self._handle = handle
        try:
            limits = _ExtendedLimitInformation()
            limits.basic_limit_information.limit_flags = self._JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if not kernel32.SetInformationJobObject(
                handle,
                self._JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                ctypes.byref(limits),
                ctypes.sizeof(limits),
            ):
                raise ctypes.WinError(ctypes.get_last_error())
            if not kernel32.AssignProcessToJobObject(handle, wintypes.HANDLE(int(process._handle))):  # type: ignore[attr-defined]
                raise ctypes.WinError(ctypes.get_last_error())
        except BaseException:
            kernel32.CloseHandle(handle)
            self._handle = None
            raise

    def close(self) -> None:
        """Close the job handle, atomically terminating remaining descendants."""
        if self._handle is not None:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None


def _create_windows_process_job(process: subprocess.Popen[bytes]) -> _WindowsProcessJob | None:
    """Attach the gated test launcher to a kill-on-close job."""
    if os.name != "nt":
        return None
    try:
        return _WindowsProcessJob(process)
    except OSError as error:
        print(
            f"Unable to assign test process PID {process.pid} to a Windows Job Object; "
            f"the test command will not be started: {error}",
            file=sys.stderr,
        )
        return None


def _windows_gate_command(command: Sequence[str]) -> list[str]:
    """Return a launcher that waits for Job Object setup before spawning a test."""
    gate = Path(__file__).with_name("run_test_child.py")
    return [sys.executable, str(gate), "--", *command]


def _release_windows_gate(process: subprocess.Popen[bytes]) -> None:
    """Allow a successfully job-bound launcher to start its test command."""
    if process.stdin is None:
        raise RuntimeError("Windows test launcher has no control pipe")
    try:
        process.stdin.write(b"\n")
        process.stdin.flush()
    finally:
        process.stdin.close()


def _process_group_is_active(process_group_id: int) -> bool:
    """Return whether a POSIX process group still has a member."""
    try:
        os.killpg(process_group_id, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # The group still exists even if a sandbox prevents signalling it.
        return True
    return True


def _terminate_process_tree(
    process: subprocess.Popen[bytes], *, force: bool = False, windows_job: _WindowsProcessJob | None = None
) -> None:
    """Terminate the process and descendants created for this test run."""
    if os.name == "nt":
        if force and windows_job is not None:
            windows_job.close()
            return
        if process.poll() is not None:
            return
        # taskkill /T covers descendants even when a child creates additional
        # worker processes. A Job Object closes the parent-exit orphan gap.
        # /F is reserved for timeout/interrupt cleanup.
        command = ["taskkill", "/PID", str(process.pid), "/T"]
        if force:
            command.append("/F")
        try:
            # Keep cleanup bounded too: a broken taskkill invocation must not
            # turn a test timeout into an indefinitely blocked test runner.
            subprocess.run(command, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            process.kill() if force else process.terminate()
        return

    try:
        # start_new_session=True gives the test its own process group. Do not
        # depend on the group leader still being alive: a test can exit while a
        # child remains in the group.
        os.killpg(process.pid, signal.SIGKILL if force else signal.SIGTERM)
    except ProcessLookupError:
        pass


def _wait_for_exit(
    process: subprocess.Popen[bytes], timeout_seconds: float, windows_job: _WindowsProcessJob | None = None
) -> bool:
    """Wait for the test root and, on POSIX, every member of its process group."""
    deadline = time.monotonic() + timeout_seconds
    while True:
        if process.poll() is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            try:
                process.wait(timeout=min(remaining, 0.1))
            except subprocess.TimeoutExpired:
                continue

        if os.name == "nt" or not _process_group_is_active(process.pid):
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.05)


def _terminate_and_reap(
    process: subprocess.Popen[bytes], *, reason: str, windows_job: _WindowsProcessJob | None = None
) -> bool:
    """Attempt bounded graceful and forced cleanup; report an unreapable child."""
    _terminate_process_tree(process, windows_job=windows_job)
    if _wait_for_exit(process, PROCESS_REAP_TIMEOUT_SECONDS, windows_job):
        return True

    _terminate_process_tree(process, force=True, windows_job=windows_job)
    if _wait_for_exit(process, PROCESS_REAP_TIMEOUT_SECONDS, windows_job):
        return True

    print(
        f"Test process PID {process.pid} remained active after {reason}; "
        "it may be in an uninterruptible kernel I/O wait and was left for the OS to reap.",
        file=sys.stderr,
    )
    return False


def run(command: Sequence[str], timeout_seconds: float) -> int:
    if not command:
        raise ValueError("a test command is required")

    creationflags = 0
    if os.name == "nt":
        creationflags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)

    launcher_command = _windows_gate_command(command) if os.name == "nt" else list(command)
    process = subprocess.Popen(
        launcher_command,
        # The Windows launcher consumes one byte only after it belongs to the
        # kill-on-close Job Object. POSIX tests do not need this handshake.
        stdin=subprocess.PIPE if os.name == "nt" else subprocess.DEVNULL,
        start_new_session=os.name != "nt",
        creationflags=creationflags,
    )
    windows_job = _create_windows_process_job(process)
    if os.name == "nt":
        if windows_job is None:
            _terminate_process_tree(process, force=True)
            try:
                process.wait(timeout=PROCESS_REAP_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                pass
            return 127
        try:
            _release_windows_gate(process)
        except (OSError, RuntimeError, ValueError) as error:
            print(f"Unable to start the Windows test command safely: {error}", file=sys.stderr)
            _terminate_process_tree(process, force=True, windows_job=windows_job)
            return 127
    interrupted = False
    cleanup_attempted = False
    exit_code = 127

    def handle_signal(signum: int, _frame: object) -> None:
        nonlocal interrupted
        interrupted = True
        print(f"\nTest process interrupted by signal {signum}; cleaning up descendants...", file=sys.stderr)
        _terminate_process_tree(process, windows_job=windows_job)

    previous_handlers = {}
    for signum in (signal.SIGINT, signal.SIGTERM):
        previous_handlers[signum] = signal.signal(signum, handle_signal)

    try:
        deadline = time.monotonic() + timeout_seconds
        while process.poll() is None:
            if interrupted:
                cleanup_attempted = True
                _terminate_and_reap(process, reason="an interrupt", windows_job=windows_job)
                exit_code = 130
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                print(
                    f"\nTest process exceeded {timeout_seconds:.3g}s; terminating process tree...",
                    file=sys.stderr,
                )
                cleanup_attempted = True
                _terminate_and_reap(process, reason="the test timeout", windows_job=windows_job)
                exit_code = 124
                break
            try:
                process.wait(timeout=min(remaining, 1.0))
            except subprocess.TimeoutExpired:
                continue

        else:
            exit_code = 130 if interrupted else int(process.returncode)
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
        if windows_job is not None:
            # Closing a kill-on-close job also clears children whose parent
            # already exited, which taskkill cannot discover afterwards.
            windows_job.close()
        if not cleanup_attempted and (process.poll() is None or (os.name != "nt" and _process_group_is_active(process.pid))):
            if not _terminate_and_reap(process, reason="runner cleanup", windows_job=windows_job) and exit_code == 0:
                exit_code = 125

    return exit_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=600.0, help="maximum runtime in seconds")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="test executable and arguments")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    try:
        return run(command, args.timeout)
    except (OSError, ValueError) as error:
        print(f"Unable to start test process: {error}", file=sys.stderr)
        return 127


if __name__ == "__main__":
    raise SystemExit(main())
