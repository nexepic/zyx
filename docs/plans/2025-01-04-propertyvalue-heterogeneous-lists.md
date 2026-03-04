# PropertyValue Architectural Expansion: Heterogeneous List Support

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extend `PropertyValue` to support `std::vector<PropertyValue>` instead of `std::vector<float>` for true heterogeneous list support.

**Architecture:**
- Change `PropertyValue::VariantType` from `std::variant<..., std::vector<float>>` to `std::variant<..., std::vector<PropertyValue>>`
- Update all code that creates, accesses, or manipulates lists
- Maintain backward compatibility where possible
- Ensure full test coverage

**Impact Analysis:**

Files requiring changes:
1. **Core:** `PropertyTypes.hpp`, `PropertyTypes.cpp`
2. **Expression Evaluation:** `EvaluationContext.cpp`, `ExpressionBuilder.cpp`
3. **Functions:** `FunctionRegistry.cpp` (range(), size(), etc.)
4. **C API:** `DatabaseImpl.cpp`
5. **Vector Index:** `DiskANNIndex.cpp` (keep `std::vector<float>` for embeddings)

**Tech Stack:**
- C++20 with Meson build system
- Google Test framework
- LLVM coverage instrumentation

---

## Task 1: Update PropertyValue Header

**Files:**
- Modify: `include/graph/core/PropertyTypes.hpp`

**Changes Required:**

1. **Line 51**: Change VariantType
   ```cpp
   // OLD:
   using VariantType = std::variant<std::monostate, bool, int64_t, double, std::string, std::vector<float>>;

   // NEW:
   using VariantType = std::variant<std::monostate, bool, int64_t, double, std::string, std::vector<PropertyValue>>;
   ```

2. **Line 77**: Update constructor
   ```cpp
   // OLD:
   explicit PropertyValue(std::vector<float> vec) : data(std::move(vec)) {}

   // NEW:
   explicit PropertyValue(std::vector<PropertyValue> vec) : data(std::move(vec)) {}
   ```

3. **Lines 114-121**: Update toString() for lists
   ```cpp
   } else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) {
       std::ostringstream oss;
       oss << "[";
       for (size_t i = 0; i < value.size(); ++i) {
           oss << value[i].toString() << (i < value.size() - 1 ? ", " : "");
       }
       oss << "]";
       return oss.str();
   }
   ```

4. **Lines 144-145**: Update typeName()
   ```cpp
   else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>)
       return "LIST";
   ```

5. **Lines 166-167**: Update getType()
   ```cpp
   if (std::holds_alternative<std::vector<PropertyValue>>(data))
       return PropertyType::LIST;
   ```

6. **Lines 175-180**: Update getList()
   ```cpp
   const std::vector<PropertyValue> &getList() const {
       if (auto *val = std::get_if<std::vector<PropertyValue>>(&data)) {
           return *val;
       }
       throw std::runtime_error("PropertyValue is not a List/Vector");
   }
   ```

7. **Line 199**: Update property_utils helper
   ```cpp
   } else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) {
       type = PropertyType::LIST;
   }
   ```

**After modifying:**
1. Build: `meson compile -C buildDir`
2. Expected: Compilation errors in dependent files
3. Commit: `git add include/graph/core/PropertyTypes.hpp && git commit -m "Change PropertyValue to support heterogeneous lists"`

---

## Task 2: Update PropertyTypes.cpp

**Files:**
- Modify: `src/core/PropertyTypes.cpp`

**Changes Required:**

Find all instances of `std::vector<float>` and change to `std::vector<PropertyValue>`:

```cpp
// In toString() method
} else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) {
    // ... implementation
}
```

**After modifying:**
1. Build: `meson compile -C buildDir`
2. Expected: More compilation errors in other files
3. Commit: `git add src/core/PropertyTypes.cpp && git commit -m "Update PropertyTypes.cpp for heterogeneous lists"`

---

## Task 3: Update EvaluationContext

**Files:**
- Modify: `src/query/expressions/EvaluationContext.cpp`

**Changes Required:**

Search for `std::vector<float>` and replace with `std::vector<PropertyValue>` in template specializations.

**After modifying:**
1. Build: `meson compile -C buildDir`
2. Commit: `git add src/query/expressions/EvaluationContext.cpp && git commit -m "Update EvaluationContext for heterogeneous lists"`

---

## Task 4: Update FunctionRegistry (range function)

**Files:**
- Modify: `src/query/expressions/FunctionRegistry.cpp`

**Changes Required:**

The `RangeFunction::evaluate()` method currently returns `std::vector<float>`. Change it to return `std::vector<PropertyValue>`:

```cpp
// OLD:
std::vector<float> result;
result.reserve(16);
for (int64_t i = start; i < end; i += step) {
    result.push_back(static_cast<float>(i));
}
return PropertyValue(result);

// NEW:
std::vector<PropertyValue> result;
result.reserve(16);
for (int64_t i = start; i < end; i += step) {
    result.push_back(PropertyValue(static_cast<int64_t>(i)));  // Note: int64_t not float
}
return PropertyValue(result);
```

**Important:** Use `int64_t` for the elements to maintain integer precision.

**After modifying:**
1. Build: `meson compile -C buildDir`
2. Run tests: `./scripts/run_tests.sh --quick --file test_FunctionRegistry_Unit.cpp`
3. Commit: `git add src/query/expressions/FunctionRegistry.cpp && git commit -m "Update RangeFunction to return PropertyValue list"`

---

## Task 5: Update ExpressionBuilder

**Files:**
- Modify: `src/query/parser/cypher/helpers/internal/ExpressionBuilder.cpp`

**Changes Required:**

Find the list literal creation code (around line with `std::vector<float> vec;`):

```cpp
// OLD:
std::vector<float> vec;
// ... populate vec
PropertyValue listValue(vec);

// NEW:
std::vector<PropertyValue> vec;
// ... populate vec with PropertyValue elements
PropertyValue listValue(vec);
```

**After modifying:**
1. Build: `meson compile -C buildDir`
2. Commit: `git add src/query/parser/cypher/helpers/internal/ExpressionBuilder.cpp && git commit -m "Update ExpressionBuilder for heterogeneous lists"`

---

## Task 6: Update DatabaseImpl (C API)

**Files:**
- Modify: `src/api/DatabaseImpl.cpp`

**Changes Required:**

Update the C API binding that handles lists:

```cpp
// Change std::vector<float> to std::vector<PropertyValue>
```

**After modifying:**
1. Build: `meson compile -C buildDir`
2. Commit: `git add src/api/DatabaseImpl.cpp && git commit -m "Update C API for heterogeneous lists"`

---

## Task 7: Implement List Slicing Evaluator

**Files:**
- Modify: `src/query/expressions/ExpressionEvaluator.cpp`

**Changes Required:**

Now that lists are `std::vector<PropertyValue>`, implement the actual slicing logic in `evaluateListSlice()`:

```cpp
PropertyValue ExpressionEvaluator::evaluateListSlice(
    const ListSliceExpression* sliceExpr) {

    PropertyValue listValue = std::any_cast<PropertyValue>(visit(sliceExpr->getList()));

    if (listValue.getType() != PropertyType::LIST) {
        throw ExpressionEvaluationException("Cannot slice non-list value");
    }

    const auto& list = listValue.getList();
    size_t listSize = list.size();

    // Single index: list[0]
    if (!sliceExpr->hasRange()) {
        if (!sliceExpr->getStart()) {
            throw ExpressionEvaluationException("List slice requires index");
        }

        PropertyValue indexValue = std::any_cast<PropertyValue>(visit(sliceExpr->getStart()));

        if (indexValue.getType() != PropertyType::INTEGER &&
            indexValue.getType() != PropertyType::DOUBLE) {
            throw ExpressionEvaluationException("List index must be integer");
        }

        int64_t index = 0;
        if (indexValue.getType() == PropertyType::INTEGER) {
            index = std::get<int64_t>(indexValue.getVariant());
        } else {
            index = static_cast<int64_t>(std::get<double>(indexValue.getVariant()));
        }

        // Handle negative index
        if (index < 0) {
            index = listSize + index;
        }

        // Check bounds
        if (index < 0 || static_cast<size_t>(index) >= listSize) {
            return PropertyValue(); // Return null for out of bounds
        }

        return list[index];  // Return the PropertyValue element
    }

    // Range slice: list[0..2]
    int64_t start = 0;
    int64_t end = static_cast<int64_t>(listSize);

    if (sliceExpr->getStart()) {
        PropertyValue startValue = std::any_cast<PropertyValue>(visit(sliceExpr->getStart()));
        // Extract index similar to above
        // ... handle INTEGER and DOUBLE cases
    }

    if (sliceExpr->getEnd()) {
        PropertyValue endValue = std::any_cast<PropertyValue>(visit(sliceExpr->getEnd()));
        // Extract index similar to above
    }

    // Clamp to valid range
    start = std::max<int64_t>(0, std::min<int64_t>(start, static_cast<int64_t>(listSize)));
    end = std::max<int64_t>(0, std::min<int64_t>(end, static_cast<int64_t>(listSize)));

    // Build result list
    std::vector<PropertyValue> result;
    for (int64_t i = start; i < end; ++i) {
        result.push_back(list[i]);
    }

    return PropertyValue(result);
}
```

Remove the "not yet supported" exception.

**After modifying:**
1. Build: `meson compile -C buildDir`
2. Commit: `git add src/query/expressions/ExpressionEvaluator.cpp && git commit -m "Implement list slicing evaluator for heterogeneous lists"`

---

## Task 8: Create Comprehensive List Tests

**Files:**
- Create: `tests/src/query/expressions/test_HeterogeneousLists_Unit.cpp`

**Test Coverage:**

```cpp
// Test homogeneous lists
TEST_F(HeterogeneousListTest, IntegerList) {
    std::vector<PropertyValue> list = {
        PropertyValue(int64_t(1)),
        PropertyValue(int64_t(2)),
        PropertyValue(int64_t(3))
    };
    PropertyValue listValue(list);
    // Test access, slicing, etc.
}

// Test heterogeneous lists
TEST_F(HeterogeneousListTest, MixedTypes) {
    std::vector<PropertyValue> list = {
        PropertyValue(int64_t(42)),
        PropertyValue(std::string("hello")),
        PropertyValue(true),
        PropertyValue(3.14)
    };
    PropertyValue listValue(list);
    // Verify all types are preserved
}

// Test nested lists
TEST_F(HeterogeneousListTest, NestedLists) {
    std::vector<PropertyValue> inner1 = {PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
    std::vector<PropertyValue> inner2 = {PropertyValue(int64_t(3)), PropertyValue(int64_t(4))};

    std::vector<PropertyValue> outer = {
        PropertyValue(inner1),
        PropertyValue(inner2)
    };
    // Test nested access
}

// Test list slicing with heterogeneous elements
TEST_F(HeterogeneousListTest, SliceMixedTypes) {
    // Test slicing [start..end] with mixed types
}

// Test range() function returns integer list
TEST_F(HeterogeneousListTest, RangeReturnsIntegers) {
    // Verify range(0, 5) returns [0, 1, 2, 3, 4] as integers
}
```

**After creating:**
1. Build and run tests
2. Verify 95%+ coverage
3. Commit: `git add tests/src/query/expressions/test_HeterogeneousLists_Unit.cpp tests/src/query/expressions/meson.build && git commit -m "Add comprehensive heterogeneous list tests"`

---

## Task 9: Update Existing Tests

**Files:**
- Modify: `tests/src/query/expressions/test_FunctionRegistry_Unit.cpp`
- Modify: `tests/src/core/test_PropertyTypes.cpp`

**Changes Required:**

Update tests that expect `std::vector<float>` to work with `std::vector<PropertyValue>`:

```cpp
// OLD:
const auto& list = std::get<std::vector<float>>(result.getVariant());

// NEW:
const auto& list = std::get<std::vector<PropertyValue>>(result.getVariant());
```

**After modifying:**
1. Run all tests: `./scripts/run_tests.sh --quick`
2. Expected: All tests pass
3. Commit: `git add tests/ && git commit -m "Update tests for heterogeneous list support"`

---

## Task 10: Full Integration Test

**Files:**
- Create: `tests/integration/test_IntegrationHeterogeneousLists.cpp`

**Integration Tests:**

```cpp
// Test end-to-end list operations in Cypher queries
TEST_F(IntegrationTest, ListLiteralInCypher) {
    // Test RETURN [1, "hello", true]
}

TEST_F(IntegrationTest, ListSlicingInCypher) {
    // Test RETURN [1,2,3,4,5][1..3]
}

TEST_F(IntegrationTest, NestedListQueries) {
    // Test complex nested list operations
}
```

**After creating:**
1. Run integration tests
2. Verify all pass
3. Commit: `git add tests/integration/test_IntegrationHeterogeneousLists.cpp && git commit -m "Add integration tests for heterogeneous lists"`
```

---

## Task 11: Update Documentation

**Files:**
- Modify: `UNSUPPORTED_CYPER_FEATURES.md`
- Modify: `docs/plans/2025-01-04-cypher-phase1-design.md`

**Documentation Updates:**

Document that lists now support heterogeneous elements:
- Lists can contain any PropertyValue: integers, strings, booleans, doubles, null, or nested lists
- List slicing works with heterogeneous lists
- range() function returns integer list for precision

**After updating:**
1. Commit: `git add UNSUPPORTED_CYPER_FEATURES.md docs/plans/ && git commit -m "Document heterogeneous list support"`
```

---

## Task 12: Final Verification

**Steps:**

1. **Full test suite:**
   ```bash
   ./scripts/run_tests.sh --quick
   ```
   Expected: All 125+ tests pass

2. **Coverage check:**
   ```bash
   ./scripts/run_tests.sh --html
   ```
   Expected: 94%+ line coverage maintained

3. **Manual CLI testing:**
   ```bash
   ./buildDir/apps/cli/metrix-cli
   ```
   Test queries:
   ```cypher
   -- Heterogeneous list literal
   RETURN [1, "hello", true, 3.14];

   -- List slicing
   UNWIND [1, "a", true, 4.0] AS x RETURN x[0..2];

   -- Nested lists
   RETURN [[1,2], [3,4]];
   ```

4. **Final commit:**
   ```bash
   git commit --allow-empty -m "Complete heterogeneous list support

   Architectural changes:
   - Changed PropertyValue from std::vector<float> to std::vector<PropertyValue>
   - Updated all list operations to support heterogeneous elements
   - Implemented full list slicing evaluator
   - Added comprehensive test coverage

   All tests passing: 125+
   Coverage maintained: 94%+ line
   "
   ```

---

## Success Criteria

After completing all tasks:

- ✅ All 125+ tests pass
- ✅ PropertyValue supports `std::vector<PropertyValue>`
- ✅ List slicing works with heterogeneous lists
- ✅ range() returns integers for precision
- ✅ Nested lists supported
- ✅ Coverage maintained at 94%+
- ✅ Documentation updated
- ✅ Manual CLI testing successful

## Risk Mitigation

**Breaking Changes:**
- Existing code expecting `std::vector<float>` will need updates
- Vector index embeddings should keep using `std::vector<float>` (separate concern)

**Testing Strategy:**
- Update all existing tests before committing
- Run full test suite after each task
- Use git bisect if regressions occur

**Rollback Plan:**
- Keep commit history clean
- Each task is independently revertible
- Tag commit before starting: `git tag pre-heterogeneous-lists`
