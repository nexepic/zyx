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

// Covers the default setChild() implementation (no-op virtual method).
// Without this test the branch in PhysicalOperator::setChild is never executed.
TEST(PhysicalOperatorTest, DefaultSetChildIsNoOp) {
	DummyPhysicalOperator op;
	auto child = std::make_unique<DummyPhysicalOperator>();
	// Default implementation does nothing — must not throw or crash.
	EXPECT_NO_THROW(op.setChild(std::move(child)));
	// Children list is still empty because the default impl discards the child.
	EXPECT_TRUE(op.getChildren().empty());
}
