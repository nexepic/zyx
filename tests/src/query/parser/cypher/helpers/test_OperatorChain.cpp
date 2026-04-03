/**
 * @file test_OperatorChain.cpp
 * @brief Unit tests for OperatorChain helper class
 */

#include <gtest/gtest.h>
#include "helpers/OperatorChain.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"

using namespace graph::parser::cypher::helpers;

// Test class for OperatorChain
class OperatorChainUnitTest : public ::testing::Test {
protected:
	// Helper methods
};

// ============== chain Tests ==============

TEST_F(OperatorChainUnitTest, Chain_NullRoot) {
	// Test when rootOp is null - newOp becomes the root
	auto newOp = std::unique_ptr<graph::query::logical::LogicalOperator>(nullptr);
	auto result = OperatorChain::chain(nullptr, std::move(newOp));
	EXPECT_EQ(result, nullptr);
}

TEST_F(OperatorChainUnitTest, Chain_NullNewOp) {
	// Test when newOp is null
	auto rootOp = std::unique_ptr<graph::query::logical::LogicalOperator>(nullptr);
	auto result = OperatorChain::chain(std::move(rootOp), nullptr);
	EXPECT_EQ(result, nullptr);
}

TEST_F(OperatorChainUnitTest, Chain_NonWrappingOperatorReplacesRoot) {
	auto root = std::make_unique<graph::query::logical::LogicalNodeScan>("n");
	auto newOp = std::make_unique<graph::query::logical::LogicalDelete>(
		std::vector<std::string>{"n"}, false);

	auto result = OperatorChain::chain(std::move(root), std::move(newOp));

	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), graph::query::logical::LogicalOpType::LOP_DELETE);
	auto *del = dynamic_cast<graph::query::logical::LogicalDelete *>(result.get());
	ASSERT_NE(del, nullptr);
	EXPECT_TRUE(del->getChildren().empty());
}
