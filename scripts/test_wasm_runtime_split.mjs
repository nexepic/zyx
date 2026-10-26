// Focused real-asset test: node --experimental-transform-types --experimental-vm-modules scripts/test_wasm_runtime_split.mjs
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';
import { WasmDriverAbi } from '../docs/apps/docs/home/wasmDriverAbi.ts';

const runtime = new URL('../docs/apps/docs/public/wasm/zyx.js', import.meta.url);
const filesystem = new URL('../docs/apps/docs/public/wasm/zyx-filesystem.mjs', import.meta.url);
const create = createRequire(import.meta.url)(fileURLToPath(runtime));
const first = await create();
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
  } finally { driver.closeTxn(txn); }
} finally { driver.close(db); }
assert.ok(first.FS.stat('/split-test.zyx').size > 0);
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
  wasmBinary: readFileSync(new URL('../docs/apps/docs/public/wasm/zyx.wasm', import.meta.url)),
  locateFile: file => assetBase + file,
});
assert.equal(requested, assetBase + 'zyx-filesystem.mjs');
browserContract.FS.writeFile('/contract', new Uint8Array([7]));
assert.deepEqual([...browserContract.FS.readFile('/contract')], [7]);
console.log('WASM split: real initialization, FS operations/isolation and classic-script URL contract passed');
