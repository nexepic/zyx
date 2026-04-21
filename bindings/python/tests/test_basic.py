"""Tests for basic CRUD operations."""

import zyxdb


class TestDatabaseLifecycle:
    def test_open_close(self, db_path):
        db = zyxdb.Database(db_path)
        db.close()

    def test_context_manager(self, db_path):
        with zyxdb.Database(db_path) as db:
            db.execute("CREATE (n:Test {x: 1})")

    def test_open_if_exists_missing(self, db_path):
        db = zyxdb.Database(db_path, open=False)
        assert db.open_if_exists() is False

    def test_open_if_exists_present(self, db_path):
        db = zyxdb.Database(db_path)
        db.save()
        db.close()
        db2 = zyxdb.Database(db_path, open=False)
        assert db2.open_if_exists() is True
        db2.close()


class TestCreateNode:
    def test_create_single_label(self, db):
        db.execute("CREATE (n:Person {name: 'Alice'})")
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name"))
        assert len(rows) == 1
        assert rows[0]["name"] == "Alice"

    def test_create_with_api(self, db):
        db.create_node("Person", {"name": "Bob", "age": 25})
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name, n.age AS age"))
        assert len(rows) == 1
        assert rows[0]["name"] == "Bob"
        assert rows[0]["age"] == 25

    def test_create_multi_label(self, db):
        db.create_node(["Person", "Employee"], {"name": "Carol"})
        rows = list(db.execute("MATCH (n:Person:Employee) RETURN n.name AS name"))
        assert len(rows) == 1
        assert rows[0]["name"] == "Carol"

    def test_create_node_ret_id(self, db):
        node_id = db.create_node_ret_id("Person", {"name": "Dave"})
        assert isinstance(node_id, int)

    def test_create_nodes_batch(self, db):
        db.create_nodes("Person", [
            {"name": "Eve", "age": 20},
            {"name": "Frank", "age": 30},
            {"name": "Grace", "age": 40},
        ])
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name ORDER BY n.name"))
        names = [r["name"] for r in rows]
        assert "Eve" in names
        assert "Frank" in names
        assert "Grace" in names


class TestCreateEdge:
    def test_create_edge_by_id(self, db):
        id1 = db.create_node_ret_id("Person", {"name": "Alice"})
        id2 = db.create_node_ret_id("Person", {"name": "Bob"})
        db.create_edge_by_id(id1, id2, "KNOWS", {"since": 2020})
        rows = list(db.execute(
            "MATCH (a:Person)-[r:KNOWS]->(b:Person) "
            "RETURN a.name AS src, b.name AS dst, r.since AS since"
        ))
        assert len(rows) == 1
        assert rows[0]["src"] == "Alice"
        assert rows[0]["dst"] == "Bob"
        assert rows[0]["since"] == 2020


class TestQuery:
    def test_parameterized_query(self, db):
        db.execute("CREATE (n:Person {name: 'Alice', age: 30})")
        rows = list(db.execute(
            "MATCH (n:Person) WHERE n.name = $name RETURN n.age AS age",
            name="Alice",
        ))
        assert len(rows) == 1
        assert rows[0]["age"] == 30

    def test_result_metadata(self, db):
        result = db.execute("CREATE (n:Test {x: 1})")
        assert result.is_success
        assert result.error is None
        assert result.duration >= 0

    def test_result_columns(self, db):
        db.execute("CREATE (n:Person {name: 'Alice'})")
        result = db.execute("MATCH (n:Person) RETURN n.name AS name, n.name AS name2")
        assert "name" in result.column_names
        assert "name2" in result.column_names

    def test_multiple_rows(self, populated_db):
        rows = list(populated_db.execute("MATCH (n:Person) RETURN n.name AS name"))
        names = {r["name"] for r in rows}
        assert names == {"Alice", "Bob"}


class TestShortestPath:
    def test_shortest_path(self, db):
        id1 = db.create_node_ret_id("N", {"val": 1})
        id2 = db.create_node_ret_id("N", {"val": 2})
        id3 = db.create_node_ret_id("N", {"val": 3})
        db.create_edge_by_id(id1, id2, "LINK")
        db.create_edge_by_id(id2, id3, "LINK")
        path = db.get_shortest_path(id1, id3)
        assert len(path) >= 2
