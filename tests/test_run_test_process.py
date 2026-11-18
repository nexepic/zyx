"""Regression coverage for the portable test-process supervisor."""

import importlib.util
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace

import pytest


def _load_script(script_name):
    source_root = Path(__file__).resolve().parents[1]
    script_path = source_root / "scripts" / script_name
    spec = importlib.util.spec_from_file_location(f"zyx_{script_name.replace('.', '_')}", script_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(script_path.parent))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.pop(0)
    return module


def _load_runner():
    return _load_script("run_test_process.py")


def _gate_path():
    return Path(__file__).resolve().parents[1] / "scripts" / "run_test_child.py"


def _pid_is_active(pid: int) -> bool:
    if os.name == "nt":
        return _windows_pid_is_active(pid)
    if sys.platform.startswith("linux"):
        try:
            stat = Path(f"/proc/{pid}/stat").read_text()
            if stat[stat.rindex(")") + 2:].split()[0] in ("Z", "X"):
                return False
        except FileNotFoundError:
            return False
        except (OSError, ValueError, IndexError):
            pass
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _windows_pid_is_active(pid: int) -> bool:
    # os.kill(pid, 0) is not a non-destructive POSIX-style probe on Windows.
    # Observe the process handle without sending a termination/control event.
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    handle = kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE only
    if not handle:
        error = ctypes.get_last_error()
        if error == 87:  # ERROR_INVALID_PARAMETER: PID no longer exists
            return False
        if error == 5:  # Access denied does not establish process death.
            return True
        raise ctypes.WinError(error)
    try:
        result = kernel.WaitForSingleObject(handle, 0)
        if result == 0:
            return False
        if result == 258:  # WAIT_TIMEOUT: process has not exited
            return True
        raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(handle)


@pytest.mark.parametrize("wait_result,open_error,expected", [(0, None, False), (258, None, True),
                                                            (None, 87, False), (None, 5, True)])
def test_windows_pid_probe_observes_handle_without_signalling(monkeypatch, wait_result, open_error, expected):
    import ctypes
    calls = []
    class Function:
        def __init__(self, name, result): self.name, self.result = name, result
        def __call__(self, *args): calls.append((self.name, args)); return self.result
    kernel = SimpleNamespace(OpenProcess=Function("open", None if open_error else 123),
                             WaitForSingleObject=Function("wait", wait_result),
                             CloseHandle=Function("close", True))
    monkeypatch.setattr(ctypes, "WinDLL", lambda *a, **kw: kernel, raising=False)
    monkeypatch.setattr(ctypes, "get_last_error", lambda: open_error, raising=False)
    assert _windows_pid_is_active(456) is expected
    assert calls[0] == ("open", (0x00100000, False, 456))
    assert [name for name, _ in calls] == (["open"] if open_error else ["open", "wait", "close"])


def test_runner_returns_success_for_a_completed_command():
	runner = _load_runner()

	assert runner.run([sys.executable, "-c", "pass"], 5.0) == 0


def test_runner_rejects_an_invalid_heartbeat_interval(monkeypatch):
	runner = _load_runner()
	monkeypatch.setenv("ZYX_TEST_HEARTBEAT_SECONDS", "not-a-number")

	with pytest.raises(ValueError, match="ZYX_TEST_HEARTBEAT_SECONDS"):
		assert runner.run([sys.executable, "-c", "pass"], 5.0) == 0


def test_shard_result_records_the_specific_invocation(tmp_path):
    shard_runner = _load_script("run_test_shards.py")
    result_path = tmp_path / "result.json"
    started_at = time.time() - 1.0

    shard_runner.write_result(
        result_path,
        0,
        2,
        run_id="native-test-run",
        started_at=started_at,
        command=["/tmp/zyx_test_suite", "--gtest_filter=Native.*"],
    )

    record = json.loads(result_path.read_text())
    assert record["command"] == ["/tmp/zyx_test_suite", "--gtest_filter=Native.*"]
    assert record["completed_at_unix_seconds"] >= started_at
    assert record["exit_code"] == 0
    assert record["jobs"] == 2
    assert record["launcher_pid"] == os.getpid()
    assert record["run_id"] == "native-test-run"
    assert record["started_at_unix_seconds"] == started_at
    assert record["working_directory"] == str(Path.cwd().resolve())


@pytest.mark.skipif(os.name == "nt", reason="parallel shard isolation is POSIX-only")
def test_shard_runner_does_not_raise_when_process_group_cannot_be_signalled(monkeypatch):
    shard_runner = _load_script("run_test_shards.py")

    def reject_process_group_signal(_process_group_id, _signal):
        raise PermissionError("sandbox owns the process group")

    monkeypatch.setattr(shard_runner.os, "killpg", reject_process_group_signal)
    assert shard_runner.terminate_process_tree(SimpleNamespace(pid=12345), force=False) is None


@pytest.mark.skipif(os.name == "nt", reason="parallel shard isolation is POSIX-only")
def test_shard_runner_retains_failed_shard_logs(tmp_path, capsys):
    shard_runner = _load_script("run_test_shards.py")
    failure_log_directory = tmp_path / "failed-shards"

    exit_code = shard_runner.run_shards(
        [sys.executable, "-c", "import sys; print('intentional shard failure'); sys.exit(17)"],
        2,
        5.0,
        failure_log_directory,
    )

    assert exit_code == 1
    retained = sorted(failure_log_directory.glob("shard-*.log"))
    assert len(retained) == 2
    assert all("intentional shard failure" in path.read_text() for path in retained)
    assert str(failure_log_directory.resolve()) in capsys.readouterr().err


def test_runner_returns_timeout_after_terminating_the_test_process():
    runner = _load_runner()
    started = time.monotonic()

    assert runner.run([sys.executable, "-c", "import time; time.sleep(60)"], 0.1) == 124
    # Windows first attempts taskkill (bounded by five seconds), then the
    # documented graceful/forced reap windows. The old <5s assertion expired
    # before the existing graceful reap window itself could finish.
    cleanup_bound = 5.0 + 2.0 * runner.PROCESS_REAP_TIMEOUT_SECONDS
    assert time.monotonic() - started < 0.1 + cleanup_bound + 2.0


def test_runner_forces_cleanup_only_after_the_graceful_reap_window(monkeypatch):
    runner = _load_runner()
    calls = []
    process, job = object(), object()
    monkeypatch.setattr(runner, "_terminate_process_tree", lambda p, **kw: calls.append(("terminate", p, kw)))
    waits = iter([False, True])
    def wait(p, timeout, j):
        calls.append(("wait", p, timeout, j)); return next(waits)
    monkeypatch.setattr(runner, "_wait_for_exit", wait)
    assert runner._terminate_and_reap(process, reason="test", windows_job=job)
    assert calls == [("terminate", process, {"windows_job": job}),
                     ("wait", process, runner.PROCESS_REAP_TIMEOUT_SECONDS, job),
                     ("terminate", process, {"force": True, "windows_job": job}),
                     ("wait", process, runner.PROCESS_REAP_TIMEOUT_SECONDS, job)]


def test_runner_reaps_child_that_outlives_successful_test_root(tmp_path):
    runner = _load_runner()
    child_pid_path = tmp_path / "child.pid"
    child_program = (
        "from pathlib import Path\n"
        "import os\n"
        "import time\n"
        f"Path({str(child_pid_path)!r}).write_text(str(os.getpid()))\n"
        "time.sleep(60)\n"
    )
    parent_program = (
        "from pathlib import Path\n"
        "import subprocess\n"
        "import sys\n"
        "import time\n"
        "time.sleep(0.3)\n"
        f"child = subprocess.Popen([sys.executable, '-c', {child_program!r}])\n"
        "deadline = time.monotonic() + 5\n"
        f"while not Path({str(child_pid_path)!r}).exists() and time.monotonic() < deadline:\n"
        "    time.sleep(0.01)\n"
    )

    # The child has its own five-second startup deadline; allow the parent
    # additional time to exit on a loaded CI worker before the supervisor timeout.
    assert runner.run([sys.executable, "-c", parent_program], 15.0) == 0
    assert child_pid_path.exists()
    child_pid = int(child_pid_path.read_text())

    deadline = time.monotonic() + 5.0
    while _pid_is_active(child_pid) and time.monotonic() < deadline:
        time.sleep(0.05)
    assert not _pid_is_active(child_pid)


def test_windows_gate_waits_for_release_before_starting_test_command(tmp_path):
    marker = tmp_path / "started"
    child_program = f"from pathlib import Path; Path({str(marker)!r}).write_text('started')"
    process = subprocess.Popen(
        [sys.executable, str(_gate_path()), "--", sys.executable, "-c", child_program],
        stdin=subprocess.PIPE,
    )
    try:
        time.sleep(0.1)
        assert not marker.exists()
        assert process.stdin is not None
        process.stdin.write(b"\n")
        process.stdin.close()
        assert process.wait(timeout=5.0) == 0
        assert marker.read_text() == "started"
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5.0)


def test_windows_gate_propagates_the_test_exit_code():
    process = subprocess.Popen(
        [sys.executable, str(_gate_path()), "--", sys.executable, "-c", "import sys; sys.exit(23)"],
        stdin=subprocess.PIPE,
    )
    try:
        assert process.stdin is not None
        process.stdin.write(b"\n")
        process.stdin.close()
        assert process.wait(timeout=5.0) == 23
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5.0)


@pytest.mark.skipif(os.name == "nt", reason="parallel shard isolation is POSIX-only")
def test_shard_runner_reaps_children_that_outlive_successful_shard_roots(tmp_path, monkeypatch):
    shard_runner = _load_script("run_test_shards.py")
    marker_directory = tmp_path / "child-pids"
    marker_directory.mkdir()
    monkeypatch.setenv("ZYX_TEST_CHILD_PID_DIRECTORY", str(marker_directory))
    child_program = (
        "from pathlib import Path\n"
        "import os\n"
        "import time\n"
        "marker = Path(os.environ['ZYX_TEST_CHILD_PID_DIRECTORY']) / (os.environ['GTEST_SHARD_INDEX'] + '.pid')\n"
        "marker.write_text(str(os.getpid()))\n"
        "time.sleep(60)\n"
    )
    parent_program = (
        "from pathlib import Path\n"
        "import os\n"
        "import subprocess\n"
        "import sys\n"
        "import time\n"
        "time.sleep(0.3)\n"
        f"child = subprocess.Popen([sys.executable, '-c', {child_program!r}])\n"
        "marker = Path(os.environ['ZYX_TEST_CHILD_PID_DIRECTORY']) / (os.environ['GTEST_SHARD_INDEX'] + '.pid')\n"
        "deadline = time.monotonic() + 5\n"
        "while not marker.exists() and time.monotonic() < deadline:\n"
        "    time.sleep(0.01)\n"
    )

    assert shard_runner.run_shards([sys.executable, "-c", parent_program], 2, 15.0, None) == 0
    child_pids = [int(path.read_text()) for path in marker_directory.glob("*.pid")]
    assert len(child_pids) == 2

    deadline = time.monotonic() + 5.0
    while any(_pid_is_active(pid) for pid in child_pids) and time.monotonic() < deadline:
        time.sleep(0.05)
    assert all(not _pid_is_active(pid) for pid in child_pids)


@pytest.mark.skipif(os.name == "nt", reason="parallel shard isolation is POSIX-only")
def test_shard_runner_fails_when_final_cleanup_reports_an_unreapable_process(monkeypatch):
    shard_runner = _load_script("run_test_shards.py")
    monkeypatch.setattr(shard_runner, "terminate_and_reap_processes", lambda *args, **kwargs: [(0, 12345)])

    assert shard_runner.run_shards([sys.executable, "-c", "pass"], 2, 5.0, None) == 125


@pytest.mark.parametrize('script_name', ['run_test_process.py', 'run_test_shards.py'])
def test_linux_group_detection_never_treats_live_member_as_zombie(tmp_path, monkeypatch, script_name):
    module = _load_script(script_name)
    for pid, state in [(101, 'Z'), (102, 'S')]:
        directory = tmp_path / str(pid)
        directory.mkdir()
        (directory / 'stat').write_text(f'{pid} (test) {state} 1 77 77 0')
    monkeypatch.setattr(module, 'Path', lambda path: tmp_path if path == '/proc' else Path(path))
    assert module._linux_group_has_live_members(77)
    (tmp_path / '102' / 'stat').write_text('102 (test) Z 1 77 77 0')
    assert not module._linux_group_has_live_members(77)
    (tmp_path / '102' / 'stat').write_text('invalid')
    assert module._linux_group_has_live_members(77)
    assert module._linux_group_has_live_members(999)

@pytest.mark.skipif(os.name == 'nt', reason='parallel shard isolation is POSIX-only')
def test_shard_timeout_keeps_logs_outside_deleted_workdir(tmp_path):
    runner = _load_script('run_test_shards.py')
    logs = tmp_path / 'retained'
    code = "import time;print('persist-before-timeout',flush=True);time.sleep(60)"
    assert runner.run_shards([sys.executable, '-c', code], 2, .5, logs) == 124
    assert len(list(logs.glob('shard-*.log'))) == 2
    assert all('persist-before-timeout' in p.read_text() for p in logs.glob('*.log'))


@pytest.mark.skipif(os.name == 'nt', reason='parallel shard isolation is POSIX-only')
@pytest.mark.parametrize('codes', [(124, 0), (0, 124), (17, 124), (124, 17)])
def test_completed_inner_timeout_survives_shard_aggregation(tmp_path, codes):
    runner = _load_script('run_test_shards.py')
    logs = tmp_path / 'logs'
    program = ("import os,sys;print('inner-status-marker',flush=True);"
               f"sys.exit({codes!r}[int(os.environ['GTEST_SHARD_INDEX'])])")
    # Inner evidence can finish its own timeout before the outer shard
    # deadline. Exercise that order without racing two real timers.
    assert runner.run_shards([sys.executable, '-c', program], 2, 10, logs) == 124
    status = json.loads((logs / 'status.json').read_text())
    assert status['exit_code'] == 124 and not status['interrupted']
    assert [item['raw_exit_code'] for item in status['shards']] == list(codes)
    assert all('inner-status-marker' in (logs / f'shard-{n}.log').read_text() for n in (0, 1))


@pytest.mark.skipif(os.name == 'nt', reason='parallel shard isolation is POSIX-only')
def test_successful_and_failed_shards_both_publish_observed_counts(tmp_path, capsys):
    runner = _load_script('run_test_shards.py')
    logs = tmp_path / 'logs'
    program = ("import os,sys,json;i=int(os.environ['GTEST_SHARD_INDEX']);"
               "print('CI_CASE_JOURNAL '+json.dumps(dict(started=2+i,completed=2+i,failed=i)));sys.exit(i)")
    assert runner.run_shards([sys.executable, '-c', program], 2, 10, logs) == 1
    summaries = [json.loads(line[len('CI_SHARD_RESULT '):]) for line in capsys.readouterr().out.splitlines()
                 if line.startswith('CI_SHARD_RESULT ')]
    status = json.loads((logs / 'status.json').read_text())
    assert summaries == status['shards']
    assert [s['raw_exit_code'] for s in summaries] == [0, 1]
    assert all(s['journal_available'] for s in summaries)
    assert [s['observed_counts'] for s in summaries] == [
        dict(started=2, completed=2, failed=0), dict(started=3, completed=3, failed=1)]

@pytest.mark.parametrize('snapshot,code,expected', [
    (b'10 77 Z\n11 77 Z+\n12 88 S\n', 0, False),
    (b'10 77 Z\n11 77 S\n', 0, True),
    (b'10 88 Z\n', 0, True),
    (b'', 0, True), (b'malformed\n', 0, True),
    (b'10 77 Z\n', 1, True), (b'x' * (1024 * 1024 + 1), 0, True)])
def test_darwin_snapshot_only_accepts_complete_nonempty_zombie_group(monkeypatch, snapshot, code, expected):
    runner = _load_runner()
    def ps(command, **kwargs):
        assert command == ['/bin/ps', '-axo', 'pid=,pgid=,stat=']
        assert kwargs['timeout'] == 1
        kwargs['stdout'].write(snapshot)
        return SimpleNamespace(returncode=code)
    monkeypatch.setattr(runner.subprocess, 'run', ps)
    assert runner._darwin_group_has_live_members(77) is expected

@pytest.mark.parametrize('error', [PermissionError(1, 'denied'), subprocess.TimeoutExpired('ps', 1)])
def test_darwin_unavailable_snapshot_remains_active(monkeypatch, error):
    runner = _load_runner()
    def ps(*args, **kwargs): raise error
    monkeypatch.setattr(runner.subprocess, 'run', ps)
    assert runner._darwin_group_has_live_members(77)

@pytest.mark.skipif(os.name == 'nt', reason='POSIX group signalling')
@pytest.mark.parametrize('live,child_code,expected', [(False, 0, 0), (True, 0, 125), (True, 17, 17)])
def test_eperm_cleanup_preserves_child_status_and_requires_verified_inactivity(monkeypatch, live, child_code, expected, capsys):
    runner = _load_runner()
    monkeypatch.setattr(runner.sys, 'platform', 'darwin')
    monkeypatch.setattr(runner, '_darwin_group_has_live_members', lambda group: live)
    monkeypatch.setattr(runner, 'PROCESS_REAP_TIMEOUT_SECONDS', .01)
    def deny(*args): raise PermissionError(1, 'Operation not permitted')
    monkeypatch.setattr(runner.os, 'killpg', deny)
    details = {}
    assert runner.run([sys.executable, '-c', f'import sys;sys.exit({child_code})'], 5, details=details) == expected
    assert details['child_exit_code'] == child_code
    assert details['cleanup_succeeded'] is not live
    if live:
        assert details['failure_phase'] == 'cleanup'
        assert 'cleanup signal denied' in capsys.readouterr().err

@pytest.mark.skipif(os.name == 'nt', reason='POSIX group signalling')
def test_eperm_timeout_is_not_reclassified_as_startup_failure(monkeypatch):
    runner = _load_runner()
    original = runner.os.killpg
    def denied_after_kill(group, sig):
        if sig: original(group, sig)
        raise PermissionError(1, 'denied')
    monkeypatch.setattr(runner.os, 'killpg', denied_after_kill)
    monkeypatch.setattr(runner, '_process_group_is_active', lambda group: True)
    monkeypatch.setattr(runner, 'PROCESS_REAP_TIMEOUT_SECONDS', .01)
    details = {}
    assert runner.run([sys.executable, '-c', 'import time;time.sleep(60)'], .05, details=details) == 124
    assert details['cleanup_succeeded'] is False
    assert details['failure_phase'] == 'cleanup'


def test_main_reports_startup_failure_separately(tmp_path, monkeypatch, capsys):
    runner = _load_runner()
    path = tmp_path / 'result.json'
    monkeypatch.setattr(runner.sys, 'argv', ['run_test_process.py', '--result-file', str(path), '--', str(tmp_path / 'missing-command')])
    # Use a direct launch denial to exercise the same startup boundary on Windows.
    def deny(*args, **kwargs): raise PermissionError(1, 'launch denied')
    monkeypatch.setattr(runner.subprocess, 'Popen', deny)
    assert runner.main() == 127
    result = json.loads(path.read_text())
    assert result['failure_phase'] == 'startup' and result['child_exit_code'] is None
    assert result['error_errno'] == 1
    assert 'during startup' in capsys.readouterr().err

@pytest.mark.skipif(os.name == 'nt', reason='POSIX group signalling')
@pytest.mark.parametrize('live', [False, True])
def test_shard_darwin_eperm_uses_same_verified_group_state(monkeypatch, live):
    runner = _load_script('run_test_shards.py')
    monkeypatch.setattr(runner.sys, 'platform', 'darwin')
    monkeypatch.setattr(runner, '_darwin_group_has_live_members', lambda group: live)
    def deny(*args): raise PermissionError(1, 'denied')
    monkeypatch.setattr(runner.os, 'killpg', deny)
    assert runner.process_tree_is_active(SimpleNamespace(pid=77)) is live

@pytest.mark.skipif(os.name == 'nt', reason='POSIX process lifecycle')
def test_main_retains_execution_error_stage_and_completed_child_status(monkeypatch, tmp_path, capsys):
    runner = _load_runner()
    path = tmp_path / 'result.json'
    process = SimpleNamespace(pid=77, returncode=0, poll=lambda: None)
    def fail_wait(**kwargs):
        process.poll = lambda: 0
        raise OSError(5, 'wait failed')
    process.wait = fail_wait
    monkeypatch.setattr(runner.subprocess, 'Popen', lambda *a, **kw: process)
    monkeypatch.setattr(runner, '_process_group_is_active', lambda pid: False)
    monkeypatch.setattr(runner.sys, 'argv', ['runner', '--result-file', str(path), '--', 'command'])
    assert runner.main() == 125
    result = json.loads(path.read_text())
    assert result['failure_phase'] == 'execution' and result['child_exit_code'] == 0
    assert 'during execution' in capsys.readouterr().err

@pytest.mark.skipif(sys.platform != 'darwin', reason='Darwin native zombie-group semantics')
def test_darwin_native_exited_unreaped_group_is_not_live():
    runner = _load_runner()
    process = subprocess.Popen([sys.executable, '-c', 'pass'], start_new_session=True)
    try:
        # Intentionally do not poll/wait until after the group-state observation.
        deadline = time.monotonic() + 5
        while runner._darwin_group_has_live_members(process.pid) and time.monotonic() < deadline:
            time.sleep(.05)
        assert not runner._darwin_group_has_live_members(process.pid)
        assert not runner._process_group_is_active(process.pid)
        assert process.wait(timeout=1) == 0
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)

@pytest.mark.skipif(os.name == 'nt', reason='POSIX group signalling')
@pytest.mark.parametrize('script', ['run_test_process.py', 'run_test_shards.py'])
@pytest.mark.parametrize('initial_denial', [False, True])
def test_darwin_successful_kernel_probe_cannot_be_overridden_by_zombie_snapshot(monkeypatch, script, initial_denial):
    runner = _load_script(script)
    monkeypatch.setattr(runner.sys, 'platform', 'darwin')
    monkeypatch.setattr(runner, '_darwin_group_has_live_members', lambda group: False)
    calls = []
    def signal_group(group, sig):
        calls.append((group, sig))
        if initial_denial and len(calls) == 1:
            raise PermissionError(1, 'zombies at first probe')
    monkeypatch.setattr(runner.os, 'killpg', signal_group)
    if script == 'run_test_process.py':
        assert runner._process_group_is_active(77)
    else:
        assert runner.process_tree_is_active(SimpleNamespace(pid=77))
    assert len(calls) == (2 if initial_denial else 1)
