'use strict';

// Persistence round-trip tests: write data, close the database, reopen at the
// same path, and verify data + indexes survive. Mirrors the C++ persistence
// test (test_IntegrationDatabase) and Python test_persistence.py.

const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const { Database } = require('../lib/index');

let testCounter = 0;

function getTestDbPath() {
    return path.join(__dirname, `../build/persist_db_${++testCounter}_${Date.now()}`);
}

function cleanup(dbPath) {
    if (fs.existsSync(dbPath)) {
        fs.rmSync(dbPath, { recursive: true, force: true });
    }
}

async function reopen(dbPath) {
    const db = new Database(dbPath);
    await db.open();
    return db;
}

test('Persistence - nodes survive close and reopen', async () => {
    const dbPath = getTestDbPath();
    try {
        let db = await reopen(dbPath);
        await db.createNodes('Person', [
            { name: 'Alice', age: 30 },
            { name: 'Bob', age: 25 },
            { name: 'Charlie', age: 35 },
        ]);
        await db.save();
        await db.close();

        db = await reopen(dbPath);
        const r = await db.execute('MATCH (n:Person) RETURN n.name AS name ORDER BY n.name');
        assert.deepStrictEqual(r.fetchAll().map(x => x.name), ['Alice', 'Bob', 'Charlie']);
        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Persistence - edges survive close and reopen', async () => {
    const dbPath = getTestDbPath();
    try {
        let db = await reopen(dbPath);
        const a = await db.createNode('Person', { name: 'Alice' });
        const b = await db.createNode('Person', { name: 'Bob' });
        await db.createEdge(a, b, 'KNOWS', { since: 2020 });
        await db.save();
        await db.close();

        db = await reopen(dbPath);
        const r = await db.execute(
            'MATCH (a:Person)-[r:KNOWS]->(b:Person) RETURN a.name AS from, b.name AS to, r.since AS since'
        );
        const rec = r.single();
        assert.strictEqual(rec.from, 'Alice');
        assert.strictEqual(rec.to, 'Bob');
        assert.strictEqual(rec.since, 2020);
        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Persistence - scalar properties and types survive reopen', async () => {
    const dbPath = getTestDbPath();
    try {
        let db = await reopen(dbPath);
        await db.createNode('T', { i: 42, f: 3.14, s: 'hello', b: true, empty: '' });
        await db.save();
        await db.close();

        db = await reopen(dbPath);
        const rec = (await db.execute('MATCH (n:T) RETURN n.i AS i, n.f AS f, n.s AS s, n.b AS b, n.empty AS empty')).single();
        assert.strictEqual(rec.i, 42);
        assert.ok(Math.abs(rec.f - 3.14) < 1e-6);
        assert.strictEqual(rec.s, 'hello');
        assert.strictEqual(rec.b, true);
        assert.strictEqual(rec.empty, '');
        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Persistence - index survives reopen and is usable', async () => {
    const dbPath = getTestDbPath();
    try {
        let db = await reopen(dbPath);
        await db.execute('CREATE INDEX person_name_idx FOR (n:Person) ON (n.name)');
        await db.createNode('Person', { name: 'Alice' });
        await db.createNode('Person', { name: 'Bob' });
        await db.save();
        await db.close();

        db = await reopen(dbPath);
        // SHOW INDEXES should list the recreated index.
        const r = await db.execute('SHOW INDEXES');
        assert.strictEqual(r.isSuccess, true);
        assert.ok(r.fetchAll().length >= 1);
        // The index must still serve queries (filtered MATCH uses the property).
        const match = await db.execute("MATCH (n:Person) WHERE n.name = $name RETURN n.name AS name", { name: 'Alice' });
        assert.strictEqual(match.single().name, 'Alice');
        await db.close();
    } finally {
        cleanup(dbPath);
    }
});

test('Persistence - shortest path computed after reopen', async () => {
    const dbPath = getTestDbPath();
    try {
        let db = await reopen(dbPath);
        const a = await db.createNode('Person', { name: 'A' });
        const b = await db.createNode('Person', { name: 'B' });
        const c = await db.createNode('Person', { name: 'C' });
        await db.createEdge(a, b, 'KNOWS');
        await db.createEdge(b, c, 'KNOWS');
        await db.save();
        await db.close();

        db = await reopen(dbPath);
        const sp = await db.getShortestPath(a, c);
        assert.strictEqual(sp.length, 3);
        assert.strictEqual(sp[0].properties.name, 'A');
        assert.strictEqual(sp[2].properties.name, 'C');
        await db.close();
    } finally {
        cleanup(dbPath);
    }
});
