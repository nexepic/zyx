#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "graph/query/execution/operators/SkipOperator.hpp"

using namespace graph::query::execution;
using namespace graph::query::execution::operators;
using graph::PropertyValue;

namespace {

class BatchSourceOperator final : public PhysicalOperator {
public:
	explicit BatchSourceOperator(std::vector<RecordBatch> batches) : batches_(std::move(batches)) {}

	void open() override { index_ = 0; }

	std::optional<RecordBatch> next() override {
		if (index_ >= batches_.size()) {
			return std::nullopt;
		}
		return batches_[index_++];
	}

	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"v"}; }
	[[nodiscard]] std::string toString() const override { return "BatchSource"; }

private:
	std::vector<RecordBatch> batches_;
	size_t index_ = 0;
};

Record makeRecord(int64_t value) {
	Record record;
	record.setValue("v", value);
	return record;
}

} // namespace

TEST(SkipOperatorStreamTest, SkipsWholeBatchesThenPartOfNextBatch) {
	RecordBatch first;
	first.push_back(makeRecord(1));
	first.push_back(makeRecord(2));
	RecordBatch second;
	second.push_back(makeRecord(3));
	second.push_back(makeRecord(4));
	second.push_back(makeRecord(5));

	auto source = std::make_unique<BatchSourceOperator>(
			std::vector<RecordBatch>{std::move(first), std::move(second)});
	SkipOperator op(std::move(source), 3);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2U);
	EXPECT_EQ((*batch)[0].getValue("v").value(), PropertyValue(int64_t{4}));
	EXPECT_EQ((*batch)[1].getValue("v").value(), PropertyValue(int64_t{5}));
	EXPECT_FALSE(op.next().has_value());
	EXPECT_EQ(op.getOutputVariables(), (std::vector<std::string>{"v"}));
	EXPECT_EQ(op.toString(), "Skip(3)");
	op.close();
}

TEST(SkipOperatorStreamTest, ZeroOffsetPassesThroughFirstBatch) {
	RecordBatch first;
	first.push_back(makeRecord(7));

	auto source = std::make_unique<BatchSourceOperator>(
			std::vector<RecordBatch>{std::move(first)});
	SkipOperator op(std::move(source), 0);

	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	EXPECT_EQ((*batch)[0].getValue("v").value(), PropertyValue(int64_t{7}));
	EXPECT_FALSE(op.next().has_value());
	op.close();
}
