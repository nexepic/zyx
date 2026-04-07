/**
 * @file test_QueryResult.cpp
 * @brief Focused unit tests for query::QueryResult data container behavior.
 */

#include <gtest/gtest.h>

#include "graph/query/api/QueryResult.hpp"

using namespace graph::query;

TEST(QueryResultTest, NotificationsAreStoredAndReturned) {
	QueryResult result;

	result.addNotification("slow query");
	result.addNotification("used fallback plan");

	const auto &notifications = result.getNotifications();
	ASSERT_EQ(notifications.size(), 2U);
	EXPECT_EQ(notifications[0], "slow query");
	EXPECT_EQ(notifications[1], "used fallback plan");
}

TEST(QueryResultTest, DefaultStateAndBasicAccessors) {
	QueryResult result;

	EXPECT_TRUE(result.isEmpty());
	EXPECT_EQ(result.rowCount(), 0U);
	EXPECT_EQ(result.getDuration(), 0.0);
	EXPECT_TRUE(result.getNotifications().empty());

	QueryResult::Row row;
	row.emplace("x", ResultValue(int64_t(42)));
	result.addRow(std::move(row));
	result.setColumns({"x"});
	result.setDuration(12.5);

	ASSERT_FALSE(result.isEmpty());
	EXPECT_EQ(result.rowCount(), 1U);
	ASSERT_EQ(result.getColumns().size(), 1U);
	EXPECT_EQ(result.getColumns()[0], "x");
	EXPECT_EQ(result.getDuration(), 12.5);
}
