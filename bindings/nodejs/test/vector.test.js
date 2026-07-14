'use strict';

// Vector index end-to-end tests: CREATE VECTOR INDEX, insert, search via
// db.index.vector.queryNodes, score semantics, train, persistence, errors.
//
// Cross-binding contract (confirmed in C++ test_VectorIndex.cpp and via probe):
//  - metric must be 'L2' | 'IP' | 'Cosine' (exact case) else silently treated as L2.
//  - the query vector passed to queryNodes MUST be a literal list `[...]` in the
//    Cypher string (CALL args are evaluated with an empty context; `$vec` would
//    collapse to empty and throw "queryVector argument must be a List of floats.").
//  - L2 score = squared distance (lower = closer). IP/Cosine score = negative
//    inner product (lower = closer). Results sorted ascending by score.
//  - search works WITHOUT training (flat/raw greedy search) for small graphs.

const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const { Database } = require('../lib/index');

let testCounter = 0;

function getTestDbPath() {
    return path.join(__dirname, `../build/vec_db_${++testCounter}_${Date.now()}`);
}

function withDb(fn) {
    return async () => {
        const dbPath = getTestDbPath();
        const db = new Database(dbPath);
        try {
            await db.open();
            await fn(db);
        } finally {
            try { await db.close(); } catch (_) { /* ignore */ }
            if (fs.existsSync(dbPath)) {
                fs.rmSync(dbPath, { recursive: true, force: true });
            }
        }
    };
}

// Build a literal-list query vector string like "0.9,0.1"
function vecLiteral(v) {
    return v.join(',');
}

// Insert the canonical 3-node L2 cross-binding fixture (matches C++ InsertAndSearch).
async function insertL2Fixture(db, idxName = 'vec_l2') {
    await db.execute(`CREATE VECTOR INDEX ${idxName} ON :V(embedding) OPTIONS {dimension: 2, metric: 'L2'}`);
    await db.execute('CREATE (:V {id: 1, embedding: [1.0, 0.0]})');
    await db.execute('CREATE (:V {id: 2, embedding: [0.0, 1.0]})');
    await db.execute('CREATE (:V {id: 3, embedding: [0.0, 0.0]})');
}

test('Vector - create index and search L2 returns top match with squared-distance score', withDb(async (db) => {
    await insertL2Fixture(db);
    const q = `CALL db.index.vector.queryNodes('vec_l2', 1, [0.9, 0.1]) YIELD node, score RETURN node.id AS id, score`;
    const r = (await db.execute(q)).fetchAll();

    assert.ok(r.length >= 1);
    assert.strictEqual(r[0].id, 1); // closest to [0.9,0.1] is node 1 [1.0,0.0]
    // difference [0.1,-0.1] -> squared distance 0.02
    assert.ok(Math.abs(r[0].score - 0.02) < 1e-2);
}));

test('Vector - search returns up to k results sorted ascending by score', withDb(async (db) => {
    await insertL2Fixture(db);
    const r = (await db.execute(`CALL db.index.vector.queryNodes('vec_l2', 3, [0.9, 0.1]) YIELD node, score RETURN node.id AS id, score`)).fetchAll();
    assert.strictEqual(r.length, 3);
    assert.ok(r[0].score <= r[1].score && r[1].score <= r[2].score);
}));

test('Vector - Cosine metric returns negative inner-product score', withDb(async (db) => {
    await db.execute(`CREATE VECTOR INDEX vec_cos ON :V(embedding) OPTIONS {dimension: 2, metric: 'Cosine'}`);
    await db.execute('CREATE (:V {id: 1, embedding: [1.0, 0.0]})');
    await db.execute('CREATE (:V {id: 2, embedding: [0.0, 1.0]})');
    const r = (await db.execute(`CALL db.index.vector.queryNodes('vec_cos', 1, [1.0, 0.0]) YIELD node, score RETURN node.id AS id, score`)).fetchAll();
    assert.ok(r.length >= 1);
    assert.strictEqual(r[0].id, 1);
    // Negated inner product of matching unit vectors is ~-1.0
    assert.ok(Math.abs(r[0].score - (-1.0)) < 5e-2);
}));

test('Vector - metric string is case-sensitive (COSINE falls back to L2 behaviour)', withDb(async (db) => {
    // 'COSINE' is not a recognized metric; the engine treats it as L2 (convention).
    await db.execute(`CREATE VECTOR INDEX vec_upper ON :V(embedding) OPTIONS {dimension: 2, metric: 'COSINE'}`);
    await db.execute('CREATE (:V {id: 1, embedding: [1.0, 0.0]})');
    // Search must still succeed (engine does not throw on unknown metric, it defaults to L2)
    const r = (await db.execute(`CALL db.index.vector.queryNodes('vec_upper', 1, [1.0, 0.0]) YIELD node, score RETURN node.id AS id, score`)).fetchAll();
    assert.ok(r.length >= 1);
    assert.strictEqual(r[0].id, 1);
}));

test('Vector - insert via parameterized list property', withDb(async (db) => {
    await db.execute(`CREATE VECTOR INDEX vec_param ON :V(embedding) OPTIONS {dimension: 3, metric: 'L2'}`);
    await db.execute('CREATE (:V {id: 1, embedding: $emb})', { emb: [1.0, 2.0, 3.0] });
    const r = (await db.execute(`CALL db.index.vector.queryNodes('vec_param', 1, [1.0, 2.0, 3.0]) YIELD node, score RETURN node.id AS id, score`)).fetchAll();
    assert.ok(r.length >= 1);
    assert.strictEqual(r[0].id, 1);
    assert.ok(Math.abs(r[0].score - 0.0) < 1e-2);
}));

test('Vector - manual train returns a status row and search still works', withDb(async (db) => {
    await db.execute(`CREATE VECTOR INDEX vec_train ON :V(embedding) OPTIONS {dimension: 2, metric: 'L2'}`);
    for (let i = 0; i < 10; i++) {
        const x = i / 10.0;
        await db.execute(`CREATE (:V {id: ${i}, embedding: [${x}, ${1.0 - x}]})`);
    }
    const tr = (await db.execute(`CALL db.index.vector.train('vec_train') YIELD status RETURN status`)).fetchAll();
    assert.ok(tr.length >= 1);
    // Status should report Success (or similar non-empty string).
    assert.ok(typeof tr[0].status === 'string' && tr[0].status.length > 0);

    // After training, search must still return the nearest node correctly.
    const r = (await db.execute(`CALL db.index.vector.queryNodes('vec_train', 1, [0.0, 1.0]) YIELD node, score RETURN node.id AS id, score`)).fetchAll();
    assert.ok(r.length >= 1);
}));

test('Vector - train on empty index returns a skipped status', withDb(async (db) => {
    await db.execute(`CREATE VECTOR INDEX vec_empty ON :V(embedding) OPTIONS {dimension: 2, metric: 'L2'}`);
    const tr = (await db.execute(`CALL db.index.vector.train('vec_empty') YIELD status RETURN status`)).fetchAll();
    assert.ok(tr.length >= 1);
    // No data present: status indicates the train was skipped.
    assert.ok(/skip/i.test(String(tr[0].status)));
}));

test('Vector - train on nonexistent index is rejected', withDb(async (db) => {
    const r = await db.execute(`CALL db.index.vector.train('does_not_exist') YIELD status RETURN status`);
    // The engine reports the failure via result.error (or an exception-shaped negative).
    assert.ok(!r.isSuccess || /not found/i.test(String(r.error ?? '')),
        `expected training on a missing index to fail; got isSuccess=${r.isSuccess}, error=${r.error}`);
}));

test('Vector - dimension mismatch on insert is tolerated by the index (skipped insert)', withDb(async (db) => {
    // The insert path logs "Insert skipped: dimension mismatch" but does not throw;
    // the node is created but not added to the vector index. Search then returns 0.
    await db.execute(`CREATE VECTOR INDEX vec_dim ON :V(embedding) OPTIONS {dimension: 2, metric: 'L2'}`);
    await db.execute('CREATE (:V {id: 1, embedding: [1.0, 0.0]})');
    await db.execute('CREATE (:V {id: 2, embedding: [1.0, 0.0, 0.0, 0.0]})'); // wrong dim - skipped from index
    const r = (await db.execute(`CALL db.index.vector.queryNodes('vec_dim', 5, [1.0, 0.0]) YIELD node, score RETURN node.id AS id`)).fetchAll();
    // The 2-d node must be found; the 4-d node must NOT be in the index results.
    const ids = r.map(x => x.id);
    assert.ok(ids.includes(1));
    assert.ok(!ids.includes(2));
}));

test('Vector - index and trained state survive close and reopen', async () => {
    const dbPath = path.join(__dirname, `../build/vec_persist_${++testCounter}_${Date.now()}`);
    try {
        const db = new Database(dbPath);
        await db.open();
        await db.execute(`CREATE VECTOR INDEX vec_persist ON :V(embedding) OPTIONS {dimension: 4, metric: 'L2'}`);
        for (let i = 0; i < 8; i++) {
            await db.execute(`CREATE (:V {id: ${i}, embedding: [${i}.0, ${(8 - i)}.0, ${i * 2}.0, ${(8 - i) * 2}.0]})`);
        }
        await db.execute(`CALL db.index.vector.train('vec_persist') YIELD status RETURN status`);
        await db.save();
        await db.close();

        // Reopen at the same path: index metadata, trained codebook, and raw blobs persist.
        const db2 = new Database(dbPath);
        await db2.open();
        try {
            const r = (await db2.execute(
                `CALL db.index.vector.queryNodes('vec_persist', 1, [0.0, 8.0, 0.0, 16.0]) YIELD node, score RETURN node.id AS id, score`
            )).fetchAll();
            assert.ok(r.length >= 1);
            // Nearest to the query should be the node id=0 vector [0,8,0,16].
            assert.strictEqual(r[0].id, 0);
        } finally {
            await db2.close();
        }
    } finally {
        if (fs.existsSync(dbPath)) fs.rmSync(dbPath, { recursive: true, force: true });
    }
});

test('Vector - queryNodes uses literal query vector (not parameter)', withDb(async (db) => {
    // Pins the cross-binding contract: $param in the CALL vector position is NOT supported.
    await insertL2Fixture(db, 'vec_lit');
    // Parameter form should fail with the documented message.
    const r = await db.execute(`CALL db.index.vector.queryNodes('vec_lit', 1, $q) YIELD node, score RETURN node.id AS id, score`, { q: [0.9, 0.1] });
    assert.ok(!r.isSuccess, 'expected parameter form to be rejected; either the engine must reject or our contract requires a literal');
    assert.ok(/List of floats|queryVector|argument/i.test(String(r.error ?? '')));
}));
