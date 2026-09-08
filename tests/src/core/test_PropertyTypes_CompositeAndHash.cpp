/**
 * @file test_PropertyTypes_CompositeAndHash.cpp
 * @brief Focused tests for CompositeKey and hash/ordering branches.
 */

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

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

TEST(PropertyTypesOrderingTest, TemporalValuesUseTheirOwnPublicOrderingContracts) {
	EXPECT_LT(PropertyValue(graph::TemporalDate{1}), PropertyValue(graph::TemporalDate{2}));
	EXPECT_LT(PropertyValue(graph::TemporalDateTime{1}), PropertyValue(graph::TemporalDateTime{2}));
	EXPECT_LT(PropertyValue(graph::TemporalDuration{1, 2, 3}),
			  PropertyValue(graph::TemporalDuration{1, 2, 4}));
}

TEST(PropertyTypesAccessTest, TemporalValuesRetainTypeNamesAndIsoFormatting) {
	const PropertyValue date(graph::TemporalDate::fromISO("2024-01-15"));
	const PropertyValue dateTime(graph::TemporalDateTime::fromISO("2024-01-15T13:45:30"));
	const PropertyValue duration(graph::TemporalDuration::fromISO("P1Y2M3DT4H5M6S"));

	EXPECT_EQ(date.getType(), graph::PropertyType::DATE);
	EXPECT_EQ(date.typeName(), "DATE");
	EXPECT_EQ(date.toString(), "2024-01-15");
	EXPECT_EQ(graph::getPropertyType(date), graph::PropertyType::DATE);
	EXPECT_EQ(dateTime.getType(), graph::PropertyType::DATETIME);
	EXPECT_EQ(dateTime.typeName(), "DATETIME");
	EXPECT_EQ(dateTime.toString(), "2024-01-15T13:45:30");
	EXPECT_EQ(graph::getPropertyType(dateTime), graph::PropertyType::DATETIME);
	EXPECT_EQ(duration.getType(), graph::PropertyType::DURATION);
	EXPECT_EQ(duration.typeName(), "DURATION");
	EXPECT_EQ(duration.toString(), "P1Y2M3DT4H5M6S");
	EXPECT_EQ(graph::getPropertyType(duration), graph::PropertyType::DURATION);
}

TEST(PropertyTypesAccessTest, BinaryAccessorRejectsOtherPropertyKinds) {
	const PropertyValue bytes(PropertyValue::Bytes{std::byte{0xAB}});
	EXPECT_EQ(bytes.getBytes(), (PropertyValue::Bytes{std::byte{0xAB}}));
	EXPECT_THROW(PropertyValue("not bytes").getBytes(), std::runtime_error);
	EXPECT_THROW(PropertyValue(std::vector<PropertyValue>{}).getBytes(), std::runtime_error);
}

TEST(PropertyTypesHashTest, PropertyValueHashCoversBoolDoubleAndVector) {
	PropertyValueHash hasher;

	EXPECT_EQ(hasher(PropertyValue(true)), hasher(PropertyValue(true)));
	EXPECT_EQ(hasher(PropertyValue(3.5)), hasher(PropertyValue(3.5)));
	EXPECT_NE(hasher(PropertyValue(int64_t{1})), hasher(PropertyValue(std::string("1"))));

	std::vector<PropertyValue> v1{PropertyValue(true), PropertyValue(3.5), PropertyValue(int64_t(7))};
	std::vector<PropertyValue> v2{PropertyValue(true), PropertyValue(3.5), PropertyValue(int64_t(7))};
	EXPECT_EQ(hasher(PropertyValue(v1)), hasher(PropertyValue(v2)));

	EXPECT_EQ(hasher(PropertyValue(graph::TemporalDate{10})), hasher(PropertyValue(graph::TemporalDate{10})));
	EXPECT_EQ(hasher(PropertyValue(graph::TemporalDateTime{1000})),
	          hasher(PropertyValue(graph::TemporalDateTime{1000})));
	EXPECT_EQ(hasher(PropertyValue(graph::TemporalDuration{1, 2, 3})),
	          hasher(PropertyValue(graph::TemporalDuration{1, 2, 3})));
}
