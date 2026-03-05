# Storage System

Metrix uses a custom **segment-based storage engine** optimized for graph workloads with efficient space management and fast access patterns.

## Segment Architecture

The storage is organized into fixed-size **segments** - contiguous blocks of disk space that can be allocated to different entity types.

```mermaid
graph LR
    subgraph "Database File"
        FH[File Header]
        ST[Segment Table]

        subgraph "Data Segments"
            S1[Segment 1<br/>Nodes]
            S2[Segment 2<br/>Edges]
            S3[Segment 3<br/>Properties]
            S4[Segment 4<br/>Free]
        end

        BM[Allocation Bitmap]
        IDX[Index Segment]
    end

    FH --> ST
    ST --> S1
    ST --> S2
    ST --> S3
    ST --> S4
    ST --> BM
    ST --> IDX

    style S4 fill:#bbf,stroke:#333
    style IDX fill:#fbf,stroke:#333
```

### Segment Structure

Each segment contains:

```
┌─────────────────────────────────┐
│ Segment Header                  │
│ - Segment ID                    │
│ - Type (Node/Edge/Property)     │
│ - Capacity                      │
│ - Used Count                    │
├─────────────────────────────────┤
│ Free Space Bitmap               │
│ - Tracks available slots        │
├─────────────────────────────────┤
│ Data Pages                      │
│ - Fixed-size records            │
│ - Compressed if enabled         │
└─────────────────────────────────┘
```

### Key Benefits

1. **Fast Allocation**: O(1) allocation using bitmap lookup
2. **Efficient Scans**: Sequential reads within segments
3. **Space Reclamation**: Entire segments can be freed
4. **Cache Friendly**: Predictable access patterns

## Storage Components

### FileHeaderManager

Manages file-level metadata:

- **Database Version**: Format identifier for compatibility
- **Segment Size**: Configurable segment size (default: 4MB)
- **Checksum**: Validates file integrity
- **Creation Time**: Database creation timestamp

### SegmentTracker

Tracks segment allocation:

```mermaid
graph TD
    A[SegmentTracker] --> B{Allocation Request}
    B -->|Free segment exists| C[Return Segment ID]
    B -->|No free segment| D[Grow File]
    D --> E[Allocate New Segment]
    E --> C

    C --> F[Mark as Used]
    F --> G[Update Bitmap]
```

**Responsibilities**:
- Track which segments are free/used
- Allocate new segments when needed
- Mark segments as free after deletion
- Maintain segment metadata

### DataManager

Handles node and edge storage:

#### Node Storage
- **Label Index**: Fast lookup of nodes by label
- **Properties**: Key-value pairs with type information
- **Relationships**: Links to incoming/outgoing edges

#### Edge Storage
- **Start Node**: Reference to source node
- **End Node**: Reference to target node
- **Type**: Relationship type label
- **Properties**: Key-value pairs

#### Property Storage
- **Type Codes**: Integer, String, Boolean, Float, etc.
- **Compression**: zlib compression for large values
- **Inline Storage**: Small values stored directly in record

### IndexManager

Manages two types of indexes:

#### Label Index
- **Structure**: Hash table mapping labels to node IDs
- **Lookup**: O(1) complexity for label-based queries
- **Updates**: Incremental updates on node creation/deletion

#### Property Index
- **B-Tree Index**: Ordered index for range queries
- **Hash Index**: Fast equality lookups
- **Composite Index**: Multi-column indexes (planned)

### DeletionManager

Tombstone-based deletion:

```mermaid
sequenceDiagram
    participant TX as Transaction
    participant DM as DeletionManager
    participant FS as FileStorage

    TX->>DM: delete(entity)
    DM->>FS: markTombstone(id)
    DM->>DM: addToFreeList()

    Note over DM: Space not immediately reclaimed

    DM->>DM: defragment()
    DM->>FS: reclaimSpace()
```

**Process**:
1. **Mark Tombstone**: Entity marked as deleted
2. **Free List**: Space added to reclamation list
3. **Defragmentation**: Background process reclaims space
4. **Reuse**: Reclaimed space available for new entities

### CacheManager

LRU cache with dirty tracking:

```mermaid
graph LR
    A[Read Request] --> B{Cache Hit?}
    B -->|Yes| C[Return Cached]
    B -->|No| D[Load from Disk]
    D --> E[Add to Cache]
    E --> F[Evict if Full]

    G[Write Request] --> H[Update Cache]
    H --> I[Mark Dirty]

    J[Commit] --> K[Flush Dirty]
    K --> L[Write to Disk]
```

**Features**:
- **LRU Eviction**: Least recently used entities evicted first
- **Dirty Tracking**: Modified entities tracked for persistence
- **Write-Back**: Dirty entities flushed on commit
- **Configurable Size**: Cache size limit in bytes

## Write-Ahead Log (WAL)

All modifications are logged before being applied to the main storage.

### WAL Flow

```mermaid
sequenceDiagram
    participant TX as Transaction
    participant WAL as Write-Ahead Log
    participant FS as FileStorage

    TX->>WAL: log(operation)
    WAL->>WAL: append(buffer)
    WAL->>WAL: fsync()

    Note over WAL: Durability guaranteed

    TX->>FS: apply(operation)
    TX->>TX: markComplete()

    TX->>TX: commit()
    TX->>WAL: checkpoint()
    WAL->>WAL: markCommitted()
```

### WAL Structure

```
┌─────────────────────────────────┐
│ WAL Header                      │
│ - Checkpoint ID                 │
│ - Log Sequence Number           │
├─────────────────────────────────┤
│ Log Entries                     │
│ ┌─────────────────────────────┐ │
│ │ Entry 1: CreateNode         │ │
│ │ - Transaction ID            │ │
│ │ - Node ID                   │ │
│ │ - Label                     │ │
│ │ - Properties                │ │
│ └─────────────────────────────┘ │
│ ┌─────────────────────────────┐ │
│ │ Entry 2: CreateRelationship │ │
│ │ - Transaction ID            │ │
│ │ - Edge ID                   │ │
│ │ - Start/End Nodes           │ │
│ └─────────────────────────────┘ │
├─────────────────────────────────┤
│ Checkpoint Marker               │
│ - Committed LSN                 │
│ - Timestamp                     │
└─────────────────────────────────┘
```

### WAL Benefits

1. **Atomicity**: Entire transaction can be replayed
2. **Durability**: Committed changes survive crashes
3. **Rollback**: Undo operations by reversing WAL entries
4. **Recovery**: Rebuild state after crash

### Checkpoint Process

```mermaid
graph TD
    A[Checkpoint Trigger] --> B[Flush WAL to Disk]
    B --> C[Apply Entries to Main Storage]
    C --> D[Mark Checkpoint in WAL]
    D --> E[Truncate Old WAL Entries]

    A --> F{Checkpoint Type}
    F -->|Full| G[Checkpoint All Data]
    F -->|Incremental| H[Checkpoint Dirty Data Only]
```

## State Management

### State Chains

Each entity maintains a chain of states for versioning:

```mermaid
graph LR
    S1[State v1<br/>Created] --> S2[State v2<br/>Modified]
    S2 --> S3[State v3<br/>Committed]

    S3 -.->|Rollback| S2
    S2 -.->|Rollback| S1

    style S1 fill:#dfd
    style S2 fill:#ffd
    style S3 fill:#ddf
```

### State Chain Benefits

- **MVCC**: Read operations don't block writes
- **Time Travel**: Query historical states
- **Rollback**: Undo without data loss
- **Conflict Detection**: Detect concurrent modifications

### State Transitions

```mermaid
stateDiagram-v2
    [*] --> Created: create()
    Created --> Modified: update()
    Modified --> Modified: update()
    Modified --> Committed: commit()
    Modified --> RolledBack: rollback()
    Committed --> [*]
    RolledBack --> [*]
```

## File Format

### Database File Layout

```
┌─────────────────────────────────┐
│ File Header (512 bytes)         │
│ - Magic Number                  │
│ - Version                       │
│ - Segment Size                  │
│ - Checksum                      │
├─────────────────────────────────┤
│ Segment Table                   │
│ - Segment Metadata              │
│ - Allocation Bitmaps            │
├─────────────────────────────────┤
│ Segment 0: Node Data            │
├─────────────────────────────────┤
│ Segment 1: Edge Data            │
├─────────────────────────────────┤
│ Segment 2: Property Data        │
├─────────────────────────────────┤
│ ... more segments ...           │
├─────────────────────────────────┤
│ Segment N: Index Data           │
└─────────────────────────────────┘
```

### WAL File Layout

```
┌─────────────────────────────────┐
│ WAL Header                      │
│ - Checkpoint ID                 │
│ - Start LSN                     │
├─────────────────────────────────┤
│ Log Record 1                    │
│ - Transaction ID                │
│ - Operation Type                │
│ - Operation Data                │
├─────────────────────────────────┤
│ Log Record 2                    │
├─────────────────────────────────┤
│ ... more records ...            │
├─────────────────────────────────┤
│ Checkpoint Marker               │
└─────────────────────────────────┘
```

## Compression

Metrix uses zlib compression for:

1. **Large Properties**: String and binary data > 1KB
2. **Segments**: Compress entire segments when idle
3. **WAL**: Compress old WAL entries

### Compression Strategy

```mermaid
graph TD
    A[Data to Write] --> B{Size > 1KB?}
    B -->|Yes| C[Compress with zlib]
    B -->|No| D[Store Uncompressed]
    C --> E{Compression Ratio > 50%?}
    E -->|Yes| F[Use Compressed Data]
    E -->|No| D
    D --> G[Write to Disk]
    F --> G
```

## Performance Characteristics

| Operation | Complexity | Notes |
|-----------|------------|-------|
| Create Node | O(1) | Direct segment allocation |
| Create Edge | O(1) | Link to existing nodes |
| Lookup by ID | O(1) | Direct offset calculation |
| Label Scan | O(n) | Scan all nodes with label |
| Property Query | O(1) | With index |
| Property Query | O(n) | Without index |
| Delete Node | O(1) | Tombstone marking |
| Compact Storage | O(n) | Background defragmentation |

## Recovery

### Crash Recovery

```mermaid
graph TD
    A[Database Open] --> B{WAL Exists?}
    B -->|No| C[Normal Open]
    B -->|Yes| D[Recover from WAL]
    D --> E[Replay Committed Transactions]
    E --> F[Rollback Uncommitted Transactions]
    F --> G[Checkpoint WAL]
    G --> C
```

### Recovery Steps

1. **Check WAL**: Determine if crash occurred
2. **Replay Log**: Apply committed transactions from WAL
3. **Rollback**: Undo uncommitted transactions
4. **Checkpoint**: Persist all changes to main storage
5. **Truncate**: Clear processed WAL entries

## Configuration

### Storage Parameters

```cpp
struct StorageConfig {
    size_t segmentSize = 4 * 1024 * 1024;  // 4MB segments
    size_t cacheSize = 256 * 1024 * 1024;  // 256MB cache
    bool compressionEnabled = true;
    size_t compressionThreshold = 1024;    // 1KB
    size_t walMaxSize = 100 * 1024 * 1024; // 100MB
};
```

### Tuning Guidelines

- **Segment Size**: Larger segments = less fragmentation
- **Cache Size**: More memory = faster reads
- **Compression**: Trade CPU for disk space
- **WAL Size**: Balance durability and disk usage

## Next Steps

- [Query Engine](/en/architecture/query-engine) - How queries are executed
- [Transactions](/en/architecture/transactions) - Transaction management details
- [API Reference](/en/api/cpp-api) - Storage API usage
