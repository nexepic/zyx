# Concurrency Optimization Design: LRU Cache Sharding & Read-Write Transaction Separation

**Date**: 2026-03-22
**Status**: Approved
**Scope**: P0 concurrency optimizations for ZYX graph database

## 1. Problem Statement

ZYX is an embeddable graph database with a primary single-threaded use case. However, when users need multi-threaded access (e.g., concurrent read queries from a web server), the current architecture has two bottlenecks:

1. **LRU Cache** (`CacheManager.hpp`): Completely non-thread-safe. No locks. Concurrent access causes data races.
2. **TransactionManager**: Uses a global `writeMutex_` that serializes ALL transactions, including read-only queries. Even `MATCH ... RETURN` blocks other queries.

## 2. Design Goals

- **Zero overhead in single-threaded mode** (default): No locks, no thread pool, identical behavior to current code
- **Opt-in concurrency** via a single config: `concurrency.threads`
- **Production-grade stability**: No data races, no deadlocks, proper ACID guarantees under concurrency
- **Future extensibility**: The ThreadPool and config model serve all future concurrency optimizations (P1/P2)
- **Benchmarkable**: Quantitative before/after comparison with configurable thread counts

## 3. Configuration Design

### 3.1 Core Config: `concurrency.threads`

| Value | Behavior |
|-------|----------|
| `1` (default) | Single-threaded mode. No locks, no ThreadPool. Identical to current behavior. |
| `0` | Auto-detect: `std::thread::hardware_concurrency()` |
| `N` (>1) | Enable N-thread concurrency. Activates ConcurrentLRUCache sharding, read-write transaction separation, and creates a global ThreadPool. |

**Rationale**: A single config item encodes both "whether" and "how much" concurrency. No separate `mode` switch avoids contradictory states (e.g., `mode=multi` + `threads=1`). This follows the convention of PostgreSQL (`max_worker_processes`), RocksDB (`max_background_jobs`).

### 3.2 SystemStateKeys Integration

```cpp
// SystemStateKeys.hpp
namespace Config {
    constexpr char CONCURRENCY_THREADS[] = "concurrency.threads";
}
```

### 3.3 Lifecycle

- Read once during `Database::open()`. **Not dynamically reloadable** (changing cache shard count or ThreadPool size requires data structure rebuilding).
- If modified via `CALL dbms.setConfig('concurrency.threads', 4)`, takes effect on next `Database::open()`.
- `SystemConfigManager::onStateUpdated()` logs a warning: "concurrency.threads change will take effect after restart."

### 3.4 Database Constructor

```cpp
Database::Database(const std::string &dbPath,
                   OpenMode mode = OpenMode::OPEN_CREATE_OR_OPEN_FILE,
                   size_t cacheSize = 10000,
                   size_t concurrencyThreads = 1);
```

The `concurrencyThreads` parameter is propagated to:
- `DataManager` (determines cache shard count)
- `TransactionManager` (determines locking strategy)
- Creates `ThreadPool` if > 1

## 4. ConcurrentLRUCache Design

### 4.1 Architecture

A sharded wrapper around the existing `LRUCache<K,V>`. Each shard has its own `LRUCache` instance and `std::shared_mutex`.

```
ConcurrentLRUCache<K, V>
├── Shard 0: { LRUCache<K,V>(capacity/N), shared_mutex }
├── Shard 1: { LRUCache<K,V>(capacity/N), shared_mutex }
├── ...
└── Shard N-1: { LRUCache<K,V>(capacity/N), shared_mutex }

Routing: shard_index = hash(key) & (shardCount - 1)
```

### 4.2 Shard Count

`shardCount = nextPowerOf2(concurrencyThreads)` for efficient bitwise AND routing.

- `threads=1` → `shardCount=1` (single shard, minimal overhead)
- `threads=4` → `shardCount=4`
- `threads=6` → `shardCount=8`

### 4.3 API

```cpp
template<typename K, typename V>
class ConcurrentLRUCache {
public:
    ConcurrentLRUCache(size_t totalCapacity, size_t shardCount = 1);

    V get(const K& key);                    // unique_lock (modifies LRU order)
    V peek(const K& key) const;             // shared_lock (true read-only)
    void put(const K& key, const V& value); // unique_lock
    void remove(const K& key);              // unique_lock
    bool contains(const K& key) const;      // shared_lock
    void clear();                           // unique_lock on all shards
    size_t size() const;                    // lock-free sum

    // Iterator support for compatibility
    // Note: iteration requires locking ALL shards, use sparingly
};
```

### 4.4 Locking Strategy

| Operation | Lock Type | Reason |
|-----------|-----------|--------|
| `get()` | `unique_lock` | Moves node to front of LRU list |
| `peek()` | `shared_lock` | Pure read, no LRU update |
| `put()` | `unique_lock` | Modifies cache data and LRU list |
| `remove()` | `unique_lock` | Modifies cache data |
| `contains()` | `shared_lock` | Pure read |
| `clear()` | `unique_lock` (all shards) | Bulk modification |
| `size()` | Lock-free | Each shard's size is `atomic`, summed without locks |

### 4.5 DataManager Integration

Replace all `LRUCache` fields in `DataManager` with `ConcurrentLRUCache`:

```cpp
// Before
mutable LRUCache<int64_t, Node> nodeCache_;

// After
mutable ConcurrentLRUCache<int64_t, Node> nodeCache_;
```

Constructor passes `shardCount`:

```cpp
DataManager::DataManager(..., size_t concurrencyThreads)
    : nodeCache_(cacheSize, nextPowerOf2(concurrencyThreads)),
      edgeCache_(cacheSize, nextPowerOf2(concurrencyThreads)),
      // ...
```

## 5. Read-Write Transaction Separation

### 5.1 Transaction Changes

```cpp
class Transaction {
    // ... existing ...
    bool readOnly_ = false;  // New field

public:
    [[nodiscard]] bool isReadOnly() const { return readOnly_; }
};
```

### 5.2 TransactionManager Changes

Replace `std::mutex writeMutex_` with `std::shared_mutex rwMutex_` (only used when `threads > 1`).

```cpp
class TransactionManager {
    // Concurrency-aware locking
    std::mutex singleMutex_;           // Used when threads == 1 (original behavior)
    std::shared_mutex rwMutex_;        // Used when threads > 1
    size_t concurrencyThreads_ = 1;

    // Lock holders (only one active at a time per transaction)
    std::unique_lock<std::mutex> singleLock_;
    std::shared_lock<std::shared_mutex> readLock_;
    std::unique_lock<std::shared_mutex> writeLock_;

    std::atomic<int> activeReaders_{0};  // For diagnostics/monitoring
};
```

### 5.3 Locking Logic

```
begin(readOnly=false):
    if threads == 1:
        acquire unique_lock(singleMutex_)     // Original behavior
    else:
        acquire unique_lock(rwMutex_)          // Blocks all readers and writers

begin(readOnly=true):
    if threads == 1:
        acquire unique_lock(singleMutex_)     // Original behavior (still serialized)
    else:
        acquire shared_lock(rwMutex_)          // Multiple readers can proceed
        activeReaders_++

commit/rollback:
    if readOnly:
        release shared_lock
        activeReaders_--
    else:
        // Write WAL, save, checkpoint (existing logic)
        release unique/single lock
```

### 5.4 Read Transaction Behavior

- **No WAL writes**: No `writeBegin()`, `writeCommit()`, or `writeRollback()` for read transactions
- **No save()**: Read transactions never call `storage_->save()`
- **No dirty tracking**: Read transactions never modify `DirtyEntityRegistry`
- **No DataManager transaction context**: `dm->setActiveTransaction()` is skipped for read transactions

### 5.5 Implicit Read Transactions

When `QueryEngine::execute()` processes a query:

1. Parse the Cypher statement
2. Analyze the plan: if it contains **only** read operators (NodeScan, Filter, Sort, Project, Limit, Skip, Aggregate, VectorSearch, AlgoShortestPath), classify as read-only
3. If `threads > 1` and query is read-only: wrap in implicit `beginTransaction(readOnly=true)` / `commit()`
4. If query has write operators (Create, Delete, Set, Merge, Remove): use `beginTransaction(readOnly=false)`

### 5.6 Concurrent Read Safety

When multiple read transactions execute concurrently, they access:

| Component | Current Safety | Action Needed |
|-----------|---------------|---------------|
| ConcurrentLRUCache | Safe (sharded + per-shard mutex) | None |
| DirtyEntityRegistry | Safe (has `shared_mutex`) | None |
| SegmentTracker | **NOT safe** (no synchronization) | Add `mutable shared_mutex` for read paths |
| SegmentIndexManager | **NOT safe** | Add `mutable shared_mutex` for lookups |
| LabelTokenRegistry | **NOT safe** | Add `mutable shared_mutex` for reads |
| fstream reads | **NOT safe** (single stream, seekg races) | Protected by SegmentTracker's lock |

### 5.7 SegmentTracker Synchronization

```cpp
class SegmentTracker {
    // ... existing ...
    mutable std::shared_mutex mutex_;  // New: protects all data access

public:
    // Read operations use shared_lock
    SegmentHeader getSegmentHeader(uint64_t offset) const;  // shared_lock
    bool getBitmapBit(uint64_t offset, uint64_t idx) const; // shared_lock

    // Write operations use unique_lock
    void updateSegmentHeader(uint64_t offset, ...);         // unique_lock
    void setEntityActive(uint64_t offset, ...);             // unique_lock
};
```

## 6. ThreadPool Design

### 6.1 Minimal ThreadPool

A simple, production-grade thread pool for the global concurrency facility:

```cpp
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads);
    ~ThreadPool();  // Joins all threads

    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

    size_t size() const;
    void shutdown();

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queueMutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_{false};
};
```

### 6.2 Ownership

`Database` owns the `ThreadPool`. Created during `Database::open()` if `concurrencyThreads > 1`. Destroyed during `Database::close()`.

```cpp
class Database {
    // ... existing ...
    std::unique_ptr<ThreadPool> threadPool_;  // nullptr when threads == 1
    size_t concurrencyThreads_ = 1;
};
```

## 7. Benchmark Design

### 7.1 New File: `BenchmarkConcurrency.hpp`

#### Test Cases

| Name | Description | Threads | Measurement |
|------|-------------|---------|-------------|
| `SingleThread Baseline` | Sequential read queries | 1 | Ops/sec, latency |
| `ConcurrentRead (Nt)` | N threads doing parallel MATCH queries | 2, 4, 8 | Total ops/sec, P99 |
| `ReadUnderWrite` | 1 writer + N readers | N+1 | Reader throughput under write load |
| `CacheContention` | N threads hitting same cache entries | 2, 4, 8 | Throughput degradation |

#### Benchmark Framework Extension

```cpp
class ConcurrentBenchmarkBase : public BenchmarkBase {
protected:
    size_t numThreads_;

    // Override execute() to run `run()` across multiple threads
    Metrics execute() override {
        // 1. Setup with single-threaded DB
        // 2. Close and reopen with concurrencyThreads = numThreads_
        // 3. Spawn numThreads_ threads, each calling run() for iterations/numThreads_
        // 4. Barrier-sync start for accurate timing
        // 5. Collect per-thread latencies, merge, compute aggregate metrics
    }
};
```

#### Key Metrics

- **Throughput scaling**: `ops_at_N_threads / ops_at_1_thread` — ideal = N
- **Latency impact**: P99 at N threads vs P99 at 1 thread
- **Zero-overhead verification**: 1-thread benchmark MUST show no regression vs original (no-concurrency) build

### 7.2 Benchmark Output Format

```
+------------------------------------+----------------+----------------+----------------+----------------+
| Benchmark Name                     | Ops/Sec        | Avg(us)        | P99(us)        | Scaling        |
+------------------------------------+----------------+----------------+----------------+----------------+
| Read Baseline (1 thread)           | 150000.00      | 6.67           | 12.50          | 1.00x          |
| ConcurrentRead (2 threads)         | 280000.00      | 7.14           | 15.20          | 1.87x          |
| ConcurrentRead (4 threads)         | 520000.00      | 7.69           | 18.30          | 3.47x          |
| ConcurrentRead (8 threads)         | 850000.00      | 9.41           | 25.60          | 5.67x          |
+------------------------------------+----------------+----------------+----------------+----------------+
```

## 8. File Change Summary

| File | Change |
|------|--------|
| `include/graph/storage/ConcurrentLRUCache.hpp` | **NEW**: Sharded concurrent cache |
| `include/graph/concurrency/ThreadPool.hpp` | **NEW**: Global thread pool |
| `include/graph/storage/CacheManager.hpp` | Unchanged (reused internally by ConcurrentLRUCache) |
| `include/graph/storage/data/DataManager.hpp` | `LRUCache` → `ConcurrentLRUCache`, add `concurrencyThreads` param |
| `src/storage/data/DataManager.cpp` | Pass shard count to cache constructors |
| `include/graph/core/TransactionManager.hpp` | Add `shared_mutex`, `readOnly` support, `concurrencyThreads` |
| `src/core/TransactionManager.cpp` | Implement read-write separation logic |
| `include/graph/core/Transaction.hpp` | Add `readOnly_` field and `isReadOnly()` |
| `include/graph/core/Database.hpp` | Add `concurrencyThreads` constructor param, `ThreadPool` member |
| `src/core/Database.cpp` | Propagate threads to components, create ThreadPool |
| `include/graph/storage/state/SystemStateKeys.hpp` | Add `CONCURRENCY_THREADS` |
| `src/config/SystemConfigManager.cpp` | Add `applyConcurrency()` |
| `include/graph/storage/SegmentTracker.hpp` | Add `mutable shared_mutex` for concurrent reads |
| `src/storage/SegmentTracker.cpp` | Add locking to read/write paths |
| `include/graph/storage/SegmentIndexManager.hpp` | Add `mutable shared_mutex` for concurrent lookups |
| `src/storage/SegmentIndexManager.cpp` | Add locking |
| `include/graph/query/api/QueryEngine.hpp` | Add read-only query detection |
| `src/query/api/QueryEngine.cpp` | Implicit read transaction wrapping |
| `apps/benchmark/BenchmarkConcurrency.hpp` | **NEW**: Concurrent benchmark cases |
| `apps/benchmark/main.cpp` | Register concurrent benchmarks |
| `tests/src/concurrency/test_ThreadPool.cpp` | **NEW**: ThreadPool unit tests |
| `tests/src/storage/test_ConcurrentLRUCache.cpp` | **NEW**: Concurrent cache tests |
| `tests/src/concurrency/test_ReadWriteTransaction.cpp` | **NEW**: Read-write separation tests |

## 9. Testing Strategy

### Unit Tests

1. **ConcurrentLRUCache**: Single-shard correctness, multi-shard distribution, concurrent read/write stress, eviction under contention
2. **ThreadPool**: Submit/future, shutdown, exception propagation
3. **TransactionManager**: Read-read concurrency, read-write blocking, write-write serialization, single-thread mode unchanged

### Integration Tests

1. Multiple threads executing read queries concurrently
2. Reader threads not blocked by other readers
3. Writer blocks all readers, readers resume after commit
4. Single-thread mode produces identical results to current behavior

### Benchmark Validation

- 1-thread mode shows <1% regression vs baseline (zero-overhead goal)
- N-thread read throughput scales sub-linearly (expected: ~0.7-0.9 efficiency per thread)
