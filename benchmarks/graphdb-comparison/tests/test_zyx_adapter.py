from __future__ import annotations

import os
import json
import sys
import subprocess
import pytest
from pathlib import Path

from runner.adapters.zyx import ZyxAdapter


def _write_executable(path: Path, body: str) -> None:
    path.write_text(body)
    path.chmod(path.stat().st_mode | 0o111)


def _successful_fake(path: Path, args_path: Path | None = None, emit_profile: bool = False) -> None:
    record_args = ""
    if args_path is not None:
        record_args = (
            f"with Path({str(args_path)!r}).open('a', encoding='utf-8') as handle:\n"
            "    handle.write(json.dumps(sys.argv[1:]) + '\\n')\n"
        )
    profile_output = ""
    if emit_profile:
        profile_output = (
            "print(json.dumps({'event':'profile','database':'zyx','equivalent_mode':'api','scale':scale,"
            "'profile':profile,'workload':workload,'iteration':0,'phase':'parse','total_time_ms':0.25,"
            "'calls':1,'value_total':4,'value_calls':1,'value_avg':4.0}))\n"
        )
    _write_executable(
        path,
        f"#!{sys.executable}\n"
        "import json, sys\n"
        "from pathlib import Path\n"
        "args = sys.argv[1:]\n"
        "workload = args[args.index('--workload') + 1]\n"
        "scale = args[args.index('--scale') + 1]\n"
        "profile = args[args.index('--profile') + 1]\n"
        "iterations = int(args[args.index('--iterations') + 1])\n"
        + record_args
        + "for iteration in range(iterations):\n"
        "    print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':scale,"
        "'iteration':iteration,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}))\n"
        + profile_output,
    )


def _read_invocations(path: Path) -> list[list[str]]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def test_zyx_adapter_disables_diagnostics_for_release_measurement(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-no-profile.py"
    args_path = tmp_path / "args.txt"
    _successful_fake(binary, args_path)
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    monkeypatch.setenv("ZYX_COMPARE_EMIT_PROFILE", "0")
    adapter = ZyxAdapter("zyx", tmp_path / "dataset", "smoke", profile="indexed")
    results = adapter.run_all(warmup=0, iterations=1)
    assert all(result.status == "ok" for result in results)
    assert len(_read_invocations(args_path)) == 4
    assert all("--emit-profile" not in args for args in _read_invocations(args_path))
    assert adapter.profile_events == []


def test_zyx_adapter_rejects_ambiguous_diagnostic_setting(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("ZYX_COMPARE_EMIT_PROFILE", "false")
    with pytest.raises(ValueError, match="ZYX_COMPARE_EMIT_PROFILE"):
        ZyxAdapter("zyx", tmp_path / "dataset", "smoke")


def test_zyx_adapter_uses_env_binary_and_parses_samples(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-fake.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json, sys\n"
        "args = sys.argv[1:]\n"
        "workload = args[args.index('--workload') + 1]\n"
        "if workload == 'load_nodes_edges':\n"
        "    print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':0,'latency_ms':1.25,'status':'ok','equivalent_mode':'api'}))\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke")

    results = adapter.run_all(warmup=0, iterations=1)

    assert results[0].workload == "load_nodes_edges"
    assert results[0].samples[0].equivalent_mode == "api"
    assert results[0].samples[0].latency_ms == 1.25
    assert results[-1].status == "failed"
    assert results[-1].workload == "label_scan_filter"
    assert "missing iterations" in results[-1].error


def test_zyx_adapter_forwards_profile_to_subprocess(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-profile.py"
    args_path = tmp_path / "args.txt"
    _successful_fake(binary, args_path)
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(
        database="zyx",
        dataset_dir=tmp_path / "dataset",
        scale="smoke",
        profile="indexed",
    )

    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.workload for result in results] == [
        "load_nodes_edges",
        "point_lookup_indexed",
        "property_equality_indexed",
        "property_range_indexed",
    ]
    invocations = _read_invocations(args_path)
    assert len(invocations) == 4
    assert [args[args.index("--workload") + 1] for args in invocations] == [
        "load_nodes_edges",
        "point_lookup_indexed",
        "property_equality_indexed",
        "property_range_indexed",
    ]
    args = invocations[-1]
    profile_index = args.index("--profile")
    iterations_index = args.index("--iterations")
    execution_mode_index = args.index("--execution-mode")
    emit_profile_index = args.index("--emit-profile")
    assert args[profile_index + 1] == "indexed"
    assert "--result-cache" not in args
    assert args[iterations_index + 1] == "1"
    assert args[execution_mode_index + 1] == "warm"
    workload_index = args.index("--workload")
    assert profile_index < iterations_index < execution_mode_index < workload_index
    assert emit_profile_index == workload_index + 2


def test_zyx_adapter_forwards_threads_to_subprocess(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-threads.py"
    args_path = tmp_path / "args.txt"
    _successful_fake(binary, args_path)
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed", threads=8)

    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.status for result in results] == ["ok"] * 4
    invocations = _read_invocations(args_path)
    assert len(invocations) == 4
    assert all(args[args.index("--threads") + 1] == "8" for args in invocations)
    assert all(args.index("--threads") < args.index("--emit-profile") for args in invocations)


def test_zyx_adapter_accepts_write_profile_workloads(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-write.py"
    _successful_fake(binary)
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="write")

    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.workload for result in results] == [
        "load_nodes_edges",
        "point_create_node",
        "point_create_edge",
        "point_update_node_property",
        "point_update_edge_property",
        "point_create_delete_edge",
        "write_then_read_edge",
    ]
    assert [result.status for result in results] == ["ok"] * 7


def test_zyx_adapter_accepts_write_durable_profile_workloads(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-write-durable.py"
    _successful_fake(binary)
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="write_durable")

    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.workload for result in results] == [
        "load_nodes_edges",
        "point_create_node_durable",
        "point_create_edge_durable",
        "point_update_node_property_durable",
        "point_update_edge_property_durable",
        "point_create_delete_edge_durable",
        "write_then_read_edge_durable",
    ]
    assert [result.status for result in results] == ["ok"] * 7



def test_zyx_adapter_accepts_operational_dynamic_profile_workloads(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-operational_dynamic.py"
    _successful_fake(binary)
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="operational_dynamic")

    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.workload for result in results] == [
        "load_nodes_edges",
        "index_seek_then_one_hop_expand",
        "index_seek_then_two_hop_expand",
        "post_persist_create_node",
        "post_persist_create_edge",
        "write_then_one_hop_expand",
        "batch_create_edges_100",
        "batch_create_edges_1000",
        "batch_create_edges_10000",
        "batch_create_edges_100_then_one_hop_expand",
        "batch_create_edges_10000_then_one_hop_expand",
    ]
    assert [result.status for result in results] == ["ok"] * 11


def test_zyx_adapter_accepts_retrieval_profile_workloads(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-retrieval.py"
    _successful_fake(binary)
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="retrieval")

    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.workload for result in results] == [
        "load_nodes_edges",
        "point_node_fetch_by_id",
        "point_edge_fetch_by_endpoints",
        "batch_node_fetch_100",
        "one_hop_fetch_neighbor_ids",
        "one_hop_fetch_neighbor_records",
        "property_index_fetch_users_by_country",
        "range_index_fetch_user_projection",
        "relationship_property_fetch",
    ]
    assert [result.status for result in results] == ["ok"] * 9


def test_zyx_adapter_forwards_coldish_execution_mode_to_subprocess(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-coldish.py"
    args_path = tmp_path / "args.txt"
    _successful_fake(binary, args_path)
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    results = adapter.run_all(warmup=0, iterations=1, execution_mode="cold-ish")

    assert [result.status for result in results] == ["ok"] * 4
    invocations = _read_invocations(args_path)
    assert all(args[args.index("--execution-mode") + 1] == "cold-ish" for args in invocations)


def test_zyx_adapter_forwards_opened_execution_mode_to_subprocess(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-opened.py"
    args_path = tmp_path / "args.txt"
    _successful_fake(binary, args_path)
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    results = adapter.run_all(warmup=2, iterations=1, execution_mode="opened")

    assert [result.status for result in results] == ["ok"] * 4
    invocations = _read_invocations(args_path)
    assert all(args[args.index("--execution-mode") + 1] == "opened" for args in invocations)


def test_zyx_adapter_fails_incomplete_indexed_workload_samples(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-incomplete.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json, sys\n"
        "args = sys.argv[1:]\n"
        "workload = args[args.index('--workload') + 1]\n"
        "for iteration in range(2):\n"
        "    if workload == 'property_range_indexed' and iteration == 1:\n"
        "        continue\n"
        "    print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':iteration,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}))\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    results = adapter.run_all(warmup=0, iterations=2)

    assert results[-1].status == "failed"
    assert results[-1].workload == "property_range_indexed"
    assert "missing iterations" in results[-1].error
    assert "property_range_indexed" in results[-1].error


def test_zyx_adapter_rejects_duplicate_iterations(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-duplicate.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json, sys\n"
        "args = sys.argv[1:]\n"
        "workload = args[args.index('--workload') + 1]\n"
        "event = {'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':0,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}\n"
        "print(json.dumps(event))\n"
        "print(json.dumps(event))\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke")

    results = adapter.run_all(warmup=0, iterations=1)

    assert len(results) == 1
    assert results[0].workload == "load_nodes_edges"
    assert results[0].status == "failed"
    assert "duplicate iterations 1" in results[0].error


def test_zyx_adapter_fails_unexpected_workload_sample_for_profile(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-unexpected.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json, sys\n"
        "args = sys.argv[1:]\n"
        "selected = args[args.index('--workload') + 1]\n"
        "workload = 'label_scan_filter' if selected == 'point_lookup_indexed' else selected\n"
        "print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':0,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}))\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    results = adapter.run_all(warmup=0, iterations=1)

    assert results[-1].status == "failed"
    assert "unexpected workload" in results[-1].error
    assert "label_scan_filter" in results[-1].error
    assert results[0].workload == "load_nodes_edges"
    assert results[0].status == "ok"


def test_zyx_adapter_returns_failed_result_for_malformed_output(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-bad.py"
    _write_executable(binary, f"#!{sys.executable}\nprint('not json')\n")
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke")

    results = adapter.run_all(warmup=0, iterations=1)

    assert len(results) == 1
    assert results[0].status == "failed"
    assert results[0].workload == "load_nodes_edges"
    assert "malformed JSONL" in results[0].error


def test_zyx_adapter_converts_subprocess_error_event(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-error.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json, sys\n"
        "args = sys.argv[1:]\n"
        "workload = args[args.index('--workload') + 1]\n"
        "if workload == 'load_nodes_edges':\n"
        "    print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':0,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}))\n"
        "else:\n"
        "    print(json.dumps({'event':'error','database':'zyx','workload':workload,'scale':'smoke','status':'failed','error':'boom','equivalent_mode':'api'}), file=sys.stderr)\n"
        "    raise SystemExit(1)\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    results = adapter.run_all(warmup=0, iterations=1)

    assert len(results) == 2
    assert results[0].status == "ok"
    assert results[1].status == "failed"
    assert results[1].workload == "point_lookup_indexed"
    assert results[1].error == "boom"
    assert results[1].equivalent_mode == "api"


def test_zyx_adapter_clears_profile_events_before_validation(tmp_path: Path, monkeypatch):
    from runner.models import ProfileEvent

    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke")
    adapter.profile_events = [
        ProfileEvent(
            database="zyx",
            workload="point_lookup_indexed",
            scale="smoke",
            profile="indexed",
            iteration=0,
            phase="parse",
            total_time_ms=1.0,
            calls=1,
        )
    ]

    results = adapter.run_all(warmup=-1, iterations=1)

    assert results[0].status == "failed"
    assert adapter.profile_events == []


def test_zyx_adapter_rejects_profile_events_missing_database_or_scale(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-bad-profile.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json, sys\n"
        "args = sys.argv[1:]\n"
        "workload = args[args.index('--workload') + 1]\n"
        "print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':0,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}))\n"
        "if workload == 'point_lookup_indexed':\n"
        "    print(json.dumps({'event':'profile','equivalent_mode':'api','scale':'smoke','profile':'indexed','workload':workload,'iteration':0,'phase':'parse','total_time_ms':0.25,'calls':1}))\n"
        "    print(json.dumps({'event':'profile','database':'zyx','equivalent_mode':'api','profile':'indexed','workload':workload,'iteration':0,'phase':'execute','total_time_ms':0.5,'calls':1}))\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    results = adapter.run_all(warmup=0, iterations=1)

    assert results[-1].status == "failed"
    assert results[-1].workload == "point_lookup_indexed"
    assert "invalid profile event" in results[-1].error
    assert "database" in results[-1].error
    assert "scale" in results[-1].error
    assert adapter.profile_events == []


def test_zyx_adapter_rejects_malformed_profile_event(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-bad-profile.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json\n"
        "print(json.dumps({'event':'profile','database':'zyx','equivalent_mode':'api','scale':'smoke','profile':'scan','workload':'load_nodes_edges','iteration':0,'phase':'parse','total_time_ms':'not-a-number','calls':1}))\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke")

    results = adapter.run_all(warmup=0, iterations=1)

    assert len(results) == 1
    assert results[0].status == "failed"
    assert results[0].workload == "load_nodes_edges"
    assert "invalid profile event" in results[0].error


def test_zyx_adapter_defaults_binary_path(tmp_path: Path, monkeypatch):
    monkeypatch.delenv("ZYX_COMPARE_BENCH", raising=False)
    monkeypatch.delenv("ZYX_COMPARE_TIMEOUT_SECONDS", raising=False)

    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke")

    assert adapter.binary == Path("/usr/local/bin/zyx-compare-bench")
    assert adapter.db_path == tmp_path / "zyx.db"
    assert adapter.timeout_seconds == 600


def test_zyx_adapter_cleans_single_file_database_artifacts(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    dataset_dir.mkdir()
    adapter = ZyxAdapter(database="zyx", dataset_dir=dataset_dir, scale="smoke")
    db_path = tmp_path / "zyx.db"
    wal_path = tmp_path / "zyx.db-wal"
    load_path = tmp_path / "zyx.db.load-iteration-0"
    workload_path = tmp_path / "zyx.db.workload-point_create_node"
    workload_path.mkdir()
    (workload_path / "part").write_text("temporary")
    coldish_path = tmp_path / "zyx.db.coldish-point_create_node-0"
    coldish_path.mkdir()
    (coldish_path / "part").write_text("temporary")
    baseline_path = tmp_path / "zyx.db.benchmark-baseline"
    baseline_path.write_text("baseline")
    unrelated = tmp_path / "zyx-not-a-db"
    db_path.write_text("db")
    wal_path.write_text("wal")
    load_path.mkdir()
    (load_path / "part").write_text("temporary")
    unrelated.write_text("keep")

    removed = adapter.cleanup_artifacts()

    assert removed == [db_path, load_path, workload_path, coldish_path, baseline_path]
    assert not db_path.exists()
    assert wal_path.read_text() == "wal"
    assert not load_path.exists()
    assert not workload_path.exists()
    assert not coldish_path.exists()
    assert not baseline_path.exists()
    assert unrelated.read_text() == "keep"


def test_zyx_adapter_returns_failed_result_on_timeout(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-slow.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json, sys, time\n"
        "args = sys.argv[1:]\n"
        "workload = args[args.index('--workload') + 1]\n"
        "if workload == 'load_nodes_edges':\n"
        "    print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':0,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}))\n"
        "else:\n"
        "    time.sleep(10)\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    monkeypatch.setenv("ZYX_COMPARE_TIMEOUT_SECONDS", "0.5")
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    def run_with_timeout(command, **kwargs):
        assert kwargs["timeout"] == 0.5
        workload = command[command.index("--workload") + 1]
        if workload != "load_nodes_edges":
            raise subprocess.TimeoutExpired(command, kwargs["timeout"])
        sample = {"event": "sample", "database": "zyx", "workload": workload,
                  "scale": "smoke", "iteration": 0, "latency_ms": 1.0,
                  "status": "ok", "equivalent_mode": "api"}
        return subprocess.CompletedProcess(command, 0, json.dumps(sample) + "\n", "")

    # Exercise timeout handling independently of Python startup and machine load.
    monkeypatch.setattr("runner.adapters.zyx.subprocess.run", run_with_timeout)

    results = adapter.run_all(warmup=0, iterations=1)

    assert len(results) == 2
    assert results[0].status == "ok"
    assert results[1].status == "failed"
    assert results[1].workload == "point_lookup_indexed"
    assert "timed out after 0.5s" in results[1].error


def test_profile_event_to_event_uses_profile_schema():
    from runner.models import ProfileEvent

    event = ProfileEvent(
        database="zyx",
        workload="label_scan_filter",
        scale="smoke",
        profile="scan",
        iteration=0,
        phase="parse",
        total_time_ms=1.5,
        calls=2,
        equivalent_mode="api",
    )

    assert event.to_event() == {
        "database": "zyx",
        "workload": "label_scan_filter",
        "scale": "smoke",
        "profile": "scan",
        "iteration": 0,
        "phase": "parse",
        "total_time_ms": 1.5,
        "calls": 2,
        "equivalent_mode": "api",
        "event": "profile",
    }

    valued_event = ProfileEvent(
        database="zyx",
        workload="label_scan_filter",
        scale="smoke",
        profile="scan",
        iteration=0,
        phase="filter.parallel.workers",
        total_time_ms=0.0,
        calls=0,
        equivalent_mode="api",
        value_total=4,
        value_calls=1,
        value_avg=4.0,
    )

    assert valued_event.to_event()["value_avg"] == 4.0


def test_zyx_adapter_parses_profile_events_separately_from_samples(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-profile-events.py"
    _successful_fake(binary, emit_profile=True)
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.status for result in results] == ["ok"] * 4
    assert [event.workload for event in adapter.profile_events] == [
        "load_nodes_edges",
        "point_lookup_indexed",
        "property_equality_indexed",
        "property_range_indexed",
    ]
    assert adapter.profile_events[1].to_event() == {
        "database": "zyx",
        "equivalent_mode": "api",
        "event": "profile",
        "scale": "smoke",
        "profile": "indexed",
        "workload": "point_lookup_indexed",
        "iteration": 0,
        "phase": "parse",
        "total_time_ms": 0.25,
        "calls": 1,
        "value_total": 4,
        "value_calls": 1,
        "value_avg": 4.0,
    }
