'use strict';

// Transaction lifecycle, isolation (rollback semantics), and robustness tests.
// Mirrors bindings/python/tests/test_transactions.py themes:
// TestTransactionIsolation, TestTransactionRobustness, and the reject-on-reuse
// and reject-while-active cases in TestTransactionBasic.
//
// Engine behaviour confirmed by probe (not assumptions):
//  - execute after commit / rollback is rejected with "Transaction already closed".
//  - closing the database while a transaction is active is rejected, but the
//    database stays usable after the transaction is rolled back.
//  - direct createNode while a transaction is active is rejected (single-writer).
//  - rolled-back writes are NOT visible to subsequent reads.
// Note: the engine permits autocommit writes while a write transaction is active
// (they do NOT throw), so we do not assert otherwise — see probe notes in plan.

const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const { Database } = require('../lib/index');

let testCounter = 0;

function getTestDbPath() {
    return path.join(__dirname, `../build/txn_db_${++testCounter}_${Date.now()}`);
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

async function count(db) {
    const r = await db.execute('MATCH (n:Person) RETURN count(n) AS c');
    return r.single().c;
}

// --- Isolation / rollback visibility (mirrors Python test_uncommitted_not_visible) ---

test('Transaction - rolled-back writes are not visible afterwards', withDb(async (db) => {
    await db.execute('CREATE (n:Person {name: $name})', { name: 'Before' });
    const tx = await db.beginTransaction();
    await tx.execute('CREATE (n:Person {name: $name})', { name: 'During' });
    await tx.rollback();

    const rows = (await db.execute('MATCH (n:Person) RETURN n.name AS name')).fetchAll();
    const names = new Set(rows.map(r => r.name));
    assert.ok(!names.has('During'));
    assert.ok(names.has('Before'));
}));

test('Transaction - committed writes are visible afterwards', withDb(async (db) => {
    await db.execute('CREATE (n:Person {name: $name})', { name: 'Before' });
    const tx = await db.beginTransaction();
    await tx.execute('CREATE (n:Person {name: $name})', { name: 'During' });
    await tx.commit();

    const rows = (await db.execute('MATCH (n:Person) RETURN n.name AS name')).fetchAll();
    const names = new Set(rows.map(r => r.name));
    assert.ok(names.has('During'));
    assert.ok(names.has('Before'));
}));

// --- Robustness / contract (mirrors Python reject tests, exact messages from probe) ---

test('Transaction - execute after commit is rejected (already closed)', withDb(async (db) => {
    const tx = await db.beginTransaction();
    await tx.execute('CREATE (n:Person {name: $name})', { name: 'Alice' });
    await tx.commit();
    assert.strictEqual(tx.isActive, false);
    // The wrapper throws synchronously once the underlying native tx is nulled
    // (see lib/transaction.js:36). Use assert.throws for the sync contract.
    assert.throws(
        () => tx.execute('CREATE (n:Person {name: $name})', { name: 'Bob' }),
        /already closed/i,
    );
}));

test('Transaction - execute after rollback is rejected (already closed)', withDb(async (db) => {
    const tx = await db.beginTransaction();
    await tx.execute('CREATE (n:Person {name: $name})', { name: 'Alice' });
    await tx.rollback();
    assert.strictEqual(tx.isActive, false);
    assert.throws(
        () => tx.execute('CREATE (n:Person {name: $name})', { name: 'Bob' }),
        /already closed/i,
    );
}));

test('Transaction - isActive tracks lifecycle', withDb(async (db) => {
    const tx = await db.beginTransaction();
    assert.strictEqual(tx.isActive, true);
    await tx.execute('CREATE (n:Person {name: $name})', { name: 'Alice' });
    await tx.commit();
    assert.strictEqual(tx.isActive, false);
}));

test('Transaction - close while a transaction is active is rejected, db stays usable', withDb(async (db) => {
    const tx = await db.beginTransaction();
    await tx.execute('CREATE (n:Person {name: $name})', { name: 'InTx' });
    // Closing must be rejected because a writer transaction is still active.
    await assert.rejects(() => db.close(), /active transaction|active/i);
    assert.strictEqual(db.hasActiveTransaction, true);

    // After releasing the transaction, the database is still usable.
    await tx.rollback();
    await db.execute('CREATE (n:Person {name: $name})', { name: 'AfterRollback' });
    const r = (await db.execute('MATCH (n:Person) RETURN n.name AS name')).fetchAll();
    assert.ok(r.some(x => x.name === 'AfterRollback'));
}));

test('Transaction - direct createNode rejected while a transaction is active', withDb(async (db) => {
    const tx = await db.beginTransaction();
    await assert.rejects(() => db.createNode('Person', { name: 'Blocked' }), /active transaction/i);
    await tx.rollback();
    const rows = (await db.execute('MATCH (n:Person) RETURN n.name AS name')).fetchAll();
    assert.strictEqual(rows.length, 0);
}));

test('Transaction - read-only transaction exposes flags and serves reads', withDb(async (db) => {
    await db.execute('CREATE (n:Person {name: $name})', { name: 'Alice' });
    const tx = await db.beginReadOnlyTransaction();
    assert.strictEqual(tx.isReadOnly, true);
    assert.strictEqual(tx.isActive, true);

    const r = await tx.execute('MATCH (n:Person) RETURN n.name AS name');
    assert.strictEqual(r.fetchAll().length, 1);
    await tx.rollback();
}));

test('Transaction - multiple sequential transactions', withDb(async (db) => {
    for (const name of ['A', 'B', 'C']) {
        const tx = await db.beginTransaction();
        await tx.execute('CREATE (n:Person {name: $name})', { name });
        await tx.commit();
    }
    assert.strictEqual(await count(db), 3);
}));
