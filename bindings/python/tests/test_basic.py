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

    def test_repr(self, db):
        assert "Database" in repr(db)


class TestCreateNode:
    def test_create_single_label(self, db):
        db.execute("CREATE (n:Person {name: 'Alice'})")
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name"))
        assert len(rows) == 1
        assert rows[0]["name"] == "Alice"

    def test_create_with_api(self, db):
        node_id = db.create_node("Person", {"name": "Bob", "age": 25})
        assert isinstance(node_id, int)
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name, n.age AS age"))
        assert len(rows) == 1
        assert rows[0]["name"] == "Bob"
        assert rows[0]["age"] == 25

    def test_create_multi_label(self, db):
        node_id = db.create_node(["Person", "Employee"], {"name": "Carol"})
        assert isinstance(node_id, int)
        rows = list(db.execute("MATCH (n:Person:Employee) RETURN n.name AS name"))
        assert len(rows) == 1
        assert rows[0]["name"] == "Carol"

    def test_create_node_returns_id(self, db):
        node_id = db.create_node("Person", {"name": "Dave"})
        assert isinstance(node_id, int)

    def test_create_nodes_batch(self, db):
        ids = db.create_nodes("Person", [
            {"name": "Eve", "age": 20},
            {"name": "Frank", "age": 30},
            {"name": "Grace", "age": 40},
        ])
        assert len(ids) == 3
        assert all(isinstance(i, int) for i in ids)
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name ORDER BY n.name"))
        names = [r["name"] for r in rows]
        assert "Eve" in names
        assert "Frank" in names
        assert "Grace" in names


class TestCreateEdge:
    def test_create_edge(self, db):
        id1 = db.create_node("Person", {"name": "Alice"})
        id2 = db.create_node("Person", {"name": "Bob"})
        edge_id = db.create_edge(id1, id2, "KNOWS", {"since": 2020})
        assert isinstance(edge_id, int)
        rows = list(db.execute(
            "MATCH (a:Person)-[r:KNOWS]->(b:Person) "
            "RETURN a.name AS src, b.name AS dst, r.since AS since"
        ))
        assert len(rows) == 1
        assert rows[0]["src"] == "Alice"
        assert rows[0]["dst"] == "Bob"
        assert rows[0]["since"] == 2020

    def test_create_edges_batch(self, db):
        id1 = db.create_node("N", {"val": 1})
        id2 = db.create_node("N", {"val": 2})
        id3 = db.create_node("N", {"val": 3})
        edge_ids = db.create_edges("LINK", [
            (id1, id2, None),
            (id2, id3, {"weight": 5}),
        ])
        assert len(edge_ids) == 2
        assert all(isinstance(i, int) for i in edge_ids)


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
        assert "name" in result.columns
        assert "name2" in result.columns

    def test_multiple_rows(self, populated_db):
        rows = list(populated_db.execute("MATCH (n:Person) RETURN n.name AS name"))
        names = {r["name"] for r in rows}
        assert names == {"Alice", "Bob"}


class TestShortestPath:
    def test_shortest_path(self, db):
        id1 = db.create_node("N", {"val": 1})
        id2 = db.create_node("N", {"val": 2})
        id3 = db.create_node("N", {"val": 3})
        db.create_edge(id1, id2, "LINK")
        db.create_edge(id2, id3, "LINK")
        path = db.get_shortest_path(id1, id3)
        assert len(path) >= 2
        assert all(isinstance(n, zyxdb.Node) for n in path)


class TestBfs:
    def test_bfs_traversal(self, db):
        id1 = db.create_node("N", {"val": 1})
        id2 = db.create_node("N", {"val": 2})
        id3 = db.create_node("N", {"val": 3})
        db.create_edge(id1, id2, "LINK")
        db.create_edge(id2, id3, "LINK")

        visited = []
        db.bfs(id1, lambda node: (visited.append(node["id"]), True)[1])
        assert len(visited) >= 2
