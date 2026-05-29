#include <gtest/gtest.h>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <memory>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/data/DirtyEntityInfo.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;

namespace {

class NodeMetadataColumnLoaderStorageTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_node_metadata_column_loader_" + boost::uuids::to_string(uuid) + ".zyx");
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		userLabel = dm->getOrCreateTokenId("User");
	}

	void TearDown() override {
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setEnabled(false);
		dm.reset();
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		fs::remove_all(testDbPath, ec);
	}

	std::vector<int64_t> addUsers(size_t count) {
		std::vector<int64_t> ids;
		ids.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			Node node(0, userLabel);
			dm->addNode(node);
			if (i == 0) {
				dm->addNodeProperties(node.getId(), {{"id", PropertyValue("first")}});
			}
			ids.push_back(node.getId());
		}
		return ids;
	}

	fs::path testDbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	int64_t userLabel = 0;
};

} // namespace

TEST(NodeMetadataBatchTest, HandlesInvalidRowsAndLabelLookups) {
	NodeMetadataBatch batch;
	EXPECT_EQ(batch.size(), 0U);
	EXPECT_FALSE(batch.isValid(0));
	EXPECT_FALSE(batch.hasLabelId(0, 1));
	EXPECT_FALSE(batch.hasLabelId(0, 0));
	EXPECT_EQ(batch.toNode(0).getId(), 0);

	Node node(7, 11);
	node.getMutableMetadata().firstOutEdgeId = 3;
	node.getMutableMetadata().firstInEdgeId = 4;
	node.getMutableMetadata().propertyEntityId = 5;
	node.getMutableMetadata().propertyStorageType = static_cast<uint32_t>(PropertyStorageType::PROPERTY_ENTITY);
	batch.appendDefault();
	batch.setFromNode(1, node);
	EXPECT_FALSE(batch.isValid(0));
	batch.setFromNode(0, node);

	EXPECT_TRUE(batch.isValid(0));
	EXPECT_TRUE(batch.hasLabelId(0, 11));
	EXPECT_FALSE(batch.hasLabelId(0, 0));
	EXPECT_FALSE(batch.hasLabelId(0, 12));
	Node restored = batch.toNode(0);
	EXPECT_EQ(restored.getId(), 7);
	EXPECT_EQ(restored.getFirstOutEdgeId(), 3);
	EXPECT_EQ(restored.getFirstInEdgeId(), 4);
	EXPECT_EQ(restored.getPropertyEntityId(), 5);
	EXPECT_EQ(restored.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);
}

TEST_F(NodeMetadataColumnLoaderStorageTest, RejectsUnsafeOrUnhelpfulLoads) {
	NodeMetadataColumnLoader nullLoader(nullptr);
	EXPECT_FALSE(nullLoader.loadBatch({1, 2, 3}, 0, 3).has_value());
	EXPECT_FALSE(nullLoader.load({1, 2, 3}, 0, 3).has_value());

	auto ids = addUsers(128);
	NodeMetadataColumnLoader loader(dm);
	EXPECT_FALSE(loader.loadBatch(ids, 0, ids.size()).has_value());
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	EXPECT_FALSE(loader.loadBatch(ids, 1, 1).has_value());
	EXPECT_FALSE(loader.loadBatch(ids, ids.size() + 1, ids.size() + 1).has_value());
	EXPECT_FALSE(loader.loadBatch(ids, 0, 4).has_value());
	std::vector<int64_t> unsorted = ids;
	std::swap(unsorted[0], unsorted[1]);
	EXPECT_FALSE(loader.loadBatch(unsorted, 0, unsorted.size()).has_value());

	std::vector<int64_t> outOfRangeIds;
	outOfRangeIds.reserve(128);
	for (int64_t id = ids.back() + 1000; outOfRangeIds.size() < 128; ++id) {
		outOfRangeIds.push_back(id);
	}
	EXPECT_FALSE(loader.loadBatch(outOfRangeIds, 0, outOfRangeIds.size()).has_value());
}

TEST_F(NodeMetadataColumnLoaderStorageTest, RejectsReadOnlySnapshotsWithNodeOverlays) {
	auto ids = addUsers(128);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeMetadataColumnLoader loader(dm);
	storage::CommittedSnapshot emptySnapshot;
	dm->setCurrentSnapshot(&emptySnapshot);
	EXPECT_TRUE(loader.loadBatch(ids, 0, ids.size()).has_value());

	storage::CommittedSnapshot nodeOverlaySnapshot;
	nodeOverlaySnapshot.nodes.emplace(ids.front(),
	                                  storage::DirtyEntityInfo<Node>(storage::EntityChangeType::CHANGE_MODIFIED,
	                                                                 dm->getNode(ids.front())));
	dm->setCurrentSnapshot(&nodeOverlaySnapshot);
	EXPECT_FALSE(loader.loadBatch(ids, 0, ids.size()).has_value());
	dm->clearCurrentSnapshot();
}

TEST_F(NodeMetadataColumnLoaderStorageTest, RejectsReadOnlySnapshotsWithPropertyOrBlobOverlays) {
	auto ids = addUsers(128);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeMetadataColumnLoader loader(dm);

	storage::CommittedSnapshot propertyOverlaySnapshot;
	Property property;
	property.setId(1);
	propertyOverlaySnapshot.properties.emplace(
			property.getId(),
			storage::DirtyEntityInfo<Property>(storage::EntityChangeType::CHANGE_MODIFIED, property));
	dm->setCurrentSnapshot(&propertyOverlaySnapshot);
	EXPECT_FALSE(loader.loadBatch(ids, 0, ids.size()).has_value());

	storage::CommittedSnapshot blobOverlaySnapshot;
	Blob blob;
	blob.setId(1);
	blobOverlaySnapshot.blobs.emplace(
			blob.getId(),
			storage::DirtyEntityInfo<Blob>(storage::EntityChangeType::CHANGE_MODIFIED, blob));
	dm->setCurrentSnapshot(&blobOverlaySnapshot);
	EXPECT_FALSE(loader.loadBatch(ids, 0, ids.size()).has_value());
	dm->clearCurrentSnapshot();
}

TEST_F(NodeMetadataColumnLoaderStorageTest, LoadsSortedMetadataBatchAndNodesFromDisk) {
	auto ids = addUsers(130);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeMetadataColumnLoader loader(dm);
	auto batch = loader.loadBatch(ids, 0, ids.size());

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), ids.size());
	EXPECT_EQ(batch->nodeIds.front(), ids.front());
	EXPECT_EQ(batch->nodeIds.back(), ids.back());
	EXPECT_TRUE(batch->hasLabelId(0, userLabel));
	EXPECT_EQ(batch->propertyStorageTypes[0], PropertyStorageType::PROPERTY_ENTITY);
	EXPECT_NE(batch->propertyEntityIds[0], 0);

	auto nodes = loader.load(ids, 0, ids.size());
	ASSERT_TRUE(nodes.has_value());
	ASSERT_EQ(nodes->size(), ids.size());
	EXPECT_EQ(nodes->front().getId(), ids.front());
	EXPECT_TRUE(nodes->front().hasLabelId(userLabel));
}

TEST_F(NodeMetadataColumnLoaderStorageTest, LoadsInactiveRowsAndRecordsTrace) {
	auto ids = addUsers(130);
	Node deleted = dm->getNode(ids[1]);
	dm->deleteNode(deleted);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();

	NodeMetadataColumnLoader loader(dm);
	auto batch = loader.loadBatch(ids, 0, ids.size());

	ASSERT_TRUE(batch.has_value());
	ASSERT_GT(batch->active.size(), 1U);
	EXPECT_EQ(batch->active[0], 1);
	EXPECT_EQ(batch->active[1], 0);
	EXPECT_EQ(batch->toNode(1).isActive(), false);
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.load_node_metadata"));
}
