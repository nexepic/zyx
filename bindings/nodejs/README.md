# ZYX Node.js Bindings

Node.js bindings for the ZYX graph database. Provides a fully asynchronous API using native C++ addons.

## Installation

```bash
npm install zyxdb
```

## Quick Start

```javascript
const { Database } = require('zyxdb');

(async () => {
    const db = new Database('/path/to/db');
    await db.open();

    // Create nodes
    const aliceId = await db.createNode('Person', { name: 'Alice', age: 30 });
    const bobId = await db.createNode('Person', { name: 'Bob', age: 25 });

    // Create relationship
    await db.createEdge(aliceId, bobId, 'KNOWS', { since: 2020 });

    // Query with Cypher
    const result = await db.execute(
        'MATCH (a:Person)-[r:KNOWS]->(b:Person) RETURN a.name, b.name, r.since'
    );

    for (const row of result) {
        console.log(`${row['a.name']} knows ${row['b.name']} since ${row['r.since']}`);
    }

    await db.close();
})();
```

## API Reference

### Database

#### Constructor

```javascript
const db = new Database(dbPath);
```

#### Methods

- `open()` - Open the database (creates if not exists)
- `close()` - Close the database
- `save()` - Flush data to disk
- `execute(cypher, params?)` - Execute a Cypher query
- `beginTransaction()` - Begin a read-write transaction
- `beginReadOnlyTransaction()` - Begin a read-only transaction
- `createNode(label, props?)` - Create a single node
- `createNodes(label, propsList)` - Create multiple nodes in batch
- `createEdge(srcId, dstId, type, props?)` - Create an edge
- `createEdges(type, edges)` - Create multiple edges in batch
- `getShortestPath(startId, endId, maxDepth?)` - Find shortest path

All methods return Promises.

#### Properties

- `hasActiveTransaction` - Whether there is an active transaction

### Transaction

```javascript
const tx = await db.beginTransaction();
try {
    await tx.execute('CREATE (n:Person {name: $name})', { name: 'Alice' });
    await tx.commit();
} catch (e) {
    await tx.rollback();
    throw e;
}
```

#### Methods

- `execute(cypher, params?)` - Execute a query within the transaction
- `commit()` - Commit the transaction
- `rollback()` - Roll back the transaction

#### Properties

- `isActive` - Whether the transaction is still active
- `isReadOnly` - Whether this is a read-only transaction

### Result

Query results support iteration and convenience methods:

```javascript
const result = await db.execute('MATCH (n:Person) RETURN n.name, n.age');

// Iteration
for (const row of result) {
    console.log(row['n.name'], row['n.age']);
}

// Fetch all rows
const rows = result.fetchAll();

// Get single row
const row = result.single();

// Get scalar value (first column of first row)
const count = (await db.execute('MATCH (n) RETURN count(n)')).scalar();

// Convert to plain objects
const data = result.data();
```

#### Properties

- `columns` - Array of column names
- `isSuccess` - Whether the query succeeded
- `error` - Error message (null if successful)
- `duration` - Query execution time in milliseconds
- `records` - Array of Record objects

### Record

Individual result rows with convenient access:

```javascript
const row = result.single();

// Access by column name
console.log(row['n.name']);
console.log(row.get('n.name'));

// Access by index
console.log(row.get(0));

// Get all keys/values
console.log(row.keys());
console.log(row.values());

// Convert to plain object
const obj = row.data();
```

## Parameterized Queries

Use parameterized queries to prevent injection and improve performance:

```javascript
await db.execute(
    'CREATE (n:Person {name: $name, age: $age})',
    { name: 'Alice', age: 30 }
);

const result = await db.execute(
    'MATCH (n:Person {name: $name}) RETURN n',
    { name: 'Alice' }
);
```

## Batch Operations

Create multiple nodes or edges efficiently:

```javascript
// Batch create nodes
const ids = await db.createNodes('Person', [
    { name: 'Alice', age: 30 },
    { name: 'Bob', age: 25 },
    { name: 'Charlie', age: 35 }
]);

// Batch create edges
await db.createEdges('KNOWS', [
    [ids[0], ids[1], { since: 2020 }],
    [ids[1], ids[2], { since: 2021 }],
    [ids[0], ids[2], null] // No properties
]);
```

## TypeScript Support

Full TypeScript definitions are included:

```typescript
import { Database, Result, Record, Transaction } from 'zyxdb';

const db = new Database('/path/to/db');
await db.open();

const result: Result = await db.execute('MATCH (n) RETURN n');
for (const record of result) {
    console.log(record.data());
}
```

## Error Handling

All async operations can throw errors. Query errors are captured in the Result object:

```javascript
try {
    const result = await db.execute('INVALID QUERY');
    if (!result.isSuccess) {
        console.error('Query failed:', result.error);
    }
} catch (e) {
    console.error('Database error:', e);
}
```

## Building from Source

```bash
npm install
npm run build
npm test
```

### Requirements

- Node.js >= 18.0.0
- CMake >= 3.15
- C++20 compiler
- ANTLR4 runtime
- Boost
- zlib

## License

Apache-2.0
