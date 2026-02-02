# Unsupported Cypher Features - Development Roadmap

This document records the Cypher query language features that are currently not supported by the Metrix graph database, as a reference for future development.

## 1. WHERE Clause Related

### 1.1 IN Operator
**Syntax**: `WHERE n.id IN [1, 2, 3]`
**Status**: ❌ Not Supported
**Workaround**: Use multiple OR conditions
```cypher
-- Not Supported
MATCH (n) WHERE n.id IN [1, 2, 3] RETURN n

-- Workaround
MATCH (n) WHERE n.id = 1 OR n.id = 2 OR n.id = 3 RETURN n
```

### 1.2 WHERE Clause Parsing Issue
**Syntax**: `MATCH(n)WHERE n.prop = 1`
**Status**: ❌ Parse Error
**Issue**: When `MATCH(n)` is directly followed by `WHERE`, the parser incorrectly merges tokens
**Workaround**: Use space separation: `MATCH (n) WHERE ...`

## 2. Expression Evaluation

### 2.1 Expression Evaluation in SET Clause
**Syntax**: `SET n.newVal = n.oldVal * 2`
**Status**: ❌ Not Supported
**Issue**: Expressions are not evaluated, but stored as strings
**Example Result**: `n.newVal` will have value `"n.oldVal*2"` instead of the calculated result

### 2.2 Arithmetic Expressions in RETURN Clause
**Syntax**: `RETURN n.a + n.b AS sum`
**Status**: ⚠️ Partially Supported
**Issue**: Some simple expressions may work, but complex arithmetic operations are not guaranteed

### 2.3 CASE WHEN Expression
**Syntax**: `CASE WHEN n.active THEN 'active' ELSE 'inactive' END`
**Status**: ❌ Not Supported
**Error**: `mismatched input 'WHEN'`

## 3. Aggregate Functions

### 3.1 COUNT
**Syntax**: `RETURN count(n)`
**Status**: ⚠️ May Not Be Supported

### 3.2 SUM
**Syntax**: `RETURN sum(n.val)`
**Status**: ⚠️ May Not Be Supported

### 3.3 AVG
**Syntax**: `RETURN avg(n.val)`
**Status**: ⚠️ May Not Be Supported

### 3.4 MAX/MIN
**Syntax**: `RETURN max(n.val), min(n.val)`
**Status**: ⚠️ May Not Be Supported

### 3.5 collect()
**Syntax**: `RETURN collect(n)`
**Status**: ⚠️ May Not Be Supported

## 4. Special Syntax

### 4.1 DISTINCT
**Syntax**: `RETURN DISTINCT n.val`
**Status**: ⚠️ May Not Be Supported

### 4.2 range() Function
**Syntax**: `UNWIND range(1, 5) AS x`
**Status**: ⚠️ May Not Be Supported

## 5. Label and Property Operations

### 5.1 SET n:Label (Set Label)
**Syntax**: `MATCH (n) SET n:NewLabel`
**Status**: ❌ Not Supported
**Note**: SET can only be used for setting properties, not for adding labels

### 5.2 UNWIND Nested Properties
**Syntax**: `MATCH (n) UNWIND n.arrayProp AS x`
**Status**: ⚠️ May Not Be Supported

## 6. Database Management

### 6.1 Complete Transaction Isolation
**Description**: Data pollution issues between tests
**Status**: ⚠️ Partially Supported
**Recommendation**: Each test should use unique label names or clean up data

## Priority Recommendations

### High Priority
1. **WHERE clause IN operator** - Common feature affecting query convenience
2. **Expression evaluation in SET clause** - Affects data update flexibility
3. **Test data isolation** - Affects test stability

### Medium Priority
4. **Aggregate functions** (COUNT, SUM, AVG, MAX, MIN) - Core analytics functionality
5. **CASE WHEN expression** - Conditional logic support
6. **DISTINCT** - Deduplication functionality

### Low Priority
7. **range() function** - Can be replaced with other approaches
8. **collect() function** - Advanced aggregation functionality

## Development Suggestions

1. **Fix WHERE clause parser**: Ensure handling of no-space scenarios
2. **Implement expression evaluation engine**: Support arithmetic and logical operations
3. **Add aggregate function support**: Start with simple COUNT
4. **Improve test data isolation**: Use UUID or temporary database instances

## Testing Notes

When writing tests for Metrix:
- Use unique label names to avoid conflicts between tests
- Avoid using unsupported syntax features listed above
- For AND/OR conditions in WHERE clause, test carefully as they may have issues
- Use simple comparison operators (`=`, `>`, `<`, `>=`, `<=`) which are well-supported
- Test transaction isolation by using unique labels per test case
