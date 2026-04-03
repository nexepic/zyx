/**
 * @file test_ProjectOperator_Parallel.cpp
 * @brief Parallel-path coverage tests for ProjectOperator.
 */

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/execution/operators/ProjectOperator.hpp"
#include "graph/query/expressions/Expression.hpp"

using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;
using graph::query::expressions::LiteralExpression;

class ProjectMockChild final : public PhysicalOperator {
public:
	explicit ProjectMockChild(RecordBatch batch) : batch_(std::move(batch)) {}

	void open() override { emitted_ = false; }
	std::optional<RecordBatch> next() override {
		if (emitted_) {
			return std::nullopt;
		}
		emitted_ = true;
		return batch_;
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
	[[nodiscard]] std::string toString() const override { return "ProjectMockChild"; }

private:
	RecordBatch batch_;
	bool emitted_ = false;
};

TEST(ProjectOperatorParallelTest, UsesParallelProjectionForLargeBatch) {
	constexpr size_t kRows = 5000; // > PARALLEL_PROJECT_THRESHOLD (4096)
	RecordBatch input;
	input.reserve(kRows);
	for (size_t i = 0; i < kRows; ++i) {
		Record r;
		r.setValue("x", PropertyValue(static_cast<int64_t>(i)));
		input.push_back(std::move(r));
	}

	auto child = std::make_unique<ProjectMockChild>(std::move(input));

	std::vector<ProjectItem> items;
	items.emplace_back(std::make_shared<LiteralExpression>(int64_t(1)), "one");

	ProjectOperator op(std::move(child), std::move(items), false, nullptr);
	graph::concurrent::ThreadPool pool(4);
	op.setThreadPool(&pool);

	op.open();
	auto out = op.next();
	ASSERT_TRUE(out.has_value());
	ASSERT_EQ(out->size(), kRows);

	for (size_t i = 0; i < 8; ++i) {
		const auto v = (*out)[i].getValue("one");
		ASSERT_TRUE(v.has_value());
		ASSERT_TRUE(std::holds_alternative<int64_t>(v->getVariant()));
		EXPECT_EQ(std::get<int64_t>(v->getVariant()), 1);
	}

	EXPECT_FALSE(op.next().has_value());
	op.close();
}
