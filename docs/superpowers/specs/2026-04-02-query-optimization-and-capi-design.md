# Query Optimization and C API Enhancement Design

**Date**: 2026-04-02
**Status**: Approved
**Scope**: Parameterized queries, query plan cache, C API improvements

## Overview

Three interconnected optimization modules that improve query performance and API completeness:

1. **Parameterized Queries** — Support `$param` syntax in Cypher, separating query template from data values
2. **Query Plan Cache** — Cache optimized logical plans keyed by query template string
3. **C API Improvements** — Transaction control, parameterized queries, batch operations, List/Map types

Dependency order: Module 1 → Module 2 → Module 3 (Module 3 depends on 1 for params API; Module 2 depends on 1 for cache effectiveness)

---

## Module 1: Parameterized Queries

### Goal

Support `$paramName` syntax in Cypher queries. Parameter values are supplied at execution time via a `std::unordered_map<std::string, Value>` parameter map, enabling query template reuse and plan caching.

### Data Flow

```
execute("MATCH (n:Person {name: $name}) RETURN n", {{"name", Value("Alice")}})
    → Parser: $name → ParameterExpression("name")
    → Parameters map propagated through pipeline to EvaluationContext
    → ExpressionEvaluator resolves ParameterExpression from context parameters
```

### Changes Required

#### 1.1 Grammar Changes (`CypherLexer.g4` / `CypherParser.g4`)

Add lexer rule for parameter token:
```antlr
// CypherLexer.g4
PARAMETER : '$' IDENTIFIER ;
```

Add parser rule for parameter in expressions:
```antlr
// CypherParser.g4 — inside atom rule
atom : literal | variable | parameter | functionInvocation | ...
parameter : PARAMETER ;
```

After modifying `.g4` files, regenerate with `bash src/query/parser/cypher/generate.sh`.

#### 1.2 New Expression: `ParameterExpression`

**File**: `include/graph/query/expressions/ParameterExpression.hpp`

```cpp
class ParameterExpression : public Expression {
public:
    explicit ParameterExpression(std::string paramName);
    [[nodiscard]] ExpressionType getType() const override { return ExpressionType::PARAMETER; }
    [[nodiscard]] const std::string &getParameterName() const { return paramName_; }
    [[nodiscard]] PropertyValue evaluate(const EvaluationContext &ctx) const override;
    [[nodiscard]] std::unique_ptr<Expression> clone() const override;
    void accept(ExpressionVisitor &visitor) const override;
    void accept(ConstExpressionVisitor &visitor) const override;
    [[nodiscard]] std::string toString() const override;
private:
    std::string paramName_;
};
```

Add `PARAMETER` to the `ExpressionType` enum.

#### 1.3 ExpressionBuilder Changes

**File**: `src/query/parser/cypher/helpers/ExpressionBuilder.cpp`

In the method that handles atom expressions, detect `CypherParser::ParameterContext` and create `ParameterExpression` instead of `LiteralExpression`.

#### 1.4 EvaluationContext Changes

**File**: `include/graph/query/expressions/EvaluationContext.hpp`

Add parameter storage and resolution:

```cpp
class EvaluationContext {
public:
    // Existing constructors + new overloads with parameters
    EvaluationContext(const execution::Record &record,
                      const std::unordered_map<std::string, PropertyValue> &parameters);
    EvaluationContext(const execution::Record &record,
                      storage::DataManager *dataManager,
                      const std::unordered_map<std::string, PropertyValue> &parameters);

    [[nodiscard]] std::optional<PropertyValue> resolveParameter(const std::string &name) const;

private:
    const std::unordered_map<std::string, PropertyValue> *parameters_ = nullptr;
};
```

Parameters are passed by pointer (non-owning) for zero-copy. The map lifetime is guaranteed by the caller (API layer holds it for the duration of execution).

#### 1.5 ExpressionEvaluator Changes

**File**: `include/graph/query/expressions/ExpressionEvaluator.hpp`

Add visitor method:

```cpp
void visit(const ParameterExpression *expr) override;
```

Implementation: call `context_.resolveParameter(expr->getParameterName())`, throw if not found.

#### 1.6 Parameter Propagation Through Pipeline

Parameters must flow from the API layer down to every `EvaluationContext` creation site:

- `QueryEngine::execute(query, params)` — new overload accepting params
- `IQueryParser::parse(query, params)` — new overload, stores params for later
- `CypherToPlanVisitor` — carries params, passes to logical plan metadata
- `PhysicalPlanConverter` — passes params to physical operators that create `EvaluationContext`
- Physical operators (`FilterOperator`, `ProjectOperator`, etc.) — store params reference, pass to `EvaluationContext` constructor

**Implementation approach**: Add `parameters` field to a `QueryContext` struct that is threaded through the pipeline, rather than adding parameters to every function signature individually.

```cpp
struct QueryContext {
    std::unordered_map<std::string, PropertyValue> parameters;
    // Future: other execution-time context (timeout, memory limit, etc.)
};
```

#### 1.7 C++ Public API Changes

**File**: `include/zyx/zyx.hpp`

```cpp
class Database {
    // Existing
    [[nodiscard]] Result execute(const std::string &cypher) const;
    // New
    [[nodiscard]] Result execute(const std::string &cypher,
                                  const std::unordered_map<std::string, Value> &params) const;
};

class Transaction {
    // Existing
    Result execute(const std::string &cypher) const;
    // New
    Result execute(const std::string &cypher,
                   const std::unordered_map<std::string, Value> &params) const;
};
```

The implementation converts `Value` params to internal `PropertyValue` params before calling `QueryEngine`.

#### 1.8 Parameter Validation

- Unknown parameters (present in query but missing from map): throw `std::runtime_error` with clear message at evaluation time
- Unused parameters (present in map but not in query): silently ignored (consistent with Neo4j behavior)
- Null parameter value: allowed, treated as Cypher NULL

---

## Module 2: Query Plan Cache

### Goal

Cache optimized logical plans to skip ANTLR4 parsing and rule-based optimization for repeated query templates. This is most effective with parameterized queries where the same template is executed with different parameter values.

### Cache Architecture

**Cache level**: Optimized `LogicalOperator` trees (post-optimization, pre-physical-conversion).

**Rationale**: Physical plans hold references to `DataManager`, `IndexManager`, etc. — they cannot be safely cached. Logical plans are pure data structures with `clone()` support, making them safe to cache and copy.

**Cache key**: Raw query string (for parameterized queries, all executions with same template share the same key).

**Cache implementation**: LRU cache with configurable max size (default: 128 entries).

### Data Flow

```
QueryEngine::execute(query, params)
    → PlanCache::get(query)
        → Hit:  clone cached LogicalOperator
        → Miss: parse(query) → optimize(logicalPlan) → cache clone
    → PhysicalPlanConverter::convert(logicalPlan)  // always runs
    → Bind params to QueryContext
    → QueryExecutor::execute(physicalPlan)
```

### Changes Required

#### 2.1 New Class: `PlanCache`

**File**: `include/graph/query/cache/PlanCache.hpp`

```cpp
class PlanCache {
public:
    explicit PlanCache(size_t maxSize = 128);

    // Returns cloned plan if cached, nullopt if miss
    [[nodiscard]] std::unique_ptr<logical::LogicalOperator> get(const std::string &query) const;

    // Stores a clone of the plan
    void put(const std::string &query, const logical::LogicalOperator *plan);

    void clear();
    [[nodiscard]] size_t size() const;
    [[nodiscard]] size_t hits() const;
    [[nodiscard]] size_t misses() const;

private:
    size_t maxSize_;
    mutable std::mutex mutex_;
    mutable std::list<std::pair<std::string, std::unique_ptr<logical::LogicalOperator>>> entries_;
    mutable std::unordered_map<std::string, decltype(entries_)::iterator> index_;
    mutable size_t hits_ = 0;
    mutable size_t misses_ = 0;
};
```

Thread-safe via mutex. LRU eviction: on `get()`, move accessed entry to front; on `put()`, evict from back if at capacity.

#### 2.2 Parser Interface Refactor

Current `IQueryParser::parse()` returns `PhysicalOperator` directly. We need to split this into two phases:

```cpp
class IQueryParser {
    // Existing (keep for backward compat, calls parseToLogical + convert internally)
    [[nodiscard]] virtual std::unique_ptr<execution::PhysicalOperator>
    parse(const std::string &query) const = 0;

    // New: returns optimized logical plan (cacheable)
    [[nodiscard]] virtual std::unique_ptr<logical::LogicalOperator>
    parseToLogical(const std::string &query) const = 0;
};
```

`CypherParserImpl::parseToLogical()`: runs ANTLR4 parse → visitor → optimizer, returns the `LogicalOperator` tree before physical conversion.

`CypherToPlanVisitor::getPlan()` currently does optimize + convert. Split into:
- `getLogicalPlan()` — optimize and return logical plan
- Keep `getPlan()` as convenience that calls `getLogicalPlan()` + convert

#### 2.3 QueryEngine Integration

```cpp
QueryResult QueryEngine::execute(const std::string &query, const QueryContext &ctx) {
    std::unique_ptr<logical::LogicalOperator> logicalPlan;

    // 1. Try cache
    logicalPlan = planCache_->get(query);

    if (!logicalPlan) {
        // 2. Cache miss: parse and optimize
        logicalPlan = getParser(Language::Cypher)->parseToLogical(query);
        planCache_->put(query, logicalPlan.get());
    }

    // 3. Convert to physical (always, because physical plans hold runtime refs)
    PhysicalPlanConverter converter(dataManager_, indexManager_, constraintManager_);
    auto physicalPlan = converter.convert(logicalPlan.get());

    // 4. Bind params and execute
    physicalPlan->setQueryContext(ctx);
    propagateThreadPool(physicalPlan.get());
    return queryExecutor_->execute(std::move(physicalPlan));
}
```

#### 2.4 Cache Invalidation

The cache must be invalidated when schema changes occur:
- `CREATE INDEX` / `DROP INDEX` — changes affect optimizer decisions
- `CREATE CONSTRAINT` / `DROP CONSTRAINT` — same reason

Implementation: `PlanCache::clear()` is called by `QueryEngine` when it detects DDL operations. Detection: check if the parsed logical plan contains DDL operator types before caching.

DDL queries (containing `LOP_CREATE_INDEX`, `LOP_DROP_INDEX`, etc.) are never cached.

---

## Module 3: C API Improvements

### 3a. Transaction Control API

**New opaque handle**:
```c
typedef struct ZYXTxn_T ZYXTxn_T;
```

**Functions**:
```c
ZYXTxn_T *zyx_begin_transaction(ZYXDB_T *db);
ZYXResult_T *zyx_txn_execute(ZYXTxn_T *txn, const char *cypher);
ZYXResult_T *zyx_txn_execute_params(ZYXTxn_T *txn, const char *cypher, const ZYXParams_T *params);
bool zyx_txn_commit(ZYXTxn_T *txn);
void zyx_txn_rollback(ZYXTxn_T *txn);
void zyx_txn_close(ZYXTxn_T *txn);  // auto-rollback if not committed
```

**Internal structure**:
```cpp
struct ZYXTxn_T {
    zyx::Transaction cpp_txn;
};
```

`zyx_txn_close` destroys the struct. If the transaction is still active (not committed/rolled back), the `zyx::Transaction` destructor auto-rolls back (existing RAII behavior).

### 3b. Parameterized Query C API

**New opaque handle**:
```c
typedef struct ZYXParams_T ZYXParams_T;
```

**Functions**:
```c
ZYXParams_T *zyx_params_create();
void zyx_params_set_int(ZYXParams_T *p, const char *key, int64_t val);
void zyx_params_set_double(ZYXParams_T *p, const char *key, double val);
void zyx_params_set_string(ZYXParams_T *p, const char *key, const char *val);
void zyx_params_set_bool(ZYXParams_T *p, const char *key, bool val);
void zyx_params_set_null(ZYXParams_T *p, const char *key);
void zyx_params_close(ZYXParams_T *p);

ZYXResult_T *zyx_execute_params(const ZYXDB_T *db, const char *cypher, const ZYXParams_T *params);
```

**Internal structure**:
```cpp
struct ZYXParams_T {
    std::unordered_map<std::string, zyx::Value> params;
};
```

### 3c. Batch Operations C API

Expose existing C++ batch capabilities:

```c
// Create a single node with properties, returns success
bool zyx_create_node(ZYXDB_T *db, const char *label, const ZYXParams_T *props);

// Create a node and return its internal ID
int64_t zyx_create_node_ret_id(ZYXDB_T *db, const char *label, const ZYXParams_T *props);

// Create an edge between two nodes by their internal IDs
bool zyx_create_edge_by_id(ZYXDB_T *db, int64_t source_id, int64_t target_id,
                            const char *label, const ZYXParams_T *props);
```

`ZYXParams_T` is reused for property maps — it's the same structure as query parameters.

### 3d. List/Map Value Type Extension

#### New type enum values:
```c
typedef enum {
    ZYX_NULL = 0,
    ZYX_BOOL = 1,
    ZYX_INT = 2,
    ZYX_DOUBLE = 3,
    ZYX_STRING = 4,
    ZYX_NODE = 5,
    ZYX_EDGE = 6,
    ZYX_LIST = 7,    // NEW
    ZYX_MAP = 8      // NEW
} ZYXValueType;
```

#### List access functions:
```c
int zyx_result_list_size(const ZYXResult_T *res, int col_index);
ZYXValueType zyx_result_list_get_type(const ZYXResult_T *res, int col_index, int list_index);
int64_t zyx_result_list_get_int(const ZYXResult_T *res, int col_index, int list_index);
double zyx_result_list_get_double(const ZYXResult_T *res, int col_index, int list_index);
bool zyx_result_list_get_bool(const ZYXResult_T *res, int col_index, int list_index);
const char *zyx_result_list_get_string(ZYXResult_T *res, int col_index, int list_index);
```

#### Map access via JSON:
```c
const char *zyx_result_get_map_json(ZYXResult_T *res, int col_index);
```

Maps are exposed as JSON strings for simplicity. Individual key access can be added later if needed.

#### Value type changes in `value.hpp`:

The current `zyx::Value` variant already contains `vector<float>` and `vector<string>`. For proper List support, we need to also handle `vector<Value>` for heterogeneous lists. However, since `PropertyValue` internally uses `vector<PropertyValue>` for lists and this is converted to `vector<string>` in `toPublicValue()`, the change is:

- Update `toPublicValue()` to preserve list structure as `vector<Value>` instead of flattening to `vector<string>`
- Add `std::unordered_map<std::string, Value>` variant for Map type (index 9)
- Update C API type detection to recognize these new variants

---

## Testing Strategy

### Unit Tests

- **Parameterized queries**: Parse `$param` in various positions (WHERE, property match, RETURN expression, SET, function args), verify `ParameterExpression` creation, test missing param error, test null params
- **Plan cache**: Hit/miss counting, LRU eviction, DDL invalidation, thread safety, clone correctness
- **C API**: Transaction lifecycle, params creation/access, batch operations, List/Map type access

### Integration Tests

- End-to-end parameterized query execution with various data types
- Plan cache effectiveness: execute same template N times, verify parse count
- C API transaction: begin → execute → commit → verify data persisted
- C API batch: create nodes/edges via C API, query back via Cypher

---

## Non-Goals

- **Cache warming/preheating**: Deferred as agreed — no startup-time cache population
- **Prepared statements with handle reuse**: Not implementing a separate `prepare()` → `execute()` two-step API. The plan cache achieves similar performance transparently
- **Distributed cache**: Single-process only, no shared-memory or networked cache
