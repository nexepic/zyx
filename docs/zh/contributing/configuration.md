# 系统配置

Metrix 提供全面的配置选项，可为您的特定工作负载优化性能和行为。

## 配置概述

配置可通过以下方式设置：
1. **编程 API**: C++ API 方法
2. **配置文件**: 基于 JSON 的配置文件
3. **环境变量**: 覆盖设置
4. **命令行选项**: CLI 工具参数

## 存储配置

### 段大小

配置存储段的大小：

```cpp
struct StorageConfig {
    size_t segmentSize = 4 * 1024 * 1024;  // 默认 4 MB
};
```

**指导原则**：
- **小段（1-4 MB）**: 更适合写密集型工作负载
- **大段（16-64 MB）**: 更适合读密集型工作负载
- **默认值**: 4 MB 在两种场景下保持平衡

### 缓存大小

控制内存缓存大小：

```cpp
struct CacheConfig {
    size_t maxCacheSize = 256 * 1024 * 1024;  // 默认 256 MB
    size_t maxEntities = 100000;               // 最大缓存实体数
};
```

**调优指南**：
- **可用 RAM**: 分配 50-70% 的可用内存
- **工作集**: 大小应超过热数据集
- **监控**: 目标 >80% 缓存命中率

### WAL 配置

写前日志设置：

```cpp
struct WALConfig {
    size_t maxWalSize = 100 * 1024 * 1024;     // 默认 100 MB
    Duration checkpointInterval = Duration::minutes(5);
    bool fsyncOnCommit = true;                  // 持久性与性能
};
```

**权衡**：
- **fsyncOnCommit = true**: 最大持久性，写入较慢
- **fsyncOnCommit = false**: 写入更快，崩溃时数据丢失风险
- **更大的 WAL**: 检查点更少，恢复时间更长

## 事务配置

### 隔离级别

设置事务隔离：

```cpp
enum class IsolationLevel {
    ReadCommitted,  // 默认：查看已提交的数据
    Serializable     // 计划中：完全隔离
};

auto tx = db->beginTransaction(IsolationLevel::ReadCommitted);
```

### 事务超时

防止长时间运行的事务：

```cpp
struct TransactionConfig {
    std::chrono::milliseconds timeout = 30000ms;  // 30 秒
    size_t maxRetries = 3;                        // 冲突重试次数
};
```

### 锁管理

乐观并发控制设置：

```cpp
struct ConcurrencyConfig {
    bool enableConflictDetection = true;
    size_t maxVersionsPerEntity = 10;             // MVCC 版本限制
};
```

## 查询引擎配置

### 查询优化

控制查询规划和优化：

```cpp
struct QueryConfig {
    bool enableOptimization = true;               // 启用查询优化器
    bool enableIndexScan = true;                  // 在可用时使用索引
    size_t maxQueryMemory = 1024 * 1024 * 1024;  // 1 GB 查询内存限制
};
```

### 并行执行

配置并行查询执行：

```cpp
struct ParallelConfig {
    size_t maxThreads = std::thread::hardware_concurrency();
    size_t minRowsForParallel = 10000;           // 并行扫描的最小行数
};
```

## 索引配置

### 索引类型

配置不同的索引策略：

```cpp
enum class IndexType {
    HASH,          // O(1) 查找，仅等值
    BTREE,         // O(log n) 查找，范围查询
    VECTOR         // 向量相似度搜索
};
```

### 索引构建

控制索引创建：

```cpp
struct IndexConfig {
    bool buildInBackground = true;
    size_t indexBuildMemory = 512 * 1024 * 1024;  // 512 MB
};
```

## 压缩配置

### 数据压缩

启用存储压缩：

```cpp
struct CompressionConfig {
    bool enabled = true;
    CompressionType type = CompressionType::ZLIB;
    size_t threshold = 1024;                       // 压缩 > 1 KB 的数据
    CompressionLevel level = CompressionLevel::Default;
};
```

**压缩类型**：
- **ZLIB**: 良好的压缩率，中等速度
- **LZ4**: 快速压缩，中等压缩率
- **ZSTD**: 最佳压缩率，压缩较慢

### 压缩级别

```cpp
enum class CompressionLevel {
    Fast,       // 最低压缩，最快
    Default,    // 平衡
    Maximum     // 最高压缩，最慢
};
```

## 日志配置

### 日志级别

控制详细程度：

```cpp
enum class LogLevel {
    Debug,
    Info,       // 默认
    Warning,
    Error
};
```

### 日志输出

配置日志目标：

```cpp
struct LogConfig {
    LogLevel level = LogLevel::Info;
    bool logToFile = false;
    std::string logFilePath = "/var/log/metrix.log";
    bool logToConsole = true;
    bool logTransactions = false;    // 详细的事务日志
};
```

## 性能调优

### 内存管理

```cpp
struct MemoryConfig {
    size_t maxMemory = 2 * 1024 * 1024 * 1024;  // 2 GB 限制
    bool enableMemoryPooling = true;
    size_t poolBlockSize = 4 * 1024 * 1024;      // 4 MB 池
};
```

### I/O 优化

```cpp
struct IOConfig {
    size_t pageSize = 4096;                       // 系统页面大小
    size_t readAheadSize = 64 * 1024;            // 64 KB 预读
    bool enableDirectIO = false;                  // 绕过 OS 缓存
};
```

## 配置文件格式

使用 JSON 配置文件：

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

加载配置：

```cpp
auto db = Database::open("./mydb", "./config.json");
```

## 环境变量

使用环境变量覆盖配置：

```bash
export METRIX_CACHE_SIZE=536870912      # 512 MB
export METRIX_LOG_LEVEL=Debug
export METRIX_WAL_SIZE=209715200        # 200 MB
export METRIX_MAX_THREADS=16
```

## 运行时重新配置

某些设置可以在运行时更改：

```cpp
// 更新缓存大小
db->setCacheSize(newSize);

// 更改日志级别
db->setLogLevel(LogLevel::Debug);

// 启用/禁用查询优化
db->setQueryOptimization(enabled);
```

## 配置最佳实践

1. **从默认值开始**: 最初使用默认设置
2. **监控性能**: 调优前跟踪指标
3. **一次更改一个设置**: 隔离更改的影响
4. **彻底测试**: 验证配置更改
5. **记录更改**: 保存工作配置的记录

## 配置验证

Metrix 在启动时验证配置：

```cpp
try {
    auto db = Database::open("./mydb", config);
} catch (const ConfigError& e) {
    std::cerr << "无效配置: " << e.what() << std::endl;
}
```

常见验证错误：
- 无效的段大小（不是 2 的幂）
- 缓存大小太大（超过可用内存）
- 无效的隔离级别
- 不兼容的压缩设置

## 另请参阅

- [安装](/zh/setup/installation) - 构建和设置说明
- [系统要求](/zh/setup/requirements) - 硬件和软件要求
- [性能优化](/zh/architecture/optimization) - 高级调优指南
- [API 参考](/zh/api/cpp-api) - 编程配置
