#include <gtest/gtest.h>

#include <unordered_map>

#include "graph/query/execution/TypedDistinctSet.hpp"
#include "graph/query/execution/TypedGroupCounter.hpp"
#include "graph/query/execution/TypedValueKey.hpp"

using graph::PropertyValue;
using graph::TemporalDate;
using graph::TemporalDateTime;
using graph::TemporalDuration;
using graph::PropertyValueHash;
using graph::query::execution::TypedDistinctSet;
using graph::query::execution::TypedEqualityKey;
using graph::query::execution::TypedGroupCounter;
using graph::query::execution::TypedOrderKey;

namespace {
	std::unordered_map<std::string, int64_t> collectGroupCounts(const TypedGroupCounter &counter) {
		std::unordered_map<std::string, int64_t> groups;
		for (const auto &group : counter.toVector()) {
			switch (group.value.getType()) {
				case graph::PropertyType::NULL_TYPE:
					groups["null"] = group.count;
					break;
				case graph::PropertyType::BOOLEAN:
					groups[std::get<bool>(group.value.getVariant()) ? "true" : "false"] = group.count;
					break;
				case graph::PropertyType::INTEGER:
					groups["int:" + std::to_string(std::get<int64_t>(group.value.getVariant()))] = group.count;
					break;
				case graph::PropertyType::STRING:
					groups["string:" + std::get<std::string>(group.value.getVariant())] = group.count;
					break;
				case graph::PropertyType::DATE:
					groups["date:" + std::to_string(std::get<TemporalDate>(group.value.getVariant()).epochDays)] =
							group.count;
					break;
				case graph::PropertyType::DATETIME:
					groups["datetime:" +
						   std::to_string(std::get<TemporalDateTime>(group.value.getVariant()).epochMillis)] =
							group.count;
					break;
				default:
					groups["fallback"] += group.count;
					break;
			}
		}
		return groups;
	}
} // namespace

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

TEST(TypedGroupCounterTest, CountsScalarGroupsWithoutStringifying) {
	TypedGroupCounter counter;
	counter.add(PropertyValue());
	counter.add(PropertyValue(), 2);
	counter.add(PropertyValue(false));
	counter.add(PropertyValue(true), 3);
	counter.add(PropertyValue(int64_t{7}), 4);
	counter.add(PropertyValue("CN"));
	counter.add(PropertyValue("CN"), 5);
	counter.add(PropertyValue("US"), 2);
	counter.add(PropertyValue(TemporalDate{42}));
	counter.add(PropertyValue(TemporalDateTime{84}), 2);

	const auto groups = collectGroupCounts(counter);
	EXPECT_EQ(counter.size(), 8U);
	EXPECT_EQ(groups.at("null"), 3);
	EXPECT_EQ(groups.at("false"), 1);
	EXPECT_EQ(groups.at("true"), 3);
	EXPECT_EQ(groups.at("int:7"), 4);
	EXPECT_EQ(groups.at("string:CN"), 6);
	EXPECT_EQ(groups.at("string:US"), 2);
	EXPECT_EQ(groups.at("date:42"), 1);
	EXPECT_EQ(groups.at("datetime:84"), 2);
}

TEST(TypedGroupCounterTest, CountsFallbackGroupsByTypedEquality) {
	TypedGroupCounter counter;
	PropertyValue::MapType left{{"a", PropertyValue(int64_t{1})}};
	PropertyValue::MapType right{{"a", PropertyValue(int64_t{2})}};

	counter.add(PropertyValue(left));
	counter.add(PropertyValue(left), 2);
	counter.add(PropertyValue(right), 4);
	counter.add(PropertyValue(TemporalDuration{0, 1, 0}));
	counter.add(PropertyValue(TemporalDuration{0, 1, 0}), 3);

	EXPECT_EQ(counter.size(), 3U);

	int64_t fallbackRows = 0;
	for (const auto &group : counter.toVector()) {
		fallbackRows += group.count;
	}
	EXPECT_EQ(fallbackRows, 11);
}


TEST(TypedOrderKeyTest, FactoryMethodsCompareAllTypedBranches) {
	EXPECT_EQ(TypedOrderKey::fromNull().compare(TypedOrderKey::fromNull()), 0);
	EXPECT_LT(TypedOrderKey::fromBoolean(false).compare(TypedOrderKey::fromBoolean(true)), 0);
	EXPECT_GT(TypedOrderKey::fromInteger(10).compare(TypedOrderKey::fromInteger(5)), 0);
	EXPECT_EQ(TypedOrderKey::fromDouble(1.5).compare(TypedOrderKey::fromDouble(1.5)), 0);
	EXPECT_LT(TypedOrderKey::fromString("aa").compare(TypedOrderKey::fromString("ab")), 0);
	EXPECT_EQ(TypedOrderKey::fromDateEpochDays(42).compare(TypedOrderKey::from(PropertyValue(TemporalDate{42}))), 0);
	EXPECT_EQ(TypedOrderKey::fromDateTimeEpochMillis(84).compare(TypedOrderKey::from(PropertyValue(TemporalDateTime{84}))), 0);
	EXPECT_GT(TypedOrderKey::fromDuration(TemporalDuration{0, 2, 0}).compare(
				 TypedOrderKey::fromDuration(TemporalDuration{0, 1, 0})),
			  0);
	std::vector<PropertyValue> list{PropertyValue(int64_t{1})};
	EXPECT_EQ(TypedOrderKey::from(PropertyValue(list)).compare(TypedOrderKey::from(PropertyValue(list))), 0);
}

TEST(TypedEqualityKeyTest, ComparesEverySpecializedStorageType) {
	EXPECT_EQ(TypedEqualityKey::from(PropertyValue()), TypedEqualityKey::from(PropertyValue()));
	EXPECT_FALSE(TypedEqualityKey::from(PropertyValue(false)) == TypedEqualityKey::from(PropertyValue(true)));
	EXPECT_EQ(TypedEqualityKey::from(PropertyValue(1.25)), TypedEqualityKey::from(PropertyValue(1.25)));
	EXPECT_FALSE(TypedEqualityKey::from(PropertyValue(1.25)) == TypedEqualityKey::from(PropertyValue(2.5)));
	EXPECT_EQ(TypedEqualityKey::from(PropertyValue("x")), TypedEqualityKey::from(PropertyValue("x")));
	EXPECT_FALSE(TypedEqualityKey::from(PropertyValue("x")) == TypedEqualityKey::from(PropertyValue("y")));
	EXPECT_EQ(TypedEqualityKey::from(PropertyValue(TemporalDuration{1, 2, 3})),
			  TypedEqualityKey::from(PropertyValue(TemporalDuration{1, 2, 3})));
	EXPECT_FALSE(TypedEqualityKey::from(PropertyValue(TemporalDuration{1, 2, 3})) ==
				 TypedEqualityKey::from(PropertyValue(TemporalDuration{1, 2, 4})));
}

TEST(TypedDistinctSetTest, TracksAllFastScalarKindsAndFallbackKinds) {
	TypedDistinctSet set;
	EXPECT_TRUE(set.insert(PropertyValue()));
	EXPECT_FALSE(set.insert(PropertyValue()));
	EXPECT_TRUE(set.insert(PropertyValue(false)));
	EXPECT_FALSE(set.insert(PropertyValue(false)));
	EXPECT_TRUE(set.insert(PropertyValue(true)));
	EXPECT_TRUE(set.insert(PropertyValue(int64_t{7})));
	EXPECT_FALSE(set.insert(PropertyValue(int64_t{7})));
	EXPECT_TRUE(set.insert(PropertyValue(2.5)));
	EXPECT_FALSE(set.insert(PropertyValue(2.5)));
	EXPECT_TRUE(set.insert(PropertyValue("CN")));
	EXPECT_FALSE(set.insert(PropertyValue("CN")));
	EXPECT_TRUE(set.insert(PropertyValue(TemporalDate{10})));
	EXPECT_FALSE(set.insert(PropertyValue(TemporalDate{10})));
	EXPECT_TRUE(set.insert(PropertyValue(TemporalDateTime{20})));
	EXPECT_FALSE(set.insert(PropertyValue(TemporalDateTime{20})));
	EXPECT_TRUE(set.insert(PropertyValue(TemporalDuration{0, 1, 0})));
	EXPECT_FALSE(set.insert(PropertyValue(TemporalDuration{0, 1, 0})));
	std::vector<PropertyValue> list{PropertyValue(int64_t{1})};
	EXPECT_TRUE(set.insert(PropertyValue(list)));
	EXPECT_FALSE(set.insert(PropertyValue(list)));
	PropertyValue::MapType map{{"k", PropertyValue(int64_t{1})}};
	EXPECT_TRUE(set.insert(PropertyValue(map)));
	EXPECT_FALSE(set.insert(PropertyValue(map)));
	EXPECT_EQ(set.size(), 11U);

	set.clear();
	EXPECT_EQ(set.size(), 0U);
	EXPECT_TRUE(set.insert(PropertyValue("after-clear")));
	EXPECT_EQ(set.size(), 1U);
}

TEST(TypedGroupCounterTest, ClearAndIgnoreNonPositiveCounts) {
	TypedGroupCounter counter;
	counter.add(PropertyValue("ignored"), 0);
	counter.add(PropertyValue("ignored"), -3);
	EXPECT_EQ(counter.size(), 0U);

	counter.add(PropertyValue(3.25), 2);
	counter.add(PropertyValue(TemporalDuration{0, 1, 0}), 5);
	EXPECT_EQ(counter.size(), 2U);
	counter.clear();
	EXPECT_EQ(counter.size(), 0U);
	EXPECT_TRUE(counter.toVector().empty());
}

TEST(TypedOrderKeyTest, FallbackListValuesUsePropertyValueOrdering) {
	std::vector<PropertyValue> left{PropertyValue(int64_t{1})};
	std::vector<PropertyValue> right{PropertyValue(int64_t{2})};
	EXPECT_LT(TypedOrderKey::from(PropertyValue(left)).compare(TypedOrderKey::from(PropertyValue(right))), 0);
	EXPECT_GT(TypedOrderKey::from(PropertyValue(right)).compare(TypedOrderKey::from(PropertyValue(left))), 0);
}
