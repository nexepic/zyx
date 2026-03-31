# Test Performance Optimization & DataManager Decomposition

## Problem

Unit tests take 10-22 seconds each due to `Database::open()` being called per test case. Seven test suites
are affected (total ~1000 tests across them). `DataManager` has grown into a God-object with ~100 methods
and a 4,878-line test file.

## Root Cause Analysis

### Database::open() Cost Breakdown

Each `Database::open()` eagerly initializes the full stack (~80-120ms):

1. `FileStorage::open()` — file I/O, header validation
2. `IDAllocator::initialize()` — walks all 6 segment chains
3. `SegmentIndexManager::buildSegmentIndexes()` — **duplicate walk** of same 6 chains
4. `SystemConfigManager::loadAndApplyAll()` — state reads
5. `ThreadPool(N)` — spawns OS threads
6. `QueryEngine` — IndexManager, ConstraintManager, Optimizer, QueryExecutor
7. `WALManager::open()` — file creation + fsync
8. `TransactionManager` — construction

### Affected Tests

| Test File | Tests | Time | Uses QueryEngine? |
|---|---|---|---|
| `test_DataManager` | 212 | 22.23s | No |
| `test_IDAllocator` | 96 | 21.81s | No |
| `test_Api` | 161 | 18.23s | Yes |
| `test_IndexBuilder` | 31 | 15.99s | Partially |
| `test_ReadingClauseHandler` | 185 | 12.51s | Yes |
| `test_ResultClauseHandler` | 167 | 11.11s | Yes |
| `test_WritingClauseHandler` | 107 | 8.93s | Yes |

### DataManager Structure Problem

`DataManager` (1,641 lines) holds ~100 methods across 11 distinct responsibility groups:
- Lifecycle / init (5)
- Label registry (4)
- Observer/notification dispatch (11)
- Node CRUD + properties (13)
- Edge CRUD + properties (11)
- Property/Blob/Index/State ops (20)
- Generic template entity ops (7)
- Transaction context (3)
- Dirty tracking / persistence (9)
- Disk I/O / segment read (15)
- Cache management (1)

## Design

### Part 1: Lazy Initialization in Database::open()

Split initialization into essential (at open) vs deferrable (at first use):

**Essential — always at open():**
- `FileStorage::open()` — required for any data access
- `SystemConfigManager` — needed for config reads
- `IDAllocator` + `SegmentIndexManager` — **merged into single pass**

**Deferred — lazy on first use:**

| Component | Trigger | Mechanism |
|---|---|---|
| `ThreadPool` | First `beginTransaction()` or parallel flush | `std::call_once` in accessor |
| `QueryEngine` | First `execute()` call | `std::call_once` in `getQueryEngine()` |
| `WALManager` | First write operation | `std::call_once` in accessor |
| `TransactionManager` | First `beginTransaction()` | `std::call_once` alongside WALManager |

**Implementation approach:**
- Replace eager member construction with `std::unique_ptr` + `std::once_flag`
- Each `get*()` accessor calls `ensureInitialized()` which uses `std::call_once`
- `close()` checks and tears down only initialized components
- Thread safety via `std::call_once` guarantees

**Merged segment chain walk:**
- Create `StorageBootstrap::scanAllChains()` that walks each chain once
- Produces both `IDAllocator` gap data and `SegmentIndexManager` index entries
- Eliminates the duplicate traversal of all 6 entity type chains

### Part 2: DataManager Decomposition

Extract two classes with clear single-responsibility boundaries:

#### 2a. EntityObserverManager

**Extracted from DataManager:**
- `registerObserver(std::shared_ptr<IEntityObserver>)`
- `registerValidator(std::shared_ptr<IEntityValidator>)`
- All 9 `notify*()` methods (notifyNodeAdded, notifyNodesAdded, notifyNodeUpdated,
  notifyNodeDeleted, notifyEdgeAdded, notifyEdgesAdded, notifyEdgeUpdated,
  notifyEdgeDeleted, notifyStateUpdated)
- `suppressNotifications_` flag

**Interface:**
```cpp
class EntityObserverManager {
public:
    void registerObserver(std::shared_ptr<IEntityObserver> observer);
    void registerValidator(std::shared_ptr<IEntityValidator> validator);

    void notifyNodeAdded(const graph::Node &node);
    void notifyNodesAdded(const std::vector<graph::Node> &nodes);
    void notifyNodeUpdated(const graph::Node &node);
    void notifyNodeDeleted(int64_t nodeId);
    void notifyEdgeAdded(const graph::Edge &edge);
    void notifyEdgesAdded(const std::vector<graph::Edge> &edges);
    void notifyEdgeUpdated(const graph::Edge &edge);
    void notifyEdgeDeleted(int64_t edgeId);
    void notifyStateUpdated(const std::string &key, const graph::PropertyValue &value);

    void setSuppressNotifications(bool suppress);

private:
    std::vector<std::shared_ptr<IEntityObserver>> observers_;
    std::vector<std::shared_ptr<IEntityValidator>> validators_;
    bool suppressNotifications_ = false;
};
```

**DataManager integration:** Holds `EntityObserverManager observerManager_` member. CRUD methods
call `observerManager_.notify*()` instead of private `notify*()` methods.

**Files:**
- `include/graph/storage/data/EntityObserverManager.hpp`
- `src/storage/data/EntityObserverManager.cpp`

#### 2b. TransactionContext

**Extracted from DataManager:**
- `setActiveTransaction(int64_t txnId)`
- `clearActiveTransaction()`
- `rollbackActiveTransaction()` — the full rollback logic
- `activeTransactionId_` field
- `transactionOps_` journal

**Interface:**
```cpp
class TransactionContext {
public:
    void setActive(int64_t txnId);
    void clear();
    void rollback(DataManager &dataManager);

    [[nodiscard]] int64_t activeTransactionId() const;
    [[nodiscard]] bool hasActiveTransaction() const;

    void recordOp(TransactionOp op);

private:
    int64_t activeTransactionId_ = -1;
    std::vector<TransactionOp> transactionOps_;
};
```

**DataManager integration:** Holds `TransactionContext txnContext_` member. Transaction methods
delegate to it. `rollback()` takes a DataManager reference for entity restoration.

**Files:**
- `include/graph/storage/data/TransactionContext.hpp`
- `src/storage/data/TransactionContext.cpp`

### Part 3: Test File Split

Split `test_DataManager.cpp` (4,878 lines, 212 tests) into 7 focused files:

| New Test File | Responsibility | Est. Tests | Fixture |
|---|---|---|---|
| `test_DataManager_NodeEdge.cpp` | Node/Edge CRUD, properties, batch ops | ~35 | Full DB |
| `test_DataManager_EntityTypes.cpp` | Property/Blob/Index/State entity ops, getEntitiesInRange | ~70 | Full DB |
| `test_DataManager_DiskIO.cpp` | loadFromDisk, readEntityFromDisk, getEntityFromMemoryOrDisk | ~30 | Full DB |
| `test_DataManager_DirtyTracking.cpp` | setEntityDirty, getDirtyInfo variants, auto-flush | ~30 | Full DB |
| `test_DataManager_Transaction.cpp` | Transaction add/update/delete, rollback, WAL | ~17 | Full DB |
| `test_EntityObserverManager.cpp` | Observer notification, registration, suppression | ~7 | **Lightweight** |
| `test_TransactionContext.cpp` | Op journaling, rollback logic | ~10 | **Lightweight** |

**Lightweight fixtures** test the extracted classes directly without `Database::open()`, running
in under 100ms.

**Full DB fixtures** benefit from lazy init — storage-only tests skip QueryEngine/ThreadPool/WAL
construction, saving ~40-50ms per test.

## Expected Impact

| Test | Current | Expected |
|---|---|---|
| test_DataManager (all 7 files) | 22.23s | ~8-10s |
| test_IDAllocator | 21.81s | ~10-12s |
| test_Api | 18.23s | ~10-12s |
| test_IndexBuilder | 15.99s | ~6-8s |
| test_ReadingClauseHandler | 12.51s | ~8-10s |
| test_ResultClauseHandler | 11.11s | ~7-9s |
| test_WritingClauseHandler | 8.93s | ~5-7s |

## Non-Goals

- Full DataManager decomposition into 5+ classes (too risky, diminishing returns)
- Shared test fixtures across mutating tests (almost all tests mutate, sharing causes flakiness)
- ANTLR4 parser caching (ATN is already cached globally via `call_once`)
