#include <gtest/gtest.h>

#include "graph/query/execution/TypedValueKey.hpp"

using graph::PropertyValue;
using graph::TemporalDate;
using graph::TemporalDateTime;
using graph::TemporalDuration;
using graph::PropertyValueHash;
using graph::query::execution::TypedEqualityKey;
using graph::query::execution::TypedOrderKey;

namespace {
	int compareValues(const PropertyValue &left, const PropertyValue &right) {
		return TypedOrderKey::from(left).compare(TypedOrderKey::from(right));
	}
} // namespace

TEST(TypedOrderKeyTest, OrdersScalarValuesLikePropertyValue) {
	EXPECT_LT(compareValues(PropertyValue(), PropertyValue(false)), 0);
	EXPECT_LT(compareValues(PropertyValue(false), PropertyValue(true)), 0);
	EXPECT_LT(compareValues(PropertyValue(int64_t{1}), PropertyValue(int64_t{2})), 0);
	EXPECT_GT(compareValues(PropertyValue(2.5), PropertyValue(1.5)), 0);
	EXPECT_LT(compareValues(PropertyValue("a"), PropertyValue("b")), 0);
	EXPECT_EQ(compareValues(PropertyValue("same"), PropertyValue("same")), 0);
}

TEST(TypedOrderKeyTest, OrdersTemporalValuesWithoutGenericVariantCompare) {
	EXPECT_LT(compareValues(PropertyValue(TemporalDate{1}), PropertyValue(TemporalDate{2})), 0);
	EXPECT_GT(compareValues(PropertyValue(TemporalDateTime{20}), PropertyValue(TemporalDateTime{10})), 0);
	EXPECT_LT(
			compareValues(PropertyValue(TemporalDuration{0, 1, 0}), PropertyValue(TemporalDuration{0, 2, 0})),
			0);
}

TEST(TypedOrderKeyTest, PreservesMapNonOrderingSemantics) {
	PropertyValue::MapType left{{"a", PropertyValue(int64_t{1})}};
	PropertyValue::MapType right{{"a", PropertyValue(int64_t{2})}};

	EXPECT_EQ(compareValues(PropertyValue(left), PropertyValue(right)), 0);
}

TEST(TypedEqualityKeyTest, HashesAndComparesScalarValuesByType) {
	auto intOne = TypedEqualityKey::from(PropertyValue(int64_t{1}));
	auto intOneAgain = TypedEqualityKey::from(PropertyValue(int64_t{1}));
	auto stringOne = TypedEqualityKey::from(PropertyValue("1"));

	EXPECT_EQ(intOne, intOneAgain);
	EXPECT_EQ(intOne.hash(), intOneAgain.hash());
	EXPECT_FALSE(intOne == stringOne);
}

TEST(TypedEqualityKeyTest, HashesTemporalValuesWithoutFallbackMaterialization) {
	auto date = TypedEqualityKey::from(PropertyValue(TemporalDate{42}));
	auto sameDate = TypedEqualityKey::from(PropertyValue(TemporalDate{42}));
	auto dateTime = TypedEqualityKey::from(PropertyValue(TemporalDateTime{42}));

	EXPECT_EQ(date, sameDate);
	EXPECT_EQ(date.hash(), sameDate.hash());
	EXPECT_FALSE(date == dateTime);
}

TEST(TypedEqualityKeyTest, FallsBackToPropertyValueSemanticsForMapsAndLists) {
	PropertyValue::MapType map{{"a", PropertyValue(int64_t{1})}};
	std::vector<PropertyValue> list{PropertyValue(int64_t{1}), PropertyValue("x")};

	PropertyValue mapValue(map);
	PropertyValue listValue(list);

	EXPECT_EQ(TypedEqualityKey::from(mapValue), TypedEqualityKey::from(mapValue));
	EXPECT_EQ(TypedEqualityKey::from(mapValue).hash(), PropertyValueHash{}(mapValue));
	EXPECT_EQ(TypedEqualityKey::from(listValue), TypedEqualityKey::from(listValue));
	EXPECT_EQ(TypedEqualityKey::from(listValue).hash(), PropertyValueHash{}(listValue));
}
