"""Pythonic transaction wrapper for ZYX database."""

from __future__ import annotations

from typing import Any

from zyxdb.result import Result


class Transaction:
    """Context manager wrapper around the core Transaction.

    Usage::

        with db.begin_transaction() as tx:
            tx.execute("CREATE (n:Person {name: $name})", name="Alice")
            # auto-commits on normal exit

    Auto-commits on normal exit, auto-rolls back on exception.
    """

    def __init__(self, core_tx: Any) -> None:
        self._tx = core_tx

    def execute(self, cypher: str, **params: Any) -> Result:
        """Execute a Cypher query within this transaction."""
        if not self._tx.is_active:
            raise RuntimeError("Cannot execute: transaction is not active")
        return Result(self._tx.execute(cypher, **params))

    def commit(self) -> None:
        """Commit the transaction."""
        if not self._tx.is_active:
            raise RuntimeError("Cannot commit: transaction is not active")
        self._tx.commit()

    def rollback(self) -> None:
        """Roll back the transaction."""
        if not self._tx.is_active:
            raise RuntimeError("Cannot rollback: transaction is not active")
        self._tx.rollback()

    @property
    def is_active(self) -> bool:
        """Whether the transaction is still active."""
        return self._tx.is_active

    @property
    def is_read_only(self) -> bool:
        """Whether this is a read-only transaction."""
        return self._tx.is_read_only

    def __enter__(self) -> Transaction:
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> bool:
        if self._tx.is_active:
            if exc_type is not None:
                self._tx.rollback()
            else:
                self._tx.commit()
        return False

    def __repr__(self) -> str:
        status = "active" if self.is_active else "closed"
        mode = "read-only" if self.is_read_only else "read-write"
        return f"Transaction({status}, {mode})"
