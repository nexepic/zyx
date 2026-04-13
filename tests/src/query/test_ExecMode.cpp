/**
 * @file test_ExecMode.cpp
 * @brief Unit tests for ExecMode helpers, QueryPlan flags, and LogicalOperator mutation helpers.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include <gtest/gtest.h>

#include "graph/query/ExecMode.hpp"
#include "graph/query/QueryPlan.hpp"
#include "graph/query/logical/LogicalOperator.hpp"

using namespace graph::query;
using namespace graph::query::logical;

// ============================================================================
// ExecMode helper tests
// ============================================================================

TEST(ExecModeTest, FullAllowsEverything) {
	EXPECT_TRUE(allowsDataWrite(ExecMode::EM_FULL));
	EXPECT_TRUE(allowsSchemaWrite(ExecMode::EM_FULL));
}

TEST(ExecModeTest, ReadWriteAllowsDataButNotSchema) {
	EXPECT_TRUE(allowsDataWrite(ExecMode::EM_READ_WRITE));
	EXPECT_FALSE(allowsSchemaWrite(ExecMode::EM_READ_WRITE));
}

TEST(ExecModeTest, ReadOnlyAllowsNothing) {
	EXPECT_FALSE(allowsDataWrite(ExecMode::EM_READ_ONLY));
	EXPECT_FALSE(allowsSchemaWrite(ExecMode::EM_READ_ONLY));
}

// ============================================================================
// isDataMutatingOp tests
// ============================================================================

TEST(LogicalOpHelperTest, DataMutatingOps) {
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

TEST(LogicalOpHelperTest, ReadOpsAreNotDataMutating) {
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_NODE_SCAN));
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_FILTER));
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_PROJECT));
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_AGGREGATE));
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_SORT));
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_LIMIT));
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_SKIP));
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_JOIN));
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_TRAVERSAL));
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_UNWIND));
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_UNION));
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_EXPLAIN));
	EXPECT_FALSE(isDataMutatingOp(LogicalOpType::LOP_PROFILE));
}

// ============================================================================
// isSchemaMutatingOp tests
// ============================================================================

TEST(LogicalOpHelperTest, SchemaMutatingOps) {
	EXPECT_TRUE(isSchemaMutatingOp(LogicalOpType::LOP_CREATE_INDEX));
	EXPECT_TRUE(isSchemaMutatingOp(LogicalOpType::LOP_DROP_INDEX));
	EXPECT_TRUE(isSchemaMutatingOp(LogicalOpType::LOP_CREATE_VECTOR_INDEX));
	EXPECT_TRUE(isSchemaMutatingOp(LogicalOpType::LOP_CREATE_CONSTRAINT));
	EXPECT_TRUE(isSchemaMutatingOp(LogicalOpType::LOP_DROP_CONSTRAINT));
}

TEST(LogicalOpHelperTest, DataMutatingOpsAreNotSchemaMutating) {
	EXPECT_FALSE(isSchemaMutatingOp(LogicalOpType::LOP_CREATE_NODE));
	EXPECT_FALSE(isSchemaMutatingOp(LogicalOpType::LOP_CREATE_EDGE));
	EXPECT_FALSE(isSchemaMutatingOp(LogicalOpType::LOP_SET));
	EXPECT_FALSE(isSchemaMutatingOp(LogicalOpType::LOP_DELETE));
	EXPECT_FALSE(isSchemaMutatingOp(LogicalOpType::LOP_MERGE_NODE));
}

TEST(LogicalOpHelperTest, ReadOpsAreNotSchemaMutating) {
	EXPECT_FALSE(isSchemaMutatingOp(LogicalOpType::LOP_NODE_SCAN));
	EXPECT_FALSE(isSchemaMutatingOp(LogicalOpType::LOP_FILTER));
	EXPECT_FALSE(isSchemaMutatingOp(LogicalOpType::LOP_PROJECT));
}

// ============================================================================
// QueryPlan flag defaults
// ============================================================================

TEST(QueryPlanTest, DefaultFlags) {
	QueryPlan plan;
	EXPECT_EQ(plan.root, nullptr);
	EXPECT_FALSE(plan.mutatesData);
	EXPECT_FALSE(plan.mutatesSchema);
	EXPECT_TRUE(plan.cacheable);
	EXPECT_TRUE(plan.optimizable);
}

TEST(QueryPlanTest, CloneCopiesFlags) {
	QueryPlan plan;
	plan.mutatesData = true;
	plan.mutatesSchema = true;
	plan.cacheable = false;
	plan.optimizable = false;

	auto cloned = plan.clone();
	EXPECT_TRUE(cloned.mutatesData);
	EXPECT_TRUE(cloned.mutatesSchema);
	EXPECT_FALSE(cloned.cacheable);
	EXPECT_FALSE(cloned.optimizable);
	EXPECT_EQ(cloned.root, nullptr); // root was null, clone should be null too
}
