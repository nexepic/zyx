from __future__ import annotations

import os
import sys
from pathlib import Path

from runner.adapters.zyx import ZyxAdapter


def _write_executable(path: Path, body: str) -> None:
    path.write_text(body)
    path.chmod(path.stat().st_mode | 0o111)


def test_zyx_adapter_uses_env_binary_and_parses_samples(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-fake.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json\n"
        "print(json.dumps({'event':'sample','database':'zyx','workload':'load_nodes_edges','scale':'smoke','iteration':0,'latency_ms':1.25,'status':'ok','equivalent_mode':'api'}))\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke")

    results = adapter.run_all(warmup=0, iterations=1)

    assert results[0].workload == "load_nodes_edges"
    assert results[0].samples[0].equivalent_mode == "api"
    assert results[0].samples[0].latency_ms == 1.25
    assert results[-1].status == "failed"
    assert "missing samples" in results[-1].error


def test_zyx_adapter_forwards_profile_to_subprocess(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-profile.py"
    args_path = tmp_path / "args.txt"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json, sys\n"
        f"from pathlib import Path\nPath({str(args_path)!r}).write_text('\\n'.join(sys.argv[1:]))\n"
        "for workload in ['load_nodes_edges', 'point_lookup_indexed', 'property_equality_indexed', 'property_range_indexed']:\n"
        "    print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':0,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}))\n",
    )
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
    args = args_path.read_text().splitlines()
    profile_index = args.index("--profile")
    iterations_index = args.index("--iterations")
    execution_mode_index = args.index("--execution-mode")
    emit_profile_index = args.index("--emit-profile")
    assert args[profile_index + 1] == "indexed"
    assert "--result-cache" not in args
    assert args[iterations_index + 1] == "1"
    assert args[execution_mode_index + 1] == "warm"
    assert profile_index < iterations_index < execution_mode_index
    assert emit_profile_index == execution_mode_index + 2



def test_zyx_adapter_forwards_coldish_execution_mode_to_subprocess(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-coldish.py"
    args_path = tmp_path / "args.txt"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json, sys\n"
        f"from pathlib import Path\nPath({str(args_path)!r}).write_text('\\n'.join(sys.argv[1:]))\n"
        "for workload in ['load_nodes_edges', 'point_lookup_indexed', 'property_equality_indexed', 'property_range_indexed']:\n"
        "    print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':0,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}))\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    results = adapter.run_all(warmup=0, iterations=1, execution_mode="cold-ish")

    assert [result.status for result in results] == ["ok"] * 4
    args = args_path.read_text().splitlines()
    assert args[args.index("--execution-mode") + 1] == "cold-ish"

def test_zyx_adapter_fails_incomplete_indexed_workload_samples(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-incomplete.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json\n"
        "workloads = ['load_nodes_edges', 'point_lookup_indexed', 'property_equality_indexed', 'property_range_indexed']\n"
        "for workload in workloads:\n"
        "    for iteration in range(2):\n"
        "        if workload == 'property_range_indexed' and iteration == 1:\n"
        "            continue\n"
        "        print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':iteration,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}))\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    results = adapter.run_all(warmup=0, iterations=2)

    assert results[-1].status == "failed"
    assert results[-1].workload == "run_all"
    assert "incomplete samples" in results[-1].error
    assert "property_range_indexed" in results[-1].error


def test_zyx_adapter_fails_unexpected_workload_sample_for_profile(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-unexpected.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json\n"
        "workloads = ['load_nodes_edges', 'point_lookup_indexed', 'property_equality_indexed', 'property_range_indexed', 'label_scan_filter']\n"
        "for workload in workloads:\n"
        "    print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':0,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}))\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    results = adapter.run_all(warmup=0, iterations=1)

    assert results[-1].status == "failed"
    assert "unexpected workload" in results[-1].error
    assert "label_scan_filter" in results[-1].error


def test_zyx_adapter_returns_failed_result_for_malformed_output(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-bad.py"
    _write_executable(binary, f"#!{sys.executable}\nprint('not json')\n")
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke")

    results = adapter.run_all(warmup=0, iterations=1)

    assert len(results) == 1
    assert results[0].status == "failed"
    assert results[0].workload == "subprocess_output"
    assert "malformed JSONL" in results[0].error


def test_zyx_adapter_converts_subprocess_error_event(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-error.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json, sys\n"
        "print(json.dumps({'event':'error','database':'zyx','workload':'point_lookup_indexed','scale':'smoke','status':'failed','error':'boom','equivalent_mode':'api'}), file=sys.stderr)\n"
        "raise SystemExit(1)\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke")

    results = adapter.run_all(warmup=0, iterations=1)

    assert len(results) == 1
    assert results[0].status == "failed"
    assert results[0].workload == "point_lookup_indexed"
    assert results[0].error == "boom"
    assert results[0].equivalent_mode == "api"


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
        "import json\n"
        "for workload in ['load_nodes_edges', 'point_lookup_indexed', 'property_equality_indexed', 'property_range_indexed']:\n"
        "    print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':0,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}))\n"
        "print(json.dumps({'event':'profile','equivalent_mode':'api','scale':'smoke','profile':'indexed','workload':'point_lookup_indexed','iteration':0,'phase':'parse','total_time_ms':0.25,'calls':1}))\n"
        "print(json.dumps({'event':'profile','database':'zyx','equivalent_mode':'api','profile':'indexed','workload':'point_lookup_indexed','iteration':0,'phase':'execute','total_time_ms':0.5,'calls':1}))\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    results = adapter.run_all(warmup=0, iterations=1)

    assert results[-1].status == "failed"
    assert results[-1].workload == "subprocess_output"
    assert "invalid profile event" in results[-1].error
    assert "database" in results[-1].error
    assert "scale" in results[-1].error


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
    assert results[0].workload == "subprocess_output"
    assert "invalid profile event" in results[0].error


def test_zyx_adapter_defaults_binary_path(tmp_path: Path, monkeypatch):
    monkeypatch.delenv("ZYX_COMPARE_BENCH", raising=False)
    monkeypatch.delenv("ZYX_COMPARE_TIMEOUT_SECONDS", raising=False)

    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke")

    assert adapter.binary == Path("/usr/local/bin/zyx-compare-bench")
    assert adapter.db_path == tmp_path / "zyx.db"
    assert adapter.timeout_seconds == 600


def test_zyx_adapter_returns_failed_result_on_timeout(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-slow.py"
    _write_executable(binary, f"#!{sys.executable}\nimport time\ntime.sleep(10)\n")
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    monkeypatch.setenv("ZYX_COMPARE_TIMEOUT_SECONDS", "0.01")
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke")

    results = adapter.run_all(warmup=0, iterations=1)

    assert len(results) == 1
    assert results[0].status == "failed"
    assert results[0].workload == "run_all"
    assert "timed out after 0.01s" in results[0].error


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


def test_zyx_adapter_parses_profile_events_separately_from_samples(tmp_path: Path, monkeypatch):
    binary = tmp_path / "zyx-bench-profile-events.py"
    _write_executable(
        binary,
        f"#!{sys.executable}\n"
        "import json\n"
        "workloads = ['load_nodes_edges', 'point_lookup_indexed', 'property_equality_indexed', 'property_range_indexed']\n"
        "for workload in workloads:\n"
        "    print(json.dumps({'event':'sample','database':'zyx','workload':workload,'scale':'smoke','iteration':0,'latency_ms':1.0,'status':'ok','equivalent_mode':'api'}))\n"
        "print(json.dumps({'event':'profile','database':'zyx','equivalent_mode':'api','scale':'smoke','profile':'indexed','workload':'point_lookup_indexed','iteration':0,'phase':'parse','total_time_ms':0.25,'calls':1}))\n",
    )
    monkeypatch.setenv("ZYX_COMPARE_BENCH", str(binary))
    adapter = ZyxAdapter(database="zyx", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.status for result in results] == ["ok"] * 4
    assert len(adapter.profile_events) == 1
    assert adapter.profile_events[0].to_event() == {
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
    }
