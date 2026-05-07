/**
 * @file test_LogicalTransactionControl.cpp
 * @brief Unit tests for LogicalTransactionControl covering all methods.
 */

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "graph/query/logical/operators/LogicalTransactionControl.hpp"

using namespace graph::query::logical;

class LogicalTxnControlUnitTest : public ::testing::Test {};

TEST_F(LogicalTxnControlUnitTest, BeginToString) {
	LogicalTransactionControl op(LogicalTxnCommand::LTXN_BEGIN);
	EXPECT_EQ(op.toString(), "TransactionControl(BEGIN)");
}

TEST_F(LogicalTxnControlUnitTest, CommitToString) {
	LogicalTransactionControl op(LogicalTxnCommand::LTXN_COMMIT);
	EXPECT_EQ(op.toString(), "TransactionControl(COMMIT)");
}

TEST_F(LogicalTxnControlUnitTest, RollbackToString) {
	LogicalTransactionControl op(LogicalTxnCommand::LTXN_ROLLBACK);
	EXPECT_EQ(op.toString(), "TransactionControl(ROLLBACK)");
}

TEST_F(LogicalTxnControlUnitTest, GetCommand) {
	LogicalTransactionControl begin(LogicalTxnCommand::LTXN_BEGIN);
	EXPECT_EQ(begin.getCommand(), LogicalTxnCommand::LTXN_BEGIN);

	LogicalTransactionControl commit(LogicalTxnCommand::LTXN_COMMIT);
	EXPECT_EQ(commit.getCommand(), LogicalTxnCommand::LTXN_COMMIT);

	LogicalTransactionControl rollback(LogicalTxnCommand::LTXN_ROLLBACK);
	EXPECT_EQ(rollback.getCommand(), LogicalTxnCommand::LTXN_ROLLBACK);
}

TEST_F(LogicalTxnControlUnitTest, Clone) {
	LogicalTransactionControl op(LogicalTxnCommand::LTXN_COMMIT);
	auto cloned = op.clone();
	ASSERT_NE(cloned, nullptr);

	auto *casted = dynamic_cast<LogicalTransactionControl *>(cloned.get());
	ASSERT_NE(casted, nullptr);
	EXPECT_EQ(casted->getCommand(), LogicalTxnCommand::LTXN_COMMIT);
	EXPECT_EQ(casted->toString(), "TransactionControl(COMMIT)");
}

TEST_F(LogicalTxnControlUnitTest, GetChildren) {
	LogicalTransactionControl op(LogicalTxnCommand::LTXN_BEGIN);
	auto children = op.getChildren();
	EXPECT_TRUE(children.empty());
}

TEST_F(LogicalTxnControlUnitTest, GetOutputVariables) {
	LogicalTransactionControl op(LogicalTxnCommand::LTXN_ROLLBACK);
	auto vars = op.getOutputVariables();
	EXPECT_TRUE(vars.empty());
}

TEST_F(LogicalTxnControlUnitTest, GetType) {
	LogicalTransactionControl op(LogicalTxnCommand::LTXN_BEGIN);
	EXPECT_EQ(op.getType(), LogicalOpType::LOP_TRANSACTION_CONTROL);
}

TEST_F(LogicalTxnControlUnitTest, SetChildThrows) {
	LogicalTransactionControl op(LogicalTxnCommand::LTXN_BEGIN);
	EXPECT_THROW(op.setChild(0, nullptr), std::logic_error);
}

TEST_F(LogicalTxnControlUnitTest, DetachChildThrows) {
	LogicalTransactionControl op(LogicalTxnCommand::LTXN_BEGIN);
	EXPECT_THROW(op.detachChild(0), std::logic_error);
}
