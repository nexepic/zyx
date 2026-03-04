# Cypher Phase 1 Design: List Slicing and Quantifier Functions

**Date**: 2025-01-04
**Author**: Claude
**Status**: Approved
**Updated**: 2026-01-04 - Heterogeneous list support completed

## Overview

Implement list slicing operations and quantifier functions (ALL, ANY, NONE, SINGLE) to provide foundational list manipulation capabilities for Cypher queries.

## Heterogeneous List Support (Completed)

As of 2026-01-04, heterogeneous list support has been fully implemented:

### What Was Implemented
- **PropertyValue extended**: Changed from `std::vector<float>` to `std::vector<PropertyValue>`
- **Mixed-type lists**: Lists can now contain integers, strings, booleans, doubles, null, and nested lists
  ```cpp
  std::vector<PropertyValue> list = {
      PropertyValue(42),
      PropertyValue("hello"),
      PropertyValue(true),
      PropertyValue(3.14)
  };
  ```

### Updated Components
- **PropertyTypes.hpp**: Core list type changed to `std::vector<PropertyValue>`
- **PropertyTypes.cpp**: Recursive size calculation for heterogeneous lists
- **Serializer.hpp**: Recursive serialization/deserialization for heterogeneous lists
- **ExpressionEvaluator.cpp**: List slicing evaluator implemented
- **FunctionRegistry.cpp**: `range()` function returns `std::vector<PropertyValue>` with int64_t elements
- **VectorIndexManager.cpp**: Conversion helpers for heterogeneous list to float vector
- **SystemStateManager.cpp**: Template updates for heterogeneous list storage
- **C API (DatabaseImpl.cpp)**: Support for heterogeneous list conversions

### Test Coverage
- **Unit tests**: `test_HeterogeneousLists_Unit.cpp` - comprehensive heterogeneous list tests
- **Integration tests**: `test_IntegrationHeterogeneousLists.cpp` - end-to-end list operations
- **Serializer tests**: Updated for heterogeneous list serialization
- **All existing tests**: Updated to work with new list type

### Current Limitations
- **List literals in RETURN**: ✅ Fully supported
- **List slicing in WHERE**: ⚠️ Partially supported (works with property access, limited in complex expressions)
- **List slicing in RETURN**: ✅ Fully supported
- **List comprehensions**: ✅ FILTER and EXTRACT fully supported, REDUCE has basic support
- **Storage**: Heterogeneous lists are fully persistent and work with WAL

### API Examples

```cypher
-- Node properties can store heterogeneous lists
CREATE (n:Test {mixed: [42, 'hello', true, 3.14, NULL]})

-- Nested lists are supported
CREATE (n:Test {nested: [[1, 2], [3, 4], [5, 6]]})

-- range() returns int64_t elements
UNWIND range(1, 5) AS x RETURN x
```

## Architecture

### Expression Hierarchy

```
Expression (abstract)
  ├── ListSliceExpression  (new)
  └── FunctionCallExpression
      └── PredicateFunction (new base class)
          ├── AllFunction
          ├── AnyFunction
          ├── NoneFunction
          └── SingleFunction
```

## 1. List Slicing

### Syntax Support

```cypher
list[0]        -- Single element
list[0..2]      -- Range slice [0, 2)
list[0..]       -- From start to end
list[..-1]      -- From start to second-to-last
list[-1]        -- Last element
```

### Implementation

**New Expression Type**: `ListSliceExpression`

```cpp
class ListSliceExpression : public Expression {
    std::unique_ptr<Expression> list_;
    std::unique_ptr<Expression> start_;  // nullable
    std::unique_ptr<Expression> end_;    // nullable
    bool hasRange_;  // true if '..' present

    [[nodiscard]] const Expression* getList() const;
    [[nodiscard]] const Expression* getStart() const;
    [[nodiscard]] const Expression* getEnd() const;
    [[nodiscard]] bool hasRange() const;
};
```

**Grammar Extension**:

```antlr
atom
    : literal
    | parameter
    | K_COUNT LPAREN MULTIPLY RPAREN
    | functionInvocation
    | caseExpression
    | variable
    | LPAREN expression RPAREN
    | listLiteral
    | listSlice  // NEW: list[0..2]
    ;

listSlice
    : atom LBRACK (expression)? (RANGE (expression)?)? RBRACK
    ;
```

**Edge Cases**:
- Out of bounds → Return partial result or empty list
- Negative indices → Count from end
- Empty list → Return empty list
- start > end → Return empty list
- Non-integer index → Throw EvaluationException

## 2. Quantifier Functions

### Syntax

```cypher
all(x IN list WHERE x > 0)
any(x IN list WHERE x > 5)
none(x IN list WHERE x < 0)
single(x IN list WHERE x.status = 'active')
```

### Implementation

**New Base Class**: `PredicateFunction`

```cpp
class PredicateFunction : public Function {
protected:
    // Common logic: iterate list + apply WHERE + return bool
    bool evaluatePredicate(
        const std::vector<PropertyValue>& list,
        const std::string& variable,
        const std::unique_ptr<Expression>& whereExpr,
        EvaluationContext& context
    );
};

class AllFunction : public PredicateFunction {
    // Short-circuit: return false on first non-matching
};

class AnyFunction : public PredicateFunction {
    // Short-circuit: return true on first matching
};

class NoneFunction : public PredicateFunction {
    // Short-circuit: return false on first matching
};

class SingleFunction : PredicateFunction {
    // Must traverse all (count matches)
};
```

**Error Handling**:
- First arg not a list → `EvaluationException`
- Missing WHERE clause → `EvaluationException`
- Variable not in scope → `EvaluationException`

**Performance Optimizations**:
- **ALL**: Short-circuit on false
- **ANY**: Short-circuit on true
- **NONE**: Short-circuit on true
- **SINGLE**: Full traversal required

## File Structure

### New Files

```
include/graph/query/expressions/
  ├── ListSliceExpression.hpp
  └── PredicateFunction.hpp

src/query/expressions/
  ├── ListSliceExpression.cpp
  ├── PredicateFunction.cpp
  └── PredicateFunctions.cpp

tests/src/query/expressions/
  ├── test_ListSlice_Unit.cpp
  └── test_PredicateFunctions_Unit.cpp
```

### Modified Files

```
src/query/parser/cypher/generated/CypherParser.g4
  - Add listSlice rule

src/query/parser/cypher/helpers/internal/ExpressionBuilder.cpp
  - Add buildListSliceExpression() method

src/query/parser/cypher/helpers/ExpressionBuilder.hpp
  - Add buildListSliceExpression() declaration

include/graph/query/expressions/Expression.hpp
  - Forward declare ListSliceExpression

src/query/expressions/ExpressionEvaluator.cpp
  - Add visit(ListSliceExpression*) method

src/query/parser/cypher/generate.sh
  - Regenerate parser after grammar changes
```

## Testing Strategy

### Coverage Targets

- **ListSliceExpression**: 100% line coverage
- **PredicateFunction**: 95%+ line, 90%+ branch coverage
- **No regressions**: All existing tests pass

### Test Cases

**List Slicing**:
```cpp
list[0]          // Single element
list[0..2]       // Range
list[-1]         // Negative index
list[0..]        // Omit end
list[..-1]       // Omit start
list[5..0]       // Empty result
emptyList[0]     // Empty list
list[100]        // Out of bounds
list["invalid"]  // Type error
```

**Quantifier Functions**:
```cpp
all(x IN [] WHERE x > 0)              // Empty → true
all(x IN [1,2,3] WHERE x > 0)         // All pass → true
all(x IN [1,2,0] WHERE x > 0)         // One fails → false
any(x IN [] WHERE x > 0)              // Empty → false
any(x IN [1,2,3] WHERE x > 2)         // One passes → true
none(x IN [1,2] WHERE x > 5)          // None pass → true
none(x IN [1,6] WHERE x > 5)          // One passes → false
single(x IN [1] WHERE x > 0)          // Exactly one → true
single(x IN [1,2,2] WHERE x = 2)      // Multiple → false
```

## Success Criteria

- ✅ Code style compliant (clang-format)
- ✅ No compiler warnings
- ✅ All 121+ existing tests pass
- ✅ New tests: 20+ test cases
- ✅ Coverage meets targets (95%+ line, 90%+ branch)
- ✅ Manual CLI testing successful

## Timeline

**Estimated**: 2-3 days
- Day 1: List slicing implementation + tests
- Day 2: Quantifier functions + tests
- Day 3: Integration testing + documentation

## Next Phase

After Phase 1 completion:
- **Phase 2**: List comprehensions (FILTER, EXTRACT, REDUCE)
- **Phase 3**: Pattern expressions and OPTIONAL MATCH

## Implementation Status (2026-01-04)

### Completed Features
- ✅ Heterogeneous list storage and serialization
- ✅ List literals in RETURN and WHERE clauses
- ✅ List slicing (single index and range)
- ✅ List comprehensions: FILTER and EXTRACT
- ✅ List comprehensions: REDUCE (basic support)
- ✅ Nested lists and mixed-type lists
- ✅ List accessor support for non-variable atoms (e.g., `[1,2,3][0]`)

### Remaining Work
- Full REDUCE function implementation with accumulator syntax parsing
- Enhanced WHERE clause list operations
- Quantifier functions (ALL, ANY, NONE, SINGLE) - not yet started
