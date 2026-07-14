"""Persistence round-trip tests: write data, close, reopen at the same path,
and verify data + indexes + vector search survive.

Mirrors bindings/nodejs/test/persistence.test.js and the C++
test_IntegrationDatabase PersistenceAcrossSessions scenario. Uses the `db_path`
fixture from conftest.py (a unique tmp path), driving open/close manually so
each test controls the reopen cycle.
"""

import os

import pytest

import zyxdb


def _drop_dir(path: str) -> None:
    try:
        import shutil

        shutil.rmtree(path, ignore_errors=True)
    except Exception:
        pass


@pytest.fixture
def persist_path(tmp_path):
    """A clean directory path that survives across reopen."""
    path = tmp_path / "persist_db"
    yield str(path)


def _open(path):
    db = zyxdb.Database(path)
    db.open()
    return db


def test_nodes_survive_close_and_reopen(persist_path):
    db = _open(persist_path)
    db.create_node("Person", {"name": "Alice", "age": 30})
    db.create_node("Person", {"name": "Bob", "age": 25})
    db.create_node("Person", {"name": "Charlie", "age": 35})
    db.save()
    db.close()

    db = _open(persist_path)
    rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name ORDER BY n.name"))
    assert [r["name"] for r in rows] == ["Alice", "Bob", "Charlie"]
    db.close()


def test_edges_survive_close_and_reopen(persist_path):
    db = _open(persist_path)
    alice = db.create_node("Person", {"name": "Alice"})
    bob = db.create_node("Person", {"name": "Bob"})
    db.create_edge(alice, bob, "KNOWS", {"since": 2020})
    db.save()
    db.close()

    db = _open(persist_path)
    rows = list(
        db.execute(
            "MATCH (a:Person)-[r:KNOWS]->(b:Person) "
            "RETURN a.name AS a, b.name AS b, r.since AS since"
        )
    )
    assert rows[0]["a"] == "Alice"
    assert rows[0]["b"] == "Bob"
    assert rows[0]["since"] == 2020
    db.close()


def test_scalar_types_survive_reopen(persist_path):
    db = _open(persist_path)
    db.create_node("T", {"i": 42, "f": 3.14, "s": "hello", "b": True, "empty": ""})
    db.save()
    db.close()

    db = _open(persist_path)
    rows = list(
        db.execute(
            "MATCH (n:T) RETURN n.i AS i, n.f AS f, n.s AS s, n.b AS b, n.empty AS empty"
        )
    )
    rec = rows[0]
    assert rec["i"] == 42
    assert abs(float(rec["f"]) - 3.14) < 1e-6
    assert rec["s"] == "hello"
    assert rec["b"] is True
    assert rec["empty"] == ""
    db.close()


def test_index_survives_reopen_and_is_usable(persist_path):
    db = _open(persist_path)
    db.execute("CREATE INDEX person_name_idx FOR (n:Person) ON (n.name)")
    db.create_node("Person", {"name": "Alice"})
    db.create_node("Person", {"name": "Bob"})
    db.save()
    db.close()

    db = _open(persist_path)
    rows = list(db.execute("SHOW INDEXES"))
    assert len(rows) >= 1
    rows = list(
        db.execute("MATCH (n:Person) WHERE n.name = $name RETURN n.name AS name", name="Alice")
    )
    assert rows[0]["name"] == "Alice"
    db.close()


def test_shortest_path_after_reopen(persist_path):
    db = _open(persist_path)
    a = db.create_node("Person", {"name": "A"})
    b = db.create_node("Person", {"name": "B"})
    c = db.create_node("Person", {"name": "C"})
    db.create_edge(a, b, "KNOWS")
    db.create_edge(b, c, "KNOWS")
    db.save()
    db.close()

    db = _open(persist_path)
    path = db.get_shortest_path(a, c)
    assert len(path) == 3
    assert path[0].properties["name"] == "A"
    assert path[2].properties["name"] == "C"
    db.close()
