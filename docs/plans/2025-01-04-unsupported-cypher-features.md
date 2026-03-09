# Unsupported Cypher Features Implementation Plan

> **Goal**: Implement remaining high-priority Cypher features for complete query language support

## Priority Assessment

Based on value and complexity, here's the recommended implementation order:

### Phase 1: Quantifier Functions (Priority: HIGH)
**Complexity**: Medium | **Value**: High | **Dependencies**: None

**Functions to implement:**
- `all(x IN list WHERE condition)` - All elements must satisfy
- `any(x IN list WHERE condition)` - At least one element satisfies
- `none(x IN list WHERE condition)` - No elements satisfy
- `single(x IN list WHERE condition)` - Exactly one element satisfies

**Why first:**
- Works with existing heterogeneous list support
- Provides powerful list filtering capabilities
- Relatively straightforward to implement
- High user value

### Phase 2: EXISTS Pattern Expressions (Priority: HIGH)
**Complexity**: High | **Value**: Very High | **Dependencies**: Pattern parsing

**Syntax variations:**
- `EXISTS((n)-[:FRIENDS]->())` - Pattern existence check
- `EXISTS { MATCH (n)-[:FRIENDS]->() }` - Subquery existence

**Why second:**
- Core Cypher feature used extensively
- Enables complex graph queries
- High user demand

### Phase 3: Pattern Comprehensions (Priority: MEDIUM)
**Complexity**: Medium | **Value**: Medium | **Dependencies**: List comprehensions

**Syntax:**
- `[(n)-[:FRIENDS]->(m) | m.name]` - Extract from patterns

**Why third:**
- Builds on existing list comprehension
- Natural extension of current capabilities

### Phase 4: OPTIONAL MATCH (Priority: MEDIUM)
**Complexity**: High | **Value**: High | **Dependencies**: Query planner

**Syntax:**
- `OPTIONAL MATCH (n)-[:FRIENDS]->(m)` - Left outer join semantics

**Why fourth:**
- Important for optional relationships
- Requires query planner changes
- Complex but valuable

### Phase 5: Parameterized Queries (Priority: LOW)
**Complexity**: Low | **Value**: Low | **Dependencies**: Parser

**Syntax:**
- `MATCH (n) WHERE n.id = $param`

**Why last:**
- Security implications (SQL injection)
- Requires parameter binding infrastructure
- Less critical for initial release

---

## Phase 1: Quantifier Functions - Detailed Plan

### Architecture

Create `PredicateFunction` base class and derived implementations.

### Files to Create/Modify

**New Files:**
1. `include/graph/query/expressions/PredicateFunction.hpp`
2. `src/query/expressions/PredicateFunction.cpp`
3. `include/graph/query/expressions/AllFunction.hpp`
4. `src/query/expressions/AllFunction.cpp`
5. `include/graph/query/expressions/AnyFunction.hpp`
6. `src/query/expressions/AnyFunction.cpp`
7. `include/graph/query/expressions/NoneFunction.hpp`
8. `src/query/expressions/NoneFunction.cpp`
9. `include/graph/query/expressions/SingleFunction.hpp`
10. `src/query/expressions/SingleFunction.cpp`

**Modify:**
- `src/query/expressions/FunctionRegistry.cpp` - Register new functions
- `tests/src/query/expressions/test_PredicateFunctions_Unit.cpp` - Tests

### Implementation Steps

Each function follows this pattern:
1. Parse arguments (variable, list, WHERE expression)
2. Iterate over list elements
3. Evaluate WHERE condition for each element
4. Return boolean result

**Key Design Decisions:**
- Use short-circuit evaluation for performance
- Support empty lists (vacuous truth for all/none, false for any/single)
- Reuse ExpressionEvaluator for WHERE condition evaluation
- Throw clear errors for invalid inputs

### Test Coverage Requirements

- 95%+ line coverage per function
- Test empty list edge cases
- Test all element types (int, string, bool, double, null, nested lists)
- Test nested WHERE conditions
- Test error conditions (wrong types, missing WHERE clause)

---

## Success Criteria

After Phase 1 completion:
- ✅ All 4 quantifier functions working
- ✅ Tests passing: 130+ tests (existing + new)
- ✅ Coverage maintained: 93%+ line, 86%+ branch
- ✅ Documentation updated (UNSUPPORTED_CYPER_FEATURES.md)
- ✅ CLI manual testing successful

---

## Next Steps

Start with Phase 1 implementation using subagent-driven-development approach.
