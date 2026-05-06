'use strict';

const { Result } = require('./result');
const { Transaction } = require('./transaction');
const addon = require('./native');

/**
 * High-level async interface to the ZYX graph database.
 *
 * @example
 * const db = new Database('/path/to/db');
 * await db.open();
 * const result = await db.execute('MATCH (n:Person) RETURN n.name');
 * for (const row of result) { console.log(row.name); }
 * await db.close();
 */
class Database {
    /**
     * @param {string} dbPath - Path to the database directory.
     */
    constructor(dbPath) {
        this._db = new addon.Database(dbPath);
    }

    /**
     * Open the database (creates if not exists).
     * @returns {Promise<void>}
     */
    async open() {
        return this._db.open();
    }

    /**
     * Close the database.
     * @returns {Promise<void>}
     */
    async close() {
        return this._db.close();
    }

    /**
     * Flush data to disk.
     * @returns {Promise<void>}
     */
    async save() {
        return this._db.save();
    }

    /**
     * Execute a Cypher query with optional parameters.
     * @param {string} cypher - The Cypher query string.
     * @param {Object} [params] - Optional query parameters.
     * @returns {Promise<Result>}
     */
    async execute(cypher, params) {
        const raw = await this._db.execute(cypher, params || undefined);
        return new Result(raw);
    }

    /**
     * Begin a read-write transaction.
     * @returns {Promise<Transaction>}
     */
    async beginTransaction() {
        const nativeTx = this._db.beginTransaction();
        return new Transaction(nativeTx);
    }

    /**
     * Begin a read-only transaction.
     * @returns {Promise<Transaction>}
     */
    async beginReadOnlyTransaction() {
        const nativeTx = this._db.beginReadOnlyTransaction();
        return new Transaction(nativeTx);
    }

    /**
     * Whether there is an active transaction.
     * @type {boolean}
     */
    get hasActiveTransaction() {
        return this._db.hasActiveTransaction;
    }

    /**
     * Create a node with given label(s) and properties.
     * @param {string|string[]} label - Node label or array of labels.
     * @param {Object} [props] - Node properties.
     * @returns {Promise<number>} The created node's ID.
     */
    async createNode(label, props) {
        return this._db.createNode(label, props || undefined);
    }

    /**
     * Create multiple nodes in one batch.
     * @param {string} label - Node label.
     * @param {Object[]} propsList - Array of property objects.
     * @returns {Promise<number[]>} Array of created node IDs.
     */
    async createNodes(label, propsList) {
        return this._db.createNodes(label, propsList);
    }

    /**
     * Create an edge between two nodes.
     * @param {number} srcId - Source node ID.
     * @param {number} dstId - Target node ID.
     * @param {string} edgeType - Relationship type.
     * @param {Object} [props] - Edge properties.
     * @returns {Promise<number>} The created edge's ID.
     */
    async createEdge(srcId, dstId, edgeType, props) {
        return this._db.createEdge(srcId, dstId, edgeType, props || undefined);
    }

    /**
     * Create multiple edges in one batch.
     * @param {string} edgeType - Relationship type.
     * @param {Array} edges - Array of [srcId, dstId, props?] tuples.
     * @returns {Promise<number[]>} Array of created edge IDs.
     */
    async createEdges(edgeType, edges) {
        return this._db.createEdges(edgeType, edges);
    }

    /**
     * Find the shortest path between two nodes.
     * @param {number} startId - Start node ID.
     * @param {number} endId - End node ID.
     * @param {number} [maxDepth=15] - Maximum search depth.
     * @returns {Promise<Object[]>} Array of node objects in the path.
     */
    async getShortestPath(startId, endId, maxDepth) {
        return this._db.getShortestPath(startId, endId, maxDepth);
    }
}

module.exports = { Database };
