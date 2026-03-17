# 批处理操作

在 ZYX 中高效处理大数据集需要批处理操作。本指南涵盖批量数据处理技术。

## 理解批处理操作

### 为什么使用批处理？

批处理操作提供以下好处：
- **性能**：通过分组操作减少开销
- **内存效率**：分块处理数据而不是一次性处理
- **事务控制**：更好地控制提交点
- **错误处理**：将故障隔离到特定批次

## 批处理策略

### 固定大小批次

```bash
# 每次处理 1000 条记录
# 批次 1
zyx> BEGIN
zyx (transaction)> CREATE (p:Person {id: 1, name: 'Alice'})
# ... 999 条更多记录 ...
zyx (transaction)> COMMIT
```

## CLI 批处理技术

### 使用脚本

创建批处理的 Cypher 脚本：

```cypher
// batch_import.cypher
// 批次 1：记录 1-1000
BEGIN
CREATE (p:Person {id: 1, name: 'Person1'})
CREATE (p:Person {id: 2, name: 'Person2'})
// ... 最多 id: 1000 ...
COMMIT;
```

运行脚本：

```bash
$ zyx-cli < batch_import.cypher
```

## 批处理模式

### 批量节点创建

```cypher
// 使用 UNWIND 进行高效批量创建
BEGIN
WITH [
  {id: 1, name: 'Alice', age: 30},
  {id: 2, name: 'Bob', age: 25}
] AS batch
UNWIND batch AS item
CREATE (p:Person {id: item.id, name: item.name, age: item.age})
COMMIT;
```

### 批量更新

```cypher
// 批量更新属性
// 批次 1
BEGIN
MATCH (p:Person)
WHERE p.id >= 1 AND p.id <= 1000
SET p.processed = true, p.updatedAt = timestamp()
COMMIT;
```

### 批量删除

```cypher
// 分批删除以避免锁定问题
// 批次 1
BEGIN
MATCH (p:Person)
WHERE p.id >= 1 AND p.id <= 1000
DETACH DELETE p
COMMIT;
```

## 最佳实践

1. **测试批次大小**：从小批次开始并增加
2. **监控资源**：观察 CPU、内存和磁盘 I/O
3. **使用索引**：在批处理操作前创建索引
4. **验证数据**：每批后检查数据完整性
5. **记录进度**：保留详细日志用于故障排除
6. **规划回滚**：知道出现问题时如何撤销

## 下一步

- 学习[导入与导出](/zh/user-guide/import-export)进行数据迁移
- 了解[事务控制](/zh/user-guide/transactions)确保数据一致性
- 探索[高级查询](/zh/user-guide/advanced-queries)进行复杂操作
