/**
 * @file test_TransactionControlOperator.cpp
 * @brief Unit tests for TransactionControlOperator.
 */

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "graph/query/execution/operators/TransactionControlOperator.hpp"

using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

namespace {

std::string expectedMessage(TransactionCommand cmd) {
	switch (cmd) {
		case TransactionCommand::TXN_CTL_BEGIN:
			return "Transaction started";
		case TransactionCommand::TXN_CTL_COMMIT:
			return "Transaction committed";
		case TransactionCommand::TXN_CTL_ROLLBACK:
			return "Transaction rolled back";
	}
	return "";
}

std::string expectedToString(TransactionCommand cmd) {
	switch (cmd) {
		case TransactionCommand::TXN_CTL_BEGIN:
			return "TransactionControl(BEGIN)";
		case TransactionCommand::TXN_CTL_COMMIT:
			return "TransactionControl(COMMIT)";
		case TransactionCommand::TXN_CTL_ROLLBACK:
			return "TransactionControl(ROLLBACK)";
	}
	return "";
}

} // namespace

class TransactionControlOperatorTest : public ::testing::TestWithParam<TransactionCommand> {};

TEST_P(TransactionControlOperatorTest, EmitsOneStatusRecordThenExhausts) {
	const auto cmd = GetParam();
	TransactionControlOperator op(cmd);

	op.open();

	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	auto result = (*batch)[0].getValue("result");
	ASSERT_TRUE(result.has_value());
	auto strPtr = std::get_if<std::string>(&result->getVariant());
	ASSERT_NE(strPtr, nullptr);
	EXPECT_EQ(*strPtr, expectedMessage(cmd));

	EXPECT_FALSE(op.next().has_value());
	op.close();
}

TEST_P(TransactionControlOperatorTest, AccessorsAndMetadata) {
	const auto cmd = GetParam();
	TransactionControlOperator op(cmd);

	EXPECT_EQ(op.getCommand(), cmd);
	EXPECT_EQ(op.toString(), expectedToString(cmd));
	EXPECT_EQ(op.getOutputVariables(), std::vector<std::string>({"result"}));
}

TEST(TransactionControlOperatorStandaloneTest, OpenResetsExecutionState) {
	TransactionControlOperator op(TransactionCommand::TXN_CTL_BEGIN);

	op.open();
	ASSERT_TRUE(op.next().has_value());
	EXPECT_FALSE(op.next().has_value());

	op.open();
	EXPECT_TRUE(op.next().has_value());
}

INSTANTIATE_TEST_SUITE_P(AllCommands,
	TransactionControlOperatorTest,
	::testing::Values(
		TransactionCommand::TXN_CTL_BEGIN,
		TransactionCommand::TXN_CTL_COMMIT,
		TransactionCommand::TXN_CTL_ROLLBACK));
