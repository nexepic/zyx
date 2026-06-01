from __future__ import annotations

from collections import defaultdict
from math import ceil
from typing import Iterable, Sequence

from runner.models import Sample, SummaryRow


def percentile(values: Sequence[float], rank: float) -> float:
    if not values:
        raise ValueError("values must not be empty")
    if rank < 0 or rank > 100:
        raise ValueError("rank must be between 0 and 100")

    sorted_values = sorted(values)
    index = max(1, ceil(rank / 100.0 * len(sorted_values))) - 1
    return float(sorted_values[index])


def summarize_samples(samples: Iterable[Sample]) -> list[SummaryRow]:
    groups: dict[tuple[str, str, str, str, str], list[Sample]] = defaultdict(list)
    for sample in samples:
        key = (sample.database, sample.workload, sample.scale, sample.status, sample.equivalent_mode)
        groups[key].append(sample)

    rows: list[SummaryRow] = []
    for key in sorted(groups):
        group = groups[key]
        latencies = [sample.latency_ms for sample in group]
        first_latency_ms = min(group, key=lambda sample: sample.iteration).latency_ms
        sample_count = len(group)
        total_latency_ms = sum(latencies)
        ops_per_sec = 0.0 if total_latency_ms <= 0 else sample_count * 1000.0 / total_latency_ms
        rows.append(
            SummaryRow(
                database=key[0],
                workload=key[1],
                scale=key[2],
                samples=sample_count,
                first_ms=first_latency_ms,
                min_ms=min(latencies),
                avg_ms=total_latency_ms / sample_count,
                p50_ms=percentile(latencies, 50),
                p95_ms=percentile(latencies, 95),
                p99_ms=percentile(latencies, 99),
                max_ms=max(latencies),
                ops_per_sec=ops_per_sec,
                status=key[3],
                equivalent_mode=key[4],
            )
        )
    return rows
