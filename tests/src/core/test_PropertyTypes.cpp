/**
 * @file test_PropertyTypes.cpp
 * @author Nexepic
 * @date 2025/7/29
 *
 * @copyright Copyright (c) 2025 Nexepic
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

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "graph/core/PropertyTypes.hpp"

using graph::PropertyType;
using graph::PropertyValue;
using graph::property_utils::checkPropertyMatch;
using graph::property_utils::getPropertyValueSize;

// ============================================================================
// Constructor and Type Identification Tests
// ============================================================================

TEST(PropertyValueTest, ConstructNull) {
	// Default constructor
	graph::PropertyValue v1;
	EXPECT_EQ(graph::getPropertyType(v1), PropertyType::NULL_TYPE);
	EXPECT_EQ(v1.typeName(), "NULL");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(v1.getVariant()));

	// Explicit monostate constructor
	graph::PropertyValue v2(std::monostate{});
	EXPECT_EQ(graph::getPropertyType(v2), PropertyType::NULL_TYPE);
}

TEST(PropertyValueTest, ConstructBoolean) {
	graph::PropertyValue v(true);
	EXPECT_EQ(graph::getPropertyType(v), PropertyType::BOOLEAN);
	EXPECT_EQ(v.typeName(), "BOOLEAN");
	EXPECT_TRUE(std::get<bool>(v.getVariant()));
}

TEST(PropertyValueTest, ConstructIntegers) {
	// Test implicit promotion to int64_t for various integral types
	int32_t i32 = 123;
	int64_t i64 = 123;
	short s = 123;

	graph::PropertyValue v32(i32);
	graph::PropertyValue v64(i64);
	graph::PropertyValue vs(s);

	EXPECT_EQ(graph::getPropertyType(v32), PropertyType::INTEGER);
	EXPECT_EQ(graph::getPropertyType(v64), PropertyType::INTEGER);
	EXPECT_EQ(graph::getPropertyType(vs), PropertyType::INTEGER);

	EXPECT_EQ(v32.typeName(), "INTEGER");
	EXPECT_EQ(std::get<int64_t>(v32.getVariant()), 123);
}

TEST(PropertyValueTest, ConstructFloats) {
	// Test implicit promotion to double
	float f = 3.14f;
	double d = 3.14;

	graph::PropertyValue vf(f);
	graph::PropertyValue vd(d);

	EXPECT_EQ(graph::getPropertyType(vf), PropertyType::DOUBLE);
	EXPECT_EQ(graph::getPropertyType(vd), PropertyType::DOUBLE);

	EXPECT_EQ(vf.typeName(), "DOUBLE");
	// Note: float to double conversion might have precision nuances,
	// but static_cast<double>(3.14f) is deterministic.
	EXPECT_DOUBLE_EQ(std::get<double>(vd.getVariant()), 3.14);
}

TEST(PropertyValueTest, ConstructStrings) {
	// const char*
	graph::PropertyValue v1("hello");
	EXPECT_EQ(graph::getPropertyType(v1), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(v1.getVariant()), "hello");

	// std::string
	std::string s = "world";
	graph::PropertyValue v2(s);
	EXPECT_EQ(graph::getPropertyType(v2), PropertyType::STRING);
	EXPECT_EQ(std::get<std::string>(v2.getVariant()), "world");

	// Move semantics
	std::string source = "moved";
	graph::PropertyValue v3(std::move(source));
	EXPECT_EQ(std::get<std::string>(v3.getVariant()), "moved");
	// Verify source was actually moved (empty state)
	EXPECT_TRUE(source.empty());
}

// ============================================================================
// Utility Method Tests (toString)
// ============================================================================

TEST(PropertyValueTest, ToStringConversion) {
	EXPECT_EQ(graph::PropertyValue().toString(), "null");
	EXPECT_EQ(graph::PropertyValue(true).toString(), "true");
	EXPECT_EQ(graph::PropertyValue(false).toString(), "false");
	EXPECT_EQ(graph::PropertyValue(42).toString(), "42");
	EXPECT_EQ(graph::PropertyValue(-100).toString(), "-100");
	EXPECT_EQ(graph::PropertyValue(3.5).toString(), "3.5");
	EXPECT_EQ(graph::PropertyValue("test_string").toString(), "test_string");
}

// ============================================================================
// Equality and Matching Tests
// ============================================================================

TEST(PropertyValueTest, EqualityOperators) {
	graph::PropertyValue a(10);
	graph::PropertyValue b(10);
	graph::PropertyValue c(20);
	graph::PropertyValue d("10"); // Different type

	EXPECT_TRUE(a == b);
	EXPECT_FALSE(a == c);
	EXPECT_FALSE(a == d); // int(10) != string("10")

	EXPECT_FALSE(a != b);
	EXPECT_TRUE(a != c);
	EXPECT_TRUE(a != d);
}

TEST(PropertyValueTest, CheckPropertyMatch) {
	graph::PropertyValue val1(3.14);
	graph::PropertyValue val2(3.14);
	graph::PropertyValue val3(1.0);

	EXPECT_TRUE(checkPropertyMatch(val1, val2));
	EXPECT_FALSE(checkPropertyMatch(val1, val3));
}

// ============================================================================
// Relational Comparison Tests
// ============================================================================

TEST(PropertyValueTest, RelationalOperatorsIntraType) {
	// Integers
	EXPECT_LT(graph::PropertyValue(10), graph::PropertyValue(20));
	EXPECT_LE(graph::PropertyValue(10), graph::PropertyValue(10));
	EXPECT_GT(graph::PropertyValue(30), graph::PropertyValue(20));
	EXPECT_GE(graph::PropertyValue(30), graph::PropertyValue(30));

	// Strings (Lexicographical)
	EXPECT_LT(graph::PropertyValue("apple"), graph::PropertyValue("banana"));

	// Doubles
	EXPECT_LT(graph::PropertyValue(1.0), graph::PropertyValue(2.0));
}

TEST(PropertyValueTest, RelationalOperatorsInterType) {
	// std::variant default comparison relies on the index of the type definition.
	// Order in definition: monostate (0), bool (1), int64_t (2), double (3), string (4).

	graph::PropertyValue nullVal;
	graph::PropertyValue boolVal(false);
	graph::PropertyValue intVal(0);
	graph::PropertyValue doubleVal(0.0);
	graph::PropertyValue strVal("");

	// 0 < 1
	EXPECT_LT(nullVal, boolVal);
	// 1 < 2
	EXPECT_LT(boolVal, intVal);
	// 2 < 3
	EXPECT_LT(intVal, doubleVal);
	// 3 < 4
	EXPECT_LT(doubleVal, strVal);

	// Verify inverse >
	EXPECT_GT(strVal, intVal);
}

// ============================================================================
// Size Calculation Tests (property_utils)
// ============================================================================

TEST(PropertyValueTest, SizeOfMonostate) {
	graph::PropertyValue v = std::monostate{};
	EXPECT_EQ(getPropertyValueSize(v), 1u); // Minimal size for NULL.
}

TEST(PropertyValueTest, SizeOfBool) {
	graph::PropertyValue v = true;
	EXPECT_EQ(getPropertyValueSize(v), sizeof(bool));
}

TEST(PropertyValueTest, SizeOfIntegers) {
	graph::PropertyValue v_int = 42; // implicit int -> int64_t
	graph::PropertyValue v_int32 = int32_t{42}; // implicit int32 -> int64_t
	graph::PropertyValue v_int64 = int64_t{42};

	EXPECT_EQ(getPropertyValueSize(v_int), sizeof(int64_t));
	EXPECT_EQ(getPropertyValueSize(v_int32), sizeof(int64_t));
	EXPECT_EQ(getPropertyValueSize(v_int64), sizeof(int64_t));
}

TEST(PropertyValueTest, SizeOfFloats) {
	graph::PropertyValue v_float = 3.14f; // implicit float -> double
	graph::PropertyValue v_double = 3.14;

	EXPECT_EQ(getPropertyValueSize(v_float), sizeof(double));
	EXPECT_EQ(getPropertyValueSize(v_double), sizeof(double));
}

TEST(PropertyValueTest, SizeOfString) {
	std::string s = "hello";
	graph::PropertyValue v = s;
	// The size is the string content + overhead for storing its length (sizeof(size_t)).
	EXPECT_EQ(getPropertyValueSize(v), s.size() + sizeof(size_t));

	// Test empty string
	graph::PropertyValue v_empty("");
	EXPECT_EQ(getPropertyValueSize(v_empty), 0u + sizeof(size_t));
}

// ============================================================================
// LIST Type Tests (std::vector<float>)
// ============================================================================

TEST(PropertyValueTest, ConstructList) {
	// Test empty list
	std::vector<float> emptyList;
	graph::PropertyValue v1(emptyList);
	EXPECT_EQ(graph::getPropertyType(v1), PropertyType::LIST);
	EXPECT_EQ(v1.typeName(), "LIST");
	EXPECT_TRUE(std::holds_alternative<std::vector<float>>(v1.getVariant()));
	EXPECT_TRUE(std::get<std::vector<float>>(v1.getVariant()).empty());

	// Test non-empty list
	std::vector<float> list = {1.0f, 2.0f, 3.0f};
	graph::PropertyValue v2(list);
	EXPECT_EQ(graph::getPropertyType(v2), PropertyType::LIST);
	EXPECT_EQ(std::get<std::vector<float>>(v2.getVariant()).size(), 3u);

	// Test move semantics
	std::vector<float> source = {4.0f, 5.0f};
	graph::PropertyValue v3(std::move(source));
	EXPECT_EQ(std::get<std::vector<float>>(v3.getVariant()).size(), 2u);
	EXPECT_TRUE(source.empty()); // Verify source was moved
}

TEST(PropertyValueTest, ListToString) {
	// Empty list
	std::vector<float> emptyList;
	graph::PropertyValue v1(emptyList);
	EXPECT_EQ(v1.toString(), "[]");

	// Non-empty list
	std::vector<float> list = {1.5f, 2.5f, 3.5f};
	graph::PropertyValue v2(list);
	std::string result = v2.toString();
	// The toString implementation should produce something like "[1.5, 2.5, 3.5]"
	EXPECT_TRUE(result.find("[") != std::string::npos);
	EXPECT_TRUE(result.find("]") != std::string::npos);
}

TEST(PropertyValueTest, ListEquality) {
	std::vector<float> list1 = {1.0f, 2.0f, 3.0f};
	std::vector<float> list2 = {1.0f, 2.0f, 3.0f};
	std::vector<float> list3 = {1.0f, 2.0f};

	graph::PropertyValue v1(list1);
	graph::PropertyValue v2(list2);
	graph::PropertyValue v3(list3);

	EXPECT_TRUE(v1 == v2); // Same elements
	EXPECT_FALSE(v1 == v3); // Different size
	EXPECT_FALSE(v1 == graph::PropertyValue(42)); // Different type
}

TEST(PropertyValueTest, ListRelationalOperators) {
	// List vs List comparison - uses variant index comparison
	std::vector<float> list1 = {1.0f};
	std::vector<float> list2 = {2.0f};

	graph::PropertyValue v1(list1);
	graph::PropertyValue v2(list2);

	// Same type (LIST), compares by value
	EXPECT_FALSE(v1 == v2); // Different values
	EXPECT_TRUE(v1 < v2 || v1 > v2); // One should be less

	// List vs other types - uses variant index comparison
	// Index order: monostate(0), bool(1), int64_t(2), double(3), string(4), vector(5)
	graph::PropertyValue intVal(42);
	graph::PropertyValue doubleVal(3.14);
	graph::PropertyValue strVal("test");

	// LIST (5) has higher index than INTEGER (2), DOUBLE (3), STRING (4)
	EXPECT_GT(v1, intVal);  // LIST (5) > INTEGER (2)
	EXPECT_GT(v1, doubleVal);  // LIST (5) > DOUBLE (3)
	EXPECT_GT(v1, strVal);  // LIST (5) > STRING (4)
}

TEST(PropertyValueTest, ListSize) {
	// Empty list - size is count (uint32_t) + data
	std::vector<float> emptyList;
	graph::PropertyValue v1(emptyList);
	EXPECT_EQ(getPropertyValueSize(v1), sizeof(uint32_t) + sizeof(float) * 0);

	// Non-empty list
	std::vector<float> list = {1.0f, 2.0f, 3.0f, 4.0f};
	graph::PropertyValue v2(list);
	EXPECT_EQ(getPropertyValueSize(v2), sizeof(uint32_t) + sizeof(float) * 4);
}

TEST(PropertyValueTest, GetListError) {
	// Test that getList() throws when value is not a LIST type
	graph::PropertyValue intVal(42);
	graph::PropertyValue strVal("test");
	graph::PropertyValue nullVal;

	EXPECT_THROW(intVal.getList(), std::runtime_error);
	EXPECT_THROW(strVal.getList(), std::runtime_error);
	EXPECT_THROW(nullVal.getList(), std::runtime_error);

	// Verify getList() works correctly for LIST type
	std::vector<float> list = {1.0f, 2.0f, 3.0f};
	graph::PropertyValue listVal(list);
	const auto &result = listVal.getList();
	EXPECT_EQ(result.size(), 3u);
	EXPECT_EQ(result[0], 1.0f);
	EXPECT_EQ(result[1], 2.0f);
	EXPECT_EQ(result[2], 3.0f);
}

// ============================================================================
// Additional Coverage Tests
// ============================================================================

// Test various integral types to ensure template coverage
TEST(PropertyValueTest, ConstructWithVariousIntegers) {
	graph::PropertyValue s1(static_cast<short>(100));
	EXPECT_EQ(graph::getPropertyType(s1), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(s1.getVariant()), 100);

	graph::PropertyValue s2(static_cast<unsigned int>(200));
	EXPECT_EQ(graph::getPropertyType(s2), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(s2.getVariant()), 200);

	graph::PropertyValue s3(static_cast<long>(300));
	EXPECT_EQ(graph::getPropertyType(s3), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(s3.getVariant()), 300);

	graph::PropertyValue s4(static_cast<unsigned long>(400));
	EXPECT_EQ(graph::getPropertyType(s4), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(s4.getVariant()), 400);

	graph::PropertyValue s5(static_cast<long long>(500));
	EXPECT_EQ(graph::getPropertyType(s5), PropertyType::INTEGER);
	EXPECT_EQ(std::get<int64_t>(s5.getVariant()), 500);
}

// Test float type specifically
TEST(PropertyValueTest, ConstructFloatVsDouble) {
	float f = 3.14f;
	double d = 3.14;

	graph::PropertyValue pvf(f);
	graph::PropertyValue pvd(d);

	EXPECT_EQ(graph::getPropertyType(pvf), PropertyType::DOUBLE);
	EXPECT_EQ(graph::getPropertyType(pvd), PropertyType::DOUBLE);

	// Both should be stored as double
	EXPECT_TRUE(std::holds_alternative<double>(pvf.getVariant()));
	EXPECT_TRUE(std::holds_alternative<double>(pvd.getVariant()));
}

// Test LIST toString with single element
TEST(PropertyValueTest, ListToStringSingleElement) {
	std::vector<float> list = {42.0f};
	graph::PropertyValue v(list);
	std::string result = v.toString();
	EXPECT_EQ(result, "[42]");
}

// Test LIST toString with multiple elements
TEST(PropertyValueTest, ListToStringMultipleElements) {
	std::vector<float> list = {1.5f, 2.5f, 3.5f, 4.5f, 5.5f};
	graph::PropertyValue v(list);
	std::string result = v.toString();
	EXPECT_EQ(result, "[1.5, 2.5, 3.5, 4.5, 5.5]");
}

// Test PropertyType UNKNOWN via getType on corrupted state
// Note: This tests the getType method's UNKNOWN fallback
TEST(PropertyValueTest, GetTypeForAllTypes) {
	graph::PropertyValue null_val;
	EXPECT_EQ(null_val.getType(), PropertyType::NULL_TYPE);
	EXPECT_EQ(null_val.typeName(), "NULL");

	graph::PropertyValue bool_val(true);
	EXPECT_EQ(bool_val.getType(), PropertyType::BOOLEAN);
	EXPECT_EQ(bool_val.typeName(), "BOOLEAN");

	graph::PropertyValue int_val(42);
	EXPECT_EQ(int_val.getType(), PropertyType::INTEGER);
	EXPECT_EQ(int_val.typeName(), "INTEGER");

	graph::PropertyValue double_val(3.14);
	EXPECT_EQ(double_val.getType(), PropertyType::DOUBLE);
	EXPECT_EQ(double_val.typeName(), "DOUBLE");

	graph::PropertyValue string_val("test");
	EXPECT_EQ(string_val.getType(), PropertyType::STRING);
	EXPECT_EQ(string_val.typeName(), "STRING");

	std::vector<float> list_vec = {1.0f, 2.0f};
	graph::PropertyValue list_val(list_vec);
	EXPECT_EQ(list_val.getType(), PropertyType::LIST);
	EXPECT_EQ(list_val.typeName(), "LIST");
}

// Test all comparison operators with different types
TEST(PropertyValueTest, AllComparisonOperators) {
	graph::PropertyValue a(10);
	graph::PropertyValue b(20);

	EXPECT_TRUE(a < b);
	EXPECT_TRUE(a <= b);
	EXPECT_TRUE(b > a);
	EXPECT_TRUE(b >= a);
	EXPECT_TRUE(a == a);
	EXPECT_TRUE(a != b);
}

// Test comparison with mixed types (by variant index)
TEST(PropertyValueTest, MixedTypeComparisons) {
	graph::PropertyValue null_val;
	graph::PropertyValue bool_val(true);
	graph::PropertyValue int_val(42);
	graph::PropertyValue double_val(3.14);
	graph::PropertyValue string_val("hello");

	// NULL < BOOL < INT < DOUBLE < STRING (by variant index)
	EXPECT_LT(null_val, bool_val);
	EXPECT_LT(bool_val, int_val);
	EXPECT_LT(int_val, double_val);
	EXPECT_LT(double_val, string_val);

	// Verify inverse
	EXPECT_GT(string_val, double_val);
	EXPECT_GT(double_val, int_val);
	EXPECT_GT(int_val, bool_val);
	EXPECT_GT(bool_val, null_val);
}

// Test checkPropertyMatch function directly
TEST(PropertyValueTest, CheckPropertyMatchAllTypes) {
	using graph::property_utils::checkPropertyMatch;

	EXPECT_TRUE(checkPropertyMatch(graph::PropertyValue(42), graph::PropertyValue(42)));
	EXPECT_FALSE(checkPropertyMatch(graph::PropertyValue(42), graph::PropertyValue(24)));
	EXPECT_TRUE(checkPropertyMatch(graph::PropertyValue("test"), graph::PropertyValue("test")));
	EXPECT_FALSE(checkPropertyMatch(graph::PropertyValue("test"), graph::PropertyValue("other")));
	EXPECT_TRUE(checkPropertyMatch(graph::PropertyValue(true), graph::PropertyValue(true)));
	EXPECT_FALSE(checkPropertyMatch(graph::PropertyValue(true), graph::PropertyValue(false)));
	EXPECT_TRUE(checkPropertyMatch(graph::PropertyValue(3.14), graph::PropertyValue(3.14)));
	EXPECT_FALSE(checkPropertyMatch(graph::PropertyValue(3.14), graph::PropertyValue(2.71)));

	std::vector<float> list1 = {1.0f, 2.0f};
	std::vector<float> list2 = {1.0f, 2.0f};
	EXPECT_TRUE(checkPropertyMatch(graph::PropertyValue(list1), graph::PropertyValue(list2)));
}
