# Unsupported Cypher Features - Development Roadmap

This document records the Cypher query language features that are currently not supported by the Metrix graph database, as a reference for future development.

## 1. WHERE Clause Related

### 1.1 IN Operator
**Syntax**: `WHERE n.id IN [1, 2, 3]`
**Status**: ✅ Fully Supported
**Note**: Can be used to check if a property value matches any value in a list.

```cypher
-- Example: Find nodes with specific IDs
MATCH (n) WHERE n.id IN [1, 2, 3] RETURN n

-- Example: Find nodes with specific names
MATCH (n:Person) WHERE n.name IN ['Alice', 'Bob'] RETURN n

-- Example: Empty result set
MATCH (n:Person) WHERE n.age IN [40, 50, 60] RETURN n
```

### 1.2 WHERE Clause Parsing Issue
**Syntax**: `MATCH(n)WHERE n.prop = 1`
**Status**: ✅ Works with proper spacing
**Note**: The Cypher grammar requires whitespace between keywords. Use `MATCH (n) WHERE ...` for correct parsing.

```cypher
-- Correct syntax with spacing
MATCH (n) WHERE n.prop = 1 RETURN n

-- Avoid syntax without spacing
MATCH(n)WHERE n.prop = 1  -- May have parsing issues
```

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
**Status**: ✅ Fully Supported
**Note**: Both simple CASE and searched CASE forms are supported.

```cypher
-- Simple CASE expression
MATCH (n:Person)
RETURN CASE n.status
  WHEN 'active' THEN 1
  WHEN 'inactive' THEN 0
  ELSE -1
END

-- Searched CASE expression
MATCH (n:Person)
RETURN CASE
  WHEN n.age < 18 THEN 'minor'
  WHEN n.age < 65 THEN 'adult'
  ELSE 'senior'
END

-- CASE in SET clause
MATCH (n:Person)
SET n.category = CASE
  WHEN n.age < 18 THEN 'minor'
  ELSE 'adult'
END
```

## 3. Aggregate Functions

### 3.1 COUNT
**Syntax**: `RETURN count(n)`
**Status**: ✅ Fully Supported

### 3.2 SUM
**Syntax**: `RETURN sum(n.val)`
**Status**: ✅ Fully Supported

### 3.3 AVG
**Syntax**: `RETURN avg(n.val)`
**Status**: ✅ Fully Supported

### 3.4 MAX/MIN
**Syntax**: `RETURN max(n.val), min(n.val)`
**Status**: ✅ Fully Supported

### 3.5 collect()
**Syntax**: `RETURN collect(n)`
**Status**: ✅ Fully Supported

## 4. Query Combination

### 4.1 UNION / UNION ALL
**Syntax**: `MATCH (n:A) RETURN n.name UNION MATCH (n:B) RETURN n.name`
**Status**: ✅ Fully Supported
**Note**: UNION removes duplicates, UNION ALL preserves duplicates.

```cypher
-- UNION (remove duplicates)
MATCH (n:Manager) RETURN n.name AS name
UNION
MATCH (n:Employee) RETURN n.name AS name

-- UNION ALL (preserve duplicates)
MATCH (n) RETURN n.val
UNION ALL
MATCH (n) RETURN n.val

-- Multiple UNIONs
MATCH (n:A) RETURN n.name
UNION
MATCH (n:B) RETURN n.name
UNION ALL
MATCH (n:C) RETURN n.name
```

## 5. Special Syntax

### 5.1 DISTINCT
**Syntax**: `RETURN DISTINCT n.val`
**Status**: ✅ Supported
**Note**: Eliminates duplicate rows from query results.

```cypher
-- Example: Return unique countries
MATCH (n:Person) RETURN DISTINCT n.country

-- Example: Return unique status values
MATCH (n:Person) WHERE n.active = true RETURN DISTINCT n.status
```

### 5.2 range() Function
**Syntax**: `UNWIND range(1, 5) AS x`
**Status**: ✅ Fully Supported
**Note**: Generates a list of integers from start (inclusive) to end (exclusive).

```cypher
-- Basic range
UNWIND range(0, 5) AS x RETURN x
-- Returns: [0, 1, 2, 3, 4]

-- Range with step
UNWIND range(0, 10, 2) AS x RETURN x
-- Returns: [0, 2, 4, 6, 8]

-- Negative step
UNWIND range(5, 0, -1) AS x RETURN x
-- Returns: [5, 4, 3, 2, 1]

-- Create nodes with sequential IDs
CREATE (n:Test)
UNWIND range(1, 4) AS i
SET n.seq = i
```

## 6. Label and Property Operations

### 5.1 SET n:Label (Set Label)
**Syntax**: `MATCH (n) SET n:NewLabel`
**Status**: ✅ Supported
**Note**: SET can set both properties and labels. Setting a label replaces the existing label on the node.

```cypher
-- Example: Set label on a node
MATCH (n {name: 'Alice'}) SET n:Person

-- Example: Replace a label
MATCH (n:OldLabel) SET n:NewLabel
```

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
1. **Expression evaluation in SET clause** - Affects data update flexibility
2. **Test data isolation** - Affects test stability

### Medium Priority
3. **UNWIND nested properties** - Advanced data manipulation
4. **UNION optimization** - Current implementation buffers results in memory

### Low Priority
5. **collect() function** - ✅ Already supported
6. **range() function** - ✅ Already supported
7. **CASE WHEN expression** - ✅ Already supported

## Development Suggestions

1. **Fix expression evaluation in SET clause**: Implement full expression evaluation
2. **Improve test data isolation**: Use UUID or temporary database instances
3. **Optimize UNION for large datasets**: Current implementation may use significant memory for deduplication

## Testing Notes

When writing tests for Metrix:
- Use unique label names to avoid conflicts between tests
- Avoid using unsupported syntax features listed above
- For AND/OR conditions in WHERE clause, test carefully as they may have issues
- Use simple comparison operators (`=`, `>`, `<`, `>=`, `<=`) which are well-supported
- Test transaction isolation by using unique labels per test case
