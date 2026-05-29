#include <gtest/gtest.h>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/core/Node.hpp"
#include "graph/query/execution/NodePropertyColumnLoader.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;

namespace {

Node makeNode(int64_t id) {
	Node node;
	node.getMutableMetadata().id = id;
	return node;
}

} // namespace

TEST(NodePropertyColumnLoaderTest, EmptyRequestedPropertiesReturnEmptyColumns) {
	Node node = makeNode(1);
	node.addProperty("age", PropertyValue(int64_t{42}));
	NodePropertyColumnLoader loader(nullptr);

	auto columns = loader.loadColumns({node}, {}, {});

	EXPECT_TRUE(columns.empty());
}

TEST(NodePropertyColumnLoaderTest, InlinePropertiesPopulateOnlyRequestedColumns) {
	Node node = makeNode(1);
	node.addProperty("age", PropertyValue(int64_t{42}));
	node.addProperty("name", PropertyValue("Alice"));
	NodePropertyColumnLoader loader(nullptr);

	auto columns = loader.loadColumns({node}, {}, {"age"});

	ASSERT_EQ(columns.size(), 1U);
	ASSERT_TRUE(columns.contains("age"));
	EXPECT_FALSE(columns.contains("name"));
	ASSERT_EQ(columns["age"].size(), 1U);
	ASSERT_TRUE(columns["age"][0].has_value());
	EXPECT_EQ(columns["age"][0].value(), PropertyValue(int64_t{42}));
}

TEST(NodePropertyColumnLoaderTest, DuplicateRequestedPropertiesProduceSingleColumn) {
	Node node = makeNode(1);
	node.addProperty("age", PropertyValue(int64_t{42}));
	NodePropertyColumnLoader loader(nullptr);

	auto columns = loader.loadColumns({node}, {}, {"age", "age"});

	ASSERT_EQ(columns.size(), 1U);
	ASSERT_TRUE(columns.contains("age"));
	ASSERT_EQ(columns["age"].size(), 1U);
	ASSERT_TRUE(columns["age"][0].has_value());
	EXPECT_EQ(columns["age"][0].value(), PropertyValue(int64_t{42}));
}

TEST(NodePropertyColumnLoaderTest, MissingPropertiesAndUnselectedRowsReturnNullopt) {
	Node first = makeNode(1);
	first.addProperty("age", PropertyValue(int64_t{42}));
	Node second = makeNode(2);
	second.addProperty("age", PropertyValue(int64_t{7}));
	Node third = makeNode(3);
	NodePropertyColumnLoader loader(nullptr);

	auto columns = loader.loadColumns({first, second, third}, {1, 0, 1}, {"age", "score"});

	ASSERT_EQ(columns.size(), 2U);
	ASSERT_EQ(columns["age"].size(), 3U);
	ASSERT_EQ(columns["score"].size(), 3U);
	EXPECT_EQ(columns["age"][0], std::optional<PropertyValue>(PropertyValue(int64_t{42})));
	EXPECT_FALSE(columns["age"][1].has_value());
	EXPECT_FALSE(columns["age"][2].has_value());
	EXPECT_FALSE(columns["score"][0].has_value());
	EXPECT_FALSE(columns["score"][1].has_value());
	EXPECT_FALSE(columns["score"][2].has_value());
}

TEST(NodePropertyColumnLoaderTest, InvalidSelectionVectorSizeReturnsEmptyColumns) {
	Node node = makeNode(1);
	node.addProperty("age", PropertyValue(int64_t{42}));
	NodePropertyColumnLoader loader(nullptr);

	auto columns = loader.loadColumns({node}, {1, 0}, {"age"});

	EXPECT_TRUE(columns.empty());
}

TEST(NodePropertyColumnLoaderTest, SkipsInvalidInactiveAndUnhelpfulNodeRows) {
	Node missingId = makeNode(0);
	missingId.addProperty("age", PropertyValue(int64_t{99}));
	Node inactive = makeNode(2);
	inactive.addProperty("age", PropertyValue(int64_t{100}));
	inactive.markInactive();
	Node selectedOut = makeNode(3);
	selectedOut.addProperty("age", PropertyValue(int64_t{101}));
	NodePropertyColumnLoader loader(nullptr);

	auto columns = loader.loadColumns({missingId, inactive, selectedOut}, {1, 1, 0}, {"age"});

	ASSERT_EQ(columns.size(), 1U);
	ASSERT_EQ(columns["age"].size(), 3U);
	EXPECT_FALSE(columns["age"][0].has_value());
	EXPECT_FALSE(columns["age"][1].has_value());
	EXPECT_FALSE(columns["age"][2].has_value());
}

TEST(NodePropertyColumnLoaderTest, MetadataBatchValidatesSelectionAndSkipsInactiveRows) {
	NodeMetadataBatch batch;
	batch.appendDefault();
	batch.nodeIds[0] = 1;
	batch.active[0] = 1;
	batch.propertyStorageTypes[0] = PropertyStorageType::PROPERTY_ENTITY;
	batch.propertyEntityIds[0] = 0;
	batch.appendDefault();
	batch.nodeIds[1] = 2;
	batch.active[1] = 0;
	batch.propertyStorageTypes[1] = PropertyStorageType::PROPERTY_ENTITY;
	batch.propertyEntityIds[1] = 42;
	batch.appendDefault();
	batch.nodeIds[2] = 0;
	batch.active[2] = 1;
	batch.propertyStorageTypes[2] = PropertyStorageType::PROPERTY_ENTITY;
	batch.propertyEntityIds[2] = 43;

	NodePropertyColumnLoader loader(nullptr);

	EXPECT_TRUE(loader.loadColumns(batch, {}, {}).empty());
	EXPECT_TRUE(loader.loadColumns(batch, {1, 0}, {"age"}).empty());

	auto columns = loader.loadColumns(batch, {1, 1, 1}, {"age", "age"});
	ASSERT_EQ(columns.size(), 1U);
	ASSERT_TRUE(columns.contains("age"));
	ASSERT_EQ(columns["age"].size(), 3U);
	EXPECT_FALSE(columns["age"][0].has_value());
	EXPECT_FALSE(columns["age"][1].has_value());
	EXPECT_FALSE(columns["age"][2].has_value());
}

class NodePropertyColumnLoaderStorageTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_property_column_loader_" + boost::uuids::to_string(uuid) + ".zyx");
		if (fs::exists(testDbPath)) {
			fs::remove_all(testDbPath);
		}
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
	}

	void TearDown() override {
		dm.reset();
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		if (fs::exists(testDbPath)) {
			fs::remove_all(testDbPath, ec);
		}
	}

	fs::path testDbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
};

TEST_F(NodePropertyColumnLoaderStorageTest, ExternalPropertyEntityValuesPopulateRequestedColumns) {
	Node node(1, 0);
	dm->addNode(node);
	dm->addNodeProperties(1, {{"age", PropertyValue(int64_t{42})}, {"name", PropertyValue("Alice")}});
	Node stored = dm->getNode(1);
	ASSERT_TRUE(stored.hasPropertyEntity());
	ASSERT_EQ(stored.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);
	NodePropertyColumnLoader loader(dm);

	auto columns = loader.loadColumns({stored}, {}, {"age"});

	ASSERT_EQ(columns.size(), 1U);
	ASSERT_TRUE(columns.contains("age"));
	ASSERT_EQ(columns["age"].size(), 1U);
	ASSERT_TRUE(columns["age"][0].has_value());
	EXPECT_EQ(columns["age"][0].value(), PropertyValue(int64_t{42}));
	EXPECT_FALSE(columns.contains("name"));
}

TEST_F(NodePropertyColumnLoaderStorageTest, MetadataBatchExternalPropertiesPopulateRequestedColumns) {
	Node node(1, 0);
	dm->addNode(node);
	dm->addNodeProperties(1, {{"age", PropertyValue(int64_t{42})}, {"name", PropertyValue("Alice")}});
	Node stored = dm->getNode(1);
	ASSERT_TRUE(stored.hasPropertyEntity());
	ASSERT_EQ(stored.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);

	NodeMetadataBatch metadataBatch;
	metadataBatch.appendDefault();
	metadataBatch.setFromNode(0, stored);
	NodePropertyColumnLoader loader(dm);

	auto columns = loader.loadColumns(metadataBatch, {}, {"age"});

	ASSERT_EQ(columns.size(), 1U);
	ASSERT_TRUE(columns.contains("age"));
	ASSERT_EQ(columns["age"].size(), 1U);
	ASSERT_TRUE(columns["age"][0].has_value());
	EXPECT_EQ(columns["age"][0].value(), PropertyValue(int64_t{42}));
	EXPECT_FALSE(columns.contains("name"));
}

TEST_F(NodePropertyColumnLoaderStorageTest, BlobBackedPropertiesFallbackToDirectLoading) {
	std::string largeValue(512, 'x');
	Node node(1, 0);
	dm->addNode(node);
	dm->addNodeProperties(1, {{"payload", PropertyValue(largeValue)}, {"age", PropertyValue(int64_t{42})}});
	Node stored = dm->getNode(1);
	ASSERT_TRUE(stored.hasPropertyEntity());
	ASSERT_EQ(stored.getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);
	NodePropertyColumnLoader loader(dm);

	auto columns = loader.loadColumns({stored}, {}, {"payload", "age"});

	ASSERT_EQ(columns.size(), 2U);
	ASSERT_EQ(columns["payload"].size(), 1U);
	ASSERT_EQ(columns["age"].size(), 1U);
	ASSERT_TRUE(columns["payload"][0].has_value());
	ASSERT_TRUE(columns["age"][0].has_value());
	EXPECT_EQ(columns["payload"][0].value(), PropertyValue(largeValue));
	EXPECT_EQ(columns["age"][0].value(), PropertyValue(int64_t{42}));
}

TEST_F(NodePropertyColumnLoaderStorageTest, MetadataBatchBlobRowsFallbackToDirectLoading) {
	std::string largeValue(512, 'z');
	Node node(1, 0);
	dm->addNode(node);
	dm->addNodeProperties(1, {{"payload", PropertyValue(largeValue)}});
	Node stored = dm->getNode(1);
	ASSERT_TRUE(stored.hasPropertyEntity());
	ASSERT_EQ(stored.getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);

	NodeMetadataBatch metadataBatch;
	metadataBatch.appendDefault();
	metadataBatch.setFromNode(0, stored);

	NodePropertyColumnLoader loader(dm);
	auto columns = loader.loadColumns(metadataBatch, {}, {"payload"});

	ASSERT_EQ(columns.size(), 1U);
	ASSERT_TRUE(columns.contains("payload"));
	ASSERT_EQ(columns["payload"].size(), 1U);
	ASSERT_TRUE(columns["payload"][0].has_value());
	EXPECT_EQ(columns["payload"][0].value(), PropertyValue(largeValue));
}

TEST_F(NodePropertyColumnLoaderStorageTest, MissingBulkResultFallbackPreservesValues) {
	Node node(1, 0);
	dm->addNode(node);
	dm->addNodeProperties(1, {{"age", PropertyValue(int64_t{42})}});
	Node stored = dm->getNode(1);
	ASSERT_TRUE(stored.hasPropertyEntity());
	ASSERT_EQ(stored.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);

	Property property = dm->getProperty(stored.getPropertyEntityId());
	dm->deleteProperty(property);
	NodePropertyColumnLoader loader(dm);

	auto columns = loader.loadColumns({stored}, {}, {"age"});

	ASSERT_EQ(columns.size(), 1U);
	ASSERT_EQ(columns["age"].size(), 1U);
	EXPECT_FALSE(columns["age"][0].has_value());
}
