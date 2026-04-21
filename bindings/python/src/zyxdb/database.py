"""Pythonic database wrapper for ZYX graph database."""

from __future__ import annotations

from typing import Any

from zyxdb._core import Database as _CoreDatabase
from zyxdb.transaction import Transaction


class Database:
    """High-level Python interface to the ZYX graph database.

    Usage::

        db = Database("/path/to/db")
        db.execute("CREATE (n:Person {name: $name})", name="Alice")

        for row in db.execute("MATCH (n:Person) RETURN n.name AS name"):
            print(row["name"])

        db.close()

    Also supports context manager::

        with Database("/tmp/mydb") as db:
            db.execute("CREATE (n:Test {x: 1})")
    """

    def __init__(self, path: str, *, open: bool = True) -> None:
        self._db = _CoreDatabase(path)
        if open:
            self._db.open()

    def open(self) -> None:
        """Open the database (creates if not exists)."""
        self._db.open()

    def open_if_exists(self) -> bool:
        """Open the database only if it already exists. Returns True if opened."""
        return self._db.open_if_exists()

    def close(self) -> None:
        """Close the database."""
        self._db.close()

    def save(self) -> None:
        """Flush data to disk."""
        self._db.save()

    def execute(self, cypher: str, **params: Any) -> Any:
        """Execute a Cypher query with optional keyword parameters.

        Returns an iterable Result object.
        """
        return self._db.execute(cypher, **params)

    def begin_transaction(self) -> Transaction:
        """Begin a read-write transaction."""
        return Transaction(self._db.begin_transaction())

    def begin_read_only_transaction(self) -> Transaction:
        """Begin a read-only transaction."""
        return Transaction(self._db.begin_read_only_transaction())

    @property
    def has_active_transaction(self) -> bool:
        """Whether there is an active transaction."""
        return self._db.has_active_transaction

    def set_thread_pool_size(self, size: int) -> None:
        """Resize the thread pool (0=auto, 1=single-thread, >1=N threads)."""
        self._db.set_thread_pool_size(size)

    def create_node(
        self, label: str | list[str], props: dict[str, Any] | None = None
    ) -> None:
        """Create a node with given label(s) and properties."""
        self._db.create_node(label, props or {})

    def create_node_ret_id(
        self, label: str, props: dict[str, Any] | None = None
    ) -> int:
        """Create a node and return its internal ID."""
        return self._db.create_node_ret_id(label, props or {})

    def create_nodes(
        self, label: str, props_list: list[dict[str, Any]]
    ) -> None:
        """Create multiple nodes in one batch."""
        self._db.create_nodes(label, props_list)

    def create_edge(
        self,
        src_label: str,
        src_key: str,
        src_val: Any,
        dst_label: str,
        dst_key: str,
        dst_val: Any,
        edge_type: str,
        props: dict[str, Any] | None = None,
    ) -> None:
        """Create an edge by matching source and target nodes via label+property."""
        self._db.create_edge(
            src_label, src_key, src_val,
            dst_label, dst_key, dst_val,
            edge_type, props or {},
        )

    def create_edge_by_id(
        self,
        src_id: int,
        dst_id: int,
        edge_type: str,
        props: dict[str, Any] | None = None,
    ) -> None:
        """Create an edge directly between two known internal IDs."""
        self._db.create_edge_by_id(src_id, dst_id, edge_type, props or {})

    def get_shortest_path(
        self, start_id: int, end_id: int, max_depth: int = 15
    ) -> list[dict[str, Any]]:
        """Find shortest path between two nodes. Returns list of node dicts."""
        return self._db.get_shortest_path(start_id, end_id, max_depth)

    def __enter__(self) -> Database:
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> bool:
        self.close()
        return False

    def __del__(self) -> None:
        try:
            self._db.close()
        except Exception:
            pass
