/**
 * @file test_LogicalAdminOperators.cpp
 * @brief Unit tests for logical DDL/admin operator classes:
 *        CreateIndex, DropIndex, ShowIndexes, CreateVectorIndex,
 *        CreateConstraint, DropConstraint, ShowConstraints,
 *        TransactionControl, CallProcedure, Explain, Profile.
 */

#include <gtest/gtest.h>

#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalCreateIndex.hpp"
#include "graph/query/logical/operators/LogicalDropIndex.hpp"
#include "graph/query/logical/operators/LogicalShowIndexes.hpp"
#include "graph/query/logical/operators/LogicalCreateVectorIndex.hpp"
#include "graph/query/logical/operators/LogicalCreateConstraint.hpp"
#include "graph/query/logical/operators/LogicalDropConstraint.hpp"
#include "graph/query/logical/operators/LogicalShowConstraints.hpp"
#include "graph/query/logical/operators/LogicalTransactionControl.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalExplain.hpp"
#include "graph/query/logical/operators/LogicalProfile.hpp"

using namespace graph::query::logical;

// =============================================================================
// LogicalCreateIndex
// =============================================================================

class LogicalCreateIndexTest : public ::testing::Test {};

TEST_F(LogicalCreateIndexTest, SinglePropertyConstruction) {
    LogicalCreateIndex ci("idx_name", "Person", "name");

    EXPECT_EQ(ci.getType(), LogicalOpType::LOP_CREATE_INDEX);
    EXPECT_EQ(ci.getIndexName(), "idx_name");
    EXPECT_EQ(ci.getLabel(), "Person");
    EXPECT_EQ(ci.getPropertyKey(), "name");
    EXPECT_FALSE(ci.isComposite());
    ASSERT_EQ(ci.getPropertyKeys().size(), 1u);
}

TEST_F(LogicalCreateIndexTest, CompositeConstruction) {
    LogicalCreateIndex ci("idx_comp", "Person", std::vector<std::string>{"name", "age"});

    EXPECT_TRUE(ci.isComposite());
    ASSERT_EQ(ci.getPropertyKeys().size(), 2u);
    EXPECT_EQ(ci.getPropertyKeys()[0], "name");
    EXPECT_EQ(ci.getPropertyKeys()[1], "age");
}

TEST_F(LogicalCreateIndexTest, LeafNode) {
    LogicalCreateIndex ci("idx", "Person", "name");
    EXPECT_TRUE(ci.getChildren().empty());
    EXPECT_TRUE(ci.getOutputVariables().empty());
    EXPECT_THROW(ci.setChild(0, nullptr), std::logic_error);
    EXPECT_THROW(ci.detachChild(0), std::logic_error);
}

TEST_F(LogicalCreateIndexTest, Clone) {
    LogicalCreateIndex ci("idx", "Person", std::vector<std::string>{"name", "age"});
    auto cloned = ci.clone();
    auto *cci = dynamic_cast<LogicalCreateIndex *>(cloned.get());
    ASSERT_NE(cci, nullptr);
    EXPECT_EQ(cci->getIndexName(), "idx");
    EXPECT_TRUE(cci->isComposite());
    EXPECT_EQ(cci->getPropertyKeys().size(), 2u);
}

TEST_F(LogicalCreateIndexTest, ToString) {
    LogicalCreateIndex ci("my_idx", "Person", "name");
    EXPECT_EQ(ci.toString(), "CreateIndex(my_idx)");
}

// =============================================================================
// LogicalDropIndex
// =============================================================================

class LogicalDropIndexTest : public ::testing::Test {};

TEST_F(LogicalDropIndexTest, BasicConstruction) {
    LogicalDropIndex di("idx_name", "Person", "name");

    EXPECT_EQ(di.getType(), LogicalOpType::LOP_DROP_INDEX);
    EXPECT_EQ(di.getIndexName(), "idx_name");
    EXPECT_EQ(di.getLabel(), "Person");
    EXPECT_EQ(di.getPropertyKey(), "name");
}

TEST_F(LogicalDropIndexTest, ToStringWithIndexName) {
    LogicalDropIndex di("my_idx", "Person", "name");
    EXPECT_EQ(di.toString(), "DropIndex(my_idx)");
}

TEST_F(LogicalDropIndexTest, ToStringWithoutIndexName) {
    LogicalDropIndex di("", "Person", "name");
    EXPECT_EQ(di.toString(), "DropIndex(Person.name)");
}

TEST_F(LogicalDropIndexTest, LeafNode) {
    LogicalDropIndex di("idx", "", "");
    EXPECT_TRUE(di.getChildren().empty());
    EXPECT_TRUE(di.getOutputVariables().empty());
    EXPECT_THROW(di.setChild(0, nullptr), std::logic_error);
    EXPECT_THROW(di.detachChild(0), std::logic_error);
}

TEST_F(LogicalDropIndexTest, Clone) {
    LogicalDropIndex di("idx", "Person", "name");
    auto cloned = di.clone();
    auto *cdi = dynamic_cast<LogicalDropIndex *>(cloned.get());
    ASSERT_NE(cdi, nullptr);
    EXPECT_EQ(cdi->getIndexName(), "idx");
    EXPECT_EQ(cdi->getLabel(), "Person");
    EXPECT_EQ(cdi->getPropertyKey(), "name");
}

// =============================================================================
// LogicalShowIndexes
// =============================================================================

TEST(LogicalShowIndexesTest, IsLeaf) {
    LogicalShowIndexes si;
    EXPECT_EQ(si.getType(), LogicalOpType::LOP_SHOW_INDEXES);
    EXPECT_TRUE(si.getChildren().empty());
    EXPECT_TRUE(si.getOutputVariables().empty());
    EXPECT_EQ(si.toString(), "ShowIndexes");
}

TEST(LogicalShowIndexesTest, LeafRejectsChildren) {
    LogicalShowIndexes si;
    EXPECT_THROW(si.setChild(0, nullptr), std::logic_error);
    EXPECT_THROW(si.detachChild(0), std::logic_error);
}

TEST(LogicalShowIndexesTest, Clone) {
    LogicalShowIndexes si;
    auto cloned = si.clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->getType(), LogicalOpType::LOP_SHOW_INDEXES);
}

// =============================================================================
// LogicalCreateVectorIndex
// =============================================================================

class LogicalCreateVectorIndexTest : public ::testing::Test {};

TEST_F(LogicalCreateVectorIndexTest, BasicConstruction) {
    LogicalCreateVectorIndex cvi("vec_idx", "Document", "embedding", 128, "cosine");

    EXPECT_EQ(cvi.getType(), LogicalOpType::LOP_CREATE_VECTOR_INDEX);
    EXPECT_EQ(cvi.getIndexName(), "vec_idx");
    EXPECT_EQ(cvi.getLabel(), "Document");
    EXPECT_EQ(cvi.getProperty(), "embedding");
    EXPECT_EQ(cvi.getDimension(), 128);
    EXPECT_EQ(cvi.getMetric(), "cosine");
}

TEST_F(LogicalCreateVectorIndexTest, LeafNode) {
    LogicalCreateVectorIndex cvi("idx", "L", "p", 64, "l2");
    EXPECT_TRUE(cvi.getChildren().empty());
    EXPECT_TRUE(cvi.getOutputVariables().empty());
    EXPECT_THROW(cvi.setChild(0, nullptr), std::logic_error);
    EXPECT_THROW(cvi.detachChild(0), std::logic_error);
}

TEST_F(LogicalCreateVectorIndexTest, Clone) {
    LogicalCreateVectorIndex cvi("vec_idx", "Document", "embedding", 128, "cosine");
    auto cloned = cvi.clone();
    auto *ccvi = dynamic_cast<LogicalCreateVectorIndex *>(cloned.get());
    ASSERT_NE(ccvi, nullptr);
    EXPECT_EQ(ccvi->getIndexName(), "vec_idx");
    EXPECT_EQ(ccvi->getDimension(), 128);
    EXPECT_EQ(ccvi->getMetric(), "cosine");
}

TEST_F(LogicalCreateVectorIndexTest, ToString) {
    LogicalCreateVectorIndex cvi("my_vec", "L", "p", 64, "l2");
    EXPECT_EQ(cvi.toString(), "CreateVectorIndex(my_vec)");
}

// =============================================================================
// LogicalCreateConstraint
// =============================================================================

class LogicalCreateConstraintTest : public ::testing::Test {};

TEST_F(LogicalCreateConstraintTest, BasicConstruction) {
    LogicalCreateConstraint cc("uniq_name", "NODE", "UNIQUE", "Person", {"name"}, "");

    EXPECT_EQ(cc.getType(), LogicalOpType::LOP_CREATE_CONSTRAINT);
    EXPECT_EQ(cc.getName(), "uniq_name");
    EXPECT_EQ(cc.getEntityType(), "NODE");
    EXPECT_EQ(cc.getConstraintType(), "UNIQUE");
    EXPECT_EQ(cc.getLabel(), "Person");
    ASSERT_EQ(cc.getProperties().size(), 1u);
    EXPECT_EQ(cc.getProperties()[0], "name");
    EXPECT_TRUE(cc.getOptions().empty());
}

TEST_F(LogicalCreateConstraintTest, LeafNode) {
    LogicalCreateConstraint cc("c", "NODE", "UNIQUE", "L", {"p"}, "");
    EXPECT_TRUE(cc.getChildren().empty());
    EXPECT_TRUE(cc.getOutputVariables().empty());
    EXPECT_THROW(cc.setChild(0, nullptr), std::logic_error);
    EXPECT_THROW(cc.detachChild(0), std::logic_error);
}

TEST_F(LogicalCreateConstraintTest, Clone) {
    LogicalCreateConstraint cc("c", "NODE", "UNIQUE", "Person", {"name", "email"}, "opt");
    auto cloned = cc.clone();
    auto *ccc = dynamic_cast<LogicalCreateConstraint *>(cloned.get());
    ASSERT_NE(ccc, nullptr);
    EXPECT_EQ(ccc->getName(), "c");
    EXPECT_EQ(ccc->getProperties().size(), 2u);
    EXPECT_EQ(ccc->getOptions(), "opt");
}

TEST_F(LogicalCreateConstraintTest, ToString) {
    LogicalCreateConstraint cc("my_constraint", "NODE", "UNIQUE", "Person", {"name"}, "");
    EXPECT_EQ(cc.toString(), "CreateConstraint(my_constraint)");
}

// =============================================================================
// LogicalDropConstraint
// =============================================================================

class LogicalDropConstraintTest : public ::testing::Test {};

TEST_F(LogicalDropConstraintTest, BasicConstruction) {
    LogicalDropConstraint dc("uniq_name", false);

    EXPECT_EQ(dc.getType(), LogicalOpType::LOP_DROP_CONSTRAINT);
    EXPECT_EQ(dc.getName(), "uniq_name");
    EXPECT_FALSE(dc.isIfExists());
}

TEST_F(LogicalDropConstraintTest, IfExistsFlag) {
    LogicalDropConstraint dc("c", true);
    EXPECT_TRUE(dc.isIfExists());
}

TEST_F(LogicalDropConstraintTest, LeafNode) {
    LogicalDropConstraint dc("c", false);
    EXPECT_TRUE(dc.getChildren().empty());
    EXPECT_TRUE(dc.getOutputVariables().empty());
    EXPECT_THROW(dc.setChild(0, nullptr), std::logic_error);
    EXPECT_THROW(dc.detachChild(0), std::logic_error);
}

TEST_F(LogicalDropConstraintTest, Clone) {
    LogicalDropConstraint dc("c", true);
    auto cloned = dc.clone();
    auto *cdc = dynamic_cast<LogicalDropConstraint *>(cloned.get());
    ASSERT_NE(cdc, nullptr);
    EXPECT_EQ(cdc->getName(), "c");
    EXPECT_TRUE(cdc->isIfExists());
}

TEST_F(LogicalDropConstraintTest, ToString) {
    LogicalDropConstraint dc("my_c", false);
    EXPECT_EQ(dc.toString(), "DropConstraint(my_c)");
}

// =============================================================================
// LogicalShowConstraints
// =============================================================================

TEST(LogicalShowConstraintsTest, IsLeaf) {
    LogicalShowConstraints sc;
    EXPECT_EQ(sc.getType(), LogicalOpType::LOP_SHOW_CONSTRAINTS);
    EXPECT_TRUE(sc.getChildren().empty());
    EXPECT_TRUE(sc.getOutputVariables().empty());
    EXPECT_EQ(sc.toString(), "ShowConstraints");
}

TEST(LogicalShowConstraintsTest, LeafRejectsChildren) {
    LogicalShowConstraints sc;
    EXPECT_THROW(sc.setChild(0, nullptr), std::logic_error);
    EXPECT_THROW(sc.detachChild(0), std::logic_error);
}

TEST(LogicalShowConstraintsTest, Clone) {
    LogicalShowConstraints sc;
    auto cloned = sc.clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(cloned->getType(), LogicalOpType::LOP_SHOW_CONSTRAINTS);
}

// =============================================================================
// LogicalTransactionControl
// =============================================================================

class LogicalTransactionControlTest : public ::testing::Test {};

TEST_F(LogicalTransactionControlTest, BeginConstruction) {
    LogicalTransactionControl tc(LogicalTxnCommand::LTXN_BEGIN);
    EXPECT_EQ(tc.getType(), LogicalOpType::LOP_TRANSACTION_CONTROL);
    EXPECT_EQ(tc.getCommand(), LogicalTxnCommand::LTXN_BEGIN);
}

TEST_F(LogicalTransactionControlTest, CommitConstruction) {
    LogicalTransactionControl tc(LogicalTxnCommand::LTXN_COMMIT);
    EXPECT_EQ(tc.getCommand(), LogicalTxnCommand::LTXN_COMMIT);
}

TEST_F(LogicalTransactionControlTest, RollbackConstruction) {
    LogicalTransactionControl tc(LogicalTxnCommand::LTXN_ROLLBACK);
    EXPECT_EQ(tc.getCommand(), LogicalTxnCommand::LTXN_ROLLBACK);
}

TEST_F(LogicalTransactionControlTest, ToStringBegin) {
    LogicalTransactionControl tc(LogicalTxnCommand::LTXN_BEGIN);
    EXPECT_EQ(tc.toString(), "TransactionControl(BEGIN)");
}

TEST_F(LogicalTransactionControlTest, ToStringCommit) {
    LogicalTransactionControl tc(LogicalTxnCommand::LTXN_COMMIT);
    EXPECT_EQ(tc.toString(), "TransactionControl(COMMIT)");
}

TEST_F(LogicalTransactionControlTest, ToStringRollback) {
    LogicalTransactionControl tc(LogicalTxnCommand::LTXN_ROLLBACK);
    EXPECT_EQ(tc.toString(), "TransactionControl(ROLLBACK)");
}

TEST_F(LogicalTransactionControlTest, LeafNode) {
    LogicalTransactionControl tc(LogicalTxnCommand::LTXN_BEGIN);
    EXPECT_TRUE(tc.getChildren().empty());
    EXPECT_TRUE(tc.getOutputVariables().empty());
    EXPECT_THROW(tc.setChild(0, nullptr), std::logic_error);
    EXPECT_THROW(tc.detachChild(0), std::logic_error);
}

TEST_F(LogicalTransactionControlTest, Clone) {
    LogicalTransactionControl tc(LogicalTxnCommand::LTXN_COMMIT);
    auto cloned = tc.clone();
    auto *ctc = dynamic_cast<LogicalTransactionControl *>(cloned.get());
    ASSERT_NE(ctc, nullptr);
    EXPECT_EQ(ctc->getCommand(), LogicalTxnCommand::LTXN_COMMIT);
}

// =============================================================================
// LogicalCallProcedure
// =============================================================================

class LogicalCallProcedureTest : public ::testing::Test {};

TEST_F(LogicalCallProcedureTest, BasicConstruction) {
    std::vector<graph::PropertyValue> args;
    args.emplace_back(std::string("arg1"));
    args.emplace_back(int64_t(42));
    LogicalCallProcedure cp("db.stats", std::move(args));

    EXPECT_EQ(cp.getType(), LogicalOpType::LOP_CALL_PROCEDURE);
    EXPECT_EQ(cp.getProcedureName(), "db.stats");
    EXPECT_EQ(cp.getArgs().size(), 2u);
}

TEST_F(LogicalCallProcedureTest, LeafNode) {
    LogicalCallProcedure cp("proc", {});
    EXPECT_TRUE(cp.getChildren().empty());
    EXPECT_TRUE(cp.getOutputVariables().empty());
    EXPECT_THROW(cp.setChild(0, nullptr), std::logic_error);
    EXPECT_THROW(cp.detachChild(0), std::logic_error);
}

TEST_F(LogicalCallProcedureTest, Clone) {
    std::vector<graph::PropertyValue> args;
    args.emplace_back(int64_t(1));
    LogicalCallProcedure cp("db.stats", std::move(args));

    auto cloned = cp.clone();
    auto *ccp = dynamic_cast<LogicalCallProcedure *>(cloned.get());
    ASSERT_NE(ccp, nullptr);
    EXPECT_EQ(ccp->getProcedureName(), "db.stats");
    EXPECT_EQ(ccp->getArgs().size(), 1u);
}

TEST_F(LogicalCallProcedureTest, ToString) {
    LogicalCallProcedure cp("db.stats", {});
    EXPECT_EQ(cp.toString(), "CallProcedure(db.stats)");
}

// =============================================================================
// LogicalExplain
// =============================================================================

class LogicalExplainTest : public ::testing::Test {};

TEST_F(LogicalExplainTest, BasicConstruction) {
    auto inner = std::make_unique<LogicalNodeScan>("n");
    LogicalExplain explain(std::move(inner));

    EXPECT_EQ(explain.getType(), LogicalOpType::LOP_EXPLAIN);
    ASSERT_EQ(explain.getChildren().size(), 1u);
    EXPECT_NE(explain.getInnerPlan(), nullptr);
}

TEST_F(LogicalExplainTest, OutputVariables_Admin) {
    auto inner = std::make_unique<LogicalNodeScan>("n");
    LogicalExplain explain(std::move(inner));

    auto vars = explain.getOutputVariables();
    ASSERT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "operator");
    EXPECT_EQ(vars[1], "details");
}

TEST_F(LogicalExplainTest, NullInnerPlan) {
    LogicalExplain explain(nullptr);
    EXPECT_TRUE(explain.getChildren().empty());
    EXPECT_EQ(explain.getInnerPlan(), nullptr);
}

TEST_F(LogicalExplainTest, SetChildIgnoresIndex) {
    auto inner = std::make_unique<LogicalNodeScan>("n");
    LogicalExplain explain(nullptr);

    explain.setChild(0, std::move(inner));
    EXPECT_NE(explain.getInnerPlan(), nullptr);

    auto replacement = std::make_unique<LogicalNodeScan>("m");
    explain.setChild(99, std::move(replacement));
    auto *scan = dynamic_cast<LogicalNodeScan *>(explain.getInnerPlan());
    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->getVariable(), "m");
}

TEST_F(LogicalExplainTest, DetachChildIgnoresIndex) {
    auto inner = std::make_unique<LogicalNodeScan>("n");
    LogicalExplain explain(std::move(inner));

    auto detached = explain.detachChild(99);
    ASSERT_NE(detached, nullptr);
    EXPECT_EQ(explain.getInnerPlan(), nullptr);
}

TEST_F(LogicalExplainTest, Clone) {
    auto inner = std::make_unique<LogicalNodeScan>("n");
    LogicalExplain explain(std::move(inner));

    auto cloned = explain.clone();
    auto *ce = dynamic_cast<LogicalExplain *>(cloned.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_NE(ce->getInnerPlan(), nullptr);
}

TEST_F(LogicalExplainTest, CloneWithNull) {
    LogicalExplain explain(nullptr);
    auto cloned = explain.clone();
    auto *ce = dynamic_cast<LogicalExplain *>(cloned.get());
    ASSERT_NE(ce, nullptr);
    EXPECT_EQ(ce->getInnerPlan(), nullptr);
}

TEST_F(LogicalExplainTest, ToString) {
    LogicalExplain explain(nullptr);
    EXPECT_EQ(explain.toString(), "Explain");
}

// =============================================================================
// LogicalProfile
// =============================================================================

class LogicalProfileTest : public ::testing::Test {};

TEST_F(LogicalProfileTest, BasicConstruction) {
    auto inner = std::make_unique<LogicalNodeScan>("n");
    LogicalProfile profile(std::move(inner));

    EXPECT_EQ(profile.getType(), LogicalOpType::LOP_PROFILE);
    ASSERT_EQ(profile.getChildren().size(), 1u);
    EXPECT_NE(profile.getInnerPlan(), nullptr);
}

TEST_F(LogicalProfileTest, OutputVariablesEmpty) {
    auto inner = std::make_unique<LogicalNodeScan>("n");
    LogicalProfile profile(std::move(inner));

    EXPECT_TRUE(profile.getOutputVariables().empty());
}

TEST_F(LogicalProfileTest, NullInnerPlan) {
    LogicalProfile profile(nullptr);
    EXPECT_TRUE(profile.getChildren().empty());
    EXPECT_EQ(profile.getInnerPlan(), nullptr);
}

TEST_F(LogicalProfileTest, SetChildIgnoresIndex) {
    LogicalProfile profile(nullptr);

    auto inner = std::make_unique<LogicalNodeScan>("n");
    profile.setChild(0, std::move(inner));
    EXPECT_NE(profile.getInnerPlan(), nullptr);

    auto replacement = std::make_unique<LogicalNodeScan>("m");
    profile.setChild(42, std::move(replacement));
    auto *scan = dynamic_cast<const LogicalNodeScan *>(profile.getInnerPlan());
    ASSERT_NE(scan, nullptr);
    EXPECT_EQ(scan->getVariable(), "m");
}

TEST_F(LogicalProfileTest, DetachChildIgnoresIndex) {
    auto inner = std::make_unique<LogicalNodeScan>("n");
    LogicalProfile profile(std::move(inner));

    auto detached = profile.detachChild(42);
    ASSERT_NE(detached, nullptr);
    EXPECT_EQ(profile.getInnerPlan(), nullptr);
}

TEST_F(LogicalProfileTest, Clone) {
    auto inner = std::make_unique<LogicalNodeScan>("n");
    LogicalProfile profile(std::move(inner));

    auto cloned = profile.clone();
    auto *cp = dynamic_cast<LogicalProfile *>(cloned.get());
    ASSERT_NE(cp, nullptr);
    EXPECT_NE(cp->getInnerPlan(), nullptr);
}

TEST_F(LogicalProfileTest, CloneWithNull) {
    LogicalProfile profile(nullptr);
    auto cloned = profile.clone();
    auto *cp = dynamic_cast<LogicalProfile *>(cloned.get());
    ASSERT_NE(cp, nullptr);
    EXPECT_EQ(cp->getInnerPlan(), nullptr);
}

TEST_F(LogicalProfileTest, ToString) {
    LogicalProfile profile(nullptr);
    EXPECT_EQ(profile.toString(), "Profile");
}
