# Cypher Feature Support - ZYX Graph Database

This document tracks the Cypher query language feature support status in the ZYX graph database.

## 1. Supported Cypher Features

### Clauses

- `MATCH` / `OPTIONAL MATCH`
- `CREATE`
- `MERGE` with `ON CREATE SET` / `ON MATCH SET`
- `SET` (property assignment, label assignment with `SET n:Label`)
- `DELETE`
- `REMOVE`
- `RETURN`
- `WITH`
- `ORDER BY`, `SKIP`, `LIMIT`
- `DISTINCT`
- `UNION` / `UNION ALL`
- `UNWIND` (including nested property expressions)
- `CALL` procedures
- `CALL { subquery }` (with variable scope isolation, supports `IN TRANSACTIONS OF n ROWS`)
- `FOREACH (x IN list | ...)`
- `LOAD CSV FROM 'file:///path' AS row` (streaming RFC 4180-compliant, custom `FIELDTERMINATOR`)
- `EXPLAIN` / `PROFILE`

### Expressions and Operators

- Arithmetic: `+`, `-`, `*`, `/`, `%`, `^`, unary `-`
- Comparison: `=`, `<>`, `<`, `>`, `<=`, `>=`
- Boolean: `AND`, `OR`, `XOR`, `NOT`
- String matching: `STARTS WITH`, `ENDS WITH`, `CONTAINS`, `=~` (regex)
- `IN` operator (static lists and dynamic variables)
- `IS NULL` / `IS NOT NULL`
- String concatenation with `+`
- `CASE WHEN ... THEN ... ELSE ... END` (simple and searched forms)
- List literals, list slicing, heterogeneous lists
- Map literals: `{key: value}`
- Parameters: `$param`

### Pattern Features

- Named paths: `p = (a)-[r]->(b)`
- Variable-length paths: `(a)-[*1..3]->(b)`
- `shortestPath()` / `allShortestPaths()`
- Multi-label patterns: `(n:Person:Employee)` (up to 6 labels, AND semantics)

### Comprehensions

- List comprehensions: `[x IN list WHERE cond | expr]`
- Pattern comprehensions: `[(a)-->(b) | b.prop]`
- `reduce(acc = init, x IN list | expr)`
- `exists()` subquery expression

### Map Projections

- `n {.prop1, key: expr, .*}`

### Aggregation Functions

| Function | Notes |
|----------|-------|
| `count()` | Including `count(DISTINCT x)` |
| `sum()` | |
| `avg()` | |
| `min()` | |
| `max()` | |
| `collect()` | Including `collect(DISTINCT x)` |

### String Functions

| Function |
|----------|
| `toString(value)` |
| `toUpper(text)` / `toLower(text)` |
| `trim(text)` / `lTrim(text)` / `rTrim(text)` |
| `left(text, n)` / `right(text, n)` |
| `substring(text, start, length?)` |
| `replace(text, search, replace)` |
| `split(text, delimiter)` |
| `reverse(text)` |
| `length(text)` / `size(text)` |
| `startsWith(text, prefix)` / `endsWith(text, suffix)` / `contains(text, sub)` |

### Math Functions

| Function |
|----------|
| `abs(value)` |
| `ceil(value)` / `floor(value)` / `round(value)` |
| `sqrt(value)` |
| `sign(value)` |

### List Functions

| Function |
|----------|
| `range(start, end, step?)` |
| `head(list)` / `tail(list)` / `last(list)` |
| `keys(entity)` |
| `reverse(list)` |
| `size(list)` |

### Type Conversion Functions

| Function |
|----------|
| `toInteger(value)` |
| `toFloat(value)` |
| `toBoolean(value)` |

### Path Functions

| Function |
|----------|
| `nodes(path)` |
| `relationships(path)` |
| `length(path)` |

### Utility Functions

| Function |
|----------|
| `coalesce(v1, v2, ...)` |
| `id(entity)` |
| `labels(node)` |
| `type(relationship)` |
| `properties(entity)` |
| `timestamp()` |
| `randomUUID()` |

### Quantifier Functions

| Function |
|----------|
| `all(x IN list WHERE pred)` |
| `any(x IN list WHERE pred)` |
| `none(x IN list WHERE pred)` |
| `single(x IN list WHERE pred)` |

### Schema and Administration

- `CREATE INDEX` / `DROP INDEX` / `SHOW INDEXES`
- Vector indexes
- `CREATE CONSTRAINT` / `DROP CONSTRAINT` / `SHOW CONSTRAINTS`
- `BEGIN` / `COMMIT` / `ROLLBACK` (transactions)

## 2. Not Yet Supported

- Temporal types (`date`, `time`, `datetime`, `duration`) and related functions
- Spatial types (`point`) and spatial functions (`distance()`, etc.)
- Extended math functions (`log()`, `e()`, `pi()`, `sin()`, `cos()`, `tan()`, etc.)
- Statistical aggregation (`percentileDisc()`, `percentileCont()`, `stDev()`, `stDevP()`)
- Undirected relationship patterns: `-[]-`
- Multiple relationship types in pattern: `-[:TYPE1|TYPE2]->`
- `DETACH DELETE`
- `REMOVE` labels from nodes
- `SET +=` for map merge
- `CREATE ... RETURN` (returning created entities)
- `WITH HEADERS` in `LOAD CSV`
- `PERIODIC COMMIT`
- Subquery expressions in `WHERE`
- Pattern expressions used as existence checks
