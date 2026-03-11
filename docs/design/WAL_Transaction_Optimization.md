# WAL and Transaction System - Architecture Design

## Project Context

Metrix is an **embedded graph database** (similar to SQLite), not a client-server database.
This has critical implications for WAL and transaction design.

## Current Architecture Analysis

### Existing Components

1. **WALManager.hpp/cpp.bak** - WAL management with:
   - Multi-file WAL rotation (`wal_0000000001.log`, `wal_0000000002.log`)
   - Background flush thread (200ms interval)
   - Background checkpoint thread (30s interval)
   - Transaction begin/commit/rollback logging
   - Crash recovery

2. **WALRecord.hpp** - Binary record format:
   - Fixed header (magic, size, LSN, type, transactionId, checksum)
   - Multiple record types (NODE_INSERT, EDGE_INSERT, TRANSACTION_BEGIN, etc.)

3. **Transaction.cpp** - Minimal wrapper:
   - Simple `beginWrite()` / `commitWrite()` / `rollbackWrite()` calls to FileStorage
   - No WriteSet/ReadSet
   - No conflict detection
   - No MVCC support

### Critical Issues for Embedded Database

#### ❌ Issue 1: Background Threads Not Suitable

```cpp
// Existing code - NOT suitable for embedded database
std::thread flushThread_;
std::thread checkpointThread_;
```

**Problem**:
- Embedded database may be closed at any time
- No guarantee of graceful shutdown
- Background threads complicate lifecycle management

**Impact**: High - Makes database unreliable for embedded use

#### ❌ Issue 2: Multi-File WAL Overcomplicated

```cpp
// Existing code - multi-file rotation
void rotateLogFileIfNeeded();
std::vector<std::string> getWalFiles() const;
```

**Problem**:
- Recovery requires processing multiple files
- File management overhead
- SQLite uses single WAL file successfully

**Impact**: Medium - Unnecessary complexity

#### ❌ Issue 3: Incomplete Checkpoint Implementation

```cpp
// Existing code - only records checkpoint, doesn't apply WAL
uint64_t WALManager::checkpoint() {
    auto record = std::make_unique<CheckpointRecord>(checkpointLSN);
    // Writes checkpoint record but doesn't apply WAL to main DB!
}
```

**Problem**:
- Checkpoint should apply WAL records to main database
- No integration with FileStorage
- No actual data persistence

**Impact**: Critical - WAL never applied to database

#### ❌ Issue 4: No Real Transaction Semantics

```cpp
// Existing Transaction.cpp - just a wrapper
void Transaction::commit() const { storage->commitWrite(); }
```

**Problem**:
- No WriteSet tracking
- No ReadSet for conflict detection
- No MVCC snapshot isolation
- No ACID guarantees

**Impact**: Critical - Not a real transaction system

## Optimized Design

### Design Goals

1. **Embedded-first**: No background threads, predictable lifecycle
2. **SQLite-inspired**: Single WAL file, proven design
3. **Graph-aware**: Handle nodes, edges, and relationships correctly
4. **ACID compliant**: Full transaction support with MVCC
5. **Production-ready**: Comprehensive error handling and testing

### Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                     Database                            │
│  - open(): Perform crash recovery if needed             │
│  - close(): Force checkpoint                            │
│  - beginTransaction(): Create transaction with snapshot  │
└──────────────────┬──────────────────────────────────────┘
                   │
        ┌──────────┴──────────┐
        ↓                     ↓
┌──────────────┐      ┌──────────────┐
│ Transaction  │      │  WALManager  │
│    Manager   │      │              │
└──────┬───────┘      └──────┬───────┘
       │                     │
       │              ┌──────┴───────┐
       ↓              ↓              ↓
┌──────────┐   ┌──────────┐  ┌──────────┐
│ MVCC     │   │   WAL    │  │Checkpt   │
│ Snapshot │   │  Buffer  │  │ Manager  │
└──────────┘   └──────────┘  └──────────┘
```

### Key Design Decisions

#### 1. Single WAL File

**Format**: `<database>.wal`

**Structure**:
```
[WAL Header]
  - Magic number: "METRWAL"
  - Version: 1
  - Page size: 4096
  - Checkpoint LSN: 0 (initially)

[WAL Records]
  - Record 1 (LSN=1)
  - Record 2 (LSN=2)
  - ...
  - Record N (LSN=N)
```

**Benefits**:
- Simple recovery (read one file)
- Easy checkpoint management
- Proven by SQLite

#### 2. No Background Threads

**Flush Strategy**:
```cpp
// On every transaction commit:
void Transaction::commit() {
    // 1. Write to WAL buffer
    // 2. Validate conflicts (OCC)
    // 3. Flush WAL to disk (fsync)
    // 4. Apply to main storage
    // 5. Mark transaction committed
}
```

**Checkpoint Strategy**:
- **On close**: Always checkpoint before closing
- **On size threshold**: When WAL > 10MB, auto-checkpoint
- **Manual**: User can request checkpoint

#### 3. MVCC with OCC

**Transaction States**:
```
begin → ACTIVE → COMMITTING → COMMITTED
                ↓
            ROLLED_BACK
```

**Read Committed Isolation**:
- Each transaction gets a snapshot on start
- Reads see only committed data as of snapshot
- Writes checked at commit time

**Conflict Detection**:
```cpp
bool TransactionManager::validateConflict(Transaction* tx) {
    // Check write-write conflicts
    for (auto& write : tx->writeSet_) {
        if (isModifiedByOtherTransaction(write.entityId, tx->id_)) {
            return false;  // Conflict!
        }
    }
    return true;  // No conflict
}
```

#### 4. WAL Record Format (Binary + JSON Debug)

**Binary Format** (runtime):
```cpp
struct WALRecordHeader {
    uint32_t magic;         // 0x57414C52 ("WALR")
    uint32_t recordSize;    // Total size
    uint64_t lsn;           // Log Sequence Number
    WALRecordType type;     // NODE_INSERT, etc.
    uint64_t transactionId; // 0 if not in transaction
    uint32_t checksum;      // CRC32
};

// Followed by type-specific payload
```

**JSON Format** (debugging):
```json
{
  "lsn": 12345,
  "type": "NODE_INSERT",
  "transactionId": 100,
  "timestamp": 1234567890,
  "payload": {
    "nodeId": 456,
    "label": "Person",
    "properties": {"name": "Alice", "age": 30}
  }
}
```

#### 5. Checkpoint Process

```cpp
void WALManager::checkpoint() {
    // 1. Flush any buffered records
    flush();

    // 2. Read all WAL records since last checkpoint
    auto records = readRecordsSince(lastCheckpointLSN_);

    // 3. Apply to main database
    for (auto& record : records) {
        if (isCommitted(record->transactionId)) {
            applyRecordToMainStorage(record);
        }
    }

    // 4. Update WAL header with new checkpoint LSN
    updateCheckpointLSN(currentLSN_);

    // 5. Truncate WAL file
    truncateWAL();

    // 6. Sync main database file
    syncMainDatabase();
}
```

#### 6. Crash Recovery

```cpp
void Database::open() {
    // 1. Check if WAL exists
    if (!walManager->needsRecovery()) {
        return;  // Clean shutdown
    }

    // 2. Perform recovery
    walManager->recover([this](const WALRecord* record) {
        // Apply committed transactions to main storage
        this->fileStorage->applyWALRecord(record);
    });

    // 3. Checkpoint to clean WAL
    walManager->checkpoint();
}
```

### Component Design

#### WALManager (Simplified)

```cpp
class WALManager {
public:
    explicit WALManager(const std::string& dbPath);

    // Write operations
    uint64_t writeRecord(std::unique_ptr<WALRecord> record);
    void flush();  // fsync WAL to disk

    // Checkpoint
    void checkpoint();

    // Recovery
    bool needsRecovery() const;
    void recover(std::function<void(const WALRecord*)> applyFunc);

    // Lifecycle
    void close();  // Force checkpoint before close

private:
    std::string walPath_;
    std::fstream walFile_;

    // Buffer management (no background thread)
    std::vector<uint8_t> buffer_;
    size_t bufferSize_;

    // LSN tracking
    std::atomic<uint64_t> currentLSN_{1};
    std::atomic<uint64_t> lastCheckpointLSN_{0};

    // Configuration
    size_t maxBufferSize_ = 1024 * 1024;  // 1MB
    size_t checkpointThreshold_ = 10 * 1024 * 1024;  // 10MB
};
```

#### Transaction (Full Implementation)

```cpp
class Transaction {
public:
    enum class State {
        ACTIVE,
        COMMITTING,
        COMMITTED,
        ROLLED_BACK
    };

    // Graph operations
    Node createNode(const std::string& label);
    Edge createEdge(int64_t from, int64_t to, const std::string& label);
    void deleteNode(int64_t nodeId);
    void deleteEdge(int64_t edgeId);

    // Property operations
    void setNodeProperty(int64_t nodeId, const std::string& key,
                        const PropertyValue& value);
    void setEdgeProperty(int64_t edgeId, const std::string& key,
                        const PropertyValue& value);

    // Query operations (with MVCC snapshot)
    std::optional<Node> getNode(int64_t nodeId);
    std::optional<Edge> getEdge(int64_t edgeId);

    // Traversal (with revalidation)
    std::vector<Edge> getOutgoingEdges(int64_t nodeId);

    // Transaction control
    void commit();
    void rollback();

    State getState() const;
    uint64_t getTransactionId() const;

private:
    uint64_t transactionId_;
    uint64_t startSnapshotId_;
    State state_;

    // WriteSet: Track all modifications
    struct WriteEntry {
        enum Type {
            CREATE_NODE, CREATE_EDGE,
            DELETE_NODE, DELETE_EDGE,
            UPDATE_PROPERTY
        };
        Type type;
        int64_t entityId;
        std::string key;
        PropertyValue value;
    };
    std::vector<WriteEntry> writeSet_;

    // ReadSet: For conflict detection
    std::unordered_set<int64_t> readSet_;

    // Friend for manager access
    friend class TransactionManager;
};
```

#### TransactionManager

```cpp
class TransactionManager {
public:
    explicit TransactionManager(std::shared_ptr<WALManager> walManager);

    // Transaction lifecycle
    std::unique_ptr<Transaction> beginTransaction();
    void commit(Transaction* tx);
    void rollback(Transaction* tx);

    // Conflict detection (OCC)
    bool validateConflict(const Transaction* tx);

    // Snapshot management
    uint64_t getCurrentSnapshotId();
    uint64_t createSnapshot();

private:
    std::shared_ptr<WALManager> walManager_;
    std::atomic<uint64_t> nextTransactionId_{1};
    std::atomic<uint64_t> currentSnapshotId_{0};

    // Track active transactions
    std::unordered_map<uint64_t, Transaction*> activeTransactions_;
    std::mutex transactionMutex_;

    // Conflict detection
    bool detectWriteWriteConflict(const Transaction* tx);
    bool detectReadWriteConflict(const Transaction* tx);
};
```

### Implementation Phases

#### Phase 1: WAL Infrastructure (Week 1-2)
- [ ] Implement WALRecord types with binary serialization
- [ ] Implement CRC32 checksums
- [ ] Implement JSON dump for debugging
- [ ] Create unit tests for WALRecord

#### Phase 2: WALManager (Week 2-3)
- [ ] Implement single-file WAL manager
- [ ] Implement buffer management (no background threads)
- [ ] Implement flush with fsync
- [ ] Create unit tests for WALManager

#### Phase 3: Transaction Basics (Week 3-4)
- [ ] Implement Transaction class with WriteSet/ReadSet
- [ ] Implement TransactionManager
- [ ] Integrate with WALManager
- [ ] Create unit tests

#### Phase 4: MVCC and Conflict Detection (Week 4-5)
- [ ] Implement MVCC snapshot mechanism
- [ ] Implement OCC conflict detection
- [ ] Implement graph traversal with revalidation
- [ ] Create unit tests

#### Phase 5: Checkpoint and Recovery (Week 5-6)
- [ ] Implement checkpoint (apply WAL to main DB)
- [ ] Implement crash recovery
- [ ] Integrate with Database::open()/close()
- [ ] Create integration tests

#### Phase 6: Testing and Optimization (Week 6-7)
- [ ] Comprehensive unit tests (95%+ coverage)
- [ ] Integration tests
- [ ] Performance benchmarks
- [ ] Documentation

### Performance Targets

- **Throughput**: >10,000 transactions/second
- **Commit latency**: P99 < 10ms (small transactions)
- **Recovery time**: < 30s for 10M records
- **Memory overhead**: < 100MB per 1,000 active transactions

### Testing Strategy

1. **Unit Tests**: Each component tested in isolation
2. **Integration Tests**: End-to-end transaction workflows
3. **Recovery Tests**: Simulate crashes and verify recovery
4. **Concurrency Tests**: Multi-threaded transaction workloads
5. **Performance Tests**: Benchmark against targets

## Conclusion

The optimized design addresses all critical issues in the existing implementation:

✅ **Embedded-first**: No background threads, predictable lifecycle
✅ **Simple WAL**: Single file, proven design
✅ **Real transactions**: ACID with MVCC and OCC
✅ **Complete checkpoint**: Actually applies WAL to database
✅ **Production-ready**: Comprehensive error handling and testing

This design provides a solid foundation for a reliable, high-performance transaction system for the Metrix graph database.
