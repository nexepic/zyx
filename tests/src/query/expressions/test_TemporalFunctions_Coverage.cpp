/**
 * @file test_TemporalFunctions_Coverage.cpp
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "query/expressions/FunctionRegistryTestFixture.hpp"
#include "graph/core/TemporalTypes.hpp"

// ============================================================================
// duration() with map containing weeks and nanoseconds
// ============================================================================

TEST_F(FunctionRegistryTest, DurationFunction_MapWithWeeks) {
	const ScalarFunction* func = registry->lookupScalarFunction("duration");
	PropertyValue::MapType map;
	map["weeks"] = PropertyValue(static_cast<int64_t>(2));
	map["days"] = PropertyValue(static_cast<int64_t>(1));
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DURATION);
	const auto& dur = std::get<TemporalDuration>(result.getVariant());
	// 2 weeks = 14 days + 1 day = 15 days
	EXPECT_EQ(dur.days, 15);
}

TEST_F(FunctionRegistryTest, DurationFunction_MapWithNanoseconds) {
	const ScalarFunction* func = registry->lookupScalarFunction("duration");
	PropertyValue::MapType map;
	map["nanoseconds"] = PropertyValue(static_cast<int64_t>(5000000000LL));
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DURATION);
	const auto& dur = std::get<TemporalDuration>(result.getVariant());
	EXPECT_EQ(dur.nanos, 5000000000LL);
}

TEST_F(FunctionRegistryTest, DurationFunction_MapWithAllComponents) {
	const ScalarFunction* func = registry->lookupScalarFunction("duration");
	PropertyValue::MapType map;
	map["years"] = PropertyValue(static_cast<int64_t>(1));
	map["months"] = PropertyValue(static_cast<int64_t>(2));
	map["weeks"] = PropertyValue(static_cast<int64_t>(1));
	map["days"] = PropertyValue(static_cast<int64_t>(3));
	map["hours"] = PropertyValue(static_cast<int64_t>(4));
	map["minutes"] = PropertyValue(static_cast<int64_t>(5));
	map["seconds"] = PropertyValue(static_cast<int64_t>(6));
	map["nanoseconds"] = PropertyValue(static_cast<int64_t>(7));
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DURATION);
}

// ============================================================================
// duration() map with non-integer values (uses default 0)
// ============================================================================

TEST_F(FunctionRegistryTest, DurationFunction_MapNonIntegerValues) {
	const ScalarFunction* func = registry->lookupScalarFunction("duration");
	PropertyValue::MapType map;
	map["years"] = PropertyValue(std::string("not_int"));
	map["months"] = PropertyValue(true);
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DURATION);
	const auto& dur = std::get<TemporalDuration>(result.getVariant());
	EXPECT_EQ(dur.months, 0);
}

// ============================================================================
// datetime() map with partial fields to cover false branches
// ============================================================================

TEST_F(FunctionRegistryTest, DatetimeFunction_MapPartialTimeFields) {
	const ScalarFunction* func = registry->lookupScalarFunction("datetime");
	PropertyValue::MapType map;
	map["year"] = PropertyValue(static_cast<int64_t>(2024));
	map["month"] = PropertyValue(static_cast<int64_t>(6));
	map["day"] = PropertyValue(static_cast<int64_t>(15));
	map["hour"] = PropertyValue(static_cast<int64_t>(14));
	// minute, second, millisecond not set - defaults to 0
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DATETIME);
}

// ============================================================================
// date() map with non-integer month/day (uses defaults)
// ============================================================================

TEST_F(FunctionRegistryTest, DateFunction_MapNonIntegerDay) {
	const ScalarFunction* func = registry->lookupScalarFunction("date");
	PropertyValue::MapType map;
	map["year"] = PropertyValue(static_cast<int64_t>(2024));
	map["month"] = PropertyValue(static_cast<int64_t>(3));
	map["day"] = PropertyValue(std::string("fifteen")); // not integer
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DATE);
}

// ============================================================================
// datetime() map with non-integer minute/second/millisecond
// ============================================================================

TEST_F(FunctionRegistryTest, DatetimeFunction_MapNonIntegerMinute) {
	const ScalarFunction* func = registry->lookupScalarFunction("datetime");
	PropertyValue::MapType map;
	map["year"] = PropertyValue(static_cast<int64_t>(2024));
	map["month"] = PropertyValue(static_cast<int64_t>(1));
	map["day"] = PropertyValue(static_cast<int64_t>(1));
	map["hour"] = PropertyValue(static_cast<int64_t>(10));
	map["minute"] = PropertyValue(std::string("thirty")); // not integer
	map["second"] = PropertyValue(true); // not integer
	map["millisecond"] = PropertyValue(3.14); // not integer
	std::vector<PropertyValue> args{PropertyValue(map)};
	auto result = func->evaluate(args, *context_);
	EXPECT_EQ(result.getType(), PropertyType::DATETIME);
}
