#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/query/execution/PropertyPredicateKernel.hpp"

namespace {

using graph::PropertyValue;
using graph::TemporalDate;
using graph::query::execution::NodeColumnBatch;
using graph::query::execution::PropertyPredicateKernel;
using graph::query::execution::VectorPredicateOp;
using graph::query::execution::VectorizedPropertyPredicate;
using graph::storage::PropertyEntityPredicateOp;

VectorizedPropertyPredicate predicate(std::string key, VectorPredicateOp op, PropertyValue value) {
	VectorizedPropertyPredicate result;
	result.propertyKey = std::move(key);
	result.op = op;
	result.value = std::move(value);
	return result;
}

TEST(PropertyPredicateKernelTest, MatchesTypedScalarPredicatesWithoutChangingSemantics) {
	PropertyPredicateKernel kernel({predicate("age", VectorPredicateOp::VPO_GE, PropertyValue(int64_t{30})),
	                                predicate("country", VectorPredicateOp::VPO_EQ, PropertyValue("CN"))});

	const std::unordered_map<std::string, PropertyValue> match = {
		{"age", PropertyValue(int64_t{42})},
		{"country", PropertyValue("CN")},
	};
	const std::unordered_map<std::string, PropertyValue> miss = {
		{"age", PropertyValue(int64_t{29})},
		{"country", PropertyValue("CN")},
	};

	EXPECT_TRUE(kernel.matchesMap(match));
	EXPECT_FALSE(kernel.matchesMap(miss));
}

TEST(PropertyPredicateKernelTest, SupportsComparisonOperatorsForCommonScalarTypes) {
	const auto expectMatch = [](const PropertyValue &actual, VectorPredicateOp op, const PropertyValue &bound) {
		PropertyPredicateKernel kernel({predicate("value", op, bound)});
		EXPECT_TRUE(kernel.matchesMap({{"value", actual}}));
	};
	const auto expectMiss = [](const PropertyValue &actual, VectorPredicateOp op, const PropertyValue &bound) {
		PropertyPredicateKernel kernel({predicate("value", op, bound)});
		EXPECT_FALSE(kernel.matchesMap({{"value", actual}}));
	};

	expectMatch(PropertyValue(4.5), VectorPredicateOp::VPO_EQ, PropertyValue(4.5));
	expectMatch(PropertyValue(4.5), VectorPredicateOp::VPO_NE, PropertyValue(3.5));
	expectMatch(PropertyValue(4.5), VectorPredicateOp::VPO_LT, PropertyValue(5.5));
	expectMatch(PropertyValue(4.5), VectorPredicateOp::VPO_LE, PropertyValue(4.5));
	expectMatch(PropertyValue(4.5), VectorPredicateOp::VPO_GT, PropertyValue(3.5));
	expectMatch(PropertyValue(4.5), VectorPredicateOp::VPO_GE, PropertyValue(4.5));
	expectMiss(PropertyValue(4.5), VectorPredicateOp::VPO_EQ, PropertyValue(3.5));

	expectMatch(PropertyValue(true), VectorPredicateOp::VPO_EQ, PropertyValue(true));
	expectMatch(PropertyValue(true), VectorPredicateOp::VPO_NE, PropertyValue(false));
	expectMatch(PropertyValue(false), VectorPredicateOp::VPO_LT, PropertyValue(true));
	expectMatch(PropertyValue(false), VectorPredicateOp::VPO_LE, PropertyValue(false));
	expectMatch(PropertyValue(true), VectorPredicateOp::VPO_GT, PropertyValue(false));
	expectMatch(PropertyValue(true), VectorPredicateOp::VPO_GE, PropertyValue(true));
	expectMiss(PropertyValue(true), VectorPredicateOp::VPO_NE, PropertyValue(true));

	expectMatch(PropertyValue("b"), VectorPredicateOp::VPO_EQ, PropertyValue("b"));
	expectMatch(PropertyValue("b"), VectorPredicateOp::VPO_NE, PropertyValue("a"));
	expectMatch(PropertyValue("b"), VectorPredicateOp::VPO_LT, PropertyValue("c"));
	expectMatch(PropertyValue("b"), VectorPredicateOp::VPO_LE, PropertyValue("b"));
	expectMatch(PropertyValue("b"), VectorPredicateOp::VPO_GT, PropertyValue("a"));
	expectMatch(PropertyValue("b"), VectorPredicateOp::VPO_GE, PropertyValue("b"));
	expectMiss(PropertyValue("b"), VectorPredicateOp::VPO_LT, PropertyValue("a"));
}

TEST(PropertyPredicateKernelTest, SupportsClosedRangeForDoubleAndString) {
	auto doubleRange = predicate("value", VectorPredicateOp::VPO_RANGE_CLOSED, PropertyValue(1.5));
	doubleRange.upperValue = PropertyValue(2.5);
	PropertyPredicateKernel doubleKernel({doubleRange});
	EXPECT_TRUE(doubleKernel.matchesMap({{"value", PropertyValue(2.0)}}));
	EXPECT_FALSE(doubleKernel.matchesMap({{"value", PropertyValue(3.0)}}));

	auto stringRange = predicate("value", VectorPredicateOp::VPO_RANGE_CLOSED, PropertyValue("b"));
	stringRange.upperValue = PropertyValue("d");
	PropertyPredicateKernel stringKernel({stringRange});
	EXPECT_TRUE(stringKernel.matchesMap({{"value", PropertyValue("c")}}));
	EXPECT_FALSE(stringKernel.matchesMap({{"value", PropertyValue("z")}}));
}

TEST(PropertyPredicateKernelTest, MissingPropertiesDoNotMatchEvenForNotEqual) {
	PropertyPredicateKernel kernel({predicate("status", VectorPredicateOp::VPO_NE, PropertyValue("deleted"))});

	EXPECT_FALSE(kernel.matchesMap({}));
	EXPECT_TRUE(kernel.matchesMap({{"status", PropertyValue("active")}}));
	EXPECT_FALSE(kernel.matchesMap({{"status", PropertyValue("deleted")}}));
}

TEST(PropertyPredicateKernelTest, EqualityMapFactoryBuildsReusableKernel) {
	const auto kernel = PropertyPredicateKernel::fromEqualityPredicates({
		{"kind", PropertyValue("person")},
		{"active", PropertyValue(true)},
	});

	EXPECT_TRUE(kernel.matchesMap({{"kind", PropertyValue("person")}, {"active", PropertyValue(true)}}));
	EXPECT_FALSE(kernel.matchesMap({{"kind", PropertyValue("person")}, {"active", PropertyValue(false)}}));
}

TEST(PropertyPredicateKernelTest, AppliesClosedRangeAndSelectionVectorInOnePass) {
	auto score = predicate("score", VectorPredicateOp::VPO_RANGE_CLOSED, PropertyValue(int64_t{10}));
	score.upperValue = PropertyValue(int64_t{20});
	PropertyPredicateKernel kernel({score});

	NodeColumnBatch batch;
	batch.nodeIds = {1, 2, 3, 4};
	batch.selected = {1, 0, 1, 1};
	batch.propertyColumns["score"] = {
		PropertyValue(int64_t{9}),
		PropertyValue(int64_t{15}),
		PropertyValue(int64_t{20}),
		PropertyValue(int64_t{21}),
	};

	kernel.apply(batch);

	ASSERT_EQ(batch.selected.size(), 4U);
	EXPECT_EQ(batch.selected[0], 0U);
	EXPECT_EQ(batch.selected[1], 0U);
	EXPECT_EQ(batch.selected[2], 1U);
	EXPECT_EQ(batch.selected[3], 0U);
}

TEST(PropertyPredicateKernelTest, FallsBackToGenericValueComparisonForStructuredValues) {
	std::vector<PropertyValue> expectedList{PropertyValue(int64_t{1}), PropertyValue("two")};
	PropertyValue::MapType expectedMap;
	expectedMap.emplace("nested", PropertyValue(int64_t{7}));

	PropertyPredicateKernel kernel({predicate("items", VectorPredicateOp::VPO_EQ, PropertyValue(expectedList)),
	                                predicate("meta", VectorPredicateOp::VPO_EQ, PropertyValue(expectedMap))});

	PropertyValue::MapType actualMap;
	actualMap.emplace("nested", PropertyValue(int64_t{7}));
	const std::unordered_map<std::string, PropertyValue> properties = {
		{"items", PropertyValue(expectedList)},
		{"meta", PropertyValue(actualMap)},
	};

	EXPECT_TRUE(kernel.matchesMap(properties));
}

TEST(PropertyPredicateKernelTest, FallsBackToGenericComparisonWhenTypesDoNotMatchTypedPath) {
	PropertyPredicateKernel lessThanDouble({predicate("value", VectorPredicateOp::VPO_LT, PropertyValue(1.5))});
	PropertyPredicateKernel greaterThanBool({predicate("value", VectorPredicateOp::VPO_GT, PropertyValue(false))});

	EXPECT_TRUE(lessThanDouble.matchesMap({{"value", PropertyValue(int64_t{7})}}));
	EXPECT_TRUE(greaterThanBool.matchesMap({{"value", PropertyValue(int64_t{1})}}));
}

TEST(PropertyPredicateKernelTest, UndersizedColumnsTreatMissingCellsAsAbsent) {
	PropertyPredicateKernel kernel({predicate("age", VectorPredicateOp::VPO_EQ, PropertyValue(int64_t{42}))});

	NodeColumnBatch batch;
	batch.nodeIds = {1, 2};
	batch.propertyColumns["age"] = {PropertyValue(int64_t{42})};

	kernel.apply(batch);

	ASSERT_EQ(batch.selected.size(), 2U);
	EXPECT_EQ(batch.selected[0], 1U);
	EXPECT_EQ(batch.selected[1], 0U);
}

TEST(PropertyPredicateKernelTest, ConvertsPredicatesForSerializedPropertyEntityScan) {
	auto range = predicate("score", VectorPredicateOp::VPO_RANGE_CLOSED, PropertyValue(int64_t{1}));
	range.upperValue = PropertyValue(int64_t{9});
	PropertyPredicateKernel kernel({predicate("age", VectorPredicateOp::VPO_GT, PropertyValue(int64_t{30})), range});

	const auto storagePredicates = kernel.toStoragePredicates();

	ASSERT_EQ(storagePredicates.size(), 2U);
	EXPECT_EQ(storagePredicates[0].key, "age");
	EXPECT_EQ(storagePredicates[0].op, PropertyEntityPredicateOp::PEP_GT);
	EXPECT_EQ(storagePredicates[0].value, PropertyValue(int64_t{30}));
	EXPECT_FALSE(storagePredicates[0].upperValue.has_value());
	EXPECT_EQ(storagePredicates[1].key, "score");
	EXPECT_EQ(storagePredicates[1].op, PropertyEntityPredicateOp::PEP_RANGE_CLOSED);
	ASSERT_TRUE(storagePredicates[1].upperValue.has_value());
	EXPECT_EQ(*storagePredicates[1].upperValue, PropertyValue(int64_t{9}));
}


TEST(PropertyPredicateKernelTest, CoversIntegerRangeAndPublicSinglePredicateHelper) {
	auto range = predicate("value", VectorPredicateOp::VPO_RANGE_CLOSED, PropertyValue(int64_t{10}));
	range.upperValue = PropertyValue(int64_t{20});
	EXPECT_TRUE(graph::query::execution::evaluatePredicateWithKernel(PropertyValue(int64_t{15}), range));
	EXPECT_FALSE(graph::query::execution::evaluatePredicateWithKernel(PropertyValue(int64_t{25}), range));

	auto missingUpper = predicate("value", VectorPredicateOp::VPO_RANGE_CLOSED, PropertyValue(int64_t{10}));
	EXPECT_FALSE(graph::query::execution::evaluatePredicateWithKernel(PropertyValue(int64_t{15}), missingUpper));
	EXPECT_FALSE(graph::query::execution::evaluatePredicateWithKernel(std::optional<PropertyValue>{}, range));
}

TEST(PropertyPredicateKernelTest, ConvertsEveryStoragePredicateOperator) {
	PropertyPredicateKernel kernel({
		predicate("eq", VectorPredicateOp::VPO_EQ, PropertyValue(int64_t{1})),
		predicate("ne", VectorPredicateOp::VPO_NE, PropertyValue(int64_t{1})),
		predicate("lt", VectorPredicateOp::VPO_LT, PropertyValue(int64_t{1})),
		predicate("le", VectorPredicateOp::VPO_LE, PropertyValue(int64_t{1})),
		predicate("gt", VectorPredicateOp::VPO_GT, PropertyValue(int64_t{1})),
		predicate("ge", VectorPredicateOp::VPO_GE, PropertyValue(int64_t{1})),
	});
	const auto storagePredicates = kernel.toStoragePredicates();
	ASSERT_EQ(storagePredicates.size(), 6U);
	EXPECT_EQ(storagePredicates[0].op, PropertyEntityPredicateOp::PEP_EQ);
	EXPECT_EQ(storagePredicates[1].op, PropertyEntityPredicateOp::PEP_NE);
	EXPECT_EQ(storagePredicates[2].op, PropertyEntityPredicateOp::PEP_LT);
	EXPECT_EQ(storagePredicates[3].op, PropertyEntityPredicateOp::PEP_LE);
	EXPECT_EQ(storagePredicates[4].op, PropertyEntityPredicateOp::PEP_GT);
	EXPECT_EQ(storagePredicates[5].op, PropertyEntityPredicateOp::PEP_GE);
}

TEST(PropertyPredicateKernelTest, MissingColumnClearsSelectionVector) {
	PropertyPredicateKernel kernel({predicate("missing", VectorPredicateOp::VPO_EQ, PropertyValue(int64_t{1}))});
	NodeColumnBatch batch;
	batch.nodeIds = {1, 2, 3};
	batch.selected = {1, 1, 0};
	batch.propertyColumns["other"] = {PropertyValue(int64_t{1}), PropertyValue(int64_t{1}), PropertyValue(int64_t{1})};
	kernel.apply(batch);
	EXPECT_EQ(batch.selected, (std::vector<uint8_t>{0, 0, 0}));
}

} // namespace

namespace {
TEST(PropertyPredicateKernelAdditionalTest, GenericFallbackCoversTemporalAndStructuredComparisons) {
	auto expect = [](const PropertyValue &actual, VectorPredicateOp op, const PropertyValue &bound, bool expected) {
		PropertyPredicateKernel kernel({predicate("value", op, bound)});
		EXPECT_EQ(kernel.matchesMap({{"value", actual}}), expected);
	};

	expect(PropertyValue(TemporalDate{5}), VectorPredicateOp::VPO_NE, PropertyValue(TemporalDate{6}), true);
	expect(PropertyValue(TemporalDate{5}), VectorPredicateOp::VPO_LE, PropertyValue(TemporalDate{5}), true);
	expect(PropertyValue(TemporalDate{5}), VectorPredicateOp::VPO_GE, PropertyValue(TemporalDate{5}), true);

	auto range = predicate("value", VectorPredicateOp::VPO_RANGE_CLOSED, PropertyValue(TemporalDate{3}));
	range.upperValue = PropertyValue(TemporalDate{7});
	PropertyPredicateKernel rangeKernel({range});
	EXPECT_TRUE(rangeKernel.matchesMap({{"value", PropertyValue(TemporalDate{5})}}));

	std::vector<PropertyValue> left{PropertyValue(int64_t{1})};
	std::vector<PropertyValue> right{PropertyValue(int64_t{2})};
	expect(PropertyValue(left), VectorPredicateOp::VPO_NE, PropertyValue(right), true);
	expect(PropertyValue(left), VectorPredicateOp::VPO_LT, PropertyValue(right), true);
}
} // namespace
