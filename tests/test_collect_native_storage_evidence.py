import hashlib
import importlib.util
import json
from pathlib import Path

import pytest


def _load_collector():
    source_root = Path(__file__).resolve().parents[1]
    script_path = source_root / "scripts" / "collect_native_storage_evidence.py"
    spec = importlib.util.spec_from_file_location("zyx_collect_native_storage_evidence", script_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _vector_report(dataset_label="frozen-dataset-v1", hardware_label="linux-x86_64", passed=True):
    return {
        "schema_version": 2,
        "dataset": {"label": dataset_label, "bytes": 42, "crc32": 123},
        "hardware_label": hardware_label,
        "gate": {"enforced": True, "passed": passed, "thresholds": {"limit": 1}, "violations": []},
    }


def _large_value_report(*, passed=True):
    return {
        "schema_version": 1,
        "dataset": {"label": "large-value-smoke-v1", "generated": True, "unique_values": 2,
                    "value_bytes": 8, "logical_bytes": 16},
        "hardware_label": "linux-x86_64",
        "storage": {"single_file": True, "database_bytes": 128, "file_growth_bytes": 64,
                    "space_amplification": 8.0},
        "unique_write": {"seconds": 0.1, "values_per_second": 20.0, "submitted_write_bytes": 32,
                         "submitted_write_amplification": 2.0},
        "dedup": {"attempts": 2, "hits": 2, "misses": 0, "hit_rate": 1.0,
                  "candidates_scanned": 2, "seconds": 0.01},
        "range_read": {"operations": 2, "bytes": 8, "seconds": 0.01, "bytes_per_second": 800.0,
                       "submitted_read_bytes": 16, "latency_us": {"p50": 1.0, "p95": 2.0, "p99": 3.0}},
        "peak_rss_bytes": 1024,
        "gate": {"enforced": True, "passed": passed, "violations": []},
    }


def _bulk_edge_report(*, passed=True):
    return {
        "schema_version": 3,
        "dataset": {"label": "bulk-edge-smoke-v1", "generated": True, "edge_count": 8},
        "hardware_label": "linux-x86_64",
        "storage": {"single_file": True, "database_bytes": 4096, "bytes_per_edge": 512.0, "sidecar_files": []},
        "ingest": {"staging_ms": 1.0, "commit_ms": 2.0, "edges_per_second": 8000.0 / 3.0},
        "reopen_scan": {"reopen_ms": 1.0, "scan_ms": 2.0, "close_ms": 1.0, "total_ms": 4.0},
        "endpoint_setup_ms": 1.0,
        "peak_rss_bytes": 1024,
        "scratch": {"candidate_segments": 8, "candidate_capacity_segments": 8, "candidate_limit": 16, "token_candidates": 1,
                    "token_resolver_candidates": 1, "outgoing_adjacency_candidates": 3,
                    "incoming_adjacency_candidates": 3, "large_value_content_candidates": 0,
                    "source_run_count": 1, "merge_passes": 0},
        "trace": {"enabled": False, "maximum_phases": 16, "commit": [], "scan": []},
        "gate": {"enforced": True, "passed": passed,
                 "thresholds": {"maximum_scratch_candidate_segments": 16,
                                "minimum_scratch_source_runs": 1,
                                "minimum_scratch_merge_passes": 0}, "violations": []},
    }


def _write_report(path, report):
    path.write_text(json.dumps(report), encoding="utf-8")


def _evidence_inputs(tmp_path, *, include_endurance=True):
    tmp_path.mkdir(parents=True, exist_ok=True)
    native_lifecycle = tmp_path / "native-lifecycle.log"
    recovery_maintenance = tmp_path / "recovery-maintenance.log"
    vector_smoke_data = tmp_path / "vector-calibration.zyx"
    large_value_smoke_data = tmp_path / "large-value-calibration.zyx"
    bulk_edge_smoke_data = tmp_path / "bulk-edge-calibration.zyx"
    native_lifecycle.write_text("native lifecycle passed\n", encoding="utf-8")
    recovery_maintenance.write_text("recovery maintenance passed\n", encoding="utf-8")
    vector_smoke_data.write_bytes(b"native vector smoke database")
    large_value_smoke_data.write_bytes(b"native LargeValue smoke database")
    bulk_edge_smoke_data.write_bytes(b"native bulk-edge smoke database")
    inputs = {
        "native_lifecycle_log_path": native_lifecycle,
        "recovery_maintenance_log_path": recovery_maintenance,
        "vector_smoke_data_path": vector_smoke_data,
        "large_value_smoke_data_path": large_value_smoke_data,
        "bulk_edge_smoke_data_path": bulk_edge_smoke_data,
    }
    if include_endurance:
        endurance = tmp_path / "endurance.log"
        endurance.write_text("endurance passed\n", encoding="utf-8")
        inputs["endurance_log_path"] = endurance
    return inputs


def _reports(tmp_path):
    build = tmp_path / "build.json"
    query = tmp_path / "query.json"
    large_value = tmp_path / "large-value.json"
    bulk_edge = tmp_path / "bulk-edge.json"
    _write_report(build, _vector_report())
    _write_report(query, _vector_report())
    _write_report(large_value, _large_value_report())
    _write_report(bulk_edge, _bulk_edge_report())
    return build, query, large_value, bulk_edge


def test_collects_checksums_and_explicit_ci_smoke_limitations(tmp_path, monkeypatch):
    collector = _load_collector()
    build, query, large_value, bulk_edge = _reports(tmp_path)
    output = tmp_path / "evidence" / "manifest.json"
    monkeypatch.setenv("RUNNER_OS", "Linux")
    monkeypatch.setenv("GITHUB_RUN_ID", "123")

    evidence_inputs = _evidence_inputs(tmp_path)
    manifest = collector.collect(build, query, large_value, bulk_edge, output, 5, 1000, **evidence_inputs)

    assert output.exists()
    assert manifest["evidence_classification"] == "ci-smoke"
    assert manifest["executed_repetitions"] == {"recovery_maintenance": 5, "manual_endurance": 1000}
    assert manifest["ci_environment"] == {"GITHUB_RUN_ID": "123", "RUNNER_OS": "Linux"}
    assert len(manifest["calibration"]["vector"]["build"]["sha256"]) == 64
    assert manifest["calibration"]["vector"]["build"]["gate"]["passed"] is True
    assert manifest["calibration"]["large_value"]["storage"]["single_file"] is True
    assert manifest["calibration"]["large_value"]["dedup"]["hits"] == 2
    assert manifest["calibration"]["bulk_edge"]["storage"]["single_file"] is True
    assert manifest["calibration"]["bulk_edge"]["ingest"]["edges_per_second"] == 8000.0 / 3.0
    assert manifest["calibration"]["bulk_edge"]["scratch"]["candidate_segments"] == 8
    assert manifest["evidence_files"]["native_lifecycle_log"]["sha256"] == hashlib.sha256(
        b"native lifecycle passed\n"
    ).hexdigest()
    assert manifest["evidence_files"]["large_value_smoke_data_file"]["bytes"] == len(
        b"native LargeValue smoke database"
    )
    assert manifest["evidence_files"]["bulk_edge_smoke_data_file"]["bytes"] == len(
        b"native bulk-edge smoke database"
    )
    assert manifest["evidence_files"]["endurance_log"]["bytes"] == len(b"endurance passed\n")
    assert "not production performance approval" in manifest["limitations"][0]


def test_rejects_incompatible_or_unpassed_vector_reports_before_writing(tmp_path):
    collector = _load_collector()
    build, query, large_value, bulk_edge = _reports(tmp_path)
    output = tmp_path / "manifest.json"
    _write_report(query, _vector_report(dataset_label="different"))
    evidence_inputs = _evidence_inputs(tmp_path, include_endurance=False)

    with pytest.raises(ValueError, match="same frozen dataset label"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    _write_report(query, _vector_report(passed=False))
    with pytest.raises(ValueError, match="passed enforced gate"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()


def test_rejects_inconsistent_or_unpassed_large_value_reports_before_writing(tmp_path):
    collector = _load_collector()
    build, query, large_value, bulk_edge = _reports(tmp_path)
    output = tmp_path / "manifest.json"
    evidence_inputs = _evidence_inputs(tmp_path, include_endurance=False)

    invalid = _large_value_report()
    invalid["storage"]["single_file"] = False
    _write_report(large_value, invalid)
    with pytest.raises(ValueError, match="one database file"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    invalid = _large_value_report()
    invalid["dedup"]["misses"] = 1
    _write_report(large_value, invalid)
    with pytest.raises(ValueError, match="internally inconsistent"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    invalid = _large_value_report()
    invalid["range_read"]["latency_us"]["p95"] = float("inf")
    _write_report(large_value, invalid)
    with pytest.raises(ValueError, match="non-negative number"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    _write_report(large_value, _large_value_report(passed=False))
    with pytest.raises(ValueError, match="passed enforced gate"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()


def test_rejects_inconsistent_or_unpassed_bulk_edge_reports_before_writing(tmp_path):
    collector = _load_collector()
    build, query, large_value, bulk_edge = _reports(tmp_path)
    output = tmp_path / "manifest.json"
    evidence_inputs = _evidence_inputs(tmp_path, include_endurance=False)

    invalid = _bulk_edge_report()
    invalid["storage"]["single_file"] = False
    _write_report(bulk_edge, invalid)
    with pytest.raises(ValueError, match="one database file"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    invalid = _bulk_edge_report()
    invalid["trace"]["commit"] = [{"name": "commit", "milliseconds": 1.0}] * 17
    _write_report(bulk_edge, invalid)
    with pytest.raises(ValueError, match="exceed their declared maximum"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    invalid = _bulk_edge_report()
    invalid["storage"]["bytes_per_edge"] = 1.0
    _write_report(bulk_edge, invalid)
    with pytest.raises(ValueError, match="bytes per Edge is internally inconsistent"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    invalid = _bulk_edge_report()
    invalid["storage"]["sidecar_files"] = ["bulk-edge.zyx-wal"]
    _write_report(bulk_edge, invalid)
    with pytest.raises(ValueError, match="must not name WAL/SHM sidecars"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    invalid = _bulk_edge_report()
    invalid["scratch"]["candidate_segments"] = 17
    _write_report(bulk_edge, invalid)
    with pytest.raises(ValueError, match="scratch candidate breakdown is internally inconsistent"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    invalid = _bulk_edge_report()
    invalid["scratch"]["outgoing_adjacency_candidates"] = 2
    _write_report(bulk_edge, invalid)
    with pytest.raises(ValueError, match="scratch candidate breakdown is internally inconsistent"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    invalid = _bulk_edge_report()
    invalid["gate"]["thresholds"]["minimum_scratch_source_runs"] = 2
    _write_report(bulk_edge, invalid)
    with pytest.raises(ValueError, match="source-run gate is internally inconsistent"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    invalid = _bulk_edge_report()
    invalid["scratch"]["merge_passes"] = 0
    invalid["gate"]["thresholds"]["minimum_scratch_merge_passes"] = 1
    _write_report(bulk_edge, invalid)
    with pytest.raises(ValueError, match="merge-pass gate is internally inconsistent"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    invalid = _bulk_edge_report()
    invalid["ingest"]["edges_per_second"] = 1.0
    _write_report(bulk_edge, invalid)
    with pytest.raises(ValueError, match="throughput is internally inconsistent"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()

    _write_report(bulk_edge, _bulk_edge_report(passed=False))
    with pytest.raises(ValueError, match="passed enforced gate"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    assert not output.exists()


def test_refuses_to_overwrite_existing_evidence_and_rejects_bad_repeat_counts(tmp_path):
    collector = _load_collector()
    build, query, large_value, bulk_edge = _reports(tmp_path)
    output = tmp_path / "manifest.json"
    evidence_inputs = _evidence_inputs(tmp_path, include_endurance=False)

    with pytest.raises(ValueError, match="recovery repetitions"):
        collector.collect(build, query, large_value, bulk_edge, output, 0, 0, **evidence_inputs)
    collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)
    with pytest.raises(ValueError, match="will not be overwritten"):
        collector.collect(build, query, large_value, bulk_edge, output, 5, 0, **evidence_inputs)


def test_requires_a_matching_nonempty_endurance_transcript(tmp_path):
    collector = _load_collector()
    build, query, large_value, bulk_edge = _reports(tmp_path)

    without_endurance = _evidence_inputs(tmp_path, include_endurance=False)
    with pytest.raises(ValueError, match="endurance transcript is required"):
        collector.collect(build, query, large_value, bulk_edge, tmp_path / "missing-endurance.json", 5, 1, **without_endurance)

    with_endurance = _evidence_inputs(tmp_path / "with-endurance")
    with pytest.raises(ValueError, match="invalid when manual endurance repetitions are zero"):
        collector.collect(build, query, large_value, bulk_edge, tmp_path / "unexpected-endurance.json", 5, 0, **with_endurance)

    with_endurance["endurance_log_path"].write_text("", encoding="utf-8")
    with pytest.raises(ValueError, match="endurance transcript must not be empty"):
        collector.collect(build, query, large_value, bulk_edge, tmp_path / "empty-endurance.json", 5, 1, **with_endurance)


def test_rejects_missing_or_empty_required_artifacts_before_writing(tmp_path):
    collector = _load_collector()
    build, query, large_value, bulk_edge = _reports(tmp_path)

    evidence_inputs = _evidence_inputs(tmp_path, include_endurance=False)
    evidence_inputs["native_lifecycle_log_path"].write_text("", encoding="utf-8")
    with pytest.raises(ValueError, match="native lifecycle transcript must not be empty"):
        collector.collect(build, query, large_value, bulk_edge, tmp_path / "empty-lifecycle.json", 5, 0, **evidence_inputs)

    evidence_inputs = _evidence_inputs(tmp_path / "missing-data", include_endurance=False)
    evidence_inputs["large_value_smoke_data_path"].unlink()
    with pytest.raises(ValueError, match="could not stat LargeValue smoke database data file"):
        collector.collect(build, query, large_value, bulk_edge, tmp_path / "missing-data.json", 5, 0, **evidence_inputs)
