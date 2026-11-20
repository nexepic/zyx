// Focused real-asset test: node --experimental-transform-types --experimental-vm-modules scripts/test_wasm_runtime_split.mjs
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
import { WasmDriverAbi } from '../docs/apps/docs/home/wasmDriverAbi.ts';

// Default to freshly built assets, never silently validate the checked-in site copy.
const assets = process.argv[2]
  ? pathToFileURL(resolve(process.argv[2]) + '/')
  : new URL('../build_wasm/', import.meta.url);
const runtime = new URL('zyx.js', assets);
const filesystem = new URL('zyx-filesystem.mjs', assets);
const create = createRequire(import.meta.url)(fileURLToPath(runtime));
const first = await create();
const initialMemoryBytes = first.HEAPU8.byteLength;
const second = await create();
assert.notEqual(first.FS, second.FS);
first.FS.mkdir('/split-test');
first.FS.writeFile('/split-test/value', new Uint8Array([0, 3, 255]));
assert.deepEqual([...first.FS.readFile('/split-test/value')], [0, 3, 255]);
assert.throws(() => second.FS.readFile('/split-test/value'));
first.FS.rename('/split-test/value', '/split-test/moved');
assert.deepEqual(first.FS.readdir('/split-test'), ['.', '..', 'moved']);
first.FS.unlink('/split-test/moved');
first.FS.rmdir('/split-test');
const driver = new WasmDriverAbi(first);
const db = driver.open('/split-test.zyx');
try {
  const txn = driver.beginReadOnly(db);
  try {
    assert.deepEqual(driver.executeReadOnly(txn, 'RETURN 7 AS value').rows, [[7]]);
    assert.deepEqual(driver.executeReadOnly(txn, 'MATCH (n) RETURN count(*) AS count').rows, [[0]]);
    assert.deepEqual(driver.executeReadOnly(txn, "RETURN null AS missing, true AS flag, 'hello' AS text, [1, 2, 3] AS values").rows,
      [[null, true, 'hello', [1, 2, 3]]]);
    const expected = Array.from({ length: 1000 }, (_, i) => [i + 1, (i + 1) * 2]);
    for (let repeat = 0; repeat < 3; ++repeat) {
      assert.deepEqual(driver.executeReadOnly(txn,
        'UNWIND range(1, 1000) AS x RETURN x AS value, x * 2 AS doubled ORDER BY value').rows, expected);
    }
    assert.deepEqual(driver.executeReadOnly(txn,
      'UNWIND range(1, 1000) AS x RETURN count(x) AS count, sum(x) AS total').rows, [[1000, 500500]]);
    assert.deepEqual(driver.executeReadOnly(txn,
      'UNWIND [3, 1, 2, 1] AS x RETURN DISTINCT x AS value ORDER BY value').rows, [[1], [2], [3]]);
  } finally { driver.closeTxn(txn); }
} finally { driver.close(db); }
assert.ok(first.FS.stat('/split-test.zyx').size > 0);
const reopened = driver.open('/split-test.zyx');
try {
  const txn = driver.beginReadOnly(reopened);
  try { assert.deepEqual(driver.executeReadOnly(txn, 'RETURN 9 AS value').rows, [[9]]); }
  finally { driver.closeTxn(txn); }
} finally { driver.close(reopened); }
for (const symbol of ['_zyx_driver_db_open', '_zyx_driver_txn_begin_read_only', '_zyx_driver_txn_execute'])
  assert.equal(typeof first[symbol], 'function');

// Emulate classic-script URL resolution (not a claim of browser execution).
const assetBase = 'https://example.invalid/project/wasm/';
const context = vm.createContext({ window: {}, document: { currentScript: { src: assetBase + 'zyx.js' } },
  console, URL, TextDecoder, TextEncoder, setTimeout, clearTimeout, performance, WebAssembly,
  Uint8Array, Int8Array, Uint16Array, Int16Array, Uint32Array, Int32Array, Float32Array, Float64Array,
  BigInt64Array, BigUint64Array, crypto: globalThis.crypto });
let requested;
const script = new vm.Script(readFileSync(runtime, 'utf8'), { filename: assetBase + 'zyx.js',
  importModuleDynamically: async (specifier) => {
    requested = specifier;
    return import(filesystem.href);
  } });
script.runInContext(context);
const browserContract = await context.createZyxModule({
  wasmBinary: readFileSync(new URL('zyx.wasm', assets)),
  locateFile: file => assetBase + file,
});
assert.equal(requested, assetBase + 'zyx-filesystem.mjs');
browserContract.FS.writeFile('/contract', new Uint8Array([7]));
assert.deepEqual([...browserContract.FS.readFile('/contract')], [7]);
console.log('WASM smoke passed: initialization, FS isolation, typed values, 3 x 1000 rows, aggregate, distinct/sort and classic-script URL contract');
console.log(JSON.stringify({ initialMemoryBytes, finalMemoryBytes: first.HEAPU8.byteLength, assets: assets.href }));
