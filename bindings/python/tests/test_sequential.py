"""Tests for sequential query execution — SIGBUS regression and parser reuse."""

import pytest

import zyxdb


class TestSequentialAutoCommit:
    def test_many_sequential_auto_commit_queries(self, db):
        """30 sequential execute() calls — Python SIGBUS regression test."""
        queries = [
            "CREATE (n:A {id: 1})",
            "CREATE (n:B {id: 2})",
            "MATCH (n:A) RETURN n.id AS id",
            "CREATE (n:C {id: 3})",
            "MATCH (n) RETURN count(n) AS cnt",
            "MATCH (n:B) SET n.name = 'updated'",
            "CREATE (n:D {id: 4})",
            "MATCH (n:D) RETURN n.id AS id",
            "CREATE (n:E {id: 5})",
            "MATCH (n:A) DELETE n",
            "CREATE (n:F {id: 6})",
            "MATCH (n) RETURN count(n) AS cnt",
            "CREATE (n:G {id: 7})",
            "MATCH (n:G) RETURN n.id AS id",
            "CREATE (n:H {id: 8})",
            "MERGE (n:I {id: 9})",
            "MATCH (n:I) RETURN n.id AS id",
            "CREATE (n:J {id: 10})",
            "MATCH (n) RETURN count(n) AS cnt",
            "CREATE (n:K {id: 11})",
            "MATCH (n:K) SET n.flag = true",
            "CREATE (n:L {id: 12})",
            "MATCH (n:L) RETURN n.id AS id",
            "CREATE (n:M {id: 13})",
            "RETURN 42 AS answer",
            "RETURN 'hello' AS msg",
            "CREATE (n:N {id: 14})",
            "MATCH (n:N) RETURN n.id AS id",
            "CREATE (n:O {id: 15})",
            "MATCH (n) RETURN count(n) AS cnt",
        ]
        for q in queries:
            result = db.execute(q)
            assert result.is_success, f"Failed on: {q} — {result.error}"

    def test_sequential_creates_then_aggregate(self, db):
        """Create 15 nodes in loop, then COUNT query."""
        for i in range(15):
            db.execute(f"CREATE (n:Num {{val: {i}}})")

        rows = db.execute("MATCH (n:Num) RETURN count(n) AS cnt").fetchall()
        assert len(rows) == 1
        assert rows[0]["cnt"] == 15

    def test_alternating_write_read_loop(self, db):
        """20 iterations: CREATE then MATCH to verify."""
        for i in range(20):
            db.execute(f"CREATE (n:Step {{i: {i}}})")
            rows = db.execute("MATCH (n:Step) RETURN count(n) AS cnt").fetchall()
            assert rows[0]["cnt"] == i + 1

    def test_syntax_error_then_valid_query_recovers(self, db):
        """Bad query (catch exception), then valid query must succeed."""
        result = db.execute("MATC (n) RETRUN n")
        assert not result.is_success

        result = db.execute("CREATE (n:OK {val: 1})")
        assert result.is_success

        rows = db.execute("MATCH (n:OK) RETURN n.val AS v").fetchall()
        assert rows[0]["v"] == 1

    def test_multiple_errors_interleaved_with_success(self, db):
        """3 bad queries interspersed between valid ones."""
        db.execute("CREATE (n:X {v: 1})")

        result = db.execute("!!invalid!!")
        assert not result.is_success

        db.execute("CREATE (n:X {v: 2})")

        result = db.execute("MATC (n)")
        assert not result.is_success

        db.execute("CREATE (n:X {v: 3})")

        result = db.execute("CREAT (n)")
        assert not result.is_success

        rows = db.execute("MATCH (n:X) RETURN count(n) AS cnt").fetchall()
        assert rows[0]["cnt"] == 3

    def test_sequential_parameterized_queries(self, db):
        """10 sequential parameterized CREATE + MATCH pairs."""
        for i in range(10):
            db.execute("CREATE (n:Param {id: $id})", id=i)
            rows = db.execute(
                "MATCH (n:Param) WHERE n.id = $id RETURN n.id AS id", id=i
            ).fetchall()
            assert len(rows) == 1
            assert rows[0]["id"] == i

    def test_result_iteration_then_next_query(self, db):
        """Partially iterate result, then execute new query — no contamination."""
        for i in range(5):
            db.execute(f"CREATE (n:Iter {{id: {i}}})")

        result = db.execute("MATCH (n:Iter) RETURN n.id AS id")
        # Only read first row
        first = next(iter(result))
        assert "id" in first

        # New query should work fine
        rows = db.execute("MATCH (n:Iter) RETURN count(n) AS cnt").fetchall()
        assert rows[0]["cnt"] == 5

    def test_fifty_sequential_queries(self, db):
        """50 execute() calls building up and tearing down a graph."""
        # Build up
        for i in range(25):
            result = db.execute(f"CREATE (n:Stress {{id: {i}}})")
            assert result.is_success, f"Create #{i} failed: {result.error}"

        rows = db.execute("MATCH (n:Stress) RETURN count(n) AS cnt").fetchall()
        assert rows[0]["cnt"] == 25

        # Tear down
        for i in range(25):
            result = db.execute(f"MATCH (n:Stress {{id: {i}}}) DELETE n")
            assert result.is_success, f"Delete #{i} failed: {result.error}"

        rows = db.execute("MATCH (n:Stress) RETURN count(n) AS cnt").fetchall()
        assert rows[0]["cnt"] == 0
