from __future__ import annotations

import sys
import types
from pathlib import Path
import pytest
import runner.adapters.kuzu as kuzu_adapter
from runner.adapters.base import UnsupportedWorkload
from runner.adapters.kuzu import KuzuAdapter, _single_quoted_path


def test_kuzu_adapter_constructs_without_kuzu_dependency(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"

    adapter = KuzuAdapter(database="kuzu", dataset_dir=dataset_dir, scale="smoke")

    assert adapter.database == "kuzu"
    assert adapter.dataset_dir == dataset_dir
    assert adapter.scale == "smoke"
    assert adapter.db_path == tmp_path / "kuzu.db"


def test_kuzu_adapter_reads_manifest_row_count(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    dataset_dir.mkdir()
    (dataset_dir / "manifest.json").write_text(
        '{"counts":{"users":2,"posts":3,"tags":5,"follows":7,"authored":11,"has_tag":13}}\n'
    )
    adapter = KuzuAdapter(database="kuzu", dataset_dir=dataset_dir, scale="smoke")

    assert adapter._manifest_row_count() == 41


class FakeConnection:
    def __init__(self, database: object | None = None, num_threads: int | None = None):
        self.database = database
        self.num_threads = num_threads
        self.queries: list[str] = []

    def execute(self, query: str) -> list[list[int]]:
        self.queries.append(query)
        return [[1]]


class BatchConnection(FakeConnection):
    def execute(self, query: str) -> list[list[int]]:
        self.queries.append(query)
        if "range(" in query and "RETURN COUNT(*)" in query:
            bounds = query.split("range(", 1)[1].split(")", 1)[0].split(",")
            first = int(bounds[0].strip())
            last = int(bounds[1].strip())
            return [[last - first + 1]]
        return [[1]]


class GetNextResult:
    def get_next(self) -> dict[str, int]:
        return {"value": 7}


class FakeIloc:
    def __getitem__(self, key: tuple[int, int]) -> int:
        assert key == (0, 0)
        return 11


class FakeDataFrame:
    empty = False
    iloc = FakeIloc()


class DataFrameResult:
    def get_as_df(self) -> FakeDataFrame:
        return FakeDataFrame()


class QueryRowsWithoutNumpyResult:
    def __init__(self, rows: list[list[str]]):
        self.rows = rows
        self.index = 0
        self.get_as_df_called = False

    def get_as_df(self) -> object:
        self.get_as_df_called = True
        raise ModuleNotFoundError("No module named 'numpy'")

    def has_next(self) -> bool:
        return self.index < len(self.rows)

    def get_next(self) -> list[str]:
        row = self.rows[self.index]
        self.index += 1
        return row


class QueryRowsConnection:
    def __init__(self, result: QueryRowsWithoutNumpyResult):
        self.result = result
        self.queries: list[str] = []

    def execute(self, query: str) -> QueryRowsWithoutNumpyResult:
        self.queries.append(query)
        return self.result


class UnsupportedIndexConnection(FakeConnection):
    def execute(self, query: str) -> list[list[int]]:
        self.queries.append(query)
        if query.startswith("CREATE INDEX"):
            raise RuntimeError("Parser exception: Invalid input <CREATE INDEX user_country>")
        return [[1]]


class BrokenIndexConnection(FakeConnection):
    def execute(self, query: str) -> list[list[int]]:
        self.queries.append(query)
        if query.startswith("CREATE INDEX"):
            raise RuntimeError("I/O error while executing CREATE INDEX")
        return [[1]]


def test_kuzu_setup_removes_existing_file_db_path(tmp_path: Path, monkeypatch):
    dataset_dir = tmp_path / "dataset"
    dataset_dir.mkdir()
    db_path = tmp_path / "kuzu.db"
    db_path.write_text("stale file")

    fake_kuzu = types.SimpleNamespace(Database=lambda path: {"path": path}, Connection=FakeConnection)
    monkeypatch.setitem(sys.modules, "kuzu", fake_kuzu)

    adapter = KuzuAdapter(database="kuzu", dataset_dir=dataset_dir, scale="smoke")
    adapter.setup()

    assert not db_path.exists()
    assert isinstance(adapter._connection, FakeConnection)
    assert adapter._connection.queries[0].startswith("CREATE NODE TABLE User")


def test_kuzu_setup_forwards_thread_count_to_database_and_connection(tmp_path: Path, monkeypatch):
    dataset_dir = tmp_path / "dataset"
    dataset_dir.mkdir()
    created_databases: list[dict[str, object]] = []
    created_connections: list[FakeConnection] = []

    class FakeDatabase:
        def __init__(self, path: str, max_num_threads: int = 0):
            self.path = path
            self.max_num_threads = max_num_threads
            created_databases.append({"path": path, "max_num_threads": max_num_threads})

    class RecordingConnection(FakeConnection):
        def __init__(self, database: object | None = None, num_threads: int | None = None):
            super().__init__(database, num_threads)
            created_connections.append(self)

    fake_kuzu = types.SimpleNamespace(Database=FakeDatabase, Connection=RecordingConnection)
    monkeypatch.setitem(sys.modules, "kuzu", fake_kuzu)

    adapter = KuzuAdapter(database="kuzu", dataset_dir=dataset_dir, scale="smoke", threads=4)
    adapter.setup()

    assert created_databases == [{"path": str(tmp_path / "kuzu.db"), "max_num_threads": 4}]
    assert created_connections[0].num_threads == 4


def test_kuzu_adapter_cleans_database_file_and_wal(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    dataset_dir.mkdir()
    adapter = KuzuAdapter(database="kuzu", dataset_dir=dataset_dir, scale="smoke")
    db_path = tmp_path / "kuzu.db"
    wal_path = tmp_path / "kuzu.db.wal"
    unrelated = tmp_path / "kuzu-not-a-db"
    db_path.write_text("db")
    wal_path.write_text("wal")
    unrelated.write_text("keep")

    removed = adapter.cleanup_artifacts()

    assert removed == [db_path, wal_path]
    assert not db_path.exists()
    assert not wal_path.exists()
    assert unrelated.read_text() == "keep"


def test_kuzu_copy_statements_use_escaped_paths(tmp_path: Path):
    dataset_dir = tmp_path / r"data\\root's dataset"
    dataset_dir.mkdir()
    (dataset_dir / "manifest.json").write_text(
        '{"counts":{"users":1,"posts":1,"tags":1,"follows":1,"authored":1,"has_tag":1}}\n'
    )
    adapter = KuzuAdapter(database="kuzu", dataset_dir=dataset_dir, scale="smoke")
    connection = FakeConnection()
    adapter._connection = connection

    assert adapter.load_nodes_edges() == 6

    copy_queries = [query for query in connection.queries if query.startswith("COPY ")]
    assert len(copy_queries) == 6
    assert copy_queries[0] == f"COPY User FROM {_single_quoted_path(dataset_dir / 'users.csv')} (HEADER=true)"
    assert "\\\\" in copy_queries[0]
    assert "\\'" in copy_queries[0]


def test_kuzu_shortest_path_chain_normalizes_reachability_to_zero_or_one(tmp_path: Path):
    adapter = KuzuAdapter(database="kuzu", dataset_dir=tmp_path / "dataset", scale="smoke")
    connection = FakeConnection()
    adapter._connection = connection
    adapter._loaded_rows = 1

    assert adapter.shortest_path_chain() == 1

    query = connection.queries[-1]
    assert "MATCH p =" in query
    assert "[:FOLLOWS*1..6]" in query
    assert "RETURN CASE WHEN count(p) > 0 THEN 1 ELSE 0 END" in query


def test_kuzu_reachable_within_uses_bounded_existence_query(tmp_path: Path):
    adapter = KuzuAdapter(database="kuzu", dataset_dir=tmp_path / "dataset", scale="smoke")
    connection = FakeConnection()
    adapter._connection = connection
    adapter._loaded_rows = 1

    assert adapter.reachable_within_24() == 1

    query = connection.queries[-1]
    assert "MATCH p =" in query
    assert "[:FOLLOWS*1..24]" in query
    assert "user-000073" in query
    assert "RETURN 1 LIMIT 1" in query


def test_kuzu_first_value_handles_representative_result_shapes(tmp_path: Path):
    adapter = KuzuAdapter(database="kuzu", dataset_dir=tmp_path / "dataset", scale="smoke")

    assert adapter._first_value(None) == 0
    assert adapter._first_value(GetNextResult()) == 7
    assert adapter._first_value(DataFrameResult()) == 11
    assert adapter._first_value([[13]]) == 13
    assert adapter._first_value({"value": 17}) == 17
    assert adapter._first_value((19,)) == 19


def test_kuzu_row_count_uses_cursor_rows_without_numpy_dataframe(tmp_path: Path):
    result = QueryRowsWithoutNumpyResult([["user-000001"], ["user-000002"], ["user-000003"]])
    adapter = KuzuAdapter(database="kuzu", dataset_dir=tmp_path / "dataset", scale="smoke")
    adapter._connection = QueryRowsConnection(result)

    assert adapter._row_count("MATCH (u:User) RETURN u.id ORDER BY u.score DESC LIMIT 100") == 3
    assert not result.get_as_df_called


def test_kuzu_diagnostic_workload_methods_issue_expected_queries(tmp_path: Path):
    adapter = KuzuAdapter(database="kuzu", dataset_dir=tmp_path / "dataset", scale="smoke", profile="scan")
    connection = FakeConnection()
    adapter._connection = connection
    adapter._loaded_rows = 1

    assert adapter.all_nodes_property_filter() == 1
    assert adapter.label_multi_property_filter() == 1
    assert adapter.relationship_type_scan() == 1
    assert adapter.relationship_property_filter() == 1
    assert adapter.aggregation_group_by() == 1
    assert adapter.aggregation_count_by_group() == 1
    assert adapter.topk_property_sort() == 1

    assert "MATCH (n) WHERE n.score >= 900 RETURN COUNT(n)" in connection.queries
    assert "MATCH (u:User) WHERE u.country = 'CN' AND u.age >= 30 RETURN COUNT(u)" in connection.queries
    assert "MATCH ()-[r:FOLLOWS]->() RETURN COUNT(r)" in connection.queries
    assert "MATCH ()-[r:FOLLOWS]->() WHERE r.weight = 1 RETURN COUNT(r)" in connection.queries
    assert "MATCH (u:User) RETURN COUNT(DISTINCT u.country)" in connection.queries
    assert "MATCH (u:User) RETURN u.country, COUNT(*)" in connection.queries
    assert "MATCH (u:User) RETURN u.id ORDER BY u.score DESC LIMIT 100" in connection.queries


def test_kuzu_indexed_workload_methods_issue_expected_queries(tmp_path: Path, monkeypatch):
    adapter = KuzuAdapter(database="kuzu", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")
    connection = FakeConnection()
    adapter._connection = connection
    adapter._loaded_rows = 1
    monkeypatch.setattr(kuzu_adapter, "_SECONDARY_PROPERTY_INDEX_SUPPORT", True)

    assert adapter.property_equality_indexed() == 1
    assert adapter.property_range_indexed() == 1

    assert "MATCH (u:User) WHERE u.country = 'CN' RETURN COUNT(u)" in connection.queries
    assert "MATCH (u:User) WHERE u.age >= 30 AND u.age < 40 RETURN COUNT(u)" in connection.queries


def test_kuzu_write_profile_methods_issue_expected_queries(tmp_path: Path):
    adapter = KuzuAdapter(database="kuzu", dataset_dir=tmp_path / "dataset", scale="smoke", profile="write")
    connection = FakeConnection()
    adapter._connection = connection
    adapter._loaded_rows = 1

    assert adapter.point_create_node() == 1
    assert adapter.point_create_edge() == 1
    assert adapter.point_update_node_property() == 1
    assert adapter.point_update_edge_property() == 1
    assert adapter.point_create_delete_edge() == 1
    assert adapter.write_then_read_edge() == 1

    assert any("CREATE (u:User" in query and "bench-user-" in query for query in connection.queries)
    assert any("CREATE (src)-[:FOLLOWS {weight: 1}]->(dst)" in query for query in connection.queries)
    assert any("SET u.score" in query and "RETURN COUNT(u)" in query for query in connection.queries)
    assert any("user-000004" in query and "SET r.weight" in query for query in connection.queries)
    assert any("DELETE r RETURN 1" in query for query in connection.queries)
    assert any("user-000007" in query and "RETURN COUNT(r)" in query for query in connection.queries)


def test_kuzu_operational_dynamic_profile_persists_base_and_issues_expected_queries(tmp_path: Path, monkeypatch):
    dataset_dir = tmp_path / "dataset"
    dataset_dir.mkdir()
    (dataset_dir / "manifest.json").write_text(
        '{"counts":{"users":1,"posts":1,"tags":1,"follows":1,"authored":1,"has_tag":1}}\n'
    )
    monkeypatch.setattr(kuzu_adapter, "supports_secondary_property_indexes", lambda: False)
    adapter = KuzuAdapter(database="kuzu", dataset_dir=dataset_dir, scale="smoke", profile="operational_dynamic")
    connection = BatchConnection()
    adapter._connection = connection
    reopen_calls = 0

    def reopen() -> None:
        nonlocal reopen_calls
        reopen_calls += 1

    adapter._reopen_existing = reopen  # type: ignore[method-assign]

    assert adapter.load_nodes_edges() == 6
    assert reopen_calls == 1
    assert "CHECKPOINT" in connection.queries

    with pytest.raises(UnsupportedWorkload):
        adapter.index_seek_then_one_hop_expand()
    with pytest.raises(UnsupportedWorkload):
        adapter.index_seek_then_two_hop_expand()
    assert adapter.post_persist_create_node() == 1
    assert adapter.post_persist_create_edge() == 1
    assert adapter.write_then_one_hop_expand() == 1
    assert adapter.batch_create_edges_100() == 100
    assert adapter.batch_create_edges_1000() == 1000
    assert adapter.batch_create_edges_10000() == 10000
    assert adapter.batch_create_edges_100_then_one_hop_expand() == 1
    assert adapter.batch_create_edges_10000_then_one_hop_expand() == 1

    assert any("CREATE (u:User" in query and "bench-user-" in query for query in connection.queries)
    assert any("CREATE (src)-[:FOLLOWS {weight: 1}]->(dst)" in query for query in connection.queries)
    assert any("RETURN COUNT(v)" in query and "user-000007" in query for query in connection.queries)
    assert any("RETURN COUNT(v)" in query and "user-000008" in query for query in connection.queries)
    assert sum(1 for query in connection.queries if "UNWIND range(" in query and "RETURN COUNT(*)" in query) == 5


def test_kuzu_operational_dynamic_runs_index_expand_when_secondary_indexes_are_supported(tmp_path: Path, monkeypatch):
    monkeypatch.setattr(kuzu_adapter, "supports_secondary_property_indexes", lambda: True)
    adapter = KuzuAdapter(database="kuzu", dataset_dir=tmp_path / "dataset", scale="smoke", profile="operational_dynamic")
    connection = FakeConnection()
    adapter._connection = connection
    adapter._loaded_rows = 1

    assert adapter.index_seek_then_one_hop_expand() == 1
    assert adapter.index_seek_then_two_hop_expand() == 1

    assert "MATCH (u:User {country: 'CN'})-[:FOLLOWS]->(v:User) RETURN COUNT(v)" in connection.queries
    assert (
        "MATCH (u:User {country: 'CN'})-[:FOLLOWS]->(:User)-[:FOLLOWS]->(v:User) RETURN COUNT(v)"
        in connection.queries
    )


def test_kuzu_write_durable_profile_runs_checkpoint_barrier(tmp_path: Path):
    adapter = KuzuAdapter(database="kuzu", dataset_dir=tmp_path / "dataset", scale="smoke", profile="write_durable")
    connection = FakeConnection()
    adapter._connection = connection
    adapter._loaded_rows = 1
    adapter._reopen_existing = lambda: None  # type: ignore[method-assign]

    assert adapter.point_create_node_durable() == 1
    assert adapter.point_create_edge_durable() == 1
    assert adapter.point_update_node_property_durable() == 1
    assert adapter.point_update_edge_property_durable() == 1
    assert adapter.point_create_delete_edge_durable() == 1
    assert adapter.write_then_read_edge_durable() == 1

    assert connection.queries.count("CHECKPOINT") == 6


def test_kuzu_indexed_profile_adds_property_indexes_when_supported(tmp_path: Path, monkeypatch):
    dataset_dir = tmp_path / "dataset"
    dataset_dir.mkdir()
    fake_kuzu = types.SimpleNamespace(Database=lambda path: {"path": path}, Connection=FakeConnection)
    monkeypatch.setitem(sys.modules, "kuzu", fake_kuzu)
    monkeypatch.setattr(kuzu_adapter, "_SECONDARY_PROPERTY_INDEX_SUPPORT", None)

    adapter = KuzuAdapter(database="kuzu", dataset_dir=dataset_dir, scale="smoke", profile="indexed")
    adapter.setup()

    assert "CREATE INDEX user_country IF NOT EXISTS ON User(country)" in adapter._connection.queries
    assert "CREATE INDEX user_age IF NOT EXISTS ON User(age)" in adapter._connection.queries


def test_kuzu_indexed_profile_marks_secondary_property_indexes_unsupported(tmp_path: Path, monkeypatch):
    dataset_dir = tmp_path / "dataset"
    dataset_dir.mkdir()
    fake_kuzu = types.SimpleNamespace(Database=lambda path: {"path": path}, Connection=UnsupportedIndexConnection)
    monkeypatch.setitem(sys.modules, "kuzu", fake_kuzu)
    monkeypatch.setattr(kuzu_adapter, "_SECONDARY_PROPERTY_INDEX_SUPPORT", None)

    adapter = KuzuAdapter(database="kuzu", dataset_dir=dataset_dir, scale="smoke", profile="indexed")
    adapter.setup()

    assert not any(query.startswith("CREATE INDEX") for query in adapter._connection.queries)
    assert adapter.run_workload("load_nodes_edges", warmup=0, iterations=1).status == "unsupported"
    assert adapter.run_workload("property_equality_indexed", warmup=0, iterations=1).status == "unsupported"


def test_kuzu_indexed_profile_reraises_non_parser_index_failures(tmp_path: Path, monkeypatch):
    dataset_dir = tmp_path / "dataset"
    dataset_dir.mkdir()
    fake_kuzu = types.SimpleNamespace(Database=lambda path: {"path": path}, Connection=BrokenIndexConnection)
    monkeypatch.setitem(sys.modules, "kuzu", fake_kuzu)
    monkeypatch.setattr(kuzu_adapter, "_SECONDARY_PROPERTY_INDEX_SUPPORT", None)

    adapter = KuzuAdapter(database="kuzu", dataset_dir=dataset_dir, scale="smoke", profile="indexed")
    try:
        adapter.setup()
    except RuntimeError as exc:
        assert str(exc) == "I/O error while executing CREATE INDEX"
    else:
        raise AssertionError("non-parser index failures should be raised")
