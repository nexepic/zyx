# Basic Operations

## Creating Nodes

```cpp
auto person = tx->createNode("Person");
person->setProperty("name", "Bob");
```

## Creating Relationships

```cpp
auto alice = tx->createNode("Person");
auto bob = tx->createNode("Person");
auto knows = alice->createRelationshipTo(bob, "KNOWS");
knows->setProperty("since", 2020);
```

## Querying

See the [API Reference](/en/api/cpp-api) for detailed query operations.
