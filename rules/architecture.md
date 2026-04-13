# Architecture Reference

## Layered Architecture

```mermaid
graph TD
    A[Applications - CLI, Benchmark] --> B[Public API - C++ & C]
    B --> C[Query Engine - Parser, Planner, Optimizer, Executor]
    C --> D[Core - Database, Transaction, Entity Management]
    D --> E[Storage Layer - FileStorage, WAL, Cache, Constraints, Indexes]
    E --> F[Utilities - Compression, Checksums, Serialization, Vector Engine]
```

## Key Components

### Database (`include/graph/core/Database.hpp`)

- Main entry point for database operations
- Manages lifecycle: open, close, transactions
- Coordinates between storage engine and query engine
- Provides access to `FileStorage` and `QueryEngine`

### Storage System (`include/graph/storage/FileStorage.hpp`)

- **Segment-based storage**: Custom file format with checksums and compression
- **Component hierarchy**:
    - `FileHeaderManager`: Manages file-level metadata
    - `SegmentTracker`: Tracks segment allocation and usage
    - `SpaceManager`: Manages free space across segments
    - `DataManager`: Coordinates node/edge/property/blob data managers
        - `NodeManager`, `EdgeManager`, `PropertyManager`, `BlobManager`: Entity-specific CRUD
        - `StateManager`: Temporal state tracking via state chains
        - `TransactionContext`: Transaction-scoped state for data operations
    - `IndexManager`: Manages label indexes, property indexes, and segment indexes
        - `LabelIndex`, `PropertyIndex`: Core index types
        - `EntityTypeIndexManager`: Per-type index management
        - `VectorIndexManager`: Vector similarity index management
        - `IndexBuilder`: Bulk index construction
    - `ConstraintManager`: Schema constraint enforcement
        - `UniqueConstraint`, `NotNullConstraint`, `NodeKeyConstraint`, `TypeConstraint`
    - `DeletionManager`: Tombstone management and space reclamation
    - `CacheManager`: LRU cache with dirty entity tracking
    - `PersistenceManager`: Manages entity persistence to disk
    - `SnapshotManager`: Committed state snapshot management
    - `DirtyEntityRegistry`: Tracks modified entities for efficient persistence
    - `TokenRegistry`: Maps string labels/types to integer tokens
    - `WALManager`: Write-Ahead Log for durability and crash recovery
    - `StorageBootstrap`: Initialization and bootstrap logic
    - `PageBufferPool`: Buffered I/O for segment reads/writes
- **ACID compliance**: Full transaction support with WAL
- **State management**: Temporal state tracking via state chains (`StateChainManager`)

### Query Engine (`include/graph/query/`)

- **Parser** (`src/query/parser/`): ANTLR4-based Cypher parser
    - `common/`: Base parser interface (`IQueryParser.hpp`)
    - `cypher/`: Cypher-specific implementation
        - `*.g4`: ANTLR4 grammar files (lexer and parser)
        - `helpers/public/`: AST extraction, expression building, pattern building
        - `helpers/internal/`: Internal parser utilities
        - `clauses/`: Cypher clause handlers (reading, writing, result, admin)
        - `generated/`: ANTLR4 auto-generated code (DO NOT EDIT)
- **Planner** (`include/graph/query/planner/`): Converts parsed Cypher to logical plans
    - `QueryPlanner`: Main planning entry point
    - `PhysicalPlanConverter`: Converts logical plans to physical operators
    - `PipelineValidator`: Validates query pipeline correctness
    - `ProcedureRegistry`: Registry for callable procedures (e.g., GDS algorithms)
    - `ScopeStack`: Variable scope management during planning
- **Logical Plan** (`include/graph/query/logical/`): Intermediate representation
    - `LogicalOperator` base class with operator-specific subclasses
    - `LogicalPlanPrinter` for EXPLAIN output
- **Optimizer** (`include/graph/query/optimizer/`): Rule-based and cost-based optimization
    - `CostModel`, `Statistics`, `StatisticsCollector` for cost estimation
    - Rules: `FilterPushdownRule`, `IndexPushdownRule`, `EnhancedIndexSelectionRule`, `ProjectionPushdownRule`, `JoinReorderRule`, `PredicateSimplificationRule`
- **Executor** (`include/graph/query/execution/`): Executes physical plans
    - Scan: `NodeScanOperator`, `TraversalOperator`, `VarLengthTraversalOperator`
    - Mutation: `CreateNodeOperator`, `CreateEdgeOperator`, `MergeNodeOperator`, `MergeEdgeOperator`, `DeleteOperator`, `SetOperator`, `RemoveOperator`
    - Query: `FilterOperator`, `SortOperator`, `ProjectOperator`, `LimitOperator`, `SkipOperator`, `UnwindOperator`, `UnionOperator`, `CartesianProductOperator`, `OptionalMatchOperator`, `CallSubqueryOperator`, `ForeachOperator`
    - Aggregation: `AggregateOperator` with `AggregateAccumulator`
    - Index/Schema: `CreateIndexOperator`, `DropIndexOperator`, `ShowIndexesOperator`, `CreateConstraintOperator`, `DropConstraintOperator`, `ShowConstraintsOperator`, `CreateVectorIndexOperator`
    - Vector: `VectorSearchOperator`, `TrainVectorIndexOperator`
    - GDS Algorithms: `GdsOperators` (shortest path via `AlgoShortestPathOperator`)
    - CSV: `LoadCsvOperator` with `CsvReader`
    - Admin: `ExplainOperator`, `ProfileOperator`, `ListConfigOperator`, `SetConfigOperator`, `ShowStatsOperator`, `ResetStatsOperator`, `TransactionControlOperator`
    - Utility: `SingleRowOperator`, `RecordInjectorOperator`
- **Expressions** (`include/graph/query/expressions/`): Expression evaluation
    - `ExpressionEvaluator` with `EvaluationContext`
    - `FunctionRegistry`: Built-in and user-defined functions
    - Quantifier functions: `AllFunction`, `AnyFunction`, `NoneFunction`, `SingleFunction`
    - List operations: `ListComprehensionExpression`, `ListSliceExpression`, `ReduceExpression`
    - Pattern: `PatternComprehensionExpression`, `ExistsExpression`, `IsNullExpression`
- **Algorithm** (`include/graph/query/algorithm/`): Neo4j GDS-compatible graph algorithms
    - `GraphProjection` / `GraphProjectionManager`: Named graph projections
    - `GraphAlgorithm`: Algorithm execution framework
- **Plan Cache** (`include/graph/query/cache/`): Query plan caching (`PlanCache` stores `QueryPlan` with mutation
  flags)
- **Access Control** (`include/graph/query/ExecMode.hpp`): `ExecMode` enum for read-only/read-write enforcement;
  `QueryPlan` carries `mutatesData`/`mutatesSchema` flags accumulated by `LogicalPlanBuilder`

### Transaction System (`include/graph/core/Transaction.hpp`)

- ACID transactions with full rollback capabilities
- Optimistic locking with versioning
- `TransactionManager` coordinates transaction lifecycle
- Coordinates with WAL for durability
- **Read-only transactions**: `beginReadOnlyTransaction()` creates a transaction that rejects all write queries

### Access Control (`include/graph/query/ExecMode.hpp`)

3-layer defense for read-only enforcement:

1. **Plan layer** (`QueryEngine::executePlan()`): `ExecMode` enum (EM_FULL / EM_READ_WRITE / EM_READ_ONLY) checked
   against `QueryPlan` mutation flags (`mutatesData`, `mutatesSchema`) — O(1) check before execution
2. **API layer**: `beginReadOnlyTransaction()` exposed in `graph::Database`, `zyx::Database` (C++ API), and C API
   (`zyx_begin_read_only_transaction`)
3. **Storage layer**: `DataManager::guardReadOnly()` on all 12 write methods — final safety net backed by
   `TransactionContext::readOnly_` flag

Key files:
- `include/graph/query/ExecMode.hpp`: ExecMode enum + helper functions
- `include/graph/query/QueryPlan.hpp`: QueryPlan struct (plan root + mutation flags)
- `include/graph/query/ir/LogicalPlanBuilder.hpp`: Stateful builder that accumulates mutation flags during plan
  construction
- `include/graph/query/planner/ProcedureRegistry.hpp`: ProcedureDescriptor with mutation metadata

### Vector Engine (`include/graph/vector/`)

- **Core**: `BFloat16` half-precision type, `VectorMetric` distance functions
- **Index**: `DiskANNIndex` for disk-based approximate nearest neighbor search
- **Quantization**: `NativeProductQuantizer`, `KMeans` for vector compression
- **Registry**: `VectorIndexRegistry` manages vector index instances
- **Configuration**: `VectorIndexConfig` for index parameters

### Configuration (`include/graph/config/SystemConfigManager.hpp`)

- Dynamic runtime configuration
- Entity observer pattern for reactive updates
- Per-module configurations (logging, memory, etc.)

### Concurrency (`include/graph/concurrent/ThreadPool.hpp`)

- Thread pool for parallel query execution and background tasks

### Traversal (`include/graph/traversal/RelationshipTraversal.hpp`)

- Graph traversal algorithms for relationship-based queries

## Data Flow

1. **Database open**: `Database::open()` → `StorageBootstrap` → `FileStorage::open()` → Initialize all components
2. **Query execution**: Cypher string → Parser → QueryPlanner → Optimizer → `QueryPlan` (with mutation flags) →
   ExecMode check → PhysicalPlanConverter → QueryExecutor → Storage operations
3. **Transaction**: `beginTransaction()` → Operations in context → commit/rollback → WAL updates
4. **Persistence**: Entity changes → `DirtyEntityRegistry` → `PersistenceManager` → `CacheManager` → FileStorage write → WAL

## Public API

- **C++ API**: `include/zyx/zyx.hpp` - Embeddable interface for C++ applications
- **C API**: `include/zyx/zyx_c_api.h` - C-compatible interface for FFI (Rust, WASM, etc.)
- **Types**: `include/zyx/value.hpp` - Data type definitions
- **Query API**: `include/graph/query/api/` - `QueryEngine`, `QueryBuilder`, `QueryResult`, `ResultValue`
- CLI tool: `buildDir/apps/cli/zyx` - Interactive REPL

## Directory Structure

```
include/
├── zyx/                     # Public headers (API)
│   ├── zyx.hpp              # C++ API
│   ├── zyx_c_api.h          # C API
│   └── value.hpp            # Value types
└── graph/                   # Internal headers
    ├── core/               # Database, Transaction, Entity, State, Index, Types
    ├── storage/            # FileStorage, WAL, cache, deletion, persistence
    │   ├── constraints/    # Schema constraints (unique, not-null, node-key, type)
    │   ├── data/           # Entity managers (node, edge, property, blob, state)
    │   ├── dictionaries/   # TokenRegistry (label/type string↔token mapping)
    │   ├── indexes/        # Label, property, vector, and segment indexes
    │   ├── state/          # System state management
    │   └── wal/            # Write-Ahead Log
    ├── query/              # Query engine
    │   ├── algorithm/      # GDS-compatible graph algorithms and projections
    │   ├── api/            # QueryEngine, QueryBuilder, QueryResult
    │   ├── cache/          # Query plan cache
    │   ├── execution/      # Physical operators and executor
    │   │   └── operators/  # All physical operator implementations
    │   ├── expressions/    # Expression evaluation and function registry
    │   ├── logical/        # Logical plan operators
    │   │   └── operators/  # All logical operator implementations
    │   ├── optimizer/      # Cost model, statistics, optimization rules
    │   │   └── rules/      # Individual optimizer rules
    │   └── planner/        # Query planner and physical plan converter
    ├── traversal/          # Graph traversal algorithms
    ├── vector/             # Vector index engine
    │   ├── core/           # BFloat16, distance metrics
    │   ├── index/          # DiskANN index implementation
    │   └── quantization/   # Product quantization, KMeans
    ├── concurrent/         # Thread pool
    ├── config/             # Configuration management
    ├── cli/                # CLI-specific components (REPL, commands)
    ├── debug/              # Performance tracing
    ├── log/                # Logging
    └── utils/              # Checksum, compression, serialization utilities

src/
├── core/                   # Core implementation
├── storage/                # Storage layer implementation
│   ├── constraints/        # Constraint implementations
│   ├── data/               # Entity manager implementations
│   ├── dictionaries/       # TokenRegistry implementation
│   ├── indexes/            # Index implementations
│   ├── state/              # System state implementation
│   └── wal/                # WAL implementation
├── query/                  # Query engine implementation
│   ├── algorithm/          # Graph algorithm implementations
│   ├── api/                # QueryEngine, QueryBuilder implementations
│   ├── execution/          # Executor and operator implementations
│   │   └── operators/      # Physical operator implementations
│   ├── expressions/        # Expression evaluator implementations
│   ├── optimizer/          # Optimizer implementations
│   ├── parser/             # Query parser implementations
│   │   ├── common/         # Base parser interface (IQueryParser)
│   │   └── cypher/         # Cypher parser
│   │       ├── *.g4        # ANTLR4 grammar files
│   │       ├── helpers/    # AST extraction, expression/pattern building
│   │       │   ├── public/ # Public helper interfaces
│   │       │   └── internal/ # Internal implementation helpers
│   │       ├── clauses/    # Clause handlers (reading, writing, result, admin)
│   │       └── generated/  # ANTLR4 auto-generated code (DO NOT EDIT)
│   └── planner/            # Planner implementations
├── traversal/              # Traversal implementation
├── vector/                 # Vector engine implementation
│   └── index/              # DiskANN implementation
├── api/                    # C API implementation
├── cli/                    # CLI implementation
├── config/                 # Configuration implementation
├── debug/                  # Debug/perf tracing implementation
└── utils/                  # Utility implementations

apps/
├── cli/                    # CLI application (REPL)
└── benchmark/              # Performance benchmarking

tests/
├── include/                # Shared test utilities and helpers
├── integration/            # Integration tests (end-to-end workflows)
└── src/                    # Unit tests (mirrors source structure)
    ├── api/                # C/C++ API tests
    ├── cli/                # CLI tests
    ├── concurrent/         # Thread pool tests
    ├── config/             # Configuration tests
    ├── core/               # Database, transaction, entity tests
    ├── log/                # Logging tests
    ├── query/              # Query engine tests
    │   ├── algorithm/      # Graph algorithm tests
    │   ├── api/            # Query API tests
    │   ├── cache/          # Plan cache tests
    │   ├── execution/      # Executor tests
    │   │   └── operators/  # Individual operator tests
    │   ├── expressions/    # Expression evaluation tests
    │   ├── logical/        # Logical plan tests
    │   ├── optimizer/      # Optimizer rule tests
    │   ├── parser/         # Parser tests
    │   │   └── cypher/     # Cypher-specific parser tests
    │   │       ├── clauses/
    │   │       ├── helpers/
    │   │       └── syntax/
    │   └── planner/        # Planner tests
    ├── storage/            # Storage tests
    │   ├── constraints/    # Constraint tests
    │   ├── data/           # Entity manager tests
    │   ├── dictionaries/   # TokenRegistry tests
    │   ├── Indexes/        # Index tests
    │   ├── state/          # System state tests
    │   └── wal/            # WAL tests
    ├── traversal/          # Traversal tests
    ├── utils/              # Utility tests
    └── vector/             # Vector engine tests
        ├── core/           # BFloat16, metrics tests
        ├── index/          # DiskANN tests
        └── quantization/   # Quantizer tests

scripts/
├── run_tests.sh            # Build + test + coverage
├── build_release.sh        # Release build
├── build_wasm.sh           # WASM build (zyx.js + zyx.wasm)
├── setup_emsdk.sh          # Emscripten SDK + antlr4 WASM setup
└── build_playground_db.mjs # Pre-build playground database files

docs/apps/docs/
├── home/                   # Homepage components
│   ├── custom-home.tsx     # Homepage layout
│   ├── playground.tsx      # Cypher Playground component
│   └── graph-view.tsx      # D3 graph visualization
└── public/
    ├── wasm/               # Deployed WASM module
    │   ├── zyx.js          # WASM JS loader
    │   └── zyx.wasm        # WASM binary
    └── data/               # Pre-built playground databases
        ├── got.db          # Game of Thrones dataset
        └── marvel.db       # Marvel Universe dataset

compiler_options_wasm.ini   # Meson cross-file for Emscripten
```

## Dependencies (via Conan)

- **boost**: Filesystem, system utilities
- **zlib**: Compression
- **gtest/gmock**: Testing framework
- **cli11**: Command line interface
- **antlr4-cppruntime**: Cypher parser generation

## Dependencies (WASM, via Emscripten)

- **Emscripten SDK**: C++ → WASM compiler toolchain (`scripts/setup_emsdk.sh`)
- **antlr4-cppruntime**: Cross-compiled to WASM static library (`emsdk/antlr4-wasm/`)
- **zlib**: Built-in Emscripten port (`-sUSE_ZLIB=1`)

## Implementation Notes

1. **Segment Architecture**: All data is stored in fixed-size segments with bitmap tracking for space management
2. **State Chains**: All persistent modifications go through state chains for versioning and rollback
3. **Dirty Tracking**: Modified entities are tracked via `DirtyEntityRegistry` for efficient persistence
4. **LRU Cache**: Hot entities are cached in memory with configurable eviction policy
5. **WAL Integration**: Write operations are logged to WAL before actual disk writes for durability
6. **Parser Generation**: ANTLR4 grammar files are in `src/query/parser/cypher/` - generated code should not be manually edited
7. **Vector Engine**: DiskANN-based approximate nearest neighbor search with product quantization support
8. **Graph Algorithms**: Neo4j GDS-compatible graph algorithms with named projection support

## Library Build Outputs

- **`zyx_core`**: Internal static library for tests and CLI (not installed)
- **`libzyx`**: Public shared library for embedding (installed to system)
- **pkg-config**: Generated for easy integration (`pkg-config --libs --cflags zyx`)

## WASM Build Target

Compiles ZYX to WebAssembly for browser-based usage via Emscripten.

### Prerequisites

```bash
./scripts/setup_emsdk.sh   # Installs emsdk + builds antlr4-cppruntime for WASM
```

Outputs: `emsdk/` (Emscripten SDK), `emsdk/antlr4-wasm/` (antlr4 static lib for WASM)

### Build

```bash
./scripts/build_wasm.sh    # Output: build_wasm/zyx.js + build_wasm/zyx.wasm
```

### How it works

1. Generates pkg-config stubs for WASM-compatible dependencies (antlr4, zlib, CLI11)
2. Configures Meson with `compiler_options_wasm.ini` cross-file (emcc/em++ toolchain, wasm32 target)
3. Compiles `libzyx_core.a` + `libcypher_parser.a` as static WASM libraries
4. Links with `em++` into final `zyx.js` + `zyx.wasm` module using `-sMODULARIZE`

### Exported C API functions

All functions listed in `build_wasm.sh` `-sEXPORTED_FUNCTIONS`. Key categories:
- **Lifecycle**: `zyx_open`, `zyx_open_if_exists`, `zyx_close`
- **Execution**: `zyx_execute`, `zyx_execute_params`
- **Transactions**: `zyx_begin_transaction`, `zyx_begin_read_only_transaction`, `zyx_txn_execute`,
  `zyx_txn_commit`, `zyx_txn_rollback`, `zyx_txn_close`, `zyx_txn_is_read_only`
- **Parameters**: `zyx_params_create`, `zyx_params_set_*`, `zyx_params_close`
- **Batch**: `zyx_create_node`, `zyx_create_node_ret_id`, `zyx_create_edge_by_id`
- **Result iteration**: `zyx_result_next`, `zyx_result_column_count`, `zyx_result_column_name`,
  `zyx_result_get_duration`
- **Data access**: `zyx_result_get_type`, `zyx_result_get_int/double/bool/string`,
  `zyx_result_get_node`, `zyx_result_get_edge`, `zyx_result_get_props_json`,
  `zyx_result_get_map_json`
- **List access**: `zyx_result_list_size`, `zyx_result_list_get_*`
- **Status**: `zyx_result_is_success`, `zyx_result_get_error`, `zyx_get_last_error`
- **Memory**: `malloc`, `free`

**When adding new C API functions**: Update both `include/zyx/zyx_c_api.h` (declaration),
`src/api/CApi.cpp` (implementation), AND `scripts/build_wasm.sh` `-sEXPORTED_FUNCTIONS` (WASM export).

### Key files

| File | Purpose |
|------|---------|
| `scripts/setup_emsdk.sh` | One-time emsdk + antlr4 WASM setup |
| `scripts/build_wasm.sh` | WASM build script with exported function list |
| `compiler_options_wasm.ini` | Meson cross-file for Emscripten toolchain |
| `docs/apps/docs/public/wasm/zyx.js` | Deployed WASM JS loader |
| `docs/apps/docs/public/wasm/zyx.wasm` | Deployed WASM binary |

## Cypher Playground (Homepage)

Browser-based interactive Cypher query workspace embedded in the docs site homepage.

### Architecture

```mermaid
graph LR
    A[playground.tsx] -->|ccall| B[zyx.js WASM Module]
    B --> C[zyx.wasm - ZYX Engine]
    C -->|MEMFS| D[Pre-built .db files]
    A --> E[graph-view.tsx - D3 Visualization]
```

### Components

| File | Purpose |
|------|---------|
| `docs/apps/docs/home/playground.tsx` | Main playground component (query editor, result display, dataset switching) |
| `docs/apps/docs/home/graph-view.tsx` | D3.js force-directed graph visualization |
| `docs/apps/docs/home/custom-home.tsx` | Homepage layout integrating the playground |
| `docs/apps/docs/public/data/*.db` | Pre-built database files fetched at runtime |

### Execution flow

1. WASM module loaded from `/wasm/zyx.js`
2. Pre-built `.db` + `.db-wal` files fetched and written to Emscripten MEMFS
3. Database opened via `zyx_open()`
4. **Read-only transaction** opened via `zyx_begin_read_only_transaction()` — all user queries are routed through
   `zyx_txn_execute()` to prevent any data mutation
5. Results parsed from C API (nodes, edges, scalars) and rendered as graph or table

### Read-only enforcement

The Playground enforces read-only mode at the engine level (not client-side filtering):
- Uses `zyx_begin_read_only_transaction` → `zyx_txn_execute` for all queries
- Write queries (CREATE, SET, DELETE, MERGE, DROP INDEX, etc.) are rejected by the engine with error:
  `"Read-only transaction cannot execute write queries"`
- 3-layer defense: QueryPlan mutation flags → ExecMode check → DataManager guard

### Pre-built datasets

Database files in `docs/apps/docs/public/data/` are pre-built offline. To regenerate:
```bash
node scripts/build_playground_db.mjs
```
