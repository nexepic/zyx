#include <gtest/gtest.h>

#include "graph/query/execution/QueryExecutor.hpp"

using namespace graph::query;

TEST(QueryExecutorBasicTest, NullPlanReturnsEmptyResult) {
	auto result = QueryExecutor::execute(nullptr);

	EXPECT_TRUE(result.getColumns().empty());
	EXPECT_TRUE(result.getRows().empty());
	EXPECT_TRUE(result.isEmpty());
}
