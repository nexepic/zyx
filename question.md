# Metrix图数据库 - WAL和事务系统实现需求

## 项目背景

**Metrix** 是一个高性能的**嵌入式图数据库**（类似SQLite，但是用于图数据），使用C++20编写，支持Cypher查询语言。项目需要实现完整的ACID事务支持，通过Write-Ahead Logging (WAL)保证数据持久性和崩溃恢复。

### 技术栈
- **语言**: C++20
- **构建系统**: Meson + Conan + Ninja
- **测试框架**: Google Test (目标95%+覆盖率)
- **查询语言**: Cypher (ANTLRR4解析器)

### 关键特性
- **嵌入式数据库**: 单进程访问，类似SQLite的使用模式
- **段存储架构**: 自定义文件格式，固定大小段，位图空间管理
- **状态链管理**: 所有持久化修改通过状态链版本化和回滚
- **脏实体跟踪**: 修改的实体通过DirtyEntityRegistry高效持久化
- **LRU缓存**: 热实体内存缓存，可配置的淘汰策略

### 当前项目状态
- ✅ 核心存储引擎完成
- ✅ Cypher查询引擎完成
- ✅ 基础Transaction类存在但功能极简
- ❌ **缺失**: WAL系统
- ❌ **缺失**: 真正的ACID事务支持
- ❌ **缺失**: 崩溃恢复机制

---

## 当前代码架构和API

### 1. Node类 (`include/graph/core/Node.hpp`)

**关键API**:
```cpp
class Node : public EntityBase<Node> {
public:
    struct Metadata {
        int64_t id = 0;
        int64_t firstOutEdgeId = 0;
        int64_t firstInEdgeId = 0;
        int64_t propertyEntityId = 0;
        int64_t labelId = 0;              // ⚠️ 使用labelId而非字符串label
        uint32_t propertyStorageType = 0;
        bool isActive = true;
    };

    Node(int64_t id, int64_t labelId);  // ⚠️ 构造函数需要labelId

    // Label操作
    [[nodiscard]] int64_t getLabelId() const;
    void setLabelId(int64_t id);

    // 属性操作
    void setProperties(std::unordered_map<std::string, PropertyValue> props);
    void addProperty(const std::string &key, const PropertyValue &value);
    void removeProperty(const std::string &key);
    [[nodiscard]] PropertyValue getProperty(const std::string &key) const;

    // 序列化
    void serialize(std::ostream &os) const;
    static Node deserialize(std::istream &is);

private:
    Metadata metadata;
    std::unordered_map<std::string, PropertyValue> properties;
};
```

**重要特点**:
- Node使用**labelId (int64_t)**而非字符串label
- 支持属性映射
- 固定大小256字节的元数据结构
- 支持序列化/反序列化

### 2. Edge类 (`include/graph/core/Edge.hpp`)

**关键API**:
```cpp
class Edge : public EntityBase<Edge> {
public:
    struct Metadata {
        int64_t id = 0;
        int64_t sourceNodeId = 0;        // ⚠️ 使用sourceNodeId/targetNodeId
        int64_t targetNodeId = 0;
        int64_t nextOutEdgeId = 0;
        int64_t prevOutEdgeId = 0;
        int64_t nextInEdgeId = 0;
        int64_t prevInEdgeId = 0;
        int64_t propertyEntityId = 0;
        int64_t labelId = 0;              // ⚠️ 使用labelId
        uint32_t propertyStorageType = 0;
        bool isActive = true;
    };

    Edge(int64_t id, int64_t sourceId, int64_t targetId, int64_t labelId);

    // Label操作
    [[nodiscard]] int64_t getLabelId() const;
    void setLabelId(int64_t id);

    // 节点关系操作
    [[nodiscard]] int64_t getSourceNodeId() const;
    [[nodiscard]] int64_t getTargetNodeId() const;
    void setSourceNodeId(int64_t sourceId);
    void setTargetNodeId(int64_t targetId);

    // 属性操作
    void setProperties(std::unordered_map<std::string, PropertyValue> props);
    void addProperty(const std::string &key, const PropertyValue &value);
    void removeProperty(const std::string &key);
    [[nodiscard]] PropertyValue getProperty(const std::string &key) const;

    // 序列化
    void serialize(std::ostream &os) const;
    static Edge deserialize(std::istream &is);
};
```

### 3. FileStorage类 (`include/graph/storage/FileStorage.hpp`)

**关键API**:
```cpp
class FileStorage {
public:
    FileStorage(std::string path, size_t cacheSize, OpenMode mode);
    ~FileStorage();

    void open();
    void save();
    void close();
    void flush();
    void initializeComponents();

    // 事务相关 - ⚠️ 当前为简单的写标记机制
    static void beginWrite();
    void commitWrite();
    static void rollbackWrite();

    // 组件访问
    [[nodiscard]] std::shared_ptr<DataManager> getDataManager() const;
    [[nodiscard]] std::shared_ptr<IDAllocator> getIDAllocator() const;
    [[nodiscard]] std::shared_ptr<SegmentTracker> getSegmentTracker() const;

    [[nodiscard]] bool isOpen() const;

private:
    std::shared_ptr<std::fstream> fileStream;
    std::shared_ptr<DataManager> dataManager;
    std::shared_ptr<IDAllocator> idAllocator;
    std::shared_ptr<SegmentTracker> segmentTracker;
    // ... 其他组件
};
```

**重要特点**:
- 当前有简单的`beginWrite()/commitWrite()/rollbackWrite()`机制
- 管理多个子组件：DataManager、IDAllocator、SegmentTracker等
- 单文件流操作

### 4. DataManager类 (`include/graph/storage/data/DataManager.hpp`)

**关键API**:
```cpp
class DataManager {
public:
    // Node操作
    void addNode(Node &node) const;
    void addNodes(std::vector<Node> &nodes) const;
    void updateNode(const Node &node);
    void deleteNode(Node &node) const;

    // Edge操作
    void addEdge(Edge &edge) const;
    void addEdges(std::vector<Edge> &edges) const;
    void updateEdge(const Edge &edge);
    void deleteEdge(Edge &edge) const;

    // 属性操作
    void addNodeProperties(int64_t nodeId,
                         const std::unordered_map<std::string, PropertyValue> &properties) const;
    void addEdgeProperties(int64_t edgeId,
                         const std::unordered_map<std::string, PropertyValue> &properties) const;

    // Label管理
    [[nodiscard]] int64_t getOrCreateLabelId(const std::string &label) const;
    [[nodiscard]] std::string getLabelString(int64_t labelId) const;

    // 观察者模式
    void registerObserver(std::weak_ptr<indexes::IEntityObserver> observer);

private:
    std::shared_ptr<NodeManager> nodeManager_;
    std::shared_ptr<EdgeManager> edgeManager_;
    std::shared_ptr<LabelTokenRegistry> labelTokenRegistry_;
    // ... 其他管理器
};
```

**重要特点**:
- DataManager是数据操作的门面（Facade）
- 内部委托给专门的管理器：NodeManager、EdgeManager等
- 有标签注册表，管理字符串label和labelId的映射
- 支持观察者模式，用于索引更新

### 5. 当前Transaction类 (`include/graph/core/Transaction.hpp`)

**当前实现**:
```cpp
class Transaction {
public:
    explicit Transaction(const Database &db);

    [[nodiscard]] Node insertNode(const std::string &label) const;
    [[nodiscard]] Edge insertEdge(const int64_t &from, const int64_t &to,
                                 const std::string &label) const;

    void commit() const;
    static void rollback();

private:
    std::shared_ptr<storage::FileStorage> storage;
};
```

**实现** (`src/core/Transaction.cpp`):
```cpp
Transaction::Transaction(const Database &db) : storage(db.getStorage()) {
    storage::FileStorage::beginWrite();
}

Node Transaction::insertNode(const std::string &label) const {
    // TODO: 未实现，返回默认构造节点
    return Node();
}

Edge Transaction::insertEdge(const int64_t &from, const int64_t &to,
                             const std::string &label) const {
    // TODO: 未实现，返回默认构造边
    return Edge();
}

void Transaction::commit() const {
    storage->commitWrite();
}

void Transaction::rollback() {
    storage::FileStorage::rollbackWrite();
}
```

**当前状态**: ⚠️ 极简实现，功能基本为空

### 6. Database类 (`include/graph/core/Database.hpp`)

```cpp
class Database {
public:
    explicit Database(const std::string &dbPath,
                      storage::OpenMode mode = storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE,
                      size_t cacheSize = 10000);
    ~Database();

    void open();
    [[nodiscard]] bool openIfExists();
    void close() const;

    [[nodiscard]] bool exists() const;
    [[nodiscard]] bool isOpen() const;

    Transaction beginTransaction();  // ⚠️ 返回值而非指针

    std::shared_ptr<query::QueryEngine> getQueryEngine();
    [[nodiscard]] std::shared_ptr<storage::FileStorage> getStorage() const;

private:
    std::shared_ptr<storage::FileStorage> storage;
    std::shared_ptr<query::QueryEngine> queryEngine;
    std::string dbPath;
    std::shared_ptr<config::SystemConfigManager> configManager_;
};
```

---

## 需要实现的功能

### 核心需求

1. **ACID事务支持**
   - **原子性 (Atomicity)**: 事务中的所有操作要么全部成功，要么全部回滚
   - **一致性 (Consistency)**: 事务执行前后数据库保持一致状态
   - **隔离性 (Isolation)**: 并发事务之间互不干扰
   - **持久性 (Durability)**: 已提交的事务在系统崩溃后仍然有效

2. **Write-Ahead Logging (WAL)**
   - 所有修改先写入WAL日志
   - WAL刷新到磁盘后才认为操作完成
   - 崩溃后可以通过WAL恢复数据

3. **崩溃恢复**
   - 数据库打开时自动检测WAL
   - 应用已提交事务的WAL记录到主数据库
   - 清理WAL文件

4. **并发控制**
   - 支持多线程并发事务
   - 冲突检测和解决机制
   - 可配置的隔离级别

5. **性能要求**
   - 最小化事务延迟
   - 高吞吐量
   - 合理的内存开销

---

## 设计约束和要求

### 1. 嵌入式数据库约束

**关键要求**: 类似SQLite，嵌入式使用场景

**含义**:
- ❌ **不适合**: 后台线程（可能在任意时刻关闭）
- ❌ **不适合**: 多进程并发访问
- ✅ **适合**: 单WAL文件（简化恢复）
- ✅ **适合**: 同步fsync保证持久性
- ✅ **适合**: 可预测的生命周期管理

### 2. 与现有API的兼容性

**严格要求**: 必须与现有的Node/Edge/FileStorage API完美兼容

**当前API特点**:
- Node/Edge使用`labelId (int64_t)`而非字符串label
- DataManager有标签注册表，管理字符串↔ID映射
- FileStorage有简单的`beginWrite()/commitWrite()/rollbackWrite()`

**要求**:
- 新的Transaction系统必须使用这些API
- WAL记录格式必须适配labelId而非字符串label
- 不能破坏现有的DataManager接口

### 3. 架构层次要求

**现有层次结构**:
```
Database
  └─> FileStorage
       └─> DataManager
            ├─> NodeManager
            ├─> EdgeManager
            └─> LabelTokenRegistry (label字符串↔ID映射)
```

**要求**:
- WAL应该在哪个层次？FileStorage之上还是DataManager之上？
- Transaction应该如何与现有组件交互？
- 是否需要新的管理类（如TransactionManager）？

### 4. 事务语义要求

**需要明确**:
- 隔离级别：Read Committed? Repeatable Read? Serializable?
- 并发控制：乐观锁？悲观锁？MVCC？
- 死锁处理：检测和回滚机制？
- 锁粒度：节点级？边级？属性级？

---

## 当前代码的问题

### 问题1: 当前Transaction实现过于简单

**现状**:
```cpp
class Transaction {
    Node insertNode(const std::string &label) const {
        return Node();  // 空实现！
    }
};
```

**问题**:
- 没有真正的功能实现
- 没有WriteSet/ReadSet跟踪
- 没有冲突检测
- 没有回滚能力

### 问题2: 缺少WAL系统

**现状**:
- 完全没有WAL相关代码
- 没有日志记录
- 没有崩溃恢复

**问题**:
- 系统崩溃后数据丢失
- 无法保证持久性
- 无法实现真正的ACID

### 问题3: FileStorage的事务机制过于简单

**现状**:
```cpp
static void beginWrite();
void commitWrite();
static void rollbackWrite();
```

**问题**:
- 只是简单的写标记
- 没有实际的事务逻辑
- 不支持并发事务

---

## 代码专家需要解决的问题

### 架构设计问题

1. **WAL系统架构**
   - WAL文件格式应该如何设计？
   - WAL记录应该包含哪些信息？
   - 如何与现有的Node/Edge API（labelId）集成？
   - 单文件还是多文件WAL？

2. **事务系统架构**
   - Transaction类应该如何实现？
   - 是否需要TransactionManager？
   - 如何实现WriteSet/ReadSet？
   - 如何实现冲突检测？

3. **与现有代码的集成**
   - WAL应该在FileStorage的哪个层次？
   - Transaction应该如何与DataManager交互？
   - 是否需要修改现有的FileStorage API？
   - 如何处理LabelTokenRegistry的label↔labelId映射？

4. **并发控制策略**
   - 应该使用OCC还是PCC？
   - 如何实现MVCC？
   - 如何处理冲突？
   - 如何保证隔离性？

5. **崩溃恢复机制**
   - 如何检测需要恢复？
   - 如何重放WAL？
   - 如何处理部分写入的事务？
   - 如何清理WAL？

### 性能优化问题

1. **I/O优化**
   - 如何减少fsync调用？
   - WAL缓冲策略？
   - 批量写入优化？

2. **内存管理**
   - WriteSet/ReadSet的内存开销？
   - WAL缓冲区大小？
   - 如何避免内存泄漏？

3. **并发性能**
   - 如何最大化并发度？
   - 如何减少锁竞争？
   - 如何处理热点数据？

---

## 期望的实现方案

### 1. 完整的WAL系统

**需要的组件**:
- WAL记录格式和序列化
- WAL文件管理
- WAL写入和读取
- 检查点机制
- 崩溃恢复

### 2. 完整的Transaction系统

**需要的功能**:
- 真正的节点/边创建/更新/删除
- WriteSet和ReadSet跟踪
- 事务提交和回滚
- 冲突检测
- 隔离级别保证

### 3. TransactionManager（可选但推荐）

**可能的功能**:
- 事务生命周期管理
- 并发控制
- 冲突检测和解决
- 事务ID分配

### 4. 与现有代码的集成

**要求**:
- 与Database类无缝集成
- 与FileStorage/DataManager兼容
- 支持现有的Node/Edge API
- 考虑LabelTokenRegistry的存在

---

## 代码上下文文件

如果代码专家需要了解更多上下文，可以提供以下文件：

### 核心数据结构
- `include/graph/core/Node.hpp` - 节点数据结构
- `include/graph/core/Edge.hpp` - 边数据结构
- `include/graph/core/Entity.hpp` - 实体基类
- `include/graph/core/Property.hpp` - 属性系统

### 存储层
- `include/graph/storage/FileStorage.hpp` - 文件存储
- `include/graph/storage/data/DataManager.hpp` - 数据管理
- `include/graph/storage/data/NodeManager.hpp` - 节点管理
- `include/graph/storage/data/EdgeManager.hpp` - 边管理

### 标签系统
- `include/graph/storage/data/LabelTokenRegistry.hpp` - 标签注册表

### 事务相关
- `include/graph/core/Transaction.hpp` - 当前事务类
- `include/graph/core/Database.hpp` - 数据库入口

### 测试
- `tests/integration/test_IntegrationTransaction.cpp` - 事务集成测试

---

## 具体需要解决的问题示例

### 示例1: 如何处理labelId vs label字符串？

**问题**: Node/Edge使用labelId (int64_t)，但用户输入的是字符串label

**现状**:
- DataManager有`getOrCreateLabelId(string)`方法
- Node构造函数需要`labelId`

**需要决策**:
- WAL记录应该存储labelId还是字符串label？
- 如果存储labelId，恢复时如何处理？
- 如果存储字符串，如何保证效率？

### 示例2: Transaction如何与DataManager交互？

**问题**: Transaction需要记录修改，但DataManager直接操作存储

**现状**:
- DataManager的`addNode(node)`直接写入存储
- Transaction需要拦截这些操作

**需要决策**:
- Transaction是否应该包装DataManager调用？
- 还是应该修改DataManager的API？
- WriteSet应该如何跟踪修改？

### 示例3: 如何保证ACID？

**问题**: 当前没有任何ACID保证

**需要实现**:
- **A**: 如何保证原子性？
- **C**: 如何保证一致性？
- **I**: 如何保证隔离性？
- **D**: 如何保证持久性？

### 示例4: 嵌入式场景的特殊要求？

**问题**: 嵌入式数据库可能随时关闭

**需要考虑**:
- 如何避免后台线程？
- 如何确保关闭时数据安全？
- 如何处理异常终止？

---

## 性能和质量要求

### 性能目标
- 事务延迟: P99 < 10ms (小事务)
- 吞吐量: > 10,000 事务/秒
- 恢复时间: < 30s (10M记录)
- 内存开销: < 100MB (1,000活跃事务)

### 代码质量要求
- **优雅性**: 清晰的职责划分，一致的命名
- **高效性**: 最小化不必要拷贝，合理的内存管理
- **可维护性**: 良好的封装，完善的错误处理
- **可测试性**: 95%+测试覆盖率
- **可扩展性**: 易于添加新功能

---

## 总结

我需要一个**完整、优雅、高效、可维护**的WAL和事务系统实现，要求：

1. **完全理解现有代码架构** (Node/Edge/FileStorage/DataManager的API)
2. **设计合理的WAL系统** (与现有API兼容，考虑labelId特性)
3. **实现真正的ACID事务** (WriteSet/ReadSet、冲突检测、回滚)
4. **支持崩溃恢复** (WAL重放、检查点、清理)
5. **嵌入式友好** (无后台线程、可预测生命周期)
6. **高性能** (最小化延迟、最大化吞吐量)

**重要**: 请先分析现有代码，再设计方案。如果当前架构有问题或需要调整，请明确说明。我更看重正确性而非实现速度。

如果需要了解更多的代码上下文，请告诉我具体哪些文件或组件，我会提供完整信息。
