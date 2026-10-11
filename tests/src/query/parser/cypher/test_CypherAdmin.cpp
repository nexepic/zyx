/**
 * @file test_CypherAdmin.cpp
 * @author Nexepic
 * @date 2026/1/29
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

#include "QueryTestFixture.hpp"

class CypherAdminTest : public QueryTestFixture {};

TEST_F(CypherAdminTest, IndexCreationAndPushdown) {
	(void) execute("CREATE INDEX ON :User(id)");
	(void) execute("CREATE (u1:User {id: 1, name: 'One'})");
	(void) execute("CREATE (u2:User {id: 2, name: 'Two'})");

	auto res = execute("SHOW INDEXES");
	ASSERT_GE(res.rowCount(), 1UL);

	auto resQuery = execute("MATCH (n:User {id: 2}) RETURN n");
	ASSERT_EQ(resQuery.rowCount(), 1UL);
	EXPECT_EQ(resQuery.getRows()[0].at("n").asNode().getProperties().at("name").toString(), "Two");
}

TEST_F(CypherAdminTest, DropIndex) {
	(void) execute("CREATE INDEX ON :Tag(name)");
	(void) execute("DROP INDEX ON :Tag(name)");
	// Checking removal is implicit via SHOW INDEXES logic or internal state check
}

TEST_F(CypherAdminTest, NamedIndexLifecycle) {
	(void) execute("CREATE INDEX user_age_idx FOR (n:User) ON (n.age)");
	auto resShow = execute("SHOW INDEXES");
	bool found = false;
	for (auto &r: resShow.getRows())
		if (r.at("name").toString() == "user_age_idx")
			found = true;
	EXPECT_TRUE(found);

	(void) execute("DROP INDEX user_age_idx");
}

TEST_F(CypherAdminTest, SystemConfigFlow) {
	EXPECT_THROW(static_cast<void>(execute("CALL dbms.setConfig('test.key', 12345)")), std::invalid_argument);
}

TEST_F(CypherAdminTest, Persistence) {
	static_cast<void>(execute("CALL dbms.setConfig('query.timeout_ms', 123)"));
	const auto result = execute("CALL dbms.getConfig('query.timeout_ms')");
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), graph::PropertyValue(int64_t{123}));
}

// --- Additional Index Tests ---

TEST_F(CypherAdminTest, ShowIndexesWhenEmpty) {
	auto res = execute("SHOW INDEXES");
	// Should return empty result or no indexes
	EXPECT_TRUE(res.isEmpty() || res.rowCount() == 0);
}

TEST_F(CypherAdminTest, ShowIndexesAfterCreatingMultiple) {
	(void) execute("CREATE INDEX ON :IdxA(prop1)");
	(void) execute("CREATE INDEX ON :IdxB(prop2)");
	(void) execute("CREATE INDEX idx_c ON :IdxC(prop3)");

	auto res = execute("SHOW INDEXES");
	ASSERT_GE(res.rowCount(), 3UL);
}

TEST_F(CypherAdminTest, DropIndexByName) {
	(void) execute("CREATE INDEX my_drop_idx ON :DropTest(prop)");
	(void) execute("DROP INDEX my_drop_idx");

	auto res = execute("SHOW INDEXES");
	bool found = false;
	for (const auto &row: res.getRows()) {
		if (row.at("name").toString() == "my_drop_idx") {
			found = true;
			break;
		}
	}
	EXPECT_FALSE(found);
}

TEST_F(CypherAdminTest, DropIndexByDefinition) {
	(void) execute("CREATE INDEX ON :DropDefTest(key)");
	(void) execute("DROP INDEX ON :DropDefTest(key)");
	// Should succeed without error
}

TEST_F(CypherAdminTest, CreateDuplicateIndex) {
	(void) execute("CREATE INDEX dup_idx1 ON :Dup(name)");
	// A second public name cannot own the same physical schema.
	EXPECT_THROW((void)execute("CREATE INDEX dup_idx2 ON :Dup(name)"), std::exception);
	// Verify that the first definition remains intact after the rejected DDL.
	auto res = execute("SHOW INDEXES");
	int count = 0;
	for (const auto &row: res.getRows()) {
		if (row.at("label").toString() == "Dup" && row.at("properties").toString() == "name") {
			count++;
		}
	}
	EXPECT_GE(count, 1);
}

// --- Additional Configuration Tests ---

TEST_F(CypherAdminTest, SetConfigDifferentTypes) {
	EXPECT_THROW(static_cast<void>(execute("CALL dbms.setConfig('cfg.string', 'hello')")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL dbms.setConfig('cfg.int', 42)")), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(execute("CALL dbms.setConfig('cfg.bool', true)")), std::invalid_argument);
}

TEST_F(CypherAdminTest, ListAllConfig) {
	const auto result = execute("CALL dbms.listConfig()");
	EXPECT_EQ(result.getColumns(), (std::vector<std::string>{"key", "value"}));
	EXPECT_EQ(result.rowCount(), 6U);
}

TEST_F(CypherAdminTest, QueryRuntimeStatisticsCanBeDisabledWithoutHidingStorageStatistics) {
	static_cast<void>(execute("CALL dbms.resetStats()"));
	static_cast<void>(execute("RETURN 42 AS answer"));
	const auto enabledStatistics = execute("CALL dbms.showStats()");
	bool sawQueryStatistics = false;
	bool sawCompleted = false;
	for (const auto &row: enabledStatistics.getRows()) {
		if (row.at("category").asPrimitive().toString() == "query") {
			sawQueryStatistics = true;
			if (row.at("metric").asPrimitive().toString() == "completed") {
				sawCompleted = true;
				EXPECT_EQ(row.at("value").asPrimitive(), graph::PropertyValue(int64_t{1}));
			}
		}
	}
	EXPECT_TRUE(sawQueryStatistics);
	EXPECT_TRUE(sawCompleted);

	static_cast<void>(execute("CALL dbms.setConfig('stats.enabled', false)"));
	EXPECT_EQ(execute("CALL dbms.getConfig('stats.enabled')").getRows().front().at("value").asPrimitive(),
			  graph::PropertyValue(false));
	const auto disabledStatistics = execute("CALL dbms.showStats()");
	for (const auto &row: disabledStatistics.getRows()) {
		EXPECT_NE(row.at("category").asPrimitive().toString(), "query");
	}

	static_cast<void>(execute("CALL dbms.setConfig('stats.enabled', true)"));
	const auto restoredStatistics = execute("CALL dbms.showStats()");
	bool restoredQueryStatistics = false;
	for (const auto &row: restoredStatistics.getRows()) {
		restoredQueryStatistics = restoredQueryStatistics || row.at("category").asPrimitive().toString() == "query";
	}
	EXPECT_TRUE(restoredQueryStatistics);
}

TEST_F(CypherAdminTest, GetNonExistentConfig) {
	EXPECT_THROW(static_cast<void>(execute("CALL dbms.getConfig('nonexistent.key')")), std::invalid_argument);
}

TEST_F(CypherAdminTest, UpdateConfigValue) {
	static_cast<void>(execute("CALL dbms.setConfig('query.max_memory_mb', 64)"));
	static_cast<void>(execute("CALL dbms.setConfig('query.max_memory_mb', 128)"));
	const auto result = execute("CALL dbms.getConfig('query.max_memory_mb')");
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), graph::PropertyValue(int64_t{128}));
}

TEST_F(CypherAdminTest, SlowQueryAdministrationIsBoundedAndDoesNotLogItsOwnInspection) {
	static_cast<void>(execute("CALL dbms.setConfig('query.slow_log.enabled', true)"));
	static_cast<void>(execute("CALL dbms.setConfig('query.slow_log.threshold_ms', 0)"));
	static_cast<void>(execute("RETURN 42 AS answer"));

	const auto records = execute("CALL dbms.listSlowQueries()");
	EXPECT_EQ(records.getColumns(), (std::vector<std::string>{"sequence", "logicalPlan", "elapsedMs", "succeeded"}));
	ASSERT_GE(records.rowCount(), 2U);
	bool observedSuccess = false;
	for (const auto &row: records.getRows()) {
		EXPECT_NE(row.at("logicalPlan").asPrimitive().toString(), "CallProcedure(dbms.listSlowQueries)");
		observedSuccess = observedSuccess || std::get<bool>(row.at("succeeded").asPrimitive().getVariant());
	}
	EXPECT_TRUE(observedSuccess);

	const auto cleared = execute("CALL dbms.clearSlowQueries()");
	ASSERT_EQ(cleared.rowCount(), 1U);
	EXPECT_EQ(cleared.getRows().front().at("result").asPrimitive().toString(), "Slow query records cleared");
	EXPECT_TRUE(execute("CALL dbms.listSlowQueries()").isEmpty());
}

TEST_F(CypherAdminTest, PreservesNativeAdministrationProcedureResultColumnsAndLogicalValueTypes) {
	const auto expectPrimitiveTypes = [](const graph::query::QueryResult &result,
													 const std::vector<graph::PropertyType> &expectedTypes) {
		ASSERT_FALSE(result.isEmpty());
		ASSERT_EQ(result.getRows().front().size(), expectedTypes.size());
		for (size_t index = 0U; index < expectedTypes.size(); ++index) {
			EXPECT_EQ(result.getRows().front().at(result.getColumns()[index]).asPrimitive().getType(), expectedTypes[index]);
		}
	};

	const auto listed = execute("CALL dbms.listConfig()");
	ASSERT_EQ(listed.getColumns(), (std::vector<std::string>{"key", "value"}));
	ASSERT_FALSE(listed.isEmpty());
	for (const auto &row: listed.getRows()) {
		EXPECT_EQ(row.at("key").asPrimitive().getType(), graph::PropertyType::STRING);
		const graph::PropertyType valueType = row.at("value").asPrimitive().getType();
		EXPECT_TRUE(valueType == graph::PropertyType::INTEGER || valueType == graph::PropertyType::BOOLEAN);
	}

	const auto fetched = execute("CALL dbms.getConfig('query.timeout_ms')");
	ASSERT_EQ(fetched.getColumns(), (std::vector<std::string>{"key", "value"}));
	expectPrimitiveTypes(fetched, {graph::PropertyType::STRING, graph::PropertyType::INTEGER});

	const auto configured = execute("CALL dbms.setConfig('query.timeout_ms', 77)");
	ASSERT_EQ(configured.getColumns(), (std::vector<std::string>{"key", "value", "status"}));
	expectPrimitiveTypes(configured,
						 {graph::PropertyType::STRING, graph::PropertyType::INTEGER, graph::PropertyType::STRING});

	const auto statistics = execute("CALL dbms.showStats()");
	ASSERT_EQ(statistics.getColumns(), (std::vector<std::string>{"category", "metric", "value"}));
	expectPrimitiveTypes(statistics,
						 {graph::PropertyType::STRING, graph::PropertyType::STRING, graph::PropertyType::INTEGER});

	const auto reset = execute("CALL dbms.resetStats()");
	ASSERT_EQ(reset.getColumns(), (std::vector<std::string>{"result"}));
	expectPrimitiveTypes(reset, {graph::PropertyType::STRING});

	(void) execute("CALL dbms.setConfig('query.slow_log.enabled', true)");
	(void) execute("CALL dbms.setConfig('query.slow_log.threshold_ms', 0)");
	(void) execute("RETURN 42 AS answer");
	const auto slowQueries = execute("CALL dbms.listSlowQueries()");
	ASSERT_EQ(slowQueries.getColumns(), (std::vector<std::string>{"sequence", "logicalPlan", "elapsedMs", "succeeded"}));
	expectPrimitiveTypes(slowQueries, {graph::PropertyType::INTEGER, graph::PropertyType::STRING,
									 graph::PropertyType::INTEGER, graph::PropertyType::BOOLEAN});

	const auto cleared = execute("CALL dbms.clearSlowQueries()");
	ASSERT_EQ(cleared.getColumns(), (std::vector<std::string>{"result"}));
	expectPrimitiveTypes(cleared, {graph::PropertyType::STRING});
}
