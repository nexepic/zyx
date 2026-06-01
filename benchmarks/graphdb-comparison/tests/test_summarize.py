import json
from pathlib import Path

from runner.models import Sample
from runner.summarize import build_comparison_rows, read_samples, write_summary_outputs


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

    outputs = write_summary_outputs(events_path, output_dir, environment={"runner": "pytest", "cpu": "unit"})

    assert outputs["csv"] == output_dir / "summary.csv"
    assert outputs["markdown"] == output_dir / "summary.md"
    assert outputs["comparison_csv"] == output_dir / "comparison.csv"
    assert outputs["comparison_markdown"] == output_dir / "comparison.md"
    assert outputs["environment"] == output_dir / "environment.json"
    assert (output_dir / "summary.csv").exists()
    assert (output_dir / "comparison.csv").exists()
    assert (output_dir / "comparison.md").exists()
    markdown = (output_dir / "summary.md").read_text()
    assert markdown.startswith("# 图数据库性能对比报告")
    assert "| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |" in markdown
    comparison = (output_dir / "comparison.md").read_text()
    assert comparison.startswith("# 横向性能差距分析")
    assert "zyx_vs_best" in comparison
    assert "zyx_first/p50" in comparison
    assert "## Limitations" in markdown
    environment = json.loads((output_dir / "environment.json").read_text())
    assert environment == {"cpu": "unit", "runner": "pytest"}


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
