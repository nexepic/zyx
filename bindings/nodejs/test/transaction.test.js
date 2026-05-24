'use strict';

const { test } = require('node:test');
const assert = require('node:assert');
const { Transaction } = require('../lib/transaction');

function emptyNativeResult() {
    return { isSuccess: true, error: null, duration: 0, columns: [], rows: [] };
}

test('Transaction - commit waits for queued execute to finish', async () => {
    const events = [];
    let resolveExecute;
    const nativeTx = {
        isActive: true,
        isReadOnly: false,
        execute() {
            events.push('execute-start');
            return new Promise(resolve => {
                resolveExecute = () => {
                    events.push('execute-resolve');
                    resolve(emptyNativeResult());
                };
            });
        },
        commit() {
            events.push('commit-start');
            return Promise.resolve().then(() => {
                events.push('commit-resolve');
                nativeTx.isActive = false;
            });
        }
    };

    const tx = new Transaction(nativeTx);
    const executePromise = tx.execute('RETURN 1 AS value');
    const commitPromise = tx.commit();

    await Promise.resolve();
    await Promise.resolve();
    assert.deepStrictEqual(events, ['execute-start']);

    resolveExecute();
    await Promise.all([executePromise, commitPromise]);

    assert.deepStrictEqual(events, ['execute-start', 'execute-resolve', 'commit-start', 'commit-resolve']);
    assert.strictEqual(tx.isActive, false);
});
