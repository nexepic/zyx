# Storage Layer Optimization Design

**Date**: 2026-03-31
**Scope**: Page Buffer Pool + WAL Group Commit

## Overview

Two high-impact storage layer optimizations:

1. **Page Buffer Pool** — Replace 6 entity-level LRU caches with a unified segment-page buffer pool
2. **WAL Group Commit** — Batch WAL writes with proper fsync for throughput and durability

## 1. Page Buffer Pool

### Problem

Current architecture has 6 independent `LRUCache<int64_t, EntityType>` instances (node, edge, property, blob, index, state). Each caches deserialized entity objects keyed by entity ID.

Issues:
- **Memory inefficiency**: Entities in the same segment are cached independently; no spatial locality benefit
- **Cache fragmentation**: 6 separate pools with fixed-ratio capacities (property=2x, blob=0.25x) can't adapt to workload
- **Architectural complexity**: Every entity type needs its own cache management code path

### Design

#### PageBufferPool class

```
Location: include/graph/storage/PageBufferPool.hpp
Namespace: graph::storage
```

```cpp
struct Page {
    uint64_t segmentOffset;          // File offset of this segment
    std::vector<uint8_t> data;       // Raw segment bytes (TOTAL_SEGMENT_SIZE)
    bool valid = true;               // Invalidated on write
};

class PageBufferPool {
public:
    explicit PageBufferPool(size_t capacityPages);

    // Lookup a page by segment offset. Returns nullptr on miss.
    const Page* getPage(uint64_t segmentOffset) const;

    // Insert or replace a page. Evicts LRU page if full.
    void putPage(uint64_t segmentOffset, std::vector<uint8_t>&& data);

    // Invalidate a page (called after flush writes to that segment)
    void invalidate(uint64_t segmentOffset);

    // Invalidate all pages (called on rollback)
    void clear();

    size_t size() const;

private:
    size_t capacity_;
    // LRU implemented as list + unordered_map (same pattern as existing LRUCache)
    std::list<Page> pages_;
    std::unordered_map<uint64_t, std::list<Page>::iterator> pageMap_;
    mutable std::shared_mutex mutex_;
};
```

Key properties:
- Capacity in number of pages (each page = `TOTAL_SEGMENT_SIZE` bytes = currently 1152 bytes)
- Uses `segmentOffset` (uint64_t) as key — stable across entity count changes
- Dynamic entity parsing: reads `entitySize` and `used` from the segment header bytes within the cached page, not hardcoded
- Thread safety: `shared_mutex` with shared lock for reads, unique lock for writes

#### Entity Read Path Change

Current path in `DataManager::getEntityFromMemoryOrDisk()`:
```
dirty registry → entity LRU cache → disk pread → cache put
```

New path:
```
dirty registry → PageBufferPool lookup → deserialize from page → return
                     ↓ (miss)
              disk pread (full segment) → putPage → deserialize → return
```

On a page hit, the target entity is deserialized from the page's raw bytes using the segment header's `used` count and the entity's `getTotalSize()`. The offset within the page is computed as:
```
entityOffset = SEGMENT_HEADER_SIZE + (slotIndex * EntityType::getTotalSize())
```
where `slotIndex` is derived from the entity ID and the segment's ID range (from `SegmentIndexManager`).

#### Scan Path (Unchanged)

The parallel scan pipeline (`NodeScanOperator::parallelLoadBatch`) continues to use coalesced pread directly into temporary buffers. Scan results are NOT populated into the page pool to avoid cache thrashing.

#### Write/Flush Path

Entity writes continue through the dirty registry. On `FileStorage::save()` / flush:
1. Dirty entities are written to disk (existing behavior)
2. After successful write, call `pagePool_->invalidate(segmentOffset)` for each written segment
3. Next read of those entities will re-populate the pool from disk

This is simpler and safer than trying to update pages in-place.

#### Capacity Sizing

Default: `capacity = configuredCacheSize` (same total page count as the current entity cache size). Since one page holds multiple entities, the effective cache coverage is larger.

The pool uses a single capacity instead of per-type ratios. The LRU policy naturally adapts to the workload — if properties are accessed more frequently, more property pages stay in the pool.

#### Compatibility with Future Segment Size Changes

The design does NOT hardcode entities-per-segment. All parsing reads from the segment header within the cached page bytes:
- `header.used` — number of entities stored
- `EntityType::getTotalSize()` — compile-time entity size
- `SEGMENT_HEADER_SIZE` and `TOTAL_SEGMENT_SIZE` — from StorageHeaders.hpp constants

When segment sizes change (e.g., increasing `SEGMENT_SIZE` from 1024 to a larger value), the page pool automatically adapts because:
- Page size = `TOTAL_SEGMENT_SIZE` (computed from constants)
- Entity offset = header size + slot * entity size (computed dynamically)
- No cached metadata about entity counts

## 2. WAL Group Commit

### Problem

Current WAL implementation:
- Each `writeRecord()` does `seekp(end) + write()` — no batching
- `sync()` calls only `std::fstream::flush()` — no `fsync()`, no durability guarantee
- File I/O via `std::fstream` — no native file descriptor for `fsync`
- Concurrent small transactions each trigger individual writes

### Design

#### Architecture

```
Callers (writeBegin/writeEntityChange/writeCommit/writeRollback)
    ↓
WAL Write Buffer (in-memory std::vector<uint8_t>)
    ↓ (on commit or buffer full)
Group Commit: single write() + single fsync()
    ↓
Waiting transactions are notified
```

#### WALManager Changes

```
File: include/graph/storage/wal/WALManager.hpp
      src/storage/wal/WALManager.cpp
```

New private members:
```cpp
// Replace std::fstream with native fd for fsync support
file_handle_t walFd_ = INVALID_HANDLE;

// Write buffer
std::vector<uint8_t> writeBuffer_;
size_t walBufferSize_ = 65536;  // 64KB default

// Group commit coordination
std::mutex commitMutex_;
std::condition_variable commitCV_;
uint64_t lastSyncedOffset_ = 0;  // File offset up to which fsync has been done
uint64_t currentWriteOffset_ = 0; // Current end-of-file offset

// Group commit timing
uint32_t groupCommitDelayUs_ = 1000; // 1ms max wait
```

#### Write Path

1. `writeRecord()` appends the serialized record to `writeBuffer_` (under `commitMutex_`)
2. If `writeBuffer_.size() >= walBufferSize_`, flush buffer to file immediately (non-commit flush, no fsync)

#### Commit Path (Group Commit)

When `writeCommit(txnId)` is called:

1. Append COMMIT record to `writeBuffer_`
2. Record the target offset: `myOffset = currentWriteOffset_ + writeBuffer_.size()`
3. Check if another thread is already performing group commit:
   - **Yes**: Wait on `commitCV_` until `lastSyncedOffset_ >= myOffset`
   - **No**: Become the group commit leader:
     a. Wait up to `groupCommitDelayUs_` for more commits to accumulate
     b. `portable_pwrite(walFd_, writeBuffer_.data(), writeBuffer_.size(), currentWriteOffset_)`
     c. `portable_fsync(walFd_)`
     d. Update `lastSyncedOffset_`, `currentWriteOffset_`
     e. Clear `writeBuffer_`
     f. `commitCV_.notify_all()` — wake all waiting committers

#### Rollback Path

`writeRollback()` appends the ROLLBACK record to buffer and flushes (no fsync needed for rollback — advisory only).

#### Recovery Path

`readRecords()` reads from the file (not the buffer). Any un-fsynced data in the buffer is lost on crash — this is correct because those transactions hadn't committed durably.

#### Checkpoint

`checkpoint()`:
1. Flush any remaining buffer to file
2. `portable_fsync(walFd_)`
3. Close fd, truncate file, rewrite header, reopen

#### API Compatibility

Public API (`writeBegin`, `writeEntityChange`, `writeCommit`, `writeRollback`, `sync`, `checkpoint`, `readRecords`, `needsRecovery`) remains unchanged. Internal implementation changes are transparent to callers.

#### File Handle Migration

Replace `std::fstream walFile_` with `file_handle_t walFd_` using existing `portable_open_rw()` / `portable_pwrite()` / `portable_fsync()` from `PwriteHelper.hpp` and `portable_pread()` from `PreadHelper.hpp` for reads.

This aligns WAL I/O with the pattern already used by `FileStorage` for the main database file.

## Performance Measurement

### Benchmarks to Run Before/After

Use the existing benchmark suite (`buildDir/apps/benchmark/zyx-bench`):

| Benchmark | Measures | Expected Impact |
|-----------|----------|-----------------|
| `insert.native.single.*` | Single insert throughput | WAL group commit: significant improvement |
| `insert.native.batch.*` | Batch insert throughput | WAL group commit: moderate improvement |
| `transaction.explicit_insert` | Transaction overhead | WAL group commit: significant improvement |
| `transaction.explicit_batch_insert` | Batch txn throughput | WAL group commit: significant improvement |
| `query.cypher.property_index` | Index lookup latency | Page pool: improvement on repeated queries |
| `query.cypher.full_scan` | Full scan throughput | No change expected (scan path unchanged) |

### Measurement Protocol

1. Build release: `./scripts/build_release.sh`
2. Run baseline: `./buildDir/apps/benchmark/zyx-bench --type insert,query,transaction`
3. Apply optimizations
4. Run again with same parameters
5. Compare ops/sec, avg latency, p99 latency

## Implementation Order

1. **Page Buffer Pool** (lower risk, self-contained):
   - Create `PageBufferPool` class
   - Modify `DataManager` read path to use pool
   - Add invalidation on flush
   - Remove 6 entity-level caches
   - Update tests

2. **WAL Group Commit** (higher complexity):
   - Migrate WAL to native file descriptor
   - Add write buffer
   - Implement group commit protocol
   - Add proper fsync
   - Update tests

## Files Modified

### Page Buffer Pool
- **New**: `include/graph/storage/PageBufferPool.hpp`
- **Modified**: `include/graph/storage/data/DataManager.hpp` — replace 6 caches with `PageBufferPool*`
- **Modified**: `src/storage/data/DataManager.cpp` — entity read path, cache invalidation on flush
- **Modified**: `src/storage/FileStorage.cpp` — pass page pool to DataManager, invalidation on save
- **Modified**: `include/graph/storage/FileStorage.hpp` — hold `PageBufferPool` instance
- **Removed/Simplified**: Entity-specific cache operations throughout DataManager
- **New**: `tests/src/storage/test_PageBufferPool.cpp`

### WAL Group Commit
- **Modified**: `include/graph/storage/wal/WALManager.hpp` — new members, native fd
- **Modified**: `src/storage/wal/WALManager.cpp` — write buffer, group commit, fsync
- **Modified**: `tests/src/storage/test_WALManager.cpp` — group commit tests
