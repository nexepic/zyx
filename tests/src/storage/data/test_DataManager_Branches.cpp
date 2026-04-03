/**
 * @file test_DataManager_Branches.cpp
 * @brief Additional branch-focused tests for DataManager.
 */

#include "DataManagerTestFixture.hpp"

#include <algorithm>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/storage/constraints/IEntityValidator.hpp"
#include "graph/storage/SegmentIndexManager.hpp"

namespace {

class CountingValidator final : public graph::storage::constraints::IEntityValidator {
public:
	void validateEdgeInsert(const Edge &,
	                        const std::unordered_map<std::string, PropertyValue> &) override {
		++edgeInsertCalls;
	}

	int edgeInsertCalls = 0;
};

bool containsId(const std::vector<Node> &nodes, int64_t id) {
	return std::any_of(nodes.begin(), nodes.end(), [id](const Node &n) { return n.getId() == id; });
}

bool containsId(const std::vector<Edge> &edges, int64_t id) {
	return std::any_of(edges.begin(), edges.end(), [id](const Edge &e) { return e.getId() == id; });
}

Node makeNodeWithLabel(const std::shared_ptr<DataManager> &dataManager, const std::string &label) {
	Node node;
	node.setLabelId(dataManager->getOrCreateLabelId(label));
	return node;
}

int64_t addNodeWithSinglePropertyEntity(const std::shared_ptr<DataManager> &dataManager,
										const std::string &label, int64_t value) {
	Node node = makeNodeWithLabel(dataManager, label);
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {{"k", PropertyValue(value)}});

	const Node stored = dataManager->getNode(node.getId());
	if (!stored.hasPropertyEntity()) {
		return 0;
	}
	return stored.getPropertyEntityId();
}

const SegmentIndexManager::SegmentIndex *findSegmentForId(
	const std::vector<SegmentIndexManager::SegmentIndex> &segIndex, int64_t id) {
	auto it = std::find_if(segIndex.begin(), segIndex.end(),
						   [id](const SegmentIndexManager::SegmentIndex &seg) {
							   return id >= seg.startId && id <= seg.endId;
						   });
	if (it == segIndex.end()) {
		return nullptr;
	}
	return &(*it);
}

void writeSegmentHeaderUsedOnDisk(const std::filesystem::path &dbPath, uint64_t segmentOffset, uint32_t used) {
	std::fstream io(dbPath, std::ios::binary | std::ios::in | std::ios::out);
	if (!io.is_open()) {
		throw std::runtime_error("Failed to open db file for header mutation");
	}

	SegmentHeader header{};
	io.seekg(static_cast<std::streamoff>(segmentOffset));
	io.read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));
	if (!io.good()) {
		throw std::runtime_error("Failed to read segment header from db file");
	}

	header.used = used;
	io.seekp(static_cast<std::streamoff>(segmentOffset));
	io.write(reinterpret_cast<const char *>(&header), sizeof(SegmentHeader));
	io.flush();
}

void writePropertyActiveFlagOnDisk(const std::filesystem::path &dbPath, uint64_t segmentOffset, int64_t segmentStartId,
								   int64_t propertyId, bool active) {
	constexpr std::streamoff kIsActiveOffset =
		static_cast<std::streamoff>(sizeof(int64_t) + sizeof(int64_t) + sizeof(uint32_t));

	const std::streamoff slot = static_cast<std::streamoff>(propertyId - segmentStartId);
	const std::streamoff entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader)) +
										slot * static_cast<std::streamoff>(Property::getTotalSize());
	const std::streamoff flagOffset = entityOffset + kIsActiveOffset;

	std::fstream io(dbPath, std::ios::binary | std::ios::in | std::ios::out);
	if (!io.is_open()) {
		throw std::runtime_error("Failed to open db file for property mutation");
	}

	const char flag = active ? 1 : 0;
	io.seekp(flagOffset);
	io.write(&flag, sizeof(flag));
	io.flush();
}

} // namespace

TEST_F(DataManagerTest, UnknownIdReadPathsReturnDefaultEntities) {
	constexpr int64_t kMissingId = 9'999'999;

	EXPECT_EQ(dataManager->getEntityFromMemoryOrDisk<Node>(kMissingId).getId(), 0);
	EXPECT_EQ(dataManager->getEntityFromMemoryOrDisk<Edge>(kMissingId).getId(), 0);
	EXPECT_EQ(dataManager->getEntityFromMemoryOrDisk<Property>(kMissingId).getId(), 0);
	EXPECT_EQ(dataManager->getEntityFromMemoryOrDisk<Blob>(kMissingId).getId(), 0);
	EXPECT_EQ(dataManager->getEntityFromMemoryOrDisk<Index>(kMissingId).getId(), 0);
	EXPECT_EQ(dataManager->getEntityFromMemoryOrDisk<State>(kMissingId).getId(), 0);
}

TEST_F(DataManagerTest, RangeReadsPreferDirtyMemoryAndSkipDeletedNodesAndEdges) {
	auto n1 = createTestNode(dataManager, "DirtyRangeNode");
	auto n2 = createTestNode(dataManager, "DirtyRangeNode");
	dataManager->addNode(n1);
	dataManager->addNode(n2);
	simulateSave();

	Node updated = dataManager->getNode(n1.getId());
	updated.setLabelId(dataManager->getOrCreateLabelId("DirtyRangeNodeUpdated"));
	dataManager->updateNode(updated);

	Node deleted = dataManager->getNode(n2.getId());
	dataManager->deleteNode(deleted);

	const auto nodes = dataManager->getNodesInRange(n1.getId(), n2.getId(), 16);
	EXPECT_TRUE(containsId(nodes, n1.getId()));
	EXPECT_FALSE(containsId(nodes, n2.getId()));

	auto a = createTestNode(dataManager, "DirtyRangeEdgeNode");
	auto b = createTestNode(dataManager, "DirtyRangeEdgeNode");
	dataManager->addNode(a);
	dataManager->addNode(b);

	auto e1 = createTestEdge(dataManager, a.getId(), b.getId(), "DIRTY_EDGE");
	auto e2 = createTestEdge(dataManager, a.getId(), b.getId(), "DIRTY_EDGE");
	dataManager->addEdge(e1);
	dataManager->addEdge(e2);
	simulateSave();

	Edge updatedEdge = dataManager->getEdge(e1.getId());
	updatedEdge.setLabelId(dataManager->getOrCreateLabelId("DIRTY_EDGE_UPDATED"));
	dataManager->updateEdge(updatedEdge);

	Edge deletedEdge = dataManager->getEdge(e2.getId());
	dataManager->deleteEdge(deletedEdge);

	const auto edges = dataManager->getEdgesInRange(e1.getId(), e2.getId(), 16);
	EXPECT_TRUE(containsId(edges, e1.getId()));
	EXPECT_FALSE(containsId(edges, e2.getId()));
}

TEST_F(DataManagerTest, BlobBackedPropertyPathsAreCoveredByDirectAndMapReads) {
	auto node = createTestNode(dataManager, "BlobPropsNode");
	dataManager->addNode(node);

	std::string large(32 * 1024, 'x');
	dataManager->addNodeProperties(node.getId(), {{"payload", PropertyValue(large)}});

	const Node stored = dataManager->getNode(node.getId());
	ASSERT_TRUE(stored.hasPropertyEntity());
	ASSERT_EQ(stored.getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);

	const auto directProps = dataManager->getNodePropertiesDirect(stored);
	ASSERT_TRUE(directProps.contains("payload"));
	EXPECT_EQ(std::get<std::string>(directProps.at("payload").getVariant()), large);

	const auto fromMapProps = dataManager->getNodePropertiesFromMap(stored, {});
	ASSERT_TRUE(fromMapProps.contains("payload"));
	EXPECT_EQ(std::get<std::string>(fromMapProps.at("payload").getVariant()), large);
}

TEST_F(DataManagerTest, BulkLoadPropertyEntitiesCoversEmptyMissingAndOutOfSlotIds) {
	auto node = createTestNode(dataManager, "BulkPropertyBranchNode");
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {{"k", PropertyValue(int64_t(1))}});

	const Node stored = dataManager->getNode(node.getId());
	ASSERT_TRUE(stored.hasPropertyEntity());
	const int64_t validPropertyId = stored.getPropertyEntityId();

	simulateSave();
	dataManager->clearCache();

	const auto emptyLoad = dataManager->bulkLoadPropertyEntities({}, nullptr);
	EXPECT_TRUE(emptyLoad.empty());

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_FALSE(segIndex.empty());
	const auto firstSeg = segIndex.front();

	const int64_t definitelyMissing = segIndex.back().endId + 1024;
	const auto missingLoad = dataManager->bulkLoadPropertyEntities({definitelyMissing}, nullptr);
	EXPECT_TRUE(missingLoad.empty());

	// Try an ID that maps to the segment range but should be outside `header.used`.
	const int64_t outOfSlotId = firstSeg.endId;
	const auto mixedLoad = dataManager->bulkLoadPropertyEntities({validPropertyId, outOfSlotId}, nullptr);
	EXPECT_TRUE(mixedLoad.contains(validPropertyId));
}

TEST_F(DataManagerTest, AddEdgesInvokesRegisteredValidatorsPath) {
	auto validator = std::make_shared<CountingValidator>();
	dataManager->registerValidator(validator);

	auto src = createTestNode(dataManager, "ValidatorNode");
	auto dst = createTestNode(dataManager, "ValidatorNode");
	dataManager->addNode(src);
	dataManager->addNode(dst);

	auto e1 = createTestEdge(dataManager, src.getId(), dst.getId(), "VALIDATED_EDGE");
	auto e2 = createTestEdge(dataManager, src.getId(), dst.getId(), "VALIDATED_EDGE");
	std::vector<Edge> edges{e1, e2};
	dataManager->addEdges(edges);

	EXPECT_EQ(validator->edgeInsertCalls, 2);
}

TEST_F(DataManagerTest, BulkLoadPropertyEntitiesSequentialSkipsInactiveAndOutOfUsedSlotIds) {
	const int64_t propertyId = addNodeWithSinglePropertyEntity(dataManager, "BulkSeqBranchNode", 1);
	ASSERT_NE(propertyId, 0);
	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_FALSE(segIndex.empty());
	const auto *seg = findSegmentForId(segIndex, propertyId);
	ASSERT_NE(seg, nullptr);
	const size_t segPos = static_cast<size_t>(seg - segIndex.data());

	writePropertyActiveFlagOnDisk(testFilePath, seg->segmentOffset, seg->startId, propertyId, false);

	auto patchedIndex = segIndex;
	auto &patchedEntry = patchedIndex[segPos];
	patchedEntry.endId = std::max(patchedEntry.endId, patchedEntry.startId + 3);
	const int64_t outOfUsedSlotId = patchedEntry.startId + 3;
	dataManager->getSegmentIndexManager()->setSegmentIndex(Property::typeId, std::move(patchedIndex));

	const auto loaded = dataManager->bulkLoadPropertyEntities({propertyId, outOfUsedSlotId}, nullptr);

	EXPECT_FALSE(loaded.contains(propertyId));
	EXPECT_FALSE(loaded.contains(outOfUsedSlotId));
}

TEST_F(DataManagerTest, BulkLoadPropertyEntitiesSequentialSkipsZeroUsedSegment) {
	const int64_t propertyId = addNodeWithSinglePropertyEntity(dataManager, "BulkSeqZeroUsedNode", 1);
	ASSERT_NE(propertyId, 0);
	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_FALSE(segIndex.empty());
	const auto *seg = findSegmentForId(segIndex, propertyId);
	ASSERT_NE(seg, nullptr);

	dataManager->getSegmentTracker()->updateSegmentHeader(seg->segmentOffset, [](SegmentHeader &header) {
		header.used = 0;
		header.inactive_count = 0;
	});

	const auto loaded = dataManager->bulkLoadPropertyEntities({seg->startId}, nullptr);
	EXPECT_TRUE(loaded.empty());
}

TEST_F(DataManagerTest, BulkLoadPropertyEntitiesParallelHandlesShortReadGroup) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(5);
	for (int i = 0; i < 5; ++i) {
		const int64_t id = addNodeWithSinglePropertyEntity(dataManager, "BulkParShortReadNode", i + 1);
		ASSERT_NE(id, 0);
		propertyIds.push_back(id);
	}

	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_GE(segIndex.size(), 2U);

	auto patchedIndex = segIndex;
	SegmentIndexManager::SegmentIndex fakeSeg{};
	fakeSeg.startId = segIndex.back().endId + 100;
	fakeSeg.endId = fakeSeg.startId;
	fakeSeg.segmentOffset = segIndex.back().segmentOffset + static_cast<uint64_t>(TOTAL_SEGMENT_SIZE) * 10'000ULL;
	patchedIndex.push_back(fakeSeg);
	dataManager->getSegmentIndexManager()->setSegmentIndex(Property::typeId, std::move(patchedIndex));

	const int64_t realIdA = propertyIds.front();
	const int64_t realIdB = propertyIds.back();
	concurrent::ThreadPool pool(2);
	const auto loaded = dataManager->bulkLoadPropertyEntities({realIdA, realIdB, fakeSeg.startId}, &pool);

	EXPECT_TRUE(loaded.contains(realIdA));
	EXPECT_TRUE(loaded.contains(realIdB));
	EXPECT_FALSE(loaded.contains(fakeSeg.startId));
}

TEST_F(DataManagerTest, BulkLoadPropertyEntitiesParallelCoversZeroUsedInactiveAndOutOfSlot) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(5);
	for (int i = 0; i < 5; ++i) {
		const int64_t id = addNodeWithSinglePropertyEntity(dataManager, "BulkParBranchNode", i + 10);
		ASSERT_NE(id, 0);
		propertyIds.push_back(id);
	}

	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_GE(segIndex.size(), 2U);

	const auto *firstSeg = findSegmentForId(segIndex, propertyIds.front());
	const auto *secondSeg = findSegmentForId(segIndex, propertyIds.back());
	ASSERT_NE(firstSeg, nullptr);
	ASSERT_NE(secondSeg, nullptr);
	ASSERT_NE(firstSeg->segmentOffset, secondSeg->segmentOffset);

	const int64_t inactiveId = propertyIds.front();
	int64_t activeIdInFirstSeg = 0;
	for (int64_t id : propertyIds) {
		if (id >= firstSeg->startId && id <= firstSeg->endId && id != inactiveId) {
			activeIdInFirstSeg = id;
			break;
		}
	}
	ASSERT_NE(activeIdInFirstSeg, 0);

	writePropertyActiveFlagOnDisk(testFilePath, firstSeg->segmentOffset, firstSeg->startId, inactiveId, false);
	writeSegmentHeaderUsedOnDisk(testFilePath, secondSeg->segmentOffset, 0);

	auto patchedIndex = segIndex;
	auto firstIt = std::find_if(patchedIndex.begin(), patchedIndex.end(),
								[firstSeg](const SegmentIndexManager::SegmentIndex &seg) {
									return seg.segmentOffset == firstSeg->segmentOffset;
								});
	ASSERT_NE(firstIt, patchedIndex.end());
	firstIt->endId = std::max(firstIt->endId, firstIt->startId + 7);
	const int64_t outOfSlotId = firstIt->startId + 7;
	dataManager->getSegmentIndexManager()->setSegmentIndex(Property::typeId, std::move(patchedIndex));

	const int64_t secondSegId = propertyIds.back();

	concurrent::ThreadPool pool(2);
	const auto loaded = dataManager->bulkLoadPropertyEntities(
		{inactiveId, activeIdInFirstSeg, outOfSlotId, secondSegId}, &pool);

	EXPECT_FALSE(loaded.contains(inactiveId));
	EXPECT_TRUE(loaded.contains(activeIdInFirstSeg));
	EXPECT_FALSE(loaded.contains(outOfSlotId));
	EXPECT_FALSE(loaded.contains(secondSegId));
}

TEST_F(DataManagerTest, BulkLoadPropertyEntitiesWithSingleThreadPoolFallsBackToSequentialPath) {
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(5);
	for (int i = 0; i < 5; ++i) {
		const int64_t id = addNodeWithSinglePropertyEntity(dataManager, "BulkSingleThreadPoolNode", i + 1);
		ASSERT_NE(id, 0);
		propertyIds.push_back(id);
	}

	simulateSave();
	dataManager->clearCache();

	concurrent::ThreadPool singleThreadPool(1);
	const auto loaded = dataManager->bulkLoadPropertyEntities({propertyIds.front(), propertyIds.back()},
															  &singleThreadPool);

	EXPECT_TRUE(loaded.contains(propertyIds.front()));
	EXPECT_TRUE(loaded.contains(propertyIds.back()));
}

TEST_F(DataManagerTest, BulkLoadPropertyEntitiesWithSingleWorkSegmentSkipsParallelPath) {
	const int64_t propertyId = addNodeWithSinglePropertyEntity(dataManager, "BulkOneWorkSegmentNode", 1);
	ASSERT_NE(propertyId, 0);
	simulateSave();
	dataManager->clearCache();

	concurrent::ThreadPool pool(2);
	const auto loaded = dataManager->bulkLoadPropertyEntities({propertyId}, &pool);

	EXPECT_TRUE(loaded.contains(propertyId));
}

TEST_F(DataManagerTest, BulkLoadPropertyEntitiesSequentialHandlesShortRead) {
	const int64_t propertyId = addNodeWithSinglePropertyEntity(dataManager, "BulkSeqShortReadNode", 1);
	ASSERT_NE(propertyId, 0);
	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_FALSE(segIndex.empty());
	const auto *seg = findSegmentForId(segIndex, propertyId);
	ASSERT_NE(seg, nullptr);

	dataManager->getSegmentTracker()->updateSegmentHeader(seg->segmentOffset, [](SegmentHeader &header) {
		header.used = 100;
		header.inactive_count = 0;
	});

	const auto loaded = dataManager->bulkLoadPropertyEntities({propertyId}, nullptr);
	EXPECT_TRUE(loaded.empty());
}

TEST_F(DataManagerTest, LoadIndexAndStateFromDiskUseExistingSegments) {
	auto index = createTestIndex(Index::NodeType::LEAF, 123);
	dataManager->addIndexEntity(index);
	ASSERT_NE(index.getId(), 0);

	auto state = createTestState("branch.load.state");
	dataManager->addStateEntity(state);
	ASSERT_NE(state.getId(), 0);

	simulateSave();
	dataManager->clearCache();

	const Index loadedIndex = dataManager->loadIndexFromDisk(index.getId());
	const State loadedState = dataManager->loadStateFromDisk(state.getId());

	EXPECT_EQ(loadedIndex.getId(), index.getId());
	EXPECT_EQ(loadedState.getId(), state.getId());
}
