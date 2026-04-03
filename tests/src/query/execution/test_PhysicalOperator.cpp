/**
 * @file test_PhysicalOperator.cpp
 * @brief Unit tests for PhysicalOperator inline accessors/defaults.
 **/

#include <gtest/gtest.h>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"

using namespace graph::query::execution;

namespace {
	class DummyPhysicalOperator final : public PhysicalOperator {
	public:
		void open() override {}
		std::optional<RecordBatch> next() override { return std::nullopt; }
		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
		[[nodiscard]] std::string toString() const override { return "Dummy"; }
	};
}

TEST(PhysicalOperatorTest, ThreadPoolAndQueryContextAccessors) {
	DummyPhysicalOperator op;

	graph::concurrent::ThreadPool pool(2);
	op.setThreadPool(&pool);
	EXPECT_EQ(op.getThreadPool(), &pool);

	graph::query::QueryContext ctx;
	ctx.parameters["p"] = graph::PropertyValue(int64_t(1));
	op.setQueryContext(&ctx);
	EXPECT_EQ(op.getQueryContext(), &ctx);
}

TEST(PhysicalOperatorTest, DefaultChildrenIsEmpty) {
	DummyPhysicalOperator op;
	EXPECT_TRUE(op.getChildren().empty());
}
