# Quick Start

## Opening a Database

```cpp
#include <metrix/metrix.hpp>

using namespace metrix;

// Open or create a database
auto db = Database::open("./mydb");
```

## Creating a Transaction

```cpp
// Begin a transaction
auto tx = db->beginTransaction();
```

## Creating Nodes

```cpp
// Create a node with a label
auto node = tx->createNode("User");
node->setProperty("name", "Alice");
node->setProperty("age", 30);
```

## Querying with Cypher

```cpp
// Execute a Cypher query
auto result = db->executeQuery(
    "MATCH (u:User) WHERE u.name = 'Alice' RETURN u"
);
```

## Next Steps

- [Basic Operations](/en/user-guide/basic-operations)
- [API Reference](/en/api/cpp-api)
- [Architecture](/en/architecture/overview)
