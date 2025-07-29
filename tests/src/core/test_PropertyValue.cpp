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
#include "graph/core/PropertyValue.hpp"

using graph::property_utils::getPropertyValueSize;

TEST(PropertyValueTest, Monostate) {
	graph::PropertyValue v = std::monostate{};
	EXPECT_EQ(getPropertyValueSize(v), 1u);
}

TEST(PropertyValueTest, Bool) {
	graph::PropertyValue v = true;
	EXPECT_EQ(getPropertyValueSize(v), sizeof(bool));
}

TEST(PropertyValueTest, Int32) {
	graph::PropertyValue v = int32_t{42};
	EXPECT_EQ(getPropertyValueSize(v), sizeof(int32_t));
}

TEST(PropertyValueTest, Int64) {
	graph::PropertyValue v = int64_t{42};
	EXPECT_EQ(getPropertyValueSize(v), sizeof(int64_t));
}

TEST(PropertyValueTest, Float) {
	graph::PropertyValue v = float{3.14f};
	EXPECT_EQ(getPropertyValueSize(v), sizeof(float));
}

TEST(PropertyValueTest, Double) {
	graph::PropertyValue v = double{3.14};
	EXPECT_EQ(getPropertyValueSize(v), sizeof(double));
}

TEST(PropertyValueTest, String) {
	std::string s = "hello";
	graph::PropertyValue v = s;
	EXPECT_EQ(getPropertyValueSize(v), s.size() + sizeof(size_t));
}

TEST(PropertyValueTest, VectorInt64) {
	std::vector<int64_t> vec = {1, 2, 3};
	graph::PropertyValue v = vec;
	EXPECT_EQ(getPropertyValueSize(v), vec.size() * sizeof(int64_t) + sizeof(size_t));
}

TEST(PropertyValueTest, VectorDouble) {
	std::vector<double> vec = {1.1, 2.2};
	graph::PropertyValue v = vec;
	EXPECT_EQ(getPropertyValueSize(v), vec.size() * sizeof(double) + sizeof(size_t));
}

TEST(PropertyValueTest, VectorString) {
	std::vector<std::string> vec = {"a", "bb", "ccc"};
	graph::PropertyValue v = vec;
	size_t expected = sizeof(size_t);
	for (const auto &s: vec)
		expected += s.size() + sizeof(size_t);
	EXPECT_EQ(getPropertyValueSize(v), expected);
}
