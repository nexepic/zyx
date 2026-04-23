/**
 * @file test_LogicalLimitSkip_NullChild.cpp
 * @brief Focused branch tests for LogicalLimit and LogicalSkip.
 */

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"

using namespace graph::query::logical;

TEST(LogicalLimitBranchTest, OutputVariablesAndCloneHandleNullChild) {
	LogicalLimit limit(nullptr, 10);

	EXPECT_TRUE(limit.getOutputVariables().empty());

	auto cloned = limit.clone();
	auto *clonedLimit = dynamic_cast<LogicalLimit *>(cloned.get());
	ASSERT_NE(clonedLimit, nullptr);
	ASSERT_EQ(clonedLimit->getChildren().size(), 1UL);
	EXPECT_EQ(clonedLimit->getChildren()[0], nullptr);
}

TEST(LogicalLimitBranchTest, OutputVariablesFromChildAndOutOfRangeGuards) {
	LogicalLimit limit(std::make_unique<LogicalNodeScan>("n"), 3);

	auto vars = limit.getOutputVariables();
	ASSERT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "n");

	EXPECT_THROW(limit.setChild(1, std::make_unique<LogicalNodeScan>("x")), std::out_of_range);
	EXPECT_THROW(limit.detachChild(1), std::out_of_range);
}

TEST(LogicalSkipBranchTest, OutputVariablesAndCloneHandleNullChild) {
	LogicalSkip skip(nullptr, 5);

	EXPECT_TRUE(skip.getOutputVariables().empty());

	auto cloned = skip.clone();
	auto *clonedSkip = dynamic_cast<LogicalSkip *>(cloned.get());
	ASSERT_NE(clonedSkip, nullptr);
	ASSERT_EQ(clonedSkip->getChildren().size(), 1UL);
	EXPECT_EQ(clonedSkip->getChildren()[0], nullptr);
}

TEST(LogicalSkipBranchTest, OutputVariablesFromChildAndOutOfRangeGuards) {
	LogicalSkip skip(std::make_unique<LogicalNodeScan>("n"), 2);

	auto vars = skip.getOutputVariables();
	ASSERT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "n");

	EXPECT_THROW(skip.setChild(1, std::make_unique<LogicalNodeScan>("x")), std::out_of_range);
	EXPECT_THROW(skip.detachChild(1), std::out_of_range);
}

