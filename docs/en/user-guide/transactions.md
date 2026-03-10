# Transaction Control

Metrix provides full ACID transaction support through its CLI, allowing you to group multiple operations into atomic units. This guide explains how to control transactions in the CLI.

## Understanding Transactions

### What is a Transaction?

A transaction is a sequence of operations performed as a single logical unit of work. In Metrix, transactions adhere to ACID properties:

- **Atomicity**: All operations in a transaction succeed or all fail
- **Consistency**: The database transitions from one valid state to another
- **Isolation**: Concurrent transactions don't interfere with each other
- **Durability**: Committed transactions persist even after system failure

### When to Use Transactions

Use transactions when:
- Multiple operations must succeed or fail together
- Data consistency requires multiple related changes
- You need to rollback changes if an error occurs
- Performing complex multi-step data modifications

## Transaction Commands in CLI

### BEGIN TRANSACTION

Start a new transaction:

```bash
# Start a transaction
metrix> BEGIN

# Transaction is now active
metrix (transaction)>
```

### COMMIT

Commit the current transaction:

```bash
# In transaction mode
metrix (transaction)> COMMIT

# Transaction is committed and closed
metrix>
```

### ROLLBACK

Rollback the current transaction:

```bash
# In transaction mode
metrix (transaction)> ROLLBACK

# All changes are discarded
metrix>
```

## Transaction Examples

### Simple Transaction

```bash
# Start transaction
metrix> BEGIN

# Create nodes
metrix (transaction)> CREATE (p:Person {name: 'Alice', age: 30})
metrix (transaction)> CREATE (p:Person {name: 'Bob', age: 25})

# Create relationship
metrix (transaction)> MATCH (a:Person {name: 'Alice'}), (b:Person {name: 'Bob'})
metrix (transaction)> CREATE (a)-[:KNOWS]->(b)

# Commit all changes
metrix (transaction)> COMMIT
```

### Transaction with Error Handling

```bash
# Start transaction
metrix> BEGIN

# Create person
metrix (transaction)> CREATE (p:Person {name: 'Charlie', age: 35})

# Try to create duplicate (will fail)
metrix (transaction)> CREATE (p:Person {name: 'Alice', age: 30})

# Rollback on error
metrix (transaction)> ROLLBACK
```

### Complex Multi-Step Transaction

```bash
# Start transaction
metrix> BEGIN

# Create company
metrix (transaction)> CREATE (c:Company {name: 'TechCorp', founded: 2020})

# Create employees
metrix (transaction)> CREATE (e1:Employee {name: 'Alice', role: 'Engineer'})
metrix (transaction)> CREATE (e2:Employee {name: 'Bob', role: 'Designer'})

# Create relationships
metrix (transaction)> MATCH (c:Company {name: 'TechCorp'}), (e1:Employee {name: 'Alice'})
metrix (transaction)> CREATE (e1)-[:WORKS_FOR]->(c)

metrix (transaction)> MATCH (c:Company {name: 'TechCorp'}), (e2:Employee {name: 'Bob'})
metrix (transaction)> CREATE (e2)-[:WORKS_FOR]->(c)

# Create employee relationship
metrix (transaction)> MATCH (e1:Employee {name: 'Alice'}), (e2:Employee {name: 'Bob'})
metrix (transaction)> CREATE (e1)-[:COLLEAGUES]->(e2)

# Commit if all succeed
metrix (transaction)> COMMIT
```

## Transaction States

### Auto-Commit Mode (Default)

In auto-commit mode, each statement is executed in its own transaction:

```bash
# Auto-commit mode (default)
metrix> CREATE (p:Person {name: 'Alice'})
# Transaction is automatically committed

metrix> CREATE (p:Person {name: 'Bob'})
# Another automatic transaction
```

### Explicit Transaction Mode

In explicit transaction mode, you control when to commit:

```bash
# Enter transaction mode
metrix> BEGIN

# All statements are part of this transaction
metrix (transaction)> CREATE (p:Person {name: 'Charlie'})
metrix (transaction)> CREATE (p:Person {name: 'Diana'})

# Only commit when ready
metrix (transaction)> COMMIT
```

## Transaction Isolation

Metrix provides isolation between concurrent transactions:

```bash
# Terminal 1
metrix> BEGIN
metrix (transaction)> MATCH (n:Person) WHERE name = 'Alice'
metrix (transaction)> SET n.age = 31

# Terminal 2 (concurrent)
metrix> BEGIN
metrix (transaction)> MATCH (n:Person) WHERE name = 'Alice'
# Sees the original age value

# Terminal 1
metrix (transaction)> COMMIT

# Terminal 2
metrix (transaction)> COMMIT
# May fail due to concurrent modification
```

## Transaction Best Practices

### Keep Transactions Short

```bash
# GOOD: Short, focused transaction
metrix> BEGIN
metrix (transaction)> CREATE (p:Person {name: 'Eve'})
metrix (transaction)> COMMIT

# AVOID: Long-running transaction
metrix> BEGIN
metrix (transaction)> CREATE (p:Person {name: 'Frank'})
# ... wait for user input ...
metrix (transaction)> CREATE (p:Person {name: 'Grace'})
metrix (transaction)> COMMIT
```

### Handle Errors Gracefully

```bash
# Start transaction
metrix> BEGIN

# Try operations
metrix (transaction)> CREATE (p:Person {name: 'Henry', age: 28})
metrix (transaction)> CREATE (p:Person {name: 'Iris', age: 32})

# Check for errors before committing
metrix (transaction)> MATCH (n:Person) WHERE n.name IN ['Henry', 'Iris']
metrix (transaction)> RETURN count(n) AS created

# If count is 2, commit
metrix (transaction)> COMMIT

# Otherwise rollback
metrix (transaction)> ROLLBACK
```

### Use Transactions for Data Integrity

```bash
# Ensure referential integrity
metrix> BEGIN

# Create department
metrix (transaction)> CREATE (d:Department {name: 'Engineering'})

# Create employee
metrix (transaction)> CREATE (e:Employee {name: 'Jack'})

# Link them
metrix (transaction)> MATCH (d:Department {name: 'Engineering'}), (e:Employee {name: 'Jack'})
metrix (transaction)> CREATE (e)-[:WORKS_IN]->(d)

# Only commit if all succeed
metrix (transaction)> COMMIT
```

## Transaction Patterns

### Batch Insert with Transaction

```bash
# Insert multiple related records
metrix> BEGIN

# Insert user
metrix (transaction)> CREATE (u:User {id: 1, name: 'Alice'})

# Insert user settings
metrix (transaction)> CREATE (s:Settings {user_id: 1, theme: 'dark'})

# Link them
metrix (transaction)> MATCH (u:User {id: 1}), (s:Settings {user_id: 1})
metrix (transaction)> CREATE (u)-[:HAS_SETTINGS]->(s)

# Commit all or none
metrix (transaction)> COMMIT
```

### Update with Validation

```bash
# Validate before committing
metrix> BEGIN

# Update record
metrix (transaction)> MATCH (p:Person {name: 'Alice'})
metrix (transaction)> SET p.age = 32

# Verify update
metrix (transaction)> MATCH (p:Person {name: 'Alice'})
metrix (transaction)> RETURN p.age

# If correct, commit; otherwise rollback
metrix (transaction)> COMMIT
```

### Conditional Transaction

```bash
# Transaction with logic
metrix> BEGIN

# Check if person exists
metrix (transaction)> MATCH (p:Person {name: 'Bob'})
metrix (transaction)> RETURN p

# If exists, update; otherwise create
metrix (transaction)> MERGE (p:Person {name: 'Bob'})
metrix (transaction)> ON CREATE SET p.age = 25, p.created = timestamp()
metrix (transaction)> ON MATCH SET p.lastUpdated = timestamp()

# Commit result
metrix (transaction)> COMMIT
```

## Transaction Monitoring

### View Transaction Status

```bash
# Check if in transaction
metrix> SHOW TRANSACTION

# View transaction details
metrix> EXPLAIN TRANSACTION
```

### Transaction Statistics

```bash
# View transaction statistics
metrix> SHOW STATS

# Shows:
# - Active transactions
# - Committed transactions
# - Rolled back transactions
# - Transaction duration
```

## Error Handling

### Constraint Violations

```bash
# Unique constraint violation
metrix> BEGIN
metrix (transaction)> CREATE (p:Person {email: 'alice@example.com'})
metrix (transaction)> CREATE (p:Person {email: 'alice@example.com'})
metrix (transaction)> COMMIT
# ERROR: Unique constraint violated
# Transaction automatically rolled back
```

### Deadlock Handling

```bash
# If deadlock occurs
metrix> BEGIN
# ... operations ...
metrix (transaction)> COMMIT
# ERROR: Deadlock detected
# Transaction automatically rolled back

# Retry the transaction
metrix> BEGIN
# ... operations ...
metrix (transaction)> COMMIT
# Success
```

## Performance Considerations

### Transaction Size

```bash
# GOOD: Small, focused transactions
metrix> BEGIN
metrix (transaction)> CREATE (p:Person {name: 'Kevin'})
metrix (transaction)> COMMIT

# AVOID: Very large transactions
metrix> BEGIN
metrix (transaction)> CREATE (p:Person {name: 'Laura'})
# ... 10,000 more creates ...
metrix (transaction)> COMMIT
# May cause performance issues
```

### Lock Contention

```bash
# Minimize lock time
metrix> BEGIN
# Do read operations first
metrix (transaction)> MATCH (p:Person) WHERE p.age > 30
# Then do writes
metrix (transaction)> MATCH (p:Person {name: 'Alice'}) SET p.age = 31
metrix (transaction)> COMMIT
```

## Best Practices Summary

1. **Keep transactions short**: Minimize time between BEGIN and COMMIT
2. **Handle errors**: Always check for errors before committing
3. **Use for consistency**: Group related operations in transactions
4. **Avoid long transactions**: Don't wait for user input in transactions
5. **Test before commit**: Verify data integrity before committing
6. **Use MERGE**: Handle create-or-update scenarios elegantly
7. **Monitor performance**: Use PROFILE to understand transaction impact

## Next Steps

- Learn about [Batch Operations](/en/user-guide/batch-operations) for bulk data processing
- Explore [Import & Export](/en/user-guide/import-export) for data migration
- Understand [Advanced Queries](/en/user-guide/advanced-queries) for complex operations
