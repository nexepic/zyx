# Project Structure

Understanding the ZYX codebase organization is essential for effective contribution. This document explains the directory structure and file organization.

## Top-Level Directory Structure

```
zyx/
├── include/              # Public header files
├── src/                  # Implementation source files
├── tests/                # Test files
├── apps/                 # Applications (CLI, benchmark)
├── scripts/              # Build and utility scripts
├── docs/                 # Documentation
└── buildDir/             # Build output (generated)
```

## Include Directory

### Public Headers (`include/zyx/`)

```cpp
include/zyx/
├── zyx.hpp            # Main public header
├── zyx_c_api.h        # C API header
├── value.hpp             # Value type definitions
├── result.hpp            # Query result types
├── transaction.hpp       # Transaction API
└── config.hpp            # Configuration API
```

### Internal Headers (`include/graph/`)

```
include/graph/
├── core/                 # Core components
│   ├── Database.hpp
│   ├── Transaction.hpp
│   ├── Entity.hpp
│   ├── State.hpp
│   └── Index.hpp
├── storage/              # Storage layer
│   ├── FileStorage.hpp
│   ├── WAL.hpp
│   ├── Cache.hpp
│   └── DeletionManager.hpp
├── query/                # Query engine
│   ├── QueryEngine.hpp
│   ├── planner/
│   │   ├── QueryPlanner.hpp
│   │   └── Optimizer.hpp
│   └── executor/
│       ├── QueryExecutor.hpp
│       └── operators/
├── traversal/            # Graph traversal
│   ├── TraversalEngine.hpp
│   └── algorithms/
└── config/               # Configuration
    └── SystemConfigManager.hpp
```

## Source Directory

```
src/
├── core/                 # Core implementation
│   ├── Database.cpp
│   ├── Transaction.cpp
│   └── Entity.cpp
├── storage/              # Storage implementation
│   ├── FileStorage.cpp
│   ├── WAL.cpp
│   ├── CacheManager.cpp
│   └── SegmentManager.cpp
├── query/                # Query engine
│   ├── QueryEngine.cpp
│   ├── planner/
│   │   ├── QueryPlanner.cpp
│   │   └── Optimizer.cpp
│   ├── executor/
│   │   ├── QueryExecutor.cpp
│   │   └── operators/
│   │       ├── ScanOperator.cpp
│   │       ├── FilterOperator.cpp
│   │       └── ...
│   └── parser/           # Query parser
│       ├── common/
│       │   └── IQueryParser.hpp
│       └── cypher/       # Cypher parser
│           ├── *.g4      # Grammar files
│           ├── helpers/  # Parser helpers
│           ├── clauses/  # Clause handlers
│           ├── generated/# Generated code
│           └── CypherParser.cpp
├── traversal/            # Traversal implementation
├── api/                  # C API implementation
└── cli/                  # CLI implementation
```

## Test Directory

```
tests/
├── integration/          # Integration tests
│   ├── test_IntegrationDatabase.cpp
│   ├── test_IntegrationCrud.cpp
│   ├── test_IntegrationQuery.cpp
│   └── test_IntegrationPersistence.cpp
└── src/                  # Unit tests
    ├── core/
    │   ├── test_Database.cpp
    │   ├── test_Transaction.cpp
    │   └── test_Entity.cpp
    ├── storage/
    │   ├── test_FileStorage.cpp
    │   ├── test_WAL.cpp
    │   └── test_Cache.cpp
    ├── query/
    │   ├── planner/
    │   │   └── test_QueryPlanner.cpp
    │   ├── executor/
    │   │   └── operators/
    │   │       ├── test_ScanOperator.cpp
    │   │       └── ...
    │   └── parser/
    │       └── cypher/
    │           └── test_CypherParser.cpp
    └── traversal/
        └── test_TraversalEngine.cpp
```

## Applications Directory

```
apps/
├── cli/                  # Command-line interface
│   ├── main.cpp
│   ├── REPL.cpp
│   └── CMakeLists.txt
└── benchmark/            # Performance benchmarking
    ├── main.cpp
    └── BenchmarkConfig.hpp
```

## Scripts Directory

```
scripts/
├── run_tests.sh          # Run all tests
├── build_release.sh      # Build release version
├── generate_coverage.sh  # Generate coverage report
└── install_dependencies.sh# Install dependencies
```

## File Naming Conventions

### Header Files

- **Public headers**: `PascalCase.hpp` (e.g., `Database.hpp`)
- **Internal headers**: `PascalCase.hpp` (e.g., `QueryPlanner.hpp`)

### Source Files

- **Implementation**: `PascalCase.cpp` matching header name
- **Tests**: `test_PascalCase.cpp` (e.g., `test_Database.cpp`)

### Generated Files

- **Parser generated**: `CypherLexer.cpp`, `CypherParser.cpp` (in `src/query/parser/cypher/generated/`)

## Component Responsibilities

### Core Components

**Database** (`include/graph/core/Database.hpp`)
- Main entry point
- Lifecycle management
- Component coordination

**Transaction** (`include/graph/core/Transaction.hpp`)
- ACID transaction management
- Operation batching
- Rollback handling

**Entity** (`include/graph/core/Entity.hpp`)
- Node and edge representation
- Property management
- State tracking

### Storage Components

**FileStorage** (`include/graph/storage/FileStorage.hpp`)
- File I/O operations
- Segment management
- Space allocation

**WAL** (`include/graph/storage/WAL.hpp`)
- Write-ahead logging
- Crash recovery
- Checkpoint management

**Cache** (`include/graph/storage/Cache.hpp`)
- LRU caching
- Dirty tracking
- Prefetching

### Query Components

**QueryEngine** (`include/graph/query/QueryEngine.hpp`)
- Query orchestration
- Result aggregation
- Error handling

**QueryPlanner** (`include/graph/query/planner/QueryPlanner.hpp`)
- Logical plan generation
- Optimization rules
- Plan selection

**QueryExecutor** (`include/graph/query/executor/QueryExecutor.hpp`)
- Physical plan execution
- Operator scheduling
- Result streaming

## Build Artifacts

```
buildDir/
├── zyx_core/          # Core static library
├── libzyx.so          # Public shared library
├── zyx-cli            # CLI executable
└── tests/                # Test executables
    ├── test_Database
    ├── test_Transaction
    └── ...
```

## Configuration Files

```
zyx/
├── meson.build           # Main build file
├── conanfile.txt         # Conan dependencies
├── .clang-format         # Code formatting rules
├── .clang-tidy           # Static analysis rules
└── CONTRIBUTING.md       # Contribution guidelines
```

## Adding New Features

### 1. Add Public API

```cpp
// include/zyx/new_feature.hpp
namespace zyx {
    class NewFeature {
    public:
        void doSomething();
    };
}
```

### 2. Implement

```cpp
// src/core/NewFeature.cpp
#include "graph/core/NewFeature.hpp"

void NewFeature::doSomething() {
    // Implementation
}
```

### 3. Add Tests

```cpp
// tests/src/core/test_NewFeature.cpp
#include <gtest/gtest.h>
#include "graph/core/NewFeature.hpp"

TEST(NewFeature, DoSomething) {
    NewFeature feature;
    feature.doSomething();
    // Assertions
}
```

### 4. Update Build Files

```python
# meson.build
zyx_core_lib = static_library(
    'zyx_core',
    'src/core/NewFeature.cpp',
    # ... other sources
)
```

## Code Organization Principles

1. **Separation of concerns**: Each component has a single responsibility
2. **Layered architecture**: Clear dependency direction (API → Query → Storage → Core)
3. **Interface segregation**: Small, focused interfaces
4. **Dependency injection**: Components receive dependencies through constructors
5. **Testability**: All components are unit testable

## Best Practices

1. **Follow naming conventions**: Consistent file and class naming
2. **Keep headers focused**: One class per header file
3. **Use forward declarations**: Reduce compilation dependencies
4. **Organize by feature**: Group related files together
5. **Document dependencies**: Clear #include statements

## See Also

- [Development Setup](/en/contributing/development-setup) - Getting started
- [Code Style](/en/contributing/code-style) - Coding standards
- [Testing Guidelines](/en/contributing/testing) - Testing practices
