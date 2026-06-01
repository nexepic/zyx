from __future__ import annotations

import csv
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from runner.models import Sample

SCAN_WORKLOADS = [
    "load_nodes_edges",
    "label_scan_filter",
    "all_nodes_property_filter",
    "label_multi_property_filter",
    "relationship_type_scan",
    "relationship_property_filter",
    "one_hop_expand",
    "two_hop_expand",
    "shortest_path_chain",
    "aggregation_group_by",
    "aggregation_count_by_group",
    "topk_property_sort",
]

INDEXED_WORKLOADS = [
    "load_nodes_edges",
    "point_lookup_indexed",
    "property_equality_indexed",
    "property_range_indexed",
]

PROFILE_WORKLOADS = {
    "scan": SCAN_WORKLOADS,
    "indexed": INDEXED_WORKLOADS,
}

DEFAULT_PROFILE = "scan"
WORKLOADS = SCAN_WORKLOADS


@dataclass(frozen=True)
class WorkloadResult:
    database: str
    workload: str
    scale: str
    status: str
    samples: list[Sample] = field(default_factory=list)
    error: str = ""
    equivalent_mode: str = "cypher"


class BenchmarkAdapter:
    def __init__(self, database: str, dataset_dir: Path, scale: str, profile: str = DEFAULT_PROFILE):
        if profile not in PROFILE_WORKLOADS:
            raise ValueError(f"unsupported benchmark profile: {profile}")
        self.database = database
        self.dataset_dir = dataset_dir
        self.scale = scale
        self.profile = profile

    def setup(self) -> None:
        return None

    def teardown(self) -> None:
        return None

    def load_nodes_edges(self) -> None:
        raise NotImplementedError

    def point_lookup_indexed(self) -> int:
        raise NotImplementedError

    def label_scan_filter(self) -> int:
        raise NotImplementedError

    def all_nodes_property_filter(self) -> int:
        raise NotImplementedError

    def label_multi_property_filter(self) -> int:
        raise NotImplementedError

    def relationship_type_scan(self) -> int:
        raise NotImplementedError

    def relationship_property_filter(self) -> int:
        raise NotImplementedError

    def one_hop_expand(self) -> int:
        raise NotImplementedError

    def two_hop_expand(self) -> int:
        raise NotImplementedError

    def shortest_path_chain(self) -> int:
        raise NotImplementedError

    def aggregation_group_by(self) -> int:
        raise NotImplementedError

    def aggregation_count_by_group(self) -> int:
        raise NotImplementedError

    def topk_property_sort(self) -> int:
        raise NotImplementedError

    def property_equality_indexed(self) -> int:
        raise NotImplementedError

    def property_range_indexed(self) -> int:
        raise NotImplementedError

    def validate(self, workload: str, actual: int) -> None:
        if actual < 0:
            raise AssertionError(f"{workload} returned a negative count")

    def run_workload(
        self,
        workload: str,
        warmup: int,
        iterations: int,
        adapter_factory: Callable[[], "BenchmarkAdapter"] | None = None,
    ) -> WorkloadResult:
        operation: Callable[[], int | None] = getattr(self, workload)
        try:
            if warmup < 0:
                raise ValueError("warmup must be >= 0")
            if iterations <= 0:
                raise ValueError("iterations must be > 0")

            if workload == "load_nodes_edges" and adapter_factory is not None:
                samples = self._run_load_workload(warmup, iterations, adapter_factory)
                return WorkloadResult(self.database, workload, self.scale, "ok", samples=samples)

            if workload != "load_nodes_edges":
                for _ in range(warmup):
                    actual = operation()
                    if actual is None:
                        raise ValueError(f"{workload} returned None")
                    self.validate(workload, int(actual))
            samples: list[Sample] = []
            for iteration in range(iterations):
                start = time.perf_counter()
                actual = operation()
                latency_ms = (time.perf_counter() - start) * 1000.0
                if workload != "load_nodes_edges":
                    if actual is None:
                        raise ValueError(f"{workload} returned None")
                    self.validate(workload, int(actual))
                samples.append(self._sample(workload, iteration, latency_ms))
            return WorkloadResult(self.database, workload, self.scale, "ok", samples=samples)
        except Exception as exc:
            return WorkloadResult(self.database, workload, self.scale, "failed", error=str(exc))

    def _run_load_workload(
        self,
        warmup: int,
        iterations: int,
        adapter_factory: Callable[[], "BenchmarkAdapter"],
    ) -> list[Sample]:
        samples: list[Sample] = []
        for _ in range(warmup):
            self._run_isolated_load(adapter_factory)
        for iteration in range(iterations):
            start = time.perf_counter()
            self._run_isolated_load(adapter_factory)
            latency_ms = (time.perf_counter() - start) * 1000.0
            samples.append(self._sample("load_nodes_edges", iteration, latency_ms))
        return samples

    def _run_isolated_load(self, adapter_factory: Callable[[], "BenchmarkAdapter"]) -> None:
        adapter = adapter_factory()
        adapter.setup()
        try:
            actual = adapter.load_nodes_edges()
            if actual is None:
                raise ValueError("load_nodes_edges returned None")
            adapter.validate("load_nodes_edges", int(actual))
        finally:
            adapter.teardown()

    def _sample(self, workload: str, iteration: int, latency_ms: float) -> Sample:
        return Sample(
            database=self.database,
            workload=workload,
            scale=self.scale,
            iteration=iteration,
            latency_ms=latency_ms,
            status="ok",
            equivalent_mode="cypher",
        )

    def run_all(self, warmup: int, iterations: int) -> list[WorkloadResult]:
        workloads = PROFILE_WORKLOADS[self.profile]
        load_result = self.run_workload("load_nodes_edges", warmup, iterations, adapter_factory=self._new_adapter)
        self.setup()
        try:
            actual = self.load_nodes_edges()
            if actual is None:
                raise ValueError("load_nodes_edges returned None")
            self.validate("load_nodes_edges", int(actual))
            query_results = [self.run_workload(workload, warmup, iterations) for workload in workloads[1:]]
            return [load_result] + query_results
        finally:
            self.teardown()

    def _new_adapter(self) -> "BenchmarkAdapter":
        return type(self)(database=self.database, dataset_dir=self.dataset_dir, scale=self.scale, profile=self.profile)


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))
