"""Pythonic transaction wrapper for ZYX database."""

from __future__ import annotations

from typing import Any


class Transaction:
    """Context manager wrapper around the core Transaction.

    Usage::

        with db.begin_transaction() as tx:
            tx.execute("CREATE (n:Person {name: $name})", name="Alice")
            tx.commit()

    Auto-rolls back on exception. If no exception and commit() was not called,
    the transaction remains active until garbage collected (auto-rollback).
    """

    def __init__(self, core_tx: Any) -> None:
        self._tx = core_tx

    def execute(self, cypher: str, **params: Any) -> Any:
        """Execute a Cypher query within this transaction."""
        return self._tx.execute(cypher, **params)

    def commit(self) -> None:
        """Commit the transaction."""
        self._tx.commit()

    def rollback(self) -> None:
        """Roll back the transaction."""
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
        return False
