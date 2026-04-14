/**
 * @file test_LogicalPlanBuilderFlags.cpp
 * @brief Unit tests for LogicalPlanBuilder mutation flag accessors.
 */

#include <gtest/gtest.h>

#include "graph/query/ir/LogicalPlanBuilder.hpp"

using namespace graph::query::ir;

TEST(LogicalPlanBuilderFlagsTest, InitialState) {
	LogicalPlanBuilder builder;
	EXPECT_FALSE(builder.mutatesData());
	EXPECT_FALSE(builder.mutatesSchema());
}

TEST(LogicalPlanBuilderFlagsTest, MarkDataMutation) {
	LogicalPlanBuilder builder;
	builder.markDataMutation();
	EXPECT_TRUE(builder.mutatesData());
	EXPECT_FALSE(builder.mutatesSchema());
}

TEST(LogicalPlanBuilderFlagsTest, MarkSchemaMutation) {
	LogicalPlanBuilder builder;
	builder.markSchemaMutation();
	EXPECT_FALSE(builder.mutatesData());
	EXPECT_TRUE(builder.mutatesSchema());
}

TEST(LogicalPlanBuilderFlagsTest, MarkBothMutations) {
	LogicalPlanBuilder builder;
	builder.markDataMutation();
	builder.markSchemaMutation();
	EXPECT_TRUE(builder.mutatesData());
	EXPECT_TRUE(builder.mutatesSchema());
}
