'use strict';

const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const { Database } = require('../lib/index');

let testCounter = 0;

function getTestDbPath() {
    return path.join(__dirname, `../build/test_db_${++testCounter}_${Date.now()}`);
}

function cleanup(dbPath) {
    if (fs.existsSync(dbPath)) {
        fs.rmSync(dbPath, { recursive: true, force: true });
    }
}

test('Database - open and close', async () => {
    const dbPath = getTestDbPath();
    try {
        const db = new Database(dbPath);
        await db.open();
        assert.strictEqual(db.hasActiveTransaction, false);
        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Database - create node and query', async () => {
    const dbPath = getTestDbPath();
    try {
        const db = new Database(dbPath);
        await db.open();

        const nodeId = await db.createNode('Person', { name: 'Alice', age: 30 });
        assert.strictEqual(typeof nodeId, 'number');

        const result = await db.execute('MATCH (n:Person) RETURN n.name AS name, n.age AS age');
        assert.strictEqual(result.isSuccess, true);
        assert.strictEqual(result.columns.length, 2);
        assert.deepStrictEqual(result.columns, ['name', 'age']);

        const rows = result.fetchAll();
        assert.strictEqual(rows.length, 1);
        assert.strictEqual(rows[0].get('name'), 'Alice');
        assert.strictEqual(rows[0].name, 'Alice'); // Proxy access
        assert.strictEqual(rows[0].get('age'), 30);

        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Database - parameterized query', async () => {
    const dbPath = getTestDbPath();
    try {
        const db = new Database(dbPath);
        await db.open();

        await db.execute('CREATE (n:Person {name: $name, age: $age})', { name: 'Bob', age: 25 });
        const result = await db.execute('MATCH (n:Person {name: $name}) RETURN n.age AS age', { name: 'Bob' });

        assert.strictEqual(result.isSuccess, true);
        const record = result.single();
        assert.strictEqual(record.age, 25);

        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Database - create multiple nodes', async () => {
    const dbPath = getTestDbPath();
    try {
        const db = new Database(dbPath);
        await db.open();

        const ids = await db.createNodes('Person', [
            { name: 'Alice', age: 30 },
            { name: 'Bob', age: 25 },
            { name: 'Charlie', age: 35 }
        ]);

        assert.strictEqual(ids.length, 3);
        assert.strictEqual(typeof ids[0], 'number');

        const result = await db.execute('MATCH (n:Person) RETURN n.name AS name ORDER BY n.age');
        const names = result.fetchAll().map(r => r.name);
        assert.deepStrictEqual(names, ['Bob', 'Alice', 'Charlie']);

        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Database - create edge', async () => {
    const dbPath = getTestDbPath();
    try {
        const db = new Database(dbPath);
        await db.open();

        const id1 = await db.createNode('Person', { name: 'Alice' });
        const id2 = await db.createNode('Person', { name: 'Bob' });
        const edgeId = await db.createEdge(id1, id2, 'KNOWS', { since: 2020 });

        assert.strictEqual(typeof edgeId, 'number');

        const result = await db.execute(
            'MATCH (a:Person)-[r:KNOWS]->(b:Person) RETURN a.name AS from, b.name AS to, r.since AS since'
        );

        const record = result.single();
        assert.strictEqual(record.from, 'Alice');
        assert.strictEqual(record.to, 'Bob');
        assert.strictEqual(record.since, 2020);

        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Transaction - commit', async () => {
    const dbPath = getTestDbPath();
    try {
        const db = new Database(dbPath);
        await db.open();

        const tx = await db.beginTransaction();
        assert.strictEqual(tx.isActive, true);
        assert.strictEqual(tx.isReadOnly, false);

        await tx.execute('CREATE (n:Person {name: $name})', { name: 'Alice' });
        await tx.commit();

        assert.strictEqual(tx.isActive, false);

        const result = await db.execute('MATCH (n:Person) RETURN n.name AS name');
        assert.strictEqual(result.fetchAll().length, 1);

        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Transaction - rollback', async () => {
    const dbPath = getTestDbPath();
    try {
        const db = new Database(dbPath);
        await db.open();

        const tx = await db.beginTransaction();
        await tx.execute('CREATE (n:Person {name: $name})', { name: 'Bob' });
        await tx.rollback();

        const result = await db.execute('MATCH (n:Person) RETURN n.name AS name');
        assert.strictEqual(result.fetchAll().length, 0);

        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Transaction - read-only', async () => {
    const dbPath = getTestDbPath();
    try {
        const db = new Database(dbPath);
        await db.open();

        await db.execute('CREATE (n:Person {name: $name})', { name: 'Alice' });

        const tx = await db.beginReadOnlyTransaction();
        assert.strictEqual(tx.isReadOnly, true);

        const result = await tx.execute('MATCH (n:Person) RETURN n.name AS name');
        assert.strictEqual(result.fetchAll().length, 1);

        await tx.commit();
        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Result - iteration', async () => {
    const dbPath = getTestDbPath();
    try {
        const db = new Database(dbPath);
        await db.open();

        await db.createNodes('Person', [
            { name: 'Alice' },
            { name: 'Bob' },
            { name: 'Charlie' }
        ]);

        const result = await db.execute('MATCH (n:Person) RETURN n.name AS name ORDER BY n.name');

        const names = [];
        for (const record of result) {
            names.push(record.name);
        }

        assert.deepStrictEqual(names, ['Alice', 'Bob', 'Charlie']);

        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Result - scalar', async () => {
    const dbPath = getTestDbPath();
    try {
        const db = new Database(dbPath);
        await db.open();

        await db.execute('CREATE (n:Person {name: $name, age: $age})', { name: 'Alice', age: 30 });
        const result = await db.execute('MATCH (n:Person) RETURN n.age AS age');

        const age = result.scalar();
        assert.strictEqual(age, 30);

        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Error handling - invalid query', async () => {
    const dbPath = getTestDbPath();
    try {
        const db = new Database(dbPath);
        await db.open();

        const result = await db.execute('INVALID CYPHER QUERY');
        assert.strictEqual(result.isSuccess, false);
        assert.strictEqual(typeof result.error, 'string');
        assert.ok(result.error.length > 0);

        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Database - shortest path', async () => {
    const dbPath = getTestDbPath();
    try {
        const db = new Database(dbPath);
        await db.open();

        const id1 = await db.createNode('Person', { name: 'A' });
        const id2 = await db.createNode('Person', { name: 'B' });
        const id3 = await db.createNode('Person', { name: 'C' });

        await db.createEdge(id1, id2, 'KNOWS');
        await db.createEdge(id2, id3, 'KNOWS');
        await db.save();

        const shortestPath = await db.getShortestPath(id1, id3);
        assert.strictEqual(shortestPath.length, 3);
        assert.strictEqual(shortestPath[0].properties.name, 'A');
        assert.strictEqual(shortestPath[2].properties.name, 'C');

        await db.close();
    } finally {
        cleanup(dbPath);
    }
});
