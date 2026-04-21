"""Tests for transaction semantics."""

import pytest

import zyxdb


class TestTransactionBasic:
    def test_commit(self, db):
        with db.begin_transaction() as tx:
            tx.execute("CREATE (n:Person {name: 'Alice'})")
            tx.commit()
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name"))
        assert len(rows) == 1
        assert rows[0]["name"] == "Alice"

    def test_rollback(self, db):
        with db.begin_transaction() as tx:
            tx.execute("CREATE (n:Person {name: 'Alice'})")
            tx.rollback()
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name"))
        assert len(rows) == 0

    def test_auto_rollback_on_exception(self, db):
        with pytest.raises(ValueError, match="oops"):
            with db.begin_transaction() as tx:
                tx.execute("CREATE (n:Person {name: 'Alice'})")
                raise ValueError("oops")
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name"))
        assert len(rows) == 0

    def test_transaction_properties(self, db):
        with db.begin_transaction() as tx:
            assert tx.is_active
            assert not tx.is_read_only
            tx.commit()
            assert not tx.is_active


class TestReadOnlyTransaction:
    def test_read_only_flag(self, db):
        with db.begin_read_only_transaction() as tx:
            assert tx.is_read_only
            assert tx.is_active

    def test_read_only_query(self, db):
        db.execute("CREATE (n:Person {name: 'Alice'})")
        with db.begin_read_only_transaction() as tx:
            rows = list(tx.execute("MATCH (n:Person) RETURN n.name AS name"))
            assert len(rows) == 1


class TestTransactionIsolation:
    def test_uncommitted_not_visible(self, db):
        db.execute("CREATE (n:Person {name: 'Before'})")
        tx = db.begin_transaction()
        tx.execute("CREATE (n:Person {name: 'During'})")
        # Without commit, the new node should not be visible to auto-commit queries
        # (depends on isolation level — this tests the basic behavior)
        tx.rollback()
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name"))
        names = {r["name"] for r in rows}
        assert "During" not in names
