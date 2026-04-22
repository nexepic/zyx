"""Pythonic database wrapper for ZYX graph database."""

from __future__ import annotations

from typing import Any

from zyxdb._core import Database as _CoreDatabase
from zyxdb.result import Result
from zyxdb.transaction import Transaction
from zyxdb.types import Node


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

    def execute(self, cypher: str, **params: Any) -> Result:
        """Execute a Cypher query with optional keyword parameters.

        Returns a :class:`Result` wrapper with iteration and convenience methods.
        """
        return Result(self._db.execute(cypher, **params))

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
    ) -> int:
        """Create a node with given label(s) and properties. Returns node ID."""
        return self._db.create_node(label, props or {})

    def create_nodes(
        self, label: str, props_list: list[dict[str, Any]]
    ) -> list[int]:
        """Create multiple nodes in one batch. Returns list of node IDs."""
        return self._db.create_nodes(label, props_list)

    def create_edge(
        self,
        src_id: int,
        dst_id: int,
        edge_type: str,
        props: dict[str, Any] | None = None,
    ) -> int:
        """Create an edge between two nodes by ID. Returns edge ID."""
        return self._db.create_edge(src_id, dst_id, edge_type, props or {})

    def create_edges(
        self,
        edge_type: str,
        edges: list[tuple[int, int, dict[str, Any] | None]],
    ) -> list[int]:
        """Create multiple edges in one batch. Returns list of edge IDs."""
        return self._db.create_edges(edge_type, edges)

    def get_shortest_path(
        self, start_id: int, end_id: int, max_depth: int = 15
    ) -> list[Node]:
        """Find shortest path between two nodes. Returns list of Node objects."""
        raw = self._db.get_shortest_path(start_id, end_id, max_depth)
        return [Node.from_dict(d) for d in raw]

    def bfs(
        self, start_id: int, visitor: Any
    ) -> None:
        """Breadth-first traversal from start_id. Visitor receives node dicts."""
        self._db.bfs(start_id, visitor)

    def __enter__(self) -> Database:
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> bool:
        self.close()
        return False

    def __repr__(self) -> str:
        return f"Database(active_txn={self.has_active_transaction})"

    def __del__(self) -> None:
        try:
            self._db.close()
        except Exception:
            pass
