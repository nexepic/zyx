import pytest

from runner.models import Sample
from runner.stats import percentile, summarize_samples


def test_percentile_uses_nearest_rank():
    values = [10.0, 20.0, 30.0, 40.0]

    assert percentile(values, 0) == 10.0
    assert percentile(values, 50) == 20.0
    assert percentile(values, 90) == 40.0
    assert percentile(values, 100) == 40.0


def test_percentile_rejects_invalid_input():
    with pytest.raises(ValueError, match="values must not be empty"):
        percentile([], 50)
    with pytest.raises(ValueError, match="rank must be between 0 and 100"):
        percentile([1.0], -1)


def test_summarize_samples_computes_latency_and_throughput():
    samples = [
        Sample(
            database="zyx",
            workload="point_lookup",
            scale="smoke",
            iteration=0,
            latency_ms=10.0,
            status="ok",
            equivalent_mode="strict",
        ),
        Sample(
            database="zyx",
            workload="point_lookup",
            scale="smoke",
            iteration=1,
            latency_ms=20.0,
            status="ok",
            equivalent_mode="strict",
        ),
        Sample(
            database="neo4j",
            workload="point_lookup",
            scale="smoke",
            iteration=0,
            latency_ms=30.0,
            status="error",
            equivalent_mode="strict",
        ),
    ]

    rows = summarize_samples(samples)

    assert len(rows) == 2
    first = rows[0]
    assert first.database == "neo4j"
    assert first.workload == "point_lookup"
    assert first.scale == "smoke"
    assert first.samples == 1
    assert first.avg_ms == 30.0
    assert first.p50_ms == 30.0
    assert first.p95_ms == 30.0
    assert first.p99_ms == 30.0
    assert first.ops_per_sec == 1000.0 / 30.0
    assert first.status == "error"
    assert first.equivalent_mode == "strict"

    second = rows[1]
    assert second.database == "zyx"
    assert second.samples == 2
    assert second.avg_ms == 15.0
    assert second.p50_ms == 10.0
    assert second.p95_ms == 20.0
    assert second.p99_ms == 20.0
    assert second.ops_per_sec == 1000.0 * 2 / 30.0
