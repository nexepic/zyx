# 基本操作

## 创建节点

```cpp
auto person = tx->createNode("Person");
person->setProperty("name", "Bob");
```

## 创建关系

```cpp
auto alice = tx->createNode("Person");
auto bob = tx->createNode("Person");
auto knows = alice->createRelationshipTo(bob, "KNOWS");
knows->setProperty("since", 2020);
```

## 查询

详细的查询操作请参阅 [API 参考](/zh/api/cpp-api)。
