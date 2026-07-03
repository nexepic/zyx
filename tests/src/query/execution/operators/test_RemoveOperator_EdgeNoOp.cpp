/**
 * @file test_RemoveOperator_EdgeNoOp.cpp
 * @brief Tests REMOVE behavior for edge actions that do not mutate storage.
 */

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <vector>

#include "graph/core/Edge.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/execution/operators/RemoveOperator.hpp"

using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

namespace {

class RemoveEdgeNoOpChild final : public PhysicalOperator {
public:
	explicit RemoveEdgeNoOpChild(RecordBatch batch) : batch_(std::move(batch)) {}

	void open() override { emitted_ = false; }

	std::optional<RecordBatch> next() override {
		if (emitted_) {
			return std::nullopt;
		}
		emitted_ = true;
		return batch_;
	}

	void close() override { closed_ = true; }

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"r"}; }
	[[nodiscard]] std::string toString() const override { return "RemoveEdgeNoOpChild"; }

	bool closed() const { return closed_; }

private:
	RecordBatch batch_;
	bool emitted_ = false;
	bool closed_ = false;
};

} // namespace

TEST(RemoveOperatorEdgeNoOpTest, LabelActionOnEdgePreservesRecordWithoutStorageAccess) {
	Record record;
	record.setEdge("r", Edge(7, 1, 2, 3));

	auto child = std::make_unique<RemoveEdgeNoOpChild>(RecordBatch{record});
	auto *childPtr = child.get();
	RemoveOperator op(
			nullptr,
			std::move(child),
			std::vector<RemoveItem>{{RemoveActionType::LABEL, "r", "IgnoredOnRelationships"}});

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	auto edge = (*batch)[0].getEdge("r");
	ASSERT_TRUE(edge.has_value());
	EXPECT_EQ(edge->getId(), 7);
	EXPECT_FALSE(op.next().has_value());
	op.close();
	EXPECT_TRUE(childPtr->closed());
}
