# CLAUDE.md

ZYX is a high-performance, embeddable graph database engine written in C++20 with Cypher query support.

**CRITICAL Platform Requirement**: Must support **macOS**, **Linux**, and **Windows**. All changes must preserve
cross-platform compatibility.

## Build & Test Commands

```bash
./scripts/run_tests.sh              # Full build + tests + coverage
./scripts/run_tests.sh --quick      # Quick build (skip Conan)
./scripts/run_tests.sh --no-conan   # Use system dependencies
./scripts/run_tests.sh --html       # HTML coverage report
./scripts/run_tests.sh --quick --file xxx.cpp  # Coverage report for specific file
ninja -C buildDir tests/zyx_test_suite && ./buildDir/tests/zyx_test_suite --gtest_filter="SuiteName.*" # Compile and run specific test suite
ninja -C buildDir tests/zyx_test_suite && ./buildDir/tests/zyx_test_suite --gtest_filter="SuiteName.TestName" # Compile and run specific test case
./scripts/build_release.sh          # Release build
./scripts/setup_emsdk.sh            # Install Emscripten SDK + antlr4 WASM (one-time)
./scripts/build_wasm.sh             # Build WASM module (zyx.js + zyx.wasm)
python3 scripts/bump_version.py vX.Y.Z  # Bump version + commit + tag
```

Build system: **Meson** + **Conan** + **Ninja**. Tests: **Google Test**, auto-discovered from `test_*.cpp` and compiled into a single `zyx_test_suite` executable for faster linking and smaller footprint.

## Verification

Before completing any code change:
1. Run `./scripts/run_tests.sh --quick` and confirm all tests pass
2. If `.g4` grammar files were modified, run `bash src/query/parser/cypher/generate.sh` first
3. Coverage target: 95%+ across line, branch, function, region metrics

## Unit Testing

**CRITICAL**: Never blindly adjust test code to make tests pass. Always identify root cause:
- Source code bug → Fix the source code
- Test code wrong → Fix the test code

Coverage goals: 95%+ with focus on branch coverage. Max **2000 lines** per test file — split into category files with
shared fixture header when exceeded (see `rules/testing.md`).

## Windows Enum Naming

**CRITICAL**: All enum members MUST use prefixes to avoid Windows macro conflicts (`min`, `max`, `AND`, `OR`,
`DELETE`, `CREATE`, `FILTER`, etc.):

```cpp
BinaryOperatorType { BOP_ADD, BOP_AND, BOP_OR, BOP_XOR }
UnaryOperatorType { UOP_MINUS, UOP_NOT }
AggregateFunctionType { AGG_COUNT, AGG_MIN, AGG_MAX }
ComprehensionType { COMP_FILTER, COMP_EXTRACT, COMP_REDUCE }
```

**When adding new enum types**: ALWAYS use descriptive prefixes (3-5 chars) followed by underscore.

## Version Management

`meson.build` line 2 is the **single source of truth** for the project version. All other locations derive from it
automatically (`src/meson.build` via `meson.project_version()`, `pyproject.toml` via `dynamic`, `__init__.py` via
`importlib.metadata`) or are synced by the bump script (docs `package.json` files).

**Never hardcode version strings** — to bump the version:
```bash
python3 scripts/bump_version.py v1.0.0        # Update all files + commit + tag
python3 scripts/bump_version.py v1.0.0 --no-commit  # Update files only
```

## Git Commits

Follow `CONTRIBUTING.md` strictly:
1. Imperative mood: "Fix bug" not "Fixed bug"
2. Be specific: describe what and why
3. One logical change per commit
4. **NO Co-Authored-By trailers**

## ANTLR4 Parser

When modifying `.g4` grammar files, you MUST regenerate: `bash src/query/parser/cypher/generate.sh`

After regenerating, run tests and commit grammar changes AND generated files together.

## Architecture

See `rules/architecture.md` for full component documentation, data flow, directory structure, and API reference.

Key entry points:
- **Database**: `include/graph/core/Database.hpp`
- **Storage**: `include/graph/storage/FileStorage.hpp`
- **Query Engine**: `include/graph/query/`
- **C++ API**: `include/zyx/zyx.hpp`
- **C API**: `include/zyx/zyx_c_api.h`
- **WASM**: `scripts/build_wasm.sh` → `build_wasm/zyx.js` + `build_wasm/zyx.wasm`

## WASM Build

Builds ZYX as a WebAssembly module for browser-based Cypher playground. See `rules/architecture.md` § WASM Build
Target for details.

```bash
./scripts/setup_emsdk.sh    # One-time: install emsdk + build antlr4 for WASM
./scripts/build_wasm.sh     # Build: output → build_wasm/zyx.js + zyx.wasm
```

After building, copy outputs to the docs site:
```bash
cp build_wasm/zyx.js build_wasm/zyx.wasm docs/apps/docs/public/wasm/
```

**Key constraints**:
- Cross-file: `compiler_options_wasm.ini` (Meson cross-compilation config for emscripten)
- Exported symbols are listed in `build_wasm.sh` `-sEXPORTED_FUNCTIONS` — update this list when adding new C API
  functions that need browser access
- The Playground uses **read-only transactions** (`zyx_begin_read_only_transaction` + `zyx_txn_execute`) to prevent
  any data mutation from user queries

## Documentation Standards

See `rules/documentation-standards.md` for full guidelines. Key rules:
- **NO ASCII art** — use Mermaid diagrams only (black/white/gray colors)
- **Bilingual**: all docs must exist in both `en/` and `zh/`
- **Real code only** in examples — no pseudo-code
- Unsupported Cypher features: see `UNSUPPORTED_CYPHER_FEATURES.md`
