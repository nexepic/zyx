import json
from pathlib import Path

from runner.models import Sample
from runner.summarize import read_samples, write_summary_outputs


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
    assert outputs["environment"] == output_dir / "environment.json"
    assert (output_dir / "summary.csv").exists()
    markdown = (output_dir / "summary.md").read_text()
    assert markdown.startswith("# 图数据库性能对比报告")
    assert "| database | workload | scale | samples | avg_ms | p50_ms | p95_ms | p99_ms | ops_per_sec | status | equivalent_mode |" in markdown
    assert "## Limitations" in markdown
    environment = json.loads((output_dir / "environment.json").read_text())
    assert environment == {"cpu": "unit", "runner": "pytest"}
