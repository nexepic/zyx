/**
 * @file test_AggregateOperator_Branches.cpp
 * @brief Focused branch tests for AggregateOperator.
 */

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <vector>

#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/execution/operators/AggregateOperator.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;
using namespace graph::query::expressions;

class AggregateBranchMockOperator : public PhysicalOperator {
public:
	explicit AggregateBranchMockOperator(std::vector<RecordBatch> batches) : batches_(std::move(batches)) {}

	void open() override { idx_ = 0; }
	std::optional<RecordBatch> next() override {
		if (idx_ >= batches_.size()) {
			return std::nullopt;
		}
		return batches_[idx_++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
	[[nodiscard]] std::string toString() const override { return "AggregateBranchMock"; }

private:
	std::vector<RecordBatch> batches_;
	size_t idx_ = 0;
};

TEST(AggregateOperatorBranchesTest, UsesQueryContextParametersForCountAndSumExpressions) {
	Record r1;
	Record r2;
	auto child = std::make_unique<AggregateBranchMockOperator>(std::vector<RecordBatch>{{r1, r2}});

	auto param = std::make_shared<ParameterExpression>("p");
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, param, "cnt");
	aggs.emplace_back(AggregateFunctionType::AGG_SUM, param, "sum");

	AggregateOperator op(std::move(child), std::move(aggs));

	QueryContext ctx;
	ctx.parameters.emplace("p", PropertyValue(int64_t(7)));
	op.setQueryContext(&ctx);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	auto cnt = (*batch)[0].getValue("cnt");
	auto sum = (*batch)[0].getValue("sum");
	ASSERT_TRUE(cnt.has_value());
	ASSERT_TRUE(sum.has_value());
	EXPECT_EQ(std::get<int64_t>(cnt->getVariant()), 2);
	EXPECT_EQ(std::get<int64_t>(sum->getVariant()), 14);
	op.close();
}

TEST(AggregateOperatorBranchesTest, GroupByNullExpressionUsesNullKeyPath) {
	Record r1;
	r1.setValue("x", PropertyValue(int64_t(1)));
	Record r2;
	r2.setValue("x", PropertyValue(int64_t(2)));
	auto child = std::make_unique<AggregateBranchMockOperator>(std::vector<RecordBatch>{{r1, r2}});

	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");
	std::vector<GroupByItem> groupBy;
	groupBy.emplace_back(nullptr, "g");

	AggregateOperator op(std::move(child), std::move(aggs), std::move(groupBy));
	op.open();

	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	auto g = (*batch)[0].getValue("g");
	ASSERT_TRUE(g.has_value());
	EXPECT_EQ(g->getType(), PropertyType::NULL_TYPE);

	auto cnt = (*batch)[0].getValue("cnt");
	ASSERT_TRUE(cnt.has_value());
	EXPECT_EQ(std::get<int64_t>(cnt->getVariant()), 2);
	op.close();
}

TEST(AggregateOperatorBranchesTest, ToStringCoversCountCase) {
	auto child = std::make_unique<AggregateBranchMockOperator>(std::vector<RecordBatch>{});
	std::vector<AggregateItem> aggs;
	aggs.emplace_back(AggregateFunctionType::AGG_COUNT, nullptr, "cnt");

	AggregateOperator op(std::move(child), std::move(aggs));
	const auto text = op.toString();
	EXPECT_NE(text.find("count("), std::string::npos);
}
