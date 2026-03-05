# 事务控制

Metrix 通过其 CLI 提供完整的 ACID 事务支持，允许您将多个操作组合成原子单元。本指南解释如何在 CLI 中控制事务。

## 理解事务

### 什么是事务？

事务是作为单个逻辑工作单元执行的一系列操作。在 Metrix 中，事务遵循 ACID 属性：
- **原子性**：所有操作成功或全部失败
- **一致性**：数据库从一个有效状态转换到另一个有效状态
- **隔离性**：并发事务不会相互干扰
- **持久性**：提交的事务在系统故障后仍然存在

## CLI 事务命令

### BEGIN TRANSACTION

开始新事务：

```bash
# 开始事务
metrix> BEGIN

# 事务现在处于活动状态
metrix (transaction)>
```

### COMMIT

提交当前事务：

```bash
# 在事务模式下
metrix (transaction)> COMMIT

# 事务已提交并关闭
metrix>
```

### ROLLBACK

回滚当前事务：

```bash
# 在事务模式下
metrix (transaction)> ROLLBACK

# 所有更改都被丢弃
metrix>
```

## 事务示例

### 简单事务

```bash
# 开始事务
metrix> BEGIN

# 创建节点
metrix (transaction)> CREATE (p:Person {name: 'Alice', age: 30})
metrix (transaction)> CREATE (p:Person {name: 'Bob', age: 25})

# 提交所有更改
metrix (transaction)> COMMIT
```

## 最佳实践

1. **保持事务简短**：最小化 BEGIN 和 COMMIT 之间的时间
2. **处理错误**：提交前检查错误
3. **使用事务保证一致性**：将相关操作分组
4. **避免长时间事务**：不要在事务中等待用户输入
5. **使用 MERGE**：优雅处理创建或更新场景
6. **监控性能**：使用 PROFILE 了解事务影响

## 下一步

- 学习[批处理操作](/zh/user-guide/batch-operations)进行批量数据处理
- 了解[导入与导出](/zh/user-guide/import-export)进行数据迁移
- 探索[高级查询](/zh/user-guide/advanced-queries)进行复杂操作
