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
class ProfileEvent:
    database: str
    workload: str
    scale: str
    profile: str
    iteration: int
    phase: str
    total_time_ms: float
    calls: int
    equivalent_mode: str = "api"
    value_total: int | None = None
    value_calls: int | None = None
    value_avg: float | None = None

    def to_event(self) -> dict[str, Any]:
        event = {key: value for key, value in asdict(self).items() if value is not None}
        event["event"] = "profile"
        return event


@dataclass(frozen=True)
class ProfileSummaryRow:
    database: str
    workload: str
    scale: str
    profile: str
    phase: str
    samples: int
    total_calls: int
    avg_calls: float
    total_value: int
    total_value_calls: int
    avg_value: float
    first_ms: float
    min_ms: float
    avg_ms: float
    p50_ms: float
    p95_ms: float
    p99_ms: float
    max_ms: float
    equivalent_mode: str = "api"


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
    first_ms: float
    min_ms: float
    avg_ms: float
    p50_ms: float
    p95_ms: float
    p99_ms: float
    max_ms: float
    ops_per_sec: float
    status: str = "ok"
    equivalent_mode: str = "cypher"
