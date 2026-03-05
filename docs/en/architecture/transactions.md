# Transaction Management

Metrix provides full ACID transaction support with optimistic concurrency control and comprehensive rollback capabilities.

## ACID Properties

```mermaid
graph TD
    A[Atomicity] --> D[Transactions]
    B[Consistency] --> D
    C[Isolation] --> D
    E[Durability] --> D

    style A fill:#dfd
    style B fill:#ffd
    style C fill:#ddf
    style E fill:#fdd
```

### Atomicity

All operations in a transaction succeed or fail together.

```mermaid
sequenceDiagram
    participant TX as Transaction
    participant DB as Database
    participant WAL as Write-Ahead Log

    TX->>WAL: log(operation1)
    TX->>WAL: log(operation2)
    TX->>WAL: log(operation3)

    Note over TX: Operation 3 fails!

    TX->>TX: rollback()
    TX->>WAL: reverse all operations

    Note over DB: Database unchanged
```

**Implementation**:
- WAL records all operations before execution
- On failure, reverse operations from WAL
- Database remains consistent

### Consistency

Database always transitions from one valid state to another.

```mermaid
stateDiagram-v2
    [*] --> Valid
    Valid --> Valid: Transaction
    Valid --> Invalid: Violation
    Invalid --> Valid: Rollback
```

**Constraints**:
- **Type Constraints**: Property types must match
- **Uniqueness Constraints**: Unique property values (future)
- **Existence Constraints**: Required properties (future)

### Isolation

Concurrent transactions don't interfere with each other.

```mermaid
sequenceDiagram
    participant TX1 as Transaction 1
    participant TX2 as Transaction 2
    participant DB as Database

    TX1->>DB: read(value)
    TX1->>TX1: value = 10

    TX2->>DB: read(value)
    TX2->>TX2: value = 10

    TX1->>TX1: modify to 20
    TX1->>DB: commit()
    Note over TX1: TX1 sees 20

    TX2->>TX2: modify to 30
    TX2->>DB: commit()
    Note over TX2: Conflict detected!
```

**Isolation Levels**:
- **Read Committed**: Default, see committed data
- **Serializable**: Full isolation (planned)
- **Snapshot Consistency**: Per-transaction views

### Durability

Committed changes survive failures.

```mermaid
graph TD
    A[Commit] --> B[WAL Flush]
    B --> C[Main Storage]
    C --> D[Checkpoint]

    style B fill:#f99,stroke:#333,stroke-width:2px
    style C fill:#9f9,stroke:#333,stroke-width:2px
```

**Guarantees**:
- WAL flushed to disk on commit
- Changes persist even if process crashes
- Recovery replays committed transactions

## Transaction Lifecycle

```mermaid
stateDiagram-v2
    [*] --> Active: begin()
    Active --> Active: execute()
    Active --> Committed: commit()
    Active --> RolledBack: rollback()
    Committed --> [*]
    RolledBack --> [*]
```

### Beginning a Transaction

```cpp
auto db = Database::open("./mydb");
auto tx = db->beginTransaction();
```

### Executing Operations

```cpp
tx->execute("CREATE (n:User {name: 'Alice'})");
tx->execute("CREATE (n:User {name: 'Bob'})");
```

### Committing

```cpp
tx->commit();
```

### Rolling Back

```cpp
tx->rollback();
```

## Optimistic Concurrency Control

Metrix uses optimistic concurrency control with versioning.

### Version Tracking

```mermaid
graph LR
    A[Entity v1] --> B[Entity v2]
    A --> C[TX1 reads v1]
    B --> D[TX2 reads v2]

    C --> E[TX1 modifies]
    D --> F[TX2 modifies]

    E --> G{Conflict?}
    F --> G
    G -->|Yes| H[Rollback]
    G -->|No| I[Commit]
```

### Conflict Detection

```cpp
class Entity {
private:
    uint64_t version_;

public:
    bool detectConflict(const Entity& other) const {
        return version_ != other.version_;
    }
};
```

### Conflict Resolution

1. **Detect**: Version mismatch on commit
2. **Retry**: Automatic retry with exponential backoff
3. **Fail**: After max retries, throw error

## Transaction State Management

### State Chains

Each entity maintains a chain of states:

```mermaid
graph LR
    S1[State v1<br/>Initial] --> S2[State v2<br/>Modified]
    S2 --> S3[State v3<br/>Modified]
    S3 -.->|Commit| S4[Committed State]
    S3 -.->|Rollback| S1

    style S1 fill:#dfd
    style S2 fill:#ffd
    style S3 fill:#ffd
    style S4 fill:#ddf
```

### Dirty Entity Tracking

```cpp
class DirtyEntityRegistry {
private:
    std::unordered_set<EntityId> dirty_;

public:
    void markDirty(EntityId id);
    bool isDirty(EntityId id) const;
    void clear();
};
```

### Flush on Commit

```mermaid
sequenceDiagram
    participant TX as Transaction
    participant DR as Dirty Registry
    participant FS as FileStorage
    participant WAL as Write-Ahead Log

    TX->>TX: commit()
    TX->>DR: getDirtyEntities()
    DR->>TX: dirty list

    loop For each dirty entity
        TX->>WAL: log(operation)
        TX->>FS: persist(entity)
    end

    TX->>WAL: flush()
    TX->>DR: clear()
```

## Transaction Isolation

### Read Committed (Default)

```cpp
auto tx = db->beginTransaction(IsolationLevel::ReadCommitted);
```

**Behavior**:
- See only committed data
- Non-repeatable reads possible
- Phantom reads possible

### Serializable (Planned)

```cpp
auto tx = db->beginTransaction(IsolationLevel::Serializable);
```

**Behavior**:
- Full isolation
- No phantom reads
- Higher overhead

## Nested Transactions

Metrix supports savepoints for nested transactions:

```cpp
auto tx = db->beginTransaction();

tx->execute("CREATE (n:User {name: 'Alice'})");

auto savepoint = tx->createSavepoint("sp1");

tx->execute("CREATE (n:User {name: 'Bob'})");

// Rollback to savepoint
tx->rollbackToSavepoint("sp1");

tx->commit(); // Only Alice is committed
```

## Transaction Configuration

### Timeout Configuration

```cpp
struct TransactionConfig {
    std::chrono::milliseconds timeout = 30000ms;  // 30 seconds
    size_t maxRetries = 3;
    std::chrono::milliseconds retryDelay = 100ms;
};
```

### Isolation Level

```cpp
enum class IsolationLevel {
    ReadCommitted,
    Serializable
};
```

## Error Handling

### Transaction Errors

```cpp
try {
    auto tx = db->beginTransaction();
    tx->execute("CREATE (n:User {name: 'Alice'})");
    tx->commit();
} catch (const TransactionError& e) {
    // Transaction failed, already rolled back
    std::cerr << "Transaction failed: " << e.what() << std::endl;
} catch (const StorageError& e) {
    // Storage I/O error
    std::cerr << "Storage error: " << e.what() << std::endl;
}
```

### Automatic Rollback

Transactions are automatically rolled back on:
- Unhandled exceptions
- Timeout
- Database closure
- Conflict detection failure

## Performance Considerations

### Transaction Size

**Best Practices**:
- Keep transactions short
- Minimize operations per transaction
- Avoid long-running transactions

### Batch Operations

```cpp
// Good - Multiple small transactions
for (int i = 0; i < 1000; ++i) {
    auto tx = db->beginTransaction();
    tx->execute("CREATE (n:User {id: $id})", {{"id", i}});
    tx->commit();
}

// Better - Batch in single transaction (if acceptable)
auto tx = db->beginTransaction();
for (int i = 0; i < 1000; ++i) {
    tx->execute("CREATE (n:User {id: $id})", {{"id", i}});
}
tx->commit();
```

### WAL Management

- **Checkpoint Frequency**: Balance performance and durability
- **WAL Size**: Monitor and truncate old entries
- **Flush Strategy**: Group flushes for efficiency

## Monitoring and Debugging

### Transaction Statistics

```cpp
class TransactionStats {
public:
    size_t getActiveCount() const;
    size_t getCommittedCount() const;
    size_t getRolledBackCount() const;
    std::chrono::milliseconds getAverageDuration() const;
};
```

### Logging

Enable transaction logging for debugging:

```cpp
db->setLogLevel(LogLevel::Debug);
db->setLogTransaction(true);
```

## Best Practices

1. **Always commit or rollback**: Never leave transactions open
2. **Handle exceptions**: Catch and handle transaction errors
3. **Keep transactions short**: Minimize lock duration
4. **Use appropriate isolation**: Use lowest isolation level needed
5. **Monitor performance**: Track transaction duration and conflicts

## Next Steps

- [Storage System](/en/architecture/storage) - How transactions persist data
- [Query Engine](/en/architecture/query-engine) - Query execution in transactions
- [API Reference](/en/api/cpp-api) - Transaction API usage
