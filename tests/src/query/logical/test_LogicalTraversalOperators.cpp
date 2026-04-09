/**
 * @file test_LogicalTraversalOperators.cpp
 * @brief Unit tests for LogicalTraversal and LogicalVarLengthTraversal.
 */

#include <gtest/gtest.h>

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalVarLengthTraversal.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"

using namespace graph::query::logical;

// =============================================================================
// LogicalTraversal
// =============================================================================

class LogicalTraversalTest : public ::testing::Test {};

TEST_F(LogicalTraversalTest, BasicConstruction) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalTraversal trav(std::move(child), "n", "r", "m", "KNOWS", "OUT");

    EXPECT_EQ(trav.getType(), LogicalOpType::LOP_TRAVERSAL);
    EXPECT_EQ(trav.getSourceVar(), "n");
    EXPECT_EQ(trav.getEdgeVar(), "r");
    EXPECT_EQ(trav.getTargetVar(), "m");
    EXPECT_EQ(trav.getEdgeType(), "KNOWS");
    EXPECT_EQ(trav.getDirection(), "OUT");
    EXPECT_TRUE(trav.getTargetLabels().empty());
    EXPECT_TRUE(trav.getTargetProperties().empty());
    EXPECT_TRUE(trav.getEdgeProperties().empty());
}

TEST_F(LogicalTraversalTest, WithTargetLabelsAndProperties) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    std::vector<std::string> targetLabels = {"Person"};
    std::vector<std::pair<std::string, graph::PropertyValue>> targetProps = {
        {"age", graph::PropertyValue(int64_t(30))}
    };
    std::unordered_map<std::string, graph::PropertyValue> edgeProps = {
        {"since", graph::PropertyValue(int64_t(2020))}
    };
    LogicalTraversal trav(std::move(child), "n", "r", "m", "KNOWS", "OUT",
                          targetLabels, targetProps, edgeProps);

    ASSERT_EQ(trav.getTargetLabels().size(), 1u);
    EXPECT_EQ(trav.getTargetLabels()[0], "Person");
    ASSERT_EQ(trav.getTargetProperties().size(), 1u);
    EXPECT_EQ(trav.getTargetProperties()[0].first, "age");
    ASSERT_EQ(trav.getEdgeProperties().size(), 1u);
    EXPECT_NE(trav.getEdgeProperties().find("since"), trav.getEdgeProperties().end());
}

TEST_F(LogicalTraversalTest, OutputVariablesWithBothVars) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalTraversal trav(std::move(child), "n", "r", "m", "KNOWS", "OUT");

    auto vars = trav.getOutputVariables();
    ASSERT_EQ(vars.size(), 3u);
    EXPECT_EQ(vars[0], "n");
    EXPECT_EQ(vars[1], "r");
    EXPECT_EQ(vars[2], "m");
}

TEST_F(LogicalTraversalTest, OutputVariablesEmptyEdgeVar) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalTraversal trav(std::move(child), "n", "", "m", "KNOWS", "OUT");

    auto vars = trav.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "n");
    EXPECT_EQ(vars[1], "m");
}

TEST_F(LogicalTraversalTest, OutputVariablesEmptyTargetVar) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalTraversal trav(std::move(child), "n", "r", "", "KNOWS", "OUT");

    auto vars = trav.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "n");
    EXPECT_EQ(vars[1], "r");
}

TEST_F(LogicalTraversalTest, OutputVariablesBothEmpty) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalTraversal trav(std::move(child), "n", "", "", "KNOWS", "OUT");

    auto vars = trav.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "n");
}

TEST_F(LogicalTraversalTest, Clone) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalTraversal trav(std::move(child), "n", "r", "m", "KNOWS", "OUT",
                          {"Person"}, {}, {});

    auto cloned = trav.clone();
    auto *ct = dynamic_cast<LogicalTraversal *>(cloned.get());
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->getSourceVar(), "n");
    EXPECT_EQ(ct->getEdgeVar(), "r");
    EXPECT_EQ(ct->getTargetVar(), "m");
    EXPECT_EQ(ct->getEdgeType(), "KNOWS");
    EXPECT_EQ(ct->getDirection(), "OUT");
    ASSERT_EQ(ct->getTargetLabels().size(), 1u);
}

TEST_F(LogicalTraversalTest, SetAndDetachChild) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalTraversal trav(std::move(child), "n", "r", "m", "KNOWS", "OUT");

    auto detached = trav.detachChild(0);
    ASSERT_NE(detached, nullptr);
    EXPECT_EQ(trav.getChildren()[0], nullptr);

    trav.setChild(0, std::move(detached));
    EXPECT_NE(trav.getChildren()[0], nullptr);
}

TEST_F(LogicalTraversalTest, OutOfRangeChild) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalTraversal trav(std::move(child), "n", "r", "m", "KNOWS", "OUT");

    EXPECT_THROW(trav.setChild(1, nullptr), std::out_of_range);
    EXPECT_THROW(trav.detachChild(1), std::out_of_range);
}

TEST_F(LogicalTraversalTest, ToString) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalTraversal trav(std::move(child), "n", "r", "m", "KNOWS", "OUT");

    auto str = trav.toString();
    EXPECT_NE(str.find("Traversal"), std::string::npos);
    EXPECT_NE(str.find("KNOWS"), std::string::npos);
}

TEST_F(LogicalTraversalTest, NullChildOutputAndCloneCoverage) {
    LogicalTraversal trav(nullptr, "n", "r", "m", "KNOWS", "OUT");

    auto vars = trav.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "r");
    EXPECT_EQ(vars[1], "m");

    auto cloned = trav.clone();
    auto *ct = dynamic_cast<LogicalTraversal *>(cloned.get());
    ASSERT_NE(ct, nullptr);
    ASSERT_EQ(ct->getChildren().size(), 1u);
    EXPECT_EQ(ct->getChildren()[0], nullptr);
}

// =============================================================================
// LogicalVarLengthTraversal
// =============================================================================

class LogicalVarLengthTraversalTest : public ::testing::Test {};

TEST_F(LogicalVarLengthTraversalTest, BasicConstruction) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalVarLengthTraversal vlt(std::move(child), "n", "r", "m", "KNOWS", "OUT", 1, 3);

    EXPECT_EQ(vlt.getType(), LogicalOpType::LOP_VAR_LENGTH_TRAVERSAL);
    EXPECT_EQ(vlt.getSourceVar(), "n");
    EXPECT_EQ(vlt.getEdgeVar(), "r");
    EXPECT_EQ(vlt.getTargetVar(), "m");
    EXPECT_EQ(vlt.getEdgeType(), "KNOWS");
    EXPECT_EQ(vlt.getDirection(), "OUT");
    EXPECT_EQ(vlt.getMinHops(), 1);
    EXPECT_EQ(vlt.getMaxHops(), 3);
}

TEST_F(LogicalVarLengthTraversalTest, DifferentHopValues) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalVarLengthTraversal vlt(std::move(child), "n", "r", "m", "FOLLOWS", "BOTH", 0, 10);

    EXPECT_EQ(vlt.getMinHops(), 0);
    EXPECT_EQ(vlt.getMaxHops(), 10);
    EXPECT_EQ(vlt.getEdgeType(), "FOLLOWS");
    EXPECT_EQ(vlt.getDirection(), "BOTH");
}

TEST_F(LogicalVarLengthTraversalTest, OutputVariablesWithBothVars) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalVarLengthTraversal vlt(std::move(child), "n", "r", "m", "KNOWS", "OUT", 1, 3);

    auto vars = vlt.getOutputVariables();
    ASSERT_EQ(vars.size(), 3u);
    EXPECT_EQ(vars[0], "n");
    EXPECT_EQ(vars[1], "r");
    EXPECT_EQ(vars[2], "m");
}

TEST_F(LogicalVarLengthTraversalTest, OutputVariablesEmptyEdgeVar) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalVarLengthTraversal vlt(std::move(child), "n", "", "m", "KNOWS", "OUT", 1, 3);

    auto vars = vlt.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "n");
    EXPECT_EQ(vars[1], "m");
}

TEST_F(LogicalVarLengthTraversalTest, OutputVariablesEmptyTargetVar) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalVarLengthTraversal vlt(std::move(child), "n", "r", "", "KNOWS", "OUT", 1, 3);

    auto vars = vlt.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "n");
    EXPECT_EQ(vars[1], "r");
}

TEST_F(LogicalVarLengthTraversalTest, Clone) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalVarLengthTraversal vlt(std::move(child), "n", "r", "m", "KNOWS", "OUT", 2, 5,
                                  {"Person"}, {});

    auto cloned = vlt.clone();
    auto *cv = dynamic_cast<LogicalVarLengthTraversal *>(cloned.get());
    ASSERT_NE(cv, nullptr);
    EXPECT_EQ(cv->getMinHops(), 2);
    EXPECT_EQ(cv->getMaxHops(), 5);
    EXPECT_EQ(cv->getSourceVar(), "n");
    ASSERT_EQ(cv->getTargetLabels().size(), 1u);
}

TEST_F(LogicalVarLengthTraversalTest, SetAndDetachChild) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalVarLengthTraversal vlt(std::move(child), "n", "r", "m", "KNOWS", "OUT", 1, 3);

    auto detached = vlt.detachChild(0);
    ASSERT_NE(detached, nullptr);

    vlt.setChild(0, std::move(detached));
    EXPECT_NE(vlt.getChildren()[0], nullptr);
}

TEST_F(LogicalVarLengthTraversalTest, OutOfRangeChild) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalVarLengthTraversal vlt(std::move(child), "n", "r", "m", "KNOWS", "OUT", 1, 3);

    EXPECT_THROW(vlt.setChild(1, nullptr), std::out_of_range);
    EXPECT_THROW(vlt.detachChild(1), std::out_of_range);
}

TEST_F(LogicalVarLengthTraversalTest, ToString) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalVarLengthTraversal vlt(std::move(child), "n", "r", "m", "KNOWS", "OUT", 1, 3);

    auto str = vlt.toString();
    EXPECT_NE(str.find("VarLengthTraversal"), std::string::npos);
    EXPECT_NE(str.find("1..3"), std::string::npos);
}

TEST_F(LogicalVarLengthTraversalTest, NullChildOutputAndCloneCoverage) {
    LogicalVarLengthTraversal vlt(nullptr, "n", "r", "m", "KNOWS", "OUT", 1, 3);

    auto vars = vlt.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "r");
    EXPECT_EQ(vars[1], "m");

    auto cloned = vlt.clone();
    auto *cv = dynamic_cast<LogicalVarLengthTraversal *>(cloned.get());
    ASSERT_NE(cv, nullptr);
    ASSERT_EQ(cv->getChildren().size(), 1u);
    EXPECT_EQ(cv->getChildren()[0], nullptr);
}
