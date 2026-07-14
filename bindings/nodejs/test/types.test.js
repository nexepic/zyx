'use strict';

// Type round-trip, collection values, and Record/node-edge access tests.
// Mirrors bindings/python/tests/test_types.py themes (TestValueRoundtrip,
// TestNodeEdgeValues, TestCollectionValues, TestRecordAccess) so Node has
// the same coverage as the Python benchmark.

const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const { Database } = require('../lib/index');

let testCounter = 0;

function getTestDbPath() {
    return path.join(__dirname, `../build/types_db_${++testCounter}_${Date.now()}`);
}

function withDb(fn) {
    return async () => {
        const dbPath = getTestDbPath();
        const db = new Database(dbPath);
        try {
            await db.open();
            await fn(db);
        } finally {
            try { await db.close(); } catch (_) { /* ignore close errors */ }
            if (fs.existsSync(dbPath)) {
                fs.rmSync(dbPath, { recursive: true, force: true });
            }
        }
    };
}

// --- Value round-trip (one test per scalar type; mirrors Python TestValueRoundtrip) ---

test('Type - null round-trip', withDb(async (db) => {
    await db.createNode('T', { id: 1, v: null });
    const r = await db.execute("MATCH (n:T) RETURN n.v AS v");
    assert.strictEqual(r.single().v, null);
}));

test('Type - boolean true round-trip', withDb(async (db) => {
    await db.createNode('T', { id: 1, v: true });
    assert.strictEqual((await db.execute('MATCH (n:T) RETURN n.v AS v')).single().v, true);
}));

test('Type - boolean false round-trip', withDb(async (db) => {
    await db.createNode('T', { id: 1, v: false });
    assert.strictEqual((await db.execute('MATCH (n:T) RETURN n.v AS v')).single().v, false);
}));

test('Type - integer round-trip', withDb(async (db) => {
    await db.createNode('T', { id: 1, v: 42 });
    assert.strictEqual((await db.execute('MATCH (n:T) RETURN n.v AS v')).single().v, 42);
}));

test('Type - negative integer round-trip', withDb(async (db) => {
    await db.createNode('T', { id: 1, v: -7 });
    assert.strictEqual((await db.execute('MATCH (n:T) RETURN n.v AS v')).single().v, -7);
}));

test('Type - float round-trip', withDb(async (db) => {
    await db.createNode('T', { id: 1, v: 3.14 });
    const val = (await db.execute('MATCH (n:T) RETURN n.v AS v')).single().v;
    assert.ok(Math.abs(val - 3.14) < 1e-6);
}));

test('Type - string round-trip', withDb(async (db) => {
    await db.createNode('T', { id: 1, v: 'hello', name: 'zyx' });
    assert.strictEqual((await db.execute('MATCH (n:T) RETURN n.v AS v')).single().v, 'hello');
}));

test('Type - empty string round-trip', withDb(async (db) => {
    await db.createNode('T', { id: 1, v: '' });
    assert.strictEqual((await db.execute('MATCH (n:T) RETURN n.v AS v')).single().v, '');
}));

test('Type - unicode string round-trip', withDb(async (db) => {
    const s = '你好世界🌍';
    await db.createNode('T', { id: 1, v: s });
    assert.strictEqual((await db.execute('MATCH (n:T) RETURN n.v AS v')).single().v, s);
}));

test('Type - large integer round-trip', withDb(async (db) => {
    const big = 2147483647; // INT32_MAX, safely in int64
    await db.createNode('T', { id: 1, v: big });
    assert.strictEqual((await db.execute('MATCH (n:T) RETURN n.v AS v')).single().v, big);
}));

test('Type - integer via literal', withDb(async (db) => {
    await db.execute('CREATE (n:T {v: 100})');
    assert.strictEqual((await db.execute('MATCH (n:T) RETURN n.v AS v')).single().v, 100);
}));

test('Type - string via literal', withDb(async (db) => {
    await db.execute("CREATE (n:T {v: 'literal'})");
    assert.strictEqual((await db.execute('MATCH (n:T) RETURN n.v AS v')).single().v, 'literal');
}));

// --- Parameterized type queries (mirrors Python TestParameterizedQueries) ---

test('Type - parameterized string in WHERE', withDb(async (db) => {
    await db.createNode('T', { name: 'Alice' });
    await db.createNode('T', { name: 'Bob' });
    const r = await db.execute('MATCH (n:T) WHERE n.name = $name RETURN n.name AS name', { name: 'Bob' });
    assert.strictEqual(r.single().name, 'Bob');
}));

test('Type - parameterized int in WHERE', withDb(async (db) => {
    await db.createNode('T', { id: 1, age: 30 });
    await db.createNode('T', { id: 2, age: 40 });
    const r = await db.execute('MATCH (n:T) WHERE n.age = $age RETURN n.id AS id', { age: 40 });
    assert.strictEqual(r.single().id, 2);
}));

// --- Collection values (mirrors Python TestCollectionValues) ---

test('Type - string list via API round-trips', withDb(async (db) => {
    await db.createNode('T', { tags: ['a', 'b', 'c'] });
    const rows = (await db.execute('MATCH (n:T) RETURN n.tags AS tags')).fetchAll();
    assert.deepStrictEqual(rows[0].tags, ['a', 'b', 'c']);
}));

test('Type - float vector via API round-trips', withDb(async (db) => {
    const vec = [0.1, 0.2, 0.3];
    await db.createNode('T', { embedding: vec });
    const rows = (await db.execute('MATCH (n:T) RETURN n.embedding AS emb')).fetchAll();
    const result = rows[0].emb;
    assert.strictEqual(result.length, 3);
    assert.ok(Math.abs(parseFloat(result[0]) - 0.1) < 1e-5);
}));

// --- Node / edge return round-trip (mirrors Python TestNodeEdgeValues) ---

test('Type - node return shape', withDb(async (db) => {
    await db.createNode('Person', { name: 'Alice', age: 30 });
    const r = await db.execute('MATCH (n:Person) RETURN n');
    const node = r.single().n;
    assert.ok(typeof node === 'object' && node !== null);
    assert.strictEqual(node.properties.name, 'Alice');
    assert.strictEqual(node.properties.age, 30);
}));

test('Type - edge return shape', withDb(async (db) => {
    const a = await db.createNode('Person', { name: 'Alice' });
    const b = await db.createNode('Person', { name: 'Bob' });
    await db.createEdge(a, b, 'KNOWS', { since: 2020 });
    const r = await db.execute('MATCH (a:Person)-[r:KNOWS]->(b:Person) RETURN r');
    const edge = r.single().r;
    assert.ok(typeof edge === 'object' && edge !== null);
    assert.strictEqual(edge.type, 'KNOWS');
    assert.strictEqual(edge.properties.since, 2020);
    assert.strictEqual(edge.sourceId, a);
    assert.strictEqual(edge.targetId, b);
}));

// --- Record access (mirrors Python TestRecordAccess) ---

test('Record - get by string key', withDb(async (db) => {
    await db.createNode('T', { name: 'Ada', age: 36 });
    const rec = (await db.execute('MATCH (n:T) RETURN n.name AS name, n.age AS age')).single();
    assert.strictEqual(rec.get('name'), 'Ada');
    assert.strictEqual(rec.get('age'), 36);
}));

test('Record - get by numeric index', withDb(async (db) => {
    await db.createNode('T', { name: 'Ada' });
    const rec = (await db.execute('MATCH (n:T) RETURN n.name AS name')).single();
    assert.strictEqual(rec.get(0), 'Ada');
}));

test('Record - keys and values', withDb(async (db) => {
    await db.createNode('T', { a: 1, b: 2 });
    const rec = (await db.execute('MATCH (n:T) RETURN n.a AS a, n.b AS b')).single();
    assert.deepStrictEqual(rec.keys(), ['a', 'b']);
    assert.deepStrictEqual(rec.values(), [1, 2]);
}));

test('Record - data() returns full map', withDb(async (db) => {
    await db.createNode('T', { a: 1, b: 'x' });
    const rec = (await db.execute('MATCH (n:T) RETURN n.a AS a, n.b AS b')).single();
    const d = rec.data();
    assert.strictEqual(d.a, 1);
    assert.strictEqual(d.b, 'x');
}));

test('Record - property proxy access', withDb(async (db) => {
    await db.createNode('T', { foo: 99 });
    const rec = (await db.execute('MATCH (n:T) RETURN n.foo AS foo')).single();
    assert.strictEqual(rec.foo, 99); // proxy access must equal get()
    assert.strictEqual(rec.get('foo'), rec.foo);
}));
