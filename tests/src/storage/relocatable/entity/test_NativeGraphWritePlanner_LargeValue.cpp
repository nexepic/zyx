/**
 * @file test_NativeGraphWritePlanner_LargeValue.cpp
 * @brief Production-boundary LargeValue tests for native graph writes.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Node.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
using graph::PropertyValue;

namespace {

	constexpr uint64_t PERSON_LABEL_TOKEN = 7U;
	constexpr uint32_t BODY_PROPERTY_TOKEN = 17U;
	constexpr uint32_t EMBEDDING_PROPERTY_TOKEN = 18U;
	constexpr uint32_t SCORE_PROPERTY_TOKEN = 19U;

	NativeGraphMutationTokenResolvers largeValueTokens() {
		NativeGraphMutationTokenResolvers tokens;
		tokens.schema_token = [](const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind kind,
									 std::string_view name) -> std::optional<uint64_t> {
			return kind == SystemCatalogSchemaTokenKind::SCSTK_LABEL && name == "Person"
					   ? std::optional<uint64_t>(PERSON_LABEL_TOKEN)
					   : std::nullopt;
		};
		tokens.property_key_token = [](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
			if (name == "body") {
				return BODY_PROPERTY_TOKEN;
			}
			if (name == "embedding") {
				return EMBEDDING_PROPERTY_TOKEN;
			}
			return name == "score" ? std::optional<uint32_t>(SCORE_PROPERTY_TOKEN) : std::nullopt;
		};
		tokens.property_key_name = [](const MetadataSnapshotPin &, uint32_t token) -> std::optional<std::string> {
			if (token == BODY_PROPERTY_TOKEN) {
				return "body";
			}
			if (token == EMBEDDING_PROPERTY_TOKEN) {
				return "embedding";
			}
			return token == SCORE_PROPERTY_TOKEN ? std::optional<std::string>("score") : std::nullopt;
		};
		return tokens;
	}

	IndexDefinition scalarDefinition() {
		return {901U, "person-score", IndexDefinitionEntityKind::IDEK_NODE, IndexDefinitionKind::IDK_PROPERTY,
				PERSON_LABEL_TOKEN, {"score"}, 0U, ""};
	}

	IndexDefinition vectorDefinition() {
		return {902U, "person-embedding", IndexDefinitionEntityKind::IDEK_NODE, IndexDefinitionKind::IDK_VECTOR,
				PERSON_LABEL_TOKEN, {"embedding"}, 2U, "l2"};
	}

	NativeGraphVectorBootstrapOptions vectorBootstrapOptions() {
		NativeGraphVectorBootstrapOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.maximum_vectors = 8U;
		options.maximum_pages = 8U;
		return options;
	}

	void allocateEmptyScalarIndexRoot(BTreePageStore &indexes, uint64_t indexId, PageId pageId = PageId(101U),
														 LogSequenceNumber checkpointLsn = LogSequenceNumber(2U)) {
		SlottedPagePoolAllocationRequest allocation;
		allocation.segment_id = SegmentId(71U);
		allocation.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		allocation.pool_owner_id = indexId;
		allocation.generation = Generation(1U);
		allocation.page_id = pageId;
		allocation.page_owner_id = indexId;
		allocation.page_owner_kind = PageOwnerKind::POK_INDEX;
		PageOwnerHeader owner;
		owner.page_id = allocation.page_id;
		owner.owner_id = allocation.page_owner_id;
		owner.kind = allocation.page_owner_kind;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = allocation.generation;
		owner.committed_lsn = checkpointLsn;
		BTreePageHeader header;
		header.index_id = indexId;
		header.page_id = allocation.page_id;
		header.generation = allocation.generation;
		if (!indexes.allocate(allocation, BTreePage::create(owner, header), PhysicalSlotId(40U), PhysicalSlotId(40U),
							  checkpointLsn)
				 .has_value()) {
			throw std::runtime_error("test scalar Index root allocation unexpectedly ran out of space");
		}
		const IndexRootLocation root{indexId, allocation.page_id, allocation.generation};
		static_cast<void>(indexes.publishRoots(
				std::span(&root, 1U), {}, LogSequenceNumber(checkpointLsn.value() + 1U)));
	}

	void appendEmptyIndexRoot(BTreePageStore &indexes, uint64_t indexId, PageId pageId,
							  LogSequenceNumber checkpointLsn,
							  BTreeLeafValueLayout leafValueLayout = BTreeLeafValueLayout::BLVL_U64) {
		SlottedPagePoolAppendRequest request;
		request.segment_id = SegmentId(71U);
		request.generation = Generation(1U);
		request.page_id = pageId;
		request.page_owner_id = indexId;
		request.page_owner_kind = PageOwnerKind::POK_INDEX;
		PageOwnerHeader owner;
		owner.page_id = pageId;
		owner.owner_id = indexId;
		owner.kind = PageOwnerKind::POK_INDEX;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = Generation(1U);
		owner.committed_lsn = checkpointLsn;
		BTreePageHeader header;
		header.index_id = indexId;
		header.page_id = pageId;
		header.generation = Generation(1U);
		header.leaf_value_layout = leafValueLayout;
		static_cast<void>(indexes.append(request, BTreePage::create(owner, header), checkpointLsn));
		const IndexRootLocation root{indexId, pageId, Generation(1U)};
		static_cast<void>(indexes.publishRoots(
				std::span(&root, 1U), {}, LogSequenceNumber(checkpointLsn.value() + 1U)));
	}

	NativeGraphMutationExecutorOptions executorOptions() {
		NativeGraphMutationExecutorOptions options;
		options.maximum_index_definitions = 8U;
		options.maximum_constraint_definitions = 8U;
		options.maximum_constraint_entity_scan = 64U;
		options.maximum_constraint_support_collisions = 8U;
		options.maximum_vector_records = 64U;
		options.maximum_vector_pages = 8U;
		options.vector_delta_rebuild_threshold = 8U;
		options.maximum_vector_delta_overlay_entries = 16U;
		options.maximum_vector_delta_pages = 4U;
		return options;
	}

	MetadataPublishOptions withBootstrapFailpoint(BootstrapPublishFailpoint failpoint) {
		MetadataPublishOptions options;
		options.bootstrap_failpoint = failpoint;
		return options;
	}

	EntityPropertyLargeValueReference storedLargeValueReference(const MetadataSnapshotPin &snapshot,
															const RelocatableMetadataStore &metadata,
															const SlottedPagePool &pages, uint64_t nodeId) {
		const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, nodeId};
		const EntityPropertyLocation location = metadata.findEntityPropertyLocation(snapshot, key);
		const SlottedPage page = pages.read(snapshot, location.page_id);
		const std::vector<std::byte> bytes = page.read({location.page_id, location.slot_id, location.slot_generation});
		if (!EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
			throw std::runtime_error("native LargeValue test expected an Entity Property LargeValue reference");
		}
		return EntityPropertyLargeValueReferenceCodec::decode(bytes);
	}

	PropertyCollectionRoot entityPropertyRoot(uint64_t nodeId, std::string_view body) {
		return {nodeId, graph::Node::typeId,
				{{BODY_PROPERTY_TOKEN, PropertyValue(std::string(body))}}};
	}

	std::vector<std::byte> encodedEntityPropertyLargeValuePayload(std::string_view body) {
		return EntityPropertyLargeValuePayloadCodec::encode(entityPropertyRoot(1U, body).entries);
	}

	struct LargeValuePoolIds {
		std::set<uint64_t> data;
		std::set<uint64_t> manifest;
	};

	LargeValuePoolIds collectLargeValuePoolIds(const MetadataSnapshotPin &snapshot,
										 const RelocatableMetadataStore &metadata, const LargeValueStore &store,
										 std::span<const LargeValueRef> references) {
		LargeValuePoolIds result;
		for (const LargeValueRef &reference: references) {
			for (const LargeValueManifest &node: store.manifestTree(snapshot, store.manifest(snapshot, reference))) {
				result.manifest.insert(metadata.findPageLocation(snapshot, node.page_id).segment_id.value());
				for (const LargeValueChunk &chunk: node.chunks) {
					for (uint16_t ordinal = 0U; ordinal < chunk.data_page_count; ++ordinal) {
						result.data.insert(metadata.findPageLocation(
										 snapshot, PageId(chunk.first_data_page_id.value() + ordinal))
									 .segment_id.value());
					}
				}
			}
		}
		return result;
	}

} // namespace

class NativeGraphLargeValueMutationTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
	std::unique_ptr<LargeValueStore> large_values_;

	void SetUp() override {
		NativeGraphWritePlannerTestFixture::SetUp();
		properties_.reset();
		large_values_ = std::make_unique<LargeValueStore>(io_, *metadata_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *large_values_, *metadata_);
	}

	void TearDown() override {
		properties_.reset();
		large_values_.reset();
		NativeGraphWritePlannerTestFixture::TearDown();
	}

	void reopenWithLargeValues() {
		large_values_.reset();
		NativeGraphWritePlannerTestFixture::reopenNativeStores();
		properties_.reset();
		large_values_ = std::make_unique<LargeValueStore>(io_, *metadata_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *large_values_, *metadata_);
	}
};

TEST_F(NativeGraphLargeValueMutationTest, ExternalizesOversizedLogicalPropertiesInTheNativeGraphGeneration) {
	const std::string firstBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, 'a');
	const std::string replacementBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 97U, 'b');
	uint64_t nodeId = 0U;
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		NativeGraphMutationExecutor executor(
				writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, largeValueTokens(),
				[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
				[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

		MetadataSnapshotPin beforeCreate = metadata_->openSnapshot();
		LogicalGraphMutationBatch create;
		create.node_creates = {{{"Person"}, {{"body", PropertyValue(firstBody)}}}};
		const NativeGraphMutationExecutionResult created = executor.execute(create);
		ASSERT_TRUE(created.committed());
		ASSERT_EQ(created.result.created_node_ids.size(), 1U);
		nodeId = created.result.created_node_ids.front();
		EXPECT_FALSE(metadata_->findLargeValueManifestOptional(beforeCreate, LargeValueId(1U)).has_value());
		EXPECT_THROW(static_cast<void>(graph.readNode(beforeCreate, nodeId)), FormatException);
		beforeCreate.release();

		MetadataSnapshotPin beforeReplace = metadata_->openSnapshot();
		const EntityPropertyLargeValueReference firstReference =
				storedLargeValueReference(beforeReplace, *metadata_, *slotted_pages_, nodeId);
		EXPECT_EQ(firstReference.value.value_id, LargeValueId(1U));
		EXPECT_EQ(graph.readNode(beforeReplace, nodeId).properties,
				  (std::vector<PropertyCollectionRootEntry>{{BODY_PROPERTY_TOKEN, PropertyValue(firstBody)}}));
		ASSERT_TRUE(metadata_->findLargeValueManifestOptional(beforeReplace, LargeValueId(1U)).has_value());

		LogicalGraphMutationBatch replace;
		replace.property_replacements = {
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
				 {{"body", PropertyValue(replacementBody)}}}};
		ASSERT_TRUE(executor.execute(replace).committed());

		// The old logical root and its immutable value remain readable by a pinned snapshot.
		EXPECT_EQ(storedLargeValueReference(beforeReplace, *metadata_, *slotted_pages_, nodeId).value.value_id,
			  LargeValueId(1U));
		EXPECT_EQ(graph.readNode(beforeReplace, nodeId).properties,
			  (std::vector<PropertyCollectionRootEntry>{{BODY_PROPERTY_TOKEN, PropertyValue(firstBody)}}));
		MetadataSnapshotPin afterReplace = metadata_->openSnapshot();
		const EntityPropertyLargeValueReference replacementReference =
				storedLargeValueReference(afterReplace, *metadata_, *slotted_pages_, nodeId);
		EXPECT_EQ(replacementReference.value.value_id, LargeValueId(2U));
		EXPECT_EQ(graph.readNode(afterReplace, nodeId).properties,
				  (std::vector<PropertyCollectionRootEntry>{{BODY_PROPERTY_TOKEN, PropertyValue(replacementBody)}}));
		EXPECT_TRUE(metadata_->findLargeValueManifestOptional(afterReplace, LargeValueId(2U)).has_value());
		EXPECT_EQ(afterReplace.superblock().next_large_value_id, 3U);
		afterReplace.release();
		beforeReplace.release();
	}

	reopenWithLargeValues();
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphMutationExecutor executor(
			writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, largeValueTokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(storedLargeValueReference(reopened, *metadata_, *slotted_pages_, nodeId).value.value_id,
		  LargeValueId(2U));
	EXPECT_EQ(graph.readNode(reopened, nodeId).properties,
		  (std::vector<PropertyCollectionRootEntry>{{BODY_PROPERTY_TOKEN, PropertyValue(replacementBody)}}));
	reopened.release();

	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{nodeId, false, 1U}};
	ASSERT_TRUE(executor.execute(erase).committed());
	MetadataSnapshotPin afterErase = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(graph.readNode(afterErase, nodeId)), FormatException);
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(
					 afterErase, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, nodeId})
					 .has_value());
	afterErase.release();
}

TEST_F(NativeGraphLargeValueMutationTest,
	   PublishesLargeValueScalarIndexAndVectorStateInOneNativeGraphGeneration) {
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 97U, 'x');
	const IndexDefinition scalar = scalarDefinition();
	const IndexDefinition vector = vectorDefinition();
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	// Scalar DDL publishes an empty root before graph mutations maintain it.
	allocateEmptyScalarIndexRoot(indexes, scalar.index_id);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphMutationExecutor executor(
			writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, largeValueTokens(),
			[scalar, vector](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{scalar, vector}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"},
						{{"body", PropertyValue(body)},
						 {"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1)),
																	   PropertyValue(int64_t(2))})},
						 {"score", PropertyValue(int64_t(42))}}}};
	const NativeGraphMutationExecutionResult result = executor.execute(create);
	ASSERT_TRUE(result.committed());
	ASSERT_EQ(result.result.created_node_ids.size(), 1U);
	const uint64_t nodeId = result.result.created_node_ids.front();

	EXPECT_FALSE(metadata_->findLargeValueManifestOptional(before, LargeValueId(1U)).has_value());
	EXPECT_TRUE(metadata_->findIndexRootOptional(before, scalar.index_id).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(before, vector.index_id).has_value());
	EXPECT_FALSE(metadata_->findVectorManifestOptional(before, vector.index_id).has_value());
	EXPECT_FALSE(BTreeIndex(scalar.index_id, indexes, *metadata_)
				 .find(before, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(42)), nodeId))
				 .has_value());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findLargeValueManifestOptional(after, LargeValueId(1U)).has_value());
	EXPECT_TRUE(metadata_->findIndexRootOptional(after, scalar.index_id).has_value());
	EXPECT_TRUE(metadata_->findIndexRootOptional(after, vector.index_id).has_value());
	EXPECT_TRUE(metadata_->findVectorManifestOptional(after, vector.index_id).has_value());
	EXPECT_EQ(BTreeIndex(scalar.index_id, indexes, *metadata_)
				  .find(after, IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(42)), nodeId)),
			  std::optional<uint64_t>(nodeId));
	EXPECT_TRUE(metadata_->findVectorLocationOptional(after, 1U).has_value());
	EXPECT_EQ(graph.readNode(after, nodeId).properties,
			  (std::vector<PropertyCollectionRootEntry>{{BODY_PROPERTY_TOKEN, PropertyValue(body)},
																 {EMBEDDING_PROPERTY_TOKEN,
																  PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1)),
																										   PropertyValue(int64_t(2))})},
																 {SCORE_PROPERTY_TOKEN, PropertyValue(int64_t(42))}}));
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io_, *metadata_).scan()));
	after.release();
	before.release();
}

TEST_F(NativeGraphLargeValueMutationTest,
	   PublishesScalarAdjacencyAndContentIndexesIntoDistinctFramesOfOneSharedPool) {
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 97U, 'i');
	const IndexDefinition scalar = scalarDefinition();
	const std::array<std::byte, 1U> scalarKey = {std::byte{'s'}};
	BTreePageStore indexes(*slotted_pages_, *metadata_);

	// Seed three independent roots in one physical Index pool. The following
	// graph publication must COW all of them without assigning the same frame to
	// scalar, adjacency, and LargeValue content-index materializers.
	allocateEmptyScalarIndexRoot(indexes, scalar.index_id, PageId(101U), LogSequenceNumber(2U));
	appendEmptyIndexRoot(indexes, RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(102U), LogSequenceNumber(4U),
						 BTreeLeafValueLayout::BLVL_EMPTY);
	appendEmptyIndexRoot(indexes, RSS_LARGE_VALUE_CONTENT_INDEX_ID, PageId(103U), LogSequenceNumber(6U));

	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	const uint64_t source = transaction.createNode(std::array<uint64_t, 1U>{PERSON_LABEL_TOKEN});
	const uint64_t target = transaction.createNode(std::array<uint64_t, 1U>{PERSON_LABEL_TOKEN});
	const uint64_t edgeId = transaction.createEdge(source, target, 17U);
	transaction.replaceNodeProperty(entityPropertyRoot(source, body));
	transaction.stageScalarIndexUpsert(indexes, scalar.index_id, scalarKey, source);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const IndexRootLocation scalarRoot = metadata_->findIndexRoot(current, scalar.index_id);
	const IndexRootLocation adjacencyRoot =
			metadata_->findIndexRoot(current, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	const IndexRootLocation contentRoot =
			metadata_->findIndexRoot(current, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
	const PageLocation scalarLocation = metadata_->findPageLocation(current, scalarRoot.root_page_id);
	const PageLocation adjacencyLocation = metadata_->findPageLocation(current, adjacencyRoot.root_page_id);
	const PageLocation contentLocation = metadata_->findPageLocation(current, contentRoot.root_page_id);
	EXPECT_EQ(scalarLocation.segment_id, SegmentId(71U));
	EXPECT_EQ(adjacencyLocation.segment_id, SegmentId(71U));
	EXPECT_EQ(contentLocation.segment_id, SegmentId(71U));
	EXPECT_NE(scalarLocation.page_index, adjacencyLocation.page_index);
	EXPECT_NE(scalarLocation.page_index, contentLocation.page_index);
	EXPECT_NE(adjacencyLocation.page_index, contentLocation.page_index);
	EXPECT_EQ(BTreeIndex(scalar.index_id, indexes, *metadata_).find(current, scalarKey),
			  std::optional<uint64_t>(source));
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(current, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
			  (std::vector<uint64_t>{edgeId}));
	const LargeValueRef reference = storedLargeValueReference(current, *metadata_, *slotted_pages_, source).value;
	EXPECT_EQ(large_values_->read(current, reference), encodedEntityPropertyLargeValuePayload(body));
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io_, *metadata_).scan()));
	current.release();

	reopenWithLargeValues();
	BTreePageStore reopenedIndexes(*slotted_pages_, *metadata_);
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(BTreeIndex(scalar.index_id, reopenedIndexes, *metadata_).find(reopened, scalarKey),
			  std::optional<uint64_t>(source));
	GlobalAdjacencyIndex reopenedAdjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(reopenedAdjacency.scan(reopened, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
			  (std::vector<uint64_t>{edgeId}));
	const LargeValueRef reopenedReference =
			storedLargeValueReference(reopened, *metadata_, *slotted_pages_, source).value;
	EXPECT_EQ(large_values_->read(reopened, reopenedReference), encodedEntityPropertyLargeValuePayload(body));
	reopened.release();
}

TEST_F(NativeGraphLargeValueMutationTest,
	   RebuildsContentIndexFreshWhenSharedPoolDemandExceedsItsRemainingFrames) {
	constexpr size_t CONTENT_ENTRY_COUNT = 80U;
	constexpr size_t FINAL_ENTRY_COUNT = 20U;
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	allocateEmptyScalarIndexRoot(indexes, RSS_LARGE_VALUE_CONTENT_INDEX_ID, PageId(101U), LogSequenceNumber(2U));
	appendEmptyIndexRoot(indexes, RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(102U), LogSequenceNumber(4U),
						 BTreeLeafValueLayout::BLVL_EMPTY);
	MetadataSnapshotPin baseline = metadata_->openSnapshot();

	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphWriteTransaction seed = writer.beginTransaction();
	std::vector<uint64_t> nodeIds;
	std::vector<std::string> bodies;
	nodeIds.reserve(CONTENT_ENTRY_COUNT);
	bodies.reserve(CONTENT_ENTRY_COUNT);
	for (size_t ordinal = 0U; ordinal < CONTENT_ENTRY_COUNT; ++ordinal) {
		const uint64_t nodeId = seed.createNode(std::array<uint64_t, 1U>{PERSON_LABEL_TOKEN});
		bodies.emplace_back(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 97U, 'r');
		bodies.back().append("-" + std::to_string(ordinal));
		seed.replaceNodeProperty(entityPropertyRoot(nodeId, bodies.back()));
		nodeIds.push_back(nodeId);
	}
	const uint64_t seedEdgeId = seed.createEdge(nodeIds.front(), nodeIds.back(), 17U);
	ASSERT_TRUE(seed.commit().committed());

	// The content tree now spans an internal root and a leaf. Leave two frames:
	// the content update needs both pages and the competing adjacency update needs
	// one, so only the content candidate can resolve the aggregate shortfall.
	size_t appendedRoots = 0U;
	while (true) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const CatalogEntry pool = metadata_->findCatalog(snapshot, SegmentId(71U));
		const bool hasThreeFrames = indexes.hasReplacementFrameCapacity(pool, 3U);
		const uint64_t checkpoint = snapshot.superblock().checkpoint_lsn.value();
		snapshot.release();
		if (!hasThreeFrames) {
			break;
		}
		ASSERT_LT(appendedRoots, RSS_PAGE_POOL_DATA_FRAME_COUNT);
		appendEmptyIndexRoot(indexes, 10'000U + appendedRoots, PageId(1'000U + appendedRoots),
							 LogSequenceNumber(checkpoint + 1U));
		++appendedRoots;
	}

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const CatalogEntry originalPool = metadata_->findCatalog(before, SegmentId(71U));
	ASSERT_TRUE(indexes.hasReplacementFrameCapacity(originalPool, 2U));
	ASSERT_FALSE(indexes.hasReplacementFrameCapacity(originalPool, 3U));
	const IndexRootLocation contentBefore = metadata_->findIndexRoot(before, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
	ASSERT_EQ(metadata_->findPageLocation(before, contentBefore.root_page_id).segment_id, originalPool.segment_id);
	ASSERT_EQ(indexes.read(before, contentBefore.root_page_id).header().kind, BTreeNodeKind::BPN_INTERNAL);
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(before, GlobalAdjacencyDirection::GAD_OUT, nodeIds.front(), 4U).edge_ids,
			  (std::vector<uint64_t>{seedEdgeId}));

	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	std::vector<uint64_t> finalNodeIds;
	std::vector<std::string> finalBodies;
	finalNodeIds.reserve(FINAL_ENTRY_COUNT);
	finalBodies.reserve(FINAL_ENTRY_COUNT);
	for (size_t ordinal = 0U; ordinal < FINAL_ENTRY_COUNT; ++ordinal) {
		const uint64_t nodeId = transaction.createNode(std::array<uint64_t, 1U>{PERSON_LABEL_TOKEN});
		finalBodies.emplace_back(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 97U, 'z');
		// Match the seed payload widths so the content keys exercise multiple
		// existing leaves rather than one length-sorted rightmost leaf.
		finalBodies.back().append("-" + std::to_string(ordinal));
		transaction.replaceNodeProperty(entityPropertyRoot(nodeId, finalBodies.back()));
		finalNodeIds.push_back(nodeId);
	}
	const uint64_t edgeId = transaction.createEdge(nodeIds.front(), finalNodeIds.front(), 19U);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const PageLocation adjacencyLocation = metadata_->findPageLocation(
			current, metadata_->findIndexRoot(current, RSS_GLOBAL_ADJACENCY_INDEX_ID).root_page_id);
	const PageLocation contentLocation = metadata_->findPageLocation(
			current, metadata_->findIndexRoot(current, RSS_LARGE_VALUE_CONTENT_INDEX_ID).root_page_id);
	EXPECT_EQ(adjacencyLocation.segment_id, originalPool.segment_id);
	EXPECT_NE(contentLocation.segment_id, originalPool.segment_id);
	EXPECT_EQ(adjacency.scan(current, GlobalAdjacencyDirection::GAD_OUT, nodeIds.front(), 4U).edge_ids,
			  (std::vector<uint64_t>{seedEdgeId, edgeId}));
	const LargeValueRef firstReference =
			storedLargeValueReference(current, *metadata_, *slotted_pages_, nodeIds.front()).value;
	const LargeValueRef finalReference =
			storedLargeValueReference(current, *metadata_, *slotted_pages_, finalNodeIds.back()).value;
	EXPECT_EQ(large_values_->read(current, firstReference), encodedEntityPropertyLargeValuePayload(bodies.front()));
	EXPECT_EQ(large_values_->read(current, finalReference), encodedEntityPropertyLargeValuePayload(finalBodies.back()));
	EXPECT_EQ(adjacency.scan(before, GlobalAdjacencyDirection::GAD_OUT, nodeIds.front(), 4U).edge_ids,
			  (std::vector<uint64_t>{seedEdgeId}));
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io_, *metadata_).scan()));
	current.release();
	before.release();
	baseline.release();

	reopenWithLargeValues();
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_NE(metadata_->findPageLocation(
				  reopened, metadata_->findIndexRoot(reopened, RSS_LARGE_VALUE_CONTENT_INDEX_ID).root_page_id)
				  .segment_id,
			  originalPool.segment_id);
	const LargeValueRef reopenedFinal =
			storedLargeValueReference(reopened, *metadata_, *slotted_pages_, finalNodeIds.back()).value;
	EXPECT_EQ(large_values_->read(reopened, reopenedFinal), encodedEntityPropertyLargeValuePayload(finalBodies.back()));
	reopened.release();
}

TEST_F(NativeGraphLargeValueMutationTest,
	   RebuildsTheLargeValueContentIndexFreshWhenItsCurrentPoolHasNoFrameCapacity) {
	const std::string seedBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 97U, 's');
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 97U, 'f');
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	allocateEmptyScalarIndexRoot(indexes, RSS_LARGE_VALUE_CONTENT_INDEX_ID, PageId(101U), LogSequenceNumber(2U));
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphWriteTransaction seed = writer.beginTransaction();
	const uint64_t seedNodeId = seed.createNode(std::array<uint64_t, 1U>{PERSON_LABEL_TOKEN});
	seed.replaceNodeProperty(entityPropertyRoot(seedNodeId, seedBody));
	ASSERT_TRUE(seed.commit().committed());

	// The content tree now has a real entry. Exhausting its current pool makes
	// the next content update take the shared-capacity fresh-pool fallback.
	size_t appendedRoots = 0U;
	while (true) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const CatalogEntry pool = metadata_->findCatalog(snapshot, SegmentId(71U));
		const bool hasCapacity = indexes.hasReplacementFrameCapacity(pool, 1U);
		const uint64_t checkpoint = snapshot.superblock().checkpoint_lsn.value();
		snapshot.release();
		if (!hasCapacity) {
			break;
		}
		ASSERT_LT(appendedRoots, RSS_PAGE_POOL_DATA_FRAME_COUNT);
		appendEmptyIndexRoot(indexes, 20'000U + appendedRoots, PageId(2'000U + appendedRoots),
							 LogSequenceNumber(checkpoint + 1U));
		++appendedRoots;
	}

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const CatalogEntry originalPool = metadata_->findCatalog(before, SegmentId(71U));
	ASSERT_FALSE(indexes.hasReplacementFrameCapacity(originalPool, 1U));
	const LargeValueRef seedReference = storedLargeValueReference(before, *metadata_, *slotted_pages_, seedNodeId).value;
	EXPECT_EQ(large_values_->read(before, seedReference), encodedEntityPropertyLargeValuePayload(seedBody));

	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	const uint64_t nodeId = transaction.createNode(std::array<uint64_t, 1U>{PERSON_LABEL_TOKEN});
	transaction.replaceNodeProperty(entityPropertyRoot(nodeId, body));
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const IndexRootLocation contentRoot = metadata_->findIndexRoot(current, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
	EXPECT_NE(metadata_->findPageLocation(current, contentRoot.root_page_id).segment_id, originalPool.segment_id);
	const LargeValueRef reference = storedLargeValueReference(current, *metadata_, *slotted_pages_, nodeId).value;
	EXPECT_EQ(large_values_->read(current, reference), encodedEntityPropertyLargeValuePayload(body));
	EXPECT_EQ(large_values_->read(current, seedReference), encodedEntityPropertyLargeValuePayload(seedBody));
	EXPECT_FALSE(metadata_->findLargeValueManifestOptional(before, reference.value_id).has_value());
	current.release();
	before.release();
}

TEST_F(NativeGraphLargeValueMutationTest,
	   RebuildsGlobalAdjacencyFreshBeforeTheCompetingLargeValueContentIndex) {
	const std::string seedBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 97U, 'a');
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 97U, 'b');
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	allocateEmptyScalarIndexRoot(indexes, RSS_LARGE_VALUE_CONTENT_INDEX_ID, PageId(101U), LogSequenceNumber(2U));
	appendEmptyIndexRoot(indexes, RSS_GLOBAL_ADJACENCY_INDEX_ID, PageId(102U), LogSequenceNumber(4U),
						 BTreeLeafValueLayout::BLVL_EMPTY);

	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphWriteTransaction seed = writer.beginTransaction();
	const uint64_t source = seed.createNode(std::array<uint64_t, 1U>{PERSON_LABEL_TOKEN});
	const uint64_t seedTarget = seed.createNode(std::array<uint64_t, 1U>{PERSON_LABEL_TOKEN});
	const uint64_t seedEdgeId = seed.createEdge(source, seedTarget, 43U);
	seed.replaceNodeProperty(entityPropertyRoot(source, seedBody));
	ASSERT_TRUE(seed.commit().committed());

	// Each of the two current trees can COW by itself, but not together. The
	// planner must select the first (global adjacency) candidate for a fresh
	// rebuild and retain the content-index update in the remaining frame.
	size_t appendedRoots = 0U;
	while (true) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const CatalogEntry pool = metadata_->findCatalog(snapshot, SegmentId(71U));
		const bool hasTwoFrames = indexes.hasReplacementFrameCapacity(pool, 2U);
		const uint64_t checkpoint = snapshot.superblock().checkpoint_lsn.value();
		snapshot.release();
		if (!hasTwoFrames) {
			break;
		}
		ASSERT_LT(appendedRoots, RSS_PAGE_POOL_DATA_FRAME_COUNT);
		appendEmptyIndexRoot(indexes, 30'000U + appendedRoots, PageId(3'000U + appendedRoots),
							 LogSequenceNumber(checkpoint + 1U));
		++appendedRoots;
	}

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const CatalogEntry originalPool = metadata_->findCatalog(before, SegmentId(71U));
	ASSERT_TRUE(indexes.hasReplacementFrameCapacity(originalPool, 1U));
	ASSERT_FALSE(indexes.hasReplacementFrameCapacity(originalPool, 2U));
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(before, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
			  (std::vector<uint64_t>{seedEdgeId}));

	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	const uint64_t target = transaction.createNode(std::array<uint64_t, 1U>{PERSON_LABEL_TOKEN});
	const uint64_t edgeId = transaction.createEdge(source, target, 47U);
	transaction.replaceNodeProperty(entityPropertyRoot(target, body));
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const IndexRootLocation adjacencyRoot = metadata_->findIndexRoot(current, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	const IndexRootLocation contentRoot = metadata_->findIndexRoot(current, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
	EXPECT_NE(metadata_->findPageLocation(current, adjacencyRoot.root_page_id).segment_id, originalPool.segment_id);
	EXPECT_EQ(metadata_->findPageLocation(current, contentRoot.root_page_id).segment_id, originalPool.segment_id);
	EXPECT_EQ(adjacency.scan(current, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
			  (std::vector<uint64_t>{seedEdgeId, edgeId}));
	const LargeValueRef reference = storedLargeValueReference(current, *metadata_, *slotted_pages_, target).value;
	EXPECT_EQ(large_values_->read(current, reference), encodedEntityPropertyLargeValuePayload(body));
	EXPECT_EQ(adjacency.scan(before, GlobalAdjacencyDirection::GAD_OUT, source, 4U).edge_ids,
			  (std::vector<uint64_t>{seedEdgeId}));
	current.release();
	before.release();
}

TEST_F(NativeGraphLargeValueMutationTest, PublishesCompressedLargeValueReferencesWithTheirOwningProperty) {
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 211U, 'z');
	NativeGraphWritePlannerOptions plannerOptions;
	plannerOptions.property_roots.large_value_compression.codec = LargeValueCodec::LVC_LZ4;
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphMutationExecutor executor(
			writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, largeValueTokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"}, {{"body", PropertyValue(body)}}}};
	const NativeGraphMutationExecutionResult result = executor.execute(create);
	ASSERT_TRUE(result.committed());
	ASSERT_EQ(result.result.created_node_ids.size(), 1U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const EntityPropertyLargeValueReference reference =
			storedLargeValueReference(snapshot, *metadata_, *slotted_pages_, result.result.created_node_ids.front());
	EXPECT_EQ(reference.value.codec, LargeValueCodec::LVC_LZ4);
	EXPECT_EQ(large_values_->manifest(snapshot, reference.value).codec, LargeValueCodec::LVC_LZ4);
	EXPECT_EQ(graph.readNode(snapshot, result.result.created_node_ids.front()).properties,
			  (std::vector<PropertyCollectionRootEntry>{{BODY_PROPERTY_TOKEN, PropertyValue(body)}}));
}

TEST_F(NativeGraphLargeValueMutationTest, ReusesSharedLargeValueDataPoolAcrossGraphPublicationsAndReopen) {
	const std::string firstBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 31U, 'a');
	const std::string secondBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 79U, 'b');
	LargeValueRef firstReference;
	LargeValueRef secondReference;
	uint64_t firstNodeId = 0U;
	uint64_t secondNodeId = 0U;
	uint64_t expectedDataPageCount = 0U;
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		NativeGraphMutationExecutor executor(
				writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, largeValueTokens(),
				[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
				[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

		LogicalGraphMutationBatch createFirst;
		createFirst.node_creates = {{{"Person"}, {{"body", PropertyValue(firstBody)}}}};
		const NativeGraphMutationExecutionResult first = executor.execute(createFirst);
		ASSERT_TRUE(first.committed());
		ASSERT_EQ(first.result.created_node_ids.size(), 1U);
		firstNodeId = first.result.created_node_ids.front();
		MetadataSnapshotPin beforeSecond = metadata_->openSnapshot();
		firstReference = storedLargeValueReference(beforeSecond, *metadata_, *slotted_pages_, firstNodeId).value;
		EXPECT_EQ(large_values_->read(beforeSecond, firstReference),
				  encodedEntityPropertyLargeValuePayload(firstBody));

		LogicalGraphMutationBatch createSecond;
		createSecond.node_creates = {{{"Person"}, {{"body", PropertyValue(secondBody)}}}};
		const NativeGraphMutationExecutionResult second = executor.execute(createSecond);
		ASSERT_TRUE(second.committed());
		ASSERT_EQ(second.result.created_node_ids.size(), 1U);
		secondNodeId = second.result.created_node_ids.front();
		MetadataSnapshotPin current = metadata_->openSnapshot();
		secondReference = storedLargeValueReference(current, *metadata_, *slotted_pages_, secondNodeId).value;
		const LargeValueManifest firstManifest = large_values_->manifest(current, firstReference);
		const LargeValueManifest secondManifest = large_values_->manifest(current, secondReference);
		ASSERT_FALSE(firstManifest.chunks.empty());
		ASSERT_FALSE(secondManifest.chunks.empty());
		const PageLocation firstLocation =
				metadata_->findPageLocation(current, firstManifest.chunks.front().first_data_page_id);
		const PageLocation secondLocation =
				metadata_->findPageLocation(current, secondManifest.chunks.front().first_data_page_id);
		EXPECT_EQ(firstLocation.segment_id, secondLocation.segment_id);
		const CatalogEntry sharedPool = metadata_->findCatalog(current, firstLocation.segment_id);
		EXPECT_EQ(sharedPool.kind, SegmentKind::SGK_LARGE_VALUE_DATA_POOL);
		EXPECT_EQ(sharedPool.owner_id, RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID);
		const auto dataPageCount = [this, &current](const LargeValueManifest &manifest) {
			uint64_t count = 0U;
			for (const LargeValueManifest &node: large_values_->manifestTree(current, manifest)) {
				for (const LargeValueChunk &chunk: node.chunks) {
					count += chunk.data_page_count;
				}
			}
			return count;
		};
		expectedDataPageCount = dataPageCount(firstManifest) + dataPageCount(secondManifest);
		EXPECT_EQ(large_values_->read(current, firstReference),
				  encodedEntityPropertyLargeValuePayload(firstBody));
		EXPECT_EQ(large_values_->read(current, secondReference),
				  encodedEntityPropertyLargeValuePayload(secondBody));
		EXPECT_EQ(RelocatableIntegrityScanner(io_, *metadata_).scan().large_value_data_pages, expectedDataPageCount);
		EXPECT_EQ(large_values_->read(beforeSecond, firstReference),
				  encodedEntityPropertyLargeValuePayload(firstBody));
		current.release();
		beforeSecond.release();
	}

	reopenWithLargeValues();
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(large_values_->read(reopened, firstReference),
			  encodedEntityPropertyLargeValuePayload(firstBody));
	EXPECT_EQ(large_values_->read(reopened, secondReference),
			  encodedEntityPropertyLargeValuePayload(secondBody));
	EXPECT_EQ(RelocatableIntegrityScanner(io_, *metadata_).scan().large_value_data_pages, expectedDataPageCount);
}

TEST_F(NativeGraphLargeValueMutationTest, ReusesPublishedSharedPoolsForSeveralValuesInOneLaterPublication) {
	const std::array bodies{std::string(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 31U, 'e'),
						  std::string(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 47U, 'f'),
						  std::string(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 63U, 'g')};
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	const std::array<uint64_t, 0U> noLabels{};

	NativeGraphWriteTransaction first = writer.beginTransaction();
	const uint64_t firstNodeId = first.createNode(noLabels);
	first.replaceNodeProperty(entityPropertyRoot(firstNodeId, bodies[0]));
	ASSERT_TRUE(first.commit().committed());
	MetadataSnapshotPin firstSnapshot = metadata_->openSnapshot();
	const LargeValueRef firstReference =
			storedLargeValueReference(firstSnapshot, *metadata_, *slotted_pages_, firstNodeId).value;
	const std::array firstReferences{firstReference};
	const LargeValuePoolIds firstPools =
			collectLargeValuePoolIds(firstSnapshot, *metadata_, *large_values_, firstReferences);
	ASSERT_EQ(firstPools.data.size(), 1U);
	ASSERT_EQ(firstPools.manifest.size(), 1U);
	firstSnapshot.release();

	NativeGraphWriteTransaction later = writer.beginTransaction();
	const uint64_t secondNodeId = later.createNode(noLabels);
	const uint64_t thirdNodeId = later.createNode(noLabels);
	later.replaceNodeProperty(entityPropertyRoot(secondNodeId, bodies[1]));
	later.replaceNodeProperty(entityPropertyRoot(thirdNodeId, bodies[2]));
	ASSERT_TRUE(later.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const LargeValueRef secondReference =
			storedLargeValueReference(current, *metadata_, *slotted_pages_, secondNodeId).value;
	const LargeValueRef thirdReference =
			storedLargeValueReference(current, *metadata_, *slotted_pages_, thirdNodeId).value;
	const std::array references{firstReference, secondReference, thirdReference};
	const LargeValuePoolIds pools = collectLargeValuePoolIds(current, *metadata_, *large_values_, references);
	EXPECT_EQ(pools.data, firstPools.data);
	EXPECT_EQ(pools.manifest, firstPools.manifest);
	EXPECT_EQ(large_values_->read(current, firstReference), encodedEntityPropertyLargeValuePayload(bodies[0]));
	EXPECT_EQ(large_values_->read(current, secondReference), encodedEntityPropertyLargeValuePayload(bodies[1]));
	EXPECT_EQ(large_values_->read(current, thirdReference), encodedEntityPropertyLargeValuePayload(bodies[2]));
	current.release();

	reopenWithLargeValues();
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(large_values_->read(reopened, secondReference), encodedEntityPropertyLargeValuePayload(bodies[1]));
	EXPECT_EQ(large_values_->read(reopened, thirdReference), encodedEntityPropertyLargeValuePayload(bodies[2]));
}

TEST_F(NativeGraphLargeValueMutationTest, PacksMultipleFreshLargeValuesIntoSharedPoolsInOnePublication) {
	const std::string firstBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 31U, 'm');
	const std::string secondBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 79U, 'n');
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphMutationExecutor executor(
			writer, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, largeValueTokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"}, {{"body", PropertyValue(firstBody)}}},
						   {{"Person"}, {{"body", PropertyValue(secondBody)}}}};
	const NativeGraphMutationExecutionResult result = executor.execute(create);
	ASSERT_TRUE(result.committed());
	ASSERT_EQ(result.result.created_node_ids.size(), 2U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const LargeValueRef firstReference = storedLargeValueReference(
			snapshot, *metadata_, *slotted_pages_, result.result.created_node_ids[0]).value;
	const LargeValueRef secondReference = storedLargeValueReference(
			snapshot, *metadata_, *slotted_pages_, result.result.created_node_ids[1]).value;
	EXPECT_NE(firstReference.value_id, secondReference.value_id);
	const std::array references{firstReference, secondReference};
	const LargeValuePoolIds pools =
			collectLargeValuePoolIds(snapshot, *metadata_, *large_values_, references);
	ASSERT_EQ(pools.data.size(), 1U);
	ASSERT_EQ(pools.manifest.size(), 1U);
	const CatalogEntry dataPool = metadata_->findCatalog(snapshot, SegmentId(*pools.data.begin()));
	const CatalogEntry manifestPool = metadata_->findCatalog(snapshot, SegmentId(*pools.manifest.begin()));
	EXPECT_EQ(dataPool.owner_id, RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID);
	EXPECT_EQ(manifestPool.owner_id, RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID);
	size_t dataPoolEntries = 0U;
	size_t manifestPoolEntries = 0U;
	metadata_->visitCatalogEntries(snapshot, [&dataPoolEntries, &manifestPoolEntries](const CatalogEntry &entry) {
		dataPoolEntries += entry.kind == SegmentKind::SGK_LARGE_VALUE_DATA_POOL ? 1U : 0U;
		manifestPoolEntries += entry.kind == SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL ? 1U : 0U;
	});
	EXPECT_EQ(dataPoolEntries, 1U);
	EXPECT_EQ(manifestPoolEntries, 1U);
	EXPECT_EQ(large_values_->read(snapshot, firstReference),
			  encodedEntityPropertyLargeValuePayload(firstBody));
	EXPECT_EQ(large_values_->read(snapshot, secondReference),
			  encodedEntityPropertyLargeValuePayload(secondBody));
	snapshot.release();

	reopenWithLargeValues();
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(large_values_->read(reopened, firstReference),
			  encodedEntityPropertyLargeValuePayload(firstBody));
	EXPECT_EQ(large_values_->read(reopened, secondReference),
			  encodedEntityPropertyLargeValuePayload(secondBody));
}

TEST_F(NativeGraphLargeValueMutationTest, RecoversOldGenerationWhenSharedFreshPoolPublicationStopsBeforeSuperblock) {
	const std::string firstBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 31U, 'p');
	const std::string secondBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 79U, 'q');
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	MetadataSnapshotPin originalSnapshot = metadata_->openSnapshot();
	const Superblock original = originalSnapshot.superblock();
	originalSnapshot.release();

	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	const std::array<uint64_t, 0U> noLabels{};
	const uint64_t firstNodeId = transaction.createNode(noLabels);
	const uint64_t secondNodeId = transaction.createNode(noLabels);
	MetadataSnapshotPin leaseSnapshot = metadata_->openSnapshot();
	const Superblock leasePublication = leaseSnapshot.superblock();
	leaseSnapshot.release();
	transaction.replaceNodeProperty(entityPropertyRoot(firstNodeId, firstBody));
	transaction.replaceNodeProperty(entityPropertyRoot(secondNodeId, secondBody));
	EXPECT_THROW(static_cast<void>(transaction.commit(
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenWithLargeValues();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	// Logical-ID leasing is independently durable. A failed graph publication
	// must retain that lease but must not expose any graph or LargeValue root.
	EXPECT_EQ(recovered.superblock(), leasePublication);
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(recovered, firstNodeId).has_value());
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(recovered, secondNodeId).has_value());
	EXPECT_FALSE(metadata_->findLargeValueManifestOptional(recovered, LargeValueId(original.next_large_value_id))
					 .has_value());
	recovered.release();
	const RelocatableIntegrityReport report = RelocatableIntegrityScanner(io_, *metadata_).scan();
	EXPECT_EQ(report.large_value_manifest_routes, 0U);
	EXPECT_EQ(report.large_value_manifest_nodes, 0U);
	EXPECT_EQ(report.large_value_data_pages, 0U);
	EXPECT_EQ(report.large_value_content_index_entries, 0U);
}

TEST_F(NativeGraphLargeValueMutationTest, RecoversSharedFreshPoolsWhenPublicationStopsAfterSuperblock) {
	const std::string firstBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 31U, 'r');
	const std::string secondBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 79U, 's');
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	MetadataSnapshotPin originalSnapshot = metadata_->openSnapshot();
	const Superblock original = originalSnapshot.superblock();
	originalSnapshot.release();

	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	const std::array<uint64_t, 0U> noLabels{};
	const uint64_t firstNodeId = transaction.createNode(noLabels);
	const uint64_t secondNodeId = transaction.createNode(noLabels);
	MetadataSnapshotPin leaseSnapshot = metadata_->openSnapshot();
	const Superblock leasePublication = leaseSnapshot.superblock();
	leaseSnapshot.release();
	transaction.replaceNodeProperty(entityPropertyRoot(firstNodeId, firstBody));
	transaction.replaceNodeProperty(entityPropertyRoot(secondNodeId, secondBody));
	EXPECT_THROW(static_cast<void>(transaction.commit(
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenWithLargeValues();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(leasePublication.generation.value() + 1U));
	EXPECT_EQ(recovered.superblock().next_large_value_id, original.next_large_value_id + 2U);
	const LargeValueRef firstReference =
			storedLargeValueReference(recovered, *metadata_, *slotted_pages_, firstNodeId).value;
	const LargeValueRef secondReference =
			storedLargeValueReference(recovered, *metadata_, *slotted_pages_, secondNodeId).value;
	EXPECT_EQ(large_values_->read(recovered, firstReference), encodedEntityPropertyLargeValuePayload(firstBody));
	EXPECT_EQ(large_values_->read(recovered, secondReference), encodedEntityPropertyLargeValuePayload(secondBody));
	const std::array references{firstReference, secondReference};
	const LargeValuePoolIds pools =
			collectLargeValuePoolIds(recovered, *metadata_, *large_values_, references);
	EXPECT_EQ(pools.data.size(), 1U);
	EXPECT_EQ(pools.manifest.size(), 1U);
	recovered.release();
	const RelocatableIntegrityReport report = RelocatableIntegrityScanner(io_, *metadata_).scan();
	EXPECT_EQ(report.large_value_manifest_routes, 2U);
	EXPECT_EQ(report.large_value_content_index_entries, 2U);
}

TEST_F(NativeGraphLargeValueMutationTest,
	   RecoversNoPartialEntityLargeValueOrScalarIndexStateBeforeSuperblockPublication) {
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 59U, 'u');
	const std::array<uint64_t, 0U> noLabels{};
	const std::array<std::byte, 1U> key = {std::byte{'u'}};
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	const uint64_t nodeId = transaction.createNode(noLabels);
	MetadataSnapshotPin leased = metadata_->openSnapshot();
	const Superblock leasePublication = leased.superblock();
	const LargeValueId firstLargeValueId(leasePublication.next_large_value_id);
	leased.release();
	transaction.replaceNodeProperty(entityPropertyRoot(nodeId, body));
	const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), nodeId}}};
	transaction.stageScalarIndexCreate(indexes, 43U, entries);

	EXPECT_THROW(static_cast<void>(transaction.commit(
				 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
			 BootstrapPublishInterrupted);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenWithLargeValues();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock(), leasePublication);
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(recovered, nodeId).has_value());
	EXPECT_FALSE(metadata_->findLargeValueManifestOptional(recovered, firstLargeValueId).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, 43U).has_value());
	recovered.release();
}

TEST_F(NativeGraphLargeValueMutationTest,
	   RecoversNoPartialEntityLargeValueScalarIndexOrVectorStateBeforeSuperblockPublication) {
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 63U, 'w');
	const std::array<uint64_t, 0U> noLabels{};
	const std::array<std::byte, 1U> scalarKey = {std::byte{'w'}};
	const IndexDefinition vector = vectorDefinition();
	const std::array<NativeGraphVectorBootstrapRecord, 1U> vectorRecords = {{
			{0U, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
	}};
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	const uint64_t nodeId = transaction.createNode(noLabels);
	MetadataSnapshotPin leased = metadata_->openSnapshot();
	const Superblock leasePublication = leased.superblock();
	const LargeValueId firstLargeValueId(leasePublication.next_large_value_id);
	leased.release();
	transaction.replaceNodeProperty(entityPropertyRoot(nodeId, body));
	const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(scalarKey.begin(), scalarKey.end()), nodeId}}};
	transaction.stageScalarIndexCreate(indexes, 43U, entries);
	auto records = vectorRecords;
	records.front().node_id = nodeId;
	transaction.stageVectorIndexBootstrap(indexes, *vector_pages_, *vector_manifests_, vector, records,
									 vectorBootstrapOptions());

	EXPECT_THROW(static_cast<void>(transaction.commit(
				 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
			 BootstrapPublishInterrupted);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenWithLargeValues();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock(), leasePublication);
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(recovered, nodeId).has_value());
	EXPECT_FALSE(metadata_->findLargeValueManifestOptional(recovered, firstLargeValueId).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, 43U).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, vector.index_id).has_value());
	EXPECT_FALSE(metadata_->findVectorManifestOptional(recovered, vector.index_id).has_value());
	EXPECT_FALSE(metadata_->findVectorLocationOptional(recovered, 1U).has_value());
	recovered.release();
}

TEST_F(NativeGraphLargeValueMutationTest,
	   PostSuperblockInterruptionReopensTheCompleteLargeValueScalarIndexAndVectorGeneration) {
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 67U, 'y');
	const std::array<uint64_t, 0U> noLabels{};
	const std::array<std::byte, 1U> scalarKey = {std::byte{'y'}};
	const IndexDefinition vector = vectorDefinition();
	uint64_t nodeId = 0U;
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		NativeGraphWriteTransaction transaction = writer.beginTransaction();
		nodeId = transaction.createNode(noLabels);
		transaction.replaceNodeProperty(entityPropertyRoot(nodeId, body));
		const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(scalarKey.begin(), scalarKey.end()), nodeId}}};
		transaction.stageScalarIndexCreate(indexes, 43U, entries);
		const std::array<NativeGraphVectorBootstrapRecord, 1U> records = {{
				{nodeId, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
		}};
		transaction.stageVectorIndexBootstrap(indexes, *vector_pages_, *vector_manifests_, vector, records,
										 vectorBootstrapOptions());

		EXPECT_THROW(static_cast<void>(transaction.commit(
					 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
			 BootstrapPublishInterrupted);
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
	}

	reopenWithLargeValues();
	BTreePageStore reopenedIndexes(*slotted_pages_, *metadata_);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	const EntityPropertyLargeValueReference reference =
			storedLargeValueReference(recovered, *metadata_, *slotted_pages_, nodeId);
	EXPECT_EQ(large_values_->read(recovered, reference.value), encodedEntityPropertyLargeValuePayload(body));
	EXPECT_EQ(BTreeIndex(43U, reopenedIndexes, *metadata_).find(recovered, scalarKey), std::optional<uint64_t>(nodeId));
	EXPECT_TRUE(metadata_->findIndexRootOptional(recovered, vector.index_id).has_value());
	EXPECT_TRUE(metadata_->findVectorManifestOptional(recovered, vector.index_id).has_value());
	EXPECT_TRUE(metadata_->findVectorLocationOptional(recovered, 1U).has_value());
	EXPECT_EQ(graph.readNode(recovered, nodeId).properties,
			  (std::vector<PropertyCollectionRootEntry>{{BODY_PROPERTY_TOKEN, PropertyValue(body)}}));
	recovered.release();
}

TEST_F(NativeGraphLargeValueMutationTest,
	   RecoversOneCompleteEntityLargeValueAndScalarIndexStateAfterSuperblockPublication) {
	const std::string body(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 61U, 'v');
	const std::array<uint64_t, 0U> noLabels{};
	const std::array<std::byte, 1U> key = {std::byte{'v'}};
	uint64_t nodeId = 0U;
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
		NativeGraphWriteTransaction transaction = writer.beginTransaction();
		nodeId = transaction.createNode(noLabels);
		MetadataSnapshotPin leased = metadata_->openSnapshot();
		const Superblock leasePublication = leased.superblock();
		const LargeValueId firstLargeValueId(leasePublication.next_large_value_id);
		leased.release();
		transaction.replaceNodeProperty(entityPropertyRoot(nodeId, body));
		const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), nodeId}}};
		transaction.stageScalarIndexCreate(indexes, 43U, entries);

		EXPECT_THROW(static_cast<void>(transaction.commit(
					 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
		MetadataSnapshotPin current = metadata_->openSnapshot();
		EXPECT_EQ(current.superblock().generation, Generation(leasePublication.generation.value() + 1U));
		EXPECT_TRUE(metadata_->findLargeValueManifestOptional(current, firstLargeValueId).has_value());
		EXPECT_TRUE(metadata_->findIndexRootOptional(current, 43U).has_value());
		current.release();
	}

	reopenWithLargeValues();
	BTreePageStore reopenedIndexes(*slotted_pages_, *metadata_);
	BTreeIndex reopenedIndex(43U, reopenedIndexes, *metadata_);
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	const EntityPropertyLargeValueReference reference =
			storedLargeValueReference(recovered, *metadata_, *slotted_pages_, nodeId);
	EXPECT_EQ(large_values_->read(recovered, reference.value), encodedEntityPropertyLargeValuePayload(body));
	EXPECT_EQ(reopenedIndex.find(recovered, key), std::optional<uint64_t>(nodeId));
	EXPECT_TRUE(metadata_->findIndexRoot(recovered, 43U).root_page_id.isValid());
	recovered.release();
}

TEST_F(NativeGraphLargeValueMutationTest, ReusesSeveralSnapshotVerifiedContentsWithoutAllocatingAnotherLargeValue) {
	const std::string firstBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 67U, 'd');
	const std::string secondBody(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 83U, 'e');
	const std::array<uint64_t, 0U> noLabels{};
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphWriteTransaction create = writer.beginTransaction();
	const uint64_t firstNodeId = create.createNode(noLabels);
	const uint64_t secondNodeId = create.createNode(noLabels);
	ASSERT_TRUE(create.commit().committed());

	const auto makeRoot = [](uint64_t nodeId, const std::string &body) {
		PropertyCollectionRoot root;
		root.owner_entity_id = nodeId;
		root.owner_entity_type = graph::Node::typeId;
		root.entries = {{BODY_PROPERTY_TOKEN, PropertyValue(body)}};
		return root;
	};
	const PropertyCollectionRoot firstRoot = makeRoot(firstNodeId, firstBody);
	const PropertyCollectionRoot secondRoot = makeRoot(secondNodeId, secondBody);
	LargeValueWriteOptions options;
	options.first_data_slot = PhysicalSlotId(1U);
	options.last_data_slot = PhysicalSlotId(64U);
	options.first_manifest_slot = PhysicalSlotId(1U);
	options.last_manifest_slot = PhysicalSlotId(64U);
	options.kind = LargeValueKind::LVK_BYTES;
	options.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
	const auto writeCanonical = [&](const PropertyCollectionRoot &root) {
		const std::vector<std::byte> bytes = EntityPropertyLargeValuePayloadCodec::encode(root.entries);
		MetadataSnapshotPin beforeWrite = metadata_->openSnapshot();
		const LogSequenceNumber writeLsn(beforeWrite.superblock().checkpoint_lsn.value() + 1U);
		beforeWrite.release();
		return std::pair{large_values_->write(bytes, options, writeLsn), bytes};
	};
	const auto [firstCanonical, firstCanonicalBytes] = writeCanonical(firstRoot);
	const auto [secondCanonical, secondCanonicalBytes] = writeCanonical(secondRoot);

	NativeGraphWriteTransaction replace = writer.beginTransaction();
	replace.replaceNodeProperty(firstRoot);
	replace.replaceNodeProperty(secondRoot);
	ASSERT_TRUE(replace.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(storedLargeValueReference(snapshot, *metadata_, *slotted_pages_, firstNodeId).value,
			  firstCanonical.reference);
	EXPECT_EQ(storedLargeValueReference(snapshot, *metadata_, *slotted_pages_, secondNodeId).value,
			  secondCanonical.reference);
	EXPECT_EQ(snapshot.superblock().next_large_value_id, 3U);
	EXPECT_EQ(large_values_->read(snapshot, firstCanonical.reference), firstCanonicalBytes);
	EXPECT_EQ(large_values_->read(snapshot, secondCanonical.reference), secondCanonicalBytes);
	snapshot.release();
}

TEST_F(NativeGraphLargeValueMutationTest, RejectsExternalMutationOfReservedContentIndex) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner writer(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphWriteTransaction transaction = writer.beginTransaction();
	const std::array<std::byte, 1U> key{std::byte{'x'}};
	const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), 1U}}};

	EXPECT_THROW(transaction.stageScalarIndexUpsert(indexes, RSS_LARGE_VALUE_CONTENT_INDEX_ID, key, 1U),
			 FormatException);
	EXPECT_THROW(transaction.stageScalarIndexErase(indexes, RSS_LARGE_VALUE_CONTENT_INDEX_ID, key), FormatException);
	EXPECT_THROW(transaction.stageScalarIndexCreate(indexes, RSS_LARGE_VALUE_CONTENT_INDEX_ID, entries),
			 FormatException);
	EXPECT_THROW(transaction.stageScalarIndexDrop(indexes, RSS_LARGE_VALUE_CONTENT_INDEX_ID, 1U), FormatException);
}
