# CONSTRAINT Support Design Spec

## Overview

Add full CONSTRAINT support to ZYX: UNIQUE, NOT NULL, Property Type, and Node Key constraints. Enforced via a pre-write validation phase in the DataManager write pipeline.

## Architecture: Two-Phase Entity Lifecycle

Current DataManager write pipeline: `Write → Notify (IEntityObserver)`

New pipeline: `Validate (IEntityValidator) → Write → Notify (IEntityObserver)`

- `IEntityValidator`: new interface, pre-write, can throw to abort
- `IEntityObserver`: unchanged, post-write notification (IndexManager stays untouched)
- `ConstraintManager`: implements `IEntityValidator`, holds constraint definitions organized by label ID

### Write Path Example

```cpp
void DataManager::addNode(Node &node) const {
    // Phase 1: Validate
    for (auto &v : validators_) v->validateNodeInsert(node, props);
    // Phase 2: Write (unchanged)
    nodeManager_->add(node);
    // ... WAL, txn tracking ...
    // Phase 3: Notify (unchanged)
    notifyNodeAdded(node);
}
```

For `addNodeProperties` (SET operations), the validate phase receives both old and new property maps so constraints can check the delta.

For `addNodes` (batch inserts), validators are called per-node before the batch write. This ensures atomicity — if any node violates a constraint, the entire batch fails before any write occurs.

## Constraint Types

### 1. UNIQUE Constraint

- **Enforcement**: Before insert/update, query `PropertyIndex::findExactMatch(key, value)`. If any existing entity (with matching label, excluding self on update) has the same value, throw `ConstraintViolationException`.
- **Backing index**: Automatically creates a `PropertyIndex` if one doesn't exist for the constrained property. The same B+Tree serves both query optimization and constraint enforcement.
- **Null handling**: NULL values are NOT considered duplicates (SQL standard behavior). Multiple entities can have NULL for a UNIQUE property.

### 2. NOT NULL Constraint

- **Enforcement**: Pure in-memory check. On insert: `props.contains(key) && !isNull(props[key])`. On update: same check on new props. On property removal (`REMOVE n.prop`): check that the removed property is not constrained.
- **No backing index needed**.

### 3. Property Type Constraint (`IS :: TYPE`)

- **Enforcement**: Pure in-memory check. `props[key].getType() == expectedType`. Only checked when the property exists (combine with NOT NULL to require both existence and type).
- **Supported types**: `BOOLEAN`, `INTEGER`, `DOUBLE`, `STRING`, `LIST`, `MAP` — maps directly to `PropertyType` enum.
- **No backing index needed**.

### 4. Node Key Constraint

- **Enforcement**: Compound constraint = NOT NULL on all key properties + UNIQUE on the composite key.
- **Composite uniqueness**: For properties `(p1, p2, ...)`, concatenate their string representations with a separator and check uniqueness of the composite value via PropertyIndex.
- **Backing index**: Creates a composite property index keyed on the concatenated value.

## Interface Design

### IEntityValidator

```cpp
class IEntityValidator {
public:
    virtual ~IEntityValidator() = default;

    virtual void validateNodeInsert(const Node &node, const PropertyMap &props) {}
    virtual void validateNodeUpdate(const Node &oldNode, const Node &newNode,
                                     const PropertyMap &oldProps, const PropertyMap &newProps) {}
    virtual void validateNodeDelete(const Node &node) {}

    virtual void validateEdgeInsert(const Edge &edge, const PropertyMap &props) {}
    virtual void validateEdgeUpdate(const Edge &oldEdge, const Edge &newEdge,
                                     const PropertyMap &oldProps, const PropertyMap &newProps) {}
    virtual void validateEdgeDelete(const Edge &edge) {}
};
```

### IConstraint (Strategy)

```cpp
enum class ConstraintType : uint8_t {
    CT_UNIQUE = 1,
    CT_NOT_NULL = 2,
    CT_PROPERTY_TYPE = 3,
    CT_NODE_KEY = 4
};

class IConstraint {
public:
    virtual ~IConstraint() = default;
    virtual void validateInsert(int64_t entityId, const PropertyMap &props) = 0;
    virtual void validateUpdate(int64_t entityId, const PropertyMap &oldProps,
                                 const PropertyMap &newProps) = 0;
    virtual ConstraintType getType() const = 0;
    virtual std::string getName() const = 0;
    virtual std::string getLabel() const = 0;
    virtual std::vector<std::string> getProperties() const = 0;
};
```

### ConstraintManager

```cpp
class ConstraintManager : public IEntityValidator {
public:
    // DDL operations
    void createConstraint(const ConstraintDefinition &def);
    void dropConstraint(const std::string &name);
    std::vector<ConstraintInfo> listConstraints() const;

    // IEntityValidator implementation
    void validateNodeInsert(const Node &node, const PropertyMap &props) override;
    void validateNodeUpdate(const Node &oldNode, const Node &newNode,
                             const PropertyMap &oldProps, const PropertyMap &newProps) override;
    // ... edges too

    // Lifecycle
    void initialize();  // Load from SystemStateManager
    void persistState() const;  // Save to SystemStateManager

private:
    // Label ID → constraints for that label. O(1) lookup, fast path when empty.
    std::unordered_map<int64_t, std::vector<std::unique_ptr<IConstraint>>> nodeConstraints_;
    std::unordered_map<int64_t, std::vector<std::unique_ptr<IConstraint>>> edgeConstraints_;
};
```

## Constraint Metadata Persistence

Stored in `SystemStateManager` under key `sys.constraints`.

Format: `constraintName → "entityType|constraintType|label|prop1,prop2|options"`

Examples:
- `"person_email_unique" → "node|unique|Person|email|"`
- `"person_name_notnull" → "node|not_null|Person|name|"`
- `"person_age_type" → "node|property_type|Person|age|INTEGER"`
- `"person_composite_key" → "node|node_key|Person|firstName,lastName|"`

## UNIQUE and PropertyIndex Integration

When creating a UNIQUE constraint:
1. Check if a PropertyIndex exists for `(entityType, property)`
2. If not, auto-create one via `IndexManager::createIndex()`
3. Scan all existing entities to verify no duplicates exist (fail-fast if data violates constraint)
4. Register the constraint

When dropping a UNIQUE constraint:
1. Remove the constraint from ConstraintManager
2. Keep the backing PropertyIndex (user may still want it for queries)
3. User can explicitly `DROP INDEX` if desired

## Existing Data Validation

When creating a constraint on a table with existing data:
- Scan all entities with the target label
- Check every entity against the new constraint
- If any violation is found, fail the CREATE CONSTRAINT with a descriptive error listing the violating entities
- This is a potentially expensive operation for large datasets, but it guarantees data integrity

## Cypher Syntax

### CREATE CONSTRAINT

```cypher
-- Unique
CREATE CONSTRAINT person_email_unique FOR (n:Person) REQUIRE n.email IS UNIQUE

-- Not null
CREATE CONSTRAINT person_name_notnull FOR (n:Person) REQUIRE n.name IS NOT NULL

-- Property type
CREATE CONSTRAINT person_age_type FOR (n:Person) REQUIRE n.age IS :: INTEGER

-- Node key (compound unique + not null)
CREATE CONSTRAINT person_key FOR (n:Person) REQUIRE (n.firstName, n.lastName) IS NODE KEY

-- Edge constraints
CREATE CONSTRAINT edge_weight FOR ()-[r:KNOWS]-() REQUIRE r.weight IS NOT NULL
```

### DROP CONSTRAINT

```cypher
DROP CONSTRAINT person_email_unique
DROP CONSTRAINT person_email_unique IF EXISTS
```

### SHOW CONSTRAINTS

```cypher
SHOW CONSTRAINTS
```

Returns table: `name | entityType | constraintType | label | properties`

## Grammar Changes

In `CypherParser.g4`, add to `administrationStatement`:

```antlr
administrationStatement
    : showIndexesStatement
    | dropIndexStatement
    | createIndexStatement
    | transactionStatement
    | createConstraintStatement    // NEW
    | dropConstraintStatement      // NEW
    | showConstraintsStatement     // NEW
    ;

createConstraintStatement
    : K_CREATE K_CONSTRAINT constraintName=symbolicName
      K_FOR nodeConstraintPattern
      K_REQUIRE constraintBody
    | K_CREATE K_CONSTRAINT constraintName=symbolicName
      K_FOR edgeConstraintPattern
      K_REQUIRE constraintBody
    ;

nodeConstraintPattern
    : '(' variable=symbolicName ':' label=symbolicName ')'
    ;

edgeConstraintPattern
    : '(' ')' '-' '[' variable=symbolicName ':' label=symbolicName ']' '-' '(' ')'
    ;

constraintBody
    : propertyExpression K_IS K_UNIQUE                          # uniqueConstraint
    | propertyExpression K_IS K_NOT K_NULL                      # notNullConstraint
    | propertyExpression K_IS '::' typeName                     # propertyTypeConstraint
    | '(' propertyExpressionList ')' K_IS K_NODE K_KEY          # nodeKeyConstraint
    ;

propertyExpression
    : variable=symbolicName '.' property=symbolicName
    ;

propertyExpressionList
    : propertyExpression (',' propertyExpression)*
    ;

typeName
    : K_BOOLEAN | K_INTEGER | K_FLOAT | K_STRING | K_LIST | K_MAP
    ;

dropConstraintStatement
    : K_DROP K_CONSTRAINT constraintName=symbolicName (K_IF K_EXISTS)?
    ;

showConstraintsStatement
    : K_SHOW K_CONSTRAINT ('S')?  // SHOW CONSTRAINTS or SHOW CONSTRAINT
    ;
```

Lexer tokens needed (most already exist):
- `K_CONSTRAINT` (exists), `K_REQUIRE` (exists), `K_UNIQUE` (exists)
- `K_NOT` (exists), `K_NULL` (exists)
- `K_NODE` (exists), `K_KEY` — need to add `K_KEY`
- `K_BOOLEAN`, `K_INTEGER`, `K_FLOAT`, `K_STRING`, `K_LIST`, `K_MAP` — need to check/add type tokens

## Query Planner & Executor

### Planner Factory Methods

Add to `QueryPlanner`:
- `createConstraintOp(name, entityType, constraintType, label, properties, options)` → `CreateConstraintOperator`
- `dropConstraintOp(name, ifExists)` → `DropConstraintOperator`
- `showConstraintsOp()` → `ShowConstraintsOperator`

### Operators

**CreateConstraintOperator**: Calls `ConstraintManager::createConstraint()`, which:
1. Validates the definition
2. For UNIQUE: ensures/creates backing PropertyIndex
3. Scans existing data for violations
4. Registers the constraint
5. Persists to `sys.constraints`

**DropConstraintOperator**: Calls `ConstraintManager::dropConstraint()`, which:
1. Removes from in-memory map
2. Removes from `sys.constraints`
3. Does NOT drop backing PropertyIndex

**ShowConstraintsOperator**: Calls `ConstraintManager::listConstraints()`, formats as RecordBatch table.

### Visitor → Handler → Planner Flow

`CypherToPlanVisitor::visitCreateConstraintStatement` → `AdminClauseHandler::handleCreateConstraint` → `planner->createConstraintOp(...)` — same pattern as existing index DDL.

## Error Handling

Constraint violations throw `std::runtime_error` with descriptive messages:

- UNIQUE: `"Constraint violation: 'person_email_unique' - Node(:Person) with email = 'x@y.com' already exists (id: 42)"`
- NOT NULL: `"Constraint violation: 'person_name_notnull' - Node(:Person) property 'name' must not be null"`
- Type: `"Constraint violation: 'person_age_type' - Node(:Person) property 'age' must be INTEGER, got STRING"`
- Node Key: `"Constraint violation: 'person_key' - Node(:Person) with (firstName, lastName) = ('John', 'Doe') already exists"`

## Files Summary

### New Files

| File | Purpose |
|------|---------|
| `include/graph/storage/constraints/IEntityValidator.hpp` | Pre-write validation interface |
| `include/graph/storage/constraints/IConstraint.hpp` | Strategy interface for constraint types |
| `include/graph/storage/constraints/ConstraintMeta.hpp` | Metadata serialization (like IndexMeta) |
| `include/graph/storage/constraints/ConstraintManager.hpp` | Orchestrator, implements IEntityValidator |
| `include/graph/storage/constraints/UniqueConstraint.hpp` | UNIQUE strategy |
| `include/graph/storage/constraints/NotNullConstraint.hpp` | NOT NULL strategy |
| `include/graph/storage/constraints/TypeConstraint.hpp` | Property type strategy |
| `include/graph/storage/constraints/NodeKeyConstraint.hpp` | Compound key strategy |
| `src/storage/constraints/ConstraintManager.cpp` | ConstraintManager implementation |
| `src/storage/constraints/UniqueConstraint.cpp` | UNIQUE implementation |
| `include/graph/query/execution/operators/CreateConstraintOperator.hpp` | DDL operator |
| `include/graph/query/execution/operators/DropConstraintOperator.hpp` | DDL operator |
| `include/graph/query/execution/operators/ShowConstraintsOperator.hpp` | DDL operator |
| `tests/src/storage/test_ConstraintManager.cpp` | Unit tests |
| `tests/integration/test_IntegrationConstraint.cpp` | Integration tests |

### Modified Files

| File | Change |
|------|--------|
| `include/graph/storage/data/DataManager.hpp` | Add `validators_` list, `registerValidator()` |
| `src/storage/data/DataManager.cpp` | Call validators before write in add/update/delete methods |
| `include/graph/core/Database.hpp` | Add `ConstraintManager` member |
| `src/core/Database.cpp` | Initialize and register ConstraintManager |
| `include/graph/query/api/QueryEngine.hpp` | Expose `getConstraintManager()` |
| `src/query/api/QueryEngine.cpp` | Wire ConstraintManager |
| `include/graph/query/planner/QueryPlanner.hpp` | Add factory methods |
| `src/query/planner/QueryPlanner.cpp` | Implement factory methods |
| `src/query/parser/cypher/CypherParser.g4` | Add constraint grammar rules |
| `src/query/parser/cypher/CypherLexer.g4` | Add missing tokens (K_KEY, type tokens) |
| `src/query/parser/cypher/CypherToPlanVisitor.hpp/.cpp` | Add visit methods |
| `src/query/parser/cypher/clauses/AdminClauseHandler.hpp/.cpp` | Add handler methods |
| `include/graph/storage/state/SystemStateKeys.hpp` | Add `SYS_CONSTRAINTS` |
| `src/meson.build` | Add new source files |

## Performance Characteristics

| Constraint | Insert Cost | Update Cost | Memory |
|------------|------------|-------------|--------|
| UNIQUE | O(log n) B+Tree lookup | O(log n) | Backed by PropertyIndex |
| NOT NULL | O(1) property check | O(1) | None |
| Type | O(1) type check | O(1) | None |
| Node Key | O(k) NOT NULL + O(log n) composite lookup | O(k) + O(log n) | Backed by PropertyIndex |

Fast path (no constraints for label): single `unordered_map::find()` returning end → near-zero overhead.

## Testing Strategy

### Unit Tests (test_ConstraintManager.cpp)
- Create/drop each constraint type
- UNIQUE: insert duplicate → error, insert unique → success, update to duplicate → error
- UNIQUE: NULL values not considered duplicates
- NOT NULL: insert with missing prop → error, insert with prop → success
- NOT NULL: REMOVE constrained prop → error
- Type: insert wrong type → error, insert correct type → success
- Node Key: compound uniqueness + not null enforcement
- Constraint on existing data with violations → CREATE fails
- Fast path: entities without constrained labels bypass all checks

### Integration Tests (test_IntegrationConstraint.cpp)
- Full Cypher DDL: CREATE CONSTRAINT, DROP CONSTRAINT, SHOW CONSTRAINTS
- Constraint enforcement through Cypher CREATE/SET/MERGE/REMOVE
- Persistence: constraints survive close/reopen
- Transaction rollback: constraint violation in transaction rolls back all changes
- Constraint + Index interaction: UNIQUE auto-creates index, DROP CONSTRAINT keeps index
