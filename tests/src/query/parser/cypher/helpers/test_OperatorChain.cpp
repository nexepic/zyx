/**
 * @file test_OperatorChain.cpp
 * @brief Unit tests for OperatorChain helper class
 */

#include <gtest/gtest.h>
#include "helpers/OperatorChain.hpp"

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
