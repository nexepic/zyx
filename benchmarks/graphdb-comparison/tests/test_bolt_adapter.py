from __future__ import annotations

import sys
import types
from pathlib import Path
from typing import Any

from runner.adapters.bolt import BoltCypherAdapter, MemgraphAdapter, Neo4jAdapter


class FakeResult:
    def __init__(self, session: "FakeSession", value: int = 1):
        self.session = session
        self.value = value
        self.consumed = False

    def single(self) -> dict[str, int]:
        if not self.session.open:
            raise RuntimeError("result accessed outside session")
        return {"value": self.value}

    def consume(self) -> None:
        if not self.session.open:
            raise RuntimeError("result consumed outside session")
        self.consumed = True

    def __iter__(self):
        if not self.session.open:
            raise RuntimeError("result iterated outside session")
        return iter([{"value": self.value}])


class FakeIterableResult(FakeResult):
    def __init__(self, session: "FakeSession", rows: list[dict[str, str]]):
        super().__init__(session)
        self.rows = rows

    def __iter__(self):
        if not self.session.open:
            raise RuntimeError("result iterated outside session")
        return iter(self.rows)


class FakeSession:
    def __init__(self, driver: "FakeDriver"):
        self.driver = driver
        self.open = False

    def __enter__(self) -> "FakeSession":
        self.open = True
        return self

    def __exit__(self, *args: object) -> None:
        self.open = False
        return None

    def run(self, query: str, parameters: dict[str, Any]) -> FakeResult:
        self.driver.queries.append((query, parameters))
        if self.driver.fail_duplicate_index and query.startswith("CREATE INDEX"):
            raise RuntimeError(self.driver.duplicate_index_message)
        if self.driver.rows is not None:
            return FakeIterableResult(self, self.driver.rows)
        return FakeResult(self, self.driver.scalar)


class FakeDriver:
    def __init__(self):
        self.queries: list[tuple[str, dict[str, Any]]] = []
        self.scalar = 1
        self.rows: list[dict[str, str]] | None = None
        self.closed = False
        self.fail_duplicate_index = False
        self.duplicate_index_message = "index already exists"

    def session(self) -> FakeSession:
        return FakeSession(self)

    def close(self) -> None:
        self.closed = True


class FakeGraphDatabase:
    def __init__(self, driver: FakeDriver):
        self.driver_instance = driver
        self.calls: list[tuple[str, tuple[str | None, str | None] | None]] = []

    def driver(self, uri: str, auth: tuple[str | None, str | None] | None = None) -> FakeDriver:
        self.calls.append((uri, auth))
        return self.driver_instance


def _install_fake_neo4j(monkeypatch: Any, driver: FakeDriver) -> FakeGraphDatabase:
    graph_database = FakeGraphDatabase(driver)
    monkeypatch.setitem(sys.modules, "neo4j", types.SimpleNamespace(GraphDatabase=graph_database))
    return graph_database


def _write_dataset(dataset_dir: Path) -> None:
    dataset_dir.mkdir()
    (dataset_dir / "users.csv").write_text("id,age,country,score\nuser-000001,20,CN,1.5\n")
    (dataset_dir / "posts.csv").write_text("id,created_at,score\npost-000001,1700000000,2.5\n")
    (dataset_dir / "tags.csv").write_text("id,rank\ntag-000001,1\n")
    (dataset_dir / "follows.csv").write_text("src,dst,weight\nuser-000001,user-000002,1\n")
    (dataset_dir / "authored.csv").write_text("src,dst,weight\nuser-000001,post-000001,1\n")
    (dataset_dir / "has_tag.csv").write_text("src,dst,weight\npost-000001,tag-000001,1\n")
    (dataset_dir / "manifest.json").write_text(
        '{"counts":{"users":1,"posts":1,"tags":1,"follows":1,"authored":1,"has_tag":1}}\n'
    )


def test_bolt_module_import_does_not_require_neo4j_package(tmp_path: Path):
    adapter = Neo4jAdapter(database="neo4j", dataset_dir=tmp_path / "dataset", scale="smoke")

    assert adapter.uri == "bolt://localhost:7687"
    assert adapter.user == "neo4j"


def test_neo4j_setup_creates_driver_resets_and_adds_constraints(tmp_path: Path, monkeypatch: Any):
    driver = FakeDriver()
    graph_database = _install_fake_neo4j(monkeypatch, driver)
    monkeypatch.setenv("NEO4J_URI", "bolt://neo4j:7687")
    monkeypatch.setenv("NEO4J_USER", "alice")
    monkeypatch.setenv("NEO4J_PASSWORD", "secret")
    adapter = Neo4jAdapter(database="neo4j", dataset_dir=tmp_path / "dataset", scale="smoke")

    adapter.setup()
    adapter.teardown()

    assert graph_database.calls == [("bolt://neo4j:7687", ("alice", "secret"))]
    queries = [query for query, _ in driver.queries]
    assert queries[:2] == ["RETURN 1 AS ok", "MATCH (n) DETACH DELETE n"]
    assert "CREATE CONSTRAINT user_id_unique IF NOT EXISTS FOR (n:User) REQUIRE n.id IS UNIQUE" in queries
    assert "CREATE CONSTRAINT post_id_unique IF NOT EXISTS FOR (n:Post) REQUIRE n.id IS UNIQUE" in queries
    assert "CREATE CONSTRAINT tag_id_unique IF NOT EXISTS FOR (n:Tag) REQUIRE n.id IS UNIQUE" in queries
    assert driver.closed


def test_setup_closes_driver_on_failure_after_driver_creation(tmp_path: Path, monkeypatch: Any):
    driver = FakeDriver()
    graph_database = _install_fake_neo4j(monkeypatch, driver)
    adapter = Neo4jAdapter(database="neo4j", dataset_dir=tmp_path / "dataset", scale="smoke")
    adapter._wait_until_ready = lambda timeout_seconds: (_ for _ in ()).throw(RuntimeError("not ready"))

    try:
        adapter.setup()
    except RuntimeError as exc:
        assert str(exc) == "not ready"
    else:
        raise AssertionError("setup should fail")

    assert graph_database.calls == [("bolt://localhost:7687", ("neo4j", "password"))]
    assert driver.closed
    assert adapter._driver is None


def test_partial_auth_raises_clear_error(tmp_path: Path, monkeypatch: Any):
    monkeypatch.setenv("BOLT_USER", "alice")
    monkeypatch.delenv("BOLT_PASSWORD", raising=False)
    adapter = BoltCypherAdapter(database="bolt", dataset_dir=tmp_path / "dataset", scale="smoke")

    try:
        adapter._auth()
    except RuntimeError as exc:
        assert "requires both BOLT_USER and BOLT_PASSWORD" in str(exc)
    else:
        raise AssertionError("partial auth should fail")


def test_memgraph_setup_ignores_duplicate_index_errors(tmp_path: Path, monkeypatch: Any):
    driver = FakeDriver()
    driver.fail_duplicate_index = True
    _install_fake_neo4j(monkeypatch, driver)
    adapter = MemgraphAdapter(database="memgraph", dataset_dir=tmp_path / "dataset", scale="smoke")

    adapter.setup()

    queries = [query for query, _ in driver.queries]
    assert "CREATE INDEX ON :User(id)" in queries
    assert "CREATE INDEX ON :Post(id)" in queries
    assert "CREATE INDEX ON :Tag(id)" in queries


def test_memgraph_duplicate_index_tolerance_is_narrow(tmp_path: Path, monkeypatch: Any):
    driver = FakeDriver()
    driver.fail_duplicate_index = True
    driver.duplicate_index_message = "property exists but index creation failed"
    _install_fake_neo4j(monkeypatch, driver)
    adapter = MemgraphAdapter(database="memgraph", dataset_dir=tmp_path / "dataset", scale="smoke")

    try:
        adapter.setup()
    except RuntimeError as exc:
        assert str(exc) == "property exists but index creation failed"
    else:
        raise AssertionError("generic exists errors should not be ignored")


def test_load_nodes_edges_batches_csv_rows_and_returns_manifest_count(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    _write_dataset(dataset_dir)
    driver = FakeDriver()
    adapter = MemgraphAdapter(database="memgraph", dataset_dir=dataset_dir, scale="smoke")
    adapter._driver = driver

    assert adapter.load_nodes_edges() == 6

    queries = [query for query, _ in driver.queries]
    assert queries[0].startswith("UNWIND $rows AS row CREATE (:User")
    assert any("CREATE (:Post" in query for query in queries)
    assert any("CREATE (:Tag" in query for query in queries)
    assert any("CREATE (src)-[:FOLLOWS" in query for query in queries)
    assert any("CREATE (src)-[:AUTHORED" in query for query in queries)
    assert any("CREATE (src)-[:HAS_TAG" in query for query in queries)


def test_load_nodes_edges_streams_more_than_one_batch(tmp_path: Path):
    dataset_dir = tmp_path / "dataset"
    _write_dataset(dataset_dir)
    (dataset_dir / "users.csv").write_text(
        "id,age,country,score\n"
        "user-000001,20,CN,1.5\n"
        "user-000002,21,US,2.5\n"
        "user-000003,22,JP,3.5\n"
    )
    (dataset_dir / "manifest.json").write_text(
        '{"counts":{"users":3,"posts":1,"tags":1,"follows":1,"authored":1,"has_tag":1}}\n'
    )
    driver = FakeDriver()
    adapter = MemgraphAdapter(database="memgraph", dataset_dir=dataset_dir, scale="smoke")
    adapter._driver = driver
    adapter.batch_size = 2

    assert adapter.load_nodes_edges() == 8

    user_batches = [params["rows"] for query, params in driver.queries if query.startswith("UNWIND $rows AS row CREATE (:User")]
    assert [len(batch) for batch in user_batches] == [2, 1]
    assert user_batches[0][0]["id"] == "user-000001"
    assert user_batches[1][0]["id"] == "user-000003"


def test_workload_methods_return_scalar_counts_without_live_database(tmp_path: Path):
    adapter = BoltCypherAdapter(database="bolt", dataset_dir=tmp_path / "dataset", scale="smoke")
    driver = FakeDriver()
    driver.scalar = 7
    adapter._driver = driver
    adapter._loaded_rows = 1

    assert adapter.point_lookup_indexed() == 7
    assert adapter.label_scan_filter() == 7
    assert adapter.one_hop_expand() == 7
    assert adapter.two_hop_expand() == 7
    assert adapter.shortest_path_chain() == 7

    queries = [query for query, _ in driver.queries]
    assert "MATCH (u:User {id: $id}) RETURN count(u) AS count" in queries
    assert "MATCH (u:User) WHERE u.country = $country RETURN count(u) AS count" in queries
    assert any("[:FOLLOWS]->(v:User)" in query for query in queries)
    assert any("[:FOLLOWS]->(:User)-[:FOLLOWS]->(v:User)" in query for query in queries)
    assert any("[:FOLLOWS*1..6]" in query for query in queries)


def test_bolt_diagnostic_workload_methods_issue_expected_queries(tmp_path: Path):
    adapter = BoltCypherAdapter(database="bolt", dataset_dir=tmp_path / "dataset", scale="smoke", profile="scan")
    driver = FakeDriver()
    driver.scalar = 7
    adapter._driver = driver
    adapter._loaded_rows = 1

    assert adapter.all_nodes_property_filter() == 7
    assert adapter.label_multi_property_filter() == 7
    assert adapter.relationship_type_scan() == 7
    assert adapter.relationship_property_filter() == 7
    assert adapter.aggregation_group_by() == 7

    driver.rows = [{"id": "user-000001"}, {"id": "user-000002"}, {"id": "user-000003"}]
    assert adapter.topk_property_sort() == 3
    driver.rows = None

    queries = [query for query, _ in driver.queries]
    assert "MATCH (n) WHERE n.score >= $min_score RETURN count(n) AS count" in queries
    assert "MATCH (u:User) WHERE u.country = $country AND u.age >= $min_age RETURN count(u) AS count" in queries
    assert "MATCH ()-[r:FOLLOWS]->() RETURN count(r) AS count" in queries
    assert "MATCH ()-[r:FOLLOWS]->() WHERE r.weight = $weight RETURN count(r) AS count" in queries
    assert "MATCH (u:User) RETURN count(DISTINCT u.country) AS count" in queries
    assert "MATCH (u:User) RETURN u.id AS id ORDER BY u.score DESC LIMIT 100" in queries


def test_bolt_indexed_workload_methods_issue_expected_queries(tmp_path: Path):
    adapter = BoltCypherAdapter(database="bolt", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")
    driver = FakeDriver()
    driver.scalar = 7
    adapter._driver = driver
    adapter._loaded_rows = 1

    assert adapter.property_equality_indexed() == 7
    assert adapter.property_range_indexed() == 7

    queries = [query for query, _ in driver.queries]
    assert "MATCH (u:User {country: $country}) RETURN count(u) AS count" in queries
    assert "MATCH (u:User) WHERE u.age >= $min_age AND u.age < $max_age RETURN count(u) AS count" in queries


def test_neo4j_indexed_profile_adds_property_indexes(tmp_path: Path, monkeypatch: Any):
    driver = FakeDriver()
    _install_fake_neo4j(monkeypatch, driver)
    adapter = Neo4jAdapter(database="neo4j", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    adapter.setup()

    queries = [query for query, _ in driver.queries]
    assert "CREATE INDEX user_country IF NOT EXISTS FOR (n:User) ON (n.country)" in queries
    assert "CREATE INDEX user_age IF NOT EXISTS FOR (n:User) ON (n.age)" in queries


def test_memgraph_indexed_profile_adds_property_indexes(tmp_path: Path, monkeypatch: Any):
    driver = FakeDriver()
    _install_fake_neo4j(monkeypatch, driver)
    adapter = MemgraphAdapter(database="memgraph", dataset_dir=tmp_path / "dataset", scale="smoke", profile="indexed")

    adapter.setup()

    queries = [query for query, _ in driver.queries]
    assert "CREATE INDEX ON :User(country)" in queries
    assert "CREATE INDEX ON :User(age)" in queries


def test_memgraph_shortest_path_chain_avoids_aggregation_inside_case(tmp_path: Path):
    adapter = MemgraphAdapter(database="memgraph", dataset_dir=tmp_path / "dataset", scale="smoke")
    driver = FakeDriver()
    adapter._driver = driver
    adapter._loaded_rows = 1

    adapter.shortest_path_chain()

    query = driver.queries[-1][0]
    assert "count(p)" not in query
    assert "CASE WHEN" not in query
    assert "LIMIT 1" in query
