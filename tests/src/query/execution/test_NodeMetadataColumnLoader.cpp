#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <memory>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/NodeMetadataColumnLoader.hpp"
#include "graph/query/execution/NodeMetadataFilter.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/data/DirtyEntityInfo.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;

namespace {

	constexpr size_t kParallelNodeMetadataUserCount = 70000;

	class NodeMetadataColumnLoaderStorageTest : public ::testing::Test {
	protected:
		void SetUp() override {
			const auto uuid = boost::uuids::random_generator()();
			testDbPath = fs::temp_directory_path() /
						 ("test_node_metadata_column_loader_" + boost::uuids::to_string(uuid) + ".zyx");
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
	NodeMetadataRow row;
	EXPECT_FALSE(row.isValid());
	EXPECT_FALSE(row.hasLabelId(11));
	EXPECT_EQ(row.toNode().getId(), 0);

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
	row.nodeId = 7;
	row.firstOutEdgeId = 3;
	row.firstInEdgeId = 4;
	row.propertyEntityId = 5;
	row.propertyStorageType = PropertyStorageType::PROPERTY_ENTITY;
	row.active = 1;
	row.labelCount = 1;
	row.labelIds[0] = 11;

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
	EXPECT_TRUE(row.isValid());
	EXPECT_TRUE(row.hasLabelId(11));
	EXPECT_FALSE(row.hasLabelId(12));
	Node restoredFromRow = row.toNode();
	EXPECT_EQ(restoredFromRow.getId(), 7);
	EXPECT_EQ(restoredFromRow.getFirstOutEdgeId(), 3);
	EXPECT_EQ(restoredFromRow.getFirstInEdgeId(), 4);
	EXPECT_EQ(restoredFromRow.getPropertyEntityId(), 5);
	EXPECT_EQ(restoredFromRow.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);
	batch.setFromMetadataRow(0, row);
	EXPECT_EQ(batch.nodeIds[0], row.nodeId);
	batch.setFromMetadataRow(99, row);
	EXPECT_EQ(batch.size(), 1U);

	NodePropertyCandidateRef ref{9, 10, PropertyStorageType::BLOB_ENTITY};
	Node refNode = ref.toNode();
	EXPECT_EQ(refNode.getId(), 9);
	EXPECT_TRUE(refNode.isActive());
	EXPECT_EQ(refNode.getPropertyEntityId(), 10);
	EXPECT_EQ(refNode.getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);

	NodePropertyCountCandidates candidates;
	candidates.reserve(8);
	EXPECT_EQ(candidates.propertyRowCount(), 0U);
	EXPECT_EQ(candidates.acceptedRowCount, 0U);
}

TEST_F(NodeMetadataColumnLoaderStorageTest, RejectsUnsafeOrUnhelpfulLoads) {
	NodeMetadataColumnLoader nullLoader(nullptr);
	EXPECT_FALSE(nullLoader.loadBatch({1, 2, 3}, 0, 3).has_value());
	EXPECT_FALSE(nullLoader.load({1, 2, 3}, 0, 3).has_value());
	NodeScanConfig config;
	NodeScanRequirements requirements;
	EXPECT_FALSE(nullLoader.collectPropertyCountCandidates({1, 2, 3}, 0, 3, config, requirements).has_value());

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
	EXPECT_FALSE(loader.visitBatch(outOfRangeIds, 0, outOfRangeIds.size(),
								   [](size_t, const NodeMetadataRow &) { return true; }));
	EXPECT_FALSE(loader.visitBatch(ids, 0, ids.size(), {}));
	EXPECT_FALSE(loader.visitBatchPartitioned(ids, 0, ids.size(), {}, {}, nullptr));
	EXPECT_FALSE(loader.collectPropertyCountCandidates(ids, 0, 4, config, requirements).has_value());
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
	nodeOverlaySnapshot.nodes.emplace(
			ids.front(),
			storage::DirtyEntityInfo<Node>(storage::EntityChangeType::CHANGE_MODIFIED, dm->getNode(ids.front())));
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
			property.getId(), storage::DirtyEntityInfo<Property>(storage::EntityChangeType::CHANGE_MODIFIED, property));
	dm->setCurrentSnapshot(&propertyOverlaySnapshot);
	EXPECT_FALSE(loader.loadBatch(ids, 0, ids.size()).has_value());

	storage::CommittedSnapshot blobOverlaySnapshot;
	Blob blob;
	blob.setId(1);
	blobOverlaySnapshot.blobs.emplace(blob.getId(),
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

TEST_F(NodeMetadataColumnLoaderStorageTest, VisitsSortedMetadataRowsFromDisk) {
	auto ids = addUsers(130);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeMetadataColumnLoader loader(dm);
	std::vector<int64_t> visitedIds;
	visitedIds.reserve(ids.size());
	size_t propertyRows = 0;
	const bool visited = loader.visitBatch(ids, 0, ids.size(), [&](size_t row, const NodeMetadataRow &metadata) {
		EXPECT_LT(row, ids.size());
		EXPECT_EQ(metadata.nodeId, ids[row]);
		EXPECT_TRUE(metadata.isValid());
		EXPECT_TRUE(metadata.hasLabelId(userLabel));
		if (metadata.propertyStorageType == PropertyStorageType::PROPERTY_ENTITY) {
			++propertyRows;
		}
		visitedIds.push_back(metadata.nodeId);
		return true;
	});

	ASSERT_TRUE(visited);
	EXPECT_EQ(visitedIds, ids);
	EXPECT_EQ(propertyRows, 1U);
}

TEST_F(NodeMetadataColumnLoaderStorageTest, VisitBatchCanStopEarlyAndPartitionWithoutInitializer) {
	auto ids = addUsers(130);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeMetadataColumnLoader loader(dm);
	size_t visitedRows = 0;
	const bool stopped = loader.visitBatch(ids, 0, ids.size(), [&](size_t row, const NodeMetadataRow &metadata) {
		EXPECT_EQ(metadata.nodeId, ids[row]);
		++visitedRows;
		return row == 0;
	});
	EXPECT_TRUE(stopped);
	EXPECT_EQ(visitedRows, 2U);

	size_t partitionedRows = 0;
	const bool partitioned = loader.visitBatchPartitioned(
			ids, 0, ids.size(), {},
			[&](size_t partition, size_t row, const NodeMetadataRow &metadata) {
				EXPECT_EQ(partition, 0U);
				EXPECT_EQ(metadata.nodeId, ids[row]);
				++partitionedRows;
				return true;
			},
			nullptr);
	EXPECT_TRUE(partitioned);
	EXPECT_EQ(partitionedRows, ids.size());
}

TEST_F(NodeMetadataColumnLoaderStorageTest, VisitBatchPartitionedScansMultipleNodeMetadataTasks) {
	auto ids = addUsers(kParallelNodeMetadataUserCount);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	graph::concurrent::ThreadPool pool(4);
	NodeMetadataColumnLoader loader(dm);
	std::vector<size_t> partitionVisits;
	std::atomic<size_t> totalVisits{0};
	std::atomic<bool> rowsValid{true};

	const bool visited = loader.visitBatchPartitioned(
			ids, 0, ids.size(), [&](size_t partitionCount) { partitionVisits.assign(partitionCount, 0); },
			[&](size_t partition, size_t row, const NodeMetadataRow &metadata) {
				if (partition >= partitionVisits.size() || row >= ids.size() || metadata.nodeId != ids[row]) {
					rowsValid.store(false, std::memory_order_relaxed);
					return true;
				}
				++partitionVisits[partition];
				totalVisits.fetch_add(1, std::memory_order_relaxed);
				return true;
			},
			&pool);

	ASSERT_TRUE(visited);
	EXPECT_TRUE(rowsValid.load());
	EXPECT_EQ(totalVisits.load(), ids.size());
	ASSERT_GT(partitionVisits.size(), 1U);
	const auto nonEmptyPartitions = static_cast<size_t>(
			std::count_if(partitionVisits.begin(), partitionVisits.end(), [](size_t count) { return count != 0; }));
	EXPECT_GT(nonEmptyPartitions, 1U);
}

TEST_F(NodeMetadataColumnLoaderStorageTest, VisitBatchPartitionedStopsEarlyAcrossParallelTasks) {
	auto ids = addUsers(kParallelNodeMetadataUserCount);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	graph::concurrent::ThreadPool pool(4);
	NodeMetadataColumnLoader loader(dm);
	std::atomic<size_t> visitedRows{0};
	std::atomic<bool> rowsValid{true};

	const bool visited = loader.visitBatchPartitioned(
			ids, 0, ids.size(), {},
			[&](size_t, size_t row, const NodeMetadataRow &metadata) {
				if (row >= ids.size() || metadata.nodeId != ids[row]) {
					rowsValid.store(false, std::memory_order_relaxed);
				}
				const size_t previous = visitedRows.fetch_add(1, std::memory_order_relaxed);
				return previous != 0;
			},
			&pool);

	EXPECT_TRUE(visited);
	EXPECT_TRUE(rowsValid.load(std::memory_order_relaxed));
	EXPECT_GT(visitedRows.load(std::memory_order_relaxed), 0U);
	EXPECT_LT(visitedRows.load(std::memory_order_relaxed), ids.size());
}

TEST_F(NodeMetadataColumnLoaderStorageTest, CollectsPropertyCandidatesWithParallelMetadataPartitions) {
	auto ids = addUsers(kParallelNodeMetadataUserCount);
	dm->addNodeProperties(ids[2048], {{"id", PropertyValue("second")}});
	const std::string largePayload(5000, 'p');
	dm->addNodeProperties(ids[4096], {{"payload", PropertyValue(largePayload)}});
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.labels = {"User"};
	NodeScanRequirements requirements;
	requirements.needsLabels = true;
	requirements.needsActiveCheck = true;

	graph::concurrent::ThreadPool pool(4);
	NodeMetadataColumnLoader loader(dm);
	auto serial = loader.collectPropertyCountCandidates(ids, 0, ids.size(), config, requirements);
	auto parallel = loader.collectPropertyCountCandidates(ids, 0, ids.size(), config, requirements, &pool);

	ASSERT_TRUE(serial.has_value());
	ASSERT_TRUE(parallel.has_value());
	EXPECT_EQ(parallel->acceptedRowCount, serial->acceptedRowCount);
	EXPECT_EQ(parallel->propertyEntityIds.size(), serial->propertyEntityIds.size());
	EXPECT_EQ(parallel->propertyNodeIds.size(), serial->propertyNodeIds.size());
	EXPECT_EQ(parallel->propertyRows.size(), serial->propertyRows.size());
	EXPECT_EQ(parallel->blobRefs.size(), serial->blobRefs.size());

	auto serialNodeIds = serial->propertyNodeIds;
	auto parallelNodeIds = parallel->propertyNodeIds;
	std::sort(serialNodeIds.begin(), serialNodeIds.end());
	std::sort(parallelNodeIds.begin(), parallelNodeIds.end());
	EXPECT_EQ(parallelNodeIds, serialNodeIds);
	ASSERT_EQ(parallel->blobRefs.size(), 1U);
	EXPECT_EQ(parallel->blobRefs.front().nodeId, ids[4096]);
}

TEST_F(NodeMetadataColumnLoaderStorageTest, FullScanPropertyCandidatesUseParallelMetadataPartitions) {
	auto ids = addUsers(kParallelNodeMetadataUserCount);
	dm->addNodeProperties(ids[2048], {{"id", PropertyValue("second")}});
	const std::string largePayload(5000, 'q');
	dm->addNodeProperties(ids[4096], {{"payload", PropertyValue(largePayload)}});
	Node deleted = dm->getNode(ids[8192]);
	dm->deleteNode(deleted);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	NodeScanRequirements requirements;
	requirements.needsLabels = false;
	requirements.needsActiveCheck = true;

	graph::concurrent::ThreadPool pool(4);
	NodeMetadataColumnLoader loader(dm);
	auto serial = loader.collectFullScanPropertyCountCandidates(config, requirements);
	auto parallel = loader.collectFullScanPropertyCountCandidates(config, requirements, &pool);

	ASSERT_TRUE(serial.has_value());
	ASSERT_TRUE(parallel.has_value());
	EXPECT_EQ(parallel->acceptedRowCount, serial->acceptedRowCount);
	EXPECT_EQ(parallel->propertyEntityIds.size(), serial->propertyEntityIds.size());
	EXPECT_EQ(parallel->propertyNodeIds.size(), serial->propertyNodeIds.size());
	EXPECT_EQ(parallel->propertyRows.size(), serial->propertyRows.size());
	EXPECT_EQ(parallel->blobRefs.size(), serial->blobRefs.size());

	auto serialNodeIds = serial->propertyNodeIds;
	auto parallelNodeIds = parallel->propertyNodeIds;
	std::sort(serialNodeIds.begin(), serialNodeIds.end());
	std::sort(parallelNodeIds.begin(), parallelNodeIds.end());
	EXPECT_EQ(parallelNodeIds, serialNodeIds);
	EXPECT_EQ(parallel->acceptedRowCount, ids.size() - 1);
}

TEST_F(NodeMetadataColumnLoaderStorageTest, FullScanPropertyCandidatesRejectDirtySnapshotsAndLabelRequirements) {
	auto ids = addUsers(130);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	NodeScanRequirements requirements;
	requirements.needsLabels = false;
	requirements.needsActiveCheck = true;
	NodeMetadataColumnLoader loader(dm);

	NodeScanRequirements labelRequirements = requirements;
	labelRequirements.needsLabels = true;
	EXPECT_FALSE(loader.collectFullScanPropertyCountCandidates(config, labelRequirements).has_value());

	storage::CommittedSnapshot nodeSnapshot;
	nodeSnapshot.nodes.emplace(ids.front(), storage::DirtyEntityInfo<Node>(storage::EntityChangeType::CHANGE_MODIFIED,
																		   dm->getNode(ids.front())));
	dm->setCurrentSnapshot(&nodeSnapshot);
	EXPECT_FALSE(loader.collectFullScanPropertyCountCandidates(config, requirements).has_value());

	storage::CommittedSnapshot propertySnapshot;
	Property property;
	property.setId(1);
	propertySnapshot.properties.emplace(
			property.getId(), storage::DirtyEntityInfo<Property>(storage::EntityChangeType::CHANGE_MODIFIED, property));
	dm->setCurrentSnapshot(&propertySnapshot);
	EXPECT_FALSE(loader.collectFullScanPropertyCountCandidates(config, requirements).has_value());

	storage::CommittedSnapshot blobSnapshot;
	Blob blob;
	blob.setId(1);
	blobSnapshot.blobs.emplace(blob.getId(),
							   storage::DirtyEntityInfo<Blob>(storage::EntityChangeType::CHANGE_MODIFIED, blob));
	dm->setCurrentSnapshot(&blobSnapshot);
	EXPECT_FALSE(loader.collectFullScanPropertyCountCandidates(config, requirements).has_value());
	dm->clearCurrentSnapshot();
}

TEST_F(NodeMetadataColumnLoaderStorageTest, VisitBatchSupportsProjectedMetadataRows) {
	auto ids = addUsers(130);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeMetadataColumnLoader loader(dm);
	NodeMetadataProjection projection;
	projection.loadEdgeRefs = false;
	projection.loadLabels = false;

	size_t visitedRows = 0;
	size_t propertyRows = 0;
	const bool visited = loader.visitBatch(
			ids, 0, ids.size(),
			[&](size_t row, const NodeMetadataRow &metadata) {
				EXPECT_EQ(metadata.nodeId, ids[row]);
				EXPECT_TRUE(metadata.isValid());
				EXPECT_FALSE(metadata.hasLabelId(userLabel));
				EXPECT_EQ(metadata.labelCount, 0);
				EXPECT_EQ(metadata.firstOutEdgeId, 0);
				EXPECT_EQ(metadata.firstInEdgeId, 0);
				if (metadata.propertyStorageType == PropertyStorageType::PROPERTY_ENTITY) {
					++propertyRows;
				}
				++visitedRows;
				return true;
			},
			projection);

	ASSERT_TRUE(visited);
	EXPECT_EQ(visitedRows, ids.size());
	EXPECT_EQ(propertyRows, 1U);
}

TEST_F(NodeMetadataColumnLoaderStorageTest, CollectsPropertyCountCandidatesWithLabelAndActiveFilters) {
	auto ids = addUsers(130);
	const std::string largePayload(5000, 'b');
	dm->addNodeProperties(ids[2], {{"payload", PropertyValue(largePayload)}});
	Node deleted = dm->getNode(ids[3]);
	dm->deleteNode(deleted);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.labels = {"User"};
	NodeScanRequirements requirements;
	requirements.needsLabels = true;
	requirements.needsActiveCheck = true;

	NodeMetadataColumnLoader loader(dm);
	auto candidates = loader.collectPropertyCountCandidates(ids, 0, ids.size(), config, requirements);
	ASSERT_TRUE(candidates.has_value());
	ASSERT_EQ(candidates->propertyEntityIds.size(), 1U);
	ASSERT_EQ(candidates->propertyNodeIds.size(), 1U);
	ASSERT_EQ(candidates->propertyRows.size(), 1U);
	EXPECT_EQ(candidates->acceptedRowCount, ids.size() - 1);
	EXPECT_EQ(candidates->propertyNodeIds.front(), ids.front());
	EXPECT_EQ(candidates->propertyRows.front(), 0U);
	ASSERT_EQ(candidates->blobRefs.size(), 1U);
	EXPECT_EQ(candidates->blobRefs.front().nodeId, ids[2]);
	EXPECT_EQ(candidates->blobRefs.front().propertyStorageType, PropertyStorageType::BLOB_ENTITY);

	NodeScanConfig missingConfig;
	missingConfig.labels = {"Missing"};
	auto missing = loader.collectPropertyCountCandidates(ids, 0, ids.size(), missingConfig, requirements);
	ASSERT_TRUE(missing.has_value());
	EXPECT_TRUE(missing->propertyEntityIds.empty());
	EXPECT_TRUE(missing->blobRefs.empty());
	EXPECT_EQ(missing->acceptedRowCount, 0U);
}

TEST_F(NodeMetadataColumnLoaderStorageTest, CollectsPropertyCountCandidatesWithoutFallbackRefs) {
	auto ids = addUsers(130);
	const std::string largePayload(5000, 'b');
	dm->addNodeProperties(ids[2], {{"payload", PropertyValue(largePayload)}});
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.labels = {"User"};
	NodeScanRequirements requirements;
	requirements.needsLabels = true;
	requirements.needsActiveCheck = true;
	NodePropertyCountCandidateOptions options;
	options.collectFallbackRefs = false;

	NodeMetadataColumnLoader loader(dm);
	auto candidates = loader.collectPropertyCountCandidates(ids, 0, ids.size(), config, requirements, options);
	ASSERT_TRUE(candidates.has_value());
	ASSERT_EQ(candidates->propertyEntityIds.size(), 1U);
	EXPECT_TRUE(candidates->propertyNodeIds.empty());
	EXPECT_TRUE(candidates->propertyRows.empty());
	ASSERT_EQ(candidates->blobRefs.size(), 1U);
	EXPECT_EQ(candidates->blobRefs.front().nodeId, ids[2]);
	EXPECT_EQ(candidates->acceptedRowCount, ids.size());
}

TEST_F(NodeMetadataColumnLoaderStorageTest, FullScanPropertyCandidatesAvoidCandidateIdMaterialization) {
	auto ids = addUsers(130);
	const std::string largePayload(5000, 'b');
	dm->addNodeProperties(ids[2], {{"payload", PropertyValue(largePayload)}});
	Node deleted = dm->getNode(ids[3]);
	dm->deleteNode(deleted);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	NodeScanConfig config;
	config.type = ScanType::FULL_SCAN;
	NodeScanRequirements requirements;
	requirements.needsLabels = false;
	requirements.needsActiveCheck = true;
	NodeMetadataColumnLoader loader(dm);

	NodePropertyCountCandidateOptions countOnly;
	countOnly.collectFallbackRefs = false;
	auto countCandidates = loader.collectFullScanPropertyCountCandidates(config, requirements, countOnly);
	ASSERT_TRUE(countCandidates.has_value());
	EXPECT_EQ(countCandidates->acceptedRowCount, ids.size() - 1);
	EXPECT_EQ(countCandidates->propertyEntityIds.size(), 1U);
	EXPECT_TRUE(countCandidates->propertyNodeIds.empty());
	EXPECT_TRUE(countCandidates->propertyRows.empty());
	ASSERT_EQ(countCandidates->blobRefs.size(), 1U);
	EXPECT_EQ(countCandidates->blobRefs.front().nodeId, ids[2]);

	auto detailed = loader.collectFullScanPropertyCountCandidates(config, requirements);
	ASSERT_TRUE(detailed.has_value());
	ASSERT_EQ(detailed->propertyNodeIds.size(), 1U);
	ASSERT_EQ(detailed->propertyRows.size(), 1U);
	EXPECT_EQ(detailed->propertyNodeIds.front(), ids.front());
	EXPECT_EQ(detailed->propertyRows.front(), 0U);

	requirements.needsActiveCheck = false;
	auto noActiveCheck = loader.collectFullScanPropertyCountCandidates(config, requirements);
	ASSERT_TRUE(noActiveCheck.has_value());
	EXPECT_EQ(noActiveCheck->acceptedRowCount, ids.size() - 1);

	config.labels = {"User"};
	EXPECT_FALSE(loader.collectFullScanPropertyCountCandidates(config, requirements).has_value());
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

TEST_F(NodeMetadataColumnLoaderStorageTest, RowFilterAppliesLabelsActiveAndValidity) {
	NodeScanConfig config;
	config.labels = {"User"};
	NodeScanRequirements requirements;
	requirements.needsLabels = true;
	requirements.needsActiveCheck = true;
	NodeMetadataRowFilter filter(dm, config, requirements);

	NodeMetadataBatch batch;
	batch.appendDefault();
	batch.nodeIds[0] = 1;
	batch.active[0] = 1;
	batch.labelCounts[0] = 1;
	batch.labelIds[0][0] = userLabel;
	EXPECT_TRUE(filter.accepts(batch, 0));
	EXPECT_FALSE(filter.accepts(batch, 1));

	batch.labelCounts[0] = 1;
	EXPECT_TRUE(filter.accepts(batch, 0));
	NodeScanConfig missingConfig;
	missingConfig.labels = {"Missing"};
	NodeMetadataRowFilter missingFilter(dm, missingConfig, requirements);
	EXPECT_FALSE(missingFilter.accepts(batch, 0));
	batch.active[0] = 0;
	EXPECT_FALSE(filter.accepts(batch, 0));

	NodeMetadataRow row;
	row.nodeId = 7;
	row.active = 1;
	row.labelCount = 1;
	row.labelIds[0] = userLabel;
	EXPECT_TRUE(filter.accepts(row));
	EXPECT_FALSE(missingFilter.accepts(row));
	row.active = 0;
	EXPECT_FALSE(filter.accepts(row));
	row.active = 1;
	row.labelIds[0] = 0;
	EXPECT_FALSE(filter.accepts(row));

	NodeScanRequirements noLabelsOrActive;
	noLabelsOrActive.needsLabels = false;
	noLabelsOrActive.needsActiveCheck = false;
	NodeMetadataRowFilter permissive(nullptr, config, noLabelsOrActive);
	row.nodeId = 8;
	row.active = 0;
	row.labelCount = 0;
	EXPECT_TRUE(permissive.accepts(row));
	batch.nodeIds[0] = 2;
	batch.active[0] = 0;
	batch.labelCounts[0] = 0;
	EXPECT_TRUE(permissive.accepts(batch, 0));
	row.nodeId = 0;
	EXPECT_FALSE(permissive.accepts(row));
}
