from __future__ import annotations

import json
from pathlib import Path

from dataset.generate import SCALES, generate_graph, write_dataset
from runner.adapters.fake import FakeAdapter
from runner.run import run_benchmark


class NoneWorkloadAdapter(FakeAdapter):
    def point_lookup_indexed(self) -> int | None:
        return None


class SetupFailureAdapter(FakeAdapter):
    def setup(self) -> None:
        raise RuntimeError("setup exploded")


def test_fake_adapter_runs_all_common_workloads(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    adapter = FakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke")
    results = adapter.run_all(warmup=1, iterations=3)

    names = {result.workload for result in results}
    assert names == {
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
    }
    for result in results:
        assert result.status == "ok"
        assert len(result.samples) == 3


def test_fake_adapter_uses_scan_profile_workloads(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    adapter = FakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke", profile="scan")
    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.workload for result in results] == [
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
    assert [result.status for result in results] == ["ok"] * 12


def test_fake_adapter_uses_indexed_profile_workloads(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    adapter = FakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke", profile="indexed")
    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.workload for result in results] == [
        "load_nodes_edges",
        "point_lookup_indexed",
        "property_equality_indexed",
        "property_range_indexed",
    ]
    assert [result.status for result in results] == ["ok"] * 4


def test_fake_adapter_uses_multihop_profile_workloads(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    adapter = FakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke", profile="multihop")
    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.workload for result in results] == [
        "load_nodes_edges",
        "reachable_within_6",
        "reachable_within_12",
        "reachable_within_24",
        "reachable_within_30",
        "varlength_frontier_count",
    ]
    assert [result.status for result in results] == ["ok"] * 6


def test_fake_adapter_uses_write_profile_workloads(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    adapter = FakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke", profile="write")
    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.workload for result in results] == [
        "load_nodes_edges",
        "point_create_node",
        "point_create_edge",
        "point_update_node_property",
        "point_update_edge_property",
        "point_create_delete_edge",
        "write_then_read_edge",
    ]
    assert [result.status for result in results] == ["ok"] * 7


def test_fake_adapter_uses_write_durable_profile_workloads(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    adapter = FakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke", profile="write_durable")
    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.workload for result in results] == [
        "load_nodes_edges",
        "point_create_node_durable",
        "point_create_edge_durable",
        "point_update_node_property_durable",
        "point_update_edge_property_durable",
        "point_create_delete_edge_durable",
        "write_then_read_edge_durable",
    ]
    assert [result.status for result in results] == ["ok"] * 7


def test_fake_adapter_uses_operational_dynamic_profile_workloads(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    adapter = FakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke", profile="operational_dynamic")
    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.workload for result in results] == [
        "load_nodes_edges",
        "index_seek_then_one_hop_expand",
        "index_seek_then_two_hop_expand",
        "post_persist_create_node",
        "post_persist_create_edge",
        "write_then_one_hop_expand",
        "batch_create_edges_100",
        "batch_create_edges_1000",
        "batch_create_edges_10000",
        "batch_create_edges_100_then_one_hop_expand",
        "batch_create_edges_10000_then_one_hop_expand",
    ]
    assert [result.status for result in results] == ["ok"] * 11


def test_write_profile_validation_requires_single_affected_row(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    class BrokenWriteAdapter(FakeAdapter):
        def point_create_node(self) -> int:
            return 2

    adapter = BrokenWriteAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke", profile="write")
    adapter.setup()
    adapter.load_nodes_edges()

    result = adapter.run_workload("point_create_node", warmup=0, iterations=1)

    assert result.status == "failed"
    assert "expected exactly one affected row" in result.error


def test_batch_operational_dynamic_validation_requires_expected_edge_count(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    class BrokenBatchAdapter(FakeAdapter):
        def batch_create_edges_100(self) -> int:
            return 99

    adapter = BrokenBatchAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke", profile="operational_dynamic")
    adapter.setup()
    adapter.load_nodes_edges()

    result = adapter.run_workload("batch_create_edges_100", warmup=0, iterations=1)

    assert result.status == "failed"
    assert "batch_create_edges_100 expected 100 created edges" in result.error


def test_shortest_path_chain_uses_loaded_edges(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    adapter = FakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke")
    adapter.setup()
    assert adapter.shortest_path_chain() == 1

    adapter.follows = []
    assert adapter.shortest_path_chain() == 0


def test_reachable_within_validation_requires_expected_target(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    adapter = FakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke", profile="multihop")
    adapter.setup()
    adapter.load_nodes_edges()
    adapter.follows = []

    result = adapter.run_workload("reachable_within_6", warmup=0, iterations=1)

    assert result.status == "failed"
    assert "reachable_within_6 expected a reachable target" in result.error


def test_run_workload_rejects_invalid_iteration_config(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    adapter = FakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke")
    adapter.setup()

    negative_warmup = adapter.run_workload("point_lookup_indexed", warmup=-1, iterations=1)
    assert negative_warmup.status == "failed"
    assert "warmup must be >= 0" in negative_warmup.error

    zero_iterations = adapter.run_workload("point_lookup_indexed", warmup=0, iterations=0)
    assert zero_iterations.status == "failed"
    assert "iterations must be > 0" in zero_iterations.error


def test_run_workload_fails_when_non_load_workload_returns_none(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    adapter = NoneWorkloadAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke")
    adapter.setup()

    result = adapter.run_workload("point_lookup_indexed", warmup=0, iterations=1)

    assert result.status == "failed"
    assert "point_lookup_indexed returned None" in result.error


def test_load_workload_uses_fresh_adapter_for_each_sample(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)
    created: list[FakeAdapter] = []

    def factory() -> FakeAdapter:
        adapter = FakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke")
        original = adapter.load_nodes_edges

        def measured_load() -> int:
            count = original()
            assert adapter.users
            return count

        adapter.load_nodes_edges = measured_load  # type: ignore[method-assign]
        created.append(adapter)
        return adapter

    parent = FakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke")
    result = parent.run_workload("load_nodes_edges", warmup=1, iterations=2, adapter_factory=factory)

    assert result.status == "ok"
    assert len(result.samples) == 2
    assert len(created) == 3
    assert all(adapter.users == [] for adapter in created)



def test_coldish_execution_mode_uses_fresh_loaded_adapter_for_queries(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    class ObservedFakeAdapter(FakeAdapter):
        setup_count = 0
        teardown_count = 0
        query_loaded_flags: list[bool] = []

        def setup(self) -> None:
            type(self).setup_count += 1
            super().setup()

        def teardown(self) -> None:
            type(self).teardown_count += 1
            super().teardown()

        def label_scan_filter(self) -> int:
            type(self).query_loaded_flags.append(bool(self.users))
            return super().label_scan_filter()

    adapter = ObservedFakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke")

    result = adapter.run_workload(
        "label_scan_filter",
        warmup=1,
        iterations=2,
        adapter_factory=lambda: ObservedFakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke"),
        execution_mode="cold-ish",
    )

    assert result.status == "ok"
    assert len(result.samples) == 2
    assert ObservedFakeAdapter.setup_count == 3
    assert ObservedFakeAdapter.teardown_count == 3
    assert ObservedFakeAdapter.query_loaded_flags == [True, True, True]


def test_opened_execution_mode_skips_explicit_query_warmup(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    class ObservedFakeAdapter(FakeAdapter):
        query_calls = 0

        def label_scan_filter(self) -> int:
            type(self).query_calls += 1
            return super().label_scan_filter()

    adapter = ObservedFakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke")
    adapter.setup()
    adapter.load_nodes_edges()

    result = adapter.run_workload("label_scan_filter", warmup=5, iterations=2, execution_mode="opened")

    assert result.status == "ok"
    assert len(result.samples) == 2
    assert ObservedFakeAdapter.query_calls == 2


def test_run_all_preloads_query_adapter_before_timing(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    class ObservedFakeAdapter(FakeAdapter):
        events: list[tuple[str, bool]] = []

        def load_nodes_edges(self) -> int:
            self.events.append(("load", bool(self.users)))
            return super().load_nodes_edges()

        def label_scan_filter(self) -> int:
            self.events.append(("label", bool(self.users)))
            return super().label_scan_filter()

    adapter = ObservedFakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke")
    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.status for result in results] == ["ok"] * 12
    assert ("label", True) in ObservedFakeAdapter.events


def test_run_all_isolates_mutating_workloads_per_workload(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    graph = generate_graph(SCALES["smoke"], seed=42)
    write_dataset(graph, dataset_dir)

    class ObservedFakeAdapter(FakeAdapter):
        next_adapter_id = 0
        operation_adapter_ids: dict[str, int] = {}

        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            type(self).next_adapter_id += 1
            self.observed_adapter_id = type(self).next_adapter_id

        def point_create_node(self) -> int:
            type(self).operation_adapter_ids["point_create_node"] = self.observed_adapter_id
            return super().point_create_node()

        def point_create_edge(self) -> int:
            type(self).operation_adapter_ids["point_create_edge"] = self.observed_adapter_id
            return super().point_create_edge()

    adapter = ObservedFakeAdapter(database="fake", dataset_dir=dataset_dir, scale="smoke", profile="write")
    results = adapter.run_all(warmup=0, iterations=1)

    assert [result.status for result in results] == ["ok"] * 7
    assert ObservedFakeAdapter.operation_adapter_ids["point_create_node"] != ObservedFakeAdapter.operation_adapter_ids[
        "point_create_edge"
    ]
    assert adapter.users == []
    assert adapter.follows == []


def _read_jsonl(path: Path) -> list[dict[str, object]]:
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def test_run_benchmark_writes_raw_and_summary(tmp_path: Path):
    iterations = 2
    output_root = tmp_path / "results"
    result_dir = run_benchmark(
        databases=["fake"],
        scale="smoke",
        seed=42,
        output_root=output_root,
        warmup=1,
        iterations=iterations,
        execution_mode="cold-ish",
    )

    assert (result_dir / "raw.jsonl").exists()
    assert (result_dir / "summary.csv").exists()
    assert (result_dir / "summary.md").exists()
    assert (result_dir / "quality_gates.json").exists()
    assert (result_dir / "run_status.json").exists()
    assert (output_root / "latest.txt").read_text().strip() == result_dir.name

    events = _read_jsonl(result_dir / "raw.jsonl")
    assert sum(1 for event in events if event["event"] == "sample") == 12 * iterations
    environment = events[0]
    assert environment["event"] == "environment"
    assert environment["seed"] == 42
    assert environment["scale"] == "smoke"
    assert environment["execution_mode"] == "cold-ish"
    assert environment["threads"] is None
    assert environment["thread_contract"] == "adapter default thread configuration"
    assert environment["benchmark_schema_version"] == 1
    assert environment["keep_db_artifacts"] is False
    assert environment["measurement_semantics"]["execution_mode"] == "cold-ish"
    assert "does not flush OS" in environment["measurement_semantics"]["execution_contract"]
    assert "workload_isolation" in environment["measurement_semantics"]
    assert environment["required_workloads"][0] == "load_nodes_edges"
    assert environment["mutating_workloads"] == []
    assert "git_commit" in environment
    assert "label_scan_filter" in (result_dir / "summary.csv").read_text()
    quality = json.loads((result_dir / "quality_gates.json").read_text())
    assert quality["status"] == "passed"


def test_run_benchmark_records_and_forwards_thread_count(tmp_path: Path, monkeypatch):
    import runner.run as run_module

    observed_threads: list[int | None] = []

    class ThreadObservedFakeAdapter(FakeAdapter):
        def __init__(
            self,
            database: str,
            dataset_dir: Path,
            scale: str,
            profile: str,
            threads: int | None = None,
        ):
            super().__init__(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile, threads=threads)
            observed_threads.append(threads)

    monkeypatch.setattr(
        run_module,
        "_adapter_for",
        lambda database, dataset_dir, scale, profile, threads=None: ThreadObservedFakeAdapter(
            database, dataset_dir, scale, profile, threads
        ),
    )

    result_dir = run_benchmark(
        databases=["fake"],
        scale="smoke",
        seed=42,
        output_root=tmp_path / "results",
        warmup=0,
        iterations=1,
        threads=4,
    )

    environment = json.loads((result_dir / "environment.json").read_text())
    assert environment["threads"] == 4
    assert environment["thread_contract"] == "fixed 4 execution thread(s)"
    assert observed_threads[0] == 4


def test_run_benchmark_cleans_adapter_artifacts_by_default(tmp_path: Path, monkeypatch):
    import runner.run as run_module

    class ArtifactFakeAdapter(FakeAdapter):
        def __init__(
            self,
            database: str,
            dataset_dir: Path,
            scale: str,
            profile: str,
            threads: int | None = None,
        ):
            super().__init__(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile, threads=threads)
            self.artifact = dataset_dir.parent / "fake.db"

        def run_all(self, warmup: int, iterations: int, execution_mode: str = "warm"):
            self.artifact.write_text("temporary benchmark database")
            return super().run_all(warmup=warmup, iterations=iterations, execution_mode=execution_mode)

        def cleanup_artifacts(self) -> list[Path]:
            if not self.artifact.exists():
                return []
            self.artifact.unlink()
            return [self.artifact]

    monkeypatch.setattr(
        run_module,
        "_adapter_for",
        lambda database, dataset_dir, scale, profile, threads=None: ArtifactFakeAdapter(
            database, dataset_dir, scale, profile, threads
        ),
    )

    result_dir = run_benchmark(
        databases=["fake"],
        scale="smoke",
        seed=42,
        output_root=tmp_path / "results",
        warmup=0,
        iterations=1,
    )

    assert not (result_dir / "fake.db").exists()
    cleanup_events = [event for event in _read_jsonl(result_dir / "raw.jsonl") if event["event"] == "artifact_cleanup"]
    assert cleanup_events == [
        {
            "event": "artifact_cleanup",
            "database": "fake",
            "scale": "smoke",
            "removed": [str(result_dir / "fake.db")],
        }
    ]


def test_run_benchmark_can_keep_adapter_artifacts_for_debugging(tmp_path: Path, monkeypatch):
    import runner.run as run_module

    class ArtifactFakeAdapter(FakeAdapter):
        def __init__(
            self,
            database: str,
            dataset_dir: Path,
            scale: str,
            profile: str,
            threads: int | None = None,
        ):
            super().__init__(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile, threads=threads)
            self.artifact = dataset_dir.parent / "fake.db"

        def run_all(self, warmup: int, iterations: int, execution_mode: str = "warm"):
            self.artifact.write_text("temporary benchmark database")
            return super().run_all(warmup=warmup, iterations=iterations, execution_mode=execution_mode)

        def cleanup_artifacts(self) -> list[Path]:
            self.artifact.unlink()
            return [self.artifact]

    monkeypatch.setattr(
        run_module,
        "_adapter_for",
        lambda database, dataset_dir, scale, profile, threads=None: ArtifactFakeAdapter(
            database, dataset_dir, scale, profile, threads
        ),
    )

    result_dir = run_benchmark(
        databases=["fake"],
        scale="smoke",
        seed=42,
        output_root=tmp_path / "results",
        warmup=0,
        iterations=1,
        keep_db_artifacts=True,
    )

    assert (result_dir / "fake.db").read_text() == "temporary benchmark database"
    environment = json.loads((result_dir / "environment.json").read_text())
    assert environment["keep_db_artifacts"] is True


def test_run_benchmark_records_profile_and_uses_profile_workloads(tmp_path: Path):
    output_root = tmp_path / "results"

    result_dir = run_benchmark(
        databases=["fake"],
        scale="smoke",
        seed=42,
        output_root=output_root,
        warmup=0,
        iterations=1,
        profile="indexed",
    )

    environment = _read_jsonl(result_dir / "raw.jsonl")[0]
    sample_workloads = [
        event["workload"]
        for event in _read_jsonl(result_dir / "raw.jsonl")
        if event.get("event") == "sample"
    ]
    assert environment["profile"] == "indexed"
    assert environment["measurement_semantics"]["profile"] == "indexed"
    assert "result_cache" not in environment
    assert sample_workloads == [
        "load_nodes_edges",
        "point_lookup_indexed",
        "property_equality_indexed",
        "property_range_indexed",
    ]


def test_run_benchmark_records_write_durable_semantics(tmp_path: Path):
    result_dir = run_benchmark(
        databases=["fake"],
        scale="smoke",
        seed=42,
        output_root=tmp_path / "results",
        warmup=0,
        iterations=1,
        profile="write_durable",
        execution_mode="opened",
    )

    environment = json.loads((result_dir / "environment.json").read_text())
    semantics = environment["measurement_semantics"]
    assert semantics["execution_mode"] == "opened"
    assert semantics["profile"] == "write_durable"
    assert semantics["profile_contract"]["latency_contract"] == "durable_after_each_write_latency"
    assert semantics["profile_contract"]["barrier_by_database"]["kuzu"] == "CHECKPOINT after each measured write."
    assert environment["mutating_workloads"] == [
        "point_create_node_durable",
        "point_create_edge_durable",
        "point_update_node_property_durable",
        "point_update_edge_property_durable",
        "point_create_delete_edge_durable",
        "write_then_read_edge_durable",
    ]


def test_run_benchmark_writes_baseline_regression_gate(tmp_path: Path):
    baseline_dir = tmp_path / "baseline"
    baseline_dir.mkdir()
    (baseline_dir / "summary.csv").write_text(
        "database,workload,scale,samples,first_ms,min_ms,avg_ms,p50_ms,p95_ms,p99_ms,max_ms,ops_per_sec,status,equivalent_mode\n"
        "fake,load_nodes_edges,smoke,1,100,100,100,100,100,100,100,10,ok,cypher\n"
    )

    result_dir = run_benchmark(
        databases=["fake"],
        scale="smoke",
        seed=42,
        output_root=tmp_path / "results",
        warmup=0,
        iterations=1,
        baseline_summary=baseline_dir / "summary.csv",
        max_regression_ratio=10.0,
    )

    environment = json.loads((result_dir / "environment.json").read_text())
    quality = json.loads((result_dir / "quality_gates.json").read_text())
    regression_gate = next(gate for gate in quality["gates"] if gate["name"] == "p50_regression_against_baseline")
    assert environment["baseline_summary"].endswith("summary.csv")
    assert environment["max_regression_ratio"] == 10.0
    assert regression_gate["status"] == "passed"


def test_run_benchmark_writes_zyx_profiles_jsonl(tmp_path: Path, monkeypatch):
    import runner.run as run_module
    from runner.adapters.base import WorkloadResult
    from runner.models import ProfileEvent, Sample

    class ProfiledZyxAdapter:
        def __init__(
            self,
            database: str,
            dataset_dir: Path,
            scale: str,
            profile: str,
            threads: int | None = None,
        ):
            self.threads = threads
            self.profile_events = [
                ProfileEvent(
                    database="zyx",
                    workload="load_nodes_edges",
                    scale=scale,
                    profile=profile,
                    iteration=0,
                    phase="parse",
                    total_time_ms=0.5,
                    calls=1,
                    equivalent_mode="api",
                )
            ]

        def run_all(self, warmup: int, iterations: int, execution_mode: str = "warm") -> list[WorkloadResult]:
            return [
                WorkloadResult(
                    database="zyx",
                    workload="load_nodes_edges",
                    scale="smoke",
                    status="ok",
                    samples=[
                        Sample(
                            database="zyx",
                            workload="load_nodes_edges",
                            scale="smoke",
                            iteration=0,
                            latency_ms=1.0,
                            equivalent_mode="api",
                        )
                    ],
                    equivalent_mode="api",
                )
            ]

    def fake_adapter_for(database: str, dataset_dir: Path, scale: str, profile: str, threads: int | None = None):
        assert database == "zyx"
        return ProfiledZyxAdapter(database, dataset_dir, scale, profile, threads)

    monkeypatch.setattr(run_module, "_adapter_for", fake_adapter_for)

    result_dir = run_benchmark(
        databases=["zyx"],
        scale="smoke",
        seed=42,
        output_root=tmp_path / "results",
        warmup=0,
        iterations=1,
        profile="scan",
    )

    profile_path = result_dir / "zyx_profiles.jsonl"
    assert profile_path.exists()
    events = _read_jsonl(profile_path)
    assert events == [
        {
            "database": "zyx",
            "equivalent_mode": "api",
            "event": "profile",
            "scale": "smoke",
            "profile": "scan",
            "workload": "load_nodes_edges",
            "iteration": 0,
            "phase": "parse",
            "total_time_ms": 0.5,
            "calls": 1,
        }
    ]


def test_run_benchmark_result_dirs_do_not_collide(tmp_path: Path):
    output_root = tmp_path / "results"

    first = run_benchmark(["fake"], "smoke", 42, output_root, warmup=0, iterations=1)
    second = run_benchmark(["fake"], "smoke", 42, output_root, warmup=0, iterations=1)

    assert first != second
    assert first.exists()
    assert second.exists()


def test_run_benchmark_records_missing_adapter_failures(tmp_path: Path):
    result_dir = run_benchmark(
        databases=["zyx"],
        scale="smoke",
        seed=42,
        output_root=tmp_path / "results",
        warmup=0,
        iterations=1,
    )

    raw_events = _read_jsonl(result_dir / "raw.jsonl")
    error_events = _read_jsonl(result_dir / "errors.jsonl")
    status = json.loads((result_dir / "run_status.json").read_text())
    summary = (result_dir / "summary.csv").read_text()

    assert any(event["event"] == "failure" and event["database"] == "zyx" for event in raw_events)
    assert any(event["event"] == "error" and event["database"] == "zyx" for event in error_events)
    assert status["failure_count"] == 1
    assert status["quality_failure_count"] > 0
    assert "failed" in summary
    assert "run_all" in summary


def test_run_benchmark_records_run_all_exceptions(tmp_path: Path, monkeypatch):
    def failing_adapter(
        database: str, dataset_dir: Path, scale: str, profile: str, threads: int | None = None
    ) -> SetupFailureAdapter:
        return SetupFailureAdapter(database=database, dataset_dir=dataset_dir, scale=scale, profile=profile)

    monkeypatch.setattr("runner.run._adapter_for", failing_adapter)

    result_dir = run_benchmark(
        databases=["fake"],
        scale="smoke",
        seed=42,
        output_root=tmp_path / "results",
        warmup=0,
        iterations=1,
    )

    raw_events = _read_jsonl(result_dir / "raw.jsonl")
    error_events = _read_jsonl(result_dir / "errors.jsonl")
    status = json.loads((result_dir / "run_status.json").read_text())
    summary = (result_dir / "summary.csv").read_text()

    assert any(event["event"] == "failure" and event["workload"] == "run_all" for event in raw_events)
    assert any(event["event"] == "error" and event["workload"] == "run_all" for event in error_events)
    assert status["failure_count"] == 1
    assert status["quality_failure_count"] > 0
    assert "failed" in summary
    assert "run_all" in summary
