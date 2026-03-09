# Contributing to Metrix Graph Database

Thank you for your interest in contributing to our project! To ensure consistency and cross-platform compatibility, please adhere to the following guidelines when writing unit tests.

## Unit Testing Guidelines

### Temporary File Handling

When writing tests that require file I/O (e.g., creating temporary databases), **never hardcode file paths** (such as `/tmp/test.dat`). Hardcoded paths often cause failures on Windows systems and can lead to race conditions if multiple tests run in parallel.

**Requirements:**

1.  **Use System Temp Directory:** Always use `std::filesystem::temp_directory_path()` to retrieve the correct temporary directory for the host OS.
2.  **Use UUIDs for Uniqueness:** Append a unique identifier (UUID) to filenames to prevent collisions between different test suites or parallel test runs.
3.  **Cleanup:** Always ensure temporary files are removed in the `TearDown()` method.

### Code Example

Below is the standard pattern for creating temporary files in a Google Test fixture:

```cpp
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

class MyStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 1. Generate a random UUID
        boost::uuids::uuid uuid = boost::uuids::random_generator()();
        
        // 2. Construct a cross-platform path ensuring uniqueness
        //    Format: path/to/temp/prefix_UUID.dat
        testFilePath = std::filesystem::temp_directory_path() / 
                       ("test_myFeature_" + to_string(uuid) + ".dat");

        // 3. Initialize your database/storage with this path
        database = std::make_unique<graph::Database>(testFilePath.string());
        database->open();
    }

    void TearDown() override {
        // 4. Close resources before deletion
        if (database) {
            database->close();
            database.reset();
        }

        // 5. Clean up the temporary file
        if (std::filesystem::exists(testFilePath)) {
            std::filesystem::remove(testFilePath);
        }
    }

    std::filesystem::path testFilePath;
    std::unique_ptr<graph::Database> database;
};
```

## Windows Platform Compatibility

### Enum Naming Guidelines

**CRITICAL**: When defining or modifying enum types, **NEVER use enum member names that conflict with Windows macros**.

Windows headers (especially `<windef.h>`, `<winnt.h>`) define these problematic macros:
- **Arithmetic/Logic**: `AND`, `OR`, `XOR`, `NOT`
- **Comparison**: `min`, `max` (yes, lowercase!)
- **Common Operations**: `DELETE`, `CREATE`, `OPEN`, `MODIFY`, `FILTER`, `EXTRACT`
- **Values**: `IN`, `OUT`, `INFINITE`

**Always use prefixes** for enum members to avoid conflicts:
```cpp
// ✅ GOOD - Use prefixes
enum class BinaryOperatorType {
    BOP_ADD, BOP_SUBTRACT, BOP_MULTIPLY, BOP_DIVIDE, BOP_MODULO,
    BOP_EQUAL, BOP_NOT_EQUAL, BOP_LESS, BOP_GREATER,
    BOP_LESS_EQUAL, BOP_GREATER_EQUAL, BOP_IN,
    BOP_AND, BOP_OR, BOP_XOR
};

enum class AggregateFunctionType {
    AGG_COUNT, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX, AGG_COLLECT
};

enum class UnaryOperatorType {
    UOP_MINUS, UOP_NOT
};

// ❌ BAD - Will break Windows builds
enum class AggregateFunctionType {
    MIN,   // Conflicts with Windows min() macro
    MAX,   // Conflicts with Windows max() macro
    // ...
};
```

**Required prefixes by enum type:**
- Binary operators: `BOP_*`
- Unary operators: `UOP_*`
- Aggregate functions: `AGG_*`
- Comprehension types: `COMP_*`
- Entity change types: `CHANGE_*`
- File open modes: `OPEN_*`

**Testing on Windows:**
Always verify compilation on Windows before submitting PR if you add new enum types.

## Code Style and Formatting

When contributing code, please ensure it adheres to the project's coding standards:

- Follow the existing code style and formatting
- Write clear, self-documenting code with meaningful variable and function names
- Add comments for complex logic or non-obvious implementations
- Keep functions focused and concise

## Git Commit Guidelines

### Commit Message Format

Write clear, concise commit messages that describe the **what** and **why** of your changes:

- **Use imperative mood**: "Fix bug" not "Fixed bug" or "Fixes bug"
- **Be specific**: Describe what was changed and why
- **Keep it focused**: One logical change per commit
- **No Co-Authored-By**: Do not include `Co-Authored-By` trailers in commit messages

### Example Commit Messages

**Good:**
```
Fix memory leak in BlobManager::deleteBlobChain

Removed double-free issue where blob entities were being deleted
both individually and during chain cleanup. Now the chain
cleanup handles all deletions atomically.
```

**Bad:**
```
Fixed stuff
Co-Authored-By: Someone <someone@example.com>
```

### Pull Request Requirements

Before submitting a pull request:

1. **Ensure all tests pass**: Run `./scripts/run_tests.sh` and verify 100% pass rate
2. **Check coverage**: New code should have adequate test coverage (target: 95%+ branch coverage)
3. **Update documentation**: If your changes affect user-facing functionality, update relevant docs
4. **One feature per PR**: Keep pull requests focused on a single feature or fix
