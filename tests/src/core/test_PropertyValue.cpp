/**
 * @file test_PropertyValue.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/7/29
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include "graph/core/PropertyTypes.hpp"

using graph::property_utils::getPropertyValueSize;

TEST(PropertyValueTest, SizeOfMonostate) {
	graph::PropertyValue v = std::monostate{};
	EXPECT_EQ(getPropertyValueSize(v), 1u); // A minimal size for NULL.
}

TEST(PropertyValueTest, SizeOfBool) {
	graph::PropertyValue v = true;
	EXPECT_EQ(getPropertyValueSize(v), sizeof(bool));
}

TEST(PropertyValueTest, IntegersAreStoredAsInt64) {
	// An int literal is stored as int64_t.
	graph::PropertyValue v_int = 42;
	EXPECT_EQ(getPropertyValueSize(v_int), sizeof(int64_t));

	// An explicit int32_t is also promoted and stored as int64_t.
	graph::PropertyValue v_int32 = int32_t{42};
	EXPECT_EQ(getPropertyValueSize(v_int32), sizeof(int64_t));

	// An explicit int64_t is stored as int64_t.
	graph::PropertyValue v_int64 = int64_t{42};
	EXPECT_EQ(getPropertyValueSize(v_int64), sizeof(int64_t));
}

TEST(PropertyValueTest, FloatsAreStoredAsDouble) {
	// A float literal is promoted and stored as double.
	graph::PropertyValue v_float = 3.14f;
	EXPECT_EQ(getPropertyValueSize(v_float), sizeof(double));

	// A double literal is stored as double.
	graph::PropertyValue v_double = 3.14;
	EXPECT_EQ(getPropertyValueSize(v_double), sizeof(double));
}

TEST(PropertyValueTest, SizeOfString) {
	std::string s = "hello";
	graph::PropertyValue v = s;
	// The size is the string content + overhead for storing its length.
	EXPECT_EQ(getPropertyValueSize(v), s.size() + sizeof(size_t));
}
