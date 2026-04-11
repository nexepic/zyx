/**
 * @file test_OperatorChain.cpp
 * @brief Unit tests for OperatorChain helper class
 */

#include <gtest/gtest.h>
#include "helpers/OperatorChain.hpp"
#include "graph/query/logical/operators/LogicalCreateEdge.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"

using namespace graph::parser::cypher::helpers;
using namespace graph::query::logical;

// Test class for OperatorChain
class OperatorChainUnitTest : public ::testing::Test {
protected:
	// Helper methods
};

// ============== chain Tests ==============

TEST_F(OperatorChainUnitTest, Chain_NullRoot) {
	// Test when rootOp is null - newOp becomes the root
	auto newOp = std::unique_ptr<LogicalOperator>(nullptr);
	auto result = OperatorChain::chain(nullptr, std::move(newOp));
	EXPECT_EQ(result, nullptr);
}

TEST_F(OperatorChainUnitTest, Chain_NullRootWithValidNewOp) {
	auto newOp = std::make_unique<LogicalNodeScan>("n");
	auto* raw = newOp.get();
	auto result = OperatorChain::chain(nullptr, std::move(newOp));
	EXPECT_EQ(result.get(), raw);
}

TEST_F(OperatorChainUnitTest, Chain_NullNewOp) {
	// Test when newOp is null
	auto rootOp = std::unique_ptr<LogicalOperator>(nullptr);
	auto result = OperatorChain::chain(std::move(rootOp), nullptr);
	EXPECT_EQ(result, nullptr);
}

TEST_F(OperatorChainUnitTest, Chain_CreateEdgeWrapsRoot) {
	auto root = std::make_unique<LogicalNodeScan>("n");
	auto edgeOp = std::make_unique<LogicalCreateEdge>(
		"r", "KNOWS",
		std::unordered_map<std::string, graph::PropertyValue>{},
		"n", "m");

	auto result = OperatorChain::chain(std::move(root), std::move(edgeOp));

	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_CREATE_EDGE);
	auto* edge = dynamic_cast<LogicalCreateEdge*>(result.get());
	ASSERT_NE(edge, nullptr);
	ASSERT_FALSE(edge->getChildren().empty());
	EXPECT_EQ(edge->getChildren()[0]->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST_F(OperatorChainUnitTest, Chain_CreateNodeWrapsRoot) {
	auto root = std::make_unique<LogicalNodeScan>("n");
	auto nodeOp = std::make_unique<LogicalCreateNode>(
		"m", std::vector<std::string>{"Person"},
		std::unordered_map<std::string, graph::PropertyValue>{},
		std::unordered_map<std::string, std::shared_ptr<graph::query::expressions::Expression>>{});

	auto result = OperatorChain::chain(std::move(root), std::move(nodeOp));

	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_CREATE_NODE);
	auto* node = dynamic_cast<LogicalCreateNode*>(result.get());
	ASSERT_NE(node, nullptr);
	ASSERT_FALSE(node->getChildren().empty());
	EXPECT_EQ(node->getChildren()[0]->getType(), LogicalOpType::LOP_NODE_SCAN);
}

TEST_F(OperatorChainUnitTest, Chain_NonWrappingOperatorReplacesRoot) {
	auto root = std::make_unique<LogicalNodeScan>("n");
	auto newOp = std::make_unique<LogicalDelete>(
		std::vector<std::string>{"n"}, false);

	auto result = OperatorChain::chain(std::move(root), std::move(newOp));

	ASSERT_NE(result, nullptr);
	EXPECT_EQ(result->getType(), LogicalOpType::LOP_DELETE);
	auto *del = dynamic_cast<LogicalDelete*>(result.get());
	ASSERT_NE(del, nullptr);
	EXPECT_TRUE(del->getChildren().empty());
}
