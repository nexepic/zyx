# Cypher Feature Support - ZYX Graph Database

This document tracks Cypher support in ZYX based on current parser/execution tests and implementation.

Last reviewed: **2026-04-10**

## 1. Supported Features

### Clauses

- `MATCH` / `OPTIONAL MATCH`
- `CREATE` (including chained patterns and `CREATE ... RETURN`)
- `MERGE` with `ON CREATE SET` / `ON MATCH SET`
- `SET` (property assignment, label assignment, `SET +=` map merge)
- `REMOVE` (properties and labels)
- `DELETE` / `DETACH DELETE`
- `RETURN`, `WITH`
- `DISTINCT`, `ORDER BY`, `SKIP`, `LIMIT`
- `UNION` / `UNION ALL`
- `UNWIND`
- `FOREACH`
- `CALL procedure(...) [YIELD ...]`
- `CALL { subquery }` and `CALL { ... } IN TRANSACTIONS [OF n ROWS]`
- `LOAD CSV` / `LOAD CSV WITH HEADERS` with optional `FIELDTERMINATOR`
- `EXPLAIN` / `PROFILE`

### Pattern Matching

- Directed and undirected patterns: `-[]->`, `<-[]-`, `-[]-`
- Variable-length relationships: `*`, `*N`, `*N..M`, `*N..`, `*..M`
- Named paths
- Multiple relationship types in one pattern: `-[:TYPE1|TYPE2]->`
- Multi-label nodes: `(n:LabelA:LabelB)`

### Expressions and Data Types

- Arithmetic: `+ - * / % ^`
- Comparison: `= <> != < <= > >=`, `IN`, `BETWEEN`
- Boolean: `AND OR XOR NOT` (3-valued logic with `NULL`)
- String matching operators: `STARTS WITH`, `ENDS WITH`, `CONTAINS`, `=~`
- `IS NULL` / `IS NOT NULL`
- `CASE` (simple and searched)
- List literals, list slicing, heterogeneous lists
- Map literals and map projection (`n {.prop, key: expr, .*}`)
- Parameters (`$param`)

### Functions

- Aggregation: `count`, `sum`, `avg`, `min`, `max`, `collect` (including `DISTINCT`)
- String: `toString`, `upper`, `lower`, `trim`, `lTrim`, `rTrim`, `left`, `right`, `substring`, `replace`, `split`, `reverse`, `length`
- Math: `abs`, `ceil`, `floor`, `round`, `sqrt`, `sign`
- List/utility: `size`, `range`, `head`, `tail`, `last`, `coalesce`
- Type conversion: `toInteger`, `toFloat`, `toBoolean`
- Path: `nodes(path)`, `relationships(path)`, `length(path)`
- Entity introspection: `id`, `labels`, `type`, `keys`, `properties`
- Quantifiers: `all`, `any`, `none`, `single`
- Misc: `timestamp`, `randomUUID`, `exists((pattern))`, `reduce(...)`

### Schema and Administration

- `CREATE INDEX` (pattern syntax and legacy label syntax)
- `DROP INDEX` (by name and legacy label/property form)
- `SHOW INDEXES`
- `CREATE VECTOR INDEX ... OPTIONS {dimension|dim, metric}`
- `CREATE CONSTRAINT` / `DROP CONSTRAINT` / `SHOW CONSTRAINT`
  - Node constraints: `UNIQUE`, `NOT NULL`, `IS ::TYPE`, `NODE KEY`
  - Edge constraints: `UNIQUE`, `NOT NULL`, `IS ::TYPE`

### Built-in Procedures

- DBMS: `dbms.listConfig`, `dbms.getConfig`, `dbms.setConfig`, `dbms.showStats`, `dbms.resetStats`
- Algorithms: `algo.shortestPath`
- Vector: `db.index.vector.queryNodes`, `db.index.vector.train`
- GDS projection: `gds.graph.project`, `gds.graph.drop`
- GDS algorithms: `gds.pageRank.stream`, `gds.wcc.stream`, `gds.betweenness.stream`, `gds.closeness.stream`, `gds.shortestPath.dijkstra.stream`

## 2. Not Yet Supported (or Not Verified as Supported)

- Temporal value types and temporal functions (`date`, `time`, `datetime`, `duration`)
- Spatial value types/functions (`point`, `distance`, ...)
- Extended math/trigonometric/log functions (`log`, `ln`, `exp`, `sin`, `cos`, `tan`, `pi`, `e`, ...)
- Statistical aggregates (`percentileDisc`, `percentileCont`, `stDev`, `stDevP`, ...)
- `USING INDEX` query hints
- `PERIODIC COMMIT`
- Full Cypher `EXISTS { ... }` subquery expression form in `WHERE`
- APOC procedure ecosystem

## 3. Notes

- Cypher compatibility is intentionally pragmatic, not a full Neo4j clone.
- If this document conflicts with tests, tests and implementation are the source of truth.
- When adding/removing features, update this file and the bilingual user guide together.
