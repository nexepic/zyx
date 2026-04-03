/**
 * @file test_LogicalSingleRowAndPlanPrinter.cpp
 * @brief Focused tests for LogicalSingleRow and LogicalPlanPrinter.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/LogicalPlanPrinter.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"

using namespace graph::query::expressions;
using namespace graph::query::logical;

TEST(LogicalSingleRowStandaloneTest, ToStringAndLeafShape) {
	LogicalSingleRow op;

	EXPECT_EQ(op.getType(), LogicalOpType::LOP_SINGLE_ROW);
	EXPECT_EQ(op.toString(), "SingleRow");
	EXPECT_TRUE(op.getChildren().empty());
	EXPECT_TRUE(op.getOutputVariables().empty());
}

TEST(LogicalSingleRowStandaloneTest, RejectsChildMutations) {
	LogicalSingleRow op;

	EXPECT_THROW(op.setChild(0, std::make_unique<LogicalNodeScan>("n")), std::logic_error);
	EXPECT_THROW(op.detachChild(0), std::logic_error);
}

TEST(LogicalPlanPrinterTest, NullPlanPrintsPlaceholder) {
	EXPECT_EQ(LogicalPlanPrinter::print(nullptr), "(empty plan)");
}

TEST(LogicalPlanPrinterTest, PrintsIndentedTreeAndSkipsNullChildren) {
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto predicate = std::make_shared<LiteralExpression>(true);
	LogicalFilter filter(std::move(scan), predicate);

	auto printed = LogicalPlanPrinter::print(&filter);
	EXPECT_NE(printed.find("Filter("), std::string::npos);
	EXPECT_NE(printed.find("  NodeScan(n:Person)"), std::string::npos);

	(void)filter.detachChild(0);
	printed = LogicalPlanPrinter::print(&filter);
	EXPECT_NE(printed.find("Filter("), std::string::npos);
	EXPECT_EQ(printed.find("NodeScan("), std::string::npos);
}
