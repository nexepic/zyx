from __future__ import annotations

import json
import shutil
from pathlib import Path
from typing import Any

from runner.adapters.base import BenchmarkAdapter, DEFAULT_PROFILE


def _single_quoted_path(path: Path) -> str:
    escaped = path.resolve().as_posix().replace("\\", "\\\\").replace("'", "\\'")
    return f"'{escaped}'"


class KuzuAdapter(BenchmarkAdapter):
    def __init__(self, database: str, dataset_dir: Path, scale: str, profile: str = DEFAULT_PROFILE):
        super().__init__(database, dataset_dir, scale, profile)
        self.db_path = dataset_dir.parent / "kuzu.db"
        self._database: Any | None = None
        self._connection: Any | None = None
        self._loaded_rows: int | None = None

    def setup(self) -> None:
        import kuzu

        if self.db_path.exists():
            if self.db_path.is_dir():
                shutil.rmtree(self.db_path)
            else:
                self.db_path.unlink()

        self._database = kuzu.Database(str(self.db_path))
        self._connection = kuzu.Connection(self._database)
        self._execute("CREATE NODE TABLE User(id STRING, age INT64, country STRING, score DOUBLE, PRIMARY KEY(id))")
        self._execute("CREATE NODE TABLE Post(id STRING, created_at INT64, score DOUBLE, PRIMARY KEY(id))")
        self._execute("CREATE NODE TABLE Tag(id STRING, rank INT64, PRIMARY KEY(id))")
        self._execute("CREATE REL TABLE FOLLOWS(FROM User TO User, weight INT64)")
        self._execute("CREATE REL TABLE AUTHORED(FROM User TO Post, weight INT64)")
        self._execute("CREATE REL TABLE HAS_TAG(FROM Post TO Tag, weight INT64)")
        if self.profile == "indexed":
            self._execute_optional_index("CREATE INDEX user_country IF NOT EXISTS ON User(country)")
            self._execute_optional_index("CREATE INDEX user_age IF NOT EXISTS ON User(age)")

    def teardown(self) -> None:
        for handle in (self._connection, self._database):
            close = getattr(handle, "close", None)
            if callable(close):
                close()
        self._connection = None
        self._database = None

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
            self._execute(f"COPY {table} FROM {_single_quoted_path(self.dataset_dir / filename)} (HEADER=true)")

        self._loaded_rows = self._manifest_row_count()
        if self._loaded_rows is None:
            self._loaded_rows = sum(
                self._scalar_int(f"MATCH (n:{label}) RETURN COUNT(n)") for label in ["User", "Post", "Tag"]
            ) + sum(
                self._scalar_int(f"MATCH ()-[r:{rel}]->() RETURN COUNT(r)") for rel in ["FOLLOWS", "AUTHORED", "HAS_TAG"]
            )
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
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User) WHERE u.country = 'CN' RETURN COUNT(u)")

    def property_range_indexed(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User) WHERE u.age >= 30 AND u.age < 40 RETURN COUNT(u)")

    def _ensure_connection(self) -> None:
        if self._connection is None:
            raise RuntimeError("Kuzu adapter is not set up")

    def _ensure_loaded(self) -> None:
        if self._loaded_rows is None:
            self.load_nodes_edges()

    def _execute(self, query: str) -> Any:
        self._ensure_connection()
        return self._connection.execute(query)

    def _execute_optional_index(self, query: str) -> None:
        try:
            self._execute(query)
        except Exception as exc:
            if not _is_unsupported_index_error(exc):
                raise

    def _scalar_int(self, query: str) -> int:
        return int(self._first_value(self._execute(query)))

    def _row_count(self, query: str) -> int:
        result = self._execute(query)
        has_next = getattr(result, "has_next", None)
        get_next = getattr(result, "get_next", None)
        if callable(has_next) and callable(get_next):
            count = 0
            while has_next():
                get_next()
                count += 1
            return count
        for dataframe_method in ("get_as_df", "to_df"):
            to_dataframe = getattr(result, dataframe_method, None)
            if callable(to_dataframe):
                return len(to_dataframe())
        if isinstance(result, list):
            return len(result)
        return int(self._first_value(result))

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
