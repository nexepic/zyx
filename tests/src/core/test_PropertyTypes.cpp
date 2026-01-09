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
