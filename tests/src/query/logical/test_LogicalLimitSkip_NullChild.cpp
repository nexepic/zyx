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

TEST(LogicalLimitNullChildTest, OutputVariablesAndCloneHandleNullChild) {
	LogicalLimit limit(nullptr, 10);

	EXPECT_TRUE(limit.getOutputVariables().empty());

	auto cloned = limit.clone();
	auto *clonedLimit = dynamic_cast<LogicalLimit *>(cloned.get());
	ASSERT_NE(clonedLimit, nullptr);
	ASSERT_EQ(clonedLimit->getChildren().size(), 1UL);
	EXPECT_EQ(clonedLimit->getChildren()[0], nullptr);
}

TEST(LogicalLimitNullChildTest, OutputVariablesFromChildAndOutOfRangeGuards) {
	LogicalLimit limit(std::make_unique<LogicalNodeScan>("n"), 3);

	EXPECT_EQ(limit.getType(), LogicalOpType::LOP_LIMIT);
	EXPECT_EQ(limit.getLimit(), 3);
	EXPECT_EQ(limit.toString(), "Limit(3)");
	ASSERT_EQ(limit.getChildren().size(), 1UL);
	ASSERT_NE(limit.getChildren().front(), nullptr);
	const auto cloned = limit.clone();
	const auto *clonedLimit = dynamic_cast<const LogicalLimit *>(cloned.get());
	ASSERT_NE(clonedLimit, nullptr);
	ASSERT_NE(clonedLimit->getChildren().front(), nullptr);
	auto vars = limit.getOutputVariables();
	ASSERT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "n");

	EXPECT_THROW(limit.setChild(1, std::make_unique<LogicalNodeScan>("x")), std::out_of_range);
	EXPECT_THROW(limit.detachChild(1), std::out_of_range);
	const auto detached = limit.detachChild(0);
	ASSERT_NE(detached, nullptr);
	limit.setChild(0, std::make_unique<LogicalNodeScan>("replacement"));
	EXPECT_EQ(limit.getOutputVariables(), (std::vector<std::string>{"replacement"}));
}

TEST(LogicalSkipNullChildTest, OutputVariablesAndCloneHandleNullChild) {
	LogicalSkip skip(nullptr, 5);

	EXPECT_TRUE(skip.getOutputVariables().empty());

	auto cloned = skip.clone();
	auto *clonedSkip = dynamic_cast<LogicalSkip *>(cloned.get());
	ASSERT_NE(clonedSkip, nullptr);
	ASSERT_EQ(clonedSkip->getChildren().size(), 1UL);
	EXPECT_EQ(clonedSkip->getChildren()[0], nullptr);
}

TEST(LogicalSkipNullChildTest, OutputVariablesFromChildAndOutOfRangeGuards) {
	LogicalSkip skip(std::make_unique<LogicalNodeScan>("n"), 2);

	EXPECT_EQ(skip.getType(), LogicalOpType::LOP_SKIP);
	EXPECT_EQ(skip.getOffset(), 2);
	EXPECT_EQ(skip.toString(), "Skip(2)");
	ASSERT_EQ(skip.getChildren().size(), 1UL);
	ASSERT_NE(skip.getChildren().front(), nullptr);
	const auto cloned = skip.clone();
	const auto *clonedSkip = dynamic_cast<const LogicalSkip *>(cloned.get());
	ASSERT_NE(clonedSkip, nullptr);
	ASSERT_NE(clonedSkip->getChildren().front(), nullptr);
	auto vars = skip.getOutputVariables();
	ASSERT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "n");

	EXPECT_THROW(skip.setChild(1, std::make_unique<LogicalNodeScan>("x")), std::out_of_range);
	EXPECT_THROW(skip.detachChild(1), std::out_of_range);
	const auto detached = skip.detachChild(0);
	ASSERT_NE(detached, nullptr);
	skip.setChild(0, std::make_unique<LogicalNodeScan>("replacement"));
	EXPECT_EQ(skip.getOutputVariables(), (std::vector<std::string>{"replacement"}));
}
