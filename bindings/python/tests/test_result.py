"""Tests for Result and Record wrapper classes."""

import pytest

import zyxdb
from zyxdb.result import Record, Result


class TestResultWrapper:
    def test_result_is_result_type(self, db):
        result = db.execute("CREATE (n:T {x: 1})")
        assert isinstance(result, Result)

    def test_result_columns(self, db):
        db.execute("CREATE (n:T {x: 1})")
        result = db.execute("MATCH (n:T) RETURN n.x AS x")
        assert "x" in result.columns

    def test_result_is_success(self, db):
        result = db.execute("CREATE (n:T {x: 1})")
        assert result.is_success

    def test_result_error_on_bad_query(self, db):
        result = db.execute("INVALID SYNTAX")
        assert not result.is_success
        assert result.error is not None

    def test_result_duration(self, db):
        result = db.execute("CREATE (n:T {x: 1})")
        assert result.duration >= 0

    def test_result_repr(self, db):
        result = db.execute("CREATE (n:T {x: 1})")
        assert "Result" in repr(result)

    def test_result_column_names_compat(self, db):
        db.execute("CREATE (n:T {x: 1})")
        result = db.execute("MATCH (n:T) RETURN n.x AS x")
        assert result.column_names == result.columns


class TestResultIteration:
    def test_iter_yields_records(self, db):
        db.execute("CREATE (n:T {x: 1})")
        for row in db.execute("MATCH (n:T) RETURN n.x AS x"):
            assert isinstance(row, Record)

    def test_fetchone(self, db):
        db.execute("CREATE (n:T {x: 1})")
        result = db.execute("MATCH (n:T) RETURN n.x AS x")
        row = result.fetchone()
        assert row is not None
        assert row["x"] == 1

    def test_fetchone_empty(self, db):
        result = db.execute("MATCH (n:T) RETURN n.x AS x")
        row = result.fetchone()
        assert row is None

    def test_fetchall(self, db):
        db.execute("CREATE (n:T {x: 1})")
        db.execute("CREATE (n:T {x: 2})")
        rows = db.execute("MATCH (n:T) RETURN n.x AS x").fetchall()
        assert len(rows) == 2
        vals = {r["x"] for r in rows}
        assert vals == {1, 2}

    def test_fetchmany(self, db):
        db.execute("CREATE (n:T {x: 1})")
        db.execute("CREATE (n:T {x: 2})")
        db.execute("CREATE (n:T {x: 3})")
        rows = db.execute("MATCH (n:T) RETURN n.x AS x").fetchmany(2)
        assert len(rows) == 2

    def test_single(self, db):
        db.execute("CREATE (n:T {x: 42})")
        row = db.execute("MATCH (n:T) RETURN n.x AS x").single()
        assert row["x"] == 42

    def test_single_strict_multiple(self, db):
        db.execute("CREATE (n:T {x: 1})")
        db.execute("CREATE (n:T {x: 2})")
        with pytest.raises(ValueError, match="Expected exactly 1 row"):
            db.execute("MATCH (n:T) RETURN n.x AS x").single()

    def test_single_strict_empty(self, db):
        with pytest.raises(ValueError):
            db.execute("MATCH (n:T) RETURN n.x AS x").single()

    def test_scalar(self, db):
        db.execute("CREATE (n:T {x: 42})")
        val = db.execute("MATCH (n:T) RETURN n.x AS x").scalar()
        assert val == 42

    def test_data(self, db):
        db.execute("CREATE (n:T {x: 1})")
        data = db.execute("MATCH (n:T) RETURN n.x AS x").data()
        assert isinstance(data, list)
        assert len(data) == 1
        assert data[0]["x"] == 1


class TestRecord:
    def test_getitem_str(self):
        rec = Record({"a": 1, "b": 2}, ["a", "b"])
        assert rec["a"] == 1

    def test_getitem_int(self):
        rec = Record({"a": 1, "b": 2}, ["a", "b"])
        assert rec[0] == 1
        assert rec[1] == 2

    def test_get_default(self):
        rec = Record({"a": 1}, ["a"])
        assert rec.get("a") == 1
        assert rec.get("missing", "default") == "default"

    def test_keys(self):
        rec = Record({"a": 1, "b": 2}, ["a", "b"])
        assert rec.keys() == ["a", "b"]

    def test_values(self):
        rec = Record({"a": 1, "b": 2}, ["a", "b"])
        assert rec.values() == [1, 2]

    def test_data(self):
        rec = Record({"a": 1}, ["a"])
        assert rec.data() == {"a": 1}

    def test_repr(self):
        rec = Record({"a": 1}, ["a"])
        assert "Record" in repr(rec)
        assert "a=1" in repr(rec)

    def test_eq(self):
        rec1 = Record({"a": 1}, ["a"])
        rec2 = Record({"a": 1}, ["a"])
        assert rec1 == rec2

    def test_len(self):
        rec = Record({"a": 1, "b": 2}, ["a", "b"])
        assert len(rec) == 2
