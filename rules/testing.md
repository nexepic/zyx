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
