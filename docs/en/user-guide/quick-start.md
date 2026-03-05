# Quick Start

This guide will help you get started with Metrix using the command-line interface (CLI).

## Starting the CLI

The Metrix CLI provides an interactive REPL (Read-Eval-Print Loop) for executing Cypher queries.

### Basic Usage

```bash
./buildDir/apps/cli/metrix-cli ./mydb
```

This command:
1. Creates a new database at `./mydb` if it doesn't exist
2. Opens the database if it already exists
3. Starts an interactive Cypher REPL session

### CLI Options

```bash
metrix-cli [database_path] [options]

Options:
  -h, --help     Show usage information
  -v, --version  Show version information
```

## Your First Query

Once in the REPL, you'll see the `cypher>` prompt. Let's create some data!

### Create a Node

```cypher
CREATE (u:User {name: 'Alice', age: 30})
```

This creates a node with:
- Label: `User`
- Properties: `name` (string) and `age` (integer)

### Query the Node

```cypher
MATCH (u:User {name: 'Alice'}) RETURN u
```

This retrieves the node we just created.

### Create Multiple Nodes

```cypher
CREATE
  (u1:User {name: 'Bob', age: 25}),
  (u2:User {name: 'Charlie', age: 35})
```

### Create Relationships

```cypher
MATCH (alice:User {name: 'Alice'})
MATCH (bob:User {name: 'Bob'})
CREATE (alice)-[:KNOWS {since: 2020}]->(bob)
```

This creates a `KNOWS` relationship from Alice to Bob with a `since` property.

### Query Relationships

```cypher
MATCH (a:User {name: 'Alice'})-[:KNOWS]->(b:User)
RETURN a.name AS alice, b.name AS friend, b.age AS friend_age
```

Result:
```
+-------+--------+------------+
| alice | friend | friend_age |
+-------+--------+------------+
| Alice | Bob    | 25         |
+-------+--------+------------+
```

## Working with the REPL

### Multi-line Queries

For complex queries, you can spread them across multiple lines:

```cypher
MATCH (u:User)
WHERE u.age > 25
RETURN u.name, u.age
ORDER BY u.age DESC
```

Press `Enter` twice to execute the query.

### Running Multiple Queries

Separate queries with a semicolon:

```cypher
CREATE (p:Person {name: 'David'});
MATCH (p:Person) RETURN p;
```

### Clear Screen

```cypher
:clear
```

### Exit the CLI

```cypher
:exit
```

or press `Ctrl+D`

## Common Patterns

### Find All Nodes with a Label

```cypher
MATCH (u:User) RETURN u
```

### Filter with Conditions

```cypher
MATCH (u:User)
WHERE u.age > 30
RETURN u.name, u.age
```

### Update Properties

```cypher
MATCH (u:User {name: 'Alice'})
SET u.age = 31, u.city = 'New York'
```

### Delete Relationships

```cypher
MATCH (a:User {name: 'Alice'})-[r:KNOWS]->(b:User)
DELETE r
```

### Delete Nodes

```cypher
MATCH (u:User {name: 'Bob'})
DELETE u
```

::: warning Delete Warning
You must delete all relationships connected to a node before deleting the node itself.
:::

## Transaction Management

The CLI supports explicit transaction control:

```cypher
# Begin a transaction
:begin

# Execute operations
CREATE (p:Person {name: 'Eve'})

# Commit the transaction
:commit

# Or rollback
:rollback
```

## Import Data from File

You can import Cypher queries from a file:

```bash
# Create import.cql
cat > import.cql << 'EOF'
CREATE (u1:User {name: 'Frank', age: 28});
CREATE (u2:User {name: 'Grace', age: 32});
MATCH (u1:User {name: 'Frank'}), (u2:User {name: 'Grace'})
CREATE (u1)-[:KNOWS]->(u2);
EOF

# Import the file
metrix-cli ./mydb < import.cql
```

## Example: Social Network

Let's create a simple social network:

```cypher
# Create users
CREATE
  (alice:User {name: 'Alice', age: 30}),
  (bob:User {name: 'Bob', age: 25}),
  (charlie:User {name: 'Charlie', age: 35}),
  (diana:User {name: 'Diana', age: 28})

# Create relationships
MATCH (alice:User {name: 'Alice'})
MATCH (bob:User {name: 'Bob'})
MATCH (charlie:User {name: 'Charlie'})
MATCH (diana:User {name: 'Diana'})

CREATE (alice)-[:KNOWS {since: 2020}]->(bob)
CREATE (alice)-[:KNOWS {since: 2021}]->(charlie)
CREATE (bob)-[:KNOWS {since: 2019}]->(diana)
CREATE (charlie)-[:KNOWS {since: 2022}]->(diana)

# Find friends of friends
MATCH (me:User {name: 'Alice'})-[:KNOWS]->(friend)-[:KNOWS]->(fof)
WHERE NOT (me)-[:KNOWS]->(fof) AND fof <> me
RETURN me.name, friend.name, fof.name
```

## Next Steps

- [Basic Operations](/en/user-guide/basic-operations) - Learn CRUD operations
- [API Reference](/en/api/cpp-api) - Embed Metrix in your application
