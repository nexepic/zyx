# Cypher Complete List Features Design

**Date**: 2026-01-04
**Author**: Claude
**Status**: Approved

## Overview

Implement complete list-related functionality for Cypher queries, including:
1. List literals in RETURN and WHERE clauses
2. List slicing in expressions
3. List comprehension functions (FILTER, EXTRACT, REDUCE)

## Architecture

### System Layered Architecture

```
┌─────────────────────────────────────────────────┐
│           Cypher Query Layer                    │
│  (List Literals, Slicing, Comprehensions)       │
└──────────────────┬──────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────┐
│        Clause Handler Layer                     │
│  ResultClauseHandler  │  ReadingClauseHandler   │
└──────────────────┬──────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────┐
│       Expression Builder Layer                  │
│  ListComprehensionExpression (NEW)              │
│  ListSliceExpression (already exists)           │
└──────────────────┬──────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────┐
│       Expression Evaluator Layer                │
│  Visit() methods for evaluation                 │
└──────────────────┬──────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────┐
│       PropertyValue Layer                       │
│  Heterogeneous lists (already implemented)      │
└─────────────────────────────────────────────────┘
```

## Core Components

### 1. ListComprehensionExpression (NEW)

```cpp
class ListComprehensionExpression : public Expression {
private:
    std::string variable_;                    // Iteration variable name "x"
    std::unique_ptr<Expression> listExpr_;   // List expression
    std::unique_ptr<Expression> whereExpr_;  // WHERE filter (optional)
    std::unique_ptr<Expression> mapExpr_;    // Map expression after | (optional)
    ComprehensionType type_;                 // FILTER/EXTRACT/REDUCE

public:
    enum class ComprehensionType {
        FILTER,    // [x IN list WHERE x > 5]
        EXTRACT,   // [x IN list | x * 2]
        REDUCE     // reduce(total = 0, x IN list | total + x)
    };

    [[nodiscard]] const std::string& getVariable() const;
    [[nodiscard]] const Expression* getListExpr() const;
    [[nodiscard]] const Expression* getWhereExpr() const;  // Can be nullptr
    [[nodiscard]] const Expression* getMapExpr() const;    // Can be nullptr
    [[nodiscard]] ComprehensionType getType() const;
};
```

### 2. ExpressionBuilder Modifications

**Current limitation location:**
```cpp
// src/query/parser/cypher/helpers/internal/ExpressionBuilder.cpp
// Line ~287
throw std::runtime_error("List literals should only be used with IN operator");
```

**Modifications:**
- ✅ Remove hardcoded limitation
- ✅ Add `buildListComprehensionExpression()` method
- ✅ Update `buildListSliceExpression()` for full slicing syntax

### 3. Clause Handler Modifications

**ResultClauseHandler:**
- ✅ Support `RETURN [1, 2, 3] AS list`
- ✅ Support `RETURN [x IN list | x * 2] AS transformed`
- ✅ Support `RETURN n.list[0..2] AS sliced`

**ReadingClauseHandler:**
- ✅ Support `WHERE n.list[0] = 1`
- ✅ Support `WHERE [x IN n.list WHERE x > 5]`

## Data Flow

### List Comprehension Data Flow

```
Input: [x IN range(1, 10) WHERE x % 2 = 0 | x^2]
                    │
                    ▼
┌─────────────────────────────────────┐
│  1. Parse Phase (ListComprehension) │
│     variable: "x"                   │
│     listExpr: range(1, 10)          │
│     whereExpr: x % 2 = 0            │
│     mapExpr: x^2                    │
│     type: EXTRACT                   │
└──────────────────┬──────────────────┘
                   │
                   ▼
┌─────────────────────────────────────┐
│  2. Evaluation Phase                │
│     a. Evaluate listExpr → [1..10]  │
│     b. Iterate each element:        │
│        - Set variable in context    │
│        - Evaluate whereExpr (if any)│
│        - Evaluate mapExpr (if any)  │
│     c. Collect results to new list  │
└──────────────────┬──────────────────┘
                   │
                   ▼
┌─────────────────────────────────────┐
│  3. Error Handling (Mixed Mode)     │
│     - Type mismatch → Exception     │
│     - NULL values → Propagate NULL  │
│     - Empty list → Return []        │
└─────────────────────────────────────┘
```

## Error Handling Strategy (Mixed Mode)

Follows standard Cypher behavior:

| Scenario | Behavior | Example |
|----------|----------|---------|
| **Type mismatch** | Throw `EvaluationException` | `[x IN [1,2] \| x + "text"]` ❌ |
| **Empty list input** | Return empty list `[]` | `[x IN [] \| x * 2] → []` ✅ |
| **WHERE returns NULL** | Skip element | `[x IN [1, null, 3] WHERE x > 0] → [1, 3]` ✅ |
| **MAP returns NULL** | Preserve NULL | `[x IN [1, 2, 3] \| null] → [null, null, null]` ✅ |
| **Index out of bounds** | Return partial or null | `[1, 2, 3][10] → null` ✅ |
| **Negative index** | Count from end | `[1, 2, 3][-1] → 3` ✅ |

## Implementation Details

### File Structure

```
New files:
include/graph/query/expressions/
  └── ListComprehensionExpression.hpp

src/query/expressions/
  └── ListComprehensionExpression.cpp

Modified files:
src/query/parser/cypher/helpers/internal/ExpressionBuilder.cpp
  ├── Remove list literal limitation (~line 287)
  ├── Add buildListComprehensionExpression()
  └── Update buildListSliceExpression()

src/query/parser/cypher/clauses/ResultClauseHandler.cpp
  └── Support list literals and slicing in RETURN

src/query/parser/cypher/clauses/ReadingClauseHandler.cpp
  └── Support list literals and slicing in WHERE

src/query/expressions/ExpressionEvaluator.cpp
  ├── Update visit(ListSliceExpression*)
  └── Add visit(ListComprehensionExpression*)

src/query/parser/cypher/CypherLexer.g4 / CypherParser.g4
  └── Add list comprehension grammar rules
```

### Grammar Extension (ANTLR4)

```antlr
listComprehension
    : LBRACK variable IN expression (WHERE expression)? (PIPE expression)? RBRACK
    ;
```

### Evaluation Pseudocode

```cpp
PropertyValue evaluateListComprehension(ListComprehensionExpression* expr, EvaluationContext& ctx) {
    // 1. Evaluate list expression
    PropertyValue listVal = evaluate(expr->getListExpr(), ctx);
    if (listVal.getType() != PropertyType::LIST) {
        throw EvaluationException("IN requires a list");
    }

    auto& elements = listVal.getList();
    std::vector<PropertyValue> result;

    // 2. Iterate and filter/map
    for (auto& elem : elements) {
        // Set iteration variable
        ctx.setVariable(expr->getVariable(), elem);

        // WHERE filter
        if (expr->getWhereExpr()) {
            PropertyValue whereVal = evaluate(expr->getWhereExpr(), ctx);
            if (whereVal.getType() != PropertyType::BOOLEAN) {
                throw EvaluationException("WHERE must return boolean");
            }
            if (!std::get<bool>(whereVal.getVariant())) {
                continue;  // Skip elements not satisfying condition
            }
        }

        // MAP mapping
        if (expr->getMapExpr()) {
            result.push_back(evaluate(expr->getMapExpr(), ctx));
        } else {
            result.push_back(elem);  // FILTER mode, preserve original element
        }
    }

    return PropertyValue(result);
}
```

### Performance Optimizations

- **Short-circuit evaluation**: Skip immediately when WHERE condition is false
- **Pre-allocate memory**: Pre-allocate result vector based on input list size
- **Avoid copying**: Use reference passing for large lists

## Implementation Phases

### Phase 1: Remove Limitations + Core Support (1-2 days)

```
Tasks:
1. Remove ExpressionBuilder hard-coded limitation
2. Update ResultClauseHandler for RETURN clause list support
3. Update ReadingClauseHandler for WHERE clause list support
4. Add tests for list literals in RETURN/WHERE
```

### Phase 2: List Comprehension Functions (1-2 days)

```
Tasks:
1. Implement FILTER function
2. Implement EXTRACT function
3. Implement REDUCE function
4. Add comprehensive tests for comprehensions
```

### Phase 3: Integration Testing and Documentation (0.5 day)

```
Tasks:
1. End-to-end testing
2. Update documentation
3. Verify all tests pass
```

## Testing Strategy

### Coverage Targets

- **ListComprehensionExpression**: 100% line coverage
- **ExpressionBuilder modifications**: 95%+ line, 90%+ branch coverage
- **Clause Handler updates**: 90%+ line coverage
- **No regressions**: All existing tests pass

### Test Cases

**List Literals:**
```cypher
RETURN [1, 2, 3] AS list
RETURN [1, 'text', true] AS mixed
RETURN [[1, 2], [3, 4]] AS nested
```

**List Slicing:**
```cypher
RETURN [1, 2, 3, 4, 5][1..3] AS sliced
RETURN [1, 2, 3][0] AS first
RETURN [1, 2, 3][-1] AS last
```

**List Comprehensions:**
```cypher
RETURN [x IN [1, 2, 3] | x * 2] AS doubled
RETURN [x IN range(1, 10) WHERE x % 2 = 0 | x] AS evens
RETURN reduce(total = 0, x IN [1, 2, 3] | total + x) AS sum
```

**Error Cases:**
```cypher
-- Type mismatch
RETURN [x IN [1, 2] | x + "text"]

-- Empty list
RETURN [x IN [] | x * 2]

-- NULL handling
RETURN [x IN [1, null, 3] | x * 2]
```

## Success Criteria

- ✅ Code style compliant (clang-format)
- ✅ No compiler warnings
- ✅ All 123+ existing tests pass
- ✅ New tests: 40+ test cases
- ✅ Coverage meets targets (95%+ line, 90%+ branch)
- ✅ Manual CLI testing successful

## Timeline

**Estimated**: 3-4 days
- Day 1-2: Core support (literals, slicing)
- Day 3-4: Comprehension functions + testing
