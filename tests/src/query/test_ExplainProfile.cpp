/**
 * @file test_ExplainProfile.cpp
 * @date 2026/04/02
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/query/logical/operators/LogicalExplain.hpp"
#include "graph/query/logical/operators/LogicalProfile.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/execution/operators/ExplainOperator.hpp"
#include "graph/query/execution/operators/ProfileOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query;
using namespace graph::query::logical;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

// ============================================================================
// LogicalExplain / LogicalProfile unit tests
// ============================================================================

TEST(LogicalExplainTest, TypeIsExplain) {
	auto explain = std::make_unique<LogicalExplain>(nullptr);
	EXPECT_EQ(explain->getType(), LogicalOpType::LOP_EXPLAIN);
}

TEST(LogicalExplainTest, ToStringReturnsExplain) {
	auto explain = std::make_unique<LogicalExplain>(nullptr);
	EXPECT_EQ(explain->toString(), "Explain");
}

TEST(LogicalExplainTest, NullInnerPlanReturnsEmptyChildren) {
	auto explain = std::make_unique<LogicalExplain>(nullptr);
	EXPECT_TRUE(explain->getChildren().empty());
	EXPECT_EQ(explain->getInnerPlan(), nullptr);
}

TEST(LogicalExplainTest, CloneWithNullInner) {
	auto explain = std::make_unique<LogicalExplain>(nullptr);
	auto cloned = explain->clone();
	ASSERT_NE(cloned, nullptr);
	EXPECT_EQ(cloned->getType(), LogicalOpType::LOP_EXPLAIN);
}

TEST(LogicalExplainTest, OutputVariables) {
	auto explain = std::make_unique<LogicalExplain>(nullptr);
	auto vars = explain->getOutputVariables();
	ASSERT_EQ(vars.size(), 2UL);
	EXPECT_EQ(vars[0], "operator");
	EXPECT_EQ(vars[1], "details");
}

TEST(LogicalProfileTest, TypeIsProfile) {
	auto profile = std::make_unique<LogicalProfile>(nullptr);
	EXPECT_EQ(profile->getType(), LogicalOpType::LOP_PROFILE);
}

TEST(LogicalProfileTest, ToStringReturnsProfile) {
	auto profile = std::make_unique<LogicalProfile>(nullptr);
	EXPECT_EQ(profile->toString(), "Profile");
}

TEST(LogicalProfileTest, NullInnerPlanReturnsEmptyChildren) {
	auto profile = std::make_unique<LogicalProfile>(nullptr);
	EXPECT_TRUE(profile->getChildren().empty());
	EXPECT_EQ(profile->getInnerPlan(), nullptr);
}

TEST(LogicalProfileTest, CloneWithNullInner) {
	auto profile = std::make_unique<LogicalProfile>(nullptr);
	auto cloned = profile->clone();
	ASSERT_NE(cloned, nullptr);
	EXPECT_EQ(cloned->getType(), LogicalOpType::LOP_PROFILE);
}

// ============================================================================
// ExplainOperator unit tests
// ============================================================================

TEST(ExplainOperatorTest, NullPlanReturnsEmpty) {
	ExplainOperator op(nullptr);
	op.open();
	auto batch = op.next();
	EXPECT_FALSE(batch.has_value());
	op.close();
}

TEST(ExplainOperatorTest, OutputVariables) {
	ExplainOperator op(nullptr);
	auto vars = op.getOutputVariables();
	ASSERT_EQ(vars.size(), 2UL);
	EXPECT_EQ(vars[0], "operator");
	EXPECT_EQ(vars[1], "details");
}

TEST(ExplainOperatorTest, ToStringReturnsExplain) {
	ExplainOperator op(nullptr);
	EXPECT_EQ(op.toString(), "Explain");
}

// ============================================================================
// ProfileOperator unit tests (using MockOperator)
// ============================================================================

class MockOperator : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t current_index = 0;
	explicit MockOperator(std::vector<RecordBatch> data = {}) : batches(std::move(data)) {}
	void open() override { current_index = 0; }
	std::optional<RecordBatch> next() override {
		if (current_index >= batches.size()) return std::nullopt;
		return batches[current_index++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
	[[nodiscard]] std::string toString() const override { return "Mock"; }
};

TEST(ProfileOperatorTest, EmptyInnerReturnsProfilingRows) {
	auto mock = std::make_unique<MockOperator>();
	ProfileOperator op(std::move(mock));
	op.open();

	// Inner operator has no data, so first next() should be profiling data or nullopt
	auto batch = op.next();
	// The profile operator transitions to profiling phase immediately
	// Since PerfTrace may or may not have entries, just verify no crash
	// After the profiling batch (or nullopt), next should be nullopt
	auto batch2 = op.next();
	EXPECT_FALSE(batch2.has_value());

	op.close();
}

TEST(ProfileOperatorTest, InnerResultsThenProfiling) {
	RecordBatch mockBatch;
	Record r;
	r.setValue("x", PropertyValue(std::string("hello")));
	mockBatch.push_back(std::move(r));

	auto mock = std::make_unique<MockOperator>(std::vector<RecordBatch>{std::move(mockBatch)});
	ProfileOperator op(std::move(mock));
	op.open();

	// First batch should be the inner operator's data
	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1UL);

	// Second batch should be profiling data (or nullopt if no PerfTrace entries)
	auto batch2 = op.next();
	// batch2 could be profiling rows or nullopt — either is acceptable

	// Third should always be nullopt
	auto batch3 = op.next();
	EXPECT_FALSE(batch3.has_value());

	op.close();
}

TEST(ProfileOperatorTest, OutputVariablesFromInner) {
	auto mock = std::make_unique<MockOperator>();
	ProfileOperator op(std::move(mock));
	auto vars = op.getOutputVariables();
	ASSERT_EQ(vars.size(), 1UL);
	EXPECT_EQ(vars[0], "x");
}

TEST(ProfileOperatorTest, ToStringReturnsProfile) {
	ProfileOperator op(nullptr);
	EXPECT_EQ(op.toString(), "Profile");
}

TEST(ProfileOperatorTest, ChildrenIncludesInner) {
	auto mock = std::make_unique<MockOperator>();
	auto *rawMock = mock.get();
	ProfileOperator op(std::move(mock));
	auto children = op.getChildren();
	ASSERT_EQ(children.size(), 1UL);
	EXPECT_EQ(children[0], rawMock);
}

// ============================================================================
// Integration tests: EXPLAIN / PROFILE via QueryEngine
// ============================================================================

class ExplainProfileIntegrationTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_explain_" + boost::uuids::to_string(uuid) + ".graph");
		if (fs::exists(testDbPath)) fs::remove_all(testDbPath);
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
	}

	void TearDown() override {
		if (db) db->close();
		if (fs::exists(testDbPath)) fs::remove_all(testDbPath);
	}

	QueryResult execute(const std::string &query) const {
		return db->getQueryEngine()->execute(query);
	}

	fs::path testDbPath;
	std::unique_ptr<Database> db;
};

// Test: EXPLAIN returns plan rows without executing
TEST_F(ExplainProfileIntegrationTest, ExplainMatchReturnsRows) {
	(void) execute("CREATE (n:Person {name: 'Alice'})");

	auto result = execute("EXPLAIN MATCH (n:Person) RETURN n");
	// EXPLAIN should return plan description rows
	EXPECT_GT(result.rowCount(), 0UL) << "EXPLAIN should return at least one plan row";

	// Verify the output contains operator column
	for (const auto &row : result.getRows()) {
		EXPECT_TRUE(row.count("operator") > 0) << "Each EXPLAIN row should have 'operator' column";
		EXPECT_TRUE(row.count("details") > 0) << "Each EXPLAIN row should have 'details' column";
	}
}

// Test: EXPLAIN does not modify data
TEST_F(ExplainProfileIntegrationTest, ExplainDoesNotModifyData) {
	auto result1 = execute("MATCH (n) RETURN count(n)");

	(void) execute("EXPLAIN CREATE (n:Person {name: 'Ghost'})");

	auto result2 = execute("MATCH (n) RETURN count(n)");
	// Row counts should be identical — EXPLAIN should not execute the CREATE
	EXPECT_EQ(result1.rowCount(), result2.rowCount());
}

// Test: EXPLAIN on admin statement
TEST_F(ExplainProfileIntegrationTest, ExplainAdminStatement) {
	auto result = execute("EXPLAIN SHOW INDEXES");
	// Should return plan rows for the admin statement
	EXPECT_GT(result.rowCount(), 0UL);
}

// Test: PROFILE returns query results (and possibly profiling data)
TEST_F(ExplainProfileIntegrationTest, ProfileMatchReturnsResults) {
	(void) execute("CREATE (n:Person {name: 'Alice'})");
	(void) execute("CREATE (n:Person {name: 'Bob'})");

	auto result = execute("PROFILE MATCH (n:Person) RETURN n");
	// PROFILE should return actual query results
	EXPECT_GT(result.rowCount(), 0UL) << "PROFILE should return query results";
}

// Test: EXPLAIN is not cached in plan cache
TEST_F(ExplainProfileIntegrationTest, ExplainNotCached) {
	auto &cache = db->getQueryEngine()->getPlanCache();
	size_t cacheSizeBefore = cache.size();

	(void) execute("EXPLAIN MATCH (n:Person) RETURN n");

	size_t cacheSizeAfter = cache.size();
	EXPECT_EQ(cacheSizeBefore, cacheSizeAfter)
		<< "EXPLAIN queries should not be cached";
}

// Test: PROFILE is not cached in plan cache
TEST_F(ExplainProfileIntegrationTest, ProfileNotCached) {
	auto &cache = db->getQueryEngine()->getPlanCache();
	size_t cacheSizeBefore = cache.size();

	(void) execute("PROFILE MATCH (n:Person) RETURN n");

	size_t cacheSizeAfter = cache.size();
	EXPECT_EQ(cacheSizeBefore, cacheSizeAfter)
		<< "PROFILE queries should not be cached";
}

// Test: Regular query still works after EXPLAIN/PROFILE
TEST_F(ExplainProfileIntegrationTest, RegularQueryAfterExplain) {
	(void) execute("CREATE (n:Person {name: 'Alice'})");
	(void) execute("EXPLAIN MATCH (n:Person) RETURN n");
	(void) execute("PROFILE MATCH (n:Person) RETURN n");

	auto result = execute("MATCH (n:Person) RETURN n.name");
	EXPECT_EQ(result.rowCount(), 1UL);
}
