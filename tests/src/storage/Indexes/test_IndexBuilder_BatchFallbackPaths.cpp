/**
 * @file test_IndexBuilder_BatchFallbackPaths.cpp
 * @brief Tests IndexBuilder's internal batch fallback contracts.
 *
 * These paths protect legacy and defensive index rebuild behavior that is hard
 * to trigger through public create-index flows once typed owner scans take over.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/SegmentIndexManager.hpp"
#include "graph/storage/StorageHeaders.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/EntityTypeIndexManager.hpp"
#include "graph/storage/indexes/IndexBuilder.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/indexes/LabelIndex.hpp"
#include "graph/storage/indexes/PropertyIndex.hpp"
#include "graph/storage/indexes/ScopedNodePropertyKey.hpp"

namespace fs = std::filesystem;

namespace graph::query::indexes::testing {

	class IndexBuilderAccess {
	public:
		static void processNodes(const IndexBuilder &builder,
								 const std::vector<int64_t> &nodeIds,
								 const std::shared_ptr<LabelIndex> &labelIndex,
								 const std::shared_ptr<PropertyIndex> &propertyIndex,
								 const std::string &propertyKey,
								 int64_t scopedLabelId = 0,
								 const std::string &scopedPropertyKey = "",
								 bool buildGlobalProperty = true) {
			builder.processNodeBatch(
					nodeIds, labelIndex, propertyIndex, propertyKey, scopedLabelId, scopedPropertyKey,
					buildGlobalProperty);
		}

		 static void processEdges(const IndexBuilder &builder,
								 const std::vector<int64_t> &edgeIds,
								 const std::shared_ptr<LabelIndex> &labelIndex,
								 const std::shared_ptr<PropertyIndex> &propertyIndex,
								 const std::string &propertyKey) {
			builder.processEdgeBatch(edgeIds, labelIndex, propertyIndex, propertyKey);
		}

		static bool buildNodeFromOwnerScan(const IndexBuilder &builder,
										   const std::shared_ptr<PropertyIndex> &propertyIndex,
										   const std::string &propertyKey,
										   int64_t scopedLabelId,
										   const std::string &scopedPropertyKey,
										   bool buildGlobalProperty) {
			return builder.buildNodePropertyIndexFromOwnerScan(
					propertyIndex, propertyKey, scopedLabelId, scopedPropertyKey, buildGlobalProperty);
		}

		static bool buildEdgeFromOwnerScan(const IndexBuilder &builder,
										   const std::shared_ptr<PropertyIndex> &propertyIndex,
										   const std::string &propertyKey) {
			return builder.buildEdgePropertyIndexFromOwnerScan(propertyIndex, propertyKey);
		}
	};

} // namespace graph::query::indexes::testing

class IndexBuilderBatchFallbackTest : public ::testing::Test {
protected:
	void SetUp() override {
		const boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_indexBuilder_batch_" + to_string(uuid) + ".dat");

		database = std::make_unique<graph::Database>(testFilePath.string());
		database->open();
		fileStorage = database->getStorage();
		dataManager = fileStorage->getDataManager();
		indexManager = database->getQueryEngine()->getIndexManager();
		indexBuilder = indexManager->getIndexBuilder();
	}

	void TearDown() override {
		if (database) {
			database->close();
		}
		database.reset();
		std::error_code ec;
		fs::remove(testFilePath, ec);
	}

	fs::path testFilePath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::FileStorage> fileStorage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	graph::query::indexes::IndexBuilder *indexBuilder = nullptr;
};

TEST_F(IndexBuilderBatchFallbackTest, ProcessNodeBatchSkipsInvalidRowsAndUnresolvedLabels) {
	const int64_t validLabel = dataManager->getOrCreateTokenId("BatchValidNode");
	graph::Node validNode(0, validLabel);
	dataManager->addNode(validNode);

	graph::Node zeroLabelNode(0, 0);
	dataManager->addNode(zeroLabelNode);

	graph::Node unresolvedLabelNode(0, 9'000'001);
	dataManager->addNode(unresolvedLabelNode);

	auto labelIndex = indexManager->getNodeIndexManager()->getLabelIndex();
	labelIndex->createIndex();
	labelIndex->clear();
	graph::query::indexes::testing::IndexBuilderAccess::processNodes(
			*indexBuilder,
			{0, validNode.getId(), zeroLabelNode.getId(), unresolvedLabelNode.getId(), -1},
			labelIndex,
			nullptr,
			"");

	EXPECT_EQ(indexManager->findNodeIdsByLabel("BatchValidNode"), (std::vector<int64_t>{validNode.getId()}));
}

TEST_F(IndexBuilderBatchFallbackTest, ProcessNodeBatchHandlesInactiveAndEmptyLabelBatches) {
	const int64_t label = dataManager->getOrCreateTokenId("BatchInactiveNode");
	graph::Node inactiveNode(0, label);
	dataManager->addNode(inactiveNode);

	graph::Node unresolvedLabelNode(0, 9'000'003);
	dataManager->addNode(unresolvedLabelNode);
	fileStorage->flush();
	dataManager->clearCache();

	const auto nodeSegments = dataManager->getSegmentIndexManager()->getNodeSegmentIndex();
	ASSERT_FALSE(nodeSegments.empty());
	const auto &segment = nodeSegments.front();
	const auto slot = static_cast<uint64_t>(inactiveNode.getId() - segment.startId);
	bool inactive = false;
	{
		std::fstream file(testFilePath, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(file.is_open());
		constexpr size_t serializedNodeActiveOffset = sizeof(int64_t) * 4 +
													  sizeof(int64_t) * graph::Node::MAX_LABELS +
													  sizeof(uint8_t) + sizeof(uint32_t);
		file.seekp(static_cast<std::streamoff>(
				segment.segmentOffset + sizeof(graph::storage::SegmentHeader) +
				slot * graph::Node::getTotalSize() + serializedNodeActiveOffset));
		file.write(reinterpret_cast<const char *>(&inactive), sizeof(inactive));
	}
	dataManager->clearCache();

	auto labelIndex = indexManager->getNodeIndexManager()->getLabelIndex();
	labelIndex->createIndex();
	labelIndex->clear();
	graph::query::indexes::testing::IndexBuilderAccess::processNodes(
			*indexBuilder,
			{inactiveNode.getId(), unresolvedLabelNode.getId()},
			labelIndex,
			nullptr,
			"");

	EXPECT_TRUE(indexManager->findNodeIdsByLabel("BatchInactiveNode").empty());
}

TEST_F(IndexBuilderBatchFallbackTest, ProcessNodeBatchIgnoresZeroLabelSlots) {
	const int64_t label = dataManager->getOrCreateTokenId("BatchZeroLabelSlotNode");
	graph::Node node(0, label);
	node.getMutableMetadata().labelIds[1] = 0;
	node.getMutableMetadata().labelCount = 2;
	dataManager->addNode(node);

	auto labelIndex = indexManager->getNodeIndexManager()->getLabelIndex();
	labelIndex->createIndex();
	labelIndex->clear();
	graph::query::indexes::testing::IndexBuilderAccess::processNodes(
			*indexBuilder,
			{node.getId()},
			labelIndex,
			nullptr,
			"");

	EXPECT_EQ(indexManager->findNodeIdsByLabel("BatchZeroLabelSlotNode"), (std::vector<int64_t>{node.getId()}));
}

TEST_F(IndexBuilderBatchFallbackTest, ProcessEdgeBatchSkipsInvalidRowsAndUnresolvedTypes) {
	graph::Node source(0, 0);
	graph::Node target(0, 0);
	dataManager->addNode(source);
	dataManager->addNode(target);

	const int64_t validType = dataManager->getOrCreateTokenId("BATCH_VALID_EDGE");
	graph::Edge validEdge(0, source.getId(), target.getId(), validType);
	dataManager->addEdge(validEdge);

	graph::Edge zeroTypeEdge(0, source.getId(), target.getId(), 0);
	dataManager->addEdge(zeroTypeEdge);

	graph::Edge unresolvedTypeEdge(0, source.getId(), target.getId(), 9'000'002);
	dataManager->addEdge(unresolvedTypeEdge);

	auto labelIndex = indexManager->getEdgeIndexManager()->getLabelIndex();
	labelIndex->createIndex();
	labelIndex->clear();
	graph::query::indexes::testing::IndexBuilderAccess::processEdges(
			*indexBuilder,
			{0, validEdge.getId(), zeroTypeEdge.getId(), unresolvedTypeEdge.getId(), -1},
			labelIndex,
			nullptr,
			"");

	EXPECT_EQ(indexManager->findEdgeIdsByType("BATCH_VALID_EDGE"), (std::vector<int64_t>{validEdge.getId()}));
}

TEST_F(IndexBuilderBatchFallbackTest, ProcessEdgeBatchHandlesInactiveAndEmptyTypeBatches) {
	graph::Node source(0, 0);
	graph::Node target(0, 0);
	dataManager->addNode(source);
	dataManager->addNode(target);

	const int64_t type = dataManager->getOrCreateTokenId("BATCH_INACTIVE_EDGE");
	graph::Edge inactiveEdge(0, source.getId(), target.getId(), type);
	dataManager->addEdge(inactiveEdge);

	graph::Edge unresolvedTypeEdge(0, source.getId(), target.getId(), 9'000'004);
	dataManager->addEdge(unresolvedTypeEdge);
	fileStorage->flush();
	dataManager->clearCache();

	const auto edgeSegments = dataManager->getSegmentIndexManager()->getEdgeSegmentIndex();
	ASSERT_FALSE(edgeSegments.empty());
	const auto &segment = edgeSegments.front();
	const auto slot = static_cast<uint64_t>(inactiveEdge.getId() - segment.startId);
	bool inactive = false;
	{
		std::fstream file(testFilePath, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(file.is_open());
		file.seekp(static_cast<std::streamoff>(
				segment.segmentOffset + sizeof(graph::storage::SegmentHeader) +
				slot * graph::Edge::getTotalSize() + offsetof(graph::Edge::Metadata, isActive)));
		file.write(reinterpret_cast<const char *>(&inactive), sizeof(inactive));
	}
	dataManager->clearCache();

	auto labelIndex = indexManager->getEdgeIndexManager()->getLabelIndex();
	labelIndex->createIndex();
	graph::query::indexes::testing::IndexBuilderAccess::processEdges(
			*indexBuilder,
			{inactiveEdge.getId(), unresolvedTypeEdge.getId()},
			labelIndex,
			nullptr,
			"");

	EXPECT_TRUE(indexManager->findEdgeIdsByType("BATCH_INACTIVE_EDGE").empty());
}

TEST_F(IndexBuilderBatchFallbackTest, ProcessNodeBatchIndexesGlobalAndScopedProperties) {
	const int64_t scopedLabel = dataManager->getOrCreateTokenId("BatchScopedNode");
	graph::Node scopedNode(0, scopedLabel);
	dataManager->addNode(scopedNode);
	dataManager->addNodeProperties(scopedNode.getId(), {{"score", graph::PropertyValue(int64_t{7})}});

	graph::Node otherNode(0, dataManager->getOrCreateTokenId("BatchOtherNode"));
	dataManager->addNode(otherNode);
	dataManager->addNodeProperties(otherNode.getId(), {{"score", graph::PropertyValue(int64_t{9})}});

	auto propertyIndex = indexManager->getNodeIndexManager()->getPropertyIndex();
	propertyIndex->createIndex("score");
	graph::query::indexes::testing::IndexBuilderAccess::processNodes(
			*indexBuilder,
			{0, scopedNode.getId(), otherNode.getId()},
			nullptr,
			propertyIndex,
			"score");

	EXPECT_EQ(indexManager->findNodeIdsByProperty("score", graph::PropertyValue(int64_t{7})),
			  (std::vector<int64_t>{scopedNode.getId()}));
	EXPECT_EQ(indexManager->findNodeIdsByProperty("score", graph::PropertyValue(int64_t{9})),
			  (std::vector<int64_t>{otherNode.getId()}));

	const auto scopedKey = graph::query::indexes::makeScopedNodePropertyKey("BatchScopedNode", "score");
	propertyIndex->createIndex(scopedKey);
	graph::query::indexes::testing::IndexBuilderAccess::processNodes(
			*indexBuilder,
			{scopedNode.getId(), otherNode.getId()},
			nullptr,
			propertyIndex,
			"score",
			scopedLabel,
			scopedKey,
			false);

	EXPECT_EQ(indexManager->findNodeIdsByLabelAndProperty("BatchScopedNode", "score", graph::PropertyValue(int64_t{7})),
			  (std::vector<int64_t>{scopedNode.getId()}));
	EXPECT_TRUE(indexManager->findNodeIdsByLabelAndProperty(
						 "BatchScopedNode", "score", graph::PropertyValue(int64_t{9}))
						.empty());

	graph::query::indexes::testing::IndexBuilderAccess::processNodes(
			*indexBuilder,
			{scopedNode.getId()},
			nullptr,
			propertyIndex,
			"score",
			0,
			"",
			false);
	graph::query::indexes::testing::IndexBuilderAccess::processNodes(
			*indexBuilder,
			{scopedNode.getId()},
			nullptr,
			propertyIndex,
			"score",
			scopedLabel,
			"",
			false);
}

TEST_F(IndexBuilderBatchFallbackTest, ProcessEdgeBatchIndexesProperties) {
	graph::Node source(0, 0);
	graph::Node target(0, 0);
	dataManager->addNode(source);
	dataManager->addNode(target);

	graph::Edge edge(0, source.getId(), target.getId(), dataManager->getOrCreateTokenId("BATCH_PROP_EDGE"));
	dataManager->addEdge(edge);
	dataManager->addEdgeProperties(edge.getId(), {{"weight", graph::PropertyValue(int64_t{42})}});

	auto propertyIndex = indexManager->getEdgeIndexManager()->getPropertyIndex();
	propertyIndex->createIndex("weight");
	graph::query::indexes::testing::IndexBuilderAccess::processEdges(
			*indexBuilder,
			{0, edge.getId()},
			nullptr,
			propertyIndex,
			"weight");

	EXPECT_EQ(indexManager->findEdgeIdsByProperty("weight", graph::PropertyValue(int64_t{42})),
			  (std::vector<int64_t>{edge.getId()}));
}

TEST_F(IndexBuilderBatchFallbackTest, OwnerScanHelpersValidateInputsAndNoopScopedBuilds) {
	auto nodePropertyIndex = indexManager->getNodeIndexManager()->getPropertyIndex();
	nodePropertyIndex->createIndex("score");

	EXPECT_FALSE(graph::query::indexes::testing::IndexBuilderAccess::buildNodeFromOwnerScan(
			*indexBuilder, nullptr, "score", 0, "", true));
	EXPECT_FALSE(graph::query::indexes::testing::IndexBuilderAccess::buildNodeFromOwnerScan(
			*indexBuilder, nodePropertyIndex, "", 0, "", true));
	EXPECT_TRUE(graph::query::indexes::testing::IndexBuilderAccess::buildNodeFromOwnerScan(
			*indexBuilder, nodePropertyIndex, "score", 0, "", false));
	const int64_t scopedLabel = dataManager->getOrCreateTokenId("OwnerScanNoScopedKey");
	EXPECT_TRUE(graph::query::indexes::testing::IndexBuilderAccess::buildNodeFromOwnerScan(
			*indexBuilder, nodePropertyIndex, "score", scopedLabel, "", false));

	auto edgePropertyIndex = indexManager->getEdgeIndexManager()->getPropertyIndex();
	edgePropertyIndex->createIndex("weight");
	EXPECT_FALSE(graph::query::indexes::testing::IndexBuilderAccess::buildEdgeFromOwnerScan(
			*indexBuilder, nullptr, "weight"));
	EXPECT_FALSE(graph::query::indexes::testing::IndexBuilderAccess::buildEdgeFromOwnerScan(
			*indexBuilder, edgePropertyIndex, ""));
	EXPECT_TRUE(graph::query::indexes::testing::IndexBuilderAccess::buildEdgeFromOwnerScan(
			*indexBuilder, edgePropertyIndex, "weight"));
}

TEST_F(IndexBuilderBatchFallbackTest, OwnerScanHelpersIndexPropertyEntityValues) {
	const int64_t scopedLabel = dataManager->getOrCreateTokenId("OwnerScanNode");
	graph::Node node(0, scopedLabel);
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {{"score", graph::PropertyValue(int64_t{17})}});
	fileStorage->flush();
	dataManager->clearCache();

	auto nodePropertyIndex = indexManager->getNodeIndexManager()->getPropertyIndex();
	nodePropertyIndex->createIndex("score");
	fileStorage->flush();
	dataManager->clearCache();
	EXPECT_TRUE(graph::query::indexes::testing::IndexBuilderAccess::buildNodeFromOwnerScan(
			*indexBuilder, nodePropertyIndex, "score", 0, "", true));
	EXPECT_EQ(indexManager->findNodeIdsByProperty("score", graph::PropertyValue(int64_t{17})),
			  (std::vector<int64_t>{node.getId()}));

	const auto scopedKey = graph::query::indexes::makeScopedNodePropertyKey("OwnerScanNode", "score");
	nodePropertyIndex->createIndex(scopedKey);
	fileStorage->flush();
	dataManager->clearCache();
	EXPECT_TRUE(graph::query::indexes::testing::IndexBuilderAccess::buildNodeFromOwnerScan(
			*indexBuilder, nodePropertyIndex, "score", scopedLabel, scopedKey, false));
	EXPECT_EQ(indexManager->findNodeIdsByLabelAndProperty("OwnerScanNode", "score",
														  graph::PropertyValue(int64_t{17})),
			  (std::vector<int64_t>{node.getId()}));

	graph::Node source(0, 0);
	graph::Node target(0, 0);
	dataManager->addNode(source);
	dataManager->addNode(target);
	graph::Edge edge(0, source.getId(), target.getId(), dataManager->getOrCreateTokenId("OWNER_SCAN_EDGE"));
	dataManager->addEdge(edge);
	dataManager->addEdgeProperties(edge.getId(), {{"weight", graph::PropertyValue(int64_t{9})}});
	fileStorage->flush();
	dataManager->clearCache();

	auto edgePropertyIndex = indexManager->getEdgeIndexManager()->getPropertyIndex();
	edgePropertyIndex->createIndex("weight");
	fileStorage->flush();
	dataManager->clearCache();
	EXPECT_TRUE(graph::query::indexes::testing::IndexBuilderAccess::buildEdgeFromOwnerScan(
			*indexBuilder, edgePropertyIndex, "weight"));
	EXPECT_EQ(indexManager->findEdgeIdsByProperty("weight", graph::PropertyValue(int64_t{9})),
			  (std::vector<int64_t>{edge.getId()}));
}

TEST_F(IndexBuilderBatchFallbackTest, BuildNodePropertyIndexesHandlesEmptyScopedOwnerSet) {
	const std::string label = "ScopedIndexWithNoOwners";
	(void) dataManager->getOrCreateTokenId(label);

	EXPECT_TRUE(indexBuilder->buildNodePropertyIndexes({{"score", label}}));
	EXPECT_TRUE(indexManager->findNodeIdsByLabelAndProperty(label, "score", graph::PropertyValue(int64_t{1})).empty());
}

TEST_F(IndexBuilderBatchFallbackTest, IdRangeCollectionDeduplicatesDirtyPersistedEntities) {
	graph::Node node(0, dataManager->getOrCreateTokenId("DirtyRangeNode"));
	dataManager->addNode(node);
	graph::Node source(0, 0);
	graph::Node target(0, 0);
	dataManager->addNode(source);
	dataManager->addNode(target);
	graph::Edge edge(0, source.getId(), target.getId(), dataManager->getOrCreateTokenId("DIRTY_RANGE_EDGE"));
	dataManager->addEdge(edge);
	fileStorage->flush();
	dataManager->clearCache();

	const auto nodeRangesBefore = indexBuilder->getNodeIdRanges();
	const auto edgeRangesBefore = indexBuilder->getEdgeIdRanges();
	auto updatedNode = dataManager->getNode(node.getId());
	updatedNode.addProperty("dirty", graph::PropertyValue(int64_t{1}));
	dataManager->updateNode(updatedNode);
	auto updatedEdge = dataManager->getEdge(edge.getId());
	updatedEdge.addProperty("dirty", graph::PropertyValue(int64_t{1}));
	dataManager->updateEdge(updatedEdge);

	EXPECT_EQ(indexBuilder->getNodeIdRanges(), nodeRangesBefore);
	EXPECT_EQ(indexBuilder->getEdgeIdRanges(), edgeRangesBefore);
}

TEST_F(IndexBuilderBatchFallbackTest, ScopedPropertyBuildValidatesStaleLabelIndexCandidates) {
	const std::string label = "ScopedStaleLabelIndex";
	const int64_t labelId = dataManager->getOrCreateTokenId(label);
	const int64_t otherLabelId = dataManager->getOrCreateTokenId("ScopedStaleOtherLabel");

	graph::Node matchingNode(0, labelId);
	dataManager->addNode(matchingNode);
	dataManager->addNodeProperties(matchingNode.getId(), {{"score", graph::PropertyValue(int64_t{7})}});

	graph::Node wrongLabelNode(0, otherLabelId);
	dataManager->addNode(wrongLabelNode);
	dataManager->addNodeProperties(wrongLabelNode.getId(), {{"score", graph::PropertyValue(int64_t{9})}});

	graph::Node inactiveNode(0, labelId);
	dataManager->addNode(inactiveNode);
	dataManager->addNodeProperties(inactiveNode.getId(), {{"score", graph::PropertyValue(int64_t{11})}});
	fileStorage->flush();
	dataManager->clearCache();

	const auto nodeSegments = dataManager->getSegmentIndexManager()->getNodeSegmentIndex();
	auto segmentIt = std::find_if(nodeSegments.begin(), nodeSegments.end(), [&](const auto &segment) {
		return inactiveNode.getId() >= segment.startId && inactiveNode.getId() <= segment.endId;
	});
	ASSERT_NE(segmentIt, nodeSegments.end());
	const auto slot = static_cast<uint64_t>(inactiveNode.getId() - segmentIt->startId);
	bool inactive = false;
	{
		std::fstream file(testFilePath, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(file.is_open());
		constexpr size_t serializedNodeActiveOffset = sizeof(int64_t) * 4 +
													  sizeof(int64_t) * graph::Node::MAX_LABELS +
													  sizeof(uint8_t) + sizeof(uint32_t);
		file.seekp(static_cast<std::streamoff>(
				segmentIt->segmentOffset + sizeof(graph::storage::SegmentHeader) +
				slot * graph::Node::getTotalSize() + serializedNodeActiveOffset));
		file.write(reinterpret_cast<const char *>(&inactive), sizeof(inactive));
	}
	dataManager->clearCache();

	auto labelIndex = indexManager->getNodeIndexManager()->getLabelIndex();
	labelIndex->createIndex();
	labelIndex->clear();
	labelIndex->addNodesBatch({{label, {9999999, wrongLabelNode.getId(), inactiveNode.getId(), matchingNode.getId()}}});
	labelIndex->flush();

	EXPECT_TRUE(indexBuilder->buildNodePropertyIndexes({{"score", label}}));
	EXPECT_EQ(indexManager->findNodeIdsByLabelAndProperty(label, "score", graph::PropertyValue(int64_t{7})),
			  (std::vector<int64_t>{matchingNode.getId()}));
	EXPECT_TRUE(indexManager->findNodeIdsByLabelAndProperty(label, "score", graph::PropertyValue(int64_t{9})).empty());
	EXPECT_TRUE(indexManager->findNodeIdsByLabelAndProperty(label, "score", graph::PropertyValue(int64_t{11})).empty());
}
