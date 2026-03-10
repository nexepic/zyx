# WAL Recovery Algorithm

The Write-Ahead Log (WAL) recovery algorithm ensures database consistency after system crashes by replaying committed transactions and rolling back uncommitted ones.

## Overview

WAL recovery operates in three phases:

1. **Analysis Phase**: Scan WAL to identify transactions
2. **Redo Phase**: Replay committed transactions
3. **Undo Phase**: Rollback uncommitted transactions

## WAL Structure

### Log Record Format

```cpp
struct WALRecord {
    uint64_t lsn;              // Log Sequence Number
    uint64_t transactionId;
    WALRecordType type;
    uint64_t timestamp;
    uint32_t size;
    uint32_t checksum;
    uint8_t data[];
};
```

### Record Types

```cpp
enum class WALRecordType : uint8_t {
    BEGIN_TX        = 0x01,
    COMMIT_TX       = 0x02,
    ROLLBACK_TX     = 0x03,
    CREATE_NODE     = 0x10,
    CREATE_EDGE     = 0x11,
    UPDATE_NODE     = 0x12,
    UPDATE_EDGE     = 0x13,
    DELETE_NODE     = 0x14,
    DELETE_EDGE     = 0x15,
    CHECKPOINT      = 0x20
};
```

## Recovery Algorithm

### Phase 1: Analysis

Scan WAL to build transaction table:

```cpp
struct TransactionInfo {
    uint64_t transactionId;
    bool hasCommitRecord;
    uint64_t firstLsn;
    uint64_t lastLsn;
    std::vector<uint64_t> recordLsns;
};

std::map<uint64_t, TransactionInfo> analyzeWAL(WAL* wal) {
    std::map<uint64_t, TransactionInfo> transactions;

    // Scan all WAL records
    auto records = wal->scanAllRecords();

    for (auto* record : records) {
        switch (record->type) {
            case WALRecordType::BEGIN_TX:
                transactions[record->transactionId] = {
                    .transactionId = record->transactionId,
                    .hasCommitRecord = false,
                    .firstLsn = record->lsn,
                    .lastLsn = record->lsn,
                    .recordLsns = {record->lsn}
                };
                break;

            case WALRecordType::COMMIT_TX:
                transactions[record->transactionId].hasCommitRecord = true;
                transactions[record->transactionId].lastLsn = record->lsn;
                break;

            default:
                // Data operations
                auto& tx = transactions[record->transactionId];
                tx.recordLsns.push_back(record->lsn);
                tx.lastLsn = record->lsn;
                break;
        }
    }

    return transactions;
}
```

**Complexity**: O(n) where n is number of WAL records

### Phase 2: Redo

Replay committed transactions:

```cpp
void redoTransactions(Database* db,
                      const std::map<uint64_t, TransactionInfo>& transactions) {
    for (const auto& [txId, txInfo] : transactions) {
        if (!txInfo.hasCommitRecord) {
            continue;  // Skip uncommitted transactions
        }

        // Replay operations in LSN order
        for (uint64_t lsn : txInfo.recordLsns) {
            WALRecord* record = loadRecord(lsn);
            redoOperation(db, record);
        }
    }
}

void redoOperation(Database* db, WALRecord* record) {
    switch (record->type) {
        case WALRecordType::CREATE_NODE:
            redoCreateNode(db, record);
            break;
        case WALRecordType::CREATE_EDGE:
            redoCreateEdge(db, record);
            break;
        case WALRecordType::UPDATE_NODE:
            redoUpdateNode(db, record);
            break;
        case WALRecordType::DELETE_NODE:
            redoDeleteNode(db, record);
            break;
        // ... other operations
    }
}
```

**Complexity**: O(m) where m is number of committed operations

### Phase 3: Undo

Rollback uncommitted transactions:

```cpp
void undoTransactions(Database* db,
                      const std::map<uint64_t, TransactionInfo>& transactions) {
    for (const auto& [txId, txInfo] : transactions) {
        if (txInfo.hasCommitRecord) {
            continue;  // Skip committed transactions
        }

        // Undo operations in reverse LSN order
        for (auto it = txInfo.recordLsns.rbegin();
             it != txInfo.recordLsns.rend(); ++it) {
            WALRecord* record = loadRecord(*it);
            undoOperation(db, record);
        }
    }
}

void undoOperation(Database* db, WALRecord* record) {
    switch (record->type) {
        case WALRecordType::CREATE_NODE:
            undoCreateNode(db, record);  // Delete the node
            break;
        case WALRecordType::DELETE_NODE:
            undoDeleteNode(db, record);  // Restore the node
            break;
        // ... other operations
    }
}
```

**Complexity**: O(u) where u is number of uncommitted operations

## Optimizations

### Checkpointing

Reduce recovery time by checkpointing:

```cpp
void checkpoint(Database* db) {
    // 1. Write checkpoint record
    WALRecord checkpointRecord;
    checkpointRecord.type = WALRecordType::CHECKPOINT;
    checkpointRecord.lsn = wal->getNextLSN();
    wal->writeRecord(&checkpointRecord);
    wal->flush();

    // 2. Flush all dirty pages to disk
    db->flushDirtyPages();

    // 3. Truncate WAL before checkpoint LSN
    wal->truncate(checkpointRecord.lsn);
}
```

**Benefit**: Only replay WAL records after checkpoint

### Fuzzy Checkpointing

Allow concurrent activity during checkpoint:

```cpp
void fuzzyCheckpoint(Database* db) {
    // 1. Stop all updates briefly
    std::lock_guard<std::mutex> lock(checkpointMutex);

    // 2. Record current LSN
    uint64_t checkpointLSN = wal->getCurrentLSN();

    // 3. Flush dirty pages (concurrent updates allowed)
    db->flushDirtyPagesAsync();

    // 4. Write checkpoint record
    WALRecord checkpointRecord;
    checkpointRecord.type = WALRecordType::CHECKPOINT;
    checkpointRecord.lsn = checkpointLSN;
    wal->writeRecord(&checkpointRecord);
    wal->flush();
}
```

### Parallel Recovery

Recover multiple transactions in parallel:

```cpp
void parallelRecovery(Database* db,
                      const std::map<uint64_t, TransactionInfo>& transactions) {
    std::vector<std::thread> workers;

    for (const auto& [txId, txInfo] : transactions) {
        if (txInfo.hasCommitRecord) {
            workers.emplace_back([&, txId]() {
                recoverTransaction(db, txId);
            });
        }
    }

    for (auto& worker : workers) {
        worker.join();
    }
}
```

## Error Handling

### Checksum Validation

```cpp
bool validateRecord(WALRecord* record) {
    uint32_t computed = computeChecksum(record);
    return computed == record->checksum;
}

void handleCorruptRecord(WALRecord* record) {
    if (!validateRecord(record)) {
        // Option 1: Stop recovery and report error
        throw WALCorruptionError(record->lsn);

        // Option 2: Skip corrupt record and continue
        LOG_ERROR("Corrupt WAL record at LSN: " << record->lsn);
    }
}
```

### Partial Recovery

```cpp
void partialRecovery(Database* db) {
    try {
        recoverFromWAL(db);
    } catch (const WALCorruptionError& e) {
        LOG_WARNING("WAL corruption detected, performing partial recovery");

        // Recover up to the point of corruption
        recoverPartial(db, e.getLSN());

        // Truncate WAL at corruption point
        wal->truncate(e.getLSN());
    }
}
```

## Performance Characteristics

### Time Complexity

| Phase | Complexity |
|-------|------------|
| Analysis | O(n) |
| Redo | O(m) |
| Undo | O(u) |
| **Total** | O(n) |

Where:
- n = total WAL records
- m = committed operations
- u = uncommitted operations

### Space Complexity

- **Transaction Table**: O(t) where t is number of transactions
- **Record Cache**: O(k) where k is cached records

### Recovery Time

Recovery time is proportional to:

- WAL size (number of records)
- I/O bandwidth (disk speed)
- Checkpoint frequency

**Typical recovery times**:
- Small database (< 1 GB WAL): < 1 second
- Medium database (1-10 GB WAL): 1-10 seconds
- Large database (> 10 GB WAL): 10+ seconds

## Best Practices

1. **Regular checkpoints**: Reduce WAL size and recovery time
2. **Async flushing**: Improve performance during recovery
3. **Validate checksums**: Detect corruption early
4. **Monitor WAL size**: Prevent unbounded growth
5. **Test recovery**: Regularly test crash recovery

## See Also

- [WAL Implementation](/en/architecture/wal) - WAL architecture details
- [Transaction Management](/en/architecture/transactions) - Transaction system
- [Storage System](/en/architecture/storage) - Persistent storage
