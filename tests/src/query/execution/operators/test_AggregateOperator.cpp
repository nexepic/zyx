/**
 * @file test_AggregateOperator.cpp
 * @author ZYX Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
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
#include <memory>
#include <optional>
#include <vector>

#include "graph/query/execution/operators/AggregateOperator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"

using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;
using namespace graph::query::expressions;

// Mock Operator for testing
class AggMockOperator : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t current_index = 0;

	explicit AggMockOperator(std::vector<RecordBatch> data = {}) : batches(std::move(data)) {}

	void open() override { current_index = 0; }
	std::optional<RecordBatch> next() override {
		if (current_index >= batches.size()) return std::nullopt;
		return batches[current_index++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
	[[nodiscard]] std::string toString() const override { return "AggMock"; }
};

class AggregateOperatorTest : public ::testing::Test {
protected:
	void SetUp() override {}
};

// ============================================================================
// Global aggregation (no GROUP BY)
// ============================================================================

TEST_F(AggregateOperatorTest, CountStar_GlobalAggregation) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(int64_t(1)));
	r2.setValue("x", PropertyValue(int64_t(2)));
	r3.setValue("x", PropertyValue(int64_t(3)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2, r3}});

	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	auto val = (*batch)[0].getValue("cnt");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(std::get<int64_t>(val->getVariant()), 3);

	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());

	op->close();
}

TEST_F(AggregateOperatorTest, CountExpr_SkipsNull) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(int64_t(10)));
	r2.setValue("x", PropertyValue()); // NULL
	r3.setValue("x", PropertyValue(int64_t(30)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2, r3}});

	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, expr, "cnt");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	auto val = (*batch)[0].getValue("cnt");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(std::get<int64_t>(val->getVariant()), 2);

	op->close();
}

TEST_F(AggregateOperatorTest, Sum_IntegerValues) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(int64_t(10)));
	r2.setValue("x", PropertyValue(int64_t(20)));
	r3.setValue("x", PropertyValue(int64_t(30)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2, r3}});

	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_SUM, expr, "total");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	auto val = (*batch)[0].getValue("total");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(std::get<int64_t>(val->getVariant()), 60);

	op->close();
}

TEST_F(AggregateOperatorTest, Avg_DoubleValues) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(10.0));
	r2.setValue("x", PropertyValue(20.0));
	r3.setValue("x", PropertyValue(30.0));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2, r3}});

	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_AVG, expr, "average");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	auto val = (*batch)[0].getValue("average");
	ASSERT_TRUE(val.has_value());
	EXPECT_DOUBLE_EQ(std::get<double>(val->getVariant()), 20.0);

	op->close();
}

TEST_F(AggregateOperatorTest, Min_IntegerValues) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(int64_t(30)));
	r2.setValue("x", PropertyValue(int64_t(10)));
	r3.setValue("x", PropertyValue(int64_t(20)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2, r3}});

	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_MIN, expr, "minimum");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	auto val = (*batch)[0].getValue("minimum");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(std::get<int64_t>(val->getVariant()), 10);

	op->close();
}

TEST_F(AggregateOperatorTest, Max_IntegerValues) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(int64_t(30)));
	r2.setValue("x", PropertyValue(int64_t(10)));
	r3.setValue("x", PropertyValue(int64_t(20)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2, r3}});

	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_MAX, expr, "maximum");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	auto val = (*batch)[0].getValue("maximum");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(std::get<int64_t>(val->getVariant()), 30);

	op->close();
}

TEST_F(AggregateOperatorTest, Collect_Values) {
	Record r1, r2;
	r1.setValue("x", PropertyValue(int64_t(1)));
	r2.setValue("x", PropertyValue(int64_t(2)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2}});

	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COLLECT, expr, "collected");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	auto val = (*batch)[0].getValue("collected");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(val->getType(), PropertyType::LIST);
	auto list = std::get<std::vector<PropertyValue>>(val->getVariant());
	ASSERT_EQ(list.size(), 2u);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 2);

	op->close();
}

// ============================================================================
// Grouped aggregation (GROUP BY)
// ============================================================================

TEST_F(AggregateOperatorTest, GroupedCount_ByValue) {
	Record r1, r2, r3, r4;
	r1.setValue("group", PropertyValue(std::string("A")));
	r1.setValue("x", PropertyValue(int64_t(1)));
	r2.setValue("group", PropertyValue(std::string("A")));
	r2.setValue("x", PropertyValue(int64_t(2)));
	r3.setValue("group", PropertyValue(std::string("B")));
	r3.setValue("x", PropertyValue(int64_t(3)));
	r4.setValue("group", PropertyValue(std::string("B")));
	r4.setValue("x", PropertyValue(int64_t(4)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2, r3, r4}});

	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");

	std::vector<GroupByItem> groupBy;
	groupBy.emplace_back(std::make_shared<graph::query::expressions::VariableReferenceExpression>("group"), "group");
	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs), std::move(groupBy));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);

	// Both groups should have count of 2
	for (const auto& record : *batch) {
		auto val = record.getValue("cnt");
		ASSERT_TRUE(val.has_value());
		EXPECT_EQ(std::get<int64_t>(val->getVariant()), 2);
	}

	op->close();
}

TEST_F(AggregateOperatorTest, GroupedCount_ByNode) {
	Record r1, r2;
	Node node1(1, 1);
	Node node2(2, 1);
	r1.setNode("n", node1);
	r1.setValue("x", PropertyValue(int64_t(10)));
	r2.setNode("n", node2);
	r2.setValue("x", PropertyValue(int64_t(20)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2}});

	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");

	std::vector<GroupByItem> groupBy;
	groupBy.emplace_back(std::make_shared<VariableReferenceExpression>("n"), "n");
	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs), std::move(groupBy));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);

	op->close();
}

TEST_F(AggregateOperatorTest, GroupedCount_ByEdge) {
	Record r1, r2;
	Edge edge1(1, 10, 20, 1);
	Edge edge2(2, 30, 40, 1);
	r1.setEdge("r", edge1);
	r1.setValue("x", PropertyValue(int64_t(10)));
	r2.setEdge("r", edge2);
	r2.setValue("x", PropertyValue(int64_t(20)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2}});

	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");

	std::vector<GroupByItem> groupBy;
	groupBy.emplace_back(std::make_shared<VariableReferenceExpression>("r"), "r");
	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs), std::move(groupBy));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);

	op->close();
}

// ============================================================================
// Multiple aggregates
// ============================================================================

TEST_F(AggregateOperatorTest, MultipleAggregates) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(int64_t(10)));
	r2.setValue("x", PropertyValue(int64_t(20)));
	r3.setValue("x", PropertyValue(int64_t(30)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2, r3}});

	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");
	aggs.emplace_back(AggregateFunctionType::AGG_SUM, expr, "total");
	aggs.emplace_back(AggregateFunctionType::AGG_AVG, expr, "avg");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	auto cnt = (*batch)[0].getValue("cnt");
	ASSERT_TRUE(cnt.has_value());
	EXPECT_EQ(std::get<int64_t>(cnt->getVariant()), 3);

	auto total = (*batch)[0].getValue("total");
	ASSERT_TRUE(total.has_value());
	EXPECT_EQ(std::get<int64_t>(total->getVariant()), 60);

	auto avg = (*batch)[0].getValue("avg");
	ASSERT_TRUE(avg.has_value());
	EXPECT_DOUBLE_EQ(std::get<double>(avg->getVariant()), 20.0);

	op->close();
}

// ============================================================================
// toString tests
// ============================================================================

TEST_F(AggregateOperatorTest, ToString_CountStar) {
	auto mock = std::make_unique<AggMockOperator>();
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	std::string str = op->toString();
	EXPECT_TRUE(str.find("count(") != std::string::npos);
}

TEST_F(AggregateOperatorTest, ToString_SumWithAlias) {
	auto mock = std::make_unique<AggMockOperator>();
	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_SUM, expr, "total");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	std::string str = op->toString();
	EXPECT_TRUE(str.find("sum(") != std::string::npos);
	EXPECT_TRUE(str.find("AS total") != std::string::npos);
}

TEST_F(AggregateOperatorTest, ToString_MultipleAggregates) {
	auto mock = std::make_unique<AggMockOperator>();
	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_AVG, expr, "avg_x");
	aggs.emplace_back(AggregateFunctionType::AGG_MIN, expr, "min_x");
	aggs.emplace_back(AggregateFunctionType::AGG_MAX, expr, "max_x");
	aggs.emplace_back(AggregateFunctionType::AGG_COLLECT, expr, "all_x");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	std::string str = op->toString();
	EXPECT_TRUE(str.find("avg(") != std::string::npos);
	EXPECT_TRUE(str.find("min(") != std::string::npos);
	EXPECT_TRUE(str.find("max(") != std::string::npos);
	EXPECT_TRUE(str.find("collect(") != std::string::npos);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_F(AggregateOperatorTest, EmptyInput_GlobalAggregation) {
	auto mock = std::make_unique<AggMockOperator>();
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	auto val = (*batch)[0].getValue("cnt");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(std::get<int64_t>(val->getVariant()), 0);

	op->close();
}

TEST_F(AggregateOperatorTest, EmptyInput_GroupedAggregation) {
	auto mock = std::make_unique<AggMockOperator>();
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");
	std::vector<GroupByItem> groupBy;
	groupBy.emplace_back(std::make_shared<VariableReferenceExpression>("g"), "g");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs), std::move(groupBy));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_TRUE(batch->empty());

	op->close();
}

TEST_F(AggregateOperatorTest, Sum_WithNullValues) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(int64_t(10)));
	r2.setValue("x", PropertyValue()); // NULL
	r3.setValue("x", PropertyValue(int64_t(30)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2, r3}});

	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_SUM, expr, "total");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	auto val = (*batch)[0].getValue("total");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(std::get<int64_t>(val->getVariant()), 40);

	op->close();
}

TEST_F(AggregateOperatorTest, Sum_MixedIntDouble) {
	Record r1, r2;
	r1.setValue("x", PropertyValue(int64_t(10)));
	r2.setValue("x", PropertyValue(20.5));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2}});

	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_SUM, expr, "total");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	auto val = (*batch)[0].getValue("total");
	ASSERT_TRUE(val.has_value());
	EXPECT_DOUBLE_EQ(std::get<double>(val->getVariant()), 30.5);

	op->close();
}

TEST_F(AggregateOperatorTest, Avg_AllNulls) {
	Record r1, r2;
	r1.setValue("x", PropertyValue());
	r2.setValue("x", PropertyValue());

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2}});

	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_AVG, expr, "avg");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	auto val = (*batch)[0].getValue("avg");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(val->getType(), PropertyType::NULL_TYPE);

	op->close();
}

TEST_F(AggregateOperatorTest, Sum_AllNulls) {
	Record r1, r2;
	r1.setValue("x", PropertyValue());
	r2.setValue("x", PropertyValue());

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2}});

	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_SUM, expr, "total");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	auto val = (*batch)[0].getValue("total");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(val->getType(), PropertyType::NULL_TYPE);

	op->close();
}

TEST_F(AggregateOperatorTest, Min_AllNulls) {
	Record r1, r2;
	r1.setValue("x", PropertyValue());
	r2.setValue("x", PropertyValue());

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2}});

	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_MIN, expr, "min");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	auto val = (*batch)[0].getValue("min");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(val->getType(), PropertyType::NULL_TYPE);

	op->close();
}

TEST_F(AggregateOperatorTest, GetOutputVariables) {
	auto mock = std::make_unique<AggMockOperator>();
	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");
	aggs.emplace_back(AggregateFunctionType::AGG_SUM, expr, "total");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	auto vars = op->getOutputVariables();
	ASSERT_EQ(vars.size(), 2UL);
	EXPECT_EQ(vars[0], "cnt");
	EXPECT_EQ(vars[1], "total");
}

TEST_F(AggregateOperatorTest, GetChildren) {
	auto mock = std::make_unique<AggMockOperator>();
	auto mockPtr = mock.get();
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	auto children = op->getChildren();
	ASSERT_EQ(children.size(), 1UL);
	EXPECT_EQ(children[0], mockPtr);
}

TEST_F(AggregateOperatorTest, MultipleBatches) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(int64_t(10)));
	r2.setValue("x", PropertyValue(int64_t(20)));
	r3.setValue("x", PropertyValue(int64_t(30)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1}, {r2, r3}});

	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	auto val = (*batch)[0].getValue("cnt");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(std::get<int64_t>(val->getVariant()), 3);

	op->close();
}

// ============================================================================
// Accumulator tests (direct)
// ============================================================================

TEST_F(AggregateOperatorTest, CreateAccumulator_AllTypes) {
	auto count = createAccumulator(AggregateFunctionType::AGG_COUNT);
	ASSERT_NE(count, nullptr);

	auto sum = createAccumulator(AggregateFunctionType::AGG_SUM);
	ASSERT_NE(sum, nullptr);

	auto avg = createAccumulator(AggregateFunctionType::AGG_AVG);
	ASSERT_NE(avg, nullptr);

	auto min = createAccumulator(AggregateFunctionType::AGG_MIN);
	ASSERT_NE(min, nullptr);

	auto max = createAccumulator(AggregateFunctionType::AGG_MAX);
	ASSERT_NE(max, nullptr);

	auto collect = createAccumulator(AggregateFunctionType::AGG_COLLECT);
	ASSERT_NE(collect, nullptr);
}

TEST_F(AggregateOperatorTest, CountAccumulator_Clone) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COUNT);
	acc->update(PropertyValue(int64_t(1)));
	acc->update(PropertyValue(int64_t(2)));

	auto clone = acc->clone();
	EXPECT_EQ(std::get<int64_t>(clone->getResult().getVariant()), 2);
}

TEST_F(AggregateOperatorTest, CountAccumulator_Reset) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COUNT);
	acc->update(PropertyValue(int64_t(1)));
	acc->reset();
	EXPECT_EQ(std::get<int64_t>(acc->getResult().getVariant()), 0);
}

TEST_F(AggregateOperatorTest, SumAccumulator_Clone) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_SUM);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(20)));

	auto clone = acc->clone();
	EXPECT_EQ(std::get<int64_t>(clone->getResult().getVariant()), 30);
}

TEST_F(AggregateOperatorTest, SumAccumulator_Reset) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_SUM);
	acc->update(PropertyValue(int64_t(10)));
	acc->reset();
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(AggregateOperatorTest, AvgAccumulator_Clone) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_AVG);
	acc->update(PropertyValue(10.0));
	acc->update(PropertyValue(20.0));

	auto clone = acc->clone();
	EXPECT_DOUBLE_EQ(std::get<double>(clone->getResult().getVariant()), 15.0);
}

TEST_F(AggregateOperatorTest, AvgAccumulator_Reset) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_AVG);
	acc->update(PropertyValue(10.0));
	acc->reset();
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(AggregateOperatorTest, MinAccumulator_Clone) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MIN);
	acc->update(PropertyValue(int64_t(30)));
	acc->update(PropertyValue(int64_t(10)));

	auto clone = acc->clone();
	EXPECT_EQ(std::get<int64_t>(clone->getResult().getVariant()), 10);
}

TEST_F(AggregateOperatorTest, MinAccumulator_Reset) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MIN);
	acc->update(PropertyValue(int64_t(10)));
	acc->reset();
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(AggregateOperatorTest, MaxAccumulator_Clone) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MAX);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(30)));

	auto clone = acc->clone();
	EXPECT_EQ(std::get<int64_t>(clone->getResult().getVariant()), 30);
}

TEST_F(AggregateOperatorTest, MaxAccumulator_Reset) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_MAX);
	acc->update(PropertyValue(int64_t(10)));
	acc->reset();
	EXPECT_EQ(acc->getResult().getType(), PropertyType::NULL_TYPE);
}

TEST_F(AggregateOperatorTest, CollectAccumulator_Clone) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT);
	acc->update(PropertyValue(int64_t(1)));
	acc->update(PropertyValue(int64_t(2)));

	auto clone = acc->clone();
	auto list = std::get<std::vector<PropertyValue>>(clone->getResult().getVariant());
	ASSERT_EQ(list.size(), 2u);
	EXPECT_EQ(std::get<int64_t>(list[0].getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(list[1].getVariant()), 2);
}

TEST_F(AggregateOperatorTest, CollectAccumulator_Reset) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_COLLECT);
	acc->update(PropertyValue(int64_t(1)));
	acc->reset();
	auto list = std::get<std::vector<PropertyValue>>(acc->getResult().getVariant());
	EXPECT_TRUE(list.empty());
}

TEST_F(AggregateOperatorTest, Avg_IntegerValues) {
	auto acc = createAccumulator(AggregateFunctionType::AGG_AVG);
	acc->update(PropertyValue(int64_t(10)));
	acc->update(PropertyValue(int64_t(20)));
	EXPECT_DOUBLE_EQ(std::get<double>(acc->getResult().getVariant()), 15.0);
}

// ============================================================================
// Branch coverage: null child (open/close with nullptr child_)
// ============================================================================

TEST_F(AggregateOperatorTest, OpenClose_NullChild) {
	// Covers False branch of if (child_) in open() and close()
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");

	auto op = std::make_unique<AggregateOperator>(nullptr, std::move(aggs));
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}

// ============================================================================
// Branch coverage: toString alias == expression->toString()
// ============================================================================

TEST_F(AggregateOperatorTest, ToString_AliasMatchesExpression) {
	// Covers False branch of agg.alias != agg.expression->toString() (line 118)
	auto mock = std::make_unique<AggMockOperator>();
	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	// Set alias to exactly match expression->toString() so the "AS" part is skipped
	aggs.emplace_back(AggregateFunctionType::AGG_SUM, expr, "x");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	std::string str = op->toString();
	EXPECT_TRUE(str.find("sum(") != std::string::npos);
	// Should NOT have "AS x" since alias equals expression
	EXPECT_EQ(str.find("AS x"), std::string::npos);
}

TEST_F(AggregateOperatorTest, ToString_EmptyAlias) {
	// Covers False branch of !agg.alias.empty() (line 118)
	auto mock = std::make_unique<AggMockOperator>();
	auto expr = std::make_shared<VariableReferenceExpression>("x");
	std::vector<AggregateItem> aggs;
	// Empty alias
	aggs.emplace_back(AggregateFunctionType::AGG_SUM, expr, "");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	std::string str = op->toString();
	EXPECT_TRUE(str.find("sum(") != std::string::npos);
	// Should NOT have "AS" since alias is empty
	EXPECT_EQ(str.find("AS"), std::string::npos);
}

// ============================================================================
// Branch coverage: non-COUNT aggregate with null expression (line 146)
// ============================================================================

TEST_F(AggregateOperatorTest, NonCountAggregate_NullExpression) {
	// Covers False branch of if (agg.expression) for non-COUNT (line 146)
	Record r1;
	r1.setValue("x", PropertyValue(int64_t(10)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1}});

	std::vector<AggregateItem> aggs;
	// SUM with null expression - should skip the update
	aggs.emplace_back(AggregateFunctionType::AGG_SUM, nullptr, "total");

	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs));
	op->open();

	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	// Result should be NULL since expression was never evaluated
	auto val = (*batch)[0].getValue("total");
	ASSERT_TRUE(val.has_value());
	EXPECT_EQ(val->getType(), PropertyType::NULL_TYPE);

	op->close();
}

// ============================================================================
// Branch coverage: group by with missing variable (line 168)
// ============================================================================

TEST_F(AggregateOperatorTest, GroupBy_MissingVariable) {
	// When groupBy key references an undefined variable, expression evaluator throws
	Record r1, r2;
	r1.setValue("x", PropertyValue(int64_t(10)));
	r2.setValue("x", PropertyValue(int64_t(20)));

	auto mock = std::make_unique<AggMockOperator>(std::vector<RecordBatch>{{r1, r2}});

	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");

	// Group by a variable that doesn't exist in records
	std::vector<GroupByItem> groupBy;
	groupBy.emplace_back(std::make_shared<VariableReferenceExpression>("missing_var"), "missing_var");
	auto op = std::make_unique<AggregateOperator>(std::move(mock), std::move(aggs), std::move(groupBy));
	op->open();

	EXPECT_THROW(op->next(), std::runtime_error);

	op->close();
}
