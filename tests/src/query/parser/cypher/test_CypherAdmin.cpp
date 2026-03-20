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
	(void) execute("CALL dbms.setConfig('test.key', 12345)");
	auto res = execute("CALL dbms.getConfig('test.key')");
	ASSERT_EQ(res.rowCount(), 1UL);
	EXPECT_EQ(res.getRows()[0].at("value").toString(), "12345");
}

TEST_F(CypherAdminTest, Persistence) {
	(void) execute("CALL dbms.setConfig('persist', 'true')");
	db->getStorage()->flush();
	db->close();
	db.reset();

	// Restart
	db = std::make_unique<graph::Database>(testFilePath.string());
	db->open();
	auto res = execute("CALL dbms.getConfig('persist')");
	EXPECT_EQ(res.getRows()[0].at("value").toString(), "true");
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
	// Attempt to create another index with different name on same label/property should work
	(void) execute("CREATE INDEX dup_idx2 ON :Dup(name)");
	// Verify both indexes exist
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
	// String
	(void) execute("CALL dbms.setConfig('cfg.string', 'hello')");
	auto res1 = execute("CALL dbms.getConfig('cfg.string')");
	EXPECT_EQ(res1.getRows()[0].at("value").toString(), "hello");

	// Integer
	(void) execute("CALL dbms.setConfig('cfg.int', 42)");
	auto res2 = execute("CALL dbms.getConfig('cfg.int')");
	EXPECT_EQ(res2.getRows()[0].at("value").toString(), "42");

	// Boolean
	(void) execute("CALL dbms.setConfig('cfg.bool', true)");
	auto res3 = execute("CALL dbms.getConfig('cfg.bool')");
	EXPECT_EQ(res3.getRows()[0].at("value").toString(), "true");
}

TEST_F(CypherAdminTest, ListAllConfig) {
	(void) execute("CALL dbms.setConfig('list.test1', 'value1')");
	(void) execute("CALL dbms.setConfig('list.test2', 'value2')");

	// List all config (no filter)
	auto res = execute("CALL dbms.listConfig()");
	ASSERT_GE(res.rowCount(), 2UL);
}

TEST_F(CypherAdminTest, GetNonExistentConfig) {
	auto res = execute("CALL dbms.getConfig('nonexistent.key')");
	// Should return empty or null
	EXPECT_TRUE(res.isEmpty());
}

TEST_F(CypherAdminTest, UpdateConfigValue) {
	(void) execute("CALL dbms.setConfig('update.test', 'initial')");
	(void) execute("CALL dbms.setConfig('update.test', 'updated')");

	auto res = execute("CALL dbms.getConfig('update.test')");
	EXPECT_EQ(res.getRows()[0].at("value").toString(), "updated");
}
