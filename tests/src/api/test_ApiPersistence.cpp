/**
 * @file test_ApiPersistence.cpp
 * @author Nexepic
 * @date 2026/1/5
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

#include <unordered_map>
#include <variant>
#include <vector>

#include "ApiTestFixture.hpp"

// ============================================================================
// Open/Close/Reopen & Persistence
// ============================================================================

TEST_F(CppApiTest, OpenIfExistsOnExistingDb) {
	db->close();

	auto db2 = std::make_unique<zyx::Database>(dbPath);
	bool exists = db2->openIfExists();
	EXPECT_TRUE(exists);
	db2->close();

	db->open();
}

TEST_F(CppApiTest, OpenIfExistsOnNonExistentDb) {
	auto tempDir = fs::temp_directory_path();
	std::string fakePath = (tempDir / ("nonexistent_db_" + std::to_string(std::rand()))).string();

	auto db2 = std::make_unique<zyx::Database>(fakePath);
	bool exists = db2->openIfExists();
	EXPECT_FALSE(exists);

	if (fs::exists(fakePath))
		fs::remove_all(fakePath);
}

TEST_F(CppApiTest, SaveFlushesData) {
	db->createNode("SaveTest", {{"key", "value"}});
	db->save();

	auto res = db->execute("MATCH (n:SaveTest) RETURN n.key");
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.key");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "value");
}

TEST_F(CppApiTest, ExecuteAutoOpensDb) {
	db->close();

	auto tempDir = fs::temp_directory_path();
	std::string newPath = (tempDir / ("api_autoopen_" + std::to_string(std::rand()))).string();
	auto newDb = std::make_unique<zyx::Database>(newPath);

	auto res = newDb->execute("RETURN 42 AS val");
	EXPECT_TRUE(res.isSuccess());

	newDb->close();
	if (fs::exists(newPath))
		fs::remove_all(newPath);
}

TEST_F(CppApiTest, CloseAndReopenDatabase) {
	db->createNode("Persist", {{"key", "value"}});
	db->save();
	db->close();

	db = std::make_unique<zyx::Database>(dbPath);
	db->open();

	auto res = db->execute("MATCH (n:Persist) RETURN n.key");
	ASSERT_TRUE(res.isSuccess());
	ASSERT_TRUE(res.hasNext());
	res.next();
	auto val = res.get("n.key");
	EXPECT_TRUE(std::holds_alternative<std::string>(val));
	EXPECT_EQ(std::get<std::string>(val), "value");
}

TEST_F(CppApiTest, FlushCloseReopenCycle) {
	for (int i = 0; i < 3; i++) {
		db->createNode("Cycle", {{"iter", (int64_t)i}});
		db->save();
	}
	db->close();

	db = std::make_unique<zyx::Database>(dbPath);
	db->open();

	db->createNode("Cycle", {{"iter", (int64_t)3}});
	db->save();

	auto res = db->execute("MATCH (n:Cycle) RETURN n.iter ORDER BY n.iter");
	int count = 0;
	while (res.hasNext()) {
		res.next();
		count++;
	}
	EXPECT_EQ(count, 4);
}

TEST_F(CppApiTest, BulkCreateDeleteFlushReopen) {
	std::vector<std::unordered_map<std::string, zyx::Value>> propsList;
	for (int i = 0; i < 20; i++) {
		propsList.push_back({{"idx", (int64_t)i}});
	}
	db->createNodes("Bulk", propsList);
	db->save();

	(void)db->execute("MATCH (n:Bulk) WHERE n.idx < 5 DELETE n");
	db->save();

	db->close();
	db = std::make_unique<zyx::Database>(dbPath);
	db->open();

	auto res = db->execute("MATCH (n:Bulk) RETURN n.idx ORDER BY n.idx");
	int count = 0;
	while (res.hasNext()) {
		res.next();
		count++;
	}
	EXPECT_EQ(count, 15);
}

TEST_F(CppApiTest, ExecuteOnClosedDbCreatesNew) {
	auto tempDir = fs::temp_directory_path();
	std::string newPath = (tempDir / ("api_never_opened_" + std::to_string(std::rand()))).string();
	auto newDb = std::make_unique<zyx::Database>(newPath);
	auto res = newDb->execute("CREATE (n:AutoOpen {val: 1})");
	EXPECT_TRUE(res.isSuccess());

	auto queryRes = newDb->execute("MATCH (n:AutoOpen) RETURN n.val");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto val = queryRes.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));

	newDb->close();
	if (fs::exists(newPath))
		fs::remove_all(newPath);
}

TEST_F(CppApiTest, SaveOnClosedDb) {
	db->close();
	db = std::make_unique<zyx::Database>(dbPath);
	db->open();
	db->save();
	SUCCEED();
}
