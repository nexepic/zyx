/**
 * @file test_PropertyTypes_CompositeAndHash.cpp
 * @brief Focused tests for CompositeKey and hash/ordering branches.
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "graph/core/PropertyTypes.hpp"

using graph::CompositeKey;
using graph::PropertyValue;
using graph::PropertyValueHash;

TEST(PropertyTypesCompositeKeyTest, RelationalOperatorsAndToString) {
	CompositeKey keyA{{PropertyValue(int64_t(1)), PropertyValue("alpha")}};
	CompositeKey keyB{{PropertyValue(int64_t(1)), PropertyValue("beta")}};
	CompositeKey keyAPrefix{{PropertyValue(int64_t(1)), PropertyValue("alpha"), PropertyValue(int64_t(9))}};
	CompositeKey keyAClone{{PropertyValue(int64_t(1)), PropertyValue("alpha")}};

	EXPECT_TRUE(keyA == keyAClone);
	EXPECT_TRUE(keyA < keyB);
	EXPECT_TRUE(keyB > keyA);
	EXPECT_TRUE(keyA <= keyB);
	EXPECT_TRUE(keyB >= keyA);
	EXPECT_TRUE(keyA < keyAPrefix);
	EXPECT_EQ(keyA.toString(), "(1, alpha)");
}

TEST(PropertyTypesOrderingTest, PropertyValueLessThanCoversMonostateAndBoolVisitors) {
	PropertyValue nullValueA;
	PropertyValue nullValueB;
	EXPECT_FALSE(nullValueA < nullValueB);

	PropertyValue falseValue(false);
	PropertyValue trueValue(true);
	EXPECT_TRUE(falseValue < trueValue);
	EXPECT_FALSE(trueValue < falseValue);
}

TEST(PropertyTypesHashTest, PropertyValueHashCoversBoolDoubleAndVector) {
	PropertyValueHash hasher;

	EXPECT_EQ(hasher(PropertyValue(true)), hasher(PropertyValue(true)));
	EXPECT_EQ(hasher(PropertyValue(3.5)), hasher(PropertyValue(3.5)));

	std::vector<PropertyValue> v1{PropertyValue(true), PropertyValue(3.5), PropertyValue(int64_t(7))};
	std::vector<PropertyValue> v2{PropertyValue(true), PropertyValue(3.5), PropertyValue(int64_t(7))};
	EXPECT_EQ(hasher(PropertyValue(v1)), hasher(PropertyValue(v2)));
}
