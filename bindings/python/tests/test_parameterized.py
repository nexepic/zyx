"""Tests for parameterized queries with inline property patterns.

Covers:
- CREATE (n:L {prop: $p}) with parameter values stored correctly
- MATCH (n:L {prop: $p}) with inline property parameter resolution
- Traversal with parameterized node/edge properties
- Multiple parameter types (string, int, double, bool)
- Combined CREATE + MATCH parameterized workflows
"""

import zyxdb


# =============================================================================
# Parameterized CREATE — verify properties are stored
# =============================================================================

class TestParameterizedCreate:
    def test_create_param_string(self, db):
        db.execute("CREATE (n:Person {name: $name})", name="Alice")
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name"))
        assert len(rows) == 1
        assert rows[0]["name"] == "Alice"

    def test_create_param_int(self, db):
        db.execute("CREATE (n:T {x: $x})", x=42)
        rows = list(db.execute("MATCH (n:T) RETURN n.x AS x"))
        assert len(rows) == 1
        assert rows[0]["x"] == 42

    def test_create_param_float(self, db):
        db.execute("CREATE (n:T {x: $x})", x=3.14)
        rows = list(db.execute("MATCH (n:T) RETURN n.x AS x"))
        assert len(rows) == 1
        assert abs(rows[0]["x"] - 3.14) < 1e-10

    def test_create_param_bool(self, db):
        db.execute("CREATE (n:T {active: $active})", active=True)
        rows = list(db.execute("MATCH (n:T) RETURN n.active AS active"))
        assert len(rows) == 1
        assert rows[0]["active"] is True

    def test_create_param_multiple_props(self, db):
        db.execute("CREATE (n:Person {name: $name, age: $age})", name="Alice", age=30)
        rows = list(db.execute("MATCH (n:Person) RETURN n.name AS name, n.age AS age"))
        assert len(rows) == 1
        assert rows[0]["name"] == "Alice"
        assert rows[0]["age"] == 30

    def test_create_param_then_literal_match(self, db):
        """CREATE with params, then MATCH with literal to verify stored value."""
        db.execute("CREATE (n:Person {name: $name, age: $age})", name="Alice", age=30)
        rows = list(db.execute("MATCH (n:Person {name: 'Alice'}) RETURN n.age AS age"))
        assert len(rows) == 1
        assert rows[0]["age"] == 30


# =============================================================================
# Parameterized MATCH — inline property pattern with $param
# =============================================================================

class TestParameterizedMatch:
    def test_match_inline_param_string(self, db):
        db.execute("CREATE (n:Person {name: 'Alice', age: 30})")
        db.execute("CREATE (n:Person {name: 'Bob', age: 25})")
        rows = list(db.execute(
            "MATCH (n:Person {name: $name}) RETURN n.age AS age",
            name="Alice",
        ))
        assert len(rows) == 1
        assert rows[0]["age"] == 30

    def test_match_inline_param_int(self, db):
        db.execute("CREATE (n:T {x: 42, label: 'first'})")
        db.execute("CREATE (n:T {x: 99, label: 'second'})")
        rows = list(db.execute(
            "MATCH (n:T {x: $val}) RETURN n.label AS label",
            val=42,
        ))
        assert len(rows) == 1
        assert rows[0]["label"] == "first"

    def test_match_inline_param_float(self, db):
        db.execute("CREATE (n:T {score: 9.5, name: 'good'})")
        db.execute("CREATE (n:T {score: 3.0, name: 'bad'})")
        rows = list(db.execute(
            "MATCH (n:T {score: $s}) RETURN n.name AS name",
            s=9.5,
        ))
        assert len(rows) == 1
        assert rows[0]["name"] == "good"

    def test_match_inline_param_bool(self, db):
        db.execute("CREATE (n:T {active: true, name: 'on'})")
        db.execute("CREATE (n:T {active: false, name: 'off'})")
        rows = list(db.execute(
            "MATCH (n:T {active: $a}) RETURN n.name AS name",
            a=True,
        ))
        assert len(rows) == 1
        assert rows[0]["name"] == "on"

    def test_match_inline_param_multiple_properties(self, db):
        db.execute("CREATE (n:Person {name: 'Alice', age: 30})")
        db.execute("CREATE (n:Person {name: 'Alice', age: 25})")
        db.execute("CREATE (n:Person {name: 'Bob', age: 30})")
        rows = list(db.execute(
            "MATCH (n:Person {name: $name, age: $age}) "
            "RETURN n.name AS name, n.age AS age",
            name="Alice", age=30,
        ))
        assert len(rows) == 1
        assert rows[0]["name"] == "Alice"
        assert rows[0]["age"] == 30

    def test_match_inline_param_no_match(self, db):
        db.execute("CREATE (n:Person {name: 'Alice'})")
        rows = list(db.execute(
            "MATCH (n:Person {name: $name}) RETURN n.name AS name",
            name="NonExistent",
        ))
        assert len(rows) == 0

    def test_match_inline_param_mixed_literal_and_param(self, db):
        db.execute("CREATE (n:Person {name: 'Alice', city: 'NYC', age: 30})")
        db.execute("CREATE (n:Person {name: 'Bob', city: 'NYC', age: 25})")
        rows = list(db.execute(
            "MATCH (n:Person {city: 'NYC', name: $name}) RETURN n.age AS age",
            name="Alice",
        ))
        assert len(rows) == 1
        assert rows[0]["age"] == 30

    def test_match_inline_param_vs_where_equivalent(self, db):
        """Inline {name: $name} should produce same results as WHERE n.name = $name."""
        db.execute("CREATE (n:Person {name: 'Alice', age: 30})")
        db.execute("CREATE (n:Person {name: 'Bob', age: 25})")

        # Inline property
        rows_inline = list(db.execute(
            "MATCH (n:Person {name: $name}) RETURN n.age AS age",
            name="Alice",
        ))
        # WHERE clause
        rows_where = list(db.execute(
            "MATCH (n:Person) WHERE n.name = $name RETURN n.age AS age",
            name="Alice",
        ))
        assert len(rows_inline) == len(rows_where) == 1
        assert rows_inline[0]["age"] == rows_where[0]["age"] == 30


# =============================================================================
# Parameterized MATCH in traversals
# =============================================================================

class TestParameterizedTraversal:
    def test_param_on_source_node(self, db):
        db.execute(
            "CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})"
        )
        db.execute(
            "CREATE (c:Person {name: 'Carol'})-[:KNOWS]->(d:Person {name: 'Dave'})"
        )
        rows = list(db.execute(
            "MATCH (a:Person {name: $name})-[:KNOWS]->(b:Person) "
            "RETURN b.name AS friend",
            name="Alice",
        ))
        assert len(rows) == 1
        assert rows[0]["friend"] == "Bob"

    def test_param_on_target_node(self, db):
        db.execute(
            "CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})"
        )
        db.execute(
            "CREATE (a2:Person {name: 'Alice'})-[:KNOWS]->(c:Person {name: 'Carol'})"
        )
        rows = list(db.execute(
            "MATCH (a:Person)-[:KNOWS]->(b:Person {name: $target}) "
            "RETURN a.name AS person",
            target="Bob",
        ))
        assert len(rows) == 1
        assert rows[0]["person"] == "Alice"

    def test_param_on_both_nodes(self, db):
        db.execute(
            "CREATE (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person {name: 'Bob'})"
        )
        db.execute(
            "CREATE (a2:Person {name: 'Alice'})-[:KNOWS]->(c:Person {name: 'Carol'})"
        )
        db.execute(
            "CREATE (d:Person {name: 'Dave'})-[:KNOWS]->(e:Person {name: 'Bob'})"
        )
        rows = list(db.execute(
            "MATCH (a:Person {name: $src})-[:KNOWS]->(b:Person {name: $dst}) "
            "RETURN a.name AS src, b.name AS dst",
            src="Alice", dst="Bob",
        ))
        assert len(rows) == 1
        assert rows[0]["src"] == "Alice"
        assert rows[0]["dst"] == "Bob"

    def test_param_in_multi_hop_query(self, db):
        """MATCH (a {name: $n})-[:KNOWS]->(b)-[:KNOWS]->(c) with param on start."""
        db.execute(
            "CREATE (a:Person {name: 'Alice'})-[:KNOWS]->"
            "(b:Person {name: 'Bob'})-[:KNOWS]->"
            "(c:Person {name: 'Carol'})"
        )
        rows = list(db.execute(
            "MATCH (a:Person {name: $name})-[:KNOWS]->(b)-[:KNOWS]->(c) "
            "RETURN c.name AS end_node",
            name="Alice",
        ))
        assert len(rows) == 1
        assert rows[0]["end_node"] == "Carol"


# =============================================================================
# Combined CREATE + MATCH with params (end-to-end)
# =============================================================================

class TestParameterizedEndToEnd:
    def test_create_then_match_with_params(self, db):
        """Create nodes with params, then match with params."""
        db.execute("CREATE (n:Person {name: $name, age: $age})", name="Alice", age=30)
        db.execute("CREATE (n:Person {name: $name, age: $age})", name="Bob", age=25)

        rows = list(db.execute(
            "MATCH (n:Person {name: $name}) RETURN n.age AS age",
            name="Bob",
        ))
        assert len(rows) == 1
        assert rows[0]["age"] == 25

    def test_full_graph_workflow_with_params(self, db):
        """Build a graph using only parameterized queries, then query it."""
        # Create nodes
        db.execute("CREATE (n:Person {name: $name})", name="Alice")
        db.execute("CREATE (n:Person {name: $name})", name="Bob")
        db.execute("CREATE (n:Movie {title: $title})", title="Matrix")

        # Create edge using literal MATCH for cross-product join
        db.execute(
            "MATCH (a:Person {name: 'Alice'}), (m:Movie {title: 'Matrix'}) "
            "CREATE (a)-[:RATED {score: 9.5}]->(m)"
        )

        # Query back with params
        rows = list(db.execute(
            "MATCH (p:Person {name: $name})-[r:RATED]->(m:Movie) "
            "RETURN m.title AS title, r.score AS score",
            name="Alice",
        ))
        assert len(rows) == 1
        assert rows[0]["title"] == "Matrix"
        assert rows[0]["score"] == 9.5

    def test_transaction_with_parameterized_match(self, db):
        """Parameterized MATCH works inside transactions."""
        db.execute("CREATE (n:Person {name: 'Alice', age: 30})")
        db.execute("CREATE (n:Person {name: 'Bob', age: 25})")

        with db.begin_transaction() as tx:
            rows = list(tx.execute(
                "MATCH (n:Person {name: $name}) RETURN n.age AS age",
                name="Alice",
            ))
            assert len(rows) == 1
            assert rows[0]["age"] == 30

    def test_read_only_transaction_with_parameterized_match(self, db):
        """Parameterized MATCH works in read-only transactions."""
        db.execute("CREATE (n:Person {name: 'Alice', age: 30})")

        with db.begin_read_only_transaction() as tx:
            rows = list(tx.execute(
                "MATCH (n:Person {name: $name}) RETURN n.age AS age",
                name="Alice",
            ))
            assert len(rows) == 1
            assert rows[0]["age"] == 30
