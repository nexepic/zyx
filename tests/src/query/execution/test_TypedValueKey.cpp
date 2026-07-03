#include <gtest/gtest.h>

#include <string_view>
#include <unordered_map>

#include "graph/query/execution/TypedDistinctSet.hpp"
#include "graph/query/execution/TypedGroupCounter.hpp"
#include "graph/query/execution/TypedScalarValue.hpp"
#include "graph/query/execution/TypedValueKey.hpp"

using graph::PropertyValue;
using graph::TemporalDate;
using graph::TemporalDateTime;
using graph::TemporalDuration;
using graph::PropertyValueHash;
using graph::query::execution::CompactGroupMap;
using graph::query::execution::StringCompactGroupMap;
using graph::query::execution::TypedDistinctSet;
using graph::query::execution::TypedEqualityKey;
using graph::query::execution::TypedGroupCounter;
using graph::query::execution::TypedScalarValue;
using graph::query::execution::TypedOrderKey;

namespace {
	TypedScalarValue scalarValue(graph::PropertyType type) {
		TypedScalarValue value;
		value.type = type;
		return value;
	}

	TypedScalarValue scalarBoolean(bool actual) {
		auto value = scalarValue(graph::PropertyType::BOOLEAN);
		value.boolValue = actual;
		return value;
	}

	TypedScalarValue scalarInteger(graph::PropertyType type, int64_t actual) {
		auto value = scalarValue(type);
		value.intValue = actual;
		return value;
	}

	TypedScalarValue scalarDouble(double actual) {
		auto value = scalarValue(graph::PropertyType::DOUBLE);
		value.doubleValue = actual;
		return value;
	}

	TypedScalarValue scalarString(std::string_view actual) {
		auto value = scalarValue(graph::PropertyType::STRING);
		value.stringValue = actual;
		return value;
	}

	TypedScalarValue scalarDuration(const TemporalDuration &actual) {
		auto value = scalarValue(graph::PropertyType::DURATION);
		value.durationValue = actual;
		return value;
	}

	TypedScalarValue scalarFallback(graph::PropertyType type, const PropertyValue &actual) {
		auto value = scalarValue(type);
		value.fallbackValue = &actual;
		return value;
	}

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
				case graph::PropertyType::DOUBLE:
					groups["double:" + std::to_string(std::get<double>(group.value.getVariant()))] = group.count;
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
	EXPECT_GT(compareValues(PropertyValue(false), PropertyValue()), 0);
	EXPECT_LT(compareValues(PropertyValue(false), PropertyValue(true)), 0);
	EXPECT_LT(compareValues(PropertyValue(int64_t{1}), PropertyValue(int64_t{2})), 0);
	EXPECT_GT(compareValues(PropertyValue(2.5), PropertyValue(1.5)), 0);
	EXPECT_GT(compareValues(PropertyValue("z"), PropertyValue(int64_t{1})), 0);
	EXPECT_LT(compareValues(PropertyValue("a"), PropertyValue("b")), 0);
	EXPECT_EQ(compareValues(PropertyValue("same"), PropertyValue("same")), 0);
}

TEST(TypedOrderKeyTest, OrdersTemporalValuesWithoutGenericVariantCompare) {
	EXPECT_LT(compareValues(PropertyValue(TemporalDate{1}), PropertyValue(TemporalDate{2})), 0);
	EXPECT_GT(compareValues(PropertyValue(TemporalDateTime{20}), PropertyValue(TemporalDateTime{10})), 0);
	EXPECT_LT(
			compareValues(PropertyValue(TemporalDuration{0, 1, 0}), PropertyValue(TemporalDuration{0, 2, 0})),
			0);
	EXPECT_GT(
			compareValues(PropertyValue(TemporalDuration{0, 2, 0}), PropertyValue(TemporalDuration{0, 1, 0})),
			0);
	EXPECT_EQ(
			compareValues(PropertyValue(TemporalDuration{1, 0, 0}), PropertyValue(TemporalDuration{1, 0, 0})),
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
	counter.addScalar(scalarValue(graph::PropertyType::NULL_TYPE));
	counter.addNull(2);
	counter.addScalar(scalarBoolean(false));
	counter.addScalar(scalarBoolean(true), 3);
	counter.addScalar(scalarInteger(graph::PropertyType::INTEGER, int64_t{7}), 4);
	counter.addScalar(scalarString("CN"));
	counter.addString(std::string_view("CN"), 5);
	counter.addString("US", 2);
	counter.addScalar(scalarInteger(graph::PropertyType::DATE, 42));
	counter.addScalar(scalarInteger(graph::PropertyType::DATETIME, 84), 2);

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

	counter.add(PropertyValue(int64_t{7}));
	counter.add(PropertyValue(TemporalDate{42}));
	const auto updatedGroups = collectGroupCounts(counter);
	EXPECT_EQ(updatedGroups.at("int:7"), 5);
	EXPECT_EQ(updatedGroups.at("date:42"), 2);
}

TEST(TypedGroupCounterTest, MergeFromCombinesTypedPartitions) {
	TypedGroupCounter left;
	TypedGroupCounter right;
	const auto duration = TemporalDuration{1, 2, 3};
	std::vector<PropertyValue> list{PropertyValue(int64_t{9})};

	left.addNull();
	left.addInteger(7, 2);
	left.addString("CN");
	left.add(PropertyValue(list));

	right.addNull(3);
	right.addInteger(7);
	right.addBoolean(true, 4);
	right.addDouble(1.25, 2);
	right.addString("CN", 2);
	right.addDateEpochDays(42, 3);
	right.addDateTimeEpochMillis(84, 6);
	right.add(PropertyValue(duration), 5);

	left.mergeFrom(right);
	const auto groups = collectGroupCounts(left);
	EXPECT_EQ(groups.at("null"), 4);
	EXPECT_EQ(groups.at("true"), 4);
	EXPECT_EQ(groups.at("int:7"), 3);
	EXPECT_EQ(groups.at("double:1.250000"), 2);
	EXPECT_EQ(groups.at("string:CN"), 3);
	EXPECT_EQ(groups.at("date:42"), 3);
	EXPECT_EQ(groups.at("datetime:84"), 6);
	EXPECT_EQ(groups.at("fallback"), 6);
}

TEST(TypedGroupCounterTest, CountsScalarAdapterValuesAndFallbacks) {
	TypedGroupCounter counter;
	const auto duration = TemporalDuration{0, 1, 0};
	std::vector<PropertyValue> list{PropertyValue(int64_t{1})};
	const PropertyValue listValue(list);

	counter.addScalar(scalarDouble(1.25));
	counter.addScalar(scalarDuration(duration));
	counter.addScalar(scalarFallback(graph::PropertyType::LIST, listValue), 2);
	counter.addScalar(scalarValue(graph::PropertyType::UNKNOWN));
	counter.addScalar(scalarValue(graph::PropertyType::MAP), 0);

	const auto groups = collectGroupCounts(counter);
	EXPECT_EQ(counter.size(), 4U);
	EXPECT_EQ(groups.at("null"), 1);
	EXPECT_EQ(groups.at("double:1.250000"), 1);
	EXPECT_EQ(groups.at("fallback"), 3);
}

TEST(TypedGroupCounterTest, CompactScalarCountersPromoteWithoutChangingCounts) {
	TypedGroupCounter counter;
	for (int64_t value = 0; value < 40; ++value) {
		counter.addInteger(value);
		counter.addInteger(value);
		counter.addDouble(static_cast<double>(value), 2);
		counter.addString("bucket-" + std::to_string(value), 3);
		counter.addDateEpochDays(static_cast<int32_t>(value), 4);
		counter.addDateTimeEpochMillis(value, 5);
	}

	const auto groups = collectGroupCounts(counter);
	EXPECT_EQ(counter.size(), 200U);
	for (int64_t value = 0; value < 40; ++value) {
		EXPECT_EQ(groups.at("int:" + std::to_string(value)), 2);
		EXPECT_EQ(groups.at("double:" + std::to_string(static_cast<double>(value))), 2);
		EXPECT_EQ(groups.at("string:bucket-" + std::to_string(value)), 3);
		EXPECT_EQ(groups.at("date:" + std::to_string(value)), 4);
		EXPECT_EQ(groups.at("datetime:" + std::to_string(value)), 5);
	}

	counter.clear();
	EXPECT_EQ(counter.size(), 0U);
	counter.addInteger(7, 4);
	EXPECT_EQ(collectGroupCounts(counter).at("int:7"), 4);
}

TEST(TypedGroupCounterTest, CompactMapIgnoresNonPositiveCounts) {
	CompactGroupMap<int64_t> integerMap;
	integerMap.add(1, 0);
	integerMap.add(2, -1);
	integerMap.add(3, 1);
	integerMap.add(3, 2);
	EXPECT_EQ(integerMap.size(), 1U);

	CompactGroupMap<std::string> stringMap;
	stringMap.add("ignored", 0);
	stringMap.add("kept", 1);
	stringMap.add("kept", 1);
	EXPECT_EQ(stringMap.size(), 1U);

	StringCompactGroupMap stringViewMap;
	stringViewMap.add("ignored", 0);
	stringViewMap.add("kept", 1);
	stringViewMap.add(std::string_view("kept"), 2);
	EXPECT_EQ(stringViewMap.size(), 1U);
	int64_t stringCount = 0;
	stringViewMap.forEach([&](const std::string &value, int64_t count) {
		EXPECT_EQ(value, "kept");
		stringCount += count;
	});
	EXPECT_EQ(stringCount, 3);

	CompactGroupMap<double> doubleMap;
	doubleMap.add(1.0, 0);
	doubleMap.add(1.0, 1);
	doubleMap.add(1.0, 1);
	EXPECT_EQ(doubleMap.size(), 1U);

	CompactGroupMap<int32_t> dateMap;
	dateMap.add(1, 0);
	dateMap.add(1, 1);
	dateMap.add(1, 1);
	EXPECT_EQ(dateMap.size(), 1U);
}

TEST(TypedGroupCounterTest, GenericCompactMapPromotesStringKeys) {
	CompactGroupMap<std::string> map;
	for (int i = 0; i < 24; ++i) {
		map.add("key-" + std::to_string(i), 1);
	}
	map.add("key-7", 4);

	EXPECT_EQ(map.size(), 24U);
	std::unordered_map<std::string, int64_t> counts;
	map.forEach([&](const std::string &value, int64_t count) {
		counts[value] = count;
	});
	EXPECT_EQ(counts.at("key-7"), 5);
	EXPECT_EQ(counts.at("key-23"), 1);

	map.clear();
	EXPECT_EQ(map.size(), 0U);
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
	EXPECT_EQ(TypedEqualityKey::from(PropertyValue(TemporalDateTime{123})),
			  TypedEqualityKey::from(PropertyValue(TemporalDateTime{123})));
	EXPECT_FALSE(TypedEqualityKey::from(PropertyValue(TemporalDateTime{123})) ==
				 TypedEqualityKey::from(PropertyValue(TemporalDateTime{124})));
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

TEST(TypedDistinctSetTest, MergeFromCombinesDistinctPartitions) {
	TypedDistinctSet left;
	TypedDistinctSet right;
	PropertyValue::MapType map{{"k", PropertyValue(int64_t{1})}};

	left.insert(PropertyValue());
	left.insert(PropertyValue(false));
	left.insert(PropertyValue(int64_t{7}));
	left.insert(PropertyValue("CN"));

	right.insert(PropertyValue());
	right.insert(PropertyValue(false));
	right.insert(PropertyValue(true));
	right.insert(PropertyValue(int64_t{7}));
	right.insert(PropertyValue(2.5));
	right.insert(PropertyValue(TemporalDate{42}));
	right.insert(PropertyValue(TemporalDateTime{84}));
	right.insert(PropertyValue(TemporalDuration{0, 1, 0}));
	right.insert(PropertyValue(map));

	left.mergeFrom(right);
	EXPECT_EQ(left.size(), 10U);
}

TEST(TypedDistinctSetTest, InsertsScalarValuesWithoutMaterializingFastKinds) {
	TypedDistinctSet set;
	const auto duration = TemporalDuration{1, 2, 3};
	std::vector<PropertyValue> list{PropertyValue(int64_t{1})};
	const PropertyValue listValue(list);

	EXPECT_TRUE(set.insertScalar(scalarValue(graph::PropertyType::NULL_TYPE)));
	EXPECT_FALSE(set.insertScalar(scalarValue(graph::PropertyType::NULL_TYPE)));
	EXPECT_TRUE(set.insertScalar(scalarBoolean(true)));
	EXPECT_FALSE(set.insertScalar(scalarBoolean(true)));
	EXPECT_TRUE(set.insertScalar(scalarInteger(graph::PropertyType::INTEGER, 7)));
	EXPECT_FALSE(set.insertScalar(scalarInteger(graph::PropertyType::INTEGER, 7)));
	EXPECT_TRUE(set.insertScalar(scalarDouble(1.5)));
	EXPECT_FALSE(set.insertScalar(scalarDouble(1.5)));
	EXPECT_TRUE(set.insertScalar(scalarString("CN")));
	EXPECT_FALSE(set.insertScalar(scalarString("CN")));
	EXPECT_TRUE(set.insertScalar(scalarInteger(graph::PropertyType::DATE, 42)));
	EXPECT_FALSE(set.insertScalar(scalarInteger(graph::PropertyType::DATE, 42)));
	EXPECT_TRUE(set.insertScalar(scalarInteger(graph::PropertyType::DATETIME, 84)));
	EXPECT_FALSE(set.insertScalar(scalarInteger(graph::PropertyType::DATETIME, 84)));
	EXPECT_TRUE(set.insertScalar(scalarDuration(duration)));
	EXPECT_FALSE(set.insertScalar(scalarDuration(duration)));
	EXPECT_TRUE(set.insertScalar(scalarFallback(graph::PropertyType::LIST, listValue)));
	EXPECT_FALSE(set.insertScalar(scalarFallback(graph::PropertyType::LIST, listValue)));
	EXPECT_EQ(set.size(), 9U);
}

TEST(TypedDistinctSetTest, HandlesFallbackScalarKindsWithoutStoredFallbackValue) {
	TypedDistinctSet set;
	auto mapWithoutFallback = scalarValue(graph::PropertyType::MAP);
	auto compositeWithoutFallback = scalarValue(graph::PropertyType::COMPOSITE);

	EXPECT_TRUE(set.insertScalar(scalarBoolean(false)));
	EXPECT_FALSE(set.insertScalar(scalarBoolean(false)));
	EXPECT_TRUE(set.insertScalar(mapWithoutFallback));
	EXPECT_FALSE(set.insertScalar(compositeWithoutFallback));
	EXPECT_FALSE(set.insertScalar(scalarValue(graph::PropertyType::UNKNOWN)));
	EXPECT_EQ(set.size(), 2U);

	set.clear();
	EXPECT_EQ(set.size(), 0U);
	EXPECT_TRUE(set.insertScalar(compositeWithoutFallback));
	EXPECT_EQ(set.size(), 1U);
}

TEST(TypedDistinctSetTest, DirectFallbackInsertsPreserveStructuredValueSemantics) {
	TypedDistinctSet set;
	PropertyValue::MapType firstMap{{"k", PropertyValue(int64_t{1})}};
	PropertyValue::MapType secondMap{{"k", PropertyValue(int64_t{2})}};

	EXPECT_TRUE(set.insert(PropertyValue(TemporalDuration{0, 0, 1})));
	EXPECT_TRUE(set.insert(PropertyValue(firstMap)));
	EXPECT_TRUE(set.insert(PropertyValue(secondMap)));
	EXPECT_FALSE(set.insert(PropertyValue(firstMap)));
	EXPECT_EQ(set.size(), 3U);
}

TEST(TypedScalarValueTest, ConvertsScalarValuesForOrderingAndFallbackValues) {
	const auto stringValue = scalarString("abc");
	EXPECT_EQ(std::get<std::string>(propertyValueFromScalar(stringValue).getVariant()), "abc");
	EXPECT_EQ(orderKeyFromScalar(stringValue).compare(TypedOrderKey::from(PropertyValue("abc"))), 0);
	EXPECT_TRUE(isNullScalar(scalarValue(graph::PropertyType::NULL_TYPE)));
	EXPECT_TRUE(isNullScalar(scalarValue(graph::PropertyType::UNKNOWN)));
	EXPECT_TRUE(isNullScalar(scalarValue(graph::PropertyType::COMPOSITE)));
	EXPECT_EQ(propertyValueFromScalar(scalarValue(graph::PropertyType::UNKNOWN)), PropertyValue());
	EXPECT_EQ(orderKeyFromScalar(scalarValue(graph::PropertyType::UNKNOWN)).compare(TypedOrderKey::fromNull()), 0);

	PropertyValue::MapType map{{"k", PropertyValue(int64_t{1})}};
	const PropertyValue mapValue(map);
	const auto fallback = scalarFallback(graph::PropertyType::MAP, mapValue);
	EXPECT_EQ(propertyValueFromScalar(fallback), mapValue);
	EXPECT_EQ(orderKeyFromScalar(fallback).compare(TypedOrderKey::from(mapValue)), 0);
	EXPECT_FALSE(isNullScalar(fallback));

	const auto compositeFallback = scalarFallback(graph::PropertyType::COMPOSITE, mapValue);
	EXPECT_EQ(propertyValueFromScalar(compositeFallback), mapValue);
	EXPECT_EQ(orderKeyFromScalar(compositeFallback).compare(TypedOrderKey::from(mapValue)), 0);
	EXPECT_FALSE(isNullScalar(compositeFallback));
}

TEST(TypedGroupCounterTest, ClearAndIgnoreNonPositiveCounts) {
	TypedGroupCounter counter;
	counter.add(PropertyValue("ignored"), 0);
	counter.add(PropertyValue("ignored"), -3);
	counter.addNull(0);
	counter.addBoolean(true, 0);
	counter.addInteger(1, 0);
	counter.addDouble(1.0, 0);
	counter.addString("ignored", 0);
	counter.addDateEpochDays(1, 0);
	counter.addDateTimeEpochMillis(1, 0);
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
