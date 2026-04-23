/**
 * @file test_FilterOperator_Expression.cpp
 * @brief Branch coverage tests for FilterOperator — covers expression-based
 *        evaluation, query context parameters, and parallel filter paths.
 **/

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <vector>

#include "graph/query/execution/operators/FilterOperator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/concurrent/ThreadPool.hpp"

using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

// Mock operator that produces configurable batches
class FilterMockOperator : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t idx = 0;

	explicit FilterMockOperator(std::vector<RecordBatch> data = {}) : batches(std::move(data)) {}

	void open() override { idx = 0; }
	std::optional<RecordBatch> next() override {
		if (idx >= batches.size()) return std::nullopt;
		return batches[idx++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
	[[nodiscard]] std::string toString() const override { return "FilterMock"; }
};

class FilterOperatorExpressionTest : public ::testing::Test {};

// ============================================================================
// Expression-based filter (filterExpr_ path in evaluateRecord)
// ============================================================================

TEST_F(FilterOperatorExpressionTest, ExpressionFilter_BasicComparison) {
	// Build a simple expression: x > 5 using VariableReferenceExpression
	// Since we can't easily build a full comparison AST, we use the predicate path
	// but construct the FilterOperator with an expression to hit the filterExpr_ branch
	auto expr = std::make_shared<graph::query::expressions::LiteralExpression>(true);

	Record r1, r2;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(10)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(3)));

	auto mock = new FilterMockOperator({{r1, r2}});

	// Expression that always evaluates to true — both records pass
	auto op = std::make_unique<FilterOperator>(
		std::unique_ptr<PhysicalOperator>(mock),
		expr,
		nullptr, // no DataManager needed for LiteralExpression
		"true");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL);
	op->close();
}

TEST_F(FilterOperatorExpressionTest, ExpressionFilter_AllFiltered) {
	// Expression that always evaluates to false — no records pass
	auto expr = std::make_shared<graph::query::expressions::LiteralExpression>(false);

	Record r1;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(10)));

	auto mock = new FilterMockOperator({{r1}});

	auto op = std::make_unique<FilterOperator>(
		std::unique_ptr<PhysicalOperator>(mock),
		expr, nullptr, "false");

	op->open();
	auto batch = op->next();
	// All filtered → next batch from child is nullopt → returns nullopt
	EXPECT_FALSE(batch.has_value());
	op->close();
}

// ============================================================================
// Expression filter with QueryContext parameters
// ============================================================================

TEST_F(FilterOperatorExpressionTest, ExpressionFilter_WithQueryContext) {
	auto expr = std::make_shared<graph::query::expressions::LiteralExpression>(true);

	Record r1;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(1)));

	auto mock = new FilterMockOperator({{r1}});

	auto op = std::make_unique<FilterOperator>(
		std::unique_ptr<PhysicalOperator>(mock),
		expr, nullptr, "true");

	// Set a QueryContext with parameters to hit the params branch (line 32-33)
	auto ctx = std::make_shared<graph::query::QueryContext>();
	ctx->parameters["$param1"] = PropertyValue(static_cast<int64_t>(42));
	op->setQueryContext(ctx.get());

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);
	op->close();
}

TEST_F(FilterOperatorExpressionTest, ExpressionFilter_WithEmptyQueryContext) {
	// QueryContext exists but parameters map is empty — should skip params
	auto expr = std::make_shared<graph::query::expressions::LiteralExpression>(true);

	Record r1;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(1)));

	auto mock = new FilterMockOperator({{r1}});

	auto op = std::make_unique<FilterOperator>(
		std::unique_ptr<PhysicalOperator>(mock),
		expr, nullptr, "true");

	auto ctx = std::make_shared<graph::query::QueryContext>();
	// Empty parameters map
	op->setQueryContext(ctx.get());

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);
	op->close();
}

// ============================================================================
// Predicate-based filter (fallback path when filterExpr_ is null)
// ============================================================================

TEST_F(FilterOperatorExpressionTest, PredicateFilter_Basic) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(10)));
	r3.setValue("x", PropertyValue(static_cast<int64_t>(5)));

	auto mock = new FilterMockOperator({{r1, r2, r3}});

	auto predicate = [](const Record &r) -> bool {
		auto v = r.getValue("x");
		return v.has_value() && std::get<int64_t>(v->getVariant()) > 3;
	};

	auto op = std::make_unique<FilterOperator>(
		std::unique_ptr<PhysicalOperator>(mock),
		predicate, "x > 3");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 2UL); // r2 and r3 pass
	op->close();
}

// ============================================================================
// Parallel filter path
// ============================================================================

TEST_F(FilterOperatorExpressionTest, ParallelFilter_LargeDataset) {
	constexpr size_t N = 5000; // Above PARALLEL_FILTER_THRESHOLD (4096)
	RecordBatch batch;
	batch.reserve(N);
	for (size_t i = 0; i < N; ++i) {
		Record r;
		r.setValue("x", PropertyValue(static_cast<int64_t>(i)));
		batch.push_back(std::move(r));
	}

	auto mock = new FilterMockOperator({std::move(batch)});

	// Keep only even values
	auto predicate = [](const Record &r) -> bool {
		auto v = r.getValue("x");
		return v.has_value() && (std::get<int64_t>(v->getVariant()) % 2 == 0);
	};

	auto op = std::make_unique<FilterOperator>(
		std::unique_ptr<PhysicalOperator>(mock),
		predicate, "x % 2 == 0");

	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	op->open();
	auto result = op->next();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->size(), N / 2);
	op->close();
}

TEST_F(FilterOperatorExpressionTest, SingleThreadedPool_FallsBackToSequential) {
	constexpr size_t N = 5000;
	RecordBatch batch;
	batch.reserve(N);
	for (size_t i = 0; i < N; ++i) {
		Record r;
		r.setValue("x", PropertyValue(static_cast<int64_t>(i)));
		batch.push_back(std::move(r));
	}

	auto mock = new FilterMockOperator({std::move(batch)});

	auto predicate = [](const Record &r) -> bool {
		auto v = r.getValue("x");
		return v.has_value() && (std::get<int64_t>(v->getVariant()) < 100);
	};

	auto op = std::make_unique<FilterOperator>(
		std::unique_ptr<PhysicalOperator>(mock),
		predicate, "x < 100");

	// Single-threaded pool → falls back to sequential path
	graph::concurrent::ThreadPool pool(1);
	op->setThreadPool(&pool);

	op->open();
	auto result = op->next();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->size(), 100UL);
	op->close();
}

// ============================================================================
// Null child + helper methods
// ============================================================================

TEST_F(FilterOperatorExpressionTest, OpenCloseWithNullChild) {
	auto predicate = [](const Record &) { return true; };
	auto op = std::make_unique<FilterOperator>(nullptr, predicate, "true");
	EXPECT_NO_THROW(op->open());
	EXPECT_NO_THROW(op->close());
}

TEST_F(FilterOperatorExpressionTest, GetOutputVariables_NullChild) {
	auto predicate = [](const Record &) { return true; };
	auto op = std::make_unique<FilterOperator>(nullptr, predicate, "true");
	auto vars = op->getOutputVariables();
	EXPECT_TRUE(vars.empty());
}

TEST_F(FilterOperatorExpressionTest, GetChildren_NullChild) {
	auto predicate = [](const Record &) { return true; };
	auto op = std::make_unique<FilterOperator>(nullptr, predicate, "true");
	auto children = op->getChildren();
	EXPECT_TRUE(children.empty());
}

TEST_F(FilterOperatorExpressionTest, GetChildren_WithChild) {
	Record r;
	r.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	auto mock = new FilterMockOperator({{r}});
	auto predicate = [](const Record &) { return true; };
	auto op = std::make_unique<FilterOperator>(
		std::unique_ptr<PhysicalOperator>(mock), predicate, "true");

	auto children = op->getChildren();
	ASSERT_EQ(children.size(), 1UL);
	EXPECT_EQ(children[0], mock);
}

TEST_F(FilterOperatorExpressionTest, ToString) {
	auto predicate = [](const Record &) { return true; };
	auto op = std::make_unique<FilterOperator>(nullptr, predicate, "x > 5");
	EXPECT_EQ(op->toString(), "Filter(x > 5)");
}

// ============================================================================
// Multiple batches — skipping empty filtered batch to get next one
// ============================================================================

TEST_F(FilterOperatorExpressionTest, MultipleBatches_SkipsEmptyFiltered) {
	Record r1, r2, r3;
	r1.setValue("x", PropertyValue(static_cast<int64_t>(1)));
	r2.setValue("x", PropertyValue(static_cast<int64_t>(2)));
	r3.setValue("x", PropertyValue(static_cast<int64_t>(10)));

	// First batch: all filtered out. Second batch: one passes.
	auto mock = new FilterMockOperator({{r1, r2}, {r3}});

	auto predicate = [](const Record &r) -> bool {
		auto v = r.getValue("x");
		return v.has_value() && std::get<int64_t>(v->getVariant()) > 5;
	};

	auto op = std::make_unique<FilterOperator>(
		std::unique_ptr<PhysicalOperator>(mock), predicate, "x > 5");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL);

	// Next call returns nullopt
	EXPECT_FALSE(op->next().has_value());
	op->close();
}
