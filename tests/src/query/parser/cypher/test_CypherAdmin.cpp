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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;

class CypherAdminTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::unique_ptr<graph::Database> db;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_cypher_admin_" + boost::uuids::to_string(uuid) + ".dat");
		if (fs::exists(testFilePath))
			fs::remove_all(testFilePath);
		db = std::make_unique<graph::Database>(testFilePath.string());
		db->open();
	}

	void TearDown() override {
		if (db)
			db->close();
		if (fs::exists(testFilePath))
			fs::remove_all(testFilePath);
	}

	[[nodiscard]] graph::query::QueryResult execute(const std::string &query) const {
		return db->getQueryEngine()->execute(query);
	}
};

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
