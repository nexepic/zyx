#include "DataManagerTestFixture.hpp"

#include "graph/storage/data/PropertyManager.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

std::unordered_map<std::string, PropertyValue> largePropertyMap(char fill) {
	std::unordered_map<std::string, PropertyValue> properties;
	for (int i = 0; i < 36; ++i) {
		properties.emplace("large_" + std::to_string(i), PropertyValue(std::string(128, fill)));
	}
	return properties;
}

} // namespace

TEST_F(DataManagerTest, PropertyBatchStorageHandlesEmptyInputAndExternalCleanup) {
	auto propertyManager = dataManager->getPropertyManager();

	std::vector<Node> empty;
	EXPECT_TRUE(propertyManager->storePropertiesBatch<Node>(empty).empty());

	Node node = createTestNode(dataManager, "BatchCleanupNode");
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {{"old", PropertyValue("value")}});
	Node stored = dataManager->getNode(node.getId());
	ASSERT_TRUE(stored.hasPropertyEntity());
	stored.clearProperties();

	std::vector<Node> nodes{stored};
	const auto changed = propertyManager->storePropertiesBatch<Node>(nodes);

	ASSERT_EQ(changed.size(), 1U);
	EXPECT_EQ(changed[0], 0U);
	EXPECT_FALSE(nodes[0].hasPropertyEntity());
	EXPECT_TRUE(nodes[0].getProperties().empty());
}

TEST_F(DataManagerTest, PropertyBatchStorageSplitsPropertyEntityAndBlobRows) {
	auto propertyManager = dataManager->getPropertyManager();

	Node small = createTestNode(dataManager, "BatchSmallNode");
	Node large = createTestNode(dataManager, "BatchLargeNode");
	dataManager->addNode(small);
	dataManager->addNode(large);
	small.setProperties({{"rank", PropertyValue(int64_t{7})}});
	large.setProperties(largePropertyMap('z'));

	std::vector<Node> nodes{small, large};
	const auto changed = propertyManager->storePropertiesBatch<Node>(nodes);

	ASSERT_EQ(changed.size(), 2U);
	EXPECT_NE(std::find(changed.begin(), changed.end(), size_t{0}), changed.end());
	EXPECT_NE(std::find(changed.begin(), changed.end(), size_t{1}), changed.end());
	EXPECT_EQ(nodes[0].getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);
	EXPECT_EQ(nodes[1].getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);
	EXPECT_TRUE(nodes[0].getProperties().empty());
	EXPECT_TRUE(nodes[1].getProperties().empty());
}

TEST_F(DataManagerTest, ColumnarPropertyBatchValidatesShapeAndSerializesCompoundValues) {
	auto propertyManager = dataManager->getPropertyManager();

	Node node = createTestNode(dataManager, "ColumnarCompoundNode");
	dataManager->addNode(node);
	std::vector<Node> noColumns{node};
	EXPECT_TRUE(propertyManager->storePropertiesColumnarBatch<Node>(noColumns, {}).empty());

	std::vector<Node> mismatchRows{node};
	EXPECT_THROW(propertyManager->storePropertiesColumnarBatch<Node>(
						 mismatchRows,
						 std::vector<BulkPropertyColumn>{{"rank", {PropertyValue(int64_t{1}), PropertyValue(int64_t{2})}}}),
				 std::invalid_argument);

	std::vector<PropertyValue> listValue{PropertyValue(int64_t{1}), PropertyValue("two")};
	PropertyValue::MapType mapValue;
	mapValue.emplace("inner", PropertyValue(int64_t{9}));

	std::vector<Node> compoundRows{node};
	const auto compoundChanged = propertyManager->storePropertiesColumnarBatch<Node>(
			compoundRows,
			std::vector<BulkPropertyColumn>{
					{"items", {PropertyValue(listValue)}},
					{"meta", {PropertyValue(mapValue)}}});

	ASSERT_EQ(compoundChanged.size(), 1U);
	EXPECT_EQ(compoundChanged[0], 0U);
	EXPECT_EQ(compoundRows[0].getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);
	const auto property = dataManager->getProperty(compoundRows[0].getPropertyEntityId());
	EXPECT_TRUE(property.isActive());
}

TEST_F(DataManagerTest, ColumnarPropertyBatchCleansExistingExternalStateAndFallsBackToBlob) {
	auto propertyManager = dataManager->getPropertyManager();

	Node existing = createTestNode(dataManager, "ColumnarCleanupNode");
	dataManager->addNode(existing);
	dataManager->addNodeProperties(existing.getId(), {{"old", PropertyValue("value")}});
	Node stored = dataManager->getNode(existing.getId());
	ASSERT_TRUE(stored.hasPropertyEntity());

	std::vector<Node> refreshedRows{stored};
	const auto refreshedChanged = propertyManager->storePropertiesColumnarBatch<Node>(
			refreshedRows,
			std::vector<BulkPropertyColumn>{{"new", {PropertyValue("value")}}});
	ASSERT_EQ(refreshedChanged.size(), 1U);
	EXPECT_EQ(refreshedRows[0].getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);

	Node blobNode = createTestNode(dataManager, "ColumnarBlobNode");
	dataManager->addNode(blobNode);
	std::vector<Node> blobRows{blobNode};
	const auto blobChanged = propertyManager->storePropertiesColumnarBatch<Node>(
			blobRows,
			std::vector<BulkPropertyColumn>{{"payload", {PropertyValue(std::string(5000, 'b'))}}});

	ASSERT_EQ(blobChanged.size(), 1U);
	EXPECT_EQ(blobRows[0].getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);
	const auto blobProperties = propertyManager->getPropertiesFromBlob(blobRows[0].getPropertyEntityId());
	ASSERT_TRUE(blobProperties.contains("payload"));
	EXPECT_EQ(blobProperties.at("payload"), PropertyValue(std::string(5000, 'b')));
}
