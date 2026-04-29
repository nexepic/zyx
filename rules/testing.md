# Test Requirement Extraction Guide

## Testing Context

**Unit Tests** (`tests/src/`): Test individual components in isolation
- Mock external dependencies where appropriate
- Test internal logic, edge cases, and error conditions
- Focus on code paths and branch coverage
- Located in `tests/src/<module>/` matching the source structure
- Directory mirrors `src/` layout (e.g., `tests/src/storage/wal/`, `tests/src/query/execution/operators/`)

**Integration Tests** (`tests/integration/`): Test complete system workflows
- Test real database operations end-to-end
- Verify ACID properties, persistence, and data recovery
- Test complex query workflows and multi-component interactions
- Use real database instances with temporary files

**Shared Test Utilities** (`tests/include/`): Reusable test fixtures
- `ApiTestFixture.hpp`: Base fixture for C/C++ API tests (database lifecycle, temp dirs)
- `QueryTestFixture.hpp`: Base fixture for query execution tests (database + query engine setup)

## Test File Size Limit

Single test files MUST NOT exceed **2000 lines**. When a test file grows beyond this limit, split it:

1. Extract the fixture class into a shared header under `tests/include/<module>/` (e.g., `tests/include/storage/FooTestFixture.hpp`)
2. Split tests by logical category into separate files with suffix naming (e.g., `test_Foo_Insert.cpp`, `test_Foo_Query.cpp`)
3. Keep the slimmed original for core/misc tests
4. Each split file includes the shared fixture header (path relative to `tests/include/`)
5. Verify: every test appears in exactly one file, zero tests lost

## Running and Filtering Tests

The project compiles all tests into a single, unified executable (`zyx_test_suite`) rather than generating hundreds of individual binaries. This drastically reduces linking time and disk space usage.

**Run all tests (Full Suite):**
```bash
./scripts/run_tests.sh --quick
# or directly via meson
meson test -C buildDir
```

**Run a specific test suite or test case:**
To avoid building the entire project, you can use `ninja` to specifically compile only the test suite binary, then use Google Test's `--gtest_filter` argument. You can use wildcards (`*`).
```bash
# Compile just the test suite
ninja -C buildDir tests/zyx_test_suite

# Run all tests inside the ReplTest suite
./buildDir/tests/zyx_test_suite --gtest_filter="REPLTest.*"

# Run a specific test case
./buildDir/tests/zyx_test_suite --gtest_filter="ImportCommandTest.SkipBadEntries"

# You can combine compilation and execution in one line:
ninja -C buildDir tests/zyx_test_suite && ./buildDir/tests/zyx_test_suite --gtest_filter="*Index*"
```

## Extract Test Requirements from Code

**For Classes/Structures:**
- Public API methods → Functional tests
- Private/internal methods → Consider white-box testing if critical
- Constructors/destructors → Lifecycle tests
- Getters/setters → Property access tests

**For Control Flow:**
- `if/else` branches → Test both true and false paths
- `for/while` loops → Test with 0, 1, and N iterations
- Switch/case statements → Test each case
- Exception handling → Test error conditions

**For State Management:**
- State transitions → Test valid and invalid transitions
- Default values → Verify initialization
- State queries → Test state retrieval

**For Data Structures:**
- Empty collections → Test edge cases
- Single element → Test minimal case
- Multiple elements → Test normal operation
- Boundary conditions → Test limits (max size, etc.)
