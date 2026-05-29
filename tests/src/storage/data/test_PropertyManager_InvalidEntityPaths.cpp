#include "DataManagerSharedTestFixture.hpp"

#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/IndexEntityManager.hpp"
#include "graph/storage/data/PropertyManager.hpp"
#include "graph/storage/data/StateManager.hpp"

class PropertyManagerInvalidEntityPathsTest : public DataManagerSharedTest {};

TEST_F(PropertyManagerInvalidEntityPathsTest, DirectNodePropertyOperationsIgnoreMissingEntities) {
	auto propertyManager = dataManager->getPropertyManager();
	const int64_t missingNodeId = 987654321;

	EXPECT_TRUE(propertyManager->getEntityProperties<Node>(missingNodeId).empty());
	EXPECT_THROW(propertyManager->addEntityProperties<Node>(
					 missingNodeId,
					 {{"missing", PropertyValue(int64_t{1})}}),
				 std::runtime_error);
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<Node>(missingNodeId, "missing"));
	EXPECT_EQ(propertyManager->calculateEntityTotalPropertySize<Node>(missingNodeId), 0u);
}

TEST_F(PropertyManagerInvalidEntityPathsTest, DirectEdgePropertyOperationsIgnoreMissingEntities) {
	auto propertyManager = dataManager->getPropertyManager();
	const int64_t missingEdgeId = 987654322;

	EXPECT_TRUE(propertyManager->getEntityProperties<Edge>(missingEdgeId).empty());
	EXPECT_THROW(propertyManager->addEntityProperties<Edge>(
					 missingEdgeId,
					 {{"missing", PropertyValue(int64_t{1})}}),
				 std::runtime_error);
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<Edge>(missingEdgeId, "missing"));
	EXPECT_EQ(propertyManager->calculateEntityTotalPropertySize<Edge>(missingEdgeId), 0u);
}

TEST_F(PropertyManagerInvalidEntityPathsTest, DirectNodePropertyOperationsIgnoreInactiveEntities) {
	auto propertyManager = dataManager->getPropertyManager();
	Node node = createTestNode(dataManager, "InactivePropertyNode");
	dataManager->addNode(node);
	dataManager->deleteNode(node);

	EXPECT_TRUE(propertyManager->getEntityProperties<Node>(node.getId()).empty());
	EXPECT_THROW(propertyManager->addEntityProperties<Node>(
					 node.getId(),
					 {{"inactive", PropertyValue(int64_t{1})}}),
				 std::runtime_error);
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<Node>(node.getId(), "inactive"));
	EXPECT_EQ(propertyManager->calculateEntityTotalPropertySize<Node>(node.getId()), 0u);
}

TEST_F(PropertyManagerInvalidEntityPathsTest, DirectEdgePropertyOperationsIgnoreInactiveEntities) {
	auto propertyManager = dataManager->getPropertyManager();
	Node source = createTestNode(dataManager, "InactiveEdgeSource");
	Node target = createTestNode(dataManager, "InactiveEdgeTarget");
	dataManager->addNode(source);
	dataManager->addNode(target);
	Edge edge = createTestEdge(dataManager, source.getId(), target.getId(), "INACTIVE_EDGE");
	dataManager->addEdge(edge);
	dataManager->deleteEdge(edge);

	EXPECT_TRUE(propertyManager->getEntityProperties<Edge>(edge.getId()).empty());
	EXPECT_THROW(propertyManager->addEntityProperties<Edge>(
					 edge.getId(),
					 {{"inactive", PropertyValue(int64_t{1})}}),
				 std::runtime_error);
	EXPECT_NO_THROW(propertyManager->removeEntityProperty<Edge>(edge.getId(), "inactive"));
	EXPECT_EQ(propertyManager->calculateEntityTotalPropertySize<Edge>(edge.getId()), 0u);
}

TEST_F(PropertyManagerInvalidEntityPathsTest, InlinePropertySizeIncludesUnsavedNodeAndEdgeValues) {
	auto propertyManager = dataManager->getPropertyManager();

	Node node = createTestNode(dataManager, "InlineSizeNode");
	node.addProperty("inline_node", PropertyValue(int64_t{42}));
	dataManager->addNode(node);

	Node source = createTestNode(dataManager, "InlineSizeEdgeEndpoint");
	Node target = createTestNode(dataManager, "InlineSizeEdgeEndpoint");
	dataManager->addNode(source);
	dataManager->addNode(target);
	Edge edge = createTestEdge(dataManager, source.getId(), target.getId(), "INLINE_SIZE_EDGE");
	edge.addProperty("inline_edge", PropertyValue("value"));
	dataManager->addEdge(edge);

	EXPECT_GT(propertyManager->calculateEntityTotalPropertySize<Node>(node.getId()), 0u);
	EXPECT_GT(propertyManager->calculateEntityTotalPropertySize<Edge>(edge.getId()), 0u);
}

TEST_F(PropertyManagerInvalidEntityPathsTest, ManagersForEntitiesWithoutPropertiesIgnorePropertyCalls) {
	auto blobManager = dataManager->getBlobManager();
	auto indexManager = dataManager->getIndexEntityManager();
	auto stateManager = dataManager->getStateManager();

	EXPECT_TRUE(blobManager->getProperties(1).empty());
	EXPECT_NO_THROW(blobManager->addProperties(1, {{"ignored", PropertyValue(int64_t{1})}}));
	EXPECT_NO_THROW(blobManager->removeProperty(1, "ignored"));

	EXPECT_TRUE(indexManager->getProperties(1).empty());
	EXPECT_NO_THROW(indexManager->addProperties(1, {{"ignored", PropertyValue(int64_t{1})}}));
	EXPECT_NO_THROW(indexManager->removeProperty(1, "ignored"));

	EXPECT_TRUE(stateManager->getProperties(1).empty());
	EXPECT_NO_THROW(stateManager->addProperties(1, {{"ignored", PropertyValue(int64_t{1})}}));
	EXPECT_NO_THROW(stateManager->removeProperty(1, "ignored"));

	EXPECT_TRUE(blobManager->getBatch({987654001, 987654002}).empty());
	EXPECT_TRUE(indexManager->getBatch({987654003, 987654004}).empty());
	EXPECT_TRUE(stateManager->getBatch({987654005, 987654006}).empty());
}

TEST_F(PropertyManagerInvalidEntityPathsTest, TotalPropertySizeIgnoresDeletedExternalPropertyEntities) {
	auto propertyManager = dataManager->getPropertyManager();

	Node node = createTestNode(dataManager, "DeletedExternalPropertyNode");
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {{"external", PropertyValue(int64_t{7})}});
	Node storedNode = dataManager->getNode(node.getId());
	ASSERT_TRUE(storedNode.hasPropertyEntity());
	Property nodeProperty = dataManager->getProperty(storedNode.getPropertyEntityId());
	dataManager->deleteProperty(nodeProperty);
	EXPECT_EQ(propertyManager->calculateEntityTotalPropertySize<Node>(storedNode.getId()), 0u);

	Node source = createTestNode(dataManager, "DeletedExternalPropertyEdgeEndpoint");
	Node target = createTestNode(dataManager, "DeletedExternalPropertyEdgeEndpoint");
	dataManager->addNode(source);
	dataManager->addNode(target);
	Edge edge = createTestEdge(dataManager, source.getId(), target.getId(), "DELETED_EXTERNAL_PROPERTY_EDGE");
	dataManager->addEdge(edge);
	dataManager->addEdgeProperties(edge.getId(), {{"external", PropertyValue(int64_t{9})}});
	Edge storedEdge = dataManager->getEdge(edge.getId());
	ASSERT_TRUE(storedEdge.hasPropertyEntity());
	Property edgeProperty = dataManager->getProperty(storedEdge.getPropertyEntityId());
	dataManager->deleteProperty(edgeProperty);
	EXPECT_EQ(propertyManager->calculateEntityTotalPropertySize<Edge>(storedEdge.getId()), 0u);
}

TEST_F(PropertyManagerInvalidEntityPathsTest, UnknownExternalStorageTypeDoesNotReportProperties) {
	auto propertyManager = dataManager->getPropertyManager();

	Node node = createTestNode(dataManager, "UnknownExternalStorageNode");
	dataManager->addNode(node);
	Node storedNode = dataManager->getNode(node.getId());
	storedNode.setPropertyEntityId(123456, static_cast<PropertyStorageType>(255));
	EXPECT_FALSE(propertyManager->hasExternalProperty<Node>(storedNode, "missing"));

	Node source = createTestNode(dataManager, "UnknownExternalStorageEdgeEndpoint");
	Node target = createTestNode(dataManager, "UnknownExternalStorageEdgeEndpoint");
	dataManager->addNode(source);
	dataManager->addNode(target);
	Edge edge = createTestEdge(dataManager, source.getId(), target.getId(), "UNKNOWN_EXTERNAL_STORAGE_EDGE");
	dataManager->addEdge(edge);
	Edge storedEdge = dataManager->getEdge(edge.getId());
	storedEdge.setPropertyEntityId(123457, static_cast<PropertyStorageType>(255));
	EXPECT_FALSE(propertyManager->hasExternalProperty<Edge>(storedEdge, "missing"));
}
