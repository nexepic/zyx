/**
 * @file test_LogicalSubqueryOperators.cpp
 * @brief Unit tests for LogicalLoadCsv, LogicalCallSubquery, LogicalForeach.
 */

#include <gtest/gtest.h>

#include "graph/query/logical/operators/LogicalLoadCsv.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalForeach.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/expressions/Expression.hpp"

using namespace graph::query::logical;
using namespace graph::query::expressions;

static std::shared_ptr<Expression> makeLiteral(const std::string &val) {
	return std::make_shared<LiteralExpression>(val);
}

static std::shared_ptr<Expression> makeVarRef(const std::string &var) {
	return std::shared_ptr<Expression>(
		std::make_unique<VariableReferenceExpression>(var).release());
}

// =============================================================================
// LogicalLoadCsv
// =============================================================================

class LogicalLoadCsvTest : public ::testing::Test {};

TEST_F(LogicalLoadCsvTest, BasicConstruction) {
	auto url = makeLiteral("file:///tmp/test.csv");
	LogicalLoadCsv csv(nullptr, url, "row", true, ",");

	EXPECT_EQ(csv.getType(), LogicalOpType::LOP_LOAD_CSV);
	EXPECT_EQ(csv.getRowVariable(), "row");
	EXPECT_TRUE(csv.isWithHeaders());
	EXPECT_EQ(csv.getFieldTerminator(), ",");
	EXPECT_EQ(csv.getUrlExpr(), url);
}

TEST_F(LogicalLoadCsvTest, DefaultParams) {
	auto url = makeLiteral("file:///tmp/test.csv");
	LogicalLoadCsv csv(nullptr, url, "row");

	EXPECT_FALSE(csv.isWithHeaders());
	EXPECT_EQ(csv.getFieldTerminator(), ",");
}

TEST_F(LogicalLoadCsvTest, GetChildrenNoChild) {
	auto url = makeLiteral("file:///tmp/test.csv");
	LogicalLoadCsv csv(nullptr, url, "row");
	EXPECT_TRUE(csv.getChildren().empty());
}

TEST_F(LogicalLoadCsvTest, GetChildrenWithChild) {
	auto url = makeLiteral("file:///tmp/test.csv");
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalLoadCsv csv(std::move(child), url, "row");
	ASSERT_EQ(csv.getChildren().size(), 1u);
	EXPECT_NE(csv.getChildren()[0], nullptr);
}

TEST_F(LogicalLoadCsvTest, OutputVariablesNoChild) {
	auto url = makeLiteral("file:///tmp/test.csv");
	LogicalLoadCsv csv(nullptr, url, "row");
	auto vars = csv.getOutputVariables();
	ASSERT_EQ(vars.size(), 1u);
	EXPECT_EQ(vars[0], "row");
}

TEST_F(LogicalLoadCsvTest, OutputVariablesWithChild) {
	auto url = makeLiteral("file:///tmp/test.csv");
	auto child = std::make_unique<LogicalNodeScan>("n");
	LogicalLoadCsv csv(std::move(child), url, "row");
	auto vars = csv.getOutputVariables();
	ASSERT_EQ(vars.size(), 2u);
	EXPECT_EQ(vars[0], "n");
	EXPECT_EQ(vars[1], "row");
}

TEST_F(LogicalLoadCsvTest, CloneWithChild) {
	auto url = makeLiteral("file:///tmp/test.csv");
	auto child = std::make_unique<LogicalNodeScan>("n");
	LogicalLoadCsv csv(std::move(child), url, "row", true, "\t");

	auto cloned = csv.clone();
	auto *clonedCsv = dynamic_cast<LogicalLoadCsv *>(cloned.get());
	ASSERT_NE(clonedCsv, nullptr);
	EXPECT_EQ(clonedCsv->getRowVariable(), "row");
	EXPECT_TRUE(clonedCsv->isWithHeaders());
	EXPECT_EQ(clonedCsv->getFieldTerminator(), "\t");
	EXPECT_EQ(clonedCsv->getChildren().size(), 1u);
}

TEST_F(LogicalLoadCsvTest, CloneNoChild) {
	auto url = makeLiteral("file:///tmp/test.csv");
	LogicalLoadCsv csv(nullptr, url, "row");

	auto cloned = csv.clone();
	auto *clonedCsv = dynamic_cast<LogicalLoadCsv *>(cloned.get());
	ASSERT_NE(clonedCsv, nullptr);
	EXPECT_TRUE(clonedCsv->getChildren().empty());
}

TEST_F(LogicalLoadCsvTest, ToStringWithHeaders) {
	auto url = makeLiteral("file:///tmp/test.csv");
	LogicalLoadCsv csv(nullptr, url, "row", true);
	EXPECT_EQ(csv.toString(), "LoadCsv(row, WITH HEADERS)");
}

TEST_F(LogicalLoadCsvTest, ToStringWithoutHeaders) {
	auto url = makeLiteral("file:///tmp/test.csv");
	LogicalLoadCsv csv(nullptr, url, "row", false);
	EXPECT_EQ(csv.toString(), "LoadCsv(row)");
}

TEST_F(LogicalLoadCsvTest, SetChild) {
	auto url = makeLiteral("file:///tmp/test.csv");
	LogicalLoadCsv csv(nullptr, url, "row");

	csv.setChild(0, std::make_unique<LogicalNodeScan>("n"));
	ASSERT_EQ(csv.getChildren().size(), 1u);
}

TEST_F(LogicalLoadCsvTest, SetChildInvalidIndex) {
	auto url = makeLiteral("file:///tmp/test.csv");
	LogicalLoadCsv csv(nullptr, url, "row");
	EXPECT_THROW(csv.setChild(1, nullptr), std::logic_error);
}

TEST_F(LogicalLoadCsvTest, DetachChild) {
	auto url = makeLiteral("file:///tmp/test.csv");
	auto child = std::make_unique<LogicalNodeScan>("n");
	LogicalLoadCsv csv(std::move(child), url, "row");

	auto detached = csv.detachChild(0);
	ASSERT_NE(detached, nullptr);
	EXPECT_TRUE(csv.getChildren().empty());
}

TEST_F(LogicalLoadCsvTest, DetachChildInvalidIndex) {
	auto url = makeLiteral("file:///tmp/test.csv");
	LogicalLoadCsv csv(nullptr, url, "row");
	EXPECT_THROW(csv.detachChild(1), std::logic_error);
}

// =============================================================================
// LogicalCallSubquery
// =============================================================================

class LogicalCallSubqueryTest : public ::testing::Test {};

TEST_F(LogicalCallSubqueryTest, BasicConstruction) {
	LogicalCallSubquery csq(nullptr, nullptr, {}, {});
	EXPECT_EQ(csq.getType(), LogicalOpType::LOP_CALL_SUBQUERY);
	EXPECT_EQ(csq.getInput(), nullptr);
	EXPECT_EQ(csq.getSubquery(), nullptr);
	EXPECT_TRUE(csq.getImportedVars().empty());
	EXPECT_TRUE(csq.getReturnedVars().empty());
	EXPECT_FALSE(csq.isInTransactions());
	EXPECT_EQ(csq.getBatchSize(), 0);
}

TEST_F(LogicalCallSubqueryTest, ConstructionWithParams) {
	auto input = std::make_unique<LogicalNodeScan>("n");
	auto subq = std::make_unique<LogicalSingleRow>();
	LogicalCallSubquery csq(std::move(input), std::move(subq),
	                        {"n"}, {"total"}, true, 100);

	EXPECT_NE(csq.getInput(), nullptr);
	EXPECT_NE(csq.getSubquery(), nullptr);
	ASSERT_EQ(csq.getImportedVars().size(), 1u);
	EXPECT_EQ(csq.getImportedVars()[0], "n");
	ASSERT_EQ(csq.getReturnedVars().size(), 1u);
	EXPECT_EQ(csq.getReturnedVars()[0], "total");
	EXPECT_TRUE(csq.isInTransactions());
	EXPECT_EQ(csq.getBatchSize(), 100);
}

TEST_F(LogicalCallSubqueryTest, GetChildrenBothPresent) {
	auto input = std::make_unique<LogicalNodeScan>("n");
	auto subq = std::make_unique<LogicalSingleRow>();
	LogicalCallSubquery csq(std::move(input), std::move(subq), {}, {});

	auto children = csq.getChildren();
	EXPECT_EQ(children.size(), 2u);
}

TEST_F(LogicalCallSubqueryTest, GetChildrenOnlyInput) {
	auto input = std::make_unique<LogicalNodeScan>("n");
	LogicalCallSubquery csq(std::move(input), nullptr, {}, {});

	auto children = csq.getChildren();
	EXPECT_EQ(children.size(), 1u);
}

TEST_F(LogicalCallSubqueryTest, GetChildrenOnlySubquery) {
	auto subq = std::make_unique<LogicalSingleRow>();
	LogicalCallSubquery csq(nullptr, std::move(subq), {}, {});

	auto children = csq.getChildren();
	EXPECT_EQ(children.size(), 1u);
}

TEST_F(LogicalCallSubqueryTest, GetChildrenEmpty) {
	LogicalCallSubquery csq(nullptr, nullptr, {}, {});
	EXPECT_TRUE(csq.getChildren().empty());
}

TEST_F(LogicalCallSubqueryTest, OutputVariablesNoInput) {
	LogicalCallSubquery csq(nullptr, nullptr, {}, {"total", "count"});
	auto vars = csq.getOutputVariables();
	ASSERT_EQ(vars.size(), 2u);
	EXPECT_EQ(vars[0], "total");
	EXPECT_EQ(vars[1], "count");
}

TEST_F(LogicalCallSubqueryTest, OutputVariablesWithInput) {
	auto input = std::make_unique<LogicalNodeScan>("n");
	LogicalCallSubquery csq(std::move(input), nullptr, {}, {"total"});
	auto vars = csq.getOutputVariables();
	ASSERT_EQ(vars.size(), 2u);
	EXPECT_EQ(vars[0], "n");
	EXPECT_EQ(vars[1], "total");
}

TEST_F(LogicalCallSubqueryTest, CloneBothChildren) {
	auto input = std::make_unique<LogicalNodeScan>("n");
	auto subq = std::make_unique<LogicalSingleRow>();
	LogicalCallSubquery csq(std::move(input), std::move(subq),
	                        {"n"}, {"total"}, true, 50);

	auto cloned = csq.clone();
	auto *ccsq = dynamic_cast<LogicalCallSubquery *>(cloned.get());
	ASSERT_NE(ccsq, nullptr);
	EXPECT_NE(ccsq->getInput(), nullptr);
	EXPECT_NE(ccsq->getSubquery(), nullptr);
	EXPECT_EQ(ccsq->getImportedVars().size(), 1u);
	EXPECT_EQ(ccsq->getReturnedVars().size(), 1u);
	EXPECT_TRUE(ccsq->isInTransactions());
	EXPECT_EQ(ccsq->getBatchSize(), 50);
}

TEST_F(LogicalCallSubqueryTest, CloneNoChildren) {
	LogicalCallSubquery csq(nullptr, nullptr, {}, {});
	auto cloned = csq.clone();
	auto *ccsq = dynamic_cast<LogicalCallSubquery *>(cloned.get());
	ASSERT_NE(ccsq, nullptr);
	EXPECT_EQ(ccsq->getInput(), nullptr);
	EXPECT_EQ(ccsq->getSubquery(), nullptr);
}

TEST_F(LogicalCallSubqueryTest, ToStringBasic) {
	LogicalCallSubquery csq(nullptr, nullptr, {}, {});
	EXPECT_EQ(csq.toString(), "CallSubquery");
}

TEST_F(LogicalCallSubqueryTest, ToStringInTransactions) {
	LogicalCallSubquery csq(nullptr, nullptr, {}, {}, true);
	EXPECT_EQ(csq.toString(), "CallSubquery(IN TRANSACTIONS)");
}

TEST_F(LogicalCallSubqueryTest, ToStringInTransactionsWithBatchSize) {
	LogicalCallSubquery csq(nullptr, nullptr, {}, {}, true, 100);
	EXPECT_EQ(csq.toString(), "CallSubquery(IN TRANSACTIONS OF 100 ROWS)");
}

TEST_F(LogicalCallSubqueryTest, SetChildIndex0) {
	LogicalCallSubquery csq(nullptr, nullptr, {}, {});
	csq.setChild(0, std::make_unique<LogicalNodeScan>("n"));
	EXPECT_NE(csq.getInput(), nullptr);
}

TEST_F(LogicalCallSubqueryTest, SetChildIndex1) {
	LogicalCallSubquery csq(nullptr, nullptr, {}, {});
	csq.setChild(1, std::make_unique<LogicalSingleRow>());
	EXPECT_NE(csq.getSubquery(), nullptr);
}

TEST_F(LogicalCallSubqueryTest, SetChildInvalidIndex) {
	LogicalCallSubquery csq(nullptr, nullptr, {}, {});
	EXPECT_THROW(csq.setChild(2, nullptr), std::logic_error);
}

TEST_F(LogicalCallSubqueryTest, DetachChildIndex0) {
	auto input = std::make_unique<LogicalNodeScan>("n");
	LogicalCallSubquery csq(std::move(input), nullptr, {}, {});
	auto detached = csq.detachChild(0);
	ASSERT_NE(detached, nullptr);
	EXPECT_EQ(csq.getInput(), nullptr);
}

TEST_F(LogicalCallSubqueryTest, DetachChildIndex1) {
	auto subq = std::make_unique<LogicalSingleRow>();
	LogicalCallSubquery csq(nullptr, std::move(subq), {}, {});
	auto detached = csq.detachChild(1);
	ASSERT_NE(detached, nullptr);
	EXPECT_EQ(csq.getSubquery(), nullptr);
}

TEST_F(LogicalCallSubqueryTest, DetachChildInvalidIndex) {
	LogicalCallSubquery csq(nullptr, nullptr, {}, {});
	EXPECT_THROW(csq.detachChild(2), std::logic_error);
}

// =============================================================================
// LogicalForeach
// =============================================================================

class LogicalForeachTest : public ::testing::Test {};

TEST_F(LogicalForeachTest, BasicConstruction) {
	auto listExpr = makeLiteral("list");
	LogicalForeach fe(nullptr, "x", listExpr, nullptr);

	EXPECT_EQ(fe.getType(), LogicalOpType::LOP_FOREACH);
	EXPECT_EQ(fe.getIterVar(), "x");
	EXPECT_EQ(fe.getListExpr(), listExpr);
	EXPECT_EQ(fe.getBody(), nullptr);
	EXPECT_EQ(fe.getInput(), nullptr);
}

TEST_F(LogicalForeachTest, WithChildAndBody) {
	auto listExpr = makeLiteral("list");
	auto child = std::make_unique<LogicalNodeScan>("n");
	auto body = std::make_unique<LogicalSingleRow>();
	LogicalForeach fe(std::move(child), "x", listExpr, std::move(body));

	EXPECT_NE(fe.getInput(), nullptr);
	EXPECT_NE(fe.getBody(), nullptr);
}

TEST_F(LogicalForeachTest, GetChildrenBothPresent) {
	auto listExpr = makeLiteral("list");
	auto child = std::make_unique<LogicalNodeScan>("n");
	auto body = std::make_unique<LogicalSingleRow>();
	LogicalForeach fe(std::move(child), "x", listExpr, std::move(body));

	auto children = fe.getChildren();
	EXPECT_EQ(children.size(), 2u);
}

TEST_F(LogicalForeachTest, GetChildrenOnlyChild) {
	auto listExpr = makeLiteral("list");
	auto child = std::make_unique<LogicalNodeScan>("n");
	LogicalForeach fe(std::move(child), "x", listExpr, nullptr);

	auto children = fe.getChildren();
	EXPECT_EQ(children.size(), 1u);
}

TEST_F(LogicalForeachTest, GetChildrenOnlyBody) {
	auto listExpr = makeLiteral("list");
	auto body = std::make_unique<LogicalSingleRow>();
	LogicalForeach fe(nullptr, "x", listExpr, std::move(body));

	auto children = fe.getChildren();
	EXPECT_EQ(children.size(), 1u);
}

TEST_F(LogicalForeachTest, GetChildrenEmpty) {
	auto listExpr = makeLiteral("list");
	LogicalForeach fe(nullptr, "x", listExpr, nullptr);
	EXPECT_TRUE(fe.getChildren().empty());
}

TEST_F(LogicalForeachTest, OutputVariablesNoChild) {
	auto listExpr = makeLiteral("list");
	LogicalForeach fe(nullptr, "x", listExpr, nullptr);
	EXPECT_TRUE(fe.getOutputVariables().empty());
}

TEST_F(LogicalForeachTest, OutputVariablesWithChild) {
	auto listExpr = makeLiteral("list");
	auto child = std::make_unique<LogicalNodeScan>("n");
	LogicalForeach fe(std::move(child), "x", listExpr, nullptr);
	auto vars = fe.getOutputVariables();
	ASSERT_EQ(vars.size(), 1u);
	EXPECT_EQ(vars[0], "n");
}

TEST_F(LogicalForeachTest, CloneBothChildren) {
	auto listExpr = makeLiteral("list");
	auto child = std::make_unique<LogicalNodeScan>("n");
	auto body = std::make_unique<LogicalSingleRow>();
	LogicalForeach fe(std::move(child), "x", listExpr, std::move(body));

	auto cloned = fe.clone();
	auto *cfe = dynamic_cast<LogicalForeach *>(cloned.get());
	ASSERT_NE(cfe, nullptr);
	EXPECT_EQ(cfe->getIterVar(), "x");
	EXPECT_NE(cfe->getInput(), nullptr);
	EXPECT_NE(cfe->getBody(), nullptr);
}

TEST_F(LogicalForeachTest, CloneNoChildren) {
	auto listExpr = makeLiteral("list");
	LogicalForeach fe(nullptr, "x", listExpr, nullptr);

	auto cloned = fe.clone();
	auto *cfe = dynamic_cast<LogicalForeach *>(cloned.get());
	ASSERT_NE(cfe, nullptr);
	EXPECT_EQ(cfe->getInput(), nullptr);
	EXPECT_EQ(cfe->getBody(), nullptr);
}

TEST_F(LogicalForeachTest, ToString) {
	auto listExpr = makeLiteral("list");
	LogicalForeach fe(nullptr, "x", listExpr, nullptr);
	EXPECT_EQ(fe.toString(), "Foreach(x)");
}

TEST_F(LogicalForeachTest, SetChildIndex0) {
	auto listExpr = makeLiteral("list");
	LogicalForeach fe(nullptr, "x", listExpr, nullptr);
	fe.setChild(0, std::make_unique<LogicalNodeScan>("n"));
	EXPECT_NE(fe.getInput(), nullptr);
}

TEST_F(LogicalForeachTest, SetChildIndex1) {
	auto listExpr = makeLiteral("list");
	LogicalForeach fe(nullptr, "x", listExpr, nullptr);
	fe.setChild(1, std::make_unique<LogicalSingleRow>());
	EXPECT_NE(fe.getBody(), nullptr);
}

TEST_F(LogicalForeachTest, SetChildInvalidIndex) {
	auto listExpr = makeLiteral("list");
	LogicalForeach fe(nullptr, "x", listExpr, nullptr);
	EXPECT_THROW(fe.setChild(2, nullptr), std::logic_error);
}

TEST_F(LogicalForeachTest, DetachChildIndex0) {
	auto listExpr = makeLiteral("list");
	auto child = std::make_unique<LogicalNodeScan>("n");
	LogicalForeach fe(std::move(child), "x", listExpr, nullptr);
	auto detached = fe.detachChild(0);
	ASSERT_NE(detached, nullptr);
	EXPECT_EQ(fe.getInput(), nullptr);
}

TEST_F(LogicalForeachTest, DetachChildIndex1) {
	auto listExpr = makeLiteral("list");
	auto body = std::make_unique<LogicalSingleRow>();
	LogicalForeach fe(nullptr, "x", listExpr, std::move(body));
	auto detached = fe.detachChild(1);
	ASSERT_NE(detached, nullptr);
	EXPECT_EQ(fe.getBody(), nullptr);
}

TEST_F(LogicalForeachTest, DetachChildInvalidIndex) {
	auto listExpr = makeLiteral("list");
	LogicalForeach fe(nullptr, "x", listExpr, nullptr);
	EXPECT_THROW(fe.detachChild(2), std::logic_error);
}
