/**
 * @file test_FunctionRegistry_TemporalFunctions.cpp
 * @author ZYX Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "query/expressions/FunctionRegistryTestFixture.hpp"

// ============================================================================
// Temporal Function Tests
// ============================================================================

TEST_F(FunctionRegistryTest, DateFunction_NoArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("date");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args;
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DATE);
}

TEST_F(FunctionRegistryTest, DateFunction_StringArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("date");
	std::vector<PropertyValue> args{PropertyValue(std::string("2024-01-15"))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DATE);
}

TEST_F(FunctionRegistryTest, DateFunction_MapArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("date");
	PropertyValue::MapType map;
	map["year"] = PropertyValue(static_cast<int64_t>(2024));
	map["month"] = PropertyValue(static_cast<int64_t>(3));
	map["day"] = PropertyValue(static_cast<int64_t>(15));
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DATE);
}

TEST_F(FunctionRegistryTest, DateFunction_InvalidType) {
	const ScalarFunction* func = registry->lookupScalarFunction("date");
	std::vector<PropertyValue> args{PropertyValue(static_cast<int64_t>(123))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, DatetimeFunction_NoArgs) {
	const ScalarFunction* func = registry->lookupScalarFunction("datetime");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args;
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DATETIME);
}

TEST_F(FunctionRegistryTest, DatetimeFunction_StringArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("datetime");
	std::vector<PropertyValue> args{PropertyValue(std::string("2024-01-15T10:30:00"))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DATETIME);
}

TEST_F(FunctionRegistryTest, DatetimeFunction_MapArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("datetime");
	PropertyValue::MapType map;
	map["year"] = PropertyValue(static_cast<int64_t>(2024));
	map["month"] = PropertyValue(static_cast<int64_t>(3));
	map["day"] = PropertyValue(static_cast<int64_t>(15));
	map["hour"] = PropertyValue(static_cast<int64_t>(10));
	map["minute"] = PropertyValue(static_cast<int64_t>(30));
	map["second"] = PropertyValue(static_cast<int64_t>(45));
	map["millisecond"] = PropertyValue(static_cast<int64_t>(500));
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DATETIME);
}

TEST_F(FunctionRegistryTest, DatetimeFunction_InvalidType) {
	const ScalarFunction* func = registry->lookupScalarFunction("datetime");
	std::vector<PropertyValue> args{PropertyValue(static_cast<int64_t>(123))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, DurationFunction_StringArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("duration");
	ASSERT_NE(func, nullptr);
	std::vector<PropertyValue> args{PropertyValue(std::string("P1Y2M3D"))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DURATION);
}

TEST_F(FunctionRegistryTest, DurationFunction_MapArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("duration");
	PropertyValue::MapType map;
	map["years"] = PropertyValue(static_cast<int64_t>(1));
	map["months"] = PropertyValue(static_cast<int64_t>(2));
	map["days"] = PropertyValue(static_cast<int64_t>(3));
	map["hours"] = PropertyValue(static_cast<int64_t>(4));
	map["minutes"] = PropertyValue(static_cast<int64_t>(5));
	map["seconds"] = PropertyValue(static_cast<int64_t>(6));
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DURATION);
}

TEST_F(FunctionRegistryTest, DurationFunction_NullArg) {
	const ScalarFunction* func = registry->lookupScalarFunction("duration");
	std::vector<PropertyValue> args{PropertyValue()};
	EXPECT_EQ(func->evaluate(args, *context_).getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, DurationFunction_InvalidType) {
	const ScalarFunction* func = registry->lookupScalarFunction("duration");
	std::vector<PropertyValue> args{PropertyValue(static_cast<int64_t>(123))};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}

TEST_F(FunctionRegistryTest, DateFunction_MapWithMissingFields) {
	const ScalarFunction* func = registry->lookupScalarFunction("date");
	PropertyValue::MapType map;
	// Only year, missing month and day → defaults to 1
	map["year"] = PropertyValue(static_cast<int64_t>(2024));
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DATE);
}

TEST_F(FunctionRegistryTest, DatetimeFunction_MapWithMissingFields) {
	const ScalarFunction* func = registry->lookupScalarFunction("datetime");
	PropertyValue::MapType map;
	map["year"] = PropertyValue(static_cast<int64_t>(2024));
	// Missing all time fields → defaults to 0
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DATETIME);
}

// Cover the false branch of "it->second.getType() == PropertyType::INTEGER"
// in dateImpl: key "year" exists but holds a string, so the branch is NOT taken
// and the default value (1970) is used.
TEST_F(FunctionRegistryTest, DateFunction_MapYearNonInteger_UsesDefault) {
	const ScalarFunction* func = registry->lookupScalarFunction("date");
	PropertyValue::MapType map;
	map["year"] = PropertyValue(std::string("2024")); // string, not integer
	map["month"] = PropertyValue(std::string("3"));   // string, not integer
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	// Should still return a DATE (using defaults year=1970, month=1, day=1)
	EXPECT_EQ(result.getType(), PropertyType::DATE);
}

// Cover the false branch of the INTEGER type check in datetimeImpl map path.
TEST_F(FunctionRegistryTest, DatetimeFunction_MapYearNonInteger_UsesDefault) {
	const ScalarFunction* func = registry->lookupScalarFunction("datetime");
	PropertyValue::MapType map;
	map["year"] = PropertyValue(std::string("2024")); // string, not integer
	map["hour"] = PropertyValue(std::string("10"));   // string, not integer
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	// Should still return a DATETIME (using defaults)
	EXPECT_EQ(result.getType(), PropertyType::DATETIME);
}

// Cover the durationImpl empty-args guard (args.empty() branch).
// durationImpl is registered with minArgs=1 so the function registry validator
// blocks too-few args before reaching durationImpl; call the function object
// directly with an empty vector to cover the guard branch.
TEST_F(FunctionRegistryTest, DurationFunction_EmptyArgs_ReturnsNull) {
	const ScalarFunction* func = registry->lookupScalarFunction("duration");
	ASSERT_NE(func, nullptr);
	// Bypass arity check by calling the underlying evaluate directly
	std::vector<PropertyValue> empty;
	// The guard "if (args.empty() || isNull(args[0]))" returns NULL
	auto result = func->evaluate(empty, *context_);
	EXPECT_EQ(result.getType(), PropertyType::NULL_TYPE);
}
