#include <gtest/gtest.h>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/RelationshipMetadataColumnLoader.hpp"
#include "graph/query/execution/RelationshipPropertyColumnLoader.hpp"
#include "graph/storage/CommittedSnapshot.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/data/DirtyEntityInfo.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;

namespace {

	Edge makeEdge(int64_t id) {
		Edge edge(id, 1, 2, 3);
		return edge;
	}

} // namespace

TEST(RelationshipPropertyColumnLoaderTest, EmptyRequestedPropertiesReturnEmptyColumns) {
	Edge edge = makeEdge(1);
	edge.addProperty("weight", PropertyValue(int64_t{7}));
	RelationshipPropertyColumnLoader loader(nullptr);

	auto columns = loader.loadColumns({edge}, {}, {});

	EXPECT_TRUE(columns.empty());
}

TEST(RelationshipPropertyColumnLoaderTest, InlinePropertiesPopulateOnlyRequestedColumns) {
	Edge edge = makeEdge(1);
	edge.addProperty("weight", PropertyValue(int64_t{7}));
	edge.addProperty("kind", PropertyValue("direct"));
	RelationshipPropertyColumnLoader loader(nullptr);

	auto columns = loader.loadColumns({edge}, {}, {"weight"});

	ASSERT_EQ(columns.size(), 1U);
	ASSERT_TRUE(columns.contains("weight"));
	ASSERT_EQ(columns["weight"].size(), 1U);
	ASSERT_TRUE(columns["weight"][0].has_value());
	EXPECT_EQ(columns["weight"][0].value(), PropertyValue(int64_t{7}));
	EXPECT_FALSE(columns.contains("kind"));
}

TEST(RelationshipPropertyColumnLoaderTest, SelectionAndDuplicateKeysAreHandled) {
	Edge first = makeEdge(1);
	first.addProperty("weight", PropertyValue(int64_t{1}));
	Edge second = makeEdge(2);
	second.addProperty("weight", PropertyValue(int64_t{2}));
	RelationshipPropertyColumnLoader loader(nullptr);

	auto columns = loader.loadColumns({first, second}, {1, 0}, {"weight", "weight"});

	ASSERT_EQ(columns.size(), 1U);
	ASSERT_EQ(columns["weight"].size(), 2U);
	ASSERT_TRUE(columns["weight"][0].has_value());
	EXPECT_EQ(columns["weight"][0].value(), PropertyValue(int64_t{1}));
	EXPECT_FALSE(columns["weight"][1].has_value());
}

TEST(RelationshipPropertyColumnLoaderTest, InvalidSelectionVectorSizeReturnsEmptyColumns) {
	Edge edge = makeEdge(1);
	edge.addProperty("weight", PropertyValue(int64_t{7}));
	RelationshipPropertyColumnLoader loader(nullptr);

	auto columns = loader.loadColumns({edge}, {1, 0}, {"weight"});

	EXPECT_TRUE(columns.empty());
}

TEST(RelationshipPropertyColumnLoaderTest, EdgeRowsSkipInactiveInvalidAndUnselectedInputs) {
	Edge selected = makeEdge(1);
	selected.addProperty("weight", PropertyValue(int64_t{7}));
	Edge zeroId = makeEdge(0);
	zeroId.addProperty("weight", PropertyValue(int64_t{8}));
	Edge inactive = makeEdge(2);
	inactive.addProperty("weight", PropertyValue(int64_t{9}));
	inactive.markInactive();
	Edge external = makeEdge(3);
	external.setPropertyEntityId(44, PropertyStorageType::PROPERTY_ENTITY);
	RelationshipPropertyColumnLoader loader(nullptr);

	auto columns = loader.loadColumns({selected, zeroId, inactive, external}, {1, 1, 1, 0}, {"weight"});

	ASSERT_TRUE(columns.contains("weight"));
	ASSERT_EQ(columns["weight"].size(), 4U);
	EXPECT_EQ(columns["weight"][0], std::optional<PropertyValue>(PropertyValue(int64_t{7})));
	EXPECT_FALSE(columns["weight"][1].has_value());
	EXPECT_FALSE(columns["weight"][2].has_value());
	EXPECT_FALSE(columns["weight"][3].has_value());
}

TEST(RelationshipPropertyColumnLoaderTest, MetadataRowsSkipInvalidInactiveAndMissingExternalProperties) {
	RelationshipMetadataBatch metadata;
	metadata.appendDefault();
	metadata.edgeIds[0] = 1;
	metadata.active[0] = 1;
	metadata.propertyEntityIds[0] = 0;
	metadata.propertyStorageTypes[0] = PropertyStorageType::PROPERTY_ENTITY;
	metadata.appendDefault();
	metadata.edgeIds[1] = 2;
	metadata.active[1] = 0;
	metadata.propertyEntityIds[1] = 20;
	metadata.propertyStorageTypes[1] = PropertyStorageType::PROPERTY_ENTITY;
	metadata.appendDefault();
	metadata.edgeIds[2] = 3;
	metadata.active[2] = 1;
	metadata.propertyEntityIds[2] = 30;
	metadata.propertyStorageTypes[2] = PropertyStorageType::BLOB_ENTITY;
	metadata.appendDefault();
	metadata.edgeIds[3] = 4;
	metadata.active[3] = 1;
	metadata.propertyEntityIds[3] = 0;
	metadata.propertyStorageTypes[3] = PropertyStorageType::BLOB_ENTITY;
	RelationshipPropertyColumnLoader loader(nullptr);

	EXPECT_TRUE(loader.loadColumns(metadata, {1, 0}, {"weight"}).empty());
	auto columns = loader.loadColumns(metadata, {}, {"weight"});

	ASSERT_TRUE(columns.contains("weight"));
	ASSERT_EQ(columns["weight"].size(), metadata.size());
	for (const auto &value: columns["weight"]) {
		EXPECT_FALSE(value.has_value());
	}
}

TEST(RelationshipMetadataBatchTest, HandlesInvalidRowsAndEdgeRoundTrip) {
	RelationshipMetadataBatch batch;
	EXPECT_EQ(batch.size(), 0U);
	EXPECT_FALSE(batch.isValid(0));
	EXPECT_EQ(batch.toEdge(0).getId(), 0);

	Edge edge(9, 1, 2, 3);
	edge.getMutableMetadata().propertyEntityId = 4;
	edge.getMutableMetadata().propertyStorageType = static_cast<uint32_t>(PropertyStorageType::PROPERTY_ENTITY);
	Edge inactive = edge;
	inactive.markInactive();
	batch.appendDefault();
	batch.setFromEdge(1, edge);
	EXPECT_FALSE(batch.isValid(0));
	batch.setFromEdge(0, edge);
	batch.appendDefault();
	batch.setFromEdge(1, inactive);

	EXPECT_TRUE(batch.isValid(0));
	Edge restored = batch.toEdge(0);
	EXPECT_EQ(restored.getId(), 9);
	EXPECT_EQ(restored.getSourceNodeId(), 1);
	EXPECT_EQ(restored.getTargetNodeId(), 2);
	EXPECT_EQ(restored.getTypeId(), 3);
	EXPECT_EQ(restored.getPropertyEntityId(), 4);
	EXPECT_EQ(restored.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);
	EXPECT_TRUE(restored.isActive());
	EXPECT_FALSE(batch.toEdge(1).isActive());
}

class RelationshipPropertyColumnLoaderStorageTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() /
					 ("test_relationship_property_column_loader_" + boost::uuids::to_string(uuid) + ".zyx");
		if (fs::exists(testDbPath)) {
			fs::remove_all(testDbPath);
		}
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		userLabel = dm->getOrCreateTokenId("User");
		followsType = dm->getOrCreateTokenId("FOLLOWS");
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

	int64_t addUser() {
		Node node(0, userLabel);
		dm->addNode(node);
		return node.getId();
	}

	Edge addFollowsWithProperties(const std::unordered_map<std::string, PropertyValue> &properties) {
		const int64_t source = addUser();
		const int64_t target = addUser();
		Edge edge(0, source, target, followsType);
		dm->addEdge(edge);
		dm->addEdgeProperties(edge.getId(), properties);
		return dm->getEdge(edge.getId());
	}

	Edge addFollowsWithoutProperties() {
		const int64_t source = addUser();
		const int64_t target = addUser();
		Edge edge(0, source, target, followsType);
		dm->addEdge(edge);
		return dm->getEdge(edge.getId());
	}

	fs::path testDbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	int64_t userLabel = 0;
	int64_t followsType = 0;
};

TEST_F(RelationshipPropertyColumnLoaderStorageTest, ExternalPropertyEntityValuesPopulateRequestedColumns) {
	Edge stored = addFollowsWithProperties({{"weight", PropertyValue(int64_t{1})}, {"kind", PropertyValue("strong")}});
	ASSERT_TRUE(stored.hasPropertyEntity());
	ASSERT_EQ(stored.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);
	RelationshipPropertyColumnLoader loader(dm);

	auto columns = loader.loadColumns({stored}, {}, {"weight"});

	ASSERT_EQ(columns.size(), 1U);
	ASSERT_TRUE(columns.contains("weight"));
	ASSERT_EQ(columns["weight"].size(), 1U);
	ASSERT_TRUE(columns["weight"][0].has_value());
	EXPECT_EQ(columns["weight"][0].value(), PropertyValue(int64_t{1}));
	EXPECT_FALSE(columns.contains("kind"));
}


TEST_F(RelationshipPropertyColumnLoaderStorageTest, MetadataBatchExternalPropertiesPopulateRequestedColumns) {
	Edge stored = addFollowsWithProperties({{"weight", PropertyValue(int64_t{5})}, {"kind", PropertyValue("fast")}});
	ASSERT_TRUE(stored.hasPropertyEntity());
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipMetadataColumnLoader metadataLoader(dm);
	auto metadata = metadataLoader.loadRange(stored.getId(), stored.getId() + 127);
	ASSERT_TRUE(metadata.has_value());
	ASSERT_GE(metadata->size(), 1U);
	ASSERT_EQ(metadata->edgeIds[0], stored.getId());
	ASSERT_EQ(metadata->typeIds[0], followsType);
	ASSERT_EQ(metadata->propertyEntityIds[0], stored.getPropertyEntityId());

	RelationshipPropertyColumnLoader propertyLoader(dm);
	auto columns = propertyLoader.loadColumns(*metadata, {}, {"weight"});

	ASSERT_TRUE(columns.contains("weight"));
	ASSERT_EQ(columns["weight"].size(), metadata->size());
	ASSERT_TRUE(columns["weight"][0].has_value());
	EXPECT_EQ(columns["weight"][0].value(), PropertyValue(int64_t{5}));
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, MetadataBatchHandlesSelectionAndBlobFallback) {
	Edge selected = addFollowsWithProperties({{"weight", PropertyValue(int64_t{5})}});
	std::string largeValue(512, 'z');
	Edge blobBacked =
			addFollowsWithProperties({{"payload", PropertyValue(largeValue)}, {"weight", PropertyValue(int64_t{9})}});
	Edge unselected = addFollowsWithProperties({{"weight", PropertyValue(int64_t{11})}});
	(void) unselected;
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipMetadataColumnLoader metadataLoader(dm);
	auto metadata = metadataLoader.loadRange(1, 128);
	ASSERT_TRUE(metadata.has_value());
	ASSERT_GE(metadata->size(), 3U);

	RelationshipPropertyColumnLoader loader(dm);
	EXPECT_TRUE(loader.loadColumns(*metadata, {}, {}).empty());
	EXPECT_TRUE(loader.loadColumns(*metadata, {1, 0}, {"weight"}).empty());

	std::vector<uint8_t> selectedRows(metadata->size(), 0);
	selectedRows[0] = 1;
	selectedRows[1] = 1;
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	auto columns = loader.loadColumns(*metadata, selectedRows, {"weight", "payload", "weight"});

	ASSERT_TRUE(columns.contains("weight"));
	ASSERT_TRUE(columns.contains("payload"));
	ASSERT_EQ(columns["weight"].size(), metadata->size());
	ASSERT_TRUE(columns["weight"][0].has_value());
	ASSERT_TRUE(columns["weight"][1].has_value());
	EXPECT_EQ(columns["weight"][0].value(), PropertyValue(int64_t{5}));
	EXPECT_EQ(columns["weight"][1].value(), PropertyValue(int64_t{9}));
	ASSERT_TRUE(columns["payload"][1].has_value());
	EXPECT_EQ(columns["payload"][1].value(), PropertyValue(largeValue));
	EXPECT_FALSE(columns["weight"][2].has_value());
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("relationship_count.extract_property_columns"));
	EXPECT_TRUE(snapshot.contains("relationship_count.load_property_entities"));
	EXPECT_EQ(selected.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);
	EXPECT_EQ(blobBacked.getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, MetadataCandidateScanKeepsOnlyEdgesWithStoredProperties) {
	Edge propertyBacked = addFollowsWithProperties({{"weight", PropertyValue(int64_t{5})}});
	std::string largeValue(512, 'x');
	Edge blobBacked =
			addFollowsWithProperties({{"payload", PropertyValue(largeValue)}, {"weight", PropertyValue(int64_t{7})}});
	Edge withoutProperties = addFollowsWithoutProperties();
	const int64_t source = addUser();
	const int64_t target = addUser();
	Edge otherType(0, source, target, dm->getOrCreateTokenId("LIKES"));
	dm->addEdge(otherType);
	dm->addEdgeProperties(otherType.getId(), {{"weight", PropertyValue(int64_t{11})}});
	otherType = dm->getEdge(otherType.getId());
	ASSERT_EQ(propertyBacked.getPropertyStorageType(), PropertyStorageType::PROPERTY_ENTITY);
	ASSERT_EQ(blobBacked.getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);
	ASSERT_EQ(withoutProperties.getPropertyStorageType(), PropertyStorageType::NONE);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipMetadataColumnLoader loader(dm);
	EXPECT_FALSE(loader.collectPropertyCandidatesByType(0, 127, followsType).has_value());
	auto candidates = loader.collectPropertyCandidatesByType(1, 128, followsType);

	ASSERT_TRUE(candidates.has_value());
	EXPECT_EQ(candidates->edgeIds, (std::vector<int64_t>{propertyBacked.getId(), blobBacked.getId()}));
	EXPECT_EQ(candidates->propertyEntityIds, (std::vector<int64_t>{propertyBacked.getPropertyEntityId()}));
	EXPECT_EQ(candidates->propertyRows, (std::vector<size_t>{0}));
	EXPECT_EQ(candidates->fallbackRows, (std::vector<size_t>{1}));

	auto countCandidates = loader.collectPropertyCountCandidatesByType(1, 128, followsType);
	ASSERT_TRUE(countCandidates.has_value());
	EXPECT_EQ(countCandidates->matchedEdges, 3U);
	EXPECT_EQ(countCandidates->propertyEntityIds, (std::vector<int64_t>{propertyBacked.getPropertyEntityId()}));
	EXPECT_EQ(countCandidates->fallbackEdgeIds, (std::vector<int64_t>{blobBacked.getId()}));
	auto cachedCountCandidates = dm->collectRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType);
	ASSERT_TRUE(cachedCountCandidates.has_value());
	EXPECT_EQ(cachedCountCandidates->matchedEdges, 3U);
	EXPECT_EQ(cachedCountCandidates->propertyEntityIds, countCandidates->propertyEntityIds);
	EXPECT_EQ(cachedCountCandidates->fallbackEdgeIds, countCandidates->fallbackEdgeIds);
	EXPECT_FALSE(dm->collectRelationshipPropertyCandidatesFromSegmentStats(2, 128, followsType).has_value());

	auto noTypeMatches = loader.collectPropertyCandidatesByType(1, 128, followsType + 999);
	ASSERT_TRUE(noTypeMatches.has_value());
	EXPECT_TRUE(noTypeMatches->edgeIds.empty());
	EXPECT_TRUE(noTypeMatches->propertyEntityIds.empty());
	EXPECT_TRUE(noTypeMatches->fallbackRows.empty());
	auto noTypeCountCandidates = loader.collectPropertyCountCandidatesByType(1, 128, followsType + 999);
	ASSERT_TRUE(noTypeCountCandidates.has_value());
	EXPECT_EQ(noTypeCountCandidates->matchedEdges, 0U);
	EXPECT_TRUE(noTypeCountCandidates->propertyEntityIds.empty());
	EXPECT_TRUE(noTypeCountCandidates->fallbackEdgeIds.empty());

	auto allTypes = loader.collectPropertyCandidatesByType(1, 128, 0);
	ASSERT_TRUE(allTypes.has_value());
	EXPECT_EQ(allTypes->edgeIds, (std::vector<int64_t>{propertyBacked.getId(), blobBacked.getId(), otherType.getId()}));
	auto cachedAllTypes = dm->collectRelationshipPropertyCandidatesFromSegmentStats(1, 128, 0);
	ASSERT_TRUE(cachedAllTypes.has_value());
	EXPECT_EQ(cachedAllTypes->matchedEdges, 4U);
	EXPECT_EQ(cachedAllTypes->propertyEntityIds,
			  (std::vector<int64_t>{propertyBacked.getPropertyEntityId(), otherType.getPropertyEntityId()}));
	EXPECT_EQ(cachedAllTypes->fallbackEdgeIds, (std::vector<int64_t>{blobBacked.getId()}));
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, MetadataLoaderRejectsUnsafeOrShortRanges) {
	Edge edge = addFollowsWithProperties({{"weight", PropertyValue(int64_t{5})}});
	RelationshipMetadataColumnLoader nullLoader(nullptr);
	EXPECT_FALSE(nullLoader.loadRange(1, 128).has_value());
	EXPECT_FALSE(nullLoader.countActiveByType(1, 128, followsType).has_value());
	EXPECT_FALSE(nullLoader.collectPropertyCandidatesByType(1, 128, followsType).has_value());
	EXPECT_FALSE(nullLoader.collectPropertyCountCandidatesByType(1, 128, followsType).has_value());

	RelationshipMetadataColumnLoader loader(dm);
	EXPECT_FALSE(loader.loadRange(1, 128).has_value());
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	EXPECT_FALSE(loader.loadRange(0, 128).has_value());
	EXPECT_FALSE(loader.loadRange(edge.getId(), edge.getId()).has_value());
	EXPECT_FALSE(loader.countActiveByType(edge.getId(), edge.getId(), followsType).has_value());
	EXPECT_FALSE(loader.collectPropertyCandidatesByType(edge.getId(), edge.getId(), followsType).has_value());
	EXPECT_FALSE(loader.collectPropertyCountCandidatesByType(edge.getId(), edge.getId(), followsType).has_value());
	EXPECT_FALSE(loader.loadRange(edge.getId() + 1000, edge.getId() + 1127).has_value());
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, MetadataLoaderStopsWhenSegmentIndexPointsPastFile) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	for (int i = 0; i < 128; ++i) {
		Edge edge(0, source, target, followsType);
		dm->addEdge(edge);
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	auto patchedIndex = dm->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(patchedIndex.empty());
	patchedIndex.front().segmentOffset += static_cast<uint64_t>(storage::TOTAL_SEGMENT_SIZE) * 10'000ULL;
	dm->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, std::move(patchedIndex));

	RelationshipMetadataColumnLoader loader(dm);
	EXPECT_FALSE(loader.loadRange(1, 128).has_value());
	EXPECT_FALSE(loader.countActiveByType(1, 128, followsType).has_value());
	EXPECT_FALSE(loader.collectPropertyCandidatesByType(1, 128, followsType).has_value());
	EXPECT_FALSE(loader.collectPropertyCountCandidatesByType(1, 128, followsType).has_value());
	EXPECT_FALSE(dm->collectRelationshipPropertyCandidatesFromSegmentStats(1, 128, followsType).has_value());
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, MetadataLoaderRejectsReadOnlySnapshotsWithEdgeOverlays) {
	Edge edge = addFollowsWithProperties({{"weight", PropertyValue(int64_t{5})}});
	for (int i = 0; i < 127; ++i) {
		addFollowsWithoutProperties();
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipMetadataColumnLoader loader(dm);
	storage::CommittedSnapshot emptySnapshot;
	dm->setCurrentSnapshot(&emptySnapshot);
	EXPECT_TRUE(loader.loadRange(1, 128).has_value());

	storage::CommittedSnapshot edgeOverlaySnapshot;
	edgeOverlaySnapshot.edges.emplace(
			edge.getId(),
			storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_MODIFIED, dm->getEdge(edge.getId())));
	dm->setCurrentSnapshot(&edgeOverlaySnapshot);
	EXPECT_FALSE(loader.loadRange(1, 128).has_value());
	dm->clearCurrentSnapshot();
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, MetadataLoaderRejectsReadOnlySnapshotsWithPropertyOrBlobOverlays) {
	for (int i = 0; i < 128; ++i) {
		addFollowsWithoutProperties();
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipMetadataColumnLoader loader(dm);

	storage::CommittedSnapshot propertyOverlaySnapshot;
	Property property;
	property.setId(1);
	propertyOverlaySnapshot.properties.emplace(
			property.getId(), storage::DirtyEntityInfo<Property>(storage::EntityChangeType::CHANGE_MODIFIED, property));
	dm->setCurrentSnapshot(&propertyOverlaySnapshot);
	EXPECT_FALSE(loader.loadRange(1, 128).has_value());

	storage::CommittedSnapshot blobOverlaySnapshot;
	Blob blob;
	blob.setId(1);
	blobOverlaySnapshot.blobs.emplace(blob.getId(),
									  storage::DirtyEntityInfo<Blob>(storage::EntityChangeType::CHANGE_MODIFIED, blob));
	dm->setCurrentSnapshot(&blobOverlaySnapshot);
	EXPECT_FALSE(loader.loadRange(1, 128).has_value());
	dm->clearCurrentSnapshot();
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, MetadataLoaderCountsAndLoadsRangesByType) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	for (int i = 0; i < 130; ++i) {
		Edge edge(0, source, target, followsType);
		dm->addEdge(edge);
		if (i == 0) {
			dm->deleteEdge(edge);
		}
	}
	Edge other(0, source, target, dm->getOrCreateTokenId("LIKES"));
	dm->addEdge(other);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipMetadataColumnLoader loader(dm);
	auto allFollows = loader.countActiveByType(1, 256, followsType);
	ASSERT_TRUE(allFollows.has_value());
	EXPECT_EQ(*allFollows, 129);
	auto allTypes = loader.countActiveByType(1, 256, 0);
	ASSERT_TRUE(allTypes.has_value());
	EXPECT_EQ(*allTypes, 130);

	auto metadata = loader.loadRange(1, 256);
	ASSERT_TRUE(metadata.has_value());
	ASSERT_GE(metadata->size(), 130U);
	EXPECT_TRUE(metadata->isValid(0));
	EXPECT_EQ(metadata->toEdge(0).getSourceNodeId(), source);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	auto tracedMetadata = loader.loadRange(1, 256);
	ASSERT_TRUE(tracedMetadata.has_value());
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(trace.contains("relationship_count.load_edge_metadata"));
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, MetadataLoaderSkipsEmptyOrNonOverlappingSegmentWindows) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	for (int i = 0; i < 128; ++i) {
		Edge edge(0, source, target, followsType);
		dm->addEdge(edge);
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	const auto originalIndex = dm->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(originalIndex.empty());
	RelationshipMetadataColumnLoader loader(dm);

	auto shiftedIndex = originalIndex;
	shiftedIndex.front().startId = 1000;
	shiftedIndex.front().endId = 1127;
	dm->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, shiftedIndex);
	auto nonOverlapping = loader.loadRange(1000, 1127);
	ASSERT_TRUE(nonOverlapping.has_value());
	EXPECT_EQ(nonOverlapping->size(), 0U);

	auto emptyHeaderIndex = originalIndex;
	dm->getSegmentIndexManager()->setSegmentIndex(Edge::typeId, emptyHeaderIndex);
	storage::SegmentHeader header = dm->getSegmentTracker()->getSegmentHeaderCopy(originalIndex.front().segmentOffset);
	header.used = 0;
	dm->getSegmentTracker()->writeSegmentHeader(originalIndex.front().segmentOffset, header);
	auto emptyHeader = loader.loadRange(1, 128);
	ASSERT_TRUE(emptyHeader.has_value());
	EXPECT_EQ(emptyHeader->size(), 0U);
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, RelationshipTypeSegmentStatsCountAndInvalidateDirtySegments) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	const int64_t likesType = dm->getOrCreateTokenId("LIKES");
	std::vector<int64_t> followsIds;
	for (int i = 0; i < 260; ++i) {
		Edge edge(0, source, target, (i % 2 == 0) ? followsType : likesType);
		dm->addEdge(edge);
		if (edge.getTypeId() == followsType) {
			followsIds.push_back(edge.getId());
		}
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	auto followsCount = dm->countActiveEdgesByTypeFromSegmentStats(1, 260, followsType);
	ASSERT_TRUE(followsCount.has_value());
	EXPECT_EQ(*followsCount, 130);
	auto allCount = dm->countActiveEdgesByTypeFromSegmentStats(1, 260, 0);
	ASSERT_TRUE(allCount.has_value());
	EXPECT_EQ(*allCount, 260);
	auto partialCount = dm->countActiveEdgesByTypeFromSegmentStats(2, 129, followsType);
	ASSERT_TRUE(partialCount.has_value());
	EXPECT_EQ(*partialCount, 64);
	auto missingTypeCount = dm->countActiveEdgesByTypeFromSegmentStats(1, 260, followsType + likesType + 1000);
	ASSERT_TRUE(missingTypeCount.has_value());
	EXPECT_EQ(*missingTypeCount, 0);
	const auto &edgeSegments = dm->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(edgeSegments.empty());
	auto cachedStats = dm->cachedRelationshipTypeSegmentStats(edgeSegments.front().segmentOffset);
	ASSERT_TRUE(cachedStats.has_value());
	EXPECT_EQ(cachedStats->segmentOffset, edgeSegments.front().segmentOffset);
	EXPECT_GT(cachedStats->activeCount, 0);
	EXPECT_FALSE(dm->countActiveEdgesByTypeFromSegmentStats(0, 260, followsType).has_value());
	EXPECT_FALSE(dm->countActiveEdgesByTypeFromSegmentStats(10, 1, followsType).has_value());
	RelationshipMetadataColumnLoader metadataLoader(dm);

	storage::CommittedSnapshot edgeOverlaySnapshot;
	Edge snapshotDeleted = dm->getEdge(followsIds.front());
	snapshotDeleted.markInactive();
	edgeOverlaySnapshot.edges.emplace(
			snapshotDeleted.getId(),
			storage::DirtyEntityInfo<Edge>(storage::EntityChangeType::CHANGE_DELETED, snapshotDeleted));
	dm->setCurrentSnapshot(&edgeOverlaySnapshot);
	auto snapshotOverlayCount = dm->countActiveEdgesByTypeFromSegmentStats(1, 260, followsType);
	ASSERT_TRUE(snapshotOverlayCount.has_value());
	EXPECT_EQ(*snapshotOverlayCount, 129);
	auto snapshotLoaderCount = metadataLoader.countActiveByType(1, 260, followsType);
	ASSERT_TRUE(snapshotLoaderCount.has_value());
	EXPECT_EQ(*snapshotLoaderCount, 129);
	dm->clearCurrentSnapshot();
	dm->clearRelationshipSegmentTypeStats();
	EXPECT_FALSE(dm->cachedRelationshipTypeSegmentStats(edgeSegments.front().segmentOffset).has_value());
	followsCount = dm->countActiveEdgesByTypeFromSegmentStats(1, 260, followsType);
	ASSERT_TRUE(followsCount.has_value());
	EXPECT_EQ(*followsCount, 130);

	Edge unsaved(0, source, target, followsType);
	dm->addEdge(unsaved);
	EXPECT_TRUE(dm->hasUnsavedChanges());
	auto unsavedAddCount = dm->countActiveEdgesByTypeFromSegmentStats(1, unsaved.getId(), followsType);
	ASSERT_TRUE(unsavedAddCount.has_value());
	EXPECT_EQ(*unsavedAddCount, 131);
	auto unsavedAddLoaderCount = metadataLoader.countActiveByType(1, unsaved.getId(), followsType);
	ASSERT_TRUE(unsavedAddLoaderCount.has_value());
	EXPECT_EQ(*unsavedAddLoaderCount, 131);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	ASSERT_FALSE(followsIds.empty());
	Edge deleted = dm->getEdge(followsIds.front());
	dm->deleteEdge(deleted);
	auto unsavedDeleteCount = dm->countActiveEdgesByTypeFromSegmentStats(1, 260, followsType);
	ASSERT_TRUE(unsavedDeleteCount.has_value());
	EXPECT_EQ(*unsavedDeleteCount, 129);
	auto unsavedDeleteLoaderCount = metadataLoader.countActiveByType(1, 260, followsType);
	ASSERT_TRUE(unsavedDeleteLoaderCount.has_value());
	EXPECT_EQ(*unsavedDeleteLoaderCount, 129);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	auto countAfterInvalidation = dm->countActiveEdgesByTypeFromSegmentStats(1, 260, followsType);
	ASSERT_TRUE(countAfterInvalidation.has_value());
	EXPECT_EQ(*countAfterInvalidation, 129);
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, RelationshipTypeTotalStatsCountsFullRangeAndOverlays) {
	auto emptyCount = dm->countActiveEdgesByTypeFromSegmentStats(1, 128, followsType);
	ASSERT_TRUE(emptyCount.has_value());
	EXPECT_EQ(*emptyCount, 0);

	const int64_t source = addUser();
	const int64_t target = addUser();
	const int64_t likesType = dm->getOrCreateTokenId("LIKES");
	for (int i = 0; i < 300; ++i) {
		Edge edge(0, source, target, (i % 3 == 0) ? followsType : likesType);
		dm->addEdge(edge);
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	auto followsFullRange = dm->countActiveEdgesByTypeFromSegmentStats(1, 4096, followsType);
	ASSERT_TRUE(followsFullRange.has_value());
	EXPECT_EQ(*followsFullRange, 100);
	auto allTypesFullRange = dm->countActiveEdgesByTypeFromSegmentStats(1, 4096, 0);
	ASSERT_TRUE(allTypesFullRange.has_value());
	EXPECT_EQ(*allTypesFullRange, 300);

	auto followsPartialRange = dm->countActiveEdgesByTypeFromSegmentStats(2, 129, followsType);
	ASSERT_TRUE(followsPartialRange.has_value());
	EXPECT_EQ(*followsPartialRange, 42);

	Edge unsaved(0, source, target, followsType);
	dm->addEdge(unsaved);
	auto withUnsavedOverlay = dm->countActiveEdgesByTypeFromSegmentStats(1, unsaved.getId(), followsType);
	ASSERT_TRUE(withUnsavedOverlay.has_value());
	EXPECT_EQ(*withUnsavedOverlay, 101);
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, MetadataLoaderScansPartialSegmentsAndFilteredRows) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	const int64_t likesType = dm->getOrCreateTokenId("LIKES");
	const int64_t mentionsType = dm->getOrCreateTokenId("MENTIONS");
	std::vector<int64_t> followsIds;
	for (int i = 0; i < 260; ++i) {
		Edge edge(0, source, target, (i % 2 == 0) ? followsType : likesType);
		dm->addEdge(edge);
		if (i == 2) {
			dm->deleteEdge(edge);
		}
		if (i == 4 || i == 5) {
			dm->addEdgeProperties(edge.getId(), {{"weight", PropertyValue(int64_t{i})}});
		}
		if (edge.getTypeId() == followsType) {
			followsIds.push_back(edge.getId());
		}
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipMetadataColumnLoader loader(dm);
	auto secondSegment = loader.loadRange(129, 256);
	ASSERT_TRUE(secondSegment.has_value());
	EXPECT_FALSE(secondSegment->edgeIds.empty());

	auto followsCount = loader.countActiveByType(1, 256, followsType);
	ASSERT_TRUE(followsCount.has_value());
	EXPECT_LT(*followsCount, 130);

	auto followsCandidates = loader.collectPropertyCandidatesByType(1, 256, followsType);
	ASSERT_TRUE(followsCandidates.has_value());
	EXPECT_FALSE(followsCandidates->edgeIds.empty());
	auto followsCountCandidates = loader.collectPropertyCountCandidatesByType(1, 256, followsType);
	ASSERT_TRUE(followsCountCandidates.has_value());
	EXPECT_EQ(followsCountCandidates->propertyEntityIds.size() + followsCountCandidates->fallbackEdgeIds.size(),
			  followsCandidates->propertyEntityIds.size() + followsCandidates->fallbackRows.size());
	auto missingTypeCandidates = loader.collectPropertyCandidatesByType(1, 256, mentionsType);
	ASSERT_TRUE(missingTypeCandidates.has_value());
	EXPECT_TRUE(missingTypeCandidates->edgeIds.empty());
	EXPECT_FALSE(followsIds.empty());
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, MetadataLoaderReadsInactiveRowsAfterPersistedDeletion) {
	const int64_t source = addUser();
	const int64_t target = addUser();
	std::vector<int64_t> edgeIds;
	for (int i = 0; i < 130; ++i) {
		Edge edge(0, source, target, followsType);
		dm->addEdge(edge);
		edgeIds.push_back(edge.getId());
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	Edge deleted = dm->getEdge(edgeIds[1]);
	dm->deleteEdge(deleted);
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());

	RelationshipMetadataColumnLoader loader(dm);
	auto metadata = loader.loadRange(1, 128);
	ASSERT_TRUE(metadata.has_value());
	ASSERT_GT(metadata->active.size(), 1U);
	EXPECT_EQ(metadata->active[1], 0);
	EXPECT_FALSE(metadata->toEdge(1).isActive());
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, BlobBackedPropertiesFallbackToDirectLoading) {
	std::string largeValue(512, 'x');
	Edge stored =
			addFollowsWithProperties({{"payload", PropertyValue(largeValue)}, {"weight", PropertyValue(int64_t{3})}});
	ASSERT_TRUE(stored.hasPropertyEntity());
	ASSERT_EQ(stored.getPropertyStorageType(), PropertyStorageType::BLOB_ENTITY);
	RelationshipPropertyColumnLoader loader(dm);

	auto columns = loader.loadColumns({stored}, {}, {"payload", "weight"});

	ASSERT_EQ(columns.size(), 2U);
	ASSERT_TRUE(columns["payload"][0].has_value());
	ASSERT_TRUE(columns["weight"][0].has_value());
	EXPECT_EQ(columns["payload"][0].value(), PropertyValue(largeValue));
	EXPECT_EQ(columns["weight"][0].value(), PropertyValue(int64_t{3}));
}

TEST(RelationshipMetadataBatchTest, CandidateBatchHelpersReserveAndReportSizes) {
	RelationshipPropertyCandidateBatch candidates;
	EXPECT_EQ(candidates.size(), 0U);
	candidates.reserve(16);
	candidates.edgeIds.push_back(10);
	candidates.propertyEntityIds.push_back(20);
	candidates.propertyRows.push_back(0);
	candidates.fallbackRows.push_back(1);
	EXPECT_EQ(candidates.size(), 1U);

	RelationshipPropertyCountCandidates countCandidates;
	EXPECT_EQ(countCandidates.matchedEdges, 0U);
	countCandidates.reserve(16);
	countCandidates.propertyEntityIds.push_back(20);
	countCandidates.fallbackEdgeIds.push_back(30);
	countCandidates.matchedEdges = 2;
	EXPECT_EQ(countCandidates.propertyEntityIds.size(), 1U);
	EXPECT_EQ(countCandidates.fallbackEdgeIds.size(), 1U);
	EXPECT_EQ(countCandidates.matchedEdges, 2U);
}

TEST_F(RelationshipPropertyColumnLoaderStorageTest, MetadataCandidateCountFallbackScansPartialRanges) {
	Edge propertyBacked = addFollowsWithProperties({{"weight", PropertyValue(int64_t{5})}});
	std::string largeValue(512, 'x');
	Edge blobBacked = addFollowsWithProperties({{"weight", PropertyValue(int64_t{7})}, {"payload", PropertyValue(largeValue)}});
	for (int i = 0; i < 130; ++i) {
		addFollowsWithoutProperties();
	}
	db->getStorage()->flush();
	ASSERT_FALSE(dm->hasUnsavedChanges());
	ASSERT_EQ(propertyBacked.getId(), 1);
	ASSERT_EQ(blobBacked.getId(), 2);

	RelationshipMetadataColumnLoader loader(dm);
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	auto candidates = loader.collectPropertyCountCandidatesByType(2, 129, followsType);
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_TRUE(candidates.has_value());
	EXPECT_EQ(candidates->matchedEdges, 128U);
	EXPECT_TRUE(candidates->propertyEntityIds.empty());
	EXPECT_EQ(candidates->fallbackEdgeIds, (std::vector<int64_t>{blobBacked.getId()}));
	EXPECT_TRUE(trace.contains("relationship_count.load_edge_metadata"));
}
