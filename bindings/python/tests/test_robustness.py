"""Tests for error handling and transaction robustness in Python bindings."""

import pytest

import zyxdb


class TestErrorHandling:
    def test_syntax_error_raises_exception(self, db):
        """db.execute("INVALID") returns error result."""
        result = db.execute("INVALID QUERY SYNTAX")
        assert not result.is_success
        assert result.error is not None

    def test_error_message_is_human_readable(self, db):
        """Error message does not contain raw ANTLR tokens like K_MATCH."""
        result = db.execute("MATC (n) RETRUN n")
        assert not result.is_success
        msg = result.error
        assert "K_MATCH" not in msg
        assert "K_RETURN" not in msg
        assert len(msg) > 0

    def test_valid_query_after_error_succeeds(self, db):
        """Execute bad query, then good query works."""
        bad = db.execute("!!!invalid!!!")
        assert not bad.is_success

        good = db.execute("CREATE (n:Recovery {val: 1})")
        assert good.is_success

        rows = db.execute("MATCH (n:Recovery) RETURN n.val AS v").fetchall()
        assert rows[0]["v"] == 1


class TestTransactionRobustness:
    def test_execute_inside_committed_txn_raises(self, db):
        """tx.commit() then tx.execute() raises."""
        tx = db.begin_transaction()
        tx.execute("CREATE (n:T {v: 1})")
        tx.commit()

        with pytest.raises(RuntimeError):
            tx.execute("MATCH (n) RETURN n")

    def test_commit_twice_raises(self, db):
        """tx.commit(); tx.commit() raises."""
        tx = db.begin_transaction()
        tx.execute("CREATE (n:T {v: 1})")
        tx.commit()

        with pytest.raises(RuntimeError):
            tx.commit()

    def test_rollback_after_commit_raises(self, db):
        """tx.commit(); tx.rollback() raises."""
        tx = db.begin_transaction()
        tx.execute("CREATE (n:T {v: 1})")
        tx.commit()

        with pytest.raises(RuntimeError):
            tx.rollback()

    def test_long_txn_with_error_then_rollback(self, db):
        """5 valid + 1 bad + rollback; verify no data committed."""
        tx = db.begin_transaction()
        for i in range(5):
            r = tx.execute(f"CREATE (n:LongTxn {{id: {i}}})")
            assert r.is_success

        bad = tx.execute("!!!invalid!!!")
        assert not bad.is_success

        tx.rollback()

        rows = db.execute("MATCH (n:LongTxn) RETURN count(n) AS cnt").fetchall()
        assert rows[0]["cnt"] == 0

    def test_sequential_transactions_isolation(self, db):
        """Rollback txn1 (node A), commit txn2 (node B); only B visible."""
        tx1 = db.begin_transaction()
        tx1.execute("CREATE (n:Iso {name: 'A'})")
        tx1.rollback()

        tx2 = db.begin_transaction()
        tx2.execute("CREATE (n:Iso {name: 'B'})")
        tx2.commit()

        rows = db.execute("MATCH (n:Iso) RETURN n.name AS name").fetchall()
        names = [r["name"] for r in rows]
        assert "A" not in names
        assert "B" in names


class TestResultBehavior:
    def test_fetchall_returns_all_rows(self, db):
        """Create 5 nodes, fetchall(), verify len == 5."""
        for i in range(5):
            db.execute(f"CREATE (n:Rows {{id: {i}}})")

        rows = db.execute("MATCH (n:Rows) RETURN n.id AS id").fetchall()
        assert len(rows) == 5

    def test_iteration_gives_same_as_fetchall(self, db):
        """for-loop count == fetchall() count."""
        for i in range(5):
            db.execute(f"CREATE (n:Iter2 {{id: {i}}})")

        result1 = db.execute("MATCH (n:Iter2) RETURN n.id AS id")
        iter_count = sum(1 for _ in result1)

        result2 = db.execute("MATCH (n:Iter2) RETURN n.id AS id")
        fetch_count = len(result2.fetchall())

        assert iter_count == fetch_count == 5

    def test_result_duration_is_non_negative(self, db):
        """result.duration >= 0 for both success and error."""
        good = db.execute("RETURN 1 AS x")
        assert good.duration >= 0

        bad = db.execute("!!!invalid!!!")
        assert bad.duration >= 0
