# 快速开始

本指南将帮助您使用命令行界面（CLI）快速上手 Metrix。

## 启动 CLI

Metrix CLI 提供了一个交互式 REPL（Read-Eval-Print Loop）用于执行 Cypher 查询。

### 基本用法

```bash
./buildDir/apps/cli/metrix-cli ./mydb
```

此命令：
1. 如果 `./mydb` 不存在，则创建新数据库
2. 如果数据库已存在，则打开它
3. 启动交互式 Cypher REPL 会话

### CLI 选项

```bash
metrix-cli [database_path] [options]

Options:
  -h, --help     显示使用信息
  -v, --version  显示版本信息
```

## 第一个查询

进入 REPL 后，您将看到 `cypher>` 提示符。让我们创建一些数据！

### 创建节点

```cypher
CREATE (u:User {name: 'Alice', age: 30})
```

这将创建一个节点：
- 标签：`User`
- 属性：`name`（字符串）和 `age`（整数）

### 查询节点

```cypher
MATCH (u:User {name: 'Alice'}) RETURN u
```

这将检索我们刚刚创建的节点。

### 创建多个节点

```cypher
CREATE
  (u1:User {name: 'Bob', age: 25}),
  (u2:User {name: 'Charlie', age: 35})
```

### 创建关系

```cypher
MATCH (alice:User {name: 'Alice'})
MATCH (bob:User {name: 'Bob'})
CREATE (alice)-[:KNOWS {since: 2020}]->(bob)
```

这将创建一个从 Alice 到 Bob 的 `KNOWS` 关系，并带有 `since` 属性。

### 查询关系

```cypher
MATCH (a:User {name: 'Alice'})-[:KNOWS]->(b:User)
RETURN a.name AS alice, b.name AS friend, b.age AS friend_age
```

结果：
```
+-------+--------+------------+
| alice | friend | friend_age |
+-------+--------+------------+
| Alice | Bob    | 25         |
+-------+--------+------------+
```

## 使用 REPL

### 多行查询

对于复杂查询，可以将其分散到多行：

```cypher
MATCH (u:User)
WHERE u.age > 25
RETURN u.name, u.age
ORDER BY u.age DESC
```

按两次 `Enter` 键执行查询。

### 运行多个查询

用分号分隔查询：

```cypher
CREATE (p:Person {name: 'David'});
MATCH (p:Person) RETURN p;
```

### 清屏

```cypher
:clear
```

### 退出 CLI

```cypher
:exit
```

或按 `Ctrl+D`

## 常见模式

### 查找带有标签的所有节点

```cypher
MATCH (u:User) RETURN u
```

### 使用条件过滤

```cypher
MATCH (u:User)
WHERE u.age > 30
RETURN u.name, u.age
```

### 更新属性

```cypher
MATCH (u:User {name: 'Alice'})
SET u.age = 31, u.city = 'New York'
```

### 删除关系

```cypher
MATCH (a:User {name: 'Alice'})-[r:KNOWS]->(b:User)
DELETE r
```

### 删除节点

```cypher
MATCH (u:User {name: 'Bob'})
DELETE u
```

::: warning 删除警告
在删除节点之前，必须先删除连接到该节点的所有关系。
:::

## 事务管理

CLI 支持显式事务控制：

```cypher
# 开始事务
:begin

# 执行操作
CREATE (p:Person {name: 'Eve'})

# 提交事务
:commit

# 或回滚
:rollback
```

## 从文件导入数据

您可以从文件导入 Cypher 查询：

```bash
# 创建 import.cql
cat > import.cql << 'EOF'
CREATE (u1:User {name: 'Frank', age: 28});
CREATE (u2:User {name: 'Grace', age: 32});
MATCH (u1:User {name: 'Frank'}), (u2:User {name: 'Grace'})
CREATE (u1)-[:KNOWS]->(u2);
EOF

# 导入文件
metrix-cli ./mydb < import.cql
```

## 示例：社交网络

让我们创建一个简单的社交网络：

```cypher
# 创建用户
CREATE
  (alice:User {name: 'Alice', age: 30}),
  (bob:User {name: 'Bob', age: 25}),
  (charlie:User {name: 'Charlie', age: 35}),
  (diana:User {name: 'Diana', age: 28})

# 创建关系
MATCH (alice:User {name: 'Alice'})
MATCH (bob:User {name: 'Bob'})
MATCH (charlie:User {name: 'Charlie'})
MATCH (diana:User {name: 'Diana'})

CREATE (alice)-[:KNOWS {since: 2020}]->(bob)
CREATE (alice)-[:KNOWS {since: 2021}]->(charlie)
CREATE (bob)-[:KNOWS {since: 2019}]->(diana)
CREATE (charlie)-[:KNOWS {since: 2022}]->(diana)

# 查找朋友的朋友
MATCH (me:User {name: 'Alice'})-[:KNOWS]->(friend)-[:KNOWS]->(fof)
WHERE NOT (me)-[:KNOWS]->(fof) AND fof <> me
RETURN me.name, friend.name, fof.name
```

## 下一步

- [基本操作](/zh/user-guide/basic-operations) - 学习 CRUD 操作
- [API 参考](/zh/api/cpp-api) - 在您的应用中嵌入 Metrix
