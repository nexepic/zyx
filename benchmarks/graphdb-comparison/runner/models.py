from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Any


@dataclass(frozen=True)
class Sample:
    database: str
    workload: str
    scale: str
    iteration: int
    latency_ms: float
    status: str = "ok"
    equivalent_mode: str = "cypher"

    def to_event(self) -> dict[str, Any]:
        event = asdict(self)
        event["event"] = "sample"
        return event


@dataclass(frozen=True)
class FailureEvent:
    database: str
    workload: str
    scale: str
    status: str
    error: str
    equivalent_mode: str = "cypher"

    def to_event(self) -> dict[str, Any]:
        event = asdict(self)
        event["event"] = "failure"
        return event


@dataclass(frozen=True)
class SummaryRow:
    database: str
    workload: str
    scale: str
    samples: int
    avg_ms: float
    p50_ms: float
    p95_ms: float
    p99_ms: float
    ops_per_sec: float
    status: str = "ok"
    equivalent_mode: str = "cypher"
