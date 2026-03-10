# Documentation Standards

Clear, comprehensive documentation is essential for Metrix's usability and maintainability. This guide outlines documentation standards.

## Documentation Types

### 1. Code Comments

#### File Headers

Every source file should have a descriptive header:

```cpp
/**
 * @file Database.cpp
 * @brief Implementation of the Database class
 *
 * This file implements the core Database class which provides
 * the main interface for database operations including opening,
 * closing, and transaction management.
 *
 * @author Metrix Contributors
 * @date 2024
 */
```

#### Class Documentation

```cpp
/**
 * @class Database
 * @brief Main database interface for graph operations
 *
 * The Database class provides the primary interface for working with
 * Metrix graph databases. It manages database lifecycle, coordinates
 * between storage and query engines, and provides transaction support.
 *
 * Example usage:
 * @code
 * auto db = Database::open("/path/to/database");
 * auto result = db->execute("MATCH (n) RETURN n");
 * db->close();
 * @endcode
 */
class Database {
    // ...
};
```

#### Method Documentation

```cpp
/**
 * @brief Opens an existing database
 *
 * Opens the database at the specified path. The database must already
 * exist or a DatabaseNotFoundException will be thrown.
 *
 * @param path Filesystem path to the database directory
 * @return Unique pointer to the opened Database instance
 *
 * @throws DatabaseNotFoundException if database doesn't exist
 * @throws DatabaseLockException if database is locked by another process
 * @throws IOException if filesystem error occurs
 *
 * @par Example
 * @code
 * try {
 *     auto db = Database::open("/data/mydb");
 *     // Use database
 * } catch (const DatabaseNotFoundException& e) {
 *     std::cerr << "Database not found" << std::endl;
 * }
 * @endcode
 */
static std::unique_ptr<Database> open(const std::string& path);
```

### 2. API Documentation

#### Public API Headers

Public API documentation should be comprehensive and include:

- **Purpose**: What the API does
- **Parameters**: Input parameters with types and constraints
- **Return values**: What is returned and possible values
- **Exceptions**: What errors can be thrown
- **Examples**: Usage examples
- **Thread safety**: Whether operations are thread-safe
- **Performance**: Performance characteristics

```cpp
/**
 * @brief Execute a Cypher query
 *
 * Executes the given Cypher query string and returns the result.
 * The query is executed in a new auto-commit transaction.
 *
 * @param cypher Cypher query string to execute
 * @return Result object containing query results
 *
 * @throws ParseException if query syntax is invalid
 * @throws ExecutionException if query execution fails
 * @throws ConstraintViolationException if constraints are violated
 *
 * @par Thread Safety
 * This method is thread-safe. Multiple threads can execute queries
 * concurrently.
 *
 * @par Performance
 * Query execution time depends on query complexity. Simple lookups
 * typically complete in < 1ms. Complex pattern matching may take
 * longer.
 *
 * @par Example
 * @code
 * auto result = db->execute("MATCH (n:Person) RETURN n.name");
 * for (const auto& row : result) {
 *     std::cout << row["n.name"].asString() << std::endl;
 * }
 * @endcode
 */
Result execute(const std::string& cypher);
```

### 3. User Documentation

#### Tutorial Style

User documentation should be tutorial and example-driven:

```markdown
# Quick Start Guide

## Creating a Database

To create a new database, use the `Database::create()` method:

```cpp
#include <metrix/metrix.hpp>

using namespace metrix;

// Create database at specified path
auto db = Database::create("/path/to/database");
```

## Adding Data

Add nodes using Cypher queries:

```cpp
// Create a person node
db->execute("CREATE (p:Person {name: 'Alice', age: 30})");
```

## Querying Data

Query data using the MATCH clause:

```cpp
// Find all people
auto result = db->execute("MATCH (p:Person) RETURN p.name, p.age");

// Process results
for (const auto& row : result) {
    std::cout << row["p.name"].asString() << std::endl;
}
```
```

#### Conceptual Documentation

Explain concepts clearly with diagrams:

```markdown
## Transaction System

Metrix provides ACID transactions through a Write-Ahead Log (WAL).

### Transaction States

```mermaid
stateDiagram-v2
    [*] --> Active: BEGIN
    Active --> Committed: COMMIT
    Active --> RolledBack: ROLLBACK
    Committed --> [*]
    RolledBack --> [*]
```

### Transaction Isolation

Metrix uses snapshot isolation to ensure transaction consistency...
```

### 4. Architecture Documentation

#### Component Overviews

```markdown
# Query Engine Architecture

The query engine is responsible for parsing, planning, and executing
Cypher queries.

## Components

### Parser

The parser converts Cypher text into an Abstract Syntax Tree (AST).

**Key responsibilities:**
- Lexical analysis
- Syntax parsing
- AST construction

**Implementation:** ANTLR4-based parser in `src/query/parser/cypher/`

### Planner

The planner transforms the AST into an executable query plan.

**Key responsibilities:**
- Logical plan generation
- Optimization rule application
- Cost estimation

**Implementation:** `src/query/planner/QueryPlanner.cpp`
```

## Documentation Structure

### Directory Organization

```
docs/
├── user-guide/           # User-facing documentation
│   ├── installation.md
│   ├── quick-start.md
│   └── advanced-queries.md
├── api/                  # API reference
│   ├── cpp-api.md
│   ├── transaction.md
│   └── types.md
├── architecture/         # Architecture documentation
│   ├── overview.md
│   ├── storage.md
│   └── query-engine.md
└── contributing/         # Contributor documentation
    ├── development-setup.md
    ├── testing.md
    └── doc-standards.md
```

## Writing Guidelines

### Style Guidelines

1. **Use clear, simple language**: Avoid jargon when possible
2. **Be concise**: Get to the point quickly
3. **Use active voice**: "Create a database" not "A database can be created"
4. **Break into sections**: Use headings to organize content
5. **Include examples**: Show, don't just tell

### Formatting

#### Code Blocks

Use syntax-highlighted code blocks:

````markdown
```cpp
auto db = Database::open("/path/to/db");
```
````

#### Admonitions

Use callouts for important information:

```markdown
::: tip Best Practice
Always close databases when done to release resources.
:::

::: warning Warning
Deleting nodes also deletes all connected relationships.
:::

::: danger Critical
Never modify the database files directly while the database is open.
:::
```

#### Tables

Use tables for structured information:

| Method | Description | Thread-Safe |
|--------|-------------|-------------|
| `open()` | Opens existing database | No |
| `create()` | Creates new database | No |
| `execute()` | Executes query | Yes |

### Diagrams

Use Mermaid for diagrams:

```markdown
```mermaid
graph LR
    A[Query] --> B[Parser]
    B --> C[Planner]
    C --> D[Executor]
    D --> E[Result]
```
```

#### Visual Diagram Standards

**CRITICAL**: Follow these strict guidelines for all diagrams in documentation.

1. **NO ASCII ART**: Absolutely NO hand-drawn ASCII art diagrams
   - **Forbidden**: Box-drawing characters like `┌│└─│/\`, `+----+`, `|    |`
   - **Why**: ASCII art is difficult to maintain, error-prone, renders poorly on different devices, and cannot be easily updated
   - **Always use**: Mermaid diagrams instead

2. **Color Restrictions**: Professional grayscale only
   - **FORBIDDEN colors** (never use these):
     - `fill:#90EE90` (light green)
     - `fill:#e1f5ff` (light blue)
     - `fill:#ffe1e1` (light red)
     - `fill:#ff0`, `fill:#f9f`, `fill:#bbf` (bright colors)
     - `fill:lightblue`, `fill:lightgreen`, etc.
     - Any `stroke:` styling with colors
   - **ALLOWED styling** (use sparingly):
     - No fill/color at all (default black/white) - **PREFERRED**
     - Subtle grays only: `fill:#f0f0f0`, `fill:#e0e0e0`, `fill:#d0d0d0`
   - **Rationale**: Colorful diagrams appear unprofessional, may not print well, and can be distracting. Simple grayscale diagrams are cleaner and more publication-ready.

3. **Diagram Types**: Use appropriate Mermaid diagram types:
   - **Structure**: `classDiagram` for class hierarchies, data structures
   - **Flow/Process**: `flowchart TD` or `graph TD` for processes and flows
   - **State**: `stateDiagram-v2` for state machines
   - **Sequence**: `sequenceDiagram` for interactions
   - **Entity Relationship**: `erDiagram` for database schemas

4. **Diagram Clarity**:
   - Keep diagrams simple and focused
   - Avoid excessive detail or visual noise
   - Ensure text is readable and labels are clear
   - Use consistent styling across all documentation

#### Examples

**GOOD - Clean Mermaid diagram:**
```mermaid
classDiagram
    class Database {
        +open() void
        +close() void
        +execute(query) Result
    }
    class Transaction {
        +commit() bool
        +rollback() void
    }
    Database --> Transaction
```

**BAD - ASCII art (FORBIDDEN):**
```
┌─────────────┐
│  Database   │
├─────────────┤
│  open()     │
│  close()    │
│  execute()  │
└─────────────┘
```

**BAD - Colorful Mermaid (FORBIDDEN):**
```mermaid
graph TD
    A[Start] --> B[Process]
    style A fill:#90EE90
    style B fill:#e1f5ff
    style A stroke:#f00,stroke-width:4px
```

**GOOD - Professional grayscale:**
```mermaid
graph TD
    A[Start] --> B[Process]
    B --> C[End]
```

#### Code Example Standards

1. **Real Code Only**: All code examples MUST be from actual implementation
   - **Forbidden**: Hypothetical code, pseudo-code, "simplified" examples
   - **Required**: Copy directly from source files
   - **Reference**: Include file paths for context (e.g., `See: include/graph/core/Database.hpp:123-145`)

2. **Code Accuracy**:
   - Verify all examples compile and work
   - Keep examples up-to-date with code changes
   - Include necessary headers and context

3. **Code Formatting**:
   - Use proper language tags: ` ```cpp `, ` ```bash `, ` ```python `
   - Include helpful comments
   - Keep examples focused and concise

#### Bilingual Documentation Requirements

1. **Parallel Structure**:
   - English files: `docs/en/...`
   - Chinese files: `docs/zh/...`
   - Mirror directory structure exactly
   - Same filenames in both languages

2. **Translation Quality**:
   - **Forbidden**: Machine translation (Google Translate, DeepL, etc.)
   - **Required**: Technically accurate human translation
   - Use proper technical terminology
   - Maintain consistency with existing translations

3. **Synchronization**:
   - Both versions must be updated together
   - Maintain parallel structure and content
   - Cross-references must work in both languages

4. **Navigation Configuration**:
   - Update both `docs/.vitepress/config/en.ts` and `docs/.vitepress/config/zh.ts`
   - Keep sidebar structure synchronized
   - Use consistent link text (translated appropriately)

#### Documentation Review Checklist

Before submitting documentation changes:

**Visual Standards:**
- [ ] No ASCII art diagrams present
- [ ] All Mermaid diagrams use only black/white/gray colors
- [ ] Diagrams are clean and professional
- [ ] Diagrams render correctly in the documentation site

**Content Standards:**
- [ ] Code examples are from actual implementation
- [ ] Code examples include file paths/references
- [ ] Both English and Chinese versions exist
- [ ] Both versions are synchronized
- [ ] Translation is technically accurate (not machine-translated)

**Technical Standards:**
- [ ] All links and cross-references work
- [ ] VitePress config files updated (if adding new files)
- [ ] Diagrams use appropriate Mermaid types
- [ ] Code blocks have correct language tags
- [ ] Tables are properly formatted
- [ ] Spelling and grammar checked

## Review Process

### Documentation Review Checklist

- [ ] All public APIs documented
- [ ] Examples are accurate and tested
- [ ] Code blocks are syntax-highlighted
- [ ] Diagrams are clear and accurate
- [ ] Spelling and grammar checked
- [ ] Links are valid
- [ ] Formatting is consistent

### Updating Documentation

When making code changes:

1. **Update code comments**: Keep them in sync with code
2. **Update API docs**: Document new APIs or changes
3. **Update examples**: Ensure examples still work
4. **Update diagrams**: Reflect architectural changes

## Tools

### Documentation Generator

- **Doxygen**: For API documentation from code comments
- **VitePress**: For user and architecture documentation

### Diagram Tools

- **Mermaid**: For text-based diagrams
- **PlantUML**: For complex UML diagrams

### Spell Checking

```bash
# Check spelling in documentation
scripts/check_spelling.sh docs/
```

### Link Checking

```bash
# Check for broken links
scripts/check_links.sh docs/
```

## Best Practices

### 1. Document as You Code

Write documentation alongside code, not as an afterthought.

### 2. Keep Examples Working

Test all code examples to ensure they work as documented.

### 3. Use Version-Specific Docs

Maintain documentation for different versions when needed.

### 4. Get Feedback

Have users review documentation for clarity and completeness.

### 5. Document Decisions

Record important architectural decisions and their rationale.

## Metrics

Track documentation quality:

- **Coverage**: Percentage of APIs documented
- **Accuracy**: Percentage of examples that work
- **Completeness**: All concepts documented
- **Clarity**: User comprehension scores

## See Also

- [Development Setup](/en/contributing/development-setup) - Getting started
- [Code Style](/en/contributing/code-style) - Coding standards
- [Writing Tests](/en/contributing/writing-tests) - Test documentation
