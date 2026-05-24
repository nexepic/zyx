import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import vm from 'node:vm';

const source = readFileSync(new URL('./wasmDriverAbi.ts', import.meta.url), 'utf8');
const buildScript = readFileSync(new URL('../../../../scripts/build_wasm.sh', import.meta.url), 'utf8');

function loadWasmDriverAbi() {
  const js = source
    .replace(/export type DriverValue =[\s\S]*?;\n\n/, '')
    .replace(/export interface DriverNode \{[\s\S]*?\}\n\n/, '')
    .replace(/export interface DriverEdge \{[\s\S]*?\}\n\n/, '')
    .replace(/export interface DriverResult \{[\s\S]*?\}\n\n/, '')
    .replace(/export interface EmscriptenZyxModule \{[\s\S]*?\}\n\n/, '')
    .replace(/export class WasmDriverAbi/, 'class WasmDriverAbi')
    .replace(/constructor\(private readonly mod: EmscriptenZyxModule\) \{/, 'constructor(mod) { this.mod = mod;')
    .replace(/: void/g, '')
    .replace(/: number/g, '')
    .replace(/: string\[\]/g, '')
    .replace(/: unknown\[\]/g, '')
    .replace(/: string\[\]/g, '')
    .replace(/: DriverResult/g, '')
    .replace(/: DriverValue\[\]\[\]/g, '')
    .replace(/: DriverValue\[\]/g, '')
    .replace(/: DriverValue/g, '')
    .replace(/: Record<string, DriverValue>/g, '')
    .replace(/: DriverNode/g, '')
    .replace(/: DriverEdge/g, '')
    .replace(/: string/g, '')
    .replace(/: boolean/g, '')
    .replace(/: unknown/g, '')
    .replace(/ as unknown/g, '')
    .replace(/ as Record<string, unknown>/g, '')
    .replace(/private readonly mod/g, 'mod')
    .replace(/private /g, '')
    .replace(/readonly /g, '')
    .replace(/\n}\s*$/, '\n}\nthis.WasmDriverAbi = WasmDriverAbi;');
  const context = {};
  vm.runInNewContext(js, context, { filename: 'wasmDriverAbi.ts' });
  return context.WasmDriverAbi;
}

const WasmDriverAbi = loadWasmDriverAbi();

function createFakeModule({ executeStatus = 0, errorMessage = 'boom', pointerSize = 4 } = {}) {
  const STATUS_OK = 0;
  const STATUS_ROW = 1;
  const STATUS_DONE = 2;
  const VALUE_INT64 = 2;
  const VALUE_STRING = 4;
  const VALUE_NODE = 5;
  const VALUE_EDGE = 6;
  const VALUE_LIST = 7;
  const VALUE_MAP = 8;

  let nextPtr = 64;
  const memory = new Map();
  const strings = new Map();
  const calls = [];
  const freed = [];
  const resultHandle = 1000;
  const errorHandle = 2000;
  let rowIndex = -1;

  const nestedRoot = {
    type: VALUE_LIST,
    values: [
      { type: VALUE_INT64, value: 7 },
      {
        type: VALUE_MAP,
        entries: {
          alpha: { type: VALUE_STRING, value: 'one' },
          beta: { type: VALUE_LIST, values: [{ type: VALUE_INT64, value: 9 }] },
          node: {
            type: VALUE_NODE,
            id: 42,
            labels: ['Person', 'Engineer'],
            properties: { name: 'Ada', score: 98 },
          },
          edge: {
            type: VALUE_EDGE,
            id: 77,
            sourceId: 42,
            targetId: 43,
            relType: 'KNOWS',
            properties: { since: 2026 },
          },
        },
      },
    ],
  };

  function malloc(size) {
    const ptr = nextPtr;
    nextPtr += Math.max(size, 8);
    memory.set(ptr, 0);
    return ptr;
  }

  function write(ptr, value) {
    memory.set(ptr, value);
  }

  function readValueRef(ref) {
    const value = memory.get(ref);
    assert.ok(value && typeof value === 'object', `expected value ref at ${ref}`);
    return value;
  }

  function putString(value) {
    const ptr = malloc(value.length + 1);
    strings.set(ptr, value);
    return ptr;
  }

  function ccall(name, returnType, argTypes, args) {
    calls.push({ name, returnType, argTypes, args });
    switch (name) {
      case 'zyx_driver_db_open':
        write(args[1], 321);
        return STATUS_OK;
      case 'zyx_driver_db_close':
        freed.push(args[0]);
        return STATUS_OK;
      case 'zyx_driver_txn_begin_read_only':
        write(args[1], 654);
        return STATUS_OK;
      case 'zyx_driver_txn_close':
        freed.push(args[0]);
        return STATUS_OK;
      case 'zyx_driver_txn_execute': {
        const outResult = args[3];
        const outError = args[4];
        if (executeStatus !== STATUS_OK) {
          write(outError, errorHandle);
          return executeStatus;
        }
        write(outResult, resultHandle);
        return STATUS_OK;
      }
      case 'zyx_driver_result_free':
        freed.push(args[0]);
        return undefined;
      case 'zyx_driver_error_message':
        assert.equal(args[0], errorHandle);
        return errorMessage;
      case 'zyx_driver_error_free':
        freed.push(args[0]);
        return undefined;
      case 'zyx_driver_result_column_count':
        return 1;
      case 'zyx_driver_result_column_name':
        return 'value';
      case 'zyx_driver_result_next':
        rowIndex += 1;
        return rowIndex === 0 ? STATUS_ROW : STATUS_DONE;
      case 'zyx_driver_result_value_type':
        return VALUE_LIST;
      case 'zyx_driver_result_get_value':
        write(args[2], nestedRoot);
        return STATUS_OK;
      case 'zyx_driver_value_ref_type':
        return readValueRef(args[0]).type;
      case 'zyx_driver_value_ref_list_count':
        write(args[1], readValueRef(args[0]).values.length);
        return STATUS_OK;
      case 'zyx_driver_value_ref_list_get':
        write(args[2], readValueRef(args[0]).values[args[1]]);
        return STATUS_OK;
      case 'zyx_driver_value_ref_map_count':
        write(args[1], Object.keys(readValueRef(args[0]).entries).length);
        return STATUS_OK;
      case 'zyx_driver_value_ref_map_key': {
        const key = Object.keys(readValueRef(args[1]).entries)[args[2]];
        write(args[3], putString(key));
        return STATUS_OK;
      }
      case 'zyx_driver_value_ref_map_get':
        write(args[2], readValueRef(args[0]).entries[String(args[1])]);
        return STATUS_OK;
      case 'zyx_driver_value_ref_get_int64':
        write(args[1], readValueRef(args[0]).value);
        return STATUS_OK;
      case 'zyx_driver_value_ref_get_string':
        write(args[2], putString(readValueRef(args[1]).value));
        return STATUS_OK;
      case 'zyx_driver_value_ref_get_node_id':
        write(args[1], readValueRef(args[0]).id);
        return STATUS_OK;
      case 'zyx_driver_value_ref_get_node_label_count':
        write(args[1], readValueRef(args[0]).labels.length);
        return STATUS_OK;
      case 'zyx_driver_value_ref_get_node_label':
        write(args[3], putString(readValueRef(args[1]).labels[args[2]]));
        return STATUS_OK;
      case 'zyx_driver_value_ref_get_edge_id':
        write(args[1], readValueRef(args[0]).id);
        return STATUS_OK;
      case 'zyx_driver_value_ref_get_edge_source_id':
        write(args[1], readValueRef(args[0]).sourceId);
        return STATUS_OK;
      case 'zyx_driver_value_ref_get_edge_target_id':
        write(args[1], readValueRef(args[0]).targetId);
        return STATUS_OK;
      case 'zyx_driver_value_ref_get_edge_type':
        write(args[2], putString(readValueRef(args[1]).relType));
        return STATUS_OK;
      case 'zyx_driver_value_ref_get_entity_properties_json':
        write(args[2], putString(JSON.stringify(readValueRef(args[1]).properties)));
        return STATUS_OK;
      default:
        throw new Error(`unexpected ccall ${name}`);
    }
  }

  return {
    module: {
      zyxPointerSize: pointerSize,
      ccall,
      UTF8ToString(ptr) {
        return strings.get(ptr) ?? '';
      },
      getValue(ptr) {
        return memory.get(ptr) ?? 0;
      },
      _malloc: malloc,
      _free(ptr) {
        freed.push(ptr);
      },
    },
    calls,
    freed,
    resultHandle,
    errorHandle,
  };
}

assert.match(source, /export class WasmDriverAbi/);
assert.match(source, /interface EmscriptenZyxModule/);

const requiredCalls = [
  'zyx_driver_db_open',
  'zyx_driver_db_close',
  'zyx_driver_txn_begin_read_only',
  'zyx_driver_txn_execute',
  'zyx_driver_txn_close',
  'zyx_driver_result_free',
  'zyx_driver_error_free',
  'zyx_driver_result_next',
  'zyx_driver_result_column_count',
  'zyx_driver_result_column_name',
  'zyx_driver_result_value_type',
  'zyx_driver_result_get_value',
  'zyx_driver_value_ref_type',
  'zyx_driver_value_ref_list_count',
  'zyx_driver_value_ref_list_get',
  'zyx_driver_value_ref_map_count',
  'zyx_driver_value_ref_map_key',
  'zyx_driver_value_ref_map_get',
  'zyx_driver_result_get_node_id',
  'zyx_driver_result_get_node_label_count',
  'zyx_driver_result_get_node_label',
  'zyx_driver_result_get_edge_id',
  'zyx_driver_result_get_edge_source_id',
  'zyx_driver_result_get_edge_target_id',
  'zyx_driver_result_get_edge_type',
  'zyx_driver_result_get_entity_properties_json',
  'zyx_driver_value_ref_get_node_id',
  'zyx_driver_value_ref_get_node_label_count',
  'zyx_driver_value_ref_get_node_label',
  'zyx_driver_value_ref_get_edge_id',
  'zyx_driver_value_ref_get_edge_source_id',
  'zyx_driver_value_ref_get_edge_target_id',
  'zyx_driver_value_ref_get_edge_type',
  'zyx_driver_value_ref_get_entity_properties_json',
];

for (const symbol of requiredCalls) {
  assert.match(source, new RegExp(`['"]${symbol}['"]`), `${symbol} is used`);
}

assert.match(source, /VALUE_REF_TOKEN_BYTES\s*=\s*8/);
assert.match(source, /VALUE_REF_FIELDS\s*=\s*4/);
assert.match(source, /VALUE_REF_BYTES\s*=\s*VALUE_REF_TOKEN_BYTES\s*\*\s*VALUE_REF_FIELDS/);
assert.match(source, /Driver ABI value references use fixed uint64 tokens/);
assert.doesNotMatch(source, /POINTER_SLOT_BYTES\s*\*\s*2/);
assert.match(source, /wasm32 pointer slots/);
assert.match(source, /freeErrorFromSlot/);
assert.match(source, /UTF8ToString/);
assert.match(source, /JSON\.parse/);

const legacyCalls = [
  'zyx_open',
  'zyx_close',
  'zyx_begin_read_only_transaction',
  'zyx_txn_execute',
  'zyx_result_close',
  'zyx_result_is_success',
];

for (const symbol of legacyCalls) {
  assert.doesNotMatch(source, new RegExp(`['"]${symbol}['"]`), `${symbol} is not used`);
}

const browserForbiddenExports = [
  '_zyx_driver_txn_begin',
  '_zyx_driver_txn_commit',
  '_zyx_driver_db_save',
  '_zyx_driver_db_create_node',
  '_zyx_driver_db_create_node_with_labels',
  '_zyx_driver_db_create_edge',
  '_zyx_driver_db_execute',
  '_zyx_driver_params_create',
  '_zyx_driver_params_set_value',
  '_zyx_driver_value_list_create',
  '_zyx_driver_value_map_set_value',
];

for (const symbol of browserForbiddenExports) {
  assert.doesNotMatch(buildScript, new RegExp(`['"]${symbol}['"]`), `${symbol} is not exported by the browser WASM profile`);
}

const browserRequiredExports = [
  '_zyx_driver_db_open',
  '_zyx_driver_db_close',
  '_zyx_driver_txn_begin_read_only',
  '_zyx_driver_txn_execute',
  '_zyx_driver_txn_rollback',
  '_zyx_driver_txn_close',
  '_zyx_driver_result_get_value',
  '_zyx_driver_value_ref_list_get',
  '_zyx_driver_value_ref_map_get',
];

for (const symbol of browserRequiredExports) {
  assert.match(buildScript, new RegExp(`['"]${symbol}['"]`), `${symbol} is exported by the browser WASM profile`);
}

{
  const fake = createFakeModule();
  const driver = new WasmDriverAbi(fake.module);
  const db = driver.open('/tmp/zyx-playground');
  assert.equal(db, 321);
  const txn = driver.beginReadOnly(db);
  assert.equal(txn, 654);
  driver.closeTxn(txn);
  driver.close(db);
  assert.ok(fake.freed.includes(654), 'read-only transaction handle is closed');
  assert.ok(fake.freed.includes(321), 'database handle is closed');
}

{
  const fake = createFakeModule();
  const driver = new WasmDriverAbi(fake.module);
  const result = driver.executeReadOnly(123, 'RETURN nested');
  assert.deepEqual(JSON.parse(JSON.stringify(result)), {
    columns: ['value'],
    rows: [[[7, { alpha: 'one', beta: [9], node: { kind: 'node', id: 42, labels: ['Person', 'Engineer'], properties: { name: 'Ada', score: 98 } }, edge: { kind: 'edge', id: 77, sourceId: 42, targetId: 43, type: 'KNOWS', properties: { since: 2026 } } }]]],
  });
  assert.ok(fake.freed.includes(fake.resultHandle), 'result handle is freed after executeReadOnly');
  assert.equal(fake.calls.some((call) => call.name.startsWith('zyx_') && !call.name.startsWith('zyx_driver_')), false);
}

{
  const fake = createFakeModule({ executeStatus: 99, errorMessage: 'query failed' });
  const driver = new WasmDriverAbi(fake.module);
  assert.throws(() => driver.executeReadOnly(123, 'RETURN bad'), /query failed/);
  assert.ok(fake.freed.includes(fake.errorHandle), 'error handle is freed on error status');
  assert.equal(fake.freed.includes(fake.resultHandle), false, 'result handle is not freed when execute did not return one');
}

{
  const fake = createFakeModule({ pointerSize: 8 });
  assert.throws(() => new WasmDriverAbi(fake.module), /requires wasm32 pointer slots/);
}

console.log('wasmDriverAbi tests passed');
