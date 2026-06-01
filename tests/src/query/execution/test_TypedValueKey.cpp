#include <gtest/gtest.h>

#include "graph/query/execution/TypedValueKey.hpp"

using graph::PropertyValue;
using graph::TemporalDate;
using graph::TemporalDateTime;
using graph::TemporalDuration;
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
