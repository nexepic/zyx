# Batch Operations

When working with large datasets in Metrix, efficient batch operations are crucial for performance and resource management. This guide covers techniques for bulk data processing.

## Understanding Batch Operations

### Why Use Batch Operations?

Batch operations provide several benefits:
- **Performance**: Reduced overhead by grouping operations
- **Memory efficiency**: Process data in chunks rather than all at once
- **Transaction control**: Better control over commit points
- **Error handling**: Isolate failures to specific batches

### When to Use Batching

Use batch operations when:
- Importing large datasets (>1000 records)
- Performing bulk updates or deletions
- Migrating data from other systems
- Processing time-consuming operations

## Batching Strategies

### Fixed Size Batches

```bash
# Process 1000 records at a time
# Batch 1
metrix> BEGIN
metrix (transaction)> CREATE (p:Person {id: 1, name: 'Alice'})
metrix (transaction)> CREATE (p:Person {id: 2, name: 'Bob'})
# ... 998 more records ...
metrix (transaction)> COMMIT

# Batch 2
metrix> BEGIN
metrix (transaction)> CREATE (p:Person {id: 1001, name: 'Charlie'})
# ... next 1000 records ...
metrix (transaction)> COMMIT
```

### Time-Based Batching

```bash
# Commit every N seconds
metrix> BEGIN
# Start timer
# Process records for 10 seconds
metrix (transaction)> CREATE (p:Person {name: 'David'})
# ... more records ...
# After 10 seconds:
metrix (transaction)> COMMIT

# Start next batch
metrix> BEGIN
# Continue processing...
```

### Memory-Based Batching

```bash
# Monitor memory usage
metrix> BEGIN
# Process until memory reaches threshold
metrix (transaction)> CREATE (p:Person {name: 'Eve'})
# ... more records ...
# Check memory: if usage > 500MB, commit
metrix (transaction)> COMMIT
```

## CLI Batching Techniques

### Using Scripts

Create a Cypher script for batch operations:

```cypher
// batch_import.cypher
// Process in batches of 1000

// Batch 1: Records 1-1000
BEGIN
CREATE (p:Person {id: 1, name: 'Person1'})
CREATE (p:Person {id: 2, name: 'Person2'})
// ... up to id: 1000 ...
COMMIT;

// Batch 2: Records 1001-2000
BEGIN
CREATE (p:Person {id: 1001, name: 'Person1001'})
// ... up to id: 2000 ...
COMMIT;
```

Run the script:

```bash
$ metrix-cli < batch_import.cypher
```

### Using Parameterized Queries

```bash
# Prepare parameterized query
metrix> :param batch_size => 1000
metrix> :param start_id => 1

# Execute with parameters
metrix> BEGIN
metrix (transaction)> CREATE (p:Person {id: $start_id})
metrix (transaction)> CREATE (p:Person {id: $start_id + 1})
# ... etc ...
metrix (transaction)> COMMIT
```

## Batch Import Patterns

### CSV Import in Batches

```bash
# Import large CSV in batches
# Using shell script to split CSV and process

#!/bin/bash
# import_batches.sh

BATCH_SIZE=1000
TOTAL_RECORDS=10000
FILE="people.csv"

for ((START=1; START<=TOTAL_RECORDS; START+=BATCH_SIZE)); do
  END=$((START + BATCH_SIZE - 1))

  # Generate Cypher for this batch
  cat > batch_$START.cypher <<EOF
BEGIN
\$(tail -n +$START $FILE | head -n $BATCH_SIZE | \
  awk -F, '{print "CREATE (:Person {id: " \$1 ", name: \"" \$2 "\", age: " \$3 "})"}')
COMMIT;
EOF

  # Execute batch
  metrix-cli < batch_$START.cypher
  echo "Imported records $START to $END"

  # Clean up
  rm batch_$START.cypher
done
```

### Batch Node Creation

```cypher
// Create nodes in batches
// Batch 1
BEGIN
UNWIND range(1, 1000) AS id
CREATE (p:Person {id: id, name: 'Person' + id})
COMMIT;

// Batch 2
BEGIN
UNWIND range(1001, 2000) AS id
CREATE (p:Person {id: id, name: 'Person' + id})
COMMIT;
```

### Batch Relationship Creation

```cypher
// Create relationships after nodes exist
// Batch 1
BEGIN
MATCH (p1:Person {id: 1}), (p2:Person {id: 2})
CREATE (p1)-[:KNOWS]->(p2);

MATCH (p1:Person {id: 1}), (p2:Person {id: 3})
CREATE (p1)-[:KNOWS]->(p2);

// ... 998 more relationships ...
COMMIT;
```

## Batch Update Patterns

### Batch Property Updates

```cypher
// Update properties in batches
// Batch 1
BEGIN
MATCH (p:Person)
WHERE p.id >= 1 AND p.id <= 1000
SET p.processed = true, p.updatedAt = timestamp()
COMMIT;

// Batch 2
BEGIN
MATCH (p:Person)
WHERE p.id >= 1001 AND p.id <= 2000
SET p.processed = true, p.updatedAt = timestamp()
COMMIT;
```

### Batch Delete

```cypher
// Delete in batches to avoid locking issues
// Batch 1
BEGIN
MATCH (p:Person)
WHERE p.id >= 1 AND p.id <= 1000
DETACH DELETE p
COMMIT;

// Batch 2
BEGIN
MATCH (p:Person)
WHERE p.id >= 1001 AND p.id <= 2000
DETACH DELETE p
COMMIT;
```

## Performance Optimization

### Use UNWIND for Bulk Operations

```cypher
// Efficient batch creation using UNWIND
BEGIN
WITH [
  {id: 1, name: 'Alice', age: 30},
  {id: 2, name: 'Bob', age: 25},
  {id: 3, name: 'Charlie', age: 35}
  // ... up to 1000 items ...
] AS batch
UNWIND batch AS item
CREATE (p:Person {id: item.id, name: item.name, age: item.age})
COMMIT;
```

### Using FOREACH for Batching

```cypher
// Process list in chunks
BEGIN
MATCH (p:Person)
WITH collect(p) AS people
WITH people, size(people) AS total
FOREACH (i IN range(0, total/1000) |
  FOREACH (p IN people[i*1000..(i+1)*1000-1] |
    SET p.batchProcessed = true
  )
)
COMMIT;
```

### Parallel Batch Processing

```bash
# Run multiple batch operations in parallel
# Terminal 1
metrix-cli < batch_1.cypher &

# Terminal 2
metrix-cli < batch_2.cypher &

# Terminal 3
metrix-cli < batch_3.cypher &

# Wait for all to complete
wait
```

## Error Handling in Batches

### Skip Failed Records

```cypher
// Use FOREACH with conditional logic
BEGIN
MATCH (p:Person)
WHERE p.id >= 1 AND p.id <= 1000
FOREACH (_ IN CASE WHEN p.age > 0 THEN [1] ELSE [] END |
  SET p.valid = true
)
COMMIT;
```

### Batch with Validation

```cypher
// Validate before batch commit
BEGIN
// Perform updates
MATCH (p:Person)
WHERE p.id >= 1 AND p.id <= 1000
SET p.processed = true

// Verify updates
WITH count(p) AS updated
WHERE updated = 1000
// Only commit if all updated
COMMIT;
```

### Continue on Error

```bash
# Wrapper script to handle batch failures
#!/bin/bash
# robust_batch.sh

for BATCH_FILE in batch_*.cypher; do
  echo "Processing $BATCH_FILE"

  # Execute batch
  if metrix-cli < $BATCH_FILE; then
    echo "Success: $BATCH_FILE"
  else
    echo "Failed: $BATCH_FILE"
    # Log failure and continue
    echo "$BATCH_FILE failed" >> failures.log
  fi
done
```

## Monitoring Batch Operations

### Progress Tracking

```cypher
// Track progress during batch operation
BEGIN
// Get initial count
MATCH (p:Person)
WITH count(p) AS initial_count

// Process batch
MATCH (p:Person)
WHERE p.id >= 1 AND p.id <= 1000
SET p.processed = true

// Report progress
WITH initial_count, count(p) AS total
RETURN initial_count, total, 'Batch 1/10' AS progress
COMMIT;
```

### Batch Statistics

```bash
# Collect statistics during batch processing
#!/bin/bash
# batch_with_stats.sh

START_TIME=$(date +%s)

for BATCH in {1..10}; do
  BATCH_START=$(date +%s)

  # Process batch
  metrix-cli < batch_${BATCH}.cypher

  BATCH_END=$(date +%s)
  BATCH_DURATION=$((BATCH_END - BATCH_START))

  echo "Batch $BATCH: ${BATCH_DURATION}s"
done

END_TIME=$(date +%s)
TOTAL_DURATION=$((END_TIME - START_TIME))

echo "Total time: ${TOTAL_DURATION}s"
```

## Best Practices

### Batch Size Guidelines

```bash
# Small records (< 1KB each): 5000-10000 per batch
# Medium records (1-10KB each): 1000-5000 per batch
# Large records (> 10KB each): 100-1000 per batch
# Complex operations: 100-500 per batch
```

### Memory Management

```cypher
// Release memory between batches
BEGIN
// Process batch
MATCH (p:Person)
WHERE p.id >= 1 AND p.id <= 1000
SET p.processed = true
COMMIT;

// Clear cache
metrix> CALL db.clearQueryCache();
```

### Transaction Control

```bash
# Always commit between batches
# Don't let transactions run too long
# Monitor lock contention

# Good practice:
for BATCH in batch_*.cypher; do
  # Each file has its own transaction
  metrix-cli < $BATCH
done
```

## Real-World Examples

### Social Network Import

```bash
#!/bin/bash
# import_social_network.sh

# Import users in batches
for BATCH in {1..100}; do
  START=$((BATCH * 10000 - 9999))
  END=$((BATCH * 10000))

  cat > user_batch_$BATCH.cypher <<EOF
BEGIN
USING PERIODIC COMMIT 1000
LOAD CSV FROM 'file:///users/users_$START-$END.csv' AS row
CREATE (u:User {id: toInteger(row.id), name: row.name, email: row.email})
COMMIT;
EOF

  metrix-cli < user_batch_$BATCH.cypher
done
```

### E-commerce Data Migration

```cypher
// Migrate product catalog in batches
// Batch 1: Electronics
BEGIN
MATCH (p:OldProduct {category: 'Electronics'})
CREATE (n:Product {
  id: p.id,
  name: p.name,
  price: p.price,
  category: 'Electronics'
})
DELETE p
COMMIT;

// Batch 2: Clothing
BEGIN
MATCH (p:OldProduct {category: 'Clothing'})
CREATE (n:Product {
  id: p.id,
  name: p.name,
  price: p.price,
  category: 'Clothing'
})
DELETE p
COMMIT;
```

## Tips and Tricks

1. **Test batch size**: Start with small batches and increase
2. **Monitor resources**: Watch CPU, memory, and disk I/O
3. **Use indexes**: Create indexes before batch operations
4. **Disable constraints**: Temporarily disable for faster imports
5. **Log progress**: Keep detailed logs for troubleshooting
6. **Validate data**: Check data integrity after each batch
7. **Plan rollback**: Know how to undo if something goes wrong

## Next Steps

- Learn about [Import & Export](/en/user-guide/import-export) for data migration
- Understand [Transaction Control](/en/user-guide/transactions) for data consistency
- Explore [Advanced Queries](/en/user-guide/advanced-queries) for complex operations
