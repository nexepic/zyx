# zyxdb

Node.js bindings for [ZYX](https://github.com/nexepic/zyx), a high-performance embeddable graph database engine with Cypher query support.

## Installation

```bash
npm install zyxdb
```

Prebuilt native binaries are available for:

| Platform | Architecture |
|----------|-------------|
| Linux | x64 (glibc) |
| macOS | ARM64 (Apple Silicon) |
| Windows | x64 (MSVC) |

### Build from source

```bash
cd bindings/nodejs
npm install
```

Requires CMake, a C++20 compiler, and Conan 2 for dependencies.

## Quick Start

```js
const { Database } = require('zyxdb');

const db = new Database('/tmp/movies');
await db.open();

// Create nodes — returns node ID (number)
const alice = await db.createNode('Person', { name: 'Alice', age: 30 });
const bob   = await db.createNode('Person', { name: 'Bob',   age: 25 });
const carol = await db.createNode('Person', { name: 'Carol', age: 35 });

// Create edges between nodes
await db.createEdge(alice, bob,   'FRIENDS_WITH');
await db.createEdge(bob,   carol, 'FRIENDS_WITH');

// Cypher works too — with parameterized queries
await db.execute(
    'CREATE (m:Movie {title: $title, year: $year})',
    { title: 'The Matrix', year: 1999 }
);

// Transactions — all or nothing
const tx = await db.beginTransaction();
await tx.execute(
    "MATCH (p:Person {name: 'Alice'}), (m:Movie {title: 'The Matrix'}) " +
    "CREATE (p)-[:RATED {score: 9.5}]->(m)"
);
await tx.execute(
    "MATCH (p:Person {name: 'Bob'}), (m:Movie {title: 'The Matrix'}) " +
    "CREATE (p)-[:RATED {score: 10.0}]->(m)"
);
await tx.commit();

// Query with iteration
const result = await db.execute(
    'MATCH (p:Person)-[r:RATED]->(m:Movie) ' +
    'RETURN p.name AS person, m.title AS movie, r.score AS score'
);
for (const row of result) {
    console.log(`${row.person} rated ${row.movie} ${row.score}/10`);
}

// Read-only transaction for safe reads
const roTx = await db.beginReadOnlyTransaction();
const counts = await roTx.execute('MATCH (p:Person) RETURN count(p) AS cnt');
console.log(`Total people: ${counts.scalar()}`);
await roTx.commit();

// Shortest path
const path = await db.getShortestPath(alice, carol);
const names = path.map(n => n.properties.name);
console.log(names.join(' -> ')); // Alice -> Bob -> Carol

await db.close();
```

## Supported Value Types

| JavaScript | ZYX |
|------------|-----|
| `null` | null |
| `boolean` | bool |
| `number` (integer) | int64 |
| `number` (float) | double |
| `string` | string |
| `number[]` | vector (embeddings) |
| `string[]` | string list |
| `Array` (mixed) | heterogeneous list |
| `Object` | map |

## API Reference

### `Database(dbPath)`

- `open()` — Open the database (creates if not exists)
- `close()` — Close the database
- `save()` — Flush data to disk
- `execute(cypher, params?)` — Execute Cypher query with optional parameters
- `beginTransaction()` — Start a read-write transaction
- `beginReadOnlyTransaction()` — Start a read-only transaction
- `createNode(label, props?)` — Create a node, return its ID (`label` can be `string` or `string[]`)
- `createNodes(label, propsList)` — Batch create nodes, return array of IDs
- `createEdge(srcId, dstId, edgeType, props?)` — Create edge between two nodes by ID
- `createEdges(edgeType, edges)` — Batch create edges
- `getShortestPath(startId, endId, maxDepth?)` — Find shortest path, return array of `Node`
- `hasActiveTransaction` — Whether there is an active transaction

All methods return `Promise` — use `await` or `.then()`.

### `Transaction`

- `execute(cypher, params?)` — Execute within transaction
- `commit()` — Commit changes
- `rollback()` — Roll back changes
- `isActive` — Whether transaction is still active
- `isReadOnly` — Whether read-only

### `Result`

- Iterable: `for (const row of result)` yields `Record` objects
- `columns` — Array of column names
- `isSuccess` — Whether query succeeded
- `error` — Error message or `null`
- `duration` — Query execution time (ms)
- `records` — All records as array
- `fetchAll()` — Fetch all records
- `single(strict?)` — Return the single result record
- `scalar()` — Return first column of first row
- `data()` — Return all rows as array of plain objects

### `Record`

- `get(key)` — Get value by column name or index
- `keys()` — Array of column names
- `values()` — Array of values
- `data()` — Plain object of key-value pairs
- `length` — Number of columns
- Property access: `row.name` is equivalent to `row.get('name')`

## TypeScript Support

Full TypeScript definitions are included:

```typescript
import { Database, Result, Record, Transaction } from 'zyxdb';
```

## License

Apache License 2.0
