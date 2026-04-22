"""Tests for value type conversions."""

import zyxdb


class TestValueRoundtrip:
    """Test that Python values survive the roundtrip through C++ and back."""

    def test_null(self, db):
        db.execute("CREATE (n:T {x: null})")
        rows = list(db.execute("MATCH (n:T) RETURN n.x AS x"))
        assert rows[0]["x"] is None

    def test_bool_true(self, db):
        db.execute("CREATE (n:T {x: true})")
        rows = list(db.execute("MATCH (n:T) RETURN n.x AS x"))
        assert rows[0]["x"] is True

    def test_bool_false(self, db):
        db.execute("CREATE (n:T {x: false})")
        rows = list(db.execute("MATCH (n:T) RETURN n.x AS x"))
        assert rows[0]["x"] is False

    def test_integer(self, db):
        db.create_node("T", {"x": 42})
        rows = list(db.execute("MATCH (n:T) RETURN n.x AS x"))
        assert rows[0]["x"] == 42
        assert isinstance(rows[0]["x"], int)

    def test_float(self, db):
        db.create_node("T", {"x": 3.14})
        rows = list(db.execute("MATCH (n:T) RETURN n.x AS x"))
        assert abs(rows[0]["x"] - 3.14) < 1e-10
        assert isinstance(rows[0]["x"], float)

    def test_string(self, db):
        db.create_node("T", {"x": "hello world"})
        rows = list(db.execute("MATCH (n:T) RETURN n.x AS x"))
        assert rows[0]["x"] == "hello world"

    def test_empty_string(self, db):
        db.create_node("T", {"x": ""})
        rows = list(db.execute("MATCH (n:T) RETURN n.x AS x"))
        assert rows[0]["x"] == ""

    def test_large_integer(self, db):
        val = 2**53
        db.create_node("T", {"x": val})
        rows = list(db.execute("MATCH (n:T) RETURN n.x AS x"))
        assert rows[0]["x"] == val

    def test_integer_literal(self, db):
        db.execute("CREATE (n:T {x: 42})")
        rows = list(db.execute("MATCH (n:T) RETURN n.x AS x"))
        assert rows[0]["x"] == 42

    def test_string_literal(self, db):
        db.execute("CREATE (n:T {x: 'hello'})")
        rows = list(db.execute("MATCH (n:T) RETURN n.x AS x"))
        assert rows[0]["x"] == "hello"


class TestParameterizedQueries:
    """Test parameter usage in WHERE clauses (supported by engine)."""

    def test_param_in_where(self, db):
        db.execute("CREATE (n:Person {name: 'Alice', age: 30})")
        db.execute("CREATE (n:Person {name: 'Bob', age: 25})")
        rows = list(db.execute(
            "MATCH (n:Person) WHERE n.name = $name RETURN n.age AS age",
            name="Alice",
        ))
        assert len(rows) == 1
        assert rows[0]["age"] == 30

    def test_param_int_in_where(self, db):
        db.execute("CREATE (n:T {x: 42})")
        db.execute("CREATE (n:T {x: 99})")
        rows = list(db.execute(
            "MATCH (n:T) WHERE n.x = $val RETURN n.x AS x",
            val=42,
        ))
        assert len(rows) == 1
        assert rows[0]["x"] == 42


class TestNodeEdgeValues:
    def test_node_return(self, db):
        db.execute("CREATE (n:Person {name: 'Alice', age: 30})")
        rows = list(db.execute("MATCH (n:Person) RETURN n"))
        node = rows[0]["n"]
        assert isinstance(node, dict)
        assert "id" in node
        assert "labels" in node
        assert "properties" in node
        assert node["properties"]["name"] == "Alice"

    def test_edge_return(self, db):
        db.execute(
            "CREATE (a:Person {name: 'A'})-[:KNOWS {since: 2020}]->(b:Person {name: 'B'})"
        )
        rows = list(db.execute("MATCH ()-[r:KNOWS]->() RETURN r"))
        edge = rows[0]["r"]
        assert isinstance(edge, dict)
        assert "source_id" in edge
        assert "target_id" in edge
        assert edge["type"] == "KNOWS"
        assert edge["properties"]["since"] == 2020


class TestCollectionValues:
    def test_string_list_via_api(self, db):
        db.create_node("T", {"tags": ["a", "b", "c"]})
        rows = list(db.execute("MATCH (n:T) RETURN n.tags AS tags"))
        assert rows[0]["tags"] == ["a", "b", "c"]

    def test_float_vector_via_api(self, db):
        vec = [0.1, 0.2, 0.3]
        db.create_node("T", {"embedding": vec})
        rows = list(db.execute("MATCH (n:T) RETURN n.embedding AS emb"))
        result = rows[0]["emb"]
        assert len(result) == 3
        assert abs(float(result[0]) - 0.1) < 1e-5


class TestErrorHandling:
    def test_invalid_cypher(self, db):
        result = db.execute("INVALID SYNTAX HERE")
        assert not result.is_success
        assert result.error is not None

    def test_database_error_exists(self):
        assert hasattr(zyxdb, "DatabaseError")


class TestRecordAccess:
    """Test that Record objects from iteration support dict-like access."""

    def test_record_getitem_str(self, db):
        db.execute("CREATE (n:T {x: 42})")
        for row in db.execute("MATCH (n:T) RETURN n.x AS x"):
            assert row["x"] == 42

    def test_record_getitem_int(self, db):
        db.execute("CREATE (n:T {x: 42})")
        for row in db.execute("MATCH (n:T) RETURN n.x AS x"):
            assert row[0] == 42

    def test_record_get(self, db):
        db.execute("CREATE (n:T {x: 42})")
        for row in db.execute("MATCH (n:T) RETURN n.x AS x"):
            assert row.get("x") == 42
            assert row.get("missing", "default") == "default"

    def test_record_keys_values(self, db):
        db.execute("CREATE (n:T {x: 42})")
        for row in db.execute("MATCH (n:T) RETURN n.x AS x"):
            assert "x" in row.keys()
            assert 42 in row.values()

    def test_record_data(self, db):
        db.execute("CREATE (n:T {x: 42})")
        for row in db.execute("MATCH (n:T) RETURN n.x AS x"):
            d = row.data()
            assert isinstance(d, dict)
            assert d["x"] == 42

    def test_record_repr(self, db):
        db.execute("CREATE (n:T {x: 42})")
        for row in db.execute("MATCH (n:T) RETURN n.x AS x"):
            assert "Record" in repr(row)
