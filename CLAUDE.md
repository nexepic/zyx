# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Metrix is a high-performance, embeddable graph database engine written in C++20 with Cypher query support. It uses a custom file-based storage system with segment-based architecture, ACID transactions via Write-Ahead Logging (WAL), and supports advanced features like vector indexes.

## Build System

The project uses **Meson** as the build system with **Conan** for dependency management and **Ninja** as the backend.

### Essential Commands

```bash
# Full build with dependency installation, tests, and coverage
./scripts/run_tests.sh

# Quick build (skip Conan install if dependencies already exist)
./scripts/run_tests.sh --quick

# Generate HTML coverage report
./scripts/run_tests.sh --html

# Manual build steps
conan install . --output-folder=buildDir --build=missing -s build_type=Debug -s compiler.cppstd=20
meson setup buildDir --native-file buildDir/conan_meson_native.ini
meson compile -C buildDir

# Run specific test (tests are auto-discovered from test_*.cpp files)
meson test -C buildDir <test_name>

# Build release version
./scripts/build_release.sh
```

### Test Structure

- Tests use **Google Test** framework
- Test files follow naming pattern `test_*.cpp` and are automatically discovered by Meson
- Each test file builds as a separate executable
- Tests link against internal static library `metrix_core`
- Coverage uses LLVM instrumentation (`-fprofile-instr-generate -fcoverage-mapping`)

## Architecture

### Layered Architecture

```
Applications (CLI, Benchmark)
         ↓
   Public API (C++ & C)
         ↓
   Query Engine (Parser → Planner → Executor)
         ↓
   Storage Layer (FileStorage, WAL, State Management)
         ↓
   Core (Database, Transaction, Entity Management)
```

### Key Components

#### Database (`include/graph/core/Database.hpp`)
- Main entry point for database operations
- Manages lifecycle: open, close, transactions
- Coordinates between storage engine and query engine
- Provides access to `FileStorage` and `QueryEngine`

#### Storage System (`include/graph/storage/FileStorage.hpp`)
- **Segment-based storage**: Custom file format with checksums and compression
- **Component hierarchy**:
  - `FileHeaderManager`: Manages file-level metadata
  - `SegmentTracker`: Tracks segment allocation and usage
  - `DataManager`: Handles node/edge/property data
  - `IndexManager`: Manages label and property indexes
  - `DeletionManager`: Tombstone management and space reclamation
  - `CacheManager`: LRU cache with dirty entity tracking
- **ACID compliance**: Full transaction support with WAL
- **State management**: Temporal state tracking via state chains

#### Query Engine (`include/graph/query/`)
- **Parser**: ANTLR4-based Cypher parser (generated code in `src/query/parser/cypher/`)
- **Planner**: Converts parsed Cypher to logical plans, optimizes with rule-based optimization
- **Executor**: Executes physical plans with various operators:
  - Scan operators: `NodeScanOperator`, traversal operations
  - Modification operators: `CreateNodeOperator`, `MergeNodeOperator`, `DeleteOperator`, etc.
  - Query operators: `FilterOperator`, `SortOperator`, `ProjectOperator`, etc.
  - Special operators: `VectorSearchOperator`, `TrainVectorIndexOperator` for vector indexes

#### Transaction System (`include/graph/core/Transaction.hpp`)
- ACID transactions with full rollback capabilities
- Optimistic locking with versioning
- Coordinates with WAL for durability

#### Configuration (`include/graph/config/SystemConfigManager.hpp`)
- Dynamic runtime configuration
- Entity observer pattern for reactive updates
- Per-module configurations (logging, memory, etc.)

### Data Flow

1. **Database open**: `Database::open()` → `FileStorage::open()` → Initialize all components
2. **Query execution**: Cypher string → Parser → QueryPlanner → Optimizer → QueryExecutor → Storage operations
3. **Transaction**: `beginTransaction()` → Operations in context → commit/rollback → WAL updates
4. **Persistence**: Entity changes → DirtyEntityRegistry → CacheManager → FileStorage write → WAL

### Public API

- **C++ API**: `include/metrix/metrix.hpp` - Embeddable interface for C++ applications
- **C API**: `include/metrix/metrix_c_api.h` - C-compatible interface for FFI
- **Types**: `include/metrix/value.hpp` - Data type definitions
- CLI tool: `buildDir/apps/cli/metrix-cli` - Interactive REPL

### Directory Structure

```
include/
├── metrix/              # Public headers (API)
└── graph/               # Internal headers
    ├── core/           # Database, Transaction, Entity, State, Index
    ├── storage/        # FileStorage, WAL, cache, deletion management
    ├── query/          # Query engine (parser, planner, executor)
    ├── traversal/      # Graph traversal algorithms
    ├── config/         # Configuration management
    └── cli/            # CLI-specific components

src/
├── core/               # Core implementation
├── storage/            # Storage layer implementation
├── query/              # Query engine implementation
├── traversal/          # Traversal implementation
├── api/                # C API implementation
├── cli/                # CLI implementation
└── query/parser/       # ANTLR-generated parser code

apps/
├── cli/                # CLI application
└── benchmark/          # Performance benchmarking

tests/
└── test_*.cpp          # Unit tests (auto-discovered)

scripts/                # Build and utility scripts
```

### Dependencies (via Conan)

- **boost**: Filesystem, system utilities
- **zlib**: Compression
- **gtest/gmock**: Testing framework
- **cli11**: Command line interface
- **antlr4-cppruntime**: Cypher parser generation

### Important Implementation Notes

1. **Segment Architecture**: All data is stored in fixed-size segments with bitmap tracking for space management
2. **State Chains**: All persistent modifications go through state chains for versioning and rollback
3. **Dirty Tracking**: Modified entities are tracked via `DirtyEntityRegistry` for efficient persistence
4. **LRU Cache**: Hot entities are cached in memory with configurable eviction policy
5. **WAL Integration**: Write operations are logged to WAL before actual disk writes for durability
6. **Parser Generation**: ANTLR4 grammar files are in `src/query/parser/cypher/` - generated code should not be manually edited

### Library Build Outputs

- **`metrix_core`**: Internal static library for tests and CLI (not installed)
- **`libmetrix`**: Public shared library for embedding (installed to system)
- **pkg-config**: Generated for easy integration (`pkg-config --libs --cflags Metrix`)
