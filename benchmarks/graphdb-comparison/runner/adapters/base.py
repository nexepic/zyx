from __future__ import annotations

import csv
import shutil
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

MULTIHOP_DEPTHS = (6, 12, 24, 30)
MULTIHOP_WORKLOADS = (
    ["load_nodes_edges"]
    + [f"reachable_within_{depth}" for depth in MULTIHOP_DEPTHS]
    + ["varlength_frontier_count"]
)
MULTIHOP_FOLLOWS_PER_USER_BY_SCALE = {
    "smoke": 3,
    "small": 5,
    "medium": 5,
}
WRITE_STATEMENT_WORKLOADS = [
    "point_create_node",
    "point_create_edge",
    "point_update_node_property",
    "point_update_edge_property",
    "point_create_delete_edge",
    "write_then_read_edge",
]
WRITE_WORKLOADS = ["load_nodes_edges"] + WRITE_STATEMENT_WORKLOADS
WRITE_DURABLE_WORKLOADS = ["load_nodes_edges"] + [f"{workload}_durable" for workload in WRITE_STATEMENT_WORKLOADS]
OPERATIONAL_DYNAMIC_BATCH_SIZES = {
    "batch_create_edges_100": 100,
    "batch_create_edges_1000": 1000,
    "batch_create_edges_10000": 10000,
}
OPERATIONAL_DYNAMIC_WORKLOADS = [
    "load_nodes_edges",
    "index_seek_then_one_hop_expand",
    "index_seek_then_two_hop_expand",
    "post_persist_create_node",
    "post_persist_create_edge",
    "write_then_one_hop_expand",
    *OPERATIONAL_DYNAMIC_BATCH_SIZES.keys(),
    "batch_create_edges_100_then_one_hop_expand",
    "batch_create_edges_10000_then_one_hop_expand",
]
RETRIEVAL_WORKLOADS = [
    "load_nodes_edges",
    "point_node_fetch_by_id",
    "point_edge_fetch_by_endpoints",
    "batch_node_fetch_100",
    "one_hop_fetch_neighbor_ids",
    "one_hop_fetch_neighbor_records",
    "property_index_fetch_users_by_country",
    "range_index_fetch_user_projection",
    "relationship_property_fetch",
]
WRITE_RESULT_ONE_WORKLOADS = (
    (set(WRITE_WORKLOADS) | set(WRITE_DURABLE_WORKLOADS))
    | {"post_persist_create_node", "post_persist_create_edge"}
) - {"load_nodes_edges"}
MUTATING_WORKLOADS = WRITE_RESULT_ONE_WORKLOADS | set(OPERATIONAL_DYNAMIC_BATCH_SIZES) | {
    "write_then_one_hop_expand",
    "batch_create_edges_100_then_one_hop_expand",
    "batch_create_edges_10000_then_one_hop_expand",
}

PROFILE_WORKLOADS = {
    "scan": SCAN_WORKLOADS,
    "indexed": INDEXED_WORKLOADS,
    "multihop": MULTIHOP_WORKLOADS,
    "write": WRITE_WORKLOADS,
    "write_durable": WRITE_DURABLE_WORKLOADS,
    "operational_dynamic": OPERATIONAL_DYNAMIC_WORKLOADS,
    "retrieval": RETRIEVAL_WORKLOADS,
}
SECONDARY_INDEX_PROFILES = frozenset({"indexed", "operational_dynamic", "retrieval"})

DEFAULT_PROFILE = "scan"
WARM_EXECUTION_MODE = "warm"
OPENED_EXECUTION_MODE = "opened"
COLDISH_EXECUTION_MODE = "cold-ish"
EXECUTION_MODES = (WARM_EXECUTION_MODE, OPENED_EXECUTION_MODE, COLDISH_EXECUTION_MODE)
WORKLOADS = SCAN_WORKLOADS


class UnsupportedWorkload(RuntimeError):
    """Raised when a database cannot execute a workload with the advertised semantics."""


def is_mutating_workload(workload: str) -> bool:
    return workload in MUTATING_WORKLOADS


def isolate_mutating_workload(execution_mode: str, workload: str) -> bool:
    return execution_mode in {WARM_EXECUTION_MODE, OPENED_EXECUTION_MODE} and is_mutating_workload(workload)


def multihop_target_user_id(depth: int, scale: str = "medium") -> str:
    follows_per_user = MULTIHOP_FOLLOWS_PER_USER_BY_SCALE.get(scale, MULTIHOP_FOLLOWS_PER_USER_BY_SCALE["medium"])
    return f"user-{1 + follows_per_user * depth:06d}"


def write_update_target_user_id(scale: str = "medium") -> str:
    return anchored_neighbor_user_id(scale)


def anchored_neighbor_user_id(scale: str = "medium") -> str:
    return "user-000004" if scale == "smoke" else "user-000006"


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
    def __init__(
        self,
        database: str,
        dataset_dir: Path,
        scale: str,
        profile: str = DEFAULT_PROFILE,
        threads: int | None = None,
    ):
        if profile not in PROFILE_WORKLOADS:
            raise ValueError(f"unsupported benchmark profile: {profile}")
        if threads is not None and threads < 0:
            raise ValueError("threads must be >= 0")
        self.database = database
        self.dataset_dir = dataset_dir
        self.scale = scale
        self.profile = profile
        self.threads = threads

    def setup(self) -> None:
        return None

    def teardown(self) -> None:
        return None

    def cleanup_artifacts(self) -> list[Path]:
        return []

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

    def reachable_within_6(self) -> int:
        raise NotImplementedError

    def reachable_within_12(self) -> int:
        raise NotImplementedError

    def reachable_within_24(self) -> int:
        raise NotImplementedError

    def reachable_within_30(self) -> int:
        raise NotImplementedError

    def varlength_frontier_count(self) -> int:
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

    def point_node_fetch_by_id(self) -> int:
        raise NotImplementedError

    def point_edge_fetch_by_endpoints(self) -> int:
        raise NotImplementedError

    def batch_node_fetch_100(self) -> int:
        raise NotImplementedError

    def one_hop_fetch_neighbor_ids(self) -> int:
        raise NotImplementedError

    def one_hop_fetch_neighbor_records(self) -> int:
        raise NotImplementedError

    def property_index_fetch_users_by_country(self) -> int:
        raise NotImplementedError

    def range_index_fetch_user_projection(self) -> int:
        raise NotImplementedError

    def relationship_property_fetch(self) -> int:
        raise NotImplementedError

    def point_create_node(self) -> int:
        raise NotImplementedError

    def point_create_edge(self) -> int:
        raise NotImplementedError

    def point_update_node_property(self) -> int:
        raise NotImplementedError

    def point_update_edge_property(self) -> int:
        raise NotImplementedError

    def point_create_delete_edge(self) -> int:
        raise NotImplementedError

    def write_then_read_edge(self) -> int:
        raise NotImplementedError

    def post_persist_create_node(self) -> int:
        raise NotImplementedError

    def post_persist_create_edge(self) -> int:
        raise NotImplementedError

    def write_then_one_hop_expand(self) -> int:
        raise NotImplementedError

    def index_seek_then_one_hop_expand(self) -> int:
        raise NotImplementedError

    def index_seek_then_two_hop_expand(self) -> int:
        raise NotImplementedError

    def batch_create_edges_100(self) -> int:
        raise NotImplementedError

    def batch_create_edges_1000(self) -> int:
        raise NotImplementedError

    def batch_create_edges_10000(self) -> int:
        raise NotImplementedError

    def batch_create_edges_100_then_one_hop_expand(self) -> int:
        raise NotImplementedError

    def batch_create_edges_10000_then_one_hop_expand(self) -> int:
        raise NotImplementedError

    def durability_barrier(self) -> None:
        return None

    def point_create_node_durable(self) -> int:
        return self._run_durable_write(self.point_create_node)

    def point_create_edge_durable(self) -> int:
        return self._run_durable_write(self.point_create_edge)

    def point_update_node_property_durable(self) -> int:
        return self._run_durable_write(self.point_update_node_property)

    def point_update_edge_property_durable(self) -> int:
        return self._run_durable_write(self.point_update_edge_property)

    def point_create_delete_edge_durable(self) -> int:
        return self._run_durable_write(self.point_create_delete_edge)

    def write_then_read_edge_durable(self) -> int:
        return self._run_durable_write(self.write_then_read_edge)

    def _run_durable_write(self, operation: Callable[[], int]) -> int:
        actual = operation()
        self.durability_barrier()
        return actual

    def validate(self, workload: str, actual: int) -> None:
        if actual < 0:
            raise AssertionError(f"{workload} returned a negative count")
        if workload.startswith("reachable_within_") and actual != 1:
            raise AssertionError(f"{workload} expected a reachable target, got {actual}")
        if workload in WRITE_RESULT_ONE_WORKLOADS and actual != 1:
            raise AssertionError(f"{workload} expected exactly one affected row, got {actual}")
        expected_batch = OPERATIONAL_DYNAMIC_BATCH_SIZES.get(workload)
        if expected_batch is not None and actual != expected_batch:
            raise AssertionError(f"{workload} expected {expected_batch} created edges, got {actual}")

    def run_workload(
        self,
        workload: str,
        warmup: int,
        iterations: int,
        adapter_factory: Callable[[], "BenchmarkAdapter"] | None = None,
        execution_mode: str = WARM_EXECUTION_MODE,
    ) -> WorkloadResult:
        operation: Callable[[], int | None] = getattr(self, workload)
        try:
            if warmup < 0:
                raise ValueError("warmup must be >= 0")
            if iterations <= 0:
                raise ValueError("iterations must be > 0")
            if execution_mode not in EXECUTION_MODES:
                raise ValueError(f"execution mode must be one of: {', '.join(EXECUTION_MODES)}")

            if workload == "load_nodes_edges" and adapter_factory is not None:
                samples = self._run_load_workload(warmup, iterations, adapter_factory)
                return WorkloadResult(self.database, workload, self.scale, "ok", samples=samples)

            if execution_mode == COLDISH_EXECUTION_MODE and workload != "load_nodes_edges" and adapter_factory is not None:
                samples = self._run_coldish_query_workload(workload, warmup, iterations, adapter_factory)
                return WorkloadResult(self.database, workload, self.scale, "ok", samples=samples)

            if isolate_mutating_workload(execution_mode, workload) and adapter_factory is not None:
                return self._run_isolated_loaded_workload(workload, warmup, iterations, adapter_factory, execution_mode)

            query_warmup = 0 if execution_mode == OPENED_EXECUTION_MODE else warmup
            if workload != "load_nodes_edges":
                for _ in range(query_warmup):
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
        except UnsupportedWorkload as exc:
            return WorkloadResult(self.database, workload, self.scale, "unsupported", error=str(exc))
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


    def _run_coldish_query_workload(
        self,
        workload: str,
        warmup: int,
        iterations: int,
        adapter_factory: Callable[[], "BenchmarkAdapter"],
    ) -> list[Sample]:
        for _ in range(warmup):
            self._run_isolated_query(workload, adapter_factory)

        samples: list[Sample] = []
        for iteration in range(iterations):
            latency_ms = self._run_isolated_query(workload, adapter_factory)
            samples.append(self._sample(workload, iteration, latency_ms))
        return samples

    def _run_isolated_query(
        self,
        workload: str,
        adapter_factory: Callable[[], "BenchmarkAdapter"],
    ) -> float:
        adapter = adapter_factory()
        adapter.setup()
        try:
            loaded = adapter.load_nodes_edges()
            if loaded is None:
                raise ValueError("load_nodes_edges returned None")
            adapter.validate("load_nodes_edges", int(loaded))
            operation: Callable[[], int | None] = getattr(adapter, workload)
            start = time.perf_counter()
            actual = operation()
            latency_ms = (time.perf_counter() - start) * 1000.0
            if actual is None:
                raise ValueError(f"{workload} returned None")
            adapter.validate(workload, int(actual))
            return latency_ms
        finally:
            adapter.teardown()

    def _run_isolated_loaded_workload(
        self,
        workload: str,
        warmup: int,
        iterations: int,
        adapter_factory: Callable[[], "BenchmarkAdapter"],
        execution_mode: str,
    ) -> WorkloadResult:
        adapter = adapter_factory()
        adapter.setup()
        try:
            loaded = adapter.load_nodes_edges()
            if loaded is None:
                raise ValueError("load_nodes_edges returned None")
            adapter.validate("load_nodes_edges", int(loaded))
            return adapter.run_workload(workload, warmup, iterations, execution_mode=execution_mode)
        finally:
            adapter.teardown()

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

    def run_all(
        self,
        warmup: int,
        iterations: int,
        execution_mode: str = WARM_EXECUTION_MODE,
    ) -> list[WorkloadResult]:
        if execution_mode not in EXECUTION_MODES:
            return [
                WorkloadResult(
                    self.database,
                    "run_all",
                    self.scale,
                    "failed",
                    error=f"execution mode must be one of: {', '.join(EXECUTION_MODES)}",
                )
            ]

        workloads = PROFILE_WORKLOADS[self.profile]
        load_result = self.run_workload("load_nodes_edges", warmup, iterations, adapter_factory=self._new_adapter)
        if execution_mode == COLDISH_EXECUTION_MODE:
            query_results = [
                self.run_workload(
                    workload, warmup, iterations, adapter_factory=self._new_adapter, execution_mode=execution_mode
                )
                for workload in workloads[1:]
            ]
            return [load_result] + query_results

        shared_adapter_loaded = False
        query_results: list[WorkloadResult] = []
        try:
            for workload in workloads[1:]:
                if isolate_mutating_workload(execution_mode, workload):
                    query_results.append(
                        self.run_workload(
                            workload,
                            warmup,
                            iterations,
                            adapter_factory=self._new_adapter,
                            execution_mode=execution_mode,
                        )
                    )
                    continue

                if not shared_adapter_loaded:
                    self.setup()
                    actual = self.load_nodes_edges()
                    if actual is None:
                        raise ValueError("load_nodes_edges returned None")
                    self.validate("load_nodes_edges", int(actual))
                    shared_adapter_loaded = True

                query_results.append(self.run_workload(workload, warmup, iterations, execution_mode=execution_mode))
            return [load_result] + query_results
        finally:
            if shared_adapter_loaded:
                self.teardown()

    def _new_adapter(self) -> "BenchmarkAdapter":
        if self.threads is None:
            return type(self)(database=self.database, dataset_dir=self.dataset_dir, scale=self.scale, profile=self.profile)
        return type(self)(
            database=self.database,
            dataset_dir=self.dataset_dir,
            scale=self.scale,
            profile=self.profile,
            threads=self.threads,
        )


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def remove_artifact_family(base_path: Path) -> list[Path]:
    removed: list[Path] = []
    if not base_path.parent.exists():
        return removed

    for path in sorted(base_path.parent.glob(f"{base_path.name}*")):
        if path.is_dir():
            shutil.rmtree(path)
        else:
            path.unlink(missing_ok=True)
        removed.append(path)
    return removed
