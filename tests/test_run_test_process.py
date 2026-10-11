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
    spec.loader.exec_module(module)
    return module


def _load_runner():
    return _load_script("run_test_process.py")


def _gate_path():
    return Path(__file__).resolve().parents[1] / "scripts" / "run_test_child.py"


def _pid_is_active(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


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
    assert time.monotonic() - started < 5.0


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

    assert runner.run([sys.executable, "-c", parent_program], 5.0) == 0
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

    assert shard_runner.run_shards([sys.executable, "-c", parent_program], 2, 5.0, None) == 0
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
