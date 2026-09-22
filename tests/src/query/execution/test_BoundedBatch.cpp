/**
 * @file test_BoundedBatch.cpp
 * @brief Tests for the shared bounded execution batch.
 */

#include <cstddef>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "graph/query/execution/BoundedBatch.hpp"

using graph::query::execution::BoundedBatch;

TEST(BoundedBatchTest, EnforcesRowsAndReusesStorageAfterClear) {
	BoundedBatch<std::string> batch(2U, 16U, [](const std::string &value) { return value.size(); });
	batch.reserve(10U);
	batch.append("abc");
	batch.append("de");
	EXPECT_EQ(batch.size(), 2U);
	EXPECT_EQ(batch.bytes(), 5U);
	EXPECT_THROW(batch.append("f"), std::runtime_error);
	batch.clear();
	EXPECT_TRUE(batch.empty());
	EXPECT_EQ(batch.bytes(), 0U);
	batch.append("123456789");
	EXPECT_EQ(batch.bytes(), 9U);
}

TEST(BoundedBatchTest, EnforcesByteBudgetAndRejectsOversizedValue) {
	BoundedBatch<std::string> batch(4U, 5U, [](const std::string &value) { return value.size(); });
	EXPECT_THROW(batch.append("123456"), std::runtime_error);
	batch.append("123");
	EXPECT_THROW(batch.append("456"), std::runtime_error);
}

TEST(BoundedBatchTest, RecalculatesBytesAfterRowsAreEnriched) {
	BoundedBatch<std::string> batch(2U, 8U, [](const std::string &value) { return value.size(); });
	batch.append("a");
	batch.values().front().append("bcdef");
	EXPECT_EQ(batch.bytes(), 1U);
	batch.recalculateBytes();
	EXPECT_EQ(batch.bytes(), 6U);
	batch.values().front().append("123");
	EXPECT_THROW(batch.recalculateBytes(), std::runtime_error);
}

TEST(BoundedBatchTest, RejectsInvalidConstruction) {
	EXPECT_THROW((BoundedBatch<int>(0U, 1U, [](const int &) { return size_t{1U}; })), std::invalid_argument);
	EXPECT_THROW((BoundedBatch<int>(1U, 0U, [](const int &) { return size_t{1U}; })), std::invalid_argument);
	EXPECT_THROW((BoundedBatch<int>(1U, 1U, {})), std::invalid_argument);
}
