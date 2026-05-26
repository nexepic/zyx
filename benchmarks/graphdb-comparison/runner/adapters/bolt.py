from __future__ import annotations

import csv
import json
import os
import re
import time
from importlib import import_module
from pathlib import Path
from typing import Any, Iterable

from runner.adapters.base import BenchmarkAdapter, DEFAULT_PROFILE


class BoltCypherAdapter(BenchmarkAdapter):
    env_prefix = "BOLT"
    default_uri = "bolt://localhost:7687"
    default_user: str | None = None
    default_password: str | None = None
    batch_size = 1_000

    def __init__(self, database: str, dataset_dir: Path, scale: str, profile: str = DEFAULT_PROFILE):
        super().__init__(database, dataset_dir, scale, profile)
        self.uri = os.getenv(f"{self.env_prefix}_URI", os.getenv("BOLT_URI", self.default_uri))
        self.user = os.getenv(f"{self.env_prefix}_USER", os.getenv("BOLT_USER", self.default_user or "")) or None
        self.password = os.getenv(
            f"{self.env_prefix}_PASSWORD", os.getenv("BOLT_PASSWORD", self.default_password or "")
        ) or None
        self._driver: Any | None = None
        self._loaded_rows: int | None = None

    def setup(self) -> None:
        self._driver = self._create_driver()
        try:
            self._wait_until_ready(timeout_seconds=120.0)
            self._execute("MATCH (n) DETACH DELETE n")
            self._create_indexes()
        except Exception:
            self.teardown()
            raise

    def teardown(self) -> None:
        if self._driver is not None:
            self._driver.close()
        self._driver = None

    def load_nodes_edges(self) -> int:
        self._ensure_driver()
        if self._loaded_rows is not None:
            return self._loaded_rows

        total = 0
        total += self._load_csv("users.csv", self._create_users)
        total += self._load_csv("posts.csv", self._create_posts)
        total += self._load_csv("tags.csv", self._create_tags)
        total += self._load_csv("follows.csv", self._create_follows)
        total += self._load_csv("authored.csv", self._create_authored)
        total += self._load_csv("has_tag.csv", self._create_has_tag)

        manifest_count = self._manifest_row_count()
        self._loaded_rows = manifest_count if manifest_count is not None else total
        return self._loaded_rows

    def point_lookup_indexed(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User {id: $id}) RETURN count(u) AS count", {"id": "user-000001"})

    def label_scan_filter(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User) WHERE u.country = $country RETURN count(u) AS count", {"country": "CN"})

    def one_hop_expand(self) -> int:
        self._ensure_loaded()
        return self._scalar_int(
            "MATCH (:User {id: $id})-[:FOLLOWS]->(v:User) RETURN count(v) AS count", {"id": "user-000001"}
        )

    def two_hop_expand(self) -> int:
        self._ensure_loaded()
        return self._scalar_int(
            "MATCH (:User {id: $id})-[:FOLLOWS]->(:User)-[:FOLLOWS]->(v:User) RETURN count(v) AS count",
            {"id": "user-000001"},
        )

    def shortest_path_chain(self) -> int:
        self._ensure_loaded()
        return self._scalar_int(
            "MATCH p = (:User {id: $src})-[:FOLLOWS*1..6]->(:User {id: $dst}) "
            "RETURN CASE WHEN count(p) > 0 THEN 1 ELSE 0 END AS count",
            {"src": "user-000001", "dst": "user-000006"},
        )

    def all_nodes_property_filter(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (n) WHERE n.score >= $min_score RETURN count(n) AS count", {"min_score": 900.0})

    def label_multi_property_filter(self) -> int:
        self._ensure_loaded()
        return self._scalar_int(
            "MATCH (u:User) WHERE u.country = $country AND u.age >= $min_age RETURN count(u) AS count",
            {"country": "CN", "min_age": 30},
        )

    def relationship_type_scan(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH ()-[r:FOLLOWS]->() RETURN count(r) AS count")

    def relationship_property_filter(self) -> int:
        self._ensure_loaded()
        return self._scalar_int(
            "MATCH ()-[r:FOLLOWS]->() WHERE r.weight = $weight RETURN count(r) AS count", {"weight": 1}
        )

    def aggregation_group_by(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User) RETURN count(DISTINCT u.country) AS count")

    def topk_property_sort(self) -> int:
        self._ensure_loaded()
        return len(self._records("MATCH (u:User) RETURN u.id AS id ORDER BY u.score DESC LIMIT 100"))

    def property_equality_indexed(self) -> int:
        self._ensure_loaded()
        return self._scalar_int("MATCH (u:User {country: $country}) RETURN count(u) AS count", {"country": "CN"})

    def property_range_indexed(self) -> int:
        self._ensure_loaded()
        return self._scalar_int(
            "MATCH (u:User) WHERE u.age >= $min_age AND u.age < $max_age RETURN count(u) AS count",
            {"min_age": 30, "max_age": 40},
        )

    def _create_driver(self) -> Any:
        graph_database = import_module("neo4j").GraphDatabase
        auth = self._auth()
        return graph_database.driver(self.uri, auth=auth)

    def _auth(self) -> tuple[str, str] | None:
        if self.user is None and self.password is None:
            return None
        if self.user is not None and self.password is not None:
            return (self.user, self.password)
        raise RuntimeError(
            f"{self.database} Bolt auth requires both {self.env_prefix}_USER and {self.env_prefix}_PASSWORD, "
            "or neither"
        )

    def _wait_until_ready(self, timeout_seconds: float) -> None:
        deadline = time.monotonic() + timeout_seconds
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                if self._scalar_int("RETURN 1 AS ok") == 1:
                    return
            except Exception as exc:
                last_error = exc
                time.sleep(1.0)
        detail = f": {last_error}" if last_error is not None else ""
        raise TimeoutError(f"{self.database} Bolt endpoint was not ready after {timeout_seconds:.0f}s{detail}")

    def _create_indexes(self) -> None:
        raise NotImplementedError

    def _execute(self, query: str, parameters: dict[str, Any] | None = None) -> None:
        self._ensure_driver()
        with self._driver.session() as session:
            result = session.run(query, parameters or {})
            consume = getattr(result, "consume", None)
            if callable(consume):
                consume()

    def _records(self, query: str, parameters: dict[str, Any] | None = None) -> list[Any]:
        self._ensure_driver()
        with self._driver.session() as session:
            return list(session.run(query, parameters or {}))

    def _execute_ddl(self, query: str, ignore_duplicate: bool = False) -> None:
        try:
            self._execute(query)
        except Exception as exc:
            if not ignore_duplicate or not _is_duplicate_schema_error(exc):
                raise

    def _scalar_int(self, query: str, parameters: dict[str, Any] | None = None) -> int:
        return int(self._first_value(self._records(query, parameters)))

    def _load_csv(self, filename: str, loader: Any) -> int:
        count = 0
        for batch in _iter_csv_batches(self.dataset_dir / filename, self.batch_size):
            loader(batch)
            count += len(batch)
        return count

    def _create_users(self, rows: list[dict[str, str]]) -> None:
        payload = [
            {"id": row["id"], "age": int(row["age"]), "country": row["country"], "score": float(row["score"])}
            for row in rows
        ]
        self._execute(
            "UNWIND $rows AS row CREATE (:User {id: row.id, age: row.age, country: row.country, score: row.score})",
            {"rows": payload},
        )

    def _create_posts(self, rows: list[dict[str, str]]) -> None:
        payload = [{"id": row["id"], "created_at": int(row["created_at"]), "score": float(row["score"])} for row in rows]
        self._execute(
            "UNWIND $rows AS row CREATE (:Post {id: row.id, created_at: row.created_at, score: row.score})",
            {"rows": payload},
        )

    def _create_tags(self, rows: list[dict[str, str]]) -> None:
        payload = [{"id": row["id"], "rank": int(row["rank"])} for row in rows]
        self._execute("UNWIND $rows AS row CREATE (:Tag {id: row.id, rank: row.rank})", {"rows": payload})

    def _create_follows(self, rows: list[dict[str, str]]) -> None:
        self._create_relationship(rows, "User", "FOLLOWS", "User")

    def _create_authored(self, rows: list[dict[str, str]]) -> None:
        self._create_relationship(rows, "User", "AUTHORED", "Post")

    def _create_has_tag(self, rows: list[dict[str, str]]) -> None:
        self._create_relationship(rows, "Post", "HAS_TAG", "Tag")

    def _create_relationship(self, rows: list[dict[str, str]], src_label: str, rel_type: str, dst_label: str) -> None:
        payload = [{"src": row["src"], "dst": row["dst"], "weight": int(row["weight"])} for row in rows]
        self._execute(
            f"UNWIND $rows AS row "
            f"MATCH (src:{src_label} {{id: row.src}}) "
            f"MATCH (dst:{dst_label} {{id: row.dst}}) "
            f"CREATE (src)-[:{rel_type} {{weight: row.weight}}]->(dst)",
            {"rows": payload},
        )

    def _ensure_driver(self) -> None:
        if self._driver is None:
            raise RuntimeError(f"{self.database} adapter is not set up")

    def _ensure_loaded(self) -> None:
        if self._loaded_rows is None:
            self.load_nodes_edges()

    def _first_value(self, result: Any) -> Any:
        if result is None:
            return 0

        single = getattr(result, "single", None)
        if callable(single):
            return self._first_cell(single())

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

        values = getattr(row, "values", None)
        if callable(values):
            values_list = list(values())
            return values_list[0] if values_list else 0

        try:
            return row[0]
        except (KeyError, IndexError, TypeError):
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


class Neo4jAdapter(BoltCypherAdapter):
    env_prefix = "NEO4J"
    default_uri = "bolt://localhost:7687"
    default_user = "neo4j"
    default_password = "password"

    def _create_indexes(self) -> None:
        for label in ["User", "Post", "Tag"]:
            name = f"{label.lower()}_id_unique"
            self._execute_ddl(f"CREATE CONSTRAINT {name} IF NOT EXISTS FOR (n:{label}) REQUIRE n.id IS UNIQUE")
        if self.profile == "indexed":
            self._execute_ddl("CREATE INDEX user_country IF NOT EXISTS FOR (n:User) ON (n.country)")
            self._execute_ddl("CREATE INDEX user_age IF NOT EXISTS FOR (n:User) ON (n.age)")


class MemgraphAdapter(BoltCypherAdapter):
    env_prefix = "MEMGRAPH"
    default_uri = "bolt://localhost:7687"

    def shortest_path_chain(self) -> int:
        self._ensure_loaded()
        return self._scalar_int(
            "MATCH p = (:User {id: $src})-[:FOLLOWS*1..6]->(:User {id: $dst}) RETURN 1 AS count LIMIT 1",
            {"src": "user-000001", "dst": "user-000006"},
        )

    def _create_indexes(self) -> None:
        for label in ["User", "Post", "Tag"]:
            self._execute_ddl(f"CREATE INDEX ON :{label}(id)", ignore_duplicate=True)
        if self.profile == "indexed":
            self._execute_ddl("CREATE INDEX ON :User(country)", ignore_duplicate=True)
            self._execute_ddl("CREATE INDEX ON :User(age)", ignore_duplicate=True)


def _iter_csv_batches(path: Path, batch_size: int) -> Iterable[list[dict[str, str]]]:
    with path.open(newline="") as handle:
        yield from _batched(csv.DictReader(handle), batch_size)


def _batched(rows: Iterable[dict[str, str]], size: int) -> Iterable[list[dict[str, str]]]:
    batch: list[dict[str, str]] = []
    for row in rows:
        batch.append(row)
        if len(batch) >= size:
            yield batch
            batch = []
    if batch:
        yield batch


def _is_duplicate_schema_error(exc: Exception) -> bool:
    message = str(exc).lower()
    return bool(re.search(r"\b(duplicate|already exists|exists already|index exists|equivalent index)\b", message))
