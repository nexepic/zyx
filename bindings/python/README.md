# zyxdb

Python bindings for [ZYX](https://github.com/nicklauslititz/zyx), a high-performance embeddable graph database engine with Cypher query support.

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

# Open a database (creates if not exists)
db = zyxdb.Database("/tmp/mydb")

# Create nodes with Cypher
db.execute("CREATE (n:Person {name: $name, age: $age})", name="Alice", age=30)

# Query with iteration
for row in db.execute("MATCH (n:Person) RETURN n.name AS name, n.age AS age"):
    print(row["name"], row["age"])

# Batch insert
db.create_nodes("Person", [
    {"name": "Bob", "age": 25},
    {"name": "Carol", "age": 35},
])

# Transactions with context manager
with db.begin_transaction() as tx:
    tx.execute("CREATE (n:Movie {title: 'Matrix'})")
    tx.execute(
        "MATCH (a:Person {name: 'Alice'}), (m:Movie {title: 'Matrix'}) "
        "CREATE (a)-[:WATCHED]->(m)"
    )
    tx.commit()

# Read-only transaction
with db.begin_read_only_transaction() as tx:
    for row in tx.execute("MATCH (n) RETURN count(n) AS cnt"):
        print(row["cnt"])

# Direct node/edge creation by ID (high performance)
id1 = db.create_node_ret_id("Person", {"name": "Dave"})
id2 = db.create_node_ret_id("Person", {"name": "Eve"})
db.create_edge_by_id(id1, id2, "KNOWS", {"since": 2024})

# Shortest path
path = db.get_shortest_path(id1, id2)

db.close()
```

## Context Manager

```python
with zyxdb.Database("/tmp/mydb") as db:
    db.execute("CREATE (n:Test {x: 1})")
    # Auto-closes on exit
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
- `create_node(label, props)` — Create a node (label can be `str` or `list[str]`)
- `create_node_ret_id(label, props)` — Create a node, return its ID
- `create_nodes(label, props_list)` — Batch create nodes
- `create_edge_by_id(src_id, dst_id, type, props)` — Create edge between known IDs
- `get_shortest_path(start_id, end_id, max_depth=15)` — Find shortest path
- `save()` — Flush to disk
- `close()` — Close the database

### `Transaction`

- `execute(cypher, **params)` — Execute within transaction
- `commit()` — Commit changes
- `rollback()` — Roll back changes
- `is_active` — Whether transaction is still active
- `is_read_only` — Whether read-only

### `Result`

- Iterable: `for row in result` yields `dict[str, Any]`
- `column_names` — List of column names
- `duration` — Query execution time (ms)
- `is_success` — Whether query succeeded
- `error` — Error message or `None`

## License

Apache License 2.0
