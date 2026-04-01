# Range Scan Optimization & Composite Index Design

**Date:** 2026-04-01
**Status:** Approved
**Scope:** Node-only (Edge support deferred)

## Overview

Two complementary enhancements to ZYX's index system:

1. **Range Scan Optimization** — Connect the existing storage-layer `findRange()` to the query execution pipeline, extend it to all comparable types (INTEGER, DOUBLE, STRING), and add `BETWEEN` syntax.
2. **Composite Index** — Multi-property combined indexes using a new `CompositeKey` variant in `PropertyValue`, enabling efficient multi-attribute lookups with leftmost-prefix matching.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Composite key strategy | COMPOSITE PropertyValue variant | Reuses IndexTreeManager, supports prefix matching and range queries |
| Range scan scope | All comparable types (int, double, string) | Full coverage; boolean excluded (no meaningful range) |
| BETWEEN syntax | Yes, desugared to two comparisons at AST level | Clean AST, no new node types needed |
| Edge support | Deferred | Query optimizer currently only handles NodeScan |
| Composite index location | Extend existing PropertyIndex | Avoids code duplication, reuses infrastructure |

---

## Part 1: Range Scan Optimization

### 1.1 ScanConfigs Extension

**File:** `include/graph/query/execution/ScanConfigs.hpp`

```cpp
enum class ScanType { FULL_SCAN, LABEL_SCAN, PROPERTY_SCAN, RANGE_SCAN };

struct NodeScanConfig {
    ScanType type = ScanType::FULL_SCAN;
    std::string variable;
    std::vector<std::string> labels;
    std::string indexKey;
    PropertyValue indexValue;       // PROPERTY_SCAN (equality)
    // New range fields
    PropertyValue rangeMin;         // RANGE_SCAN lower bound (monostate = unbounded)
    PropertyValue rangeMax;         // RANGE_SCAN upper bound (monostate = unbounded)
    bool minInclusive = true;       // >= vs >
    bool maxInclusive = true;       // <= vs <
    std::string label() const;
    std::string toString() const;
};
```

### 1.2 LogicalNodeScan Range Predicates

**File:** `include/graph/query/logical/operators/LogicalNodeScan.hpp`

New struct and member:

```cpp
struct RangePredicate {
    std::string key;
    PropertyValue minValue;     // monostate = no lower bound
    PropertyValue maxValue;     // monostate = no upper bound
    bool minInclusive = true;
    bool maxInclusive = true;
};

// New member alongside existing propertyPredicates_
std::vector<RangePredicate> rangePredicates_;
// Accessors
const std::vector<RangePredicate> &getRangePredicates() const;
void setRangePredicates(std::vector<RangePredicate> predicates);
```

### 1.3 FilterPushdownRule Extension

**File:** `include/graph/query/optimizer/rules/FilterPushdownRule.hpp`

New method `extractPropertyRange()`:

- Matches patterns: `var.prop > literal`, `var.prop >= literal`, `var.prop < literal`, `var.prop <= literal`
- Also matches reversed form: `literal < var.prop` (equivalent to `var.prop > literal`)
- Recognizes operators: `BOP_LESS`, `BOP_GREATER`, `BOP_LESS_EQUAL`, `BOP_GREATER_EQUAL`
- Outputs: property key, bound value, bound direction (min/max), inclusivity

Range predicate merging in `pushDown()`:

- Multiple range predicates on the same property key are merged into a single `RangePredicate`
- Example: `age > 18 AND age < 65` merges to `RangePredicate{key="age", min=18 exclusive, max=65 exclusive}`
- Conflicting bounds (e.g., `age > 30 AND age > 20`) keep the tighter bound (`age > 30`)

### 1.4 EnhancedIndexSelectionRule Extension

**File:** `include/graph/query/optimizer/rules/EnhancedIndexSelectionRule.hpp`

In `selectBestScan()`:

- For each `RangePredicate` where `hasPropertyIndex("node", key)` is true:
  - Compute `CostModel::rangeIndexCost(stats, label, key)` with default selectivity 0.3
  - Compare against current best
- `RANGE_SCAN` can win over `LABEL_SCAN` but `PROPERTY_SCAN` (equality) still preferred when available

CostModel addition:

```cpp
static double rangeIndexCost(const TableStats &stats, const std::string &label,
                             const std::string &key) {
    double baseRows = stats.estimatedRows(label);
    return baseRows * 0.3;  // Range selectivity estimate
}
```

### 1.5 NodeScanOperator Extension

**File:** `include/graph/query/execution/operators/NodeScanOperator.hpp`

New case in `open()`:

```cpp
case ScanType::RANGE_SCAN:
    candidateIds_ = im_->findNodeIdsByPropertyRange(
        config_.indexKey, config_.rangeMin, config_.rangeMax);
    break;
```

### 1.6 IndexManager New API

**File:** `include/graph/storage/indexes/IndexManager.hpp`

```cpp
std::vector<int64_t> findNodeIdsByPropertyRange(
    const std::string &key,
    const PropertyValue &minValue,
    const PropertyValue &maxValue) const;
```

Delegates to `nodeIndexManager_->findPropertyRange(key, minValue, maxValue)`.

### 1.7 PropertyIndex::findRange() Full Type Extension

**File:** `src/storage/indexes/PropertyIndex.cpp`

Current state: Only supports INTEGER and DOUBLE.

Changes:

1. **STRING support**: Use string B+Tree's existing lexicographic comparator. `findRange(key, minStr, maxStr)` performs a range scan on the string tree.
2. **BOOLEAN**: Not supported for range (no meaningful ordering). Range queries on boolean properties fall back to full scan.
3. **Type promotion**: When min/max types differ from indexed type but are promotable (int->double), perform type conversion before querying.
4. **Bug fix**: Current `findRange()` has a re-entrant shared lock issue — calls `getIndexedKeyType(key)` which re-acquires the shared lock. Fix by accessing `indexedKeyTypes_` directly inside the already-held lock scope.

Updated signature to accept `PropertyValue` instead of `double`:

```cpp
std::vector<int64_t> findRange(const std::string &key,
                                const PropertyValue &minValue,
                                const PropertyValue &maxValue) const;
```

### 1.8 BETWEEN Syntax

**Grammar:** `src/query/parser/cypher/CypherParser.g4`

```antlr
oC_ComparisonExpression
    : oC_StringListNullPredicateExpression
      ( SP? oC_PartialComparisonExpression )*
    | oC_StringListNullPredicateExpression SP BETWEEN SP
      oC_StringListNullPredicateExpression SP AND SP
      oC_StringListNullPredicateExpression
    ;
```

**Desugaring:** In `ExpressionBuilder`, `BETWEEN a AND b` is transformed to:

```
BinaryOpExpression(BOP_AND,
    BinaryOpExpression(BOP_GREATER_EQUAL, expr, a),
    BinaryOpExpression(BOP_LESS_EQUAL, expr, b))
```

This keeps the AST clean — no new expression types needed. The existing `FilterPushdownRule` range extraction handles the desugared form naturally.

---

## Part 2: Composite Index

### 2.1 PropertyValue COMPOSITE Extension

**File:** `include/graph/core/PropertyTypes.hpp`

```cpp
enum class PropertyType : uint32_t {
    UNKNOWN = 0, NULL_TYPE = 1, BOOLEAN = 2, INTEGER = 3,
    DOUBLE = 4, STRING = 5, LIST = 6, MAP = 7,
    COMPOSITE = 8  // New
};

struct CompositeKey {
    std::vector<PropertyValue> components;

    bool operator==(const CompositeKey &other) const;
    bool operator<(const CompositeKey &other) const;  // Lexicographic by component
    std::string toString() const;
};

// PropertyValue variant adds CompositeKey
using ValueVariant = std::variant<
    std::monostate, bool, int64_t, double, std::string,
    std::vector<PropertyValue>, MapType, CompositeKey>;
```

Comparison semantics: field-by-field lexicographic. `("Alice", 30) < ("Alice", 31) < ("Bob", 25)`. This naturally supports prefix matching via range scans.

### 2.2 IndexTreeManager COMPOSITE Comparator

**File:** `include/graph/core/IndexTreeManager.hpp`

A new `IndexTreeManager` instance with a composite comparator:

```cpp
auto compositeComparator = [](const PropertyValue &a, const PropertyValue &b) -> int {
    const auto &ka = std::get<CompositeKey>(a.getVariant());
    const auto &kb = std::get<CompositeKey>(b.getVariant());
    size_t len = std::min(ka.components.size(), kb.components.size());
    for (size_t i = 0; i < len; ++i) {
        if (ka.components[i] < kb.components[i]) return -1;
        if (kb.components[i] < ka.components[i]) return 1;
    }
    if (ka.components.size() < kb.components.size()) return -1;
    if (ka.components.size() > kb.components.size()) return 1;
    return 0;
};
```

`indexType_` = `COMPOSITE_NODE_PROPERTY_TYPE` (7), `keyDataType_` = `PropertyType::COMPOSITE`.

### 2.3 PropertyIndex Extension for Composite

**File:** `include/graph/storage/indexes/PropertyIndex.hpp`

New members:

```cpp
std::unique_ptr<IndexTreeManager> compositeTreeManager_;
std::unordered_map<std::string, int64_t> compositeRootIds_;  // "name,age" -> rootId
std::unordered_map<std::string, std::vector<std::string>> compositeKeyDefinitions_;  // "name,age" -> ["name","age"]
```

New methods:

```cpp
void createCompositeIndex(const std::vector<std::string> &keys);
void removeCompositeIndex(const std::vector<std::string> &keys);
bool hasCompositeIndex(const std::vector<std::string> &keys) const;

void addCompositeEntry(int64_t entityId,
                       const std::vector<std::string> &keys,
                       const std::vector<PropertyValue> &values);
void removeCompositeEntry(int64_t entityId,
                          const std::vector<std::string> &keys,
                          const std::vector<PropertyValue> &values);

// Exact match on all key components
std::vector<int64_t> findCompositeExact(
    const std::vector<std::string> &keys,
    const std::vector<PropertyValue> &values) const;

// Prefix match: only first N keys specified
std::vector<int64_t> findCompositePrefix(
    const std::vector<std::string> &prefixKeys,
    const std::vector<PropertyValue> &prefixValues) const;

// Range on composite key
std::vector<int64_t> findCompositeRange(
    const std::vector<std::string> &keys,
    const PropertyValue &minKey,
    const PropertyValue &maxKey) const;
```

Composite key string: properties joined by comma (`"name,age"`), used as map key for root IDs.

State persistence keys:

```
node.index.property.composite_roots     (map: "name,age" -> rootId)
node.index.property.composite_defs      (map: "name,age" -> "name,age")
```

### 2.4 IndexManager Extension

**File:** `include/graph/storage/indexes/IndexManager.hpp`

New IndexType constant:

```cpp
static constexpr uint32_t COMPOSITE_NODE_PROPERTY_TYPE = 7;
```

IndexMetadata extension:

```cpp
struct IndexMetadata {
    std::string name;
    std::string entityType;   // "node" or "edge"
    std::string indexType;    // "label", "property", "composite", "vector"
    std::string label;
    std::string property;                   // Single-property index
    std::vector<std::string> properties;    // Composite index (new)
    bool isComposite() const { return !properties.empty() && properties.size() > 1; }
};
```

Serialization format: `"node|composite|User|name,age"` (properties comma-separated in property field).

API extension:

```cpp
void createIndex(const std::string &indexName, const std::string &entityType,
                 const std::string &label, const std::vector<std::string> &properties);

bool hasCompositeIndex(const std::string &entityType,
                       const std::vector<std::string> &keys) const;

std::vector<int64_t> findNodeIdsByCompositeIndex(
    const std::vector<std::string> &keys,
    const std::vector<PropertyValue> &values) const;
```

### 2.5 CREATE INDEX Syntax Extension

**Grammar:** `src/query/parser/cypher/CypherParser.g4`

The property list in `ON (n.prop1, n.prop2)` becomes a comma-separated list:

```antlr
oC_CreateIndexByPattern
    : CREATE SP INDEX (SP oC_SymbolicName)? SP FOR SP
      '(' SP? oC_NodePattern SP? ')' SP ON SP
      '(' SP? oC_PropertyExpression (SP? ',' SP? oC_PropertyExpression)* SP? ')'
    ;
```

**AdminClauseHandler changes:**

- When multiple `oC_PropertyExpression` items are found, extract each property key into a `vector<string>`
- If `size == 1`: create single-property index (existing path)
- If `size > 1`: create composite index (new path)

**LogicalCreateIndex extension:**

```cpp
class LogicalCreateIndex : public LogicalOperator {
    // Existing: single property
    LogicalCreateIndex(std::string indexName, std::string label, std::string propertyKey);
    // New: composite
    LogicalCreateIndex(std::string indexName, std::string label, std::vector<std::string> propertyKeys);

    bool isComposite() const { return propertyKeys_.size() > 1; }
    const std::vector<std::string> &getPropertyKeys() const;

private:
    std::vector<std::string> propertyKeys_;  // replaces single propertyKey_
};
```

### 2.6 Query Optimizer Integration

**FilterPushdownRule:**

When multiple equality predicates exist on the same node variable (`n.name = 'Alice' AND n.age = 30`), check if any composite index covers a prefix of those predicates. If so, push down as a composite predicate.

New member in `LogicalNodeScan`:

```cpp
struct CompositeEqualityPredicate {
    std::vector<std::string> keys;
    std::vector<PropertyValue> values;
};
std::optional<CompositeEqualityPredicate> compositeEquality_;
```

**EnhancedIndexSelectionRule:**

New scan type `COMPOSITE_SCAN` in `ScanType` enum. Cost model for composite:

```cpp
static double compositeIndexCost(const TableStats &stats, const std::string &label,
                                  size_t matchedFields) {
    double baseRows = stats.estimatedRows(label);
    // Each matched field improves selectivity
    double selectivity = std::pow(0.1, matchedFields);
    return baseRows * selectivity;
}
```

Composite index preferred when it matches >= 2 predicate fields.

**NodeScanConfig extension:**

```cpp
struct NodeScanConfig {
    // ... existing fields ...
    // New composite fields
    std::vector<std::string> compositeKeys;
    std::vector<PropertyValue> compositeValues;
};
```

**NodeScanOperator extension:**

```cpp
case ScanType::COMPOSITE_SCAN:
    candidateIds_ = im_->findNodeIdsByCompositeIndex(
        config_.compositeKeys, config_.compositeValues);
    break;
```

### 2.7 Event Handling (onNodeAdded/Updated)

**IndexManager::onNodeAdded():**

For each composite index definition:

1. Extract all required property values from the node
2. If any property is missing (monostate), skip this node for this index
3. Construct `CompositeKey` from extracted values
4. Insert into composite B+Tree

**IndexManager::onNodeUpdated():**

1. Check if any composite index key property changed
2. If yes: remove old composite entry, insert new one
3. Handle property addition/removal (node may enter/exit composite index)

---

## Testing Strategy

### Range Scan Tests

1. **Unit tests** for `FilterPushdownRule::extractPropertyRange()` — all operator variants
2. **Unit tests** for range predicate merging (same-key multiple bounds)
3. **Unit tests** for `PropertyIndex::findRange()` with STRING, INTEGER, DOUBLE types
4. **Unit tests** for `NodeScanOperator` with `RANGE_SCAN` config
5. **Integration tests**: `WHERE n.age > 30`, `WHERE n.name >= 'M'`, `WHERE n.age BETWEEN 18 AND 65`
6. **Edge cases**: unbounded range (only min or only max), empty result, single-element result

### Composite Index Tests

1. **Unit tests** for `CompositeKey` comparison (lexicographic ordering)
2. **Unit tests** for `PropertyIndex` composite CRUD operations
3. **Unit tests** for composite index prefix matching
4. **Unit tests** for `FilterPushdownRule` composite predicate detection
5. **Integration tests**: create composite index, query with all keys, query with prefix
6. **Edge cases**: missing properties, NULL values, duplicate entries, index rebuild

### BETWEEN Syntax Tests

1. **Parser tests**: `BETWEEN` keyword parsing, desugaring to AND of two comparisons
2. **Integration tests**: `WHERE n.age BETWEEN 18 AND 65` end-to-end

---

## Files Changed Summary

### Range Scan (existing files modified)

| File | Change |
|------|--------|
| `include/graph/query/execution/ScanConfigs.hpp` | Add `RANGE_SCAN`, range fields |
| `include/graph/query/logical/operators/LogicalNodeScan.hpp` | Add `RangePredicate`, rangePredicates_ |
| `include/graph/query/optimizer/rules/FilterPushdownRule.hpp` | Add `extractPropertyRange()`, range merging |
| `include/graph/query/optimizer/rules/EnhancedIndexSelectionRule.hpp` | Add range cost model, RANGE_SCAN selection |
| `include/graph/query/execution/operators/NodeScanOperator.hpp` | Add RANGE_SCAN case |
| `include/graph/storage/indexes/IndexManager.hpp` | Add `findNodeIdsByPropertyRange()` |
| `src/storage/indexes/IndexManager.cpp` | Implement range delegation |
| `include/graph/storage/indexes/PropertyIndex.hpp` | Change `findRange()` signature |
| `src/storage/indexes/PropertyIndex.cpp` | Full-type findRange(), fix lock bug |
| `src/query/parser/cypher/CypherParser.g4` | Add BETWEEN syntax |
| `src/query/parser/cypher/CypherLexer.g4` | Add BETWEEN token |
| `src/query/parser/cypher/helpers/ExpressionBuilder.*` | BETWEEN desugaring |

### Composite Index (existing files modified + minimal new)

| File | Change |
|------|--------|
| `include/graph/core/PropertyTypes.hpp` | Add COMPOSITE type, CompositeKey struct |
| `include/graph/storage/indexes/PropertyIndex.hpp` | Add composite members and methods |
| `src/storage/indexes/PropertyIndex.cpp` | Implement composite index logic |
| `include/graph/storage/indexes/IndexManager.hpp` | Add composite API, IndexMetadata.properties |
| `src/storage/indexes/IndexManager.cpp` | Implement composite index creation, event handling |
| `include/graph/query/logical/operators/LogicalCreateIndex.hpp` | Support multiple property keys |
| `include/graph/query/logical/operators/LogicalNodeScan.hpp` | Add compositeEquality_ |
| `include/graph/query/execution/ScanConfigs.hpp` | Add COMPOSITE_SCAN, composite fields |
| `include/graph/query/execution/operators/NodeScanOperator.hpp` | Add COMPOSITE_SCAN case |
| `include/graph/query/optimizer/rules/FilterPushdownRule.hpp` | Composite predicate detection |
| `include/graph/query/optimizer/rules/EnhancedIndexSelectionRule.hpp` | Composite cost model |
| `src/query/parser/cypher/CypherParser.g4` | Multi-property syntax |
| `src/query/parser/cypher/clauses/AdminClauseHandler.cpp` | Multi-property extraction |
