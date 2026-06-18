/**
 * @file test_PhysicalOperator.cpp
 * @brief Unit tests for PhysicalOperator inline accessors/defaults.
 **/

#include <gtest/gtest.h>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/operators/LimitOperator.hpp"
#include "graph/query/execution/operators/ProjectOperator.hpp"

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

	class HintAwareOperator final : public PhysicalOperator {
	public:
		void open() override { opened = true; }
		std::optional<RecordBatch> next() override { return std::nullopt; }
		void close() override {}
		void setOutputLimitHint(size_t limit) override {
			receivedHint = limit;
			hintBeforeOpen = !opened;
		}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
		[[nodiscard]] std::string toString() const override { return "HintAware"; }

		size_t receivedHint = 0;
		bool hintBeforeOpen = false;
		bool opened = false;
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

TEST(PhysicalOperatorTest, LimitPropagatesOutputHintBeforeOpen) {
	auto child = std::make_unique<HintAwareOperator>();
	auto *childPtr = child.get();
	graph::query::execution::operators::LimitOperator limit(std::move(child), 7);

	limit.open();

	EXPECT_TRUE(childPtr->opened);
	EXPECT_TRUE(childPtr->hintBeforeOpen);
	EXPECT_EQ(childPtr->receivedHint, 7U);
}

TEST(PhysicalOperatorTest, ProjectPropagatesOutputHintWhenCardinalityPreserving) {
	auto child = std::make_unique<HintAwareOperator>();
	auto *childPtr = child.get();
	graph::query::execution::operators::ProjectOperator project(std::move(child), {}, false);

	project.setOutputLimitHint(11);

	EXPECT_EQ(childPtr->receivedHint, 11U);
	EXPECT_TRUE(childPtr->hintBeforeOpen);
}

TEST(PhysicalOperatorTest, DistinctProjectDoesNotPropagateOutputHint) {
	auto child = std::make_unique<HintAwareOperator>();
	auto *childPtr = child.get();
	graph::query::execution::operators::ProjectOperator project(std::move(child), {}, true);

	project.setOutputLimitHint(11);

	EXPECT_EQ(childPtr->receivedHint, 0U);
	EXPECT_FALSE(childPtr->hintBeforeOpen);
}
