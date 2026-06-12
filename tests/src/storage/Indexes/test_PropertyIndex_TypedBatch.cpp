/**
 * @file test_PropertyIndex_TypedBatch.cpp
 * @brief Tests typed scalar batch insertion for property indexes.
 */

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "graph/core/Database.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/PropertyIndex.hpp"

namespace fs = std::filesystem;

class PropertyIndexTypedBatchTest : public ::testing::Test {
protected:
	void SetUp() override {
		const boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_propertyIndex_typed_" + to_string(uuid) + ".dat");
		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		propertyIndex = database->getQueryEngine()->getIndexManager()->getNodeIndexManager()->getPropertyIndex();
	}

	void TearDown() override {
		propertyIndex.reset();
		if (database) {
			database->close();
			database.reset();
		}
		std::error_code ec;
		fs::remove(testFilePath, ec);
	}

	static bool contains(const std::vector<int64_t> &values, int64_t expected) {
		return std::ranges::find(values, expected) != values.end();
	}

	fs::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::query::indexes::PropertyIndex> propertyIndex;
};

TEST_F(PropertyIndexTypedBatchTest, AddTypedPropertiesBatchIndexesRegisteredScalarKeys) {
	using Entry = graph::query::indexes::PropertyIndex::TypedPropertyEntry;

	propertyIndex->createIndex("name");
	propertyIndex->createIndex("age");
	propertyIndex->createIndex("score");
	propertyIndex->createIndex("active");

	std::vector<Entry> entries;
	entries.push_back(Entry{.entityId = 1, .key = "name", .type = graph::PropertyType::STRING, .stringValue = "Alice"});
	entries.push_back(Entry{.entityId = 2, .key = "age", .type = graph::PropertyType::INTEGER, .intValue = 42});
	entries.push_back(Entry{.entityId = 3, .key = "score", .type = graph::PropertyType::DOUBLE, .doubleValue = 7.5});
	entries.push_back(Entry{.entityId = 4, .key = "active", .type = graph::PropertyType::BOOLEAN, .boolValue = true});

	propertyIndex->addTypedPropertiesBatch(std::move(entries));

	EXPECT_TRUE(contains(propertyIndex->findExactMatch("name", graph::PropertyValue("Alice")), 1));
	EXPECT_TRUE(contains(propertyIndex->findExactMatch("age", graph::PropertyValue(int64_t{42})), 2));
	EXPECT_TRUE(contains(propertyIndex->findExactMatch("score", graph::PropertyValue(7.5)), 3));
	EXPECT_TRUE(contains(propertyIndex->findExactMatch("active", graph::PropertyValue(true)), 4));
}

TEST_F(PropertyIndexTypedBatchTest, AddTypedPropertiesBatchSkipsUnsupportedUnregisteredAndMismatchedValues) {
	using Entry = graph::query::indexes::PropertyIndex::TypedPropertyEntry;

	propertyIndex->createIndex("age");
	propertyIndex->addProperty(1, "age", int64_t{10});
	propertyIndex->createIndex("payload");

	std::vector<Entry> entries;
	entries.push_back(Entry{.entityId = 2, .key = "age", .type = graph::PropertyType::STRING, .stringValue = "wrong"});
	entries.push_back(Entry{.entityId = 3, .key = "missing", .type = graph::PropertyType::INTEGER, .intValue = 30});
	entries.push_back(Entry{.entityId = 4, .key = "payload", .type = graph::PropertyType::MAP});
	entries.push_back(Entry{.entityId = 5, .key = "", .type = graph::PropertyType::INTEGER, .intValue = 50});
	entries.push_back(Entry{.entityId = 6, .key = "age", .type = graph::PropertyType::INTEGER, .intValue = 20});

	EXPECT_NO_THROW(propertyIndex->addTypedPropertiesBatch(std::move(entries)));

	EXPECT_TRUE(contains(propertyIndex->findExactMatch("age", graph::PropertyValue(int64_t{10})), 1));
	EXPECT_TRUE(contains(propertyIndex->findExactMatch("age", graph::PropertyValue(int64_t{20})), 6));
	EXPECT_TRUE(propertyIndex->findExactMatch("age", graph::PropertyValue("wrong")).empty());
	EXPECT_TRUE(propertyIndex->findExactMatch("missing", graph::PropertyValue(int64_t{30})).empty());
	EXPECT_EQ(propertyIndex->getIndexedKeyType("payload"), graph::PropertyType::UNKNOWN);
}
