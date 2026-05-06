# zyxdb

Python bindings for [ZYX](https://github.com/nexepic/zyx), a high-performance embeddable graph database engine with Cypher query support.

## Installation

```bash
pip install zyxdb
```

### Build from source

```bash
cd bindings/python
pip install -e ".[test]"
```

## Quick Start

```python
import zyxdb

# Open a database (creates if not exists, auto-closes on exit)
with zyxdb.Database("/tmp/movies") as db:

    # Create nodes — returns node ID (int)
    alice = db.create_node("Person", {"name": "Alice", "age": 30})
    bob   = db.create_node("Person", {"name": "Bob",   "age": 25})
    carol = db.create_node("Person", {"name": "Carol", "age": 35})

    # Create edges between nodes
    db.create_edge(alice, bob,   "FRIENDS_WITH")
    db.create_edge(bob,   carol, "FRIENDS_WITH")

    # Cypher works too — with parameterized queries
    db.execute("CREATE (m:Movie {title: $title, year: $year})",
               title="The Matrix", year=1999)

    # Transactions — all or nothing
    with db.begin_transaction() as tx:
        tx.execute(
            "MATCH (p:Person {name: 'Alice'}), (m:Movie {title: 'The Matrix'}) "
            "CREATE (p)-[:RATED {score: 9.5}]->(m)"
        )
        tx.execute(
            "MATCH (p:Person {name: 'Bob'}), (m:Movie {title: 'The Matrix'}) "
            "CREATE (p)-[:RATED {score: 10.0}]->(m)"
        )
        tx.commit()

    # Query with iteration
    for row in db.execute(
        "MATCH (p:Person)-[r:RATED]->(m:Movie) "
        "RETURN p.name AS person, m.title AS movie, r.score AS score"
    ):
        print(f"{row['person']} rated {row['movie']} {row['score']}/10")

    # Read-only transaction for safe reads
    with db.begin_read_only_transaction() as tx:
        for row in tx.execute("MATCH (p:Person) RETURN count(p) AS cnt"):
            print(f"Total people: {row['cnt']}")

    # Shortest path
    path = db.get_shortest_path(alice, carol)
    names = [n.properties["name"] for n in path]
    print(" -> ".join(names))  # Alice -> Bob -> Carol
```

## Supported Value Types

| Python | ZYX |
|--------|-----|
| `None` | null |
| `bool` | bool |
| `int` | int64 |
| `float` | double |
| `str` | string |
| `list[float]` | vector (embeddings) |
| `list[str]` | string list |
| `list` (mixed) | heterogeneous list |
| `dict` | map |

## API Reference

### `Database(path, *, open=True)`

- `execute(cypher, **params)` — Execute Cypher query with keyword parameters
- `begin_transaction()` — Start a read-write transaction
- `begin_read_only_transaction()` — Start a read-only transaction
- `create_node(label, props)` — Create a node, return its ID (label can be `str` or `list[str]`)
- `create_nodes(label, props_list)` — Batch create nodes, return list of IDs
- `create_edge(src_id, dst_id, edge_type, props)` — Create edge between two nodes by ID
- `create_edges(edge_type, edges)` — Batch create edges
- `get_shortest_path(start_id, end_id, max_depth=15)` — Find shortest path, return list of `Node`
- `bfs(start_id, visitor)` — Breadth-first traversal
- `save()` — Flush to disk
- `close()` — Close the database
- `has_active_transaction` — Whether there is an active transaction

### `Transaction`

- `execute(cypher, **params)` — Execute within transaction
- `commit()` — Commit changes
- `rollback()` — Roll back changes
- `is_active` — Whether transaction is still active
- `is_read_only` — Whether read-only

### `Result`

- Iterable: `for row in result` yields `Record` objects
- `columns` / `column_names` — List of column names
- `is_success` — Whether query succeeded
- `error` — Error message or `None`
- `duration` — Query execution time (ms)
- `fetchone()` — Fetch next row or `None`
- `fetchall()` — Fetch all remaining rows
- `fetchmany(size)` — Fetch up to *size* rows
- `single(strict=True)` — Return the single result row
- `scalar()` — Return first column of first row
- `data()` — Return all rows as list of dicts
- `to_df()` — Convert to pandas DataFrame

## License

Apache License 2.0
