# 高级查询

本指南涵盖 ZYX 中复杂数据检索和操作的高级 Cypher 查询模式和技术。

## 聚合和分组

### 基本聚合

```cypher
-- 计数节点
MATCH (n:Person)
RETURN COUNT(n) AS total

-- 求和聚合
MATCH (n:Person)
RETURN SUM(n.salary) AS total_salary

-- 平均聚合
MATCH (n:Person)
RETURN AVG(n.age) AS avg_age
```

## 高级过滤

### 模式过滤

```cypher
-- 基于关系存在性过滤
MATCH (n:Person)
WHERE (n)-[:KNOWS]->()
RETURN n

-- 基于不存在性过滤
MATCH (n:Person)
WHERE NOT (n)-[:KNOWS]->()
RETURN n

-- 按关系计数过滤
MATCH (n:Person)
WHERE size((n)-[:KNOWS]->()) > 5
RETURN n
```

## 可选模式

### 可选关系

```cypher
-- 匹配带有可选关系的节点
MATCH (n:Person)
OPTIONAL MATCH (n)-[:WORKS_AT]->(c:Company)
RETURN n.name, c.name AS company
```

## UNION 和组合结果

### UNION ALL

```cypher
-- 组合结果（包括重复项）
MATCH (n:Person)
WHERE n.city = 'New York'
RETURN n.name AS name
UNION ALL
MATCH (n:Person)
WHERE n.city = 'San Francisco'
RETURN n.name AS name
```

## 子查询和 WITH 子句

### 复杂的 WITH 操作

```cypher
-- 链接多个操作
MATCH (n:Person)
WITH n
WHERE n.age > 30
WITH n, n.age * 2 AS double_age
WHERE double_age > 60
RETURN n.name, double_age
```

## 最短路径查询

### 单个最短路径

```cypher
-- 查找两个节点之间的最短路径
MATCH (a:Person {name: 'Alice'}), (b:Person {name: 'Charlie'})
MATCH path = shortestPath((a)-[:KNOWS*]-(b))
RETURN path
```

## 变长度路径

### 无界路径

```cypher
-- 查找所有连接的节点（任意深度）
MATCH (n:Person {name: 'Alice'})-[:KNOWS*]-(connected)
RETURN DISTINCT connected

-- 指定最小深度
MATCH (n:Person {name: 'Alice'})-[:KNOWS*2..]-(connected)
RETURN DISTINCT connected
```

## 复杂模式匹配

### 交集模式

```cypher
-- 查找连接到多个特定节点的节点
MATCH (n:Person)
WHERE (n)-[:KNOWS]->({name: 'Alice'})
  AND (n)-[:KNOWS]->({name: 'Bob'})
RETURN n
```

## 条件逻辑

### CASE 表达式

```cypher
-- 简单 CASE
MATCH (n:Person)
RETURN n.name,
       CASE n.age
         WHEN < 20 THEN 'Young'
         WHEN < 40 THEN 'Adult'
         ELSE 'Senior'
       END AS age_group
```

## 性能优化

### 查询分析

```cypher
-- 分析查询执行
PROFILE MATCH (n:Person)
WHERE n.age > 30
RETURN n

-- 解释查询计划（不执行）
EXPLAIN MATCH (n:Person)
RETURN n
```

## 最佳实践

1. **使用 PROFILE**：始终分析复杂查询以了解性能
2. **限制路径长度**：使用有界变长度路径避免爆炸
3. **提前过滤**：尽可能在 MATCH 中应用 WHERE 子句
4. **使用索引**：在频繁查询的属性上创建索引
5. **避免笛卡尔积**：显式连接所有模式

## 下一步

- 探索[模式匹配](/zh/user-guide/pattern-matching)了解更多高级模式
- 了解[事务控制](/zh/user-guide/transactions)确保数据一致性
- 学习[批处理操作](/zh/user-guide/batch-operations)进行批量数据处理
