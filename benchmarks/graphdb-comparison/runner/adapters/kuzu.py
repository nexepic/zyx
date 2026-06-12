from __future__ import annotations

import json
import shutil
import tempfile
from pathlib import Path
from typing import Any

from runner.adapters.base import (
    BenchmarkAdapter,
    DEFAULT_PROFILE,
    UnsupportedWorkload,
    WorkloadResult,
    multihop_target_user_id,
    remove_artifact_family,
    write_update_target_user_id,
)

SECONDARY_PROPERTY_INDEX_QUERIES = (
    "CREATE INDEX user_country IF NOT EXISTS ON User(country)",
    "CREATE INDEX user_age IF NOT EXISTS ON User(age)",
)
_SECONDARY_PROPERTY_INDEX_SUPPORT: bool | None = None


def _single_quoted_path(path: Path) -> str:
    escaped = path.resolve().as_posix().replace("\\", "\\\\").replace("'", "\\'")
    return f"'{escaped}'"


class KuzuAdapter(BenchmarkAdapter):
    def __init__(
        self,
        database: str,
        dataset_dir: Path,
        scale: str,
        profile: str = DEFAULT_PROFILE,
        threads: int | None = None,
    ):
        super().__init__(database, dataset_dir, scale, profile, threads)
        self.db_path = dataset_dir.parent / "kuzu.db"
        self._database: Any | None = None
        self._connection: Any | None = None
        self._loaded_rows: int | None = None
        self._write_counter = 0

    def setup(self) -> None:
        import kuzu

        if self.db_path.exists():
            if self.db_path.is_dir():
                shutil.rmtree(self.db_path)
            else:
                self.db_path.unlink()

        self._database = self._open_database(kuzu)
        self._connection = self._open_connection(kuzu)
        self._execute_statement("CREATE NODE TABLE User(id STRING, age INT64, country STRING, score DOUBLE, PRIMARY KEY(id))")
        self._execute_statement("CREATE NODE TABLE Post(id STRING, created_at INT64, score DOUBLE, PRIMARY KEY(id))")
        self._execute_statement("CREATE NODE TABLE Tag(id STRING, rank INT64, PRIMARY KEY(id))")
        self._execute_statement("CREATE REL TABLE FOLLOWS(FROM User TO User, weight INT64)")
        self._execute_statement("CREATE REL TABLE AUTHORED(FROM User TO Post, weight INT64)")
        self._execute_statement("CREATE REL TABLE HAS_TAG(FROM Post TO Tag, weight INT64)")
        if self.profile in {"indexed", "operational_dynamic"} and supports_secondary_property_indexes():
            self._create_secondary_property_indexes()

    def teardown(self) -> None:
        for handle in (self._connection, self._database):
            close = getattr(handle, "close", None)
            if callable(close):
                close()
        self._connection = None
        self._database = None

    def cleanup_artifacts(self) -> list[Path]:
        return remove_artifact_family(self.db_path)

    def _open_database(self, kuzu_module: Any) -> Any:
        if self.threads is None:
            return kuzu_module.Database(str(self.db_path))
        try:
            return kuzu_module.Database(str(self.db_path), max_num_threads=self.threads)
        except TypeError:
            return kuzu_module.Database(str(self.db_path))

    def _open_connection(self, kuzu_module: Any) -> Any:
        if self.threads is None:
            return kuzu_module.Connection(self._database)
        try:
            return kuzu_module.Connection(self._database, num_threads=self.threads)
        except TypeError:
            connection = kuzu_module.Connection(self._database)
            setter = getattr(connection, "set_max_threads_for_exec", None)
            if callable(setter):
                setter(self.threads)
                return connection
            raise RuntimeError("Kuzu adapter cannot configure execution threads for this Kuzu version")

    def run_workload(self, workload: str, warmup: int, iterations: int, *args: Any, **kwargs: Any) -> WorkloadResult:
        if self.profile == "indexed" and workload == "load_nodes_edges" and not supports_secondary_property_indexes():
            return WorkloadResult(
                self.database,
                workload,
                self.scale,
                "unsupported",
                error=(
                    "Kuzu scalar secondary property indexes are not supported by this Kuzu version/adapter; "
                    "refusing to report base-load latency as indexed-load latency"
                ),
            )
        return super().run_workload(workload, warmup, iterations, *args, **kwargs)

    def load_nodes_edges(self) -> int:
        self._ensure_connection()
        if self._loaded_rows is not None:
            return self._loaded_rows

        for table, filename in [
            ("User", "users.csv"),
            ("Post", "posts.csv"),
            ("Tag", "tags.csv"),
            ("FOLLOWS", "follows.csv"),
            ("AUTHORED", "authored.csv"),
            ("HAS_TAG", "has_tag.csv"),
        ]:
            self._execute_statement(f"COPY {table} FROM {_single_quoted_path(self.dataset_dir / filename)} (HEADER=true)")

        self._loaded_rows = self._manifest_row_count()
        if self._loaded_rows is None:
            self._loaded_rows = sum(
                self._scalar_int(f"MATCH (n:{label}) RETURN COUNT(n)") for label in ["User", "Post", "Tag"]
            ) + sum(
                self._scalar_int(f"MATCH ()-[r:{rel}]->() RETURN COUNT(r)") for rel in ["FOLLOWS", "AUTHORED", "HAS_TAG"]
            )
        if self.profile == "operational_dynamic":
            self.durability_barrier()
            self._reopen_existing()
        return self._loaded_rows

    def point_lookup_indexed(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User {id: 'user-000001'}) RETURN COUNT(u)")

    def label_scan_filter(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User) WHERE u.country = 'CN' RETURN COUNT(u)")

    def one_hop_expand(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (:User {id: 'user-000001'})-[:FOLLOWS]->(v:User) RETURN COUNT(v)")

    def two_hop_expand(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (:User {id: 'user-000001'})-[:FOLLOWS]->(:User)-[:FOLLOWS]->(v:User) RETURN COUNT(v)")

    def shortest_path_chain(self) -> int:
        self._ensure_loaded()
        return self._scalar_int(
            "MATCH p = (:User {id: 'user-000001'})-[:FOLLOWS*1..6]->(:User {id: 'user-000006'}) "
            "RETURN CASE WHEN count(p) > 0 THEN 1 ELSE 0 END"
        )

    def reachable_within_6(self) -> int:
        return self._reachable_within(6)

    def reachable_within_12(self) -> int:
        return self._reachable_within(12)

    def reachable_within_24(self) -> int:
        return self._reachable_within(24)

    def reachable_within_30(self) -> int:
        return self._reachable_within(30)

    def all_nodes_property_filter(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (n) WHERE n.score >= 900 RETURN COUNT(n)")

    def label_multi_property_filter(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User) WHERE u.country = 'CN' AND u.age >= 30 RETURN COUNT(u)")

    def relationship_type_scan(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH ()-[r:FOLLOWS]->() RETURN COUNT(r)")

    def relationship_property_filter(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH ()-[r:FOLLOWS]->() WHERE r.weight = 1 RETURN COUNT(r)")

    def aggregation_group_by(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User) RETURN COUNT(DISTINCT u.country)")

    def aggregation_count_by_group(self) -> int:
        self._ensure_loaded()
        return self._row_count("MATCH (u:User) RETURN u.country, COUNT(*)")

    def topk_property_sort(self) -> int:
        self._ensure_loaded()
        return self._row_count("MATCH (u:User) RETURN u.id ORDER BY u.score DESC LIMIT 100")

    def property_equality_indexed(self) -> int:
        self._require_secondary_property_indexes()
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User) WHERE u.country = 'CN' RETURN COUNT(u)")

    def property_range_indexed(self) -> int:
        self._require_secondary_property_indexes()
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User) WHERE u.age >= 30 AND u.age < 40 RETURN COUNT(u)")

    def point_create_node(self) -> int:
        self._ensure_loaded()
        write_id = self._next_write_id()
        return self._row_count(
            "CREATE (u:User {"
            f"id: 'bench-user-{write_id:06d}', age: 41, country: 'ZZ', score: {float(write_id):.1f}"
            "}) RETURN 1"
        )

    def point_create_edge(self) -> int:
        self._ensure_loaded()
        self._next_write_id()
        return self._row_count(
            "MATCH (src:User {id: 'user-000006'}), (dst:User {id: 'user-000001'}) "
            "CREATE (src)-[:FOLLOWS {weight: 1}]->(dst) RETURN 1"
        )

    def point_update_node_property(self) -> int:
        self._ensure_loaded()
        write_id = self._next_write_id()
        return self._scalar_int(
            f"MATCH (u:User {{id: 'user-000001'}}) SET u.score = {1000.0 + write_id:.1f} RETURN COUNT(u)"
        )

    def point_update_edge_property(self) -> int:
        self._ensure_loaded()
        write_id = self._next_write_id()
        target = write_update_target_user_id(self.scale)
        return self._scalar_int(
            f"MATCH (:User {{id: 'user-000001'}})-[r:FOLLOWS]->(:User {{id: '{target}'}}) "
            f"SET r.weight = {10_000 + write_id} RETURN COUNT(r)"
        )

    def point_create_delete_edge(self) -> int:
        self._ensure_loaded()
        write_id = self._next_write_id()
        weight = -write_id
        self._execute_statement(
            "MATCH (src:User {id: 'user-000006'}), (dst:User {id: 'user-000001'}) "
            f"CREATE (src)-[:FOLLOWS {{weight: {weight}}}]->(dst)"
        )
        return self._row_count(
            "MATCH (:User {id: 'user-000006'})-[r:FOLLOWS]->(:User {id: 'user-000001'}) "
            f"WHERE r.weight = {weight} DELETE r RETURN 1"
        )

    def point_create_delete_edge_durable(self) -> int:
        actual = self.point_create_delete_edge()
        self.durability_barrier()
        # Kuzu 0.11.x can leave the Python connection unstable after a DELETE
        # followed by CHECKPOINT. Reopen the embedded handle before the next
        # iteration so the benchmark measures a safe durable-delete boundary.
        self._reopen_existing()
        return actual

    def write_then_read_edge(self) -> int:
        self._ensure_loaded()
        write_id = self._next_write_id()
        weight = -1_000_000 - write_id
        self._execute_statement(
            "MATCH (src:User {id: 'user-000007'}), (dst:User {id: 'user-000001'}) "
            f"CREATE (src)-[:FOLLOWS {{weight: {weight}}}]->(dst)"
        )
        return self._scalar_int(
            "MATCH (:User {id: 'user-000007'})-[r:FOLLOWS]->(:User {id: 'user-000001'}) "
            f"WHERE r.weight = {weight} RETURN COUNT(r)"
        )

    def post_persist_create_node(self) -> int:
        return self.point_create_node()

    def post_persist_create_edge(self) -> int:
        return self.point_create_edge()

    def write_then_one_hop_expand(self) -> int:
        self._ensure_loaded()
        write_id = self._next_write_id()
        weight = -2_000_000 - write_id
        self._execute_statement(
            "MATCH (src:User {id: 'user-000007'}), (dst:User {id: 'user-000001'}) "
            f"CREATE (src)-[:FOLLOWS {{weight: {weight}}}]->(dst)"
        )
        return self._scalar_int("MATCH (:User {id: 'user-000007'})-[:FOLLOWS]->(v:User) RETURN COUNT(v)")

    def index_seek_then_one_hop_expand(self) -> int:
        self._require_secondary_property_indexes()
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User {country: 'CN'})-[:FOLLOWS]->(v:User) RETURN COUNT(v)")

    def index_seek_then_two_hop_expand(self) -> int:
        self._require_secondary_property_indexes()
        self._ensure_loaded()
        return self._scalar_int(
            "MATCH (u:User {country: 'CN'})-[:FOLLOWS]->(:User)-[:FOLLOWS]->(v:User) RETURN COUNT(v)"
        )

    def batch_create_edges_100(self) -> int:
        return self._batch_create_edges(100)

    def batch_create_edges_1000(self) -> int:
        return self._batch_create_edges(1000)

    def batch_create_edges_10000(self) -> int:
        return self._batch_create_edges(10000)

    def batch_create_edges_100_then_one_hop_expand(self) -> int:
        self._batch_create_edges(100, src="user-000007")
        return self._scalar_int("MATCH (:User {id: 'user-000007'})-[:FOLLOWS]->(v:User) RETURN COUNT(v)")

    def batch_create_edges_10000_then_one_hop_expand(self) -> int:
        self._batch_create_edges(10000, src="user-000008")
        return self._scalar_int("MATCH (:User {id: 'user-000008'})-[:FOLLOWS]->(v:User) RETURN COUNT(v)")

    def write_then_read_edge_durable(self) -> int:
        self._ensure_loaded()
        write_id = self._next_write_id()
        weight = -1_000_000 - write_id
        self._execute_statement(
            "MATCH (src:User {id: 'user-000007'}), (dst:User {id: 'user-000001'}) "
            f"CREATE (src)-[:FOLLOWS {{weight: {weight}}}]->(dst)"
        )
        self.durability_barrier()
        return self._scalar_int(
            "MATCH (:User {id: 'user-000007'})-[r:FOLLOWS]->(:User {id: 'user-000001'}) "
            f"WHERE r.weight = {weight} RETURN COUNT(r)"
        )

    def durability_barrier(self) -> None:
        self._ensure_loaded()
        self._execute_statement("CHECKPOINT")

    def _next_write_id(self) -> int:
        self._write_counter += 1
        return self._write_counter

    def _batch_create_edges(self, count: int, src: str = "user-000006", dst: str = "user-000001") -> int:
        self._ensure_loaded()
        first_id = self._write_counter + 1
        self._write_counter += count
        last_id = self._write_counter
        return self._scalar_int(
            f"UNWIND range({first_id}, {last_id}) AS i "
            f"MATCH (src:User {{id: '{src}'}}), (dst:User {{id: '{dst}'}}) "
            "CREATE (src)-[:FOLLOWS {weight: -3000000 - i}]->(dst) "
            "RETURN COUNT(*)"
        )

    def _ensure_connection(self) -> None:
        if self._connection is None:
            raise RuntimeError("Kuzu adapter is not set up")

    def _ensure_loaded(self) -> None:
        if self._loaded_rows is None:
            self.load_nodes_edges()

    def _execute(self, query: str) -> Any:
        self._ensure_connection()
        return self._connection.execute(query)

    def _execute_statement(self, query: str) -> None:
        self._drain_result(self._execute(query))

    def _reopen_existing(self) -> None:
        import kuzu

        self.teardown()
        self._database = self._open_database(kuzu)
        self._connection = self._open_connection(kuzu)

    def _create_secondary_property_indexes(self) -> None:
        for query in SECONDARY_PROPERTY_INDEX_QUERIES:
            self._execute_statement(query)

    def _require_secondary_property_indexes(self) -> None:
        if not supports_secondary_property_indexes():
            raise UnsupportedWorkload(
                "Kuzu scalar secondary property indexes are not supported by this Kuzu version/adapter; "
                "refusing to report scan latency as indexed predicate latency"
            )

    def _scalar_int(self, query: str) -> int:
        result = self._execute(query)
        value = self._first_value(result)
        self._drain_result(result)
        return int(value)

    def _row_count(self, query: str) -> int:
        result = self._execute(query)
        has_next = getattr(result, "has_next", None)
        get_next = getattr(result, "get_next", None)
        if callable(has_next) and callable(get_next):
            count = 0
            while has_next():
                get_next()
                count += 1
            self._drain_result(result)
            return count
        for dataframe_method in ("get_as_df", "to_df"):
            to_dataframe = getattr(result, dataframe_method, None)
            if callable(to_dataframe):
                return len(to_dataframe())
        if isinstance(result, list):
            return len(result)
        value = int(self._first_value(result))
        self._drain_result(result)
        return value

    def _drain_result(self, result: Any) -> None:
        has_next = getattr(result, "has_next", None)
        get_next = getattr(result, "get_next", None)
        if callable(has_next) and callable(get_next):
            while has_next():
                get_next()
        close = getattr(result, "close", None)
        if callable(close):
            close()

    def _reachable_within(self, depth: int) -> int:
        self._ensure_loaded()
        target = multihop_target_user_id(depth, self.scale)
        return self._row_count(
            f"MATCH p = (:User {{id: 'user-000001'}})-[:FOLLOWS*1..{depth}]->(:User {{id: '{target}'}}) "
            "RETURN 1 LIMIT 1"
        )

    def _first_value(self, result: Any) -> Any:
        if result is None:
            return 0

        get_next = getattr(result, "get_next", None)
        if callable(get_next):
            row = get_next()
            return self._first_cell(row)

        for dataframe_method in ("get_as_df", "to_df"):
            to_dataframe = getattr(result, dataframe_method, None)
            if callable(to_dataframe):
                dataframe = to_dataframe()
                if getattr(dataframe, "empty", False):
                    return 0
                return dataframe.iloc[0, 0]

        if isinstance(result, list):
            if not result:
                return 0
            return self._first_cell(result[0])

        return self._first_cell(result)

    def _first_cell(self, row: Any) -> Any:
        if row is None:
            return 0
        if isinstance(row, dict):
            return next(iter(row.values()), 0)
        if isinstance(row, (list, tuple)):
            return row[0] if row else 0
        return row


    def _manifest_row_count(self) -> int | None:
        manifest_path = self.dataset_dir / "manifest.json"
        if not manifest_path.exists():
            return None
        manifest = json.loads(manifest_path.read_text())
        counts = manifest.get("counts", {})
        if not isinstance(counts, dict):
            return None
        return sum(int(counts[name]) for name in ["users", "posts", "tags", "follows", "authored", "has_tag"])


def _is_unsupported_index_error(exc: Exception) -> bool:
    message = str(exc)
    return "Parser exception" in message and "CREATE INDEX" in message


def supports_secondary_property_indexes() -> bool:
    global _SECONDARY_PROPERTY_INDEX_SUPPORT
    if _SECONDARY_PROPERTY_INDEX_SUPPORT is not None:
        return _SECONDARY_PROPERTY_INDEX_SUPPORT

    import kuzu

    with tempfile.TemporaryDirectory(prefix="zyx-kuzu-index-probe-") as tmp:
        database = kuzu.Database(str(Path(tmp) / "probe.db"))
        connection = kuzu.Connection(database)
        try:
            _drain_probe_result(
                connection.execute(
                    "CREATE NODE TABLE User(id STRING, age INT64, country STRING, score DOUBLE, PRIMARY KEY(id))"
                )
            )
            _drain_probe_result(connection.execute(SECONDARY_PROPERTY_INDEX_QUERIES[0]))
        except Exception as exc:
            if _is_unsupported_index_error(exc):
                _SECONDARY_PROPERTY_INDEX_SUPPORT = False
                return False
            raise
        finally:
            for handle in (connection, database):
                close = getattr(handle, "close", None)
                if callable(close):
                    close()

    _SECONDARY_PROPERTY_INDEX_SUPPORT = True
    return True


def _drain_probe_result(result: Any) -> None:
    has_next = getattr(result, "has_next", None)
    get_next = getattr(result, "get_next", None)
    if callable(has_next) and callable(get_next):
        while has_next():
            get_next()
    close = getattr(result, "close", None)
    if callable(close):
        close()
