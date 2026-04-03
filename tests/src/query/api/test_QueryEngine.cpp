/**
 * @file test_QueryEngine.cpp
 * @author Nexepic
 * @date 2026/1/16
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
#include <memory>

#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/FileStorage.hpp"

// We need a dummy operator for direct execution testing
#include "graph/query/execution/operators/LimitOperator.hpp" // Any simple operator

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query;

class QueryEngineTest : public ::testing::Test {
protected:
	std::filesystem::path testFilePath;
	std::shared_ptr<storage::FileStorage> storage;
	std::unique_ptr<QueryEngine> engine;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_engine_" + to_string(uuid) + ".db");

		storage =
				std::make_shared<storage::FileStorage>(testFilePath.string(), 4096, storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();

		engine = std::make_unique<QueryEngine>(storage);
	}

	void TearDown() override {
		if (storage)
			storage->close();
		if (fs::exists(testFilePath))
			fs::remove(testFilePath);
	}
};

// ============================================================================
// Initialization & Accessors
// ============================================================================

TEST_F(QueryEngineTest, Initialization) {
	EXPECT_NE(engine->getIndexManager(), nullptr);
	EXPECT_NE(engine->getConstraintManager(), nullptr);

	// Verify QueryBuilder creation
	auto qb = engine->query();
	// Just verify it returns a valid object (by calling a method that returns ref)
	qb.skip_(0);

	// Cover inline accessors in QueryEngine.hpp
	auto &cache = engine->getPlanCache();
	(void)cache;
	const QueryEngine &constEngine = *engine;
	const auto &constCache = constEngine.getPlanCache();
	(void)constCache;
}

TEST_F(QueryEngineTest, SetThreadPoolAccessor) {
	graph::concurrent::ThreadPool pool(2);
	engine->setThreadPool(&pool);
	EXPECT_NE(engine->getIndexManager(), nullptr);
}

// ============================================================================
// Parsing & Language Support
// ============================================================================

TEST_F(QueryEngineTest, Execute_Cypher_Valid) {
	// We assume CypherParserImpl is implemented and can parse simple queries.
	// If not fully implemented, we might get a "Not Implemented" exception from parser,
	// but here we check that the routing logic works.

	// Simple query: RETURN 1
	// The executor should run it.
	try {
		auto result = engine->execute("RETURN 1");
		// We don't check result content here (depends on parser/executor logic),
		// just that it doesn't crash or throw "Unsupported language".
	} catch (const std::exception &e) {
		// If parser throws SyntaxError, that's fine, it means engine routed it correctly.
		// We only fail if it's "Unsupported query language"
		std::string msg = e.what();
		EXPECT_NE(msg, "Unsupported query language.");
	}
}

TEST_F(QueryEngineTest, Execute_UnsupportedLanguage) {
	// Cast to invalid enum
	Language invalidLang = static_cast<Language>(999);

	EXPECT_THROW(
			{ engine->execute("SELECT * FROM table", invalidLang); },
			std::runtime_error); // "Unsupported query language."
}

TEST_F(QueryEngineTest, GetParser_Caching) {
	// The engine caches parsers. We can verify this implicitly by calling execute twice.
	// White-box: getParser() checks `parsers_.contains(lang)`.

	EXPECT_NO_THROW(engine->execute("RETURN 1")); // Creates parser
	EXPECT_NO_THROW(engine->execute("RETURN 2")); // Reuses parser
}

// ============================================================================
// Direct Plan Execution
// ============================================================================

TEST_F(QueryEngineTest, Execute_DirectPlan) {
	// Create a dummy plan manually (e.g., Unwind)
	// UNWIND [1, 2, 3] AS x
	std::vector<PropertyValue> list = {PropertyValue(1), PropertyValue(2), PropertyValue(3)};

	// We need access to planner to create ops easily, but engine hides it.
	// We can use QueryBuilder to build a plan, then pass it to execute(plan).

	auto plan = engine->query().unwind(list, "x").build();
	ASSERT_NE(plan, nullptr);

	// Execute the pre-built plan
	auto result = engine->execute(std::move(plan));

	// Verify result
	// Assuming QueryExecutor works and UNWIND works
	// We can iterate the result
	int count = 0;
	// Use range-based for loop which works for most standard-compliant containers
	for ([[maybe_unused]] const auto &record: result.getRows()) {
		count++;
	}
	EXPECT_EQ(count, 3);
}
