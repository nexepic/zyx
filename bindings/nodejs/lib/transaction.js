'use strict';

const { Result } = require('./result');

/**
 * Transaction wrapper with async operations.
 *
 * @example
 * const tx = await db.beginTransaction();
 * try {
 *   await tx.execute('CREATE (n:Person {name: $name})', { name: 'Alice' });
 *   await tx.commit();
 * } catch (e) {
 *   await tx.rollback();
 *   throw e;
 * }
 */
class Transaction {
    /**
     * @param {Object} nativeTx - Native transaction object from addon.
     * @private
     */
    constructor(nativeTx) {
        this._tx = nativeTx;
    }

    /**
     * Execute a Cypher query within this transaction.
     * @param {string} cypher - The Cypher query string.
     * @param {Object} [params] - Optional query parameters.
     * @returns {Promise<Result>}
     */
    async execute(cypher, params) {
        if (!this._tx) {
            throw new Error('Transaction already closed');
        }
        const raw = await this._tx.execute(cypher, params || undefined);
        return new Result(raw);
    }

    /**
     * Commit the transaction.
     * @returns {Promise<void>}
     */
    async commit() {
        if (!this._tx) {
            throw new Error('Transaction already closed');
        }
        const tx = this._tx;
        this._tx = null;
        return tx.commit();
    }

    /**
     * Roll back the transaction.
     * @returns {Promise<void>}
     */
    async rollback() {
        if (!this._tx) {
            throw new Error('Transaction already closed');
        }
        const tx = this._tx;
        this._tx = null;
        return tx.rollback();
    }

    /**
     * Whether the transaction is still active.
     * @type {boolean}
     */
    get isActive() {
        return this._tx ? this._tx.isActive : false;
    }

    /**
     * Whether this is a read-only transaction.
     * @type {boolean}
     */
    get isReadOnly() {
        return this._tx ? this._tx.isReadOnly : false;
    }
}

module.exports = { Transaction };
