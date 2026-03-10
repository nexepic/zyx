# System Configuration

Metrix provides comprehensive configuration options to optimize performance and behavior for your specific workload.

## Configuration Overview

Configuration can be set through:
1. **Programmatic API**: C++ API methods
2. **Configuration File**: JSON-based config file
3. **Environment Variables**: Override settings
4. **Command-Line Options**: CLI tool arguments

## Storage Configuration

### Segment Size

Configure the size of storage segments:

```cpp
struct StorageConfig {
    size_t segmentSize = 4 * 1024 * 1024;  // 4 MB default
};
```

**Guidelines**:
- **Small segments (1-4 MB)**: Better for write-intensive workloads
- **Large segments (16-64 MB)**: Better for read-intensive workloads
- **Default**: 4 MB balances both scenarios

### Cache Size

Control the in-memory cache size:

```cpp
struct CacheConfig {
    size_t maxCacheSize = 256 * 1024 * 1024;  // 256 MB default
    size_t maxEntities = 100000;               // Max entities to cache
};
```

**Tuning Guidelines**:
- **Available RAM**: Allocate 50-70% of available memory
- **Working Set**: Size should exceed hot data set
- **Monitoring**: Target >80% cache hit rate

### WAL Configuration

Write-Ahead Log settings:

```cpp
struct WALConfig {
    size_t maxWalSize = 100 * 1024 * 1024;     // 100 MB default
    Duration checkpointInterval = Duration::minutes(5);
    bool fsyncOnCommit = true;                  // Durability vs performance
};
```

**Trade-offs**:
- **fsyncOnCommit = true**: Maximum durability, slower writes
- **fsyncOnCommit = false**: Faster writes, risk of data loss on crash
- **Larger WAL**: Fewer checkpoints, longer recovery time

## Transaction Configuration

### Isolation Levels

Set transaction isolation:

```cpp
enum class IsolationLevel {
    ReadCommitted,  // Default: See committed data
    Serializable     // Planned: Full isolation
};

auto tx = db->beginTransaction(IsolationLevel::ReadCommitted);
```

### Transaction Timeout

Prevent long-running transactions:

```cpp
struct TransactionConfig {
    std::chrono::milliseconds timeout = 30000ms;  // 30 seconds
    size_t maxRetries = 3;                        // Conflict retry count
};
```

### Lock Management

Optimistic concurrency control settings:

```cpp
struct ConcurrencyConfig {
    bool enableConflictDetection = true;
    size_t maxVersionsPerEntity = 10;             // MVCC version limit
};
```

## Query Engine Configuration

### Query Optimization

Control query planning and optimization:

```cpp
struct QueryConfig {
    bool enableOptimization = true;               // Enable query optimizer
    bool enableIndexScan = true;                  // Use indexes when available
    size_t maxQueryMemory = 1024 * 1024 * 1024;  // 1 GB query memory limit
};
```

### Parallel Execution

Configure parallel query execution:

```cpp
struct ParallelConfig {
    size_t maxThreads = std::thread::hardware_concurrency();
    size_t minRowsForParallel = 10000;           // Minimum rows for parallel scan
};
```

## Index Configuration

### Index Types

Configure different index strategies:

```cpp
enum class IndexType {
    HASH,          // O(1) lookup, equality only
    BTREE,         // O(log n) lookup, range queries
    VECTOR         // Vector similarity search
};
```

### Index Building

Control index creation:

```cpp
struct IndexConfig {
    bool buildInBackground = true;
    size_t indexBuildMemory = 512 * 1024 * 1024;  // 512 MB
};
```

## Compression Configuration

### Data Compression

Enable compression for storage:

```cpp
struct CompressionConfig {
    bool enabled = true;
    CompressionType type = CompressionType::ZLIB;
    size_t threshold = 1024;                       // Compress data > 1 KB
    CompressionLevel level = CompressionLevel::Default;
};
```

**Compression Types**:
- **ZLIB**: Good compression ratio, moderate speed
- **LZ4**: Fast compression, moderate ratio
- **ZSTD**: Best ratio, slower compression

### Compression Levels

```cpp
enum class CompressionLevel {
    Fast,       // Lowest compression, fastest
    Default,    // Balanced
    Maximum     // Highest compression, slowest
};
```

## Logging Configuration

### Log Levels

Control verbosity:

```cpp
enum class LogLevel {
    Debug,
    Info,       // Default
    Warning,
    Error
};
```

### Log Output

Configure logging destinations:

```cpp
struct LogConfig {
    LogLevel level = LogLevel::Info;
    bool logToFile = false;
    std::string logFilePath = "/var/log/metrix.log";
    bool logToConsole = true;
    bool logTransactions = false;    // Detailed transaction logging
};
```

## Performance Tuning

### Memory Management

```cpp
struct MemoryConfig {
    size_t maxMemory = 2 * 1024 * 1024 * 1024;  // 2 GB limit
    bool enableMemoryPooling = true;
    size_t poolBlockSize = 4 * 1024 * 1024;      // 4 MB pools
};
```

### I/O Optimization

```cpp
struct IOConfig {
    size_t pageSize = 4096;                       // System page size
    size_t readAheadSize = 64 * 1024;            // 64 KB read-ahead
    bool enableDirectIO = false;                  // Bypass OS cache
};
```

## Configuration File Format

Use JSON configuration files:

```json
{
  "storage": {
    "segmentSize": 4194304,
    "cacheSize": 268435456,
    "walSize": 104857600
  },
  "transactions": {
    "isolationLevel": "ReadCommitted",
    "timeoutMs": 30000,
    "maxRetries": 3
  },
  "query": {
    "enableOptimization": true,
    "maxThreads": 8
  },
  "compression": {
    "enabled": true,
    "type": "ZLIB",
    "threshold": 1024
  },
  "logging": {
    "level": "Info",
    "logToFile": true,
    "logFilePath": "/var/log/metrix.log"
  }
}
```

Load configuration:

```cpp
auto db = Database::open("./mydb", "./config.json");
```

## Environment Variables

Override configuration with environment variables:

```bash
export METRIX_CACHE_SIZE=536870912      # 512 MB
export METRIX_LOG_LEVEL=Debug
export METRIX_WAL_SIZE=209715200        # 200 MB
export METRIX_MAX_THREADS=16
```

## Runtime Reconfiguration

Some settings can be changed at runtime:

```cpp
// Update cache size
db->setCacheSize(newSize);

// Change log level
db->setLogLevel(LogLevel::Debug);

// Enable/disable query optimization
db->setQueryOptimization(enabled);
```

## Configuration Best Practices

1. **Start with defaults**: Use default settings initially
2. **Monitor performance**: Track metrics before tuning
3. **Change one setting at a time**: Isolate effects of changes
4. **Test thoroughly**: Validate configuration changes
5. **Document changes**: Keep records of working configurations

## Configuration Validation

Metrix validates configuration on startup:

```cpp
try {
    auto db = Database::open("./mydb", config);
} catch (const ConfigError& e) {
    std::cerr << "Invalid configuration: " << e.what() << std::endl;
}
```

Common validation errors:
- Invalid segment size (not power of 2)
- Cache size too large (exceeds available memory)
- Invalid isolation level
- Incompatible compression settings

## See Also

- [Installation](/en/setup/installation) - Build and setup instructions
- [System Requirements](/en/setup/requirements) - Hardware and software requirements
- [Performance Optimization](/en/architecture/optimization) - Advanced tuning guide
- [API Reference](/en/api/cpp-api) - Programmatic configuration
