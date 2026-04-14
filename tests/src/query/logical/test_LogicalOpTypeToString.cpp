/**
 * @file test_LogicalOpTypeToString.cpp
 * @brief Unit tests for LogicalOpType toString(), isDataMutatingOp(), isSchemaMutatingOp().
 */

#include <gtest/gtest.h>

#include "graph/query/logical/LogicalOperator.hpp"

using namespace graph::query::logical;

TEST(LogicalOpTypeToStringTest, ReadOperators) {
    EXPECT_EQ(toString(LogicalOpType::LOP_NODE_SCAN), "NodeScan");
    EXPECT_EQ(toString(LogicalOpType::LOP_FILTER), "Filter");
    EXPECT_EQ(toString(LogicalOpType::LOP_PROJECT), "Project");
    EXPECT_EQ(toString(LogicalOpType::LOP_AGGREGATE), "Aggregate");
    EXPECT_EQ(toString(LogicalOpType::LOP_SORT), "Sort");
    EXPECT_EQ(toString(LogicalOpType::LOP_LIMIT), "Limit");
    EXPECT_EQ(toString(LogicalOpType::LOP_SKIP), "Skip");
    EXPECT_EQ(toString(LogicalOpType::LOP_JOIN), "Join");
    EXPECT_EQ(toString(LogicalOpType::LOP_OPTIONAL_MATCH), "OptionalMatch");
    EXPECT_EQ(toString(LogicalOpType::LOP_TRAVERSAL), "Traversal");
    EXPECT_EQ(toString(LogicalOpType::LOP_VAR_LENGTH_TRAVERSAL), "VarLengthTraversal");
    EXPECT_EQ(toString(LogicalOpType::LOP_UNWIND), "Unwind");
    EXPECT_EQ(toString(LogicalOpType::LOP_UNION), "Union");
    EXPECT_EQ(toString(LogicalOpType::LOP_SINGLE_ROW), "SingleRow");
}

TEST(LogicalOpTypeToStringTest, WriteOperators) {
    EXPECT_EQ(toString(LogicalOpType::LOP_CREATE_NODE), "CreateNode");
    EXPECT_EQ(toString(LogicalOpType::LOP_CREATE_EDGE), "CreateEdge");
    EXPECT_EQ(toString(LogicalOpType::LOP_SET), "Set");
    EXPECT_EQ(toString(LogicalOpType::LOP_DELETE), "Delete");
    EXPECT_EQ(toString(LogicalOpType::LOP_REMOVE), "Remove");
    EXPECT_EQ(toString(LogicalOpType::LOP_MERGE_NODE), "MergeNode");
    EXPECT_EQ(toString(LogicalOpType::LOP_MERGE_EDGE), "MergeEdge");
}

TEST(LogicalOpTypeToStringTest, DDLAndAdminOperators) {
    EXPECT_EQ(toString(LogicalOpType::LOP_CREATE_INDEX), "CreateIndex");
    EXPECT_EQ(toString(LogicalOpType::LOP_DROP_INDEX), "DropIndex");
    EXPECT_EQ(toString(LogicalOpType::LOP_SHOW_INDEXES), "ShowIndexes");
    EXPECT_EQ(toString(LogicalOpType::LOP_CREATE_VECTOR_INDEX), "CreateVectorIndex");
    EXPECT_EQ(toString(LogicalOpType::LOP_CREATE_CONSTRAINT), "CreateConstraint");
    EXPECT_EQ(toString(LogicalOpType::LOP_DROP_CONSTRAINT), "DropConstraint");
    EXPECT_EQ(toString(LogicalOpType::LOP_SHOW_CONSTRAINTS), "ShowConstraints");
    EXPECT_EQ(toString(LogicalOpType::LOP_TRANSACTION_CONTROL), "TransactionControl");
    EXPECT_EQ(toString(LogicalOpType::LOP_LIST_CONFIG), "ListConfig");
    EXPECT_EQ(toString(LogicalOpType::LOP_SET_CONFIG), "SetConfig");
    EXPECT_EQ(toString(LogicalOpType::LOP_CALL_PROCEDURE), "CallProcedure");
    EXPECT_EQ(toString(LogicalOpType::LOP_ALGO_SHORTEST_PATH), "ShortestPath");
    EXPECT_EQ(toString(LogicalOpType::LOP_VECTOR_SEARCH), "VectorSearch");
    EXPECT_EQ(toString(LogicalOpType::LOP_TRAIN_VECTOR_INDEX), "TrainVectorIndex");
}

TEST(LogicalOpTypeToStringTest, ObservabilityOperators) {
    EXPECT_EQ(toString(LogicalOpType::LOP_EXPLAIN), "Explain");
    EXPECT_EQ(toString(LogicalOpType::LOP_PROFILE), "Profile");
}

TEST(LogicalOpTypeToStringTest, SubqueryAndAdvancedOperators) {
    EXPECT_EQ(toString(LogicalOpType::LOP_FOREACH), "Foreach");
    EXPECT_EQ(toString(LogicalOpType::LOP_CALL_SUBQUERY), "CallSubquery");
    EXPECT_EQ(toString(LogicalOpType::LOP_LOAD_CSV), "LoadCsv");
    EXPECT_EQ(toString(LogicalOpType::LOP_NAMED_PATH), "NamedPath");
}

TEST(LogicalOpTypeToStringTest, UnknownFallback) {
    EXPECT_EQ(toString(static_cast<LogicalOpType>(9999)), "Unknown");
}

// =========================================================================
// isDataMutatingOp tests
// =========================================================================

TEST(LogicalOpTypeMutationTest, DataMutatingOps_ReturnTrue) {
    EXPECT_TRUE(isDataMutatingOp(LogicalOpType::LOP_CREATE_NODE));
    EXPECT_TRUE(isDataMutatingOp(LogicalOpType::LOP_CREATE_EDGE));
    EXPECT_TRUE(isDataMutatingOp(LogicalOpType::LOP_SET));
    EXPECT_TRUE(isDataMutatingOp(LogicalOpType::LOP_DELETE));
    EXPECT_TRUE(isDataMutatingOp(LogicalOpType::LOP_REMOVE));
    EXPECT_TRUE(isDataMutatingOp(LogicalOpType::LOP_MERGE_NODE));
    EXPECT_TRUE(isDataMutatingOp(LogicalOpType::LOP_MERGE_EDGE));
    EXPECT_TRUE(isDataMutatingOp(LogicalOpType::LOP_FOREACH));
    EXPECT_TRUE(isDataMutatingOp(LogicalOpType::LOP_LOAD_CSV));
    EXPECT_TRUE(isDataMutatingOp(LogicalOpType::LOP_SET_CONFIG));
    EXPECT_TRUE(isDataMutatingOp(LogicalOpType::LOP_TRAIN_VECTOR_INDEX));
}

TEST(LogicalOpTypeMutationTest, NonDataMutatingOps_ReturnFalse) {
    EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_NODE_SCAN));
    EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_FILTER));
    EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_PROJECT));
    EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_CREATE_INDEX));
    EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_EXPLAIN));
}

// =========================================================================
// isSchemaMutatingOp tests
// =========================================================================

TEST(LogicalOpTypeMutationTest, SchemaMutatingOps_ReturnTrue) {
    EXPECT_TRUE(isSchemaMutatingOp(LogicalOpType::LOP_CREATE_INDEX));
    EXPECT_TRUE(isSchemaMutatingOp(LogicalOpType::LOP_DROP_INDEX));
    EXPECT_TRUE(isSchemaMutatingOp(LogicalOpType::LOP_CREATE_VECTOR_INDEX));
    EXPECT_TRUE(isSchemaMutatingOp(LogicalOpType::LOP_CREATE_CONSTRAINT));
    EXPECT_TRUE(isSchemaMutatingOp(LogicalOpType::LOP_DROP_CONSTRAINT));
}

TEST(LogicalOpTypeMutationTest, NonSchemaMutatingOps_ReturnFalse) {
    EXPECT_FALSE(isSchemaMutatingOp(LogicalOpType::LOP_NODE_SCAN));
    EXPECT_FALSE(isSchemaMutatingOp(LogicalOpType::LOP_CREATE_NODE));
    EXPECT_FALSE(isSchemaMutatingOp(LogicalOpType::LOP_DELETE));
    EXPECT_FALSE(isSchemaMutatingOp(LogicalOpType::LOP_SHOW_INDEXES));
}
