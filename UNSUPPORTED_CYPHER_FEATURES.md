# Cypher Feature Support - ZYX Graph Database

This document tracks the Cypher query language feature support status in the ZYX graph database.

## 1. Expression Evaluation

### 1.1 Expression Evaluation in SET Clause
**Syntax**: `SET n.newVal = n.oldVal * 2`
**Status**: ✅ Fully Supported

### 1.2 String Concatenation with +
**Syntax**: `RETURN n.firstName + ' ' + n.lastName`
**Status**: ✅ Fully Supported

### 1.3 Arithmetic Expressions
**Status**: ✅ Fully Supported
**Operations**: `+`, `-`, `*`, `/`, `%`, `^`, unary `-`, `NOT`

### 1.4 CASE WHEN Expression
**Status**: ✅ Fully Supported (both simple and searched CASE forms)

### 1.5 SET += (Map Merge)
**Syntax**: `SET n += {newProp: 'value', otherProp: 42}`
**Status**: ✅ Supported
**Note**: Merges map literal properties into existing node/relationship properties.

## 2. Scalar Functions

### 2.1 Type Conversion
| Function | Status |
|----------|--------|
| `toString(value)` | ✅ |
| `toInteger(value)` | ✅ |
| `toFloat(value)` | ✅ |
| `toBoolean(value)` | ✅ |

### 2.2 String Functions
| Function | Status |
|----------|--------|
| `upper(text)` | ✅ |
| `lower(text)` | ✅ |
| `trim(text)` | ✅ |
| `lTrim(text)` | ✅ |
| `rTrim(text)` | ✅ |
| `left(text, n)` | ✅ |
| `right(text, n)` | ✅ |
| `substring(text, start, length?)` | ✅ |
| `replace(text, search, replace)` | ✅ |
| `split(text, delimiter)` | ✅ |
| `reverse(value)` | ✅ |
| `length(value)` | ✅ |
| `startsWith(text, prefix)` | ✅ |
| `endsWith(text, suffix)` | ✅ |
| `contains(text, substring)` | ✅ |

### 2.3 Math Functions
| Function | Status |
|----------|--------|
| `abs(value)` | ✅ |
| `ceil(value)` | ✅ |
| `floor(value)` | ✅ |
| `round(value)` | ✅ |
| `sqrt(value)` | ✅ |
| `sign(value)` | ✅ |

### 2.4 List Functions
| Function | Status |
|----------|--------|
| `size(list)` | ✅ |
| `head(list)` | ✅ |
| `tail(list)` | ✅ |
| `last(list)` | ✅ |
| `range(start, end, step?)` | ✅ |
| `coalesce(v1, v2, ...)` | ✅ |

### 2.5 Entity Introspection
| Function | Status |
|----------|--------|
| `id(entity)` | ✅ |
| `labels(node)` | ✅ |
| `type(relationship)` | ✅ |
| `keys(entity)` | ✅ |
| `properties(entity)` | ✅ |

### 2.6 Utility Functions
| Function | Status |
|----------|--------|
| `timestamp()` | ✅ |
| `randomUUID()` | ✅ |

### 2.7 Quantifier Functions
| Function | Status |
|----------|--------|
| `all(x IN list WHERE pred)` | ✅ |
| `any(x IN list WHERE pred)` | ✅ |
| `none(x IN list WHERE pred)` | ✅ |
| `single(x IN list WHERE pred)` | ✅ |

## 3. Aggregate Functions

All aggregate functions are ✅ Supported: `count()`, `sum()`, `avg()`, `max()`, `min()`, `collect()`

## 4. Query Features

| Feature | Status |
|---------|--------|
| WHERE with IN operator | ✅ |
| UNION / UNION ALL | ✅ |
| DISTINCT | ✅ |
| List literals | ✅ |
| List slicing | ✅ |
| List comprehensions (FILTER/EXTRACT) | ✅ |
| Heterogeneous lists | ✅ |
| SET n:Label | ✅ |

## 5. Recently Implemented Features

### 5.1 FOREACH Statement
**Syntax**: `FOREACH (x IN list | SET x.prop = value)`
**Status**: ✅ Supported
**Note**: Iterates over a list and executes write-only operations (CREATE, SET, DELETE, REMOVE, MERGE) for each element. Does not export variables to the outer scope.

### 5.2 CALL { subquery }
**Syntax**: `CALL { MATCH (m) RETURN m } RETURN m`
**Status**: ✅ Supported
**Note**: Executes a subquery per input row with variable scope isolation. Supports `IN TRANSACTIONS OF n ROWS` for batched execution.

### 5.3 LOAD CSV
**Syntax**: `LOAD CSV WITH HEADERS FROM 'file:///path/to/file.csv' AS row`
**Status**: ✅ Supported
**Note**: Streaming RFC 4180-compliant CSV reader. Supports `WITH HEADERS` (map rows) and without headers (list rows). Custom `FIELDTERMINATOR` supported.

### 5.4 EXISTS Pattern Expressions
**Syntax**: `WHERE EXISTS { (n)-[:KNOWS]->(m) }` or `WHERE exists((n)-[:KNOWS]->(m))`
**Status**: ✅ Supported
**Note**: Evaluates pattern existence using graph traversal within expression evaluator.

### 5.2 Pattern Comprehensions
**Syntax**: `[(n)-[:KNOWS]->(m) | m.name]`
**Status**: ✅ Supported
**Note**: Traverses graph patterns and collects projected expressions into a list.

### 5.3 REDUCE Function
**Syntax**: `reduce(total = 0, x IN [1, 2, 3] | total + x)`
**Status**: ✅ Supported
**Note**: Accumulator-based list reduction with custom body expression.

### 5.4 Multi-Label Support
**Syntax**: `(n:Person:Employee)`
**Status**: ✅ Supported
**Note**: Nodes support up to 6 labels. MATCH uses AND semantics (node must have ALL specified labels). SET appends labels, REMOVE removes individual labels.

### 5.5 UNWIND Nested Properties
**Syntax**: `MATCH (n) UNWIND n.arrayProp AS x`
**Status**: ✅ Supported
**Note**: Expression-based UNWIND evaluates property access at runtime.

## Testing Notes

When writing tests for ZYX:
- Use unique label names to avoid conflicts between tests
- Avoid using features listed in Section 5
- Test transaction isolation by using unique labels per test case
