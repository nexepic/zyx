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

    def test_auto_commit_on_normal_exit(self, db):
        with db.begin_transaction() as tx:
            tx.execute("CREATE (n:Person {name: 'AutoCommit'})")
            # No explicit commit — should auto-commit on exit
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name"))
        assert len(rows) == 1
        assert rows[0]["name"] == "AutoCommit"

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

    def test_transaction_repr(self, db):
        tx = db.begin_transaction()
        assert "active" in repr(tx)
        tx.rollback()

    def test_transaction_execute_returns_result(self, db):
        with db.begin_transaction() as tx:
            tx.execute("CREATE (n:Person {name: 'Test'})")
            result = tx.execute("MATCH (n:Person) RETURN n.name AS name")
            assert isinstance(result, zyxdb.Result)
            rows = result.fetchall()
            assert len(rows) == 1

    def test_close_with_active_transaction_keeps_database_open(self, db):
        tx = db.begin_transaction()
        with pytest.raises(zyxdb.DatabaseError):
            db.close()

        assert db.has_active_transaction
        tx.rollback()
        db.execute("CREATE (n:Person {name: 'AfterRollback'})")
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name"))
        assert rows[0]["name"] == "AfterRollback"

    def test_direct_create_node_rejects_active_transaction(self, db):
        tx = db.begin_transaction()
        with pytest.raises(zyxdb.DatabaseError):
            db.create_node("Person", {"name": "Blocked"})

        tx.rollback()
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name"))
        assert rows == []

    def test_native_transaction_rejects_execute_after_commit(self, db):
        tx = db._db.begin_transaction()
        tx.commit()

        with pytest.raises(zyxdb.DatabaseError, match="already closed"):
            tx.execute("RETURN 1 AS value")

    def test_native_transaction_rejects_execute_after_rollback(self, db):
        tx = db._db.begin_transaction()
        tx.rollback()

        with pytest.raises(zyxdb.DatabaseError, match="already closed"):
            tx.execute("RETURN 1 AS value")


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
        tx.rollback()
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name"))
        names = {r["name"] for r in rows}
        assert "During" not in names
