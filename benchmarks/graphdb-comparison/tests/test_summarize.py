import json
from pathlib import Path

from runner.models import ProfileEvent, Sample, SummaryRow
from runner.summarize import (
    build_comparison_rows,
    build_operation_summary_rows,
    build_phase_category_summary_rows,
    build_profile_summary_rows,
    build_quality_gate_report,
    read_samples,
    write_summary_outputs,
)


def test_read_samples_ignores_non_sample_events(tmp_path: Path):
    events_path = tmp_path / "events.jsonl"
    events = [
        {"event": "start", "database": "zyx"},
        Sample(
            database="zyx",
            workload="scan",
            scale="smoke",
            iteration=0,
            latency_ms=12.5,
            status="ok",
            equivalent_mode="relaxed",
        ).to_event(),
        {"event": "finish", "database": "zyx"},
    ]
    events_path.write_text("".join(json.dumps(event) + "\n" for event in events))

    samples = read_samples(events_path)

    assert samples == [
        Sample(
            database="zyx",
            workload="scan",
            scale="smoke",
            iteration=0,
            latency_ms=12.5,
            status="ok",
            equivalent_mode="relaxed",
        )
    ]


def test_write_summary_outputs_creates_csv_markdown_and_environment(tmp_path: Path):
    events_path = tmp_path / "events.jsonl"
    profiles_path = tmp_path / "profiles.jsonl"
    output_dir = tmp_path / "summary"
    events = [
        Sample(
            database="zyx",
            workload="scan",
            scale="smoke",
            iteration=0,
            latency_ms=10.0,
            status="ok",
            equivalent_mode="strict",
        ).to_event(),
        Sample(
            database="kuzu",
            workload="scan",
            scale="smoke",
            iteration=0,
            latency_ms=20.0,
            status="ok",
            equivalent_mode="strict",
        ).to_event(),
    ]
    events_path.write_text("".join(json.dumps(event) + "\n" for event in events))
    profiles = [
        ProfileEvent("zyx", "scan", "smoke", "scan", 0, "warm.operation", 10.0, 1).to_event(),
        ProfileEvent("zyx", "scan", "smoke", "scan", 1, "warm.operation", 12.0, 1).to_event(),
    ]
    profiles_path.write_text("".join(json.dumps(event) + "\n" for event in profiles))

    outputs = write_summary_outputs(
        events_path,
        output_dir,
        environment={"runner": "pytest", "cpu": "unit"},
        profile_events_path=profiles_path,
    )

    assert outputs["csv"] == output_dir / "summary.csv"
    assert outputs["markdown"] == output_dir / "summary.md"
    assert outputs["comparison_csv"] == output_dir / "comparison.csv"
    assert outputs["comparison_markdown"] == output_dir / "comparison.md"
    assert outputs["profile_summary_csv"] == output_dir / "profile_summary.csv"
    assert outputs["profile_summary_markdown"] == output_dir / "profile_summary.md"
    assert outputs["operation_summary_csv"] == output_dir / "operation_summary.csv"
    assert outputs["operation_summary_markdown"] == output_dir / "operation_summary.md"
    assert outputs["phase_category_summary_csv"] == output_dir / "phase_category_summary.csv"
    assert outputs["phase_category_summary_markdown"] == output_dir / "phase_category_summary.md"
    assert outputs["environment"] == output_dir / "environment.json"
    assert outputs["quality"] == output_dir / "quality_gates.json"
    assert (output_dir / "summary.csv").exists()
    assert (output_dir / "comparison.csv").exists()
    assert (output_dir / "comparison.md").exists()
    assert (output_dir / "profile_summary.csv").exists()
    assert (output_dir / "profile_summary.md").exists()
    assert (output_dir / "operation_summary.csv").exists()
    assert (output_dir / "operation_summary.md").exists()
    assert (output_dir / "phase_category_summary.csv").exists()
    assert (output_dir / "phase_category_summary.md").exists()
    assert (output_dir / "quality_gates.json").exists()
    markdown = (output_dir / "summary.md").read_text()
    assert markdown.startswith("# 图数据库性能对比报告")
    assert "| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |" in markdown
    comparison = (output_dir / "comparison.md").read_text()
    assert comparison.startswith("# 横向性能差距分析")
    assert "zyx_vs_best" in comparison
    assert "zyx_first/p50" in comparison
    profile_summary = (output_dir / "profile_summary.md").read_text()
    assert profile_summary.startswith("# Benchmark Profile Breakdown")
    assert "warm.operation" in profile_summary
    operation_summary = (output_dir / "operation_summary.md").read_text()
    assert operation_summary.startswith("# Operation Type Summary")
    assert "other" in operation_summary
    phase_summary = (output_dir / "phase_category_summary.md").read_text()
    assert phase_summary.startswith("# Measurement Phase Summary")
    assert "statement_latency" in phase_summary
    assert "## Limitations" in markdown
    environment = json.loads((output_dir / "environment.json").read_text())
    assert environment == {"cpu": "unit", "runner": "pytest"}
    quality = json.loads((output_dir / "quality_gates.json").read_text())
    assert quality["status"] == "passed"


def test_build_profile_summary_rows_aggregates_phase_timings_and_calls():
    rows = build_profile_summary_rows(
        [
            ProfileEvent("zyx", "scan", "small", "scan", 0, "node_scan.count", 3.0, 2),
            ProfileEvent("zyx", "scan", "small", "scan", 1, "node_scan.count", 1.0, 4),
            ProfileEvent("zyx", "scan", "small", "scan", 2, "node_scan.count", 5.0, 6),
            ProfileEvent("zyx", "scan", "small", "scan", 0, "node_scan.load", 7.0, 1),
        ]
    )

    count_row = next(row for row in rows if row.phase == "node_scan.count")
    assert count_row.samples == 3
    assert count_row.total_calls == 12
    assert count_row.avg_calls == 4.0
    assert count_row.first_ms == 3.0
    assert count_row.min_ms == 1.0
    assert count_row.avg_ms == 3.0
    assert count_row.p50_ms == 3.0
    assert count_row.p95_ms == 5.0
    assert count_row.max_ms == 5.0


def test_operation_and_phase_category_summaries_make_benchmark_semantics_explicit():
    rows = [
        SummaryRow("zyx", "one_hop_expand", "small", 2, 1.0, 1.0, 1.5, 1.5, 2.0, 2.0, 2.0, 100.0),
        SummaryRow("zyx", "point_create_node", "small", 2, 3.0, 3.0, 3.5, 3.5, 4.0, 4.0, 4.0, 50.0),
        SummaryRow("kuzu", "batch_create_edges_100", "small", 1, 5.0, 5.0, 5.0, 5.0, 5.0, 5.0, 5.0, 20.0),
    ]

    operation_rows = build_operation_summary_rows(rows)

    assert next(row for row in operation_rows if row["database"] == "zyx" and row["operation_type"] == "adjacency expand")[
        "workload_count"
    ] == 1
    assert next(row for row in operation_rows if row["database"] == "zyx" and row["operation_type"] == "point write")[
        "geomean_p50_ms"
    ] == 3.5
    assert next(row for row in operation_rows if row["database"] == "kuzu")["operation_type"] == "batch write"

    phase_rows = build_phase_category_summary_rows(
        [
            ProfileEvent("zyx", "point_create_node", "small", "write", 0, "warm.operation", 10.0, 1),
            ProfileEvent("zyx", "point_create_node", "small", "write", 0, "wal.commit_sync", 2.0, 1),
            ProfileEvent("zyx", "point_create_node", "small", "write", 0, "db.flush.wal_checkpoint", 3.0, 1),
            ProfileEvent("zyx", "point_create_node", "small", "write", 0, "save.total", 5.0, 1),
            ProfileEvent("zyx", "load_nodes_edges", "small", "write", 0, "load.graph", 4.0, 1),
        ]
    )

    phases = {row["semantic_phase"]: row for row in phase_rows}
    assert phases["statement_latency"]["p50_ms"] == 10.0
    assert phases["durable_wal_fsync"]["raw_phases"] == "wal.commit_sync"
    assert phases["close_checkpoint"]["p50_ms"] == 8.0
    assert phases["load_import"]["raw_phases"] == "load.graph"


def test_build_comparison_rows_ranks_zyx_against_best():
    rows = [
        Sample("zyx", "scan", "smoke", 0, 10.0, equivalent_mode="api"),
        Sample("zyx", "scan", "smoke", 1, 20.0, equivalent_mode="api"),
        Sample("kuzu", "scan", "smoke", 0, 5.0),
        Sample("kuzu", "scan", "smoke", 1, 6.0),
        Sample("neo4j", "scan", "smoke", 0, 30.0),
        Sample("neo4j", "scan", "smoke", 1, 40.0),
    ]

    from runner.stats import summarize_samples

    comparison = build_comparison_rows(summarize_samples(rows))

    assert len(comparison) == 1
    row = comparison[0]
    assert row["workload"] == "scan"
    assert row["best_database"] == "kuzu"
    assert row["zyx_rank_by_p50"] == 2
    assert row["zyx_vs_best_p50"] == 2.0
    assert row["zyx_first_ms"] == 10.0
    assert row["zyx_first_to_p50"] == 1.0
    assert row["fastest_non_zyx_database"] == "kuzu"


def test_build_quality_gate_report_validates_benchmark_schema_and_profiles():
    from runner.stats import summarize_samples

    samples = [
        Sample("zyx", "load_nodes_edges", "smoke", 0, 10.0, equivalent_mode="api"),
        Sample("zyx", "label_scan_filter", "smoke", 0, 1.0, equivalent_mode="api"),
    ]
    rows = summarize_samples(samples)
    environment = {
        "benchmark_schema_version": 1,
        "databases": ["zyx"],
        "execution_mode": "cold-ish",
        "git_commit": "abc123",
        "git_dirty": False,
        "iterations": 1,
        "profile": "scan",
        "required_workloads": ["load_nodes_edges", "label_scan_filter"],
        "scale": "smoke",
        "seed": 42,
        "warmup": 0,
    }
    profiles = [
        ProfileEvent("zyx", "load_nodes_edges", "smoke", "scan", 0, "save.total", 0.5, 1),
        ProfileEvent("zyx", "label_scan_filter", "smoke", "scan", 0, "node_scan.count", 0.2, 1),
    ]

    report = build_quality_gate_report(rows, environment, profiles)

    assert report["status"] == "passed"
    assert report["failure_count"] == 0


def test_build_quality_gate_report_allows_explicit_unsupported_non_primary_workloads():
    from runner.stats import summarize_samples

    rows = summarize_samples([Sample("zyx", "property_equality_indexed", "smoke", 0, 1.0, equivalent_mode="api")])
    rows.append(
        SummaryRow(
            database="kuzu",
            workload="property_equality_indexed",
            scale="smoke",
            samples=0,
            first_ms=0.0,
            min_ms=0.0,
            avg_ms=0.0,
            p50_ms=0.0,
            p95_ms=0.0,
            p99_ms=0.0,
            max_ms=0.0,
            ops_per_sec=0.0,
            status="unsupported",
        )
    )
    environment = {
        "benchmark_schema_version": 1,
        "databases": ["zyx", "kuzu"],
        "execution_mode": "warm",
        "git_commit": "abc123",
        "git_dirty": False,
        "iterations": 1,
        "profile": "indexed",
        "required_workloads": ["property_equality_indexed"],
        "scale": "smoke",
        "seed": 42,
        "warmup": 0,
    }
    profiles = [ProfileEvent("zyx", "property_equality_indexed", "smoke", "indexed", 0, "index.lookup", 0.1, 1)]

    report = build_quality_gate_report(rows, environment, profiles)

    assert report["status"] == "passed"
    assert "unsupported_workloads_declared" in {gate["name"] for gate in report["gates"] if gate["status"] == "passed"}


def test_build_quality_gate_report_fails_missing_samples_and_profiles():
    from runner.stats import summarize_samples

    rows = summarize_samples([Sample("zyx", "load_nodes_edges", "smoke", 0, 10.0, equivalent_mode="api")])
    environment = {
        "benchmark_schema_version": 1,
        "databases": ["zyx"],
        "execution_mode": "cold-ish",
        "git_commit": "abc123",
        "git_dirty": False,
        "iterations": 2,
        "profile": "scan",
        "required_workloads": ["load_nodes_edges", "label_scan_filter"],
        "scale": "smoke",
        "seed": 42,
        "warmup": 0,
    }

    report = build_quality_gate_report(rows, environment, [])

    assert report["status"] == "failed"
    failed_gate_names = {gate["name"] for gate in report["gates"] if gate["status"] == "failed"}
    assert "expected_sample_counts" in failed_gate_names
    assert "zyx_primary_rows_present" in failed_gate_names
    assert "zyx_profile_coverage" in failed_gate_names


def test_build_quality_gate_report_detects_p50_regression_against_baseline():
    from runner.stats import summarize_samples

    current_rows = summarize_samples(
        [
            Sample("zyx", "label_scan_filter", "smoke", 0, 12.0, equivalent_mode="api"),
            Sample("zyx", "label_scan_filter", "smoke", 1, 12.0, equivalent_mode="api"),
        ]
    )
    baseline_rows = summarize_samples(
        [
            Sample("zyx", "label_scan_filter", "smoke", 0, 10.0, equivalent_mode="api"),
            Sample("zyx", "label_scan_filter", "smoke", 1, 10.0, equivalent_mode="api"),
        ]
    )

    report = build_quality_gate_report(
        current_rows,
        {
            "databases": ["zyx"],
            "required_workloads": ["label_scan_filter"],
            "iterations": 2,
        },
        [ProfileEvent("zyx", "label_scan_filter", "smoke", "scan", 0, "node_scan.count", 0.1, 1)],
        baseline_rows,
        max_regression_ratio=1.10,
    )

    failed_gate_names = {gate["name"] for gate in report["gates"] if gate["status"] == "failed"}
    assert "p50_regression_against_baseline" in failed_gate_names
