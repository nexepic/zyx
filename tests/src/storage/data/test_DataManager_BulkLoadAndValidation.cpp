/**
 * @file test_DataManager_BulkLoadAndValidation.cpp
 * @brief Additional branch-focused tests for DataManager.
 */

#include "DataManagerTestFixture.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
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
	node.setLabelId(dataManager->getOrCreateTokenId(label));
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

int64_t addNodeWithPropertyEntity(
		const std::shared_ptr<DataManager> &dataManager,
		const std::string &label,
		const std::unordered_map<std::string, PropertyValue> &properties) {
	Node node = makeNodeWithLabel(dataManager, label);
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), properties);

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

template<typename EntityType>
void writeSerializedEntityOnDisk(const std::filesystem::path &dbPath,
								 uint64_t segmentOffset,
								 int64_t segmentStartId,
								 int64_t entityId,
								 const EntityType &entity) {
	const std::streamoff slot = static_cast<std::streamoff>(entityId - segmentStartId);
	const std::streamoff entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader)) +
										slot * static_cast<std::streamoff>(EntityType::getTotalSize());
	std::ostringstream serialized(std::ios::binary);
	entity.serialize(serialized);
	const std::string bytes = serialized.str();

	std::fstream io(dbPath, std::ios::binary | std::ios::in | std::ios::out);
	if (!io.is_open()) {
		throw std::runtime_error("Failed to open db file for entity mutation");
	}

	io.seekp(entityOffset);
	io.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	io.flush();
}

template<typename EntityType>
void writeEntityIdOnDisk(const std::filesystem::path &dbPath,
						 uint64_t segmentOffset,
						 int64_t segmentStartId,
						 int64_t entityId,
						 int64_t replacementId) {
	const std::streamoff slot = static_cast<std::streamoff>(entityId - segmentStartId);
	const std::streamoff entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader)) +
										slot * static_cast<std::streamoff>(EntityType::getTotalSize());

	std::fstream io(dbPath, std::ios::binary | std::ios::in | std::ios::out);
	if (!io.is_open()) {
		throw std::runtime_error("Failed to open db file for entity id mutation");
	}

	io.seekp(entityOffset);
	io.write(reinterpret_cast<const char *>(&replacementId), sizeof(replacementId));
	io.flush();
}

void writePropertyKeyLengthOnDisk(const std::filesystem::path &dbPath,
								  uint64_t segmentOffset,
								  int64_t segmentStartId,
								  int64_t propertyId,
								  uint32_t keyLength) {
	constexpr std::streamoff kPropertyCountOffset =
		static_cast<std::streamoff>(sizeof(int64_t) + sizeof(int64_t) + sizeof(uint32_t) + sizeof(bool));
	constexpr std::streamoff kFirstKeyLengthOffset = kPropertyCountOffset + static_cast<std::streamoff>(sizeof(uint32_t));
	const std::streamoff slot = static_cast<std::streamoff>(propertyId - segmentStartId);
	const std::streamoff entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader)) +
										slot * static_cast<std::streamoff>(Property::getTotalSize());

	std::fstream io(dbPath, std::ios::binary | std::ios::in | std::ios::out);
	if (!io.is_open()) {
		throw std::runtime_error("Failed to open db file for property key mutation");
	}

	io.seekp(entityOffset + kFirstKeyLengthOffset);
	io.write(reinterpret_cast<const char *>(&keyLength), sizeof(keyLength));
	io.flush();
}

void writeFirstPropertyValueTypeOnDisk(const std::filesystem::path &dbPath,
									   uint64_t segmentOffset,
									   int64_t segmentStartId,
									   int64_t propertyId,
									   const std::string &key,
									   PropertyType type) {
	constexpr std::streamoff kPropertyCountOffset =
		static_cast<std::streamoff>(sizeof(int64_t) + sizeof(int64_t) + sizeof(uint32_t) + sizeof(bool));
	const std::streamoff firstValueTypeOffset = kPropertyCountOffset + static_cast<std::streamoff>(sizeof(uint32_t)) +
												static_cast<std::streamoff>(sizeof(uint32_t)) +
												static_cast<std::streamoff>(key.size());
	const std::streamoff slot = static_cast<std::streamoff>(propertyId - segmentStartId);
	const std::streamoff entityOffset = static_cast<std::streamoff>(segmentOffset + sizeof(SegmentHeader)) +
										slot * static_cast<std::streamoff>(Property::getTotalSize());

	std::fstream io(dbPath, std::ios::binary | std::ios::in | std::ios::out);
	if (!io.is_open()) {
		throw std::runtime_error("Failed to open db file for property value mutation");
	}

	io.seekp(entityOffset + firstValueTypeOffset);
	io.write(reinterpret_cast<const char *>(&type), sizeof(type));
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
	updated.setLabelId(dataManager->getOrCreateTokenId("DirtyRangeNodeUpdated"));
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
	updatedEdge.setTypeId(dataManager->getOrCreateTokenId("DIRTY_EDGE_UPDATED"));
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

TEST_F(DataManagerTest, DirectPropertyReadsIgnoreUnknownExternalStorageType) {
	Node node(123, dataManager->getOrCreateTokenId("UnknownStorageTypeNode"));
	node.addProperty("inline", PropertyValue(int64_t{7}));
	node.setPropertyEntityId(999, static_cast<PropertyStorageType>(999));

	const auto directProps = dataManager->getNodePropertiesDirect(node);
	ASSERT_TRUE(directProps.contains("inline"));
	EXPECT_EQ(directProps.at("inline"), PropertyValue(int64_t{7}));

	const auto mappedProps = dataManager->getNodePropertiesFromMap(node, {});
	ASSERT_TRUE(mappedProps.contains("inline"));
	EXPECT_EQ(mappedProps.at("inline"), PropertyValue(int64_t{7}));
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

TEST_F(DataManagerTest, BulkLoadPropertyEntityValuesLoadsOnlyRequestedKeys) {
	PropertyValue::MapType mapValue;
	mapValue.emplace("inner", PropertyValue(int64_t(7)));
	std::vector<PropertyValue> ignoredList{PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkSelectedPropertyNode",
		{{"keep", PropertyValue(int64_t(42))},
		 {"map", PropertyValue(std::move(mapValue))},
		 {"ignored", PropertyValue("skip-me")},
		 {"ignoredList", PropertyValue(std::move(ignoredList))}});
	ASSERT_NE(propertyId, 0);

	PropertyValue::MapType ignoredMap;
	ignoredMap.emplace("x", PropertyValue(int64_t(1)));
	const int64_t mixedTypePropertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkSelectedPropertyMixedTypeNode",
		{{"keep", PropertyValue(int64_t(11))},
		 {"skipBool", PropertyValue(true)},
		 {"skipDouble", PropertyValue(1.5)},
		 {"skipDate", PropertyValue(TemporalDate{1})},
		 {"skipDateTime", PropertyValue(TemporalDateTime{2})},
		 {"skipDuration", PropertyValue(TemporalDuration{3, 4, 5})},
		 {"skipNull", PropertyValue(std::monostate{})},
		 {"skipMap", PropertyValue(std::move(ignoredMap))}});
	ASSERT_NE(mixedTypePropertyId, 0);

	simulateSave();
	dataManager->clearCache();

	const auto emptyKeys = dataManager->bulkLoadPropertyEntityValues({propertyId}, {}, nullptr);
	EXPECT_TRUE(emptyKeys.empty());
	const auto emptyIds = dataManager->bulkLoadPropertyEntityValues({}, {"keep"}, nullptr);
	EXPECT_TRUE(emptyIds.empty());

	const auto loaded = dataManager->bulkLoadPropertyEntityValues(
		{propertyId, propertyId, mixedTypePropertyId}, {"keep", "map", "missing"}, nullptr);

	ASSERT_TRUE(loaded.contains(propertyId));
	const auto &values = loaded.at(propertyId);
	ASSERT_EQ(values.size(), 2U);
	EXPECT_EQ(values.at("keep"), PropertyValue(int64_t(42)));
	ASSERT_TRUE(values.contains("map"));
	EXPECT_EQ(values.at("map").getMap().at("inner"), PropertyValue(int64_t(7)));
	EXPECT_FALSE(values.contains("ignored"));
	EXPECT_FALSE(values.contains("ignoredList"));
	EXPECT_FALSE(values.contains("missing"));

	ASSERT_TRUE(loaded.contains(mixedTypePropertyId));
	const auto &mixedValues = loaded.at(mixedTypePropertyId);
	ASSERT_EQ(mixedValues.size(), 1U);
	EXPECT_EQ(mixedValues.at("keep"), PropertyValue(int64_t(11)));
}

TEST_F(DataManagerTest, BulkLoadPropertyEntityValuesSequentialHandlesMissingAndCorruptSegmentMetadata) {
	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkSelectedPropertyCorruptSegmentNode",
		{{"keep", PropertyValue(int64_t(42))}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_FALSE(segIndex.empty());
	const auto *seg = findSegmentForId(segIndex, propertyId);
	ASSERT_NE(seg, nullptr);
	const size_t segPos = static_cast<size_t>(seg - segIndex.data());
	const uint64_t segmentOffset = seg->segmentOffset;
	const int64_t segmentStartId = seg->startId;

	const int64_t missingId = segIndex.back().endId + 1024;
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityValues({missingId}, {"keep"}, nullptr).empty());

	auto patchedIndex = segIndex;
	auto &patchedEntry = patchedIndex[segPos];
	const SegmentHeader header = dataManager->getSegmentTracker()->getSegmentHeaderCopy(segmentOffset);
	const int64_t outOfSlotId = patchedEntry.startId + static_cast<int64_t>(header.used) + 1;
	patchedEntry.endId = std::max(patchedEntry.endId, outOfSlotId);
	dataManager->getSegmentIndexManager()->setSegmentIndex(Property::typeId, patchedIndex);
	EXPECT_FALSE(dataManager->bulkLoadPropertyEntityValues({outOfSlotId}, {"keep"}, nullptr).contains(outOfSlotId));

	writePropertyActiveFlagOnDisk(testFilePath, segmentOffset, segmentStartId, propertyId, false);
	EXPECT_FALSE(dataManager->bulkLoadPropertyEntityValues({propertyId}, {"keep"}, nullptr).contains(propertyId));

	dataManager->getSegmentTracker()->updateSegmentHeader(segmentOffset, [](SegmentHeader &trackedHeader) {
		trackedHeader.used = 0;
		trackedHeader.inactive_count = 0;
	});
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityValues({propertyId}, {"keep"}, nullptr).empty());

	dataManager->getSegmentTracker()->updateSegmentHeader(segmentOffset, [](SegmentHeader &trackedHeader) {
		trackedHeader.used = PROPERTIES_PER_SEGMENT * 100;
		trackedHeader.inactive_count = 0;
	});
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityValues({propertyId}, {"keep"}, nullptr).empty());
}

TEST_F(DataManagerTest, BulkLoadPropertyEntityColumnsFillsRequestedRowsDirectly) {
	const int64_t firstPropertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkColumnPropertyNode",
		{{"keep", PropertyValue(int64_t(42))}, {"ignored", PropertyValue("skip-me")}});
	ASSERT_NE(firstPropertyId, 0);
	const int64_t secondPropertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkColumnPropertyNode",
		{{"keep", PropertyValue(int64_t(11))}, {"ignored", PropertyValue("skip-me-too")}});
	ASSERT_NE(secondPropertyId, 0);

	simulateSave();
	dataManager->clearCache();

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
	columns.emplace("keep", std::vector<std::optional<PropertyValue>>(4, std::nullopt));
	columns.emplace("missing", std::vector<std::optional<PropertyValue>>(4, std::nullopt));

	auto loadedRows = dataManager->bulkLoadPropertyEntityColumns(
		{firstPropertyId, secondPropertyId, firstPropertyId, 0},
		{0, 1, 2, 3},
		4,
		{"keep", "missing"},
		columns,
		nullptr);

	std::sort(loadedRows.begin(), loadedRows.end());
	EXPECT_EQ(loadedRows, (std::vector<size_t>{0U, 1U, 2U}));
	EXPECT_EQ(columns.at("keep")[0], std::optional<PropertyValue>(PropertyValue(int64_t(42))));
	EXPECT_EQ(columns.at("keep")[1], std::optional<PropertyValue>(PropertyValue(int64_t(11))));
	EXPECT_EQ(columns.at("keep")[2], std::optional<PropertyValue>(PropertyValue(int64_t(42))));
	EXPECT_FALSE(columns.at("keep")[3].has_value());
	EXPECT_FALSE(columns.at("missing")[0].has_value());
	EXPECT_FALSE(columns.at("missing")[1].has_value());
	EXPECT_FALSE(columns.at("missing")[2].has_value());

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> duplicateColumns;
	duplicateColumns.emplace("keep", std::vector<std::optional<PropertyValue>>(2, std::nullopt));
	auto duplicateRows = dataManager->bulkLoadPropertyEntityColumns(
		{firstPropertyId, firstPropertyId, 0},
		{0, 0, 1},
		2,
		{"keep", "keep"},
		duplicateColumns,
		nullptr);
	EXPECT_EQ(duplicateRows, (std::vector<size_t>{0U}));
	EXPECT_EQ(duplicateColumns.at("keep")[0], std::optional<PropertyValue>(PropertyValue(int64_t(42))));
}

TEST_F(DataManagerTest, BulkLoadPropertyEntityColumnsSequentialHandlesOutOfSlotInactiveAndShortReads) {
	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkColumnCorruptSegmentNode",
		{{"keep", PropertyValue(int64_t(42))}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_FALSE(segIndex.empty());
	const auto *seg = findSegmentForId(segIndex, propertyId);
	ASSERT_NE(seg, nullptr);
	const size_t segPos = static_cast<size_t>(seg - segIndex.data());
	const uint64_t segmentOffset = seg->segmentOffset;
	const int64_t segmentStartId = seg->startId;

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
	columns.emplace("keep", std::vector<std::optional<PropertyValue>>(3, std::nullopt));

	auto patchedIndex = segIndex;
	auto &patchedEntry = patchedIndex[segPos];
	const SegmentHeader header = dataManager->getSegmentTracker()->getSegmentHeaderCopy(segmentOffset);
	const int64_t outOfSlotId = patchedEntry.startId + static_cast<int64_t>(header.used) + 1;
	patchedEntry.endId = std::max(patchedEntry.endId, outOfSlotId);
	dataManager->getSegmentIndexManager()->setSegmentIndex(Property::typeId, patchedIndex);
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({outOfSlotId}, {0}, 3, {"keep"}, columns, nullptr).empty());

	writePropertyActiveFlagOnDisk(testFilePath, segmentOffset, segmentStartId, propertyId, false);
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({propertyId}, {1}, 3, {"keep"}, columns, nullptr).empty());

	dataManager->getSegmentTracker()->updateSegmentHeader(segmentOffset, [](SegmentHeader &trackedHeader) {
		trackedHeader.used = 0;
		trackedHeader.inactive_count = 0;
	});
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({propertyId}, {2}, 3, {"keep"}, columns, nullptr).empty());

	dataManager->getSegmentTracker()->updateSegmentHeader(segmentOffset, [](SegmentHeader &trackedHeader) {
		trackedHeader.used = PROPERTIES_PER_SEGMENT * 100;
		trackedHeader.inactive_count = 0;
	});
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({propertyId}, {2}, 3, {"keep"}, columns, nullptr).empty());
}

TEST_F(DataManagerTest, BulkLoadPropertyEntityColumnsHandlesInvalidInputsAndMissingWork) {
	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkColumnInvalidNode",
		{{"keep", PropertyValue(int64_t(42))}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
	columns.emplace("keep", std::vector<std::optional<PropertyValue>>(1, std::nullopt));

	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({}, {}, 1, {"keep"}, columns, nullptr).empty());
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({propertyId}, {}, 1, {"keep"}, columns, nullptr).empty());
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({propertyId}, {0}, 1, {}, columns, nullptr).empty());
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({propertyId}, {0}, 0, {"keep"}, columns, nullptr).empty());
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({propertyId}, {0}, 1, {"missing"}, columns, nullptr).empty());

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> undersizedColumns;
	undersizedColumns.emplace("keep", std::vector<std::optional<PropertyValue>>{});
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns(
		{propertyId}, {0}, 1, {"keep"}, undersizedColumns, nullptr).empty());

	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({0, propertyId}, {0, 9}, 1, {"keep"}, columns, nullptr).empty());

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_FALSE(segIndex.empty());
	const int64_t missingId = segIndex.back().endId + 1024;
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({missingId}, {0}, 1, {"keep"}, columns, nullptr).empty());
}


TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicatesReturnsLoadedAndMatchedRows) {
	const int64_t firstPropertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkPredicateNode",
		{{"keep", PropertyValue(int64_t(42))}, {"kind", PropertyValue("match")}});
	ASSERT_NE(firstPropertyId, 0);
	const int64_t secondPropertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkPredicateNode",
		{{"keep", PropertyValue(int64_t(11))}, {"kind", PropertyValue("miss")}});
	ASSERT_NE(secondPropertyId, 0);

	simulateSave();
	dataManager->clearCache();

	auto result = dataManager->bulkMatchPropertyEntityPredicates(
		{firstPropertyId, secondPropertyId, firstPropertyId, 0},
		{0, 1, 2, 3},
		4,
		{{"keep", PropertyValue(int64_t(42))}, {"kind", PropertyValue("match")}},
		nullptr);

	std::sort(result.loadedRows.begin(), result.loadedRows.end());
	std::sort(result.matchedRows.begin(), result.matchedRows.end());
	EXPECT_EQ(result.loadedRows, (std::vector<size_t>{0U, 1U, 2U}));
	EXPECT_EQ(result.matchedRows, (std::vector<size_t>{0U, 2U}));
	EXPECT_EQ(result.loadedCount, 3U);
	EXPECT_EQ(result.matchedCount, 2U);

	auto duplicateRows = dataManager->bulkMatchPropertyEntityPredicates(
		{firstPropertyId, firstPropertyId, 0},
		{0, 0, 1},
		2,
		{{"keep", PropertyValue(int64_t(42))}},
		nullptr);
	EXPECT_EQ(duplicateRows.loadedRows, (std::vector<size_t>{0U}));
	EXPECT_EQ(duplicateRows.matchedRows, (std::vector<size_t>{0U}));
	EXPECT_EQ(duplicateRows.loadedCount, 1U);
	EXPECT_EQ(duplicateRows.matchedCount, 1U);

	auto countOnly = dataManager->bulkMatchPropertyEntityPredicates(
		{firstPropertyId, secondPropertyId},
		{0, 1},
		2,
		{{"keep", PropertyValue(int64_t(42))}},
		nullptr,
		false);
	EXPECT_EQ(countOnly.loadedRows, (std::vector<size_t>{0U, 1U}));
	EXPECT_TRUE(countOnly.matchedRows.empty());
	EXPECT_EQ(countOnly.loadedCount, 2U);
	EXPECT_EQ(countOnly.matchedCount, 1U);

	PropertyEntityPredicateMatchOptions countOnlyWithoutRows;
	countOnlyWithoutRows.collectLoadedRows = false;
	countOnlyWithoutRows.collectMatchedRows = false;
	auto rowlessCountOnly = dataManager->bulkMatchPropertyEntityPredicates(
		{firstPropertyId, secondPropertyId},
		{0, 1},
		2,
		{{"keep", PropertyValue(int64_t(42))}},
		nullptr,
		countOnlyWithoutRows);
	EXPECT_TRUE(rowlessCountOnly.loadedRows.empty());
	EXPECT_TRUE(rowlessCountOnly.matchedRows.empty());
	EXPECT_EQ(rowlessCountOnly.loadedCount, 2U);
	EXPECT_EQ(rowlessCountOnly.matchedCount, 1U);

	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicates(
		          {firstPropertyId, secondPropertyId, firstPropertyId, 0},
		          {{"keep", PropertyValue(int64_t(42))}},
		          nullptr),
	          2U);
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicates(
		          {firstPropertyId, secondPropertyId},
		          {{"keep", PropertyValue(int64_t(42))}, {"kind", PropertyValue("match")}},
		          nullptr),
	          1U);
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicates(
		          {secondPropertyId},
		          {{"keep", PropertyValue(int64_t(42))}},
		          nullptr),
	          0U);
}

TEST_F(DataManagerTest, BulkCountPropertyEntityPredicatesCountsAcrossAdjacentSegments) {
	const int entityCount = static_cast<int>(PROPERTIES_PER_SEGMENT) + 8;
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(entityCount);
	size_t expectedMatches = 0;
	for (int i = 0; i < entityCount; ++i) {
		const bool shouldMatch = i % 3 == 0;
		const int64_t propertyId = addNodeWithPropertyEntity(
			dataManager,
			"BulkPredicateAdjacentSegmentNode",
			{{"keep", PropertyValue(int64_t(shouldMatch ? 42 : 7))}, {"ordinal", PropertyValue(int64_t(i))}});
		ASSERT_NE(propertyId, 0);
		propertyIds.push_back(propertyId);
		if (shouldMatch) {
			++expectedMatches;
		}
	}

	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	const auto *firstSeg = findSegmentForId(segIndex, propertyIds.front());
	const auto *lastSeg = findSegmentForId(segIndex, propertyIds.back());
	ASSERT_NE(firstSeg, nullptr);
	ASSERT_NE(lastSeg, nullptr);
	ASSERT_NE(firstSeg->segmentOffset, lastSeg->segmentOffset);

	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicates(
		          propertyIds,
		          {{"keep", PropertyValue(int64_t(42))}},
		          nullptr),
	          expectedMatches);
}

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicatesParallelHandlesSegmentEdgeCases) {
	const int entityCount = static_cast<int>(PROPERTIES_PER_SEGMENT) + 1;
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(entityCount);
	for (int i = 0; i < entityCount; ++i) {
		const int64_t id = addNodeWithPropertyEntity(
			dataManager,
			"BulkPredicateParallelNode",
			{{"keep", PropertyValue(int64_t(i + 1))}, {"skip", PropertyValue("not-requested")}});
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

	writePropertyActiveFlagOnDisk(testFilePath, firstSeg->segmentOffset, firstSeg->startId, propertyIds.front(), false);
	writeSegmentHeaderUsedOnDisk(testFilePath, secondSeg->segmentOffset, 0);

	auto patchedIndex = segIndex;
	auto firstIt = std::find_if(patchedIndex.begin(), patchedIndex.end(),
	                            [firstSeg](const SegmentIndexManager::SegmentIndex &entry) {
		                            return entry.segmentOffset == firstSeg->segmentOffset;
	                            });
	ASSERT_NE(firstIt, patchedIndex.end());
	const SegmentHeader firstHeader = dataManager->getSegmentTracker()->getSegmentHeaderCopy(firstSeg->segmentOffset);
	const int64_t outOfSlotId = firstIt->startId + static_cast<int64_t>(firstHeader.used) + 1;
	firstIt->endId = std::max(firstIt->endId, outOfSlotId);
	dataManager->getSegmentIndexManager()->setSegmentIndex(Property::typeId, std::move(patchedIndex));

	concurrent::ThreadPool pool(2);
	const auto result = dataManager->bulkMatchPropertyEntityPredicates(
		{propertyIds.front(), propertyIds[1], outOfSlotId, propertyIds.back()},
		{0, 1, 2, 3},
		4,
		{{"keep", PropertyValue(int64_t(2))}},
		&pool);

	EXPECT_EQ(result.loadedRows, (std::vector<size_t>{1U}));
	EXPECT_EQ(result.matchedRows, (std::vector<size_t>{1U}));
}

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicatesHandlesScalarTypesWithoutMaterialization) {
	const auto expectedDate = TemporalDate{12345};
	const auto expectedDateTime = TemporalDateTime{9876543210};
	const auto expectedDuration = TemporalDuration{2, 3, 4000};
	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkPredicateScalarNode",
		{{"flag", PropertyValue(true)},
		 {"ratio", PropertyValue(3.5)},
		 {"name", PropertyValue("match")},
		 {"date", PropertyValue(expectedDate)},
		 {"datetime", PropertyValue(expectedDateTime)},
		 {"duration", PropertyValue(expectedDuration)}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	auto match = dataManager->bulkMatchPropertyEntityPredicates(
		{propertyId},
		{0},
		1,
		{{"flag", PropertyValue(true)},
		 {"ratio", PropertyValue(3.5)},
		 {"name", PropertyValue("match")},
		 {"date", PropertyValue(expectedDate)},
		 {"datetime", PropertyValue(expectedDateTime)},
		 {"duration", PropertyValue(expectedDuration)}},
		nullptr);
	EXPECT_EQ(match.loadedRows, (std::vector<size_t>{0U}));
	EXPECT_EQ(match.matchedRows, (std::vector<size_t>{0U}));

	auto typeMismatch = dataManager->bulkMatchPropertyEntityPredicates(
		{propertyId},
		{0},
		1,
		{{"ratio", PropertyValue(int64_t(3))}},
		nullptr);
	EXPECT_EQ(typeMismatch.loadedRows, (std::vector<size_t>{0U}));
	EXPECT_TRUE(typeMismatch.matchedRows.empty());
}

TEST_F(DataManagerTest, BulkVisitPropertyEntityValuesStreamsSelectedKeyWithoutColumnMaterialization) {
	const int64_t cnPropertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkVisitNode",
		{{"country", PropertyValue("CN")}, {"age", PropertyValue(int64_t{30})}});
	const int64_t usPropertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkVisitNode",
		{{"country", PropertyValue("US")}, {"age", PropertyValue(int64_t{31})}});
	const int64_t missingPropertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkVisitNode",
		{{"age", PropertyValue(int64_t{32})}});
	ASSERT_NE(cnPropertyId, 0);
	ASSERT_NE(usPropertyId, 0);
	ASSERT_NE(missingPropertyId, 0);

	simulateSave();
	dataManager->clearCache();

	std::map<size_t, std::string> valuesByRow;
	const size_t visited = dataManager->bulkVisitPropertyEntityValues(
		{usPropertyId, cnPropertyId, missingPropertyId, cnPropertyId, 0},
		{2, 0, 1, 3, 4},
		5,
		"country",
		[&](size_t row, const PropertyValue &value) {
			valuesByRow[row] = std::get<std::string>(value.getVariant());
		},
		nullptr);

	EXPECT_EQ(visited, 3U);
	ASSERT_EQ(valuesByRow.size(), 3U);
	EXPECT_EQ(valuesByRow[0], "CN");
	EXPECT_EQ(valuesByRow[2], "US");
	EXPECT_EQ(valuesByRow[3], "CN");
	EXPECT_FALSE(valuesByRow.contains(1));
	EXPECT_FALSE(valuesByRow.contains(4));
}

TEST_F(DataManagerTest, BulkVisitPropertyEntityValuesStreamsScalarAndTemporalValues) {
	const auto expectedDate = TemporalDate::fromYMD(2026, 6, 2);
	const auto expectedDateTime = TemporalDateTime::fromComponents(2026, 6, 2, 12, 34, 56, 789);
	const auto expectedDuration = TemporalDuration::fromComponents(0, 1, 0, 2, 3, 4, 5);
	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkVisitScalarNode",
		{{"active", PropertyValue(true)},
		 {"name", PropertyValue("alice")},
		 {"age", PropertyValue(int64_t{42})},
		 {"score", PropertyValue(9.5)},
		 {"date", PropertyValue(expectedDate)},
		 {"datetime", PropertyValue(expectedDateTime)},
		 {"duration", PropertyValue(expectedDuration)}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	std::unordered_map<std::string, PropertyValue> values;
	auto visitKey = [&](const std::string &key) {
		return dataManager->bulkVisitPropertyEntityValues(
			{propertyId},
			{0},
			1,
			key,
			[&](size_t, const PropertyValue &value) { values[key] = value; },
			nullptr);
	};

	EXPECT_EQ(visitKey("active"), 1U);
	EXPECT_EQ(visitKey("name"), 1U);
	EXPECT_EQ(visitKey("age"), 1U);
	EXPECT_EQ(visitKey("score"), 1U);
	EXPECT_EQ(visitKey("date"), 1U);
	EXPECT_EQ(visitKey("datetime"), 1U);
	EXPECT_EQ(visitKey("duration"), 1U);
	EXPECT_EQ(values.at("active"), PropertyValue(true));
	EXPECT_EQ(values.at("name"), PropertyValue("alice"));
	EXPECT_EQ(values.at("age"), PropertyValue(int64_t{42}));
	EXPECT_EQ(values.at("score"), PropertyValue(9.5));
	EXPECT_EQ(values.at("date"), PropertyValue(expectedDate));
	EXPECT_EQ(values.at("datetime"), PropertyValue(expectedDateTime));
	EXPECT_EQ(values.at("duration"), PropertyValue(expectedDuration));
}

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicatesHandlesStructuredAndNullValues) {
	std::vector<PropertyValue> expectedList{PropertyValue(int64_t(1)), PropertyValue("two")};
	PropertyValue::MapType expectedMap;
	expectedMap.emplace("inner", PropertyValue(int64_t(9)));

	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkPredicateStructuredNode",
		{{"nothing", PropertyValue(std::monostate{})},
		 {"items", PropertyValue(expectedList)},
		 {"meta", PropertyValue(expectedMap)},
		 {"skip", PropertyValue("ignored")}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	auto match = dataManager->bulkMatchPropertyEntityPredicates(
		{propertyId},
		{0},
		1,
		{{"nothing", PropertyValue(std::monostate{})},
		 {"items", PropertyValue(expectedList)},
		 {"meta", PropertyValue(expectedMap)}},
		nullptr);
	EXPECT_EQ(match.loadedRows, (std::vector<size_t>{0U}));
	EXPECT_EQ(match.matchedRows, (std::vector<size_t>{0U}));

	std::vector<PropertyValue> differentList{PropertyValue(int64_t(1)), PropertyValue("other")};
	auto mismatch = dataManager->bulkMatchPropertyEntityPredicates(
		{propertyId},
		{0},
		1,
		{{"items", PropertyValue(differentList)}},
		nullptr);
	EXPECT_EQ(mismatch.loadedRows, (std::vector<size_t>{0U}));
	EXPECT_TRUE(mismatch.matchedRows.empty());
}

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicatesHandlesInvalidInputsAndMissingRows) {
	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkPredicateInvalidNode",
		{{"keep", PropertyValue(int64_t(42))}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	EXPECT_TRUE(dataManager->bulkMatchPropertyEntityPredicates({}, {}, 1, {{"keep", PropertyValue(int64_t(42))}}, nullptr).loadedRows.empty());
	EXPECT_TRUE(dataManager->bulkMatchPropertyEntityPredicates({propertyId}, {}, 1, {{"keep", PropertyValue(int64_t(42))}}, nullptr).loadedRows.empty());
	EXPECT_TRUE(dataManager->bulkMatchPropertyEntityPredicates({propertyId}, {0}, 0, {{"keep", PropertyValue(int64_t(42))}}, nullptr).loadedRows.empty());
	EXPECT_TRUE(dataManager->bulkMatchPropertyEntityPredicates({propertyId}, {0}, 1, {}, nullptr).loadedRows.empty());
	EXPECT_TRUE(dataManager->bulkMatchPropertyEntityPredicates({0, propertyId}, {0, 9}, 1, {{"keep", PropertyValue(int64_t(42))}}, nullptr).loadedRows.empty());

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_FALSE(segIndex.empty());
	const int64_t missingId = segIndex.back().endId + 1024;
	EXPECT_TRUE(dataManager->bulkMatchPropertyEntityPredicates({missingId}, {0}, 1, {{"keep", PropertyValue(int64_t(42))}}, nullptr).loadedRows.empty());
}

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicatesSequentialHandlesCorruptSegmentMetadata) {
	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkPredicateCorruptSegmentNode",
		{{"keep", PropertyValue(int64_t(42))}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_FALSE(segIndex.empty());
	const auto *seg = findSegmentForId(segIndex, propertyId);
	ASSERT_NE(seg, nullptr);
	const size_t segPos = static_cast<size_t>(seg - segIndex.data());
	const uint64_t segmentOffset = seg->segmentOffset;
	const int64_t segmentStartId = seg->startId;

	auto patchedIndex = segIndex;
	auto &patchedEntry = patchedIndex[segPos];
	const SegmentHeader header = dataManager->getSegmentTracker()->getSegmentHeaderCopy(segmentOffset);
	const int64_t outOfSlotId = patchedEntry.startId + static_cast<int64_t>(header.used) + 1;
	patchedEntry.endId = std::max(patchedEntry.endId, outOfSlotId);
	dataManager->getSegmentIndexManager()->setSegmentIndex(Property::typeId, patchedIndex);
	EXPECT_TRUE(dataManager->bulkMatchPropertyEntityPredicates(
		{outOfSlotId}, {0}, 1, {{"keep", PropertyValue(int64_t(42))}}, nullptr).loadedRows.empty());

	writePropertyActiveFlagOnDisk(testFilePath, segmentOffset, segmentStartId, propertyId, false);
	EXPECT_TRUE(dataManager->bulkMatchPropertyEntityPredicates(
		{propertyId}, {0}, 1, {{"keep", PropertyValue(int64_t(42))}}, nullptr).loadedRows.empty());

	dataManager->getSegmentTracker()->updateSegmentHeader(segmentOffset, [](SegmentHeader &trackedHeader) {
		trackedHeader.used = 0;
		trackedHeader.inactive_count = 0;
	});
	EXPECT_TRUE(dataManager->bulkMatchPropertyEntityPredicates(
		{propertyId}, {0}, 1, {{"keep", PropertyValue(int64_t(42))}}, nullptr).loadedRows.empty());

	dataManager->getSegmentTracker()->updateSegmentHeader(segmentOffset, [](SegmentHeader &trackedHeader) {
		trackedHeader.used = PROPERTIES_PER_SEGMENT * 100;
		trackedHeader.inactive_count = 0;
	});
	EXPECT_TRUE(dataManager->bulkMatchPropertyEntityPredicates(
		{propertyId}, {0}, 1, {{"keep", PropertyValue(int64_t(42))}}, nullptr).loadedRows.empty());
}

TEST_F(DataManagerTest, BulkLoadPropertyEntityColumnsParallelSkipsInactiveAndMissingRows) {
	const int entityCount = static_cast<int>(PROPERTIES_PER_SEGMENT) + 1;
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(entityCount);
	for (int i = 0; i < entityCount; ++i) {
		const int64_t id = addNodeWithPropertyEntity(
			dataManager,
			"BulkColumnParallelNode",
			{{"keep", PropertyValue(int64_t(i + 1))}, {"skip", PropertyValue("not-requested")}});
		ASSERT_NE(id, 0);
		propertyIds.push_back(id);
	}

	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_GE(segIndex.size(), 2U);
	const auto *firstSeg = findSegmentForId(segIndex, propertyIds.front());
	ASSERT_NE(firstSeg, nullptr);
	writePropertyActiveFlagOnDisk(testFilePath, firstSeg->segmentOffset, firstSeg->startId, propertyIds.front(), false);

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
	columns.emplace("keep", std::vector<std::optional<PropertyValue>>(3, std::nullopt));
	concurrent::ThreadPool pool(2);
	const int64_t missingId = segIndex.back().endId + 1024;

	auto loadedRows = dataManager->bulkLoadPropertyEntityColumns(
		{propertyIds.front(), propertyIds.back(), missingId},
		{0, 1, 2},
		3,
		{"keep"},
		columns,
		&pool);

	std::sort(loadedRows.begin(), loadedRows.end());
	EXPECT_EQ(loadedRows, (std::vector<size_t>{1U}));
	EXPECT_FALSE(columns.at("keep")[0].has_value());
	ASSERT_TRUE(columns.at("keep")[1].has_value());
	EXPECT_EQ(columns.at("keep")[1].value(), PropertyValue(int64_t(entityCount)));
	EXPECT_FALSE(columns.at("keep")[2].has_value());
}

TEST_F(DataManagerTest, ParallelColumnAndPredicateScansSkipMismatchedIdsAndZeroUsedSegments) {
	const int entityCount = static_cast<int>(PROPERTIES_PER_SEGMENT) + 1;
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(entityCount);
	for (int i = 0; i < entityCount; ++i) {
		const int64_t id = addNodeWithPropertyEntity(
			dataManager,
			"BulkParallelMismatchedIdNode",
			{{"keep", PropertyValue(int64_t(i + 1))}});
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
	writeEntityIdOnDisk<Property>(
		testFilePath, firstSeg->segmentOffset, firstSeg->startId, propertyIds.front(), propertyIds.front() + 10'000);
	writeSegmentHeaderUsedOnDisk(testFilePath, secondSeg->segmentOffset, 0);

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
	columns.emplace("keep", std::vector<std::optional<PropertyValue>>(3, std::nullopt));
	concurrent::ThreadPool pool(2);
	auto loadedRows = dataManager->bulkLoadPropertyEntityColumns(
		{propertyIds.front(), propertyIds[1], propertyIds.back()},
		{0, 1, 2},
		3,
		{"keep"},
		columns,
		&pool);
	std::sort(loadedRows.begin(), loadedRows.end());
	EXPECT_EQ(loadedRows, (std::vector<size_t>{1U}));
	EXPECT_FALSE(columns.at("keep")[0].has_value());
	ASSERT_TRUE(columns.at("keep")[1].has_value());
	EXPECT_EQ(columns.at("keep")[1].value(), PropertyValue(int64_t(2)));
	EXPECT_FALSE(columns.at("keep")[2].has_value());

	auto matches = dataManager->bulkMatchPropertyEntityPredicates(
		{propertyIds.front(), propertyIds[1], propertyIds.back()},
		{0, 1, 2},
		3,
		{{"keep", PropertyValue(int64_t(2))}},
		&pool);
	std::sort(matches.loadedRows.begin(), matches.loadedRows.end());
	std::sort(matches.matchedRows.begin(), matches.matchedRows.end());
	EXPECT_EQ(matches.loadedRows, (std::vector<size_t>{1U}));
	EXPECT_EQ(matches.matchedRows, (std::vector<size_t>{1U}));
}

TEST_F(DataManagerTest, BulkLoadPropertyEntityValuesParallelSkipsInactiveAndMissingRows) {
	const int entityCount = static_cast<int>(PROPERTIES_PER_SEGMENT) + 1;
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(entityCount);
	for (int i = 0; i < entityCount; ++i) {
		const int64_t id = addNodeWithPropertyEntity(
			dataManager,
			"BulkSelectedPropertyParallelNode",
			{{"k", PropertyValue(int64_t(i + 1))}, {"skip", PropertyValue("not-requested")}});
		ASSERT_NE(id, 0);
		propertyIds.push_back(id);
	}

	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	ASSERT_GE(segIndex.size(), 2U);
	const auto *firstSeg = findSegmentForId(segIndex, propertyIds.front());
	ASSERT_NE(firstSeg, nullptr);
	writePropertyActiveFlagOnDisk(testFilePath, firstSeg->segmentOffset, firstSeg->startId, propertyIds.front(), false);

	concurrent::ThreadPool pool(2);
	const int64_t missingId = segIndex.back().endId + 1024;
	const auto loaded = dataManager->bulkLoadPropertyEntityValues(
		{propertyIds.front(), propertyIds.back(), missingId}, {"k"}, &pool);

	EXPECT_FALSE(loaded.contains(propertyIds.front()));
	ASSERT_TRUE(loaded.contains(propertyIds.back()));
	EXPECT_EQ(loaded.at(propertyIds.back()).at("k"), PropertyValue(int64_t(entityCount)));
	EXPECT_FALSE(loaded.contains(missingId));
}

TEST_F(DataManagerTest, BulkLoadPropertyEntityValuesParallelHandlesSegmentEdgeCases) {
	const int entityCount = static_cast<int>(PROPERTIES_PER_SEGMENT) + 1;
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(entityCount);
	for (int i = 0; i < entityCount; ++i) {
		const int64_t id = addNodeWithPropertyEntity(
			dataManager,
			"BulkSelectedPropertyParallelEdgeCaseNode",
			{{"k", PropertyValue(int64_t(i + 1))}});
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
	writeSegmentHeaderUsedOnDisk(testFilePath, secondSeg->segmentOffset, 0);

	auto patchedIndex = segIndex;
	auto firstIt = std::find_if(patchedIndex.begin(), patchedIndex.end(),
								[firstSeg](const SegmentIndexManager::SegmentIndex &entry) {
									return entry.segmentOffset == firstSeg->segmentOffset;
								});
	ASSERT_NE(firstIt, patchedIndex.end());
	const SegmentHeader firstHeader = dataManager->getSegmentTracker()->getSegmentHeaderCopy(firstSeg->segmentOffset);
	const int64_t outOfSlotId = firstIt->startId + static_cast<int64_t>(firstHeader.used) + 1;
	firstIt->endId = std::max(firstIt->endId, outOfSlotId);
	SegmentIndexManager::SegmentIndex fakeSeg{};
	fakeSeg.startId = segIndex.back().endId + 100;
	fakeSeg.endId = fakeSeg.startId;
	fakeSeg.segmentOffset = segIndex.back().segmentOffset + static_cast<uint64_t>(TOTAL_SEGMENT_SIZE) * 10'000ULL;
	patchedIndex.push_back(fakeSeg);
	dataManager->getSegmentIndexManager()->setSegmentIndex(Property::typeId, std::move(patchedIndex));

	concurrent::ThreadPool pool(2);
	const auto loaded = dataManager->bulkLoadPropertyEntityValues(
		{propertyIds[1], outOfSlotId, propertyIds.back(), fakeSeg.startId}, {"k"}, &pool);

	ASSERT_TRUE(loaded.contains(propertyIds[1]));
	EXPECT_EQ(loaded.at(propertyIds[1]).at("k"), PropertyValue(int64_t(2)));
	EXPECT_FALSE(loaded.contains(outOfSlotId));
	EXPECT_FALSE(loaded.contains(propertyIds.back()));
	EXPECT_FALSE(loaded.contains(fakeSeg.startId));
}

TEST_F(DataManagerTest, BulkLoadPropertyEntityValuesIgnoreZeroSerializedId) {
	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkSelectedPropertyZeroIdNode",
		{{"keep", PropertyValue(int64_t(42))}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	const auto *seg = findSegmentForId(segIndex, propertyId);
	ASSERT_NE(seg, nullptr);
	writeEntityIdOnDisk<Property>(testFilePath, seg->segmentOffset, seg->startId, propertyId, 0);

	const auto loaded = dataManager->bulkLoadPropertyEntityValues({propertyId}, {"keep"}, nullptr);
	EXPECT_FALSE(loaded.contains(propertyId));
}

TEST_F(DataManagerTest, PropertyReadersRejectInvalidKeyLength) {
	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkInvalidKeyLengthNode",
		{{"keep", PropertyValue(int64_t(42))}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	const auto *seg = findSegmentForId(segIndex, propertyId);
	ASSERT_NE(seg, nullptr);
	writePropertyKeyLengthOnDisk(testFilePath, seg->segmentOffset, seg->startId, propertyId, Property::getTotalSize());

	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityValues({propertyId}, {"keep"}, nullptr).empty());

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
	columns.emplace("keep", std::vector<std::optional<PropertyValue>>(1, std::nullopt));
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({propertyId}, {0}, 1, {"keep"}, columns, nullptr).empty());
	EXPECT_FALSE(columns.at("keep")[0].has_value());

	auto matches = dataManager->bulkMatchPropertyEntityPredicates(
		{propertyId}, {0}, 1, {{"keep", PropertyValue(int64_t(42))}}, nullptr);
	EXPECT_TRUE(matches.loadedRows.empty());
	EXPECT_TRUE(matches.matchedRows.empty());
}

TEST_F(DataManagerTest, PropertyReadersRejectUnknownSkippedValueType) {
	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkUnknownSkippedValueNode",
		{{"skip", PropertyValue(int64_t(42))}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	const auto *seg = findSegmentForId(segIndex, propertyId);
	ASSERT_NE(seg, nullptr);
	writeFirstPropertyValueTypeOnDisk(testFilePath, seg->segmentOffset, seg->startId, propertyId, "skip", PropertyType::UNKNOWN);

	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityValues({propertyId}, {"keep"}, nullptr).empty());

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
	columns.emplace("keep", std::vector<std::optional<PropertyValue>>(1, std::nullopt));
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({propertyId}, {0}, 1, {"keep"}, columns, nullptr).empty());
	EXPECT_FALSE(columns.at("keep")[0].has_value());

	auto matches = dataManager->bulkMatchPropertyEntityPredicates(
		{propertyId}, {0}, 1, {{"keep", PropertyValue(int64_t(42))}}, nullptr);
	EXPECT_TRUE(matches.loadedRows.empty());
	EXPECT_TRUE(matches.matchedRows.empty());
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
	// Create enough entities to guarantee at least 2 property segments
	const int entityCount = static_cast<int>(PROPERTIES_PER_SEGMENT) + 1;
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(entityCount);
	for (int i = 0; i < entityCount; ++i) {
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
	// Create enough entities to guarantee at least 2 property segments
	const int entityCount = static_cast<int>(PROPERTIES_PER_SEGMENT) + 1;
	std::vector<int64_t> propertyIds;
	propertyIds.reserve(entityCount);
	for (int i = 0; i < entityCount; ++i) {
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
	// Place outOfSlotId beyond the actual used count so slot >= header.used
	const SegmentHeader firstHeader = dataManager->getSegmentTracker()->getSegmentHeaderCopy(firstSeg->segmentOffset);
	const int64_t outOfSlotId = firstIt->startId + static_cast<int64_t>(firstHeader.used) + 1;
	firstIt->endId = std::max(firstIt->endId, outOfSlotId);
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

TEST_F(DataManagerTest, BulkLoadEntitiesSkipsZeroUsedSegmentsForAllPublicTypes) {
	auto source = createTestNode(dataManager, "BulkEntityZeroUsedNode");
	auto target = createTestNode(dataManager, "BulkEntityZeroUsedNode");
	dataManager->addNode(source);
	dataManager->addNode(target);
	auto edge = createTestEdge(dataManager, source.getId(), target.getId(), "BULK_ZERO_USED_EDGE");
	dataManager->addEdge(edge);
	const int64_t propertyId = addNodeWithSinglePropertyEntity(dataManager, "BulkEntityZeroUsedProperty", 7);
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	const auto &nodeSegments = dataManager->getSegmentIndexManager()->getNodeSegmentIndex();
	const auto &edgeSegments = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	const auto &propertySegments = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	const auto *nodeSeg = findSegmentForId(nodeSegments, source.getId());
	const auto *edgeSeg = findSegmentForId(edgeSegments, edge.getId());
	const auto *propertySeg = findSegmentForId(propertySegments, propertyId);
	ASSERT_NE(nodeSeg, nullptr);
	ASSERT_NE(edgeSeg, nullptr);
	ASSERT_NE(propertySeg, nullptr);

	dataManager->getSegmentTracker()->updateSegmentHeader(nodeSeg->segmentOffset, [](SegmentHeader &header) {
		header.used = 0;
		header.inactive_count = 0;
	});
	dataManager->getSegmentTracker()->updateSegmentHeader(edgeSeg->segmentOffset, [](SegmentHeader &header) {
		header.used = 0;
		header.inactive_count = 0;
	});
	dataManager->getSegmentTracker()->updateSegmentHeader(propertySeg->segmentOffset, [](SegmentHeader &header) {
		header.used = 0;
		header.inactive_count = 0;
	});

	EXPECT_TRUE(dataManager->bulkLoadEntities<Node>(source.getId(), source.getId()).empty());
	EXPECT_TRUE(dataManager->bulkLoadEntities<Edge>(edge.getId(), edge.getId()).empty());
	EXPECT_TRUE(dataManager->bulkLoadEntities<Property>(propertyId, propertyId).empty());
}

TEST_F(DataManagerTest, BulkLoadEntitiesSkipSegmentsWhenReadsAreShort) {
	auto source = createTestNode(dataManager, "BulkEntityShortReadNode");
	auto target = createTestNode(dataManager, "BulkEntityShortReadNode");
	dataManager->addNode(source);
	dataManager->addNode(target);
	auto edge = createTestEdge(dataManager, source.getId(), target.getId(), "BULK_SHORT_READ_EDGE");
	dataManager->addEdge(edge);
	const int64_t propertyId = addNodeWithSinglePropertyEntity(dataManager, "BulkEntityShortReadProperty", 9);
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	const auto *nodeSeg = findSegmentForId(dataManager->getSegmentIndexManager()->getNodeSegmentIndex(), source.getId());
	const auto *edgeSeg = findSegmentForId(dataManager->getSegmentIndexManager()->getEdgeSegmentIndex(), edge.getId());
	const auto *propertySeg = findSegmentForId(dataManager->getSegmentIndexManager()->getPropertySegmentIndex(), propertyId);
	ASSERT_NE(nodeSeg, nullptr);
	ASSERT_NE(edgeSeg, nullptr);
	ASSERT_NE(propertySeg, nullptr);

	dataManager->getSegmentTracker()->updateSegmentHeader(nodeSeg->segmentOffset, [](SegmentHeader &header) {
		header.used = NODES_PER_SEGMENT * 100;
		header.inactive_count = 0;
	});
	dataManager->getSegmentTracker()->updateSegmentHeader(edgeSeg->segmentOffset, [](SegmentHeader &header) {
		header.used = EDGES_PER_SEGMENT * 100;
		header.inactive_count = 0;
	});
	dataManager->getSegmentTracker()->updateSegmentHeader(propertySeg->segmentOffset, [](SegmentHeader &header) {
		header.used = PROPERTIES_PER_SEGMENT * 100;
		header.inactive_count = 0;
	});

	EXPECT_TRUE(dataManager->bulkLoadEntities<Node>(source.getId(), source.getId()).empty());
	EXPECT_TRUE(dataManager->bulkLoadEntities<Edge>(edge.getId(), edge.getId()).empty());
	EXPECT_TRUE(dataManager->bulkLoadEntities<Property>(propertyId, propertyId).empty());
}

TEST_F(DataManagerTest, BulkLoadEntitiesHonorsTightUpperBoundWithinSegment) {
	auto first = createTestNode(dataManager, "BulkEntityFilterNode");
	auto second = createTestNode(dataManager, "BulkEntityFilterNode");
	dataManager->addNode(first);
	dataManager->addNode(second);

	simulateSave();
	dataManager->clearCache();

	auto loaded = dataManager->bulkLoadEntities<Node>(first.getId(), first.getId());

	ASSERT_EQ(loaded.size(), 1U);
	EXPECT_EQ(loaded.front().getId(), first.getId());
}

TEST_F(DataManagerTest, DirectDiskLoadsTreatInactivePersistedEntitiesAsMissing) {
	auto source = createTestNode(dataManager, "InactiveDirectLoadNode");
	auto target = createTestNode(dataManager, "InactiveDirectLoadNode");
	dataManager->addNode(source);
	dataManager->addNode(target);
	auto edge = createTestEdge(dataManager, source.getId(), target.getId(), "INACTIVE_DIRECT_EDGE");
	dataManager->addEdge(edge);
	const int64_t propertyId = addNodeWithSinglePropertyEntity(dataManager, "InactiveDirectPropertyNode", 5);
	ASSERT_NE(propertyId, 0);
	auto blob = createTestBlob("inactive blob");
	dataManager->addBlobEntity(blob);
	auto index = createTestIndex(Index::NodeType::LEAF, 321);
	dataManager->addIndexEntity(index);
	auto state = createTestState("inactive.direct.state");
	dataManager->addStateEntity(state);

	simulateSave();
	dataManager->clearCache();

	const auto *nodeSeg = findSegmentForId(dataManager->getSegmentIndexManager()->getNodeSegmentIndex(), source.getId());
	const auto *edgeSeg = findSegmentForId(dataManager->getSegmentIndexManager()->getEdgeSegmentIndex(), edge.getId());
	const auto *propertySeg = findSegmentForId(dataManager->getSegmentIndexManager()->getPropertySegmentIndex(), propertyId);
	const auto *blobSeg = findSegmentForId(dataManager->getSegmentIndexManager()->getBlobSegmentIndex(), blob.getId());
	const auto *indexSeg = findSegmentForId(dataManager->getSegmentIndexManager()->getIndexSegmentIndex(), index.getId());
	const auto *stateSeg = findSegmentForId(dataManager->getSegmentIndexManager()->getStateSegmentIndex(), state.getId());
	ASSERT_NE(nodeSeg, nullptr);
	ASSERT_NE(edgeSeg, nullptr);
	ASSERT_NE(propertySeg, nullptr);
	ASSERT_NE(blobSeg, nullptr);
	ASSERT_NE(indexSeg, nullptr);
	ASSERT_NE(stateSeg, nullptr);

	Node inactiveNode = source;
	inactiveNode.markInactive();
	Edge inactiveEdge = edge;
	inactiveEdge.markInactive();
	Property inactiveProperty = dataManager->loadPropertyFromDisk(propertyId);
	inactiveProperty.markInactive();
	Blob inactiveBlob = blob;
	inactiveBlob.markInactive();
	Index inactiveIndex = index;
	inactiveIndex.markInactive();
	State inactiveState = state;
	inactiveState.markInactive();

	writeSerializedEntityOnDisk<Node>(testFilePath, nodeSeg->segmentOffset, nodeSeg->startId, source.getId(), inactiveNode);
	writeSerializedEntityOnDisk<Edge>(testFilePath, edgeSeg->segmentOffset, edgeSeg->startId, edge.getId(), inactiveEdge);
	writeSerializedEntityOnDisk<Property>(
		testFilePath, propertySeg->segmentOffset, propertySeg->startId, propertyId, inactiveProperty);
	writeSerializedEntityOnDisk<Blob>(testFilePath, blobSeg->segmentOffset, blobSeg->startId, blob.getId(), inactiveBlob);
	writeSerializedEntityOnDisk<Index>(testFilePath, indexSeg->segmentOffset, indexSeg->startId, index.getId(), inactiveIndex);
	writeSerializedEntityOnDisk<State>(testFilePath, stateSeg->segmentOffset, stateSeg->startId, state.getId(), inactiveState);
	dataManager->clearCache();

	EXPECT_EQ(dataManager->loadNodeFromDisk(source.getId()).getId(), 0);
	EXPECT_EQ(dataManager->loadEdgeFromDisk(edge.getId()).getId(), 0);
	EXPECT_EQ(dataManager->loadPropertyFromDisk(propertyId).getId(), 0);
	EXPECT_EQ(dataManager->loadBlobFromDisk(blob.getId()).getId(), 0);
	EXPECT_EQ(dataManager->loadIndexFromDisk(index.getId()).getId(), 0);
	EXPECT_EQ(dataManager->loadStateFromDisk(state.getId()).getId(), 0);
}

TEST_F(DataManagerTest, PropertyColumnAndPredicateScansSkipMismatchedSerializedIds) {
	const int64_t propertyId = addNodeWithPropertyEntity(
		dataManager,
		"MismatchedPropertyIdNode",
		{{"keep", PropertyValue(int64_t(42))}});
	ASSERT_NE(propertyId, 0);

	simulateSave();
	dataManager->clearCache();

	const auto &segIndex = dataManager->getSegmentIndexManager()->getPropertySegmentIndex();
	const auto *seg = findSegmentForId(segIndex, propertyId);
	ASSERT_NE(seg, nullptr);
	writeEntityIdOnDisk<Property>(testFilePath, seg->segmentOffset, seg->startId, propertyId, propertyId + 1000);

	std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>> columns;
	columns.emplace("keep", std::vector<std::optional<PropertyValue>>(1, std::nullopt));
	EXPECT_TRUE(dataManager->bulkLoadPropertyEntityColumns({propertyId}, {0}, 1, {"keep"}, columns, nullptr).empty());
	EXPECT_FALSE(columns.at("keep")[0].has_value());

	auto matches = dataManager->bulkMatchPropertyEntityPredicates(
		{propertyId}, {0}, 1, {{"keep", PropertyValue(int64_t(42))}}, nullptr);
	EXPECT_TRUE(matches.loadedRows.empty());
	EXPECT_TRUE(matches.matchedRows.empty());
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
		// Set used far beyond what the file could contain to guarantee a short read from pread
		header.used = PROPERTIES_PER_SEGMENT * 100;
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

TEST_F(DataManagerTest, DirtyEntriesWithDefaultEntityIdsAreIgnoredForAllEntityKinds) {
	dataManager->setEntityDirty(DirtyEntityInfo<Node>(EntityChangeType::CHANGE_MODIFIED, Node{}));
	dataManager->setEntityDirty(DirtyEntityInfo<Edge>(EntityChangeType::CHANGE_MODIFIED, Edge{}));
	dataManager->setEntityDirty(DirtyEntityInfo<Property>(EntityChangeType::CHANGE_MODIFIED, Property{}));
	dataManager->setEntityDirty(DirtyEntityInfo<Blob>(EntityChangeType::CHANGE_MODIFIED, Blob{}));
	dataManager->setEntityDirty(DirtyEntityInfo<Index>(EntityChangeType::CHANGE_MODIFIED, Index{}));
	dataManager->setEntityDirty(DirtyEntityInfo<State>(EntityChangeType::CHANGE_MODIFIED, State{}));
}

TEST_F(DataManagerTest, BulkMatchPropertyEntityPredicateSpecsSupportsComparisonOps) {
	const int64_t firstPropertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkPredicateSpecNode",
		{{"age", PropertyValue(int64_t(30))}, {"score", PropertyValue(int64_t(900))}, {"country", PropertyValue("CN")}});
	const int64_t secondPropertyId = addNodeWithPropertyEntity(
		dataManager,
		"BulkPredicateSpecNode",
		{{"age", PropertyValue(int64_t(25))}, {"score", PropertyValue(int64_t(100))}, {"country", PropertyValue("US")}});
	ASSERT_NE(firstPropertyId, 0);
	ASSERT_NE(secondPropertyId, 0);

	simulateSave();
	dataManager->clearCache();

	using Op = PropertyEntityPredicateOp;
	const std::vector<int64_t> ids{firstPropertyId, secondPropertyId};
	const std::vector<size_t> rows{0, 1};

	auto firstOnly = dataManager->bulkMatchPropertyEntityPredicateSpecs(
		ids,
		rows,
		2,
		{{"age", Op::PEP_GT, PropertyValue(int64_t(29)), std::nullopt},
		 {"score", Op::PEP_LE, PropertyValue(int64_t(900)), std::nullopt},
		 {"country", Op::PEP_NE, PropertyValue("US"), std::nullopt}},
		nullptr);
	EXPECT_EQ(firstOnly.loadedRows, (std::vector<size_t>{0U, 1U}));
	EXPECT_EQ(firstOnly.matchedRows, (std::vector<size_t>{0U}));

	PropertyEntityPredicateMatchOptions countOnlyOptions;
	countOnlyOptions.collectLoadedRows = false;
	countOnlyOptions.collectMatchedRows = false;
	auto firstOnlyCounts = dataManager->bulkMatchPropertyEntityPredicateSpecs(
		ids,
		rows,
		2,
		{{"age", Op::PEP_GT, PropertyValue(int64_t(29)), std::nullopt},
		 {"score", Op::PEP_LE, PropertyValue(int64_t(900)), std::nullopt},
		 {"country", Op::PEP_NE, PropertyValue("US"), std::nullopt}},
		nullptr,
		countOnlyOptions);
	EXPECT_TRUE(firstOnlyCounts.loadedRows.empty());
	EXPECT_TRUE(firstOnlyCounts.matchedRows.empty());
	EXPECT_EQ(firstOnlyCounts.loadedCount, 2U);
	EXPECT_EQ(firstOnlyCounts.matchedCount, 1U);

	auto secondOnly = dataManager->bulkMatchPropertyEntityPredicateSpecs(
		ids,
		rows,
		2,
		{{"age", Op::PEP_LT, PropertyValue(int64_t(30)), std::nullopt},
		 {"score", Op::PEP_GE, PropertyValue(int64_t(100)), std::nullopt}},
		nullptr);
	EXPECT_EQ(secondOnly.loadedRows, (std::vector<size_t>{0U, 1U}));
	EXPECT_EQ(secondOnly.matchedRows, (std::vector<size_t>{1U}));

	auto rangeMatch = dataManager->bulkMatchPropertyEntityPredicateSpecs(
		ids,
		rows,
		2,
		{{"age", Op::PEP_RANGE_CLOSED, PropertyValue(int64_t(25)), PropertyValue(int64_t(30))}},
		nullptr);
	EXPECT_EQ(rangeMatch.loadedRows, (std::vector<size_t>{0U, 1U}));
	EXPECT_EQ(rangeMatch.matchedRows, (std::vector<size_t>{0U, 1U}));

	auto repeatedKeyRange = dataManager->bulkMatchPropertyEntityPredicateSpecs(
		ids,
		rows,
		2,
		{{"age", Op::PEP_GE, PropertyValue(int64_t(26)), std::nullopt},
		 {"age", Op::PEP_LT, PropertyValue(int64_t(31)), std::nullopt}},
		nullptr);
	EXPECT_EQ(repeatedKeyRange.loadedRows, (std::vector<size_t>{0U, 1U}));
	EXPECT_EQ(repeatedKeyRange.matchedRows, (std::vector<size_t>{0U}));

	auto missingUpperBound = dataManager->bulkMatchPropertyEntityPredicateSpecs(
		ids,
		rows,
		2,
		{{"age", Op::PEP_RANGE_CLOSED, PropertyValue(int64_t(25)), std::nullopt}},
		nullptr);
	EXPECT_EQ(missingUpperBound.loadedRows, (std::vector<size_t>{0U, 1U}));
	EXPECT_TRUE(missingUpperBound.matchedRows.empty());

	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicateSpecs(
				  {firstPropertyId, secondPropertyId, firstPropertyId, 0},
				  {{"age", Op::PEP_GT, PropertyValue(int64_t(29)), std::nullopt},
				   {"score", Op::PEP_LE, PropertyValue(int64_t(900)), std::nullopt},
				   {"country", Op::PEP_NE, PropertyValue("US"), std::nullopt}},
				  nullptr),
			  2U);
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicateSpecs(
				  ids,
				  {{"age", Op::PEP_LT, PropertyValue(int64_t(30)), std::nullopt},
				   {"score", Op::PEP_GE, PropertyValue(int64_t(100)), std::nullopt}},
				  nullptr),
			  1U);
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicateSpecs(
				  ids,
				  {{"country", Op::PEP_RANGE_CLOSED, PropertyValue("CA"), PropertyValue("NZ")}},
				  nullptr),
			  1U);
	EXPECT_EQ(dataManager->bulkCountPropertyEntityPredicateSpecs(
				  ids,
				  {{"age", Op::PEP_RANGE_CLOSED, PropertyValue(int64_t(25)), std::nullopt}},
				  nullptr),
			  0U);
}
