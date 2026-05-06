'use strict';

/**
 * A single row from a query result.
 * Supports dict-like access by column name or index.
 */
class Record {
    /**
     * @param {Object} data - Row data as key-value pairs.
     * @param {string[]} columns - Column names in order.
     * @private
     */
    constructor(data, columns) {
        this._data = data;
        this._columns = columns;

        // Create a Proxy to support both record.name and record['name']
        return new Proxy(this, {
            get(target, prop) {
                // First check if it's a method or property of Record
                if (prop in target) {
                    return target[prop];
                }
                // Then check if it's a column name
                if (typeof prop === 'string' && prop in target._data) {
                    return target._data[prop];
                }
                return undefined;
            },
            has(target, prop) {
                return prop in target || prop in target._data;
            }
        });
    }

    /**
     * Get value by column name or index.
     * @param {string|number} key - Column name or index.
     * @returns {*}
     */
    get(key) {
        if (typeof key === 'number') {
            return this._data[this._columns[key]];
        }
        return this._data[key];
    }

    /**
     * Get all column names.
     * @returns {string[]}
     */
    keys() {
        return [...this._columns];
    }

    /**
     * Get all values in column order.
     * @returns {Array}
     */
    values() {
        return this._columns.map(c => this._data[c]);
    }

    /**
     * Get the underlying data object.
     * @returns {Object}
     */
    data() {
        return { ...this._data };
    }

    /**
     * Number of columns.
     * @type {number}
     */
    get length() {
        return this._columns.length;
    }

    toString() {
        const items = this._columns.map(k => `${k}=${JSON.stringify(this._data[k])}`).join(', ');
        return `Record(${items})`;
    }
}

/**
 * Wraps a native result object with iteration and convenience methods.
 */
class Result {
    /**
     * @param {Object} nativeResult - Native result from addon.
     * @private
     */
    constructor(nativeResult) {
        this._result = nativeResult;
    }

    /**
     * Column names from the result set.
     * @type {string[]}
     */
    get columns() {
        return this._result.columns;
    }

    /**
     * Whether the query executed successfully.
     * @type {boolean}
     */
    get isSuccess() {
        return this._result.isSuccess;
    }

    /**
     * Error message, or null if successful.
     * @type {string|null}
     */
    get error() {
        return this._result.error;
    }

    /**
     * Query execution duration in milliseconds.
     * @type {number}
     */
    get duration() {
        return this._result.duration;
    }

    /**
     * All rows as Record objects.
     * @type {Record[]}
     */
    get records() {
        return this._result.rows.map(row => new Record(row, this.columns));
    }

    /**
     * Iterate over records.
     * @returns {Iterator<Record>}
     */
    *[Symbol.iterator]() {
        for (const row of this._result.rows) {
            yield new Record(row, this.columns);
        }
    }

    /**
     * Fetch all rows as Record objects.
     * @returns {Record[]}
     */
    fetchAll() {
        return this.records;
    }

    /**
     * Return the single result row.
     * @param {boolean} [strict=true] - If true, throw if not exactly one row.
     * @returns {Record}
     * @throws {Error} If strict and row count != 1.
     */
    single(strict = true) {
        const rows = this.records;
        if (strict && rows.length !== 1) {
            throw new Error(`Expected exactly 1 row, got ${rows.length}`);
        }
        if (rows.length === 0) {
            throw new Error('Result is empty');
        }
        return rows[0];
    }

    /**
     * Return the first column of the first row.
     * @returns {*}
     */
    scalar() {
        const record = this.single();
        return record.get(0);
    }

    /**
     * Return all rows as plain objects.
     * @returns {Object[]}
     */
    data() {
        return this._result.rows.map(row => ({ ...row }));
    }

    toString() {
        const status = this.isSuccess ? 'ok' : `error=${this.error}`;
        return `Result(${status}, columns=[${this.columns.join(', ')}], rows=${this.records.length})`;
    }
}

module.exports = { Result, Record };
