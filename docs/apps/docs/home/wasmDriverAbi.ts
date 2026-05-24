export type DriverValue =
  | null
  | boolean
  | number
  | string
  | DriverValue[]
  | { [key: string]: DriverValue }
  | DriverNode
  | DriverEdge;

export interface DriverNode {
  kind: 'node';
  id: number;
  labels: string[];
  properties: Record<string, DriverValue>;
}

export interface DriverEdge {
  kind: 'edge';
  id: number;
  sourceId: number;
  targetId: number;
  type: string;
  properties: Record<string, DriverValue>;
}

export interface DriverResult {
  columns: string[];
  rows: DriverValue[][];
  durationMs?: number;
}

export interface EmscriptenZyxModule {
  ccall(name: string, returnType: string | null, argTypes: string[], args: unknown[]): unknown;
  UTF8ToString(ptr: number): string;
  getValue(ptr: number, type: string): number;
  _malloc(size: number): number;
  _free(ptr: number): void;
  zyxPointerSize?: number;
  _zyx_driver_pointer_size?: () => number;
}

const STATUS_OK = 0;
const STATUS_ROW = 1;
const STATUS_DONE = 2;

const VALUE_NULL = 0;
const VALUE_BOOL = 1;
const VALUE_INT64 = 2;
const VALUE_DOUBLE = 3;
const VALUE_STRING = 4;
const VALUE_NODE = 5;
const VALUE_EDGE = 6;
const VALUE_LIST = 7;
const VALUE_MAP = 8;

// scripts/build_wasm.sh currently targets Emscripten wasm32, so Driver ABI pointer slots are 4 bytes.
const POINTER_SLOT_BYTES = 4;
// Driver ABI value references use fixed uint64 tokens even when pointer slots are wasm32.
const VALUE_REF_TOKEN_BYTES = 8;
const VALUE_REF_FIELDS = 4;
const VALUE_REF_BYTES = VALUE_REF_TOKEN_BYTES * VALUE_REF_FIELDS;
const U32_BYTES = 4;
const I64_BYTES = 8;
const DOUBLE_BYTES = 8;

export class WasmDriverAbi {
  constructor(private readonly mod: EmscriptenZyxModule) {
    this.assertWasm32PointerSlots();
  }

  open(path: string): number {
    const outDb = this.allocPtrSlot();
    const outError = this.allocPtrSlot();
    try {
      const status = this.ccallNumber('zyx_driver_db_open', ['string', 'number', 'number'], [path, outDb, outError]);
      this.throwIfNotOk(status, outError);
      return this.readPointer(outDb);
    } finally {
      this.mod._free(outDb);
      this.freeErrorFromSlot(outError);
      this.mod._free(outError);
    }
  }

  close(db: number): void {
    if (!db) return;
    const outError = this.allocPtrSlot();
    try {
      const status = this.ccallNumber('zyx_driver_db_close', ['number', 'number'], [db, outError]);
      this.throwIfNotOk(status, outError);
    } finally {
      this.freeErrorFromSlot(outError);
      this.mod._free(outError);
    }
  }

  beginReadOnly(db: number): number {
    const outTxn = this.allocPtrSlot();
    const outError = this.allocPtrSlot();
    try {
      const status = this.ccallNumber('zyx_driver_txn_begin_read_only', ['number', 'number', 'number'], [db, outTxn, outError]);
      this.throwIfNotOk(status, outError);
      return this.readPointer(outTxn);
    } finally {
      this.mod._free(outTxn);
      this.freeErrorFromSlot(outError);
      this.mod._free(outError);
    }
  }

  closeTxn(txn: number): void {
    if (!txn) return;
    const outError = this.allocPtrSlot();
    try {
      const status = this.ccallNumber('zyx_driver_txn_close', ['number', 'number'], [txn, outError]);
      this.throwIfNotOk(status, outError);
    } finally {
      this.freeErrorFromSlot(outError);
      this.mod._free(outError);
    }
  }

  executeReadOnly(txn: number, cypher: string): DriverResult {
    return this.executeAndMaterialize('zyx_driver_txn_execute', ['number', 'string', 'number', 'number', 'number'], [txn, cypher, 0]);
  }

  private executeAndMaterialize(name: string, argTypes: string[], argsBeforeOut: unknown[]): DriverResult {
    const outResult = this.allocPtrSlot();
    const outError = this.allocPtrSlot();
    let result = 0;
    try {
      const status = this.ccallNumber(name, argTypes, [...argsBeforeOut, outResult, outError]);
      this.throwIfNotOk(status, outError);
      result = this.readPointer(outResult);
      return this.materializeResult(result);
    } finally {
      if (result) this.mod.ccall('zyx_driver_result_free', null, ['number'], [result]);
      this.mod._free(outResult);
      this.freeErrorFromSlot(outError);
      this.mod._free(outError);
    }
  }

  private materializeResult(result: number): DriverResult {
    const columns = this.readColumns(result);
    const rows: DriverValue[][] = [];
    const outError = this.allocPtrSlot();
    try {
      while (true) {
        const status = this.ccallNumber('zyx_driver_result_next', ['number', 'number'], [result, outError]);
        if (status === STATUS_DONE) break;
        this.throwIfNotOk(status, outError, STATUS_ROW);
        rows.push(columns.map((_, column) => this.readColumnValue(result, column)));
      }
    } finally {
      this.freeErrorFromSlot(outError);
      this.mod._free(outError);
    }
    return { columns, rows };
  }

  private readColumns(result: number): string[] {
    const count = this.ccallNumber('zyx_driver_result_column_count', ['number'], [result]);
    const columns: string[] = [];
    for (let column = 0; column < count; column += 1) {
      const name = this.mod.ccall('zyx_driver_result_column_name', 'string', ['number', 'number'], [result, column]);
      columns.push(typeof name === 'string' && name.length > 0 ? name : `col${column}`);
    }
    return columns;
  }

  private readColumnValue(result: number, column: number): DriverValue {
    const type = this.ccallNumber('zyx_driver_result_value_type', ['number', 'number'], [result, column]);
    switch (type) {
      case VALUE_NULL:
        return null;
      case VALUE_BOOL:
        return this.readResultBool(result, column);
      case VALUE_INT64:
        return this.readResultInt64(result, column);
      case VALUE_DOUBLE:
        return this.readResultDouble(result, column);
      case VALUE_STRING:
        return this.readResultString(result, column);
      case VALUE_NODE:
        return this.readNode(result, column);
      case VALUE_EDGE:
        return this.readEdge(result, column);
      case VALUE_LIST:
      case VALUE_MAP:
        return this.readTopLevelValueRef(result, column);
      default:
        throw new Error(`Unsupported Driver ABI value type ${type}`);
    }
  }

  private readTopLevelValueRef(result: number, column: number): DriverValue {
    const root = this.allocValueRef();
    const outError = this.allocPtrSlot();
    try {
      const status = this.ccallNumber('zyx_driver_result_get_value', ['number', 'number', 'number', 'number'], [
        result,
        column,
        root,
        outError,
      ]);
      this.throwIfNotOk(status, outError);
      // Value refs borrow result-owned storage; materialize synchronously before freeing the ref.
      return this.readValueRef(result, root);
    } finally {
      this.mod._free(root);
      this.freeErrorFromSlot(outError);
      this.mod._free(outError);
    }
  }

  private readValueRef(result: number, ref: number): DriverValue {
    const type = this.ccallNumber('zyx_driver_value_ref_type', ['number'], [ref]);
    switch (type) {
      case VALUE_NULL:
        return null;
      case VALUE_BOOL:
        return this.readValueRefBool(ref);
      case VALUE_INT64:
        return this.readValueRefInt64(ref);
      case VALUE_DOUBLE:
        return this.readValueRefDouble(ref);
      case VALUE_STRING:
        return this.readValueRefString(result, ref);
      case VALUE_NODE:
        return this.readNodeRef(result, ref);
      case VALUE_EDGE:
        return this.readEdgeRef(result, ref);
      case VALUE_LIST:
        return this.readListRef(result, ref);
      case VALUE_MAP:
        return this.readMapRef(result, ref);
      default:
        throw new Error(`Unsupported nested Driver ABI value type ${type}`);
    }
  }

  private readListRef(result: number, ref: number): DriverValue[] {
    const count = this.readCount('zyx_driver_value_ref_list_count', ['number', 'number', 'number'], [ref]);
    const values: DriverValue[] = [];
    for (let index = 0; index < count; index += 1) {
      const child = this.allocValueRef();
      const outError = this.allocPtrSlot();
      try {
        const status = this.ccallNumber('zyx_driver_value_ref_list_get', ['number', 'number', 'number', 'number'], [
          ref,
          index,
          child,
          outError,
        ]);
        this.throwIfNotOk(status, outError);
        // Child refs are valid only while the result lives and until this synchronous read finishes.
        const value = this.readValueRef(result, child);
        values.push(value);
      } finally {
        this.mod._free(child);
        this.freeErrorFromSlot(outError);
        this.mod._free(outError);
      }
    }
    return values;
  }

  private readMapRef(result: number, ref: number): Record<string, DriverValue> {
    const count = this.readCount('zyx_driver_value_ref_map_count', ['number', 'number', 'number'], [ref]);
    const values: Record<string, DriverValue> = {};
    for (let index = 0; index < count; index += 1) {
      const key = this.readMapKey(result, ref, index);
      const child = this.allocValueRef();
      const outError = this.allocPtrSlot();
      try {
        const status = this.ccallNumber('zyx_driver_value_ref_map_get', ['number', 'string', 'number', 'number'], [
          ref,
          key,
          child,
          outError,
        ]);
        this.throwIfNotOk(status, outError);
        // Never retain child refs across iterations; copy the value before freeing the scratch ref.
        const value = this.readValueRef(result, child);
        values[key] = value;
      } finally {
        this.mod._free(child);
        this.freeErrorFromSlot(outError);
        this.mod._free(outError);
      }
    }
    return values;
  }

  private readNode(result: number, column: number): DriverNode {
    return {
      kind: 'node',
      id: this.readEntityInt64('zyx_driver_result_get_node_id', result, column),
      labels: this.readNodeLabels(result, column),
      properties: this.readEntityProperties(result, column),
    };
  }

  private readEdge(result: number, column: number): DriverEdge {
    return {
      kind: 'edge',
      id: this.readEntityInt64('zyx_driver_result_get_edge_id', result, column),
      sourceId: this.readEntityInt64('zyx_driver_result_get_edge_source_id', result, column),
      targetId: this.readEntityInt64('zyx_driver_result_get_edge_target_id', result, column),
      type: this.readEdgeType(result, column),
      properties: this.readEntityProperties(result, column),
    };
  }

  private readNodeLabels(result: number, column: number): string[] {
    const count = this.readCount('zyx_driver_result_get_node_label_count', ['number', 'number', 'number', 'number'], [
      result,
      column,
    ]);
    const labels: string[] = [];
    for (let index = 0; index < count; index += 1) {
      labels.push(this.readStringOut('zyx_driver_result_get_node_label', ['number', 'number', 'number', 'number', 'number'], [
        result,
        column,
        index,
      ]));
    }
    return labels;
  }

  private readEdgeType(result: number, column: number): string {
    return this.readStringOut('zyx_driver_result_get_edge_type', ['number', 'number', 'number', 'number'], [result, column]);
  }

  private readEntityProperties(result: number, column: number): Record<string, DriverValue> {
    const json = this.readStringOut('zyx_driver_result_get_entity_properties_json', ['number', 'number', 'number', 'number'], [
      result,
      column,
    ]);
    return this.parseEntityProperties(json);
  }

  private parseEntityProperties(json: string): Record<string, DriverValue> {
    if (!json) return {};
    const parsed = JSON.parse(json) as unknown;
    const normalized = this.normalizeJsonValue(parsed);
    return normalized !== null && typeof normalized === 'object' && !Array.isArray(normalized) && !('kind' in normalized)
      ? normalized
      : {};
  }

  private normalizeJsonValue(value: unknown): DriverValue {
    if (value === null || typeof value === 'boolean' || typeof value === 'string') return value;
    if (typeof value === 'number') return Number.isFinite(value) ? value : null;
    if (Array.isArray(value)) return value.map((item) => this.normalizeJsonValue(item));
    if (typeof value === 'object') {
      const record: Record<string, DriverValue> = {};
      for (const [key, nested] of Object.entries(value as Record<string, unknown>)) {
        record[key] = this.normalizeJsonValue(nested);
      }
      return record;
    }
    return null;
  }

  private readResultBool(result: number, column: number): boolean {
    return Boolean(this.readScalarOut('zyx_driver_result_get_bool', ['number', 'number', 'number', 'number'], [result, column], U32_BYTES, 'i8'));
  }

  private readResultInt64(result: number, column: number): number {
    return this.readScalarOut('zyx_driver_result_get_int64', ['number', 'number', 'number', 'number'], [result, column], I64_BYTES, 'i64');
  }

  private readResultDouble(result: number, column: number): number {
    return this.readScalarOut('zyx_driver_result_get_double', ['number', 'number', 'number', 'number'], [result, column], DOUBLE_BYTES, 'double');
  }

  private readResultString(result: number, column: number): string {
    return this.readStringOut('zyx_driver_result_get_string', ['number', 'number', 'number', 'number'], [result, column]);
  }

  private readValueRefBool(ref: number): boolean {
    return Boolean(this.readScalarOut('zyx_driver_value_ref_get_bool', ['number', 'number', 'number'], [ref], U32_BYTES, 'i8'));
  }

  private readValueRefInt64(ref: number): number {
    return this.readScalarOut('zyx_driver_value_ref_get_int64', ['number', 'number', 'number'], [ref], I64_BYTES, 'i64');
  }

  private readValueRefDouble(ref: number): number {
    return this.readScalarOut('zyx_driver_value_ref_get_double', ['number', 'number', 'number'], [ref], DOUBLE_BYTES, 'double');
  }

  private readValueRefString(result: number, ref: number): string {
    return this.readStringOut('zyx_driver_value_ref_get_string', ['number', 'number', 'number', 'number'], [result, ref]);
  }

  private readNodeRef(result: number, ref: number): DriverNode {
    return {
      kind: 'node',
      id: this.readValueRefEntityInt64('zyx_driver_value_ref_get_node_id', ref),
      labels: this.readNodeRefLabels(result, ref),
      properties: this.readValueRefEntityProperties(result, ref),
    };
  }

  private readEdgeRef(result: number, ref: number): DriverEdge {
    return {
      kind: 'edge',
      id: this.readValueRefEntityInt64('zyx_driver_value_ref_get_edge_id', ref),
      sourceId: this.readValueRefEntityInt64('zyx_driver_value_ref_get_edge_source_id', ref),
      targetId: this.readValueRefEntityInt64('zyx_driver_value_ref_get_edge_target_id', ref),
      type: this.readValueRefEdgeType(result, ref),
      properties: this.readValueRefEntityProperties(result, ref),
    };
  }

  private readNodeRefLabels(result: number, ref: number): string[] {
    const count = this.readCount('zyx_driver_value_ref_get_node_label_count', ['number', 'number', 'number'], [ref]);
    const labels: string[] = [];
    for (let index = 0; index < count; index += 1) {
      labels.push(this.readStringOut('zyx_driver_value_ref_get_node_label', ['number', 'number', 'number', 'number', 'number'], [
        result,
        ref,
        index,
      ]));
    }
    return labels;
  }

  private readValueRefEdgeType(result: number, ref: number): string {
    return this.readStringOut('zyx_driver_value_ref_get_edge_type', ['number', 'number', 'number', 'number'], [result, ref]);
  }

  private readValueRefEntityProperties(result: number, ref: number): Record<string, DriverValue> {
    const json = this.readStringOut('zyx_driver_value_ref_get_entity_properties_json', ['number', 'number', 'number', 'number'], [
      result,
      ref,
    ]);
    return this.parseEntityProperties(json);
  }

  private readMapKey(result: number, ref: number, index: number): string {
    return this.readStringOut('zyx_driver_value_ref_map_key', ['number', 'number', 'number', 'number', 'number'], [result, ref, index]);
  }

  private readEntityInt64(name: string, result: number, column: number): number {
    return this.readScalarOut(name, ['number', 'number', 'number', 'number'], [result, column], I64_BYTES, 'i64');
  }

  private readValueRefEntityInt64(name: string, ref: number): number {
    return this.readScalarOut(name, ['number', 'number', 'number'], [ref], I64_BYTES, 'i64');
  }

  private readCount(name: string, argTypes: string[], argsBeforeOut: unknown[]): number {
    return this.readScalarOut(name, argTypes, argsBeforeOut, U32_BYTES, 'i32');
  }

  private readScalarOut(name: string, argTypes: string[], argsBeforeOut: unknown[], bytes: number, valueType: string): number {
    const out = this.allocBytes(bytes);
    const outError = this.allocPtrSlot();
    try {
      const status = this.ccallNumber(name, argTypes, [...argsBeforeOut, out, outError]);
      this.throwIfNotOk(status, outError);
      return Number(this.mod.getValue(out, valueType));
    } finally {
      this.mod._free(out);
      this.freeErrorFromSlot(outError);
      this.mod._free(outError);
    }
  }

  private readStringOut(name: string, argTypes: string[], argsBeforeOut: unknown[]): string {
    const out = this.allocPtrSlot();
    const outError = this.allocPtrSlot();
    try {
      const status = this.ccallNumber(name, argTypes, [...argsBeforeOut, out, outError]);
      this.throwIfNotOk(status, outError);
      const ptr = this.readPointer(out);
      return ptr ? String(this.mod.UTF8ToString(ptr)) : '';
    } finally {
      this.mod._free(out);
      this.freeErrorFromSlot(outError);
      this.mod._free(outError);
    }
  }

  private assertWasm32PointerSlots(): void {
    const reportedPointerSize =
      typeof this.mod._zyx_driver_pointer_size === 'function'
        ? Number(this.mod._zyx_driver_pointer_size())
        : typeof this.mod.zyxPointerSize === 'number'
          ? this.mod.zyxPointerSize
          : POINTER_SLOT_BYTES;
    if (reportedPointerSize !== POINTER_SLOT_BYTES) {
      throw new Error(`Unsupported WASM pointer size ${reportedPointerSize}; this wrapper requires wasm32 pointer slots`);
    }
  }

  private allocPtrSlot(): number {
    return this.allocBytes(POINTER_SLOT_BYTES);
  }

  private allocValueRef(): number {
    return this.allocBytes(VALUE_REF_BYTES);
  }

  private allocBytes(bytes: number): number {
    const ptr = this.mod._malloc(bytes);
    if (!ptr) throw new Error('WASM allocation failed');
    return ptr;
  }

  private readPointer(ptr: number): number {
    return Number(this.mod.getValue(ptr, '*'));
  }

  private ccallNumber(name: string, argTypes: string[], args: unknown[]): number {
    return Number(this.mod.ccall(name, 'number', argTypes, args));
  }

  private throwIfNotOk(status: number, outError: number, alsoOk = STATUS_OK): void {
    if (status === STATUS_OK || status === alsoOk) return;
    const error = this.readPointer(outError);
    const message = error
      ? String(this.mod.ccall('zyx_driver_error_message', 'string', ['number'], [error]))
      : `Driver ABI status ${status}`;
    throw new Error(message);
  }

  private freeErrorFromSlot(outError: number): void {
    const error = this.readPointer(outError);
    if (error) this.mod.ccall('zyx_driver_error_free', null, ['number'], [error]);
  }
}
