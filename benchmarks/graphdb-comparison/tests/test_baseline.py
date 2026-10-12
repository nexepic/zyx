from pathlib import Path
import json
import sys
import weakref
from unittest.mock import patch

import pytest

from runner import baseline
from runner import baseline_report
from runner import run
from runner.adapters.base import PROFILE_WORKLOADS, WorkloadResult
from runner.adapters.fake import FakeAdapter
from runner.adapters.zyx import ZyxAdapter
from dataset.generate import generate_graph, write_dataset, SCALES


def test_round_exports_measurements_and_cleans_temporary_data(tmp_path):
    temporary_paths = []
    orders = []
    def fake_run(databases, scale, seed, output_root, warmup, iterations, **kwargs):
        temporary_paths.append(output_root)
        orders.append(databases)
        result = output_root / "result"
        (result / "dataset").mkdir(parents=True)
        (result / "dataset/users.csv").write_text("id\nuser-1\n")
        (result / "summary.csv").write_text("measurements")
        (result / "zyx.db").write_text("database")
        (result / "run_status.json").write_text(json.dumps({"failure_count": 0, "quality_failure_count": 0}))
        return result
    original = PROFILE_WORKLOADS["scan"]
    with patch.object(sys, "argv", ["baseline", "--round", "2", "--scale", "small",
                                   "--output-root", str(tmp_path), "--profile", "scan"]), \
         patch.object(baseline, "run_benchmark", side_effect=fake_run), \
         patch.object(baseline, "verify_counts", return_value={"failures": []}):
        assert baseline.main() == 0
    assert orders == [["kuzu", "zyx"]]
    assert PROFILE_WORKLOADS["scan"] is original
    assert all(not path.exists() for path in temporary_paths)
    exported = tmp_path / "small-round-2/scan"
    assert (exported / "summary.csv").is_file()
    assert (exported / "dataset-hashes.json").is_file()
    assert not (exported / "dataset").exists()
    assert not (exported / "zyx.db").exists()


def test_query_only_profile_does_not_skip_first_workload(tmp_path, monkeypatch):
    dataset = tmp_path / "dataset"
    write_dataset(generate_graph(SCALES["smoke"]), dataset)
    monkeypatch.setitem(PROFILE_WORKLOADS, "scan", ["label_scan_filter", "one_hop_expand"])
    results = FakeAdapter("fake", dataset, "smoke").run_all(1, 2)
    assert [result.workload for result in results] == ["label_scan_filter", "one_hop_expand"]
    assert all(result.status == "ok" and len(result.samples) == 2 for result in results)


@pytest.mark.parametrize("value,valid", [(13, True), (-1, False), (True, False), (1.5, False)])
def test_native_result_events_are_parsed_and_validated(tmp_path, value, valid):
    adapter = ZyxAdapter("zyx", tmp_path, "smoke")
    malformed = []
    adapter._consume_event({"event": "result", "workload": "label_scan_filter", "iteration": 0, "value": value},
                           "label_scan_filter", WorkloadResult("zyx", "label_scan_filter", "smoke", "ok"),
                           [], [], malformed, "stdout", 1)
    assert bool(adapter.result_events) == valid
    assert bool(malformed) != valid


def test_failure_restores_profile_and_cleans_temporary_directory(tmp_path):
    temporary_paths = []
    def fail(*args, **kwargs):
        temporary_paths.append(args[3])
        raise RuntimeError("failed run")
    original = PROFILE_WORKLOADS["scan"]
    with patch.object(sys, "argv", ["baseline", "--round", "1", "--output-root", str(tmp_path),
                                   "--profile", "scan"]), patch.object(baseline, "run_benchmark", side_effect=fail):
        with pytest.raises(RuntimeError, match="failed run"):
            baseline.main()
    assert PROFILE_WORKLOADS["scan"] is original
    assert all(not path.exists() for path in temporary_paths)


def test_independent_counts_reject_wrong_and_missing_observations(tmp_path):
    write_dataset(generate_graph(SCALES["smoke"]), tmp_path / "dataset")
    observations = [{"event": "result", "database": database, "workload": "label_scan_filter",
                     "iteration": 0, "value": 13} for database in ("zyx", "kuzu")]
    raw = tmp_path / "raw.jsonl"
    raw.write_text("\n".join(json.dumps(event) for event in observations))
    assert baseline.verify_counts(tmp_path, ["label_scan_filter"], 1)["failures"] == []
    observations[0]["value"] = 12
    raw.write_text("\n".join(json.dumps(event) for event in observations))
    assert len(baseline.verify_counts(tmp_path, ["label_scan_filter"], 1)["failures"]) == 1
    raw.write_text(json.dumps(observations[1]))
    assert len(baseline.verify_counts(tmp_path, ["label_scan_filter"], 1)["failures"]) == 1


def test_summary_preserves_process_variation_and_rejects_incomplete_rounds(tmp_path):
    for round_id, value in enumerate([1, 100, 2], 1):
        directory = tmp_path / f"medium-round-{round_id}" / "scan"
        directory.mkdir(parents=True)
        (directory / "summary.csv").write_text(
            f"database,workload,status,samples,p50_ms,p95_ms\nzyx,label_scan_filter,ok,10,{value},{value * 2}\n")
    result = baseline_report.summarize(tmp_path)["rows"][0]
    assert result["p50_ms"] == 2
    assert result["process_p50_min_ms"] == 1
    assert result["process_p50_max_ms"] == 100
    assert result["three_rounds_complete"]
    (tmp_path / "medium-round-3/scan/summary.csv").unlink()
    assert not baseline_report.summarize(tmp_path)["rows"][0]["three_rounds_complete"]


def test_metadata_without_repository_handles_shallow_container_path(monkeypatch):
    monkeypatch.setattr(run, "__file__", "/runner.py")
    assert run._git_metadata() == {"git_branch": "unknown", "git_commit": "unknown", "git_dirty": None}


def test_generated_graph_is_released_before_adapter_measurement(tmp_path, monkeypatch):
    references = []
    original_generate = run.generate_graph
    original_adapter = run._adapter_for
    def generate(*args, **kwargs):
        graph = original_generate(*args, **kwargs)
        references.append(weakref.ref(graph))
        return graph
    def adapter(*args, **kwargs):
        assert references[-1]() is None
        return original_adapter(*args, **kwargs)
    monkeypatch.setattr(run, "generate_graph", generate)
    monkeypatch.setattr(run, "_adapter_for", adapter)
    result = run.run_benchmark(["fake"], "smoke", 42, tmp_path, 0, 1, profile="indexed")
    assert json.loads((result / "run_status.json").read_text())["failure_count"] == 0
