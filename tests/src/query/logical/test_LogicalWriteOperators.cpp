/**
 * @file test_LogicalWriteOperators.cpp
 * @brief Unit tests for logical write operator classes:
 *        CreateNode, CreateEdge, Delete, Set, Remove, MergeNode, MergeEdge.
 */

#include <gtest/gtest.h>

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalCreateEdge.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalSet.hpp"
#include "graph/query/logical/operators/LogicalRemove.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"
#include "graph/query/logical/operators/LogicalMergeEdge.hpp"
#include "graph/query/expressions/Expression.hpp"

using namespace graph::query::logical;
using namespace graph::query::expressions;

static std::shared_ptr<Expression> makeVarRefExpr(const std::string &var) {
    return std::shared_ptr<Expression>(
        std::make_unique<VariableReferenceExpression>(var).release());
}

static std::shared_ptr<Expression> makeLiteralExpr(int64_t val) {
    return std::make_shared<LiteralExpression>(val);
}

// =============================================================================
// LogicalCreateNode
// =============================================================================

class LogicalCreateNodeTest : public ::testing::Test {};

TEST_F(LogicalCreateNodeTest, BasicConstruction) {
    std::unordered_map<std::string, graph::PropertyValue> props = {
        {"name", graph::PropertyValue(std::string("Alice"))}
    };
    std::unordered_map<std::string, std::shared_ptr<Expression>> exprProps;
    LogicalCreateNode cn("n", {"Person"}, props, exprProps);

    EXPECT_EQ(cn.getType(), LogicalOpType::LOP_CREATE_NODE);
    EXPECT_EQ(cn.getVariable(), "n");
    ASSERT_EQ(cn.getLabels().size(), 1u);
    EXPECT_EQ(cn.getLabels()[0], "Person");
    EXPECT_EQ(cn.getProperties().size(), 1u);
    EXPECT_TRUE(cn.getChildren().empty());
}

TEST_F(LogicalCreateNodeTest, WithChild) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    std::unordered_map<std::string, std::shared_ptr<Expression>> exprProps;
    auto child = std::make_unique<LogicalNodeScan>("m");
    LogicalCreateNode cn("n", {"Person"}, props, exprProps, std::move(child));

    ASSERT_EQ(cn.getChildren().size(), 1u);
    EXPECT_NE(cn.getChildren()[0], nullptr);
}

TEST_F(LogicalCreateNodeTest, OutputVariablesAlwaysIncludesVariable) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    std::unordered_map<std::string, std::shared_ptr<Expression>> exprProps;
    LogicalCreateNode cn("n", {}, props, exprProps);

    auto vars = cn.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "n");
}

TEST_F(LogicalCreateNodeTest, OutputVariablesWithChild) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    std::unordered_map<std::string, std::shared_ptr<Expression>> exprProps;
    auto child = std::make_unique<LogicalNodeScan>("m");
    LogicalCreateNode cn("n", {}, props, exprProps, std::move(child));

    auto vars = cn.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "m");
    EXPECT_EQ(vars[1], "n");
}

TEST_F(LogicalCreateNodeTest, Clone) {
    std::unordered_map<std::string, graph::PropertyValue> props = {
        {"age", graph::PropertyValue(int64_t(30))}
    };
    std::unordered_map<std::string, std::shared_ptr<Expression>> exprProps = {
        {"score", makeLiteralExpr(100)}
    };
    LogicalCreateNode cn("n", {"Person"}, props, exprProps);

    auto cloned = cn.clone();
    auto *ccn = dynamic_cast<LogicalCreateNode *>(cloned.get());
    ASSERT_NE(ccn, nullptr);
    EXPECT_EQ(ccn->getVariable(), "n");
    EXPECT_EQ(ccn->getLabels().size(), 1u);
    EXPECT_EQ(ccn->getProperties().size(), 1u);
    EXPECT_EQ(ccn->getPropertyExprs().size(), 1u);
}

TEST_F(LogicalCreateNodeTest, CloneWithNullExpression) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    std::unordered_map<std::string, std::shared_ptr<Expression>> exprProps = {
        {"score", nullptr}
    };
    LogicalCreateNode cn("n", {}, props, exprProps);

    auto cloned = cn.clone();
    auto *ccn = dynamic_cast<LogicalCreateNode *>(cloned.get());
    ASSERT_NE(ccn, nullptr);
    EXPECT_EQ(ccn->getPropertyExprs().at("score"), nullptr);
}

TEST_F(LogicalCreateNodeTest, SetAndDetachChild) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    std::unordered_map<std::string, std::shared_ptr<Expression>> exprProps;
    LogicalCreateNode cn("n", {}, props, exprProps);

    cn.setChild(0, std::make_unique<LogicalNodeScan>("m"));
    ASSERT_EQ(cn.getChildren().size(), 1u);

    auto detached = cn.detachChild(0);
    ASSERT_NE(detached, nullptr);
}

TEST_F(LogicalCreateNodeTest, InvalidChildIndex) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    std::unordered_map<std::string, std::shared_ptr<Expression>> exprProps;
    LogicalCreateNode cn("n", {}, props, exprProps);

    EXPECT_THROW(cn.setChild(1, nullptr), std::logic_error);
    EXPECT_THROW(cn.detachChild(1), std::logic_error);
}

TEST_F(LogicalCreateNodeTest, ToString) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    std::unordered_map<std::string, std::shared_ptr<Expression>> exprProps;
    LogicalCreateNode cn("n", {}, props, exprProps);
    EXPECT_EQ(cn.toString(), "CreateNode(n)");
}

// =============================================================================
// LogicalCreateEdge
// =============================================================================

class LogicalCreateEdgeTest : public ::testing::Test {};

TEST_F(LogicalCreateEdgeTest, BasicConstruction) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    LogicalCreateEdge ce("r", "KNOWS", props, "n", "m");

    EXPECT_EQ(ce.getType(), LogicalOpType::LOP_CREATE_EDGE);
    EXPECT_EQ(ce.getVariable(), "r");
    EXPECT_EQ(ce.getEdgeType(), "KNOWS");
    EXPECT_EQ(ce.getSourceVar(), "n");
    EXPECT_EQ(ce.getTargetVar(), "m");
}

TEST_F(LogicalCreateEdgeTest, OutputVariablesNonEmptyVariable) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    LogicalCreateEdge ce("r", "KNOWS", props, "n", "m");

    auto vars = ce.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "r");
}

TEST_F(LogicalCreateEdgeTest, OutputVariablesEmptyVariable) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    LogicalCreateEdge ce("", "KNOWS", props, "n", "m");

    auto vars = ce.getOutputVariables();
    EXPECT_TRUE(vars.empty());
}

TEST_F(LogicalCreateEdgeTest, OutputVariablesWithChild) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalCreateEdge ce("r", "KNOWS", props, "n", "m", std::move(child));

    auto vars = ce.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "n");
    EXPECT_EQ(vars[1], "r");
}

TEST_F(LogicalCreateEdgeTest, Clone) {
    std::unordered_map<std::string, graph::PropertyValue> props = {
        {"since", graph::PropertyValue(int64_t(2020))}
    };
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalCreateEdge ce("r", "KNOWS", props, "n", "m", std::move(child));

    auto cloned = ce.clone();
    auto *cce = dynamic_cast<LogicalCreateEdge *>(cloned.get());
    ASSERT_NE(cce, nullptr);
    EXPECT_EQ(cce->getVariable(), "r");
    EXPECT_EQ(cce->getEdgeType(), "KNOWS");
    EXPECT_EQ(cce->getProperties().size(), 1u);
    EXPECT_EQ(cce->getChildren().size(), 1u);
}

TEST_F(LogicalCreateEdgeTest, NullChildGetChildrenAndCloneCoverage) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    LogicalCreateEdge ce("r", "KNOWS", props, "n", "m");

    EXPECT_TRUE(ce.getChildren().empty());

    auto cloned = ce.clone();
    auto *cce = dynamic_cast<LogicalCreateEdge *>(cloned.get());
    ASSERT_NE(cce, nullptr);
    EXPECT_TRUE(cce->getChildren().empty());
}

TEST_F(LogicalCreateEdgeTest, InvalidChildIndex) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    LogicalCreateEdge ce("r", "KNOWS", props, "n", "m");
    EXPECT_THROW(ce.setChild(1, nullptr), std::logic_error);
    EXPECT_THROW(ce.detachChild(1), std::logic_error);
}

TEST_F(LogicalCreateEdgeTest, ToString) {
    std::unordered_map<std::string, graph::PropertyValue> props;
    LogicalCreateEdge ce("r", "KNOWS", props, "n", "m");
    EXPECT_EQ(ce.toString(), "CreateEdge(r:KNOWS)");
}

// =============================================================================
// LogicalDelete
// =============================================================================

class LogicalDeleteTest : public ::testing::Test {};

TEST_F(LogicalDeleteTest, BasicConstruction) {
    LogicalDelete del({"n"}, false);
    EXPECT_EQ(del.getType(), LogicalOpType::LOP_DELETE);
    EXPECT_FALSE(del.isDetach());
    ASSERT_EQ(del.getVariables().size(), 1u);
    EXPECT_EQ(del.getVariables()[0], "n");
}

TEST_F(LogicalDeleteTest, DetachMode) {
    LogicalDelete del({"n", "r"}, true);
    EXPECT_TRUE(del.isDetach());
}

TEST_F(LogicalDeleteTest, ToStringDetachVsDelete) {
    LogicalDelete del({"n"}, false);
    EXPECT_EQ(del.toString(), "Delete");

    LogicalDelete detachDel({"n"}, true);
    EXPECT_EQ(detachDel.toString(), "DetachDelete");
}

TEST_F(LogicalDeleteTest, OutputVariablesFromChild) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalDelete del({"n"}, false, std::move(child));

    auto vars = del.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "n");
}

TEST_F(LogicalDeleteTest, OutputVariablesNoChild) {
    LogicalDelete del({"n"}, false);
    EXPECT_TRUE(del.getOutputVariables().empty());
}

TEST_F(LogicalDeleteTest, Clone) {
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalDelete del({"n", "r"}, true, std::move(child));

    auto cloned = del.clone();
    auto *cd = dynamic_cast<LogicalDelete *>(cloned.get());
    ASSERT_NE(cd, nullptr);
    EXPECT_TRUE(cd->isDetach());
    EXPECT_EQ(cd->getVariables().size(), 2u);
    EXPECT_EQ(cd->getChildren().size(), 1u);
}

TEST_F(LogicalDeleteTest, NullChildGetChildrenAndCloneCoverage) {
    LogicalDelete del({"n"}, false);

    EXPECT_TRUE(del.getChildren().empty());

    auto cloned = del.clone();
    auto *cd = dynamic_cast<LogicalDelete *>(cloned.get());
    ASSERT_NE(cd, nullptr);
    EXPECT_TRUE(cd->getChildren().empty());
}

TEST_F(LogicalDeleteTest, InvalidChildIndex) {
    LogicalDelete del({"n"}, false);
    EXPECT_THROW(del.setChild(1, nullptr), std::logic_error);
    EXPECT_THROW(del.detachChild(1), std::logic_error);
}

// =============================================================================
// LogicalSet
// =============================================================================

class LogicalSetTest : public ::testing::Test {};

TEST_F(LogicalSetTest, PropertySet) {
    std::vector<LogicalSetItem> items;
    items.push_back({SetActionType::LSET_PROPERTY, "n", "age", makeLiteralExpr(25)});
    LogicalSet set(std::move(items));

    EXPECT_EQ(set.getType(), LogicalOpType::LOP_SET);
    ASSERT_EQ(set.getItems().size(), 1u);
    EXPECT_EQ(set.getItems()[0].type, SetActionType::LSET_PROPERTY);
    EXPECT_EQ(set.getItems()[0].variable, "n");
    EXPECT_EQ(set.getItems()[0].key, "age");
    EXPECT_NE(set.getItems()[0].expression, nullptr);
}

TEST_F(LogicalSetTest, LabelSet) {
    std::vector<LogicalSetItem> items;
    items.push_back({SetActionType::LSET_LABEL, "n", "Admin", nullptr});
    LogicalSet set(std::move(items));

    ASSERT_EQ(set.getItems().size(), 1u);
    EXPECT_EQ(set.getItems()[0].type, SetActionType::LSET_LABEL);
    EXPECT_EQ(set.getItems()[0].key, "Admin");
}

TEST_F(LogicalSetTest, MapMerge) {
    std::vector<LogicalSetItem> items;
    items.push_back({SetActionType::LSET_MAP_MERGE, "n", "", makeVarRefExpr("props")});
    LogicalSet set(std::move(items));

    ASSERT_EQ(set.getItems().size(), 1u);
    EXPECT_EQ(set.getItems()[0].type, SetActionType::LSET_MAP_MERGE);
}

TEST_F(LogicalSetTest, OutputVariablesFromChild) {
    std::vector<LogicalSetItem> items;
    items.push_back({SetActionType::LSET_PROPERTY, "n", "age", makeLiteralExpr(25)});
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalSet set(std::move(items), std::move(child));

    auto vars = set.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "n");
}

TEST_F(LogicalSetTest, OutputVariablesNoChild) {
    std::vector<LogicalSetItem> items;
    items.push_back({SetActionType::LSET_PROPERTY, "n", "age", makeLiteralExpr(25)});
    LogicalSet set(std::move(items));

    EXPECT_TRUE(set.getOutputVariables().empty());
}

TEST_F(LogicalSetTest, Clone) {
    std::vector<LogicalSetItem> items;
    items.push_back({SetActionType::LSET_PROPERTY, "n", "age", makeLiteralExpr(25)});
    items.push_back({SetActionType::LSET_LABEL, "n", "Admin", nullptr});
    LogicalSet set(std::move(items));

    auto cloned = set.clone();
    auto *cs = dynamic_cast<LogicalSet *>(cloned.get());
    ASSERT_NE(cs, nullptr);
    ASSERT_EQ(cs->getItems().size(), 2u);
    EXPECT_NE(cs->getItems()[0].expression, nullptr);
    EXPECT_EQ(cs->getItems()[1].expression, nullptr);
}

TEST_F(LogicalSetTest, InvalidChildIndex) {
    std::vector<LogicalSetItem> items;
    LogicalSet set(std::move(items));
    EXPECT_THROW(set.setChild(1, nullptr), std::logic_error);
    EXPECT_THROW(set.detachChild(1), std::logic_error);
}

TEST_F(LogicalSetTest, ToString) {
    std::vector<LogicalSetItem> items;
    LogicalSet set(std::move(items));
    EXPECT_EQ(set.toString(), "Set");
}

// =============================================================================
// LogicalRemove
// =============================================================================

class LogicalRemoveTest : public ::testing::Test {};

TEST_F(LogicalRemoveTest, PropertyRemove) {
    std::vector<LogicalRemoveItem> items;
    items.push_back({LogicalRemoveActionType::LREM_PROPERTY, "n", "age"});
    LogicalRemove rem(std::move(items));

    EXPECT_EQ(rem.getType(), LogicalOpType::LOP_REMOVE);
    ASSERT_EQ(rem.getItems().size(), 1u);
    EXPECT_EQ(rem.getItems()[0].type, LogicalRemoveActionType::LREM_PROPERTY);
}

TEST_F(LogicalRemoveTest, LabelRemove) {
    std::vector<LogicalRemoveItem> items;
    items.push_back({LogicalRemoveActionType::LREM_LABEL, "n", "Admin"});
    LogicalRemove rem(std::move(items));

    ASSERT_EQ(rem.getItems().size(), 1u);
    EXPECT_EQ(rem.getItems()[0].type, LogicalRemoveActionType::LREM_LABEL);
    EXPECT_EQ(rem.getItems()[0].key, "Admin");
}

TEST_F(LogicalRemoveTest, OutputVariablesFromChild) {
    std::vector<LogicalRemoveItem> items;
    items.push_back({LogicalRemoveActionType::LREM_PROPERTY, "n", "age"});
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalRemove rem(std::move(items), std::move(child));

    auto vars = rem.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "n");
}

TEST_F(LogicalRemoveTest, OutputVariablesNoChild) {
    std::vector<LogicalRemoveItem> items;
    items.push_back({LogicalRemoveActionType::LREM_PROPERTY, "n", "age"});
    LogicalRemove rem(std::move(items));

    EXPECT_TRUE(rem.getOutputVariables().empty());
}

TEST_F(LogicalRemoveTest, Clone) {
    std::vector<LogicalRemoveItem> items;
    items.push_back({LogicalRemoveActionType::LREM_PROPERTY, "n", "age"});
    items.push_back({LogicalRemoveActionType::LREM_LABEL, "n", "Admin"});
    LogicalRemove rem(std::move(items));

    auto cloned = rem.clone();
    auto *cr = dynamic_cast<LogicalRemove *>(cloned.get());
    ASSERT_NE(cr, nullptr);
    ASSERT_EQ(cr->getItems().size(), 2u);
    EXPECT_EQ(cr->getItems()[0].type, LogicalRemoveActionType::LREM_PROPERTY);
    EXPECT_EQ(cr->getItems()[1].type, LogicalRemoveActionType::LREM_LABEL);
}

TEST_F(LogicalRemoveTest, InvalidChildIndex) {
    std::vector<LogicalRemoveItem> items;
    LogicalRemove rem(std::move(items));
    EXPECT_THROW(rem.setChild(1, nullptr), std::logic_error);
    EXPECT_THROW(rem.detachChild(1), std::logic_error);
}

TEST_F(LogicalRemoveTest, ToString) {
    std::vector<LogicalRemoveItem> items;
    LogicalRemove rem(std::move(items));
    EXPECT_EQ(rem.toString(), "Remove");
}

// =============================================================================
// LogicalMergeNode
// =============================================================================

class LogicalMergeNodeTest : public ::testing::Test {};

TEST_F(LogicalMergeNodeTest, BasicConstruction) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps = {
        {"name", graph::PropertyValue(std::string("Alice"))}
    };
    std::vector<MergeSetAction> onCreate;
    std::vector<MergeSetAction> onMatch;
    LogicalMergeNode mn("n", {"Person"}, matchProps, onCreate, onMatch);

    EXPECT_EQ(mn.getType(), LogicalOpType::LOP_MERGE_NODE);
    EXPECT_EQ(mn.getVariable(), "n");
    ASSERT_EQ(mn.getLabels().size(), 1u);
    EXPECT_EQ(mn.getMatchProps().size(), 1u);
}

TEST_F(LogicalMergeNodeTest, OutputVariables) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    LogicalMergeNode mn("n", {}, matchProps, {}, {});

    auto vars = mn.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "n");
}

TEST_F(LogicalMergeNodeTest, OutputVariablesWithChild) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    auto child = std::make_unique<LogicalNodeScan>("m");
    LogicalMergeNode mn("n", {}, matchProps, {}, {}, std::move(child));

    auto vars = mn.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "m");
    EXPECT_EQ(vars[1], "n");
}

TEST_F(LogicalMergeNodeTest, OnCreateAndOnMatchActions) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    std::vector<MergeSetAction> onCreate = {
        {"n", "createdAt", makeLiteralExpr(2024)}
    };
    std::vector<MergeSetAction> onMatch = {
        {"n", "updatedAt", makeLiteralExpr(2024)}
    };
    LogicalMergeNode mn("n", {}, matchProps, onCreate, onMatch);

    ASSERT_EQ(mn.getOnCreateItems().size(), 1u);
    EXPECT_EQ(mn.getOnCreateItems()[0].key, "createdAt");
    ASSERT_EQ(mn.getOnMatchItems().size(), 1u);
    EXPECT_EQ(mn.getOnMatchItems()[0].key, "updatedAt");
}

TEST_F(LogicalMergeNodeTest, Clone) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    std::vector<MergeSetAction> onCreate = {
        {"n", "prop", makeLiteralExpr(1)}
    };
    std::vector<MergeSetAction> onMatch = {
        {"n", "prop", nullptr}
    };
    LogicalMergeNode mn("n", {"Person"}, matchProps, onCreate, onMatch);

    auto cloned = mn.clone();
    auto *cmn = dynamic_cast<LogicalMergeNode *>(cloned.get());
    ASSERT_NE(cmn, nullptr);
    EXPECT_EQ(cmn->getVariable(), "n");
    ASSERT_EQ(cmn->getOnCreateItems().size(), 1u);
    EXPECT_NE(cmn->getOnCreateItems()[0].expression, nullptr);
    ASSERT_EQ(cmn->getOnMatchItems().size(), 1u);
    EXPECT_EQ(cmn->getOnMatchItems()[0].expression, nullptr);
}

TEST_F(LogicalMergeNodeTest, InvalidChildIndex) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    LogicalMergeNode mn("n", {}, matchProps, {}, {});
    EXPECT_THROW(mn.setChild(1, nullptr), std::logic_error);
    EXPECT_THROW(mn.detachChild(1), std::logic_error);
}

TEST_F(LogicalMergeNodeTest, ToString) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    LogicalMergeNode mn("n", {}, matchProps, {}, {});
    EXPECT_EQ(mn.toString(), "MergeNode(n)");
}

// =============================================================================
// LogicalMergeEdge
// =============================================================================

class LogicalMergeEdgeTest : public ::testing::Test {};

TEST_F(LogicalMergeEdgeTest, BasicConstruction) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    LogicalMergeEdge me("n", "r", "m", "KNOWS", "OUT", matchProps, {}, {});

    EXPECT_EQ(me.getType(), LogicalOpType::LOP_MERGE_EDGE);
    EXPECT_EQ(me.getSourceVar(), "n");
    EXPECT_EQ(me.getEdgeVar(), "r");
    EXPECT_EQ(me.getTargetVar(), "m");
    EXPECT_EQ(me.getEdgeType(), "KNOWS");
    EXPECT_EQ(me.getDirection(), "OUT");
}

TEST_F(LogicalMergeEdgeTest, OutputVariablesNonEmptyEdgeVar) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    LogicalMergeEdge me("n", "r", "m", "KNOWS", "OUT", matchProps, {}, {});

    auto vars = me.getOutputVariables();
    ASSERT_EQ(vars.size(), 1u);
    EXPECT_EQ(vars[0], "r");
}

TEST_F(LogicalMergeEdgeTest, OutputVariablesEmptyEdgeVar) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    LogicalMergeEdge me("n", "", "m", "KNOWS", "OUT", matchProps, {}, {});

    auto vars = me.getOutputVariables();
    EXPECT_TRUE(vars.empty());
}

TEST_F(LogicalMergeEdgeTest, OutputVariablesWithChild) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    auto child = std::make_unique<LogicalNodeScan>("n");
    LogicalMergeEdge me("n", "r", "m", "KNOWS", "OUT", matchProps, {}, {}, std::move(child));

    auto vars = me.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "n");
    EXPECT_EQ(vars[1], "r");
}

TEST_F(LogicalMergeEdgeTest, OnCreateAndOnMatchActions) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    std::vector<MergeSetAction> onCreate = {{"r", "since", makeLiteralExpr(2024)}};
    std::vector<MergeSetAction> onMatch = {{"r", "updatedAt", makeLiteralExpr(2024)}};
    LogicalMergeEdge me("n", "r", "m", "KNOWS", "OUT", matchProps, onCreate, onMatch);

    ASSERT_EQ(me.getOnCreateActions().size(), 1u);
    EXPECT_EQ(me.getOnCreateActions()[0].key, "since");
    ASSERT_EQ(me.getOnMatchActions().size(), 1u);
    EXPECT_EQ(me.getOnMatchActions()[0].key, "updatedAt");
}

TEST_F(LogicalMergeEdgeTest, Clone) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    std::vector<MergeSetAction> onCreate = {{"r", "since", makeLiteralExpr(2024)}};
    std::vector<MergeSetAction> onMatch = {{"r", "prop", nullptr}};
    LogicalMergeEdge me("n", "r", "m", "KNOWS", "OUT", matchProps, onCreate, onMatch);

    auto cloned = me.clone();
    auto *cme = dynamic_cast<LogicalMergeEdge *>(cloned.get());
    ASSERT_NE(cme, nullptr);
    EXPECT_EQ(cme->getSourceVar(), "n");
    EXPECT_EQ(cme->getEdgeVar(), "r");
    ASSERT_EQ(cme->getOnCreateActions().size(), 1u);
    EXPECT_NE(cme->getOnCreateActions()[0].expression, nullptr);
    ASSERT_EQ(cme->getOnMatchActions().size(), 1u);
    EXPECT_EQ(cme->getOnMatchActions()[0].expression, nullptr);
}

TEST_F(LogicalMergeEdgeTest, InvalidChildIndex) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    LogicalMergeEdge me("n", "r", "m", "KNOWS", "OUT", matchProps, {}, {});

    EXPECT_THROW(me.setChild(1, nullptr), std::out_of_range);
    EXPECT_THROW(me.detachChild(1), std::out_of_range);
}

TEST_F(LogicalMergeEdgeTest, ToString) {
    std::unordered_map<std::string, graph::PropertyValue> matchProps;
    LogicalMergeEdge me("n", "r", "m", "KNOWS", "OUT", matchProps, {}, {});
    EXPECT_EQ(me.toString(), "MergeEdge(r:KNOWS)");
}
