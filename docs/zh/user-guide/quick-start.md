# 快速开始

## 打开数据库

```cpp
#include <metrix/metrix.hpp>

using namespace metrix;

// 打开或创建数据库
auto db = Database::open("./mydb");
```

## 创建事务

```cpp
// 开始事务
auto tx = db->beginTransaction();
```

## 创建节点

```cpp
// 创建带标签的节点
auto node = tx->createNode("User");
node->setProperty("name", "Alice");
node->setProperty("age", 30);
```

## 使用 Cypher 查询

```cpp
// 执行 Cypher 查询
auto result = db->executeQuery(
    "MATCH (u:User) WHERE u.name = 'Alice' RETURN u"
);
```

## 下一步

- [基本操作](/zh/user-guide/basic-operations)
- [API 参考](/zh/api/cpp-api)
- [架构](/zh/architecture/overview)
