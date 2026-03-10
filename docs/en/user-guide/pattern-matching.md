# Pattern Matching

Pattern matching is the heart of Cypher, allowing you to express complex graph patterns and traverse relationships efficiently. This guide dives deep into pattern matching techniques.

## Pattern Fundamentals

### Node Patterns

Nodes are the foundation of graph patterns:

```cypher
-- Anonymous node
MATCH () RETURN 1

-- Named node
MATCH (n) RETURN n

-- Node with label
MATCH (n:Person) RETURN n

-- Node with multiple labels
MATCH (n:Person:Developer) RETURN n

-- Node with properties
MATCH (n:Person {name: 'Alice'}) RETURN n

-- Node with multiple properties
MATCH (n:Person {name: 'Alice', age: 30}) RETURN n
```

### Relationship Patterns

Relationships connect nodes in patterns:

```cypher
-- Directed relationship
MATCH (a)-[:KNOWS]->(b) RETURN a, b

-- Undirected relationship
MATCH (a)-[:KNOWS]-(b) RETURN a, b

-- Left-directed
MATCH (a)<-[:KNOWS]-(b) RETURN a, b

-- Named relationship
MATCH (a)-[r:KNOWS]->(b) RETURN a, r, b

-- Relationship with properties
MATCH (a)-[r:KNOWS {since: 2020}]->(b) RETURN a, r, b

-- Relationship without type
MATCH (a)-[r]->(b) RETURN a, r, b
```

## Complex Patterns

### Path Patterns

```cypher
-- Simple path
MATCH p = (a)-[:KNOWS]->(b) RETURN p

-- Multi-hop path
MATCH p = (a)-[:KNOWS]->(b)-[:KNOWS]->(c) RETURN p

-- Accessing path elements
MATCH p = (a)-[:KNOWS]->(b)-[:KNOWS]->(c)
RETURN nodes(p) AS nodes, relationships(p) AS rels

-- Path length
MATCH p = (a)-[:KNOWS*2..3]->(b)
RETURN length(p) AS hops
```

### Variable Length Patterns

```cypher
-- Any length path
MATCH (a:Person {name: 'Alice'})-[:KNOWS*]->(b) RETURN b

-- Minimum length
MATCH (a:Person {name: 'Alice'})-[:KNOWS*2..]->(b) RETURN b

-- Maximum length
MATCH (a:Person {name: 'Alice'})-[:KNOWS*..3]->(b) RETURN b

-- Exact range
MATCH (a:Person {name: 'Alice'})-[:KNOWS*2..3]->(b) RETURN b

-- Unbounded (be careful!)
MATCH (a:Person {name: 'Alice'})-[:KNOWS*]-(b) RETURN b
```

### Multi-Relationship Patterns

```cypher
-- Relationships from same node
MATCH (a:Person {name: 'Alice'})-[:KNOWS]->(b),
      (a)-[:WORKS_WITH]->(c)
RETURN b, c

-- Multiple relationship types
MATCH (a)-[:KNOWS|WORKS_WITH]->(b) RETURN a, b

-- Pattern with specific relationship
MATCH (a)-[r:KNOWS|WORKS_WITH]->(b)
WHERE r.since > 2020
RETURN a, r, b
```

## Pattern Compositions

### Conjunction Patterns

```cypher
-- Multiple patterns in same MATCH
MATCH (a:Person)-[:KNOWS]->(b:Person),
      (b)-[:WORKS_AT]->(c:Company)
RETURN a, b, c

-- Filtered conjunction
MATCH (a:Person)-[:KNOWS]->(b:Person)
WHERE (b)-[:WORKS_AT]->(c:Company {name: 'TechCorp'})
RETURN a, b
```

### Disjunction Patterns

```cypher
-- Using OR in patterns
MATCH (a:Person)
WHERE (a)-[:KNOWS]->(:Person) OR (a)-[:WORKS_WITH]->(:Person)
RETURN a

-- Multiple relationship paths
MATCH (a:Person)-[:KNOWS|WORKS_WITH]->(b:Person)
RETURN a, b
```

### Negation Patterns

```cypher
-- Pattern doesn't exist
MATCH (n:Person)
WHERE NOT (n)-[:KNOWS]->()
RETURN n

-- Exclude specific pattern
MATCH (n:Person)
WHERE NOT (n)-[:KNOWS]->(:Person {name: 'Bob'})
RETURN n

-- Complex negation
MATCH (n:Person)
WHERE NOT (n)-[:KNOWS]->(:Person)-[:WORKS_AT]->(:Company)
RETURN n
```

## Advanced Pattern Techniques

### Pattern Comprehensions

```cypher
-- Collect nodes from pattern
MATCH (n:Person {name: 'Alice'})
RETURN [(n)-[:KNOWS]->(b) | b.name] AS friend_names

-- With filter
MATCH (n:Person {name: 'Alice'})
RETURN [(n)-[:KNOWS]->(b) WHERE b.age > 30 | b.name] AS older_friends

-- With transformation
MATCH (n:Person {name: 'Alice'})
RETURN [(n)-[:KNOWS]->(b) | {name: b.name, age: b.age}] AS friend_details

-- Nested pattern comprehension
MATCH (n:Person {name: 'Alice'})
RETURN [(n)-[:KNOWS]->(b) |
  {name: b.name, friends: [(b)-[:KNOWS]->(c) | c.name]}
] AS friend_network
```

### Pattern Existence

```cypher
-- Check if pattern exists
MATCH (n:Person)
WHERE EXISTS((n)-[:KNOWS]->(:Person))
RETURN n

-- Count relationships
MATCH (n:Person)
WHERE size((n)-[:KNOWS]->()) > 5
RETURN n

-- Multiple pattern checks
MATCH (n:Person)
WHERE EXISTS((n)-[:KNOWS]->())
  AND EXISTS((n)-[:WORKS_AT]->())
RETURN n
```

### Optional Patterns

```cypher
-- Optional relationship
MATCH (n:Person)
OPTIONAL MATCH (n)-[:WORKS_AT]->(c:Company)
RETURN n.name, coalesce(c.name, 'Unemployed') AS company

-- Multiple optional patterns
MATCH (n:Person)
OPTIONAL MATCH (n)-[:WORKS_AT]->(c:Company)
OPTIONAL MATCH (n)-[:LIVES_IN]->(l:Location)
RETURN n.name, c.name, l.city

-- Optional with filter
MATCH (n:Person {name: 'Alice'})
OPTIONAL MATCH (n)-[:KNOWS]->(b:Person)
WHERE b.age > 30
RETURN b
```

## Special Pattern Patterns

### Diamond Patterns

```cypher
-- Diamond: multiple paths to same node
MATCH (a:Person {name: 'Alice'})-[:KNOWS]->(b:Person),
      (a)-[:KNOWS]->(c:Person),
      (b)-[:KNOWS]->(d:Person),
      (c)-[:KNOWS]->(d)
RETURN a, b, c, d

-- Diamond detection
MATCH (a:Person)-[:KNOWS]->(b:Person),
      (a)-[:KNOWS]->(c:Person),
      (b)-[:KNOWS]->(d:Person),
      (c)-[:KNOWS]->(d)
WHERE b <> c
RETURN a, d
```

### Cycle Detection

```cypher
-- Detect cycles
MATCH (a:Person)-[:KNOWS*]->(a)
RETURN DISTINCT a

-- Cycle of specific length
MATCH (a:Person)-[:KNOWS*3]->(a)
RETURN a

-- Shortest cycle
MATCH (a:Person)-[:KNOWS*]->(a)
WITH a, length([p in (a)-[:KNOWS*]->(a) | p][0]) AS cycle_length
RETURN a.name, cycle_length
ORDER BY cycle_length
LIMIT 10
```

### Bipartite Patterns

```cypher
-- Match two node types connected
MATCH (p:Person)-[:PURCHASED]->(p2:Product)
RETURN p, p2

-- Find nodes connected to two different sets
MATCH (n:Person)
WHERE (n)-[:KNOWS]->(:Person {name: 'Alice'})
  AND (n)-[:KNOWS]->(:Person {name: 'Bob'})
RETURN n
```

## Real-World Patterns

### Social Network Patterns

```cypher
-- Mutual friends
MATCH (a:Person {name: 'Alice'})-[:KNOWS]->(common)<-[:KNOWS]-(b:Person {name: 'Bob'})
RETURN common.name AS mutual_friend

-- Friends of friends (not direct friends)
MATCH (me:Person {name: 'Alice'})-[:KNOWS]->(friend)-[:KNOWS]->(fof)
WHERE NOT (me)-[:KNOWS]->(fof) AND fof <> me
RETURN DISTINCT fof.name

-- Influence network (2-hop)
MATCH (a:Person {name: 'Alice'})-[:KNOWS*1..2]->(influenced)
RETURN DISTINCT influenced.name
```

### Recommendation Patterns

```cypher
-- Collaborative filtering
MATCH (me:Person {name: 'Alice'})-[:LIKES]->(item:Product)<-[:LIKES]-(other:Person)
MATCH (other)-[:LIKES]->(rec:Product)
WHERE NOT (me)-[:LIKES]->(rec)
RETURN rec.name, COUNT(*) AS score
ORDER BY score DESC
LIMIT 10

-- Similar users
MATCH (me:Person {name: 'Alice'})-[:HAS_INTEREST]->(i:Interest)<-[:HAS_INTEREST]-(other:Person)
WHERE me <> other
WITH other, COUNT(i) AS common_interests
ORDER BY common_interests DESC
RETURN other.name, common_interests
LIMIT 10
```

### Hierarchical Patterns

```cypher
-- Organization hierarchy
MATCH (e:Employee)-[:WORKS_IN]->(d:Department)-[:PART_OF]->(c:Company)
RETURN e.name, d.name, c.name

-- Management chain
MATCH p = (e:Employee {name: 'John'})-[:REPORTS_TO*]->(ceo:Person)
WHERE NOT (ceo)-[:REPORTS_TO]->()
RETURN [node in nodes(p) | node.name] AS management_chain

-- Find all subordinates
MATCH (manager:Person {name: 'Alice'})-[:MANAGES*]->(subordinate)
RETURN DISTINCT subordinate.name
```

### Temporal Patterns

```cypher
-- Time-based relationships
MATCH (p:Person)-[r:MET {date: date('2024-01-15')}]->(other)
RETURN p, other

-- Relationships in time range
MATCH (p:Person)-[r:KNOWS]->(other)
WHERE r.since >= 2020 AND r.since <= 2024
RETURN p.name, other.name, r.since

-- Latest relationship
MATCH (a:Person)-[r:KNOWS]->(b:Person)
WITH a, b, r
ORDER BY r.since DESC
RETURN a.name, b.name, r.since
```

## Pattern Performance

### Efficient Patterns

```cypher
-- GOOD: Start with specific node
MATCH (n:Person {name: 'Alice'})-[:KNOWS]->(b)
RETURN b

-- AVOID: Too broad pattern
MATCH (a)-[:KNOWS]->(b)
RETURN a, b

-- GOOD: Use labels
MATCH (n:Person)-[:KNOWS]->(m:Person)
RETURN n, m

-- GOOD: Filter early
MATCH (n:Person)
WHERE n.age > 30
MATCH (n)-[:KNOWS]->(b)
RETURN b
```

### Pattern Optimization

```cypher
-- Use specific relationship types
MATCH (n:Person)-[:KNOWS|FRIEND]->(b)
RETURN b

-- Limit path exploration
MATCH (a:Person {name: 'Alice'})-[:KNOWS*..3]->(b)
RETURN b

-- Use PROFILE to analyze
PROFILE MATCH (n:Person)-[:KNOWS]->(b)
RETURN n, b
```

## Pattern Visualization

### Displaying Patterns

```cypher
-- Return nodes and relationships
MATCH (a:Person {name: 'Alice'})-[r:KNOWS]->(b:Person)
RETURN a, r, b

-- Return path
MATCH p = (a:Person {name: 'Alice'})-[:KNOWS*2..3]->(b:Person)
RETURN p

-- Return custom structure
MATCH (a:Person {name: 'Alice'})-[r:KNOWS]->(b:Person)
RETURN {
  from: a.name,
  to: b.name,
  type: type(r),
  since: r.since
} AS connection
```

## Best Practices

1. **Be Specific**: Use labels and properties to make patterns selective
2. **Limit Depth**: Use bounded variable length paths
3. **Use PROFILE**: Understand pattern performance characteristics
4. **Filter Early**: Apply WHERE clauses in the MATCH when possible
5. **Avoid Cartesian Products**: Connect all patterns explicitly
6. **Use WITH**: Break complex patterns into logical steps
7. **Consider Indexes**: Create indexes on frequently queried properties

## Next Steps

- Learn [Advanced Queries](/en/user-guide/advanced-queries) for more complex operations
- Understand [Transaction Control](/en/user-guide/transactions) for data consistency
- Explore [Batch Operations](/en/user-guide/batch-operations) for bulk processing
