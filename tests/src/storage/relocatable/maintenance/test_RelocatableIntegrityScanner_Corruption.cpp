/**
 * @file test_RelocatableIntegrityScanner_Corruption.cpp
 * @brief Additional immutable-object and fixed-record corruption tests for the relocatable scanner.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "storage/relocatable/maintenance/RelocatableIntegrityScannerTestFixture.hpp"

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Types.hpp"
#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/StateRecordStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorManifestPage.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPage.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;
using graph::storage::test::integrityScannerExtentHeaderOffset;
using graph::storage::test::integrityScannerInitialSuperblock;
using graph::storage::test::integrityScannerNextCheckpointLsn;
using graph::storage::test::integrityScannerPagePayloadOffset;
using graph::storage::test::overwriteIntegrityScannerPagePayload;
using graph::storage::test::RelocatableIntegrityScannerTestFixture;

namespace {

	void replaceIntegrityScannerBTreePage(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
										  PageId pageId, const BTreePage &replacement) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		PageLocation location = metadata.findPageLocation(snapshot, pageId);
		location.free_bytes = replacement.slottedPage().stats().contiguous_free_bytes;
		snapshot.release();
		overwriteIntegrityScannerPagePayload(io, metadata, pageId, replacement.serialize());
		static_cast<void>(
				metadata.upsertLocations(std::span(&location, 1U), {}, integrityScannerNextCheckpointLsn(metadata)));
	}

	void overwriteIntegrityScannerPageOwner(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
											const PageOwnerHeader &owner) {
		const auto bytes = PageOwnerHeaderCodec::encode(owner);
		io->writeAt(integrityScannerPagePayloadOffset(metadata, owner.page_id) - RSS_PAGE_OWNER_HEADER_BYTES,
					bytes.data(), bytes.size());
	}

	struct IntegrityLocationLeafImage {
		Superblock superblock;
		PhysicalPageRef leaf_reference;
		LocationLeafPage leaf;
		size_t entry_index = 0U;
	};

	[[nodiscard]] IntegrityLocationLeafImage readIntegrityLocationLeaf(const std::shared_ptr<StorageIO> &io,
																	   RelocatableMetadataStore &metadata,
																	   PhysicalPageRef root, uint64_t logicalId) {
		const MetadataSnapshotPin snapshot = metadata.openSnapshot();
		IntegrityLocationLeafImage result;
		result.superblock = snapshot.superblock();
		MetadataPageFile pages(io, result.superblock);
		const uint64_t leafOrdinal = (logicalId - 1U) / RSS_LOCATION_LEAF_MAX_ENTRIES;
		MetadataRadixPage radix = pages.readMetadataRadix(root);
		for (;;) {
			const std::optional<PhysicalPageRef> child =
					CatalogRadixPageCodec::findChildForLeafOrdinal(radix, leafOrdinal);
			if (!child.has_value()) {
				throw std::runtime_error("test Location Directory is missing its route leaf");
			}
			if (radix.level == 0U) {
				result.leaf_reference = *child;
				break;
			}
			radix = pages.readMetadataRadix(*child);
		}
		result.leaf = pages.readLocationLeaf(result.leaf_reference);
		const auto entry = std::ranges::find(result.leaf.entries, logicalId, &LocationLeafEntry::logical_id);
		if (entry == result.leaf.entries.end()) {
			throw std::runtime_error("test Location Directory leaf is missing its logical entry");
		}
		result.entry_index = static_cast<size_t>(std::distance(result.leaf.entries.begin(), entry));
		return result;
	}

	void overwriteIntegrityLocationLeaf(const std::shared_ptr<StorageIO> &io, const IntegrityLocationLeafImage &image) {
		MetadataPageFile(io, image.superblock).writeLocationLeaf(image.leaf_reference, image.leaf);
	}

	struct NativeEntityIntegrityGraph {
		std::unique_ptr<RelocatableMetadataStore> metadata;
		EntityPropertyKey property_key{EntityPropertyOwnerKind::EPOK_NODE, 1001U};
		EntityPropertyKey edge_property_key{EntityPropertyOwnerKind::EPOK_EDGE, 2001U};
		PageId node_page{};
	};

	[[nodiscard]] NativeEntityIntegrityGraph createNativeEntityIntegrityGraph(const std::shared_ptr<StorageIO> &io) {
		NativeEntityIntegrityGraph result;
		Superblock initial = integrityScannerInitialSuperblock();
		initial.next_logical_node_id = result.property_key.entity_id;
		initial.next_logical_edge_id = result.edge_property_key.entity_id;
		result.metadata = RelocatableMetadataStore::initializeNew(io, initial);
		EntityPageStore entities(io, *result.metadata);
		SlottedPagePool pages(io, *result.metadata);
		EntityLabelSetStore labels(io, *result.metadata, pages);
		PropertyCollectionRootStore properties(pages, *result.metadata);
		std::vector<uint64_t> labelIds;
		labelIds.reserve(RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
		for (size_t index = 0U; index <= RSS_ENTITY_INLINE_LABEL_CAPACITY; ++index) {
			labelIds.push_back(7U + static_cast<uint64_t>(index) * 4U);
		}
		BTreePageStore trees(pages, *result.metadata);
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(42U);
		options.entity_pool_owner_id = 901U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(42U), 903U};
		NativeGraphWritePlanner planner(*result.metadata, entities, labels, properties, trees, options);
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		const uint64_t nodeId = transaction.createNode(labelIds);
		const uint64_t edgeId = transaction.createEdge(nodeId, nodeId, 77U);
		if (nodeId != result.property_key.entity_id || edgeId != result.edge_property_key.entity_id) {
			throw std::runtime_error("test native graph writer allocated unexpected logical entity IDs");
		}
		transaction.replaceNodeProperty(
				{nodeId, graph::Node::typeId, {{3U, graph::PropertyValue("native integrity root")}}});
		transaction.replaceEdgeProperty(
				{edgeId, graph::Edge::typeId, {{4U, graph::PropertyValue("native edge integrity root")}}});
		if (!transaction.commit().committed()) {
			throw std::runtime_error("test failed to publish native Entity/Property integrity graph");
		}
		MetadataSnapshotPin snapshot = result.metadata->openSnapshot();
		result.node_page = result.metadata->findNodePageLocation(snapshot, nodeId).page_id;
		snapshot.release();
		return result;
	}

	struct NativeGlobalAdjacencyIntegrityGraph {
		std::unique_ptr<RelocatableMetadataStore> metadata;
		uint64_t source_node_id = 0U;
		uint64_t target_node_id = 0U;
		uint64_t edge_id = 0U;
	};

	[[nodiscard]] NativeGlobalAdjacencyIntegrityGraph
	createNativeGlobalAdjacencyIntegrityGraph(const std::shared_ptr<StorageIO> &io) {
		NativeGlobalAdjacencyIntegrityGraph result;
		result.metadata = RelocatableMetadataStore::initializeNew(io, integrityScannerInitialSuperblock());
		EntityPageStore entities(io, *result.metadata);
		SlottedPagePool pages(io, *result.metadata);
		EntityLabelSetStore labels(io, *result.metadata, pages);
		PropertyCollectionRootStore properties(pages, *result.metadata);
		BTreePageStore trees(pages, *result.metadata);

		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 901U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
		NativeGraphWritePlanner planner(*result.metadata, entities, labels, properties, trees, options);

		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		const std::array<uint64_t, 0U> no_labels{};
		result.source_node_id = transaction.createNode(no_labels);
		result.target_node_id = transaction.createNode(no_labels);
		result.edge_id = transaction.createEdge(result.source_node_id, result.target_node_id, 77U);
		if (!transaction.commit().committed()) {
			throw std::runtime_error("test failed to publish native global adjacency state");
		}
		return result;
	}

	struct IntegrityBTreePageImage {
		PageOwnerHeader owner;
		BTreePageHeader header;
		std::vector<BTreePageEntry> entries;
	};

	[[nodiscard]] IntegrityBTreePageImage readIntegrityGlobalAdjacencyRoot(const std::shared_ptr<StorageIO> &io,
																		   RelocatableMetadataStore &metadata) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const std::optional<IndexRootLocation> root =
				metadata.findIndexRootOptional(snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID);
		if (!root.has_value()) {
			throw std::runtime_error("test global adjacency root is absent");
		}
		SharedPagePoolAllocator pages(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
		const SharedPagePoolContents contents = pages.read(snapshot, root->root_page_id);
		const BTreePage page = BTreePage::open(contents.owner, contents.payload);
		IntegrityBTreePageImage result{contents.owner, page.header(), page.entries()};
		snapshot.release();
		return result;
	}

	[[nodiscard]] BTreePage rebuildIntegrityBTreePage(const IntegrityBTreePageImage &image,
													  std::span<const BTreePageEntry> entries,
													  BTreeLeafValueLayout leafValueLayout) {
		BTreePageHeader header = image.header;
		header.entry_count = 0U;
		header.leaf_value_layout = leafValueLayout;
		BTreePage result = BTreePage::create(image.owner, header);
		for (const BTreePageEntry &entry: entries) {
			result.insert(entry);
		}
		return result;
	}

	struct NativeVectorDeltaIntegrityGraph {
		std::unique_ptr<RelocatableMetadataStore> metadata;
		uint64_t node_id = 0U;
		VectorLocation current_raw_location;
		VectorManifest manifest;
		PageOwnerHeader manifest_owner;
		PageOwnerHeader delta_owner;
		BTreePageHeader delta_header;
		std::vector<BTreePageEntry> delta_entries;
	};

	[[nodiscard]] NativeVectorDeltaIntegrityGraph
	createNativeVectorDeltaIntegrityGraph(const std::shared_ptr<StorageIO> &io) {
		NativeVectorDeltaIntegrityGraph result;
		result.metadata = RelocatableMetadataStore::initializeNew(io, integrityScannerInitialSuperblock());
		EntityPageStore entities(io, *result.metadata);
		SlottedPagePool slottedPages(io, *result.metadata);
		EntityLabelSetStore labels(io, *result.metadata, slottedPages);
		PropertyCollectionRootStore properties(slottedPages, *result.metadata);
		BTreePageStore trees(slottedPages, *result.metadata);
		VectorPageStore vectors(io, *result.metadata);
		VectorManifestStore manifests(io, *result.metadata);

		NativeGraphWritePlannerOptions plannerOptions;
		plannerOptions.first_slot = PhysicalSlotId(40U);
		plannerOptions.last_slot = PhysicalSlotId(80U);
		plannerOptions.entity_pool_owner_id = 906U;
		plannerOptions.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 907U};
		NativeGraphWritePlanner planner(*result.metadata, entities, labels, properties, trees, plannerOptions);

		IndexDefinition definition;
		definition.index_id = 77U;
		definition.name = "integrity-vector";
		definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
		definition.kind = IndexDefinitionKind::IDK_VECTOR;
		definition.label_token_id = 7U;
		definition.property_names = {"embedding"};
		definition.vector_dimensions = 2U;
		definition.vector_metric = "l2";

		NativeGraphWriteTransaction bootstrap = planner.beginTransaction();
		const std::array<uint64_t, 1U> nodeLabels{7U};
		result.node_id = bootstrap.createNode(nodeLabels);
		const std::array<NativeGraphVectorBootstrapRecord, 1U> initial{{
				{result.node_id, {std::byte{0x10}, std::byte{0x30}, std::byte{0x50}, std::byte{0x70}}},
		}};
		NativeGraphVectorBootstrapOptions bootstrapOptions;
		bootstrapOptions.first_slot = PhysicalSlotId(40U);
		bootstrapOptions.last_slot = PhysicalSlotId(80U);
		bootstrapOptions.maximum_vectors = 8U;
		bootstrapOptions.maximum_pages = 16U;
		bootstrap.stageVectorIndexBootstrap(trees, vectors, manifests, definition, initial, bootstrapOptions);
		if (!bootstrap.commit().committed()) {
			throw std::runtime_error("test failed to bootstrap native Vector state");
		}

		NativeGraphWriteTransaction delta = planner.beginTransaction();
		const std::array<NativeVectorMutation, 2U> mutations{{
				{NativeVectorMutationKind::NVMK_ERASE,
				 definition.index_id,
				 result.node_id,
				 2U,
				 VectorDistanceMetric::VDM_L2,
				 {}},
				{NativeVectorMutationKind::NVMK_INSERT,
				 definition.index_id,
				 result.node_id,
				 2U,
				 VectorDistanceMetric::VDM_L2,
				 {std::byte{0x21}, std::byte{0x43}, std::byte{0x65}, std::byte{0x87}}},
		}};
		NativeGraphVectorDeltaOptions deltaOptions;
		deltaOptions.first_slot = PhysicalSlotId(40U);
		deltaOptions.last_slot = PhysicalSlotId(80U);
		deltaOptions.maximum_records = 8U;
		deltaOptions.maximum_raw_pages = 8U;
		deltaOptions.rebuild_threshold = 16U;
		deltaOptions.maximum_overlay_entries = 64U;
		deltaOptions.maximum_delta_pages = 8U;
		delta.stageVectorIndexDelta(trees, vectors, manifests, definition, mutations, deltaOptions);
		if (!delta.commit().committed()) {
			throw std::runtime_error("test failed to publish native Vector delta state");
		}

		MetadataSnapshotPin snapshot = result.metadata->openSnapshot();
		result.current_raw_location = result.metadata->findVectorLocation(snapshot, 2U);
		result.manifest = manifests.read(snapshot, definition.index_id).manifest();
		SharedPagePoolAllocator pagePools(io, *result.metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
		const SharedPagePoolContents manifestContents = pagePools.read(snapshot, result.manifest.page_id);
		result.manifest_owner = manifestContents.owner;
		const SharedPagePoolContents deltaContents =
				pagePools.read(snapshot, result.manifest.delta_mapping_root_page_id);
		result.delta_owner = deltaContents.owner;
		const BTreePage deltaPage = BTreePage::open(deltaContents.owner, deltaContents.payload);
		result.delta_header = deltaPage.header();
		result.delta_entries = deltaPage.entries();
		snapshot.release();
		if (result.delta_entries.size() != 2U) {
			throw std::runtime_error("test native Vector delta does not contain erase and insert entries");
		}
		return result;
	}

} // namespace

TEST_F(RelocatableIntegrityScannerTestFixture, ScansNativeEntityMapsLabelSetsAndOwnerKeyedProperties) {
	NativeEntityIntegrityGraph graph = createNativeEntityIntegrityGraph(io);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *graph.metadata).scan()));
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsLiveEdgesWithoutTheReservedGlobalAdjacencyRoot) {
	NativeGlobalAdjacencyIntegrityGraph graph = createNativeGlobalAdjacencyIntegrityGraph(io);
	RelocatableIntegrityScanner scanner(io, *graph.metadata);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));

	const std::array<uint64_t, 1U> erased = {RSS_GLOBAL_ADJACENCY_INDEX_ID};
	SlottedPagePool pages(io, *graph.metadata);
	BTreePageStore trees(pages, *graph.metadata);
	static_cast<void>(trees.publishRoots({}, erased, integrityScannerNextCheckpointLsn(*graph.metadata)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsMissingOrMismatchedGlobalAdjacencyKeys) {
	NativeGlobalAdjacencyIntegrityGraph graph = createNativeGlobalAdjacencyIntegrityGraph(io);
	RelocatableIntegrityScanner scanner(io, *graph.metadata);
	const IntegrityBTreePageImage original = readIntegrityGlobalAdjacencyRoot(io, *graph.metadata);
	ASSERT_EQ(original.header.kind, BTreeNodeKind::BPN_LEAF);
	ASSERT_EQ(original.header.leaf_value_layout, BTreeLeafValueLayout::BLVL_EMPTY);
	ASSERT_EQ(original.entries.size(), 2U);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));

	const std::array<BTreePageEntry, 1U> missingIncoming = {original.entries.front()};
	BTreePage missing = rebuildIntegrityBTreePage(original, missingIncoming, BTreeLeafValueLayout::BLVL_EMPTY);
	replaceIntegrityScannerBTreePage(io, *graph.metadata, original.header.page_id, missing);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);

	BTreePage restored = rebuildIntegrityBTreePage(original, original.entries, BTreeLeafValueLayout::BLVL_EMPTY);
	replaceIntegrityScannerBTreePage(io, *graph.metadata, original.header.page_id, restored);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));

	const std::array<BTreePageEntry, 2U> mismatched = {{
			{AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, graph.source_node_id, graph.edge_id}), 0U},
			{AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_IN, graph.source_node_id, graph.edge_id}), 0U},
	}};
	BTreePage wrongEndpoint = rebuildIntegrityBTreePage(original, mismatched, BTreeLeafValueLayout::BLVL_EMPTY);
	replaceIntegrityScannerBTreePage(io, *graph.metadata, original.header.page_id, wrongEndpoint);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsGlobalAdjacencyLeafWithAValueLayout) {
	NativeGlobalAdjacencyIntegrityGraph graph = createNativeGlobalAdjacencyIntegrityGraph(io);
	RelocatableIntegrityScanner scanner(io, *graph.metadata);
	const IntegrityBTreePageImage original = readIntegrityGlobalAdjacencyRoot(io, *graph.metadata);
	ASSERT_EQ(original.header.kind, BTreeNodeKind::BPN_LEAF);
	ASSERT_EQ(original.entries.size(), 2U);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));

	std::array<BTreePageEntry, 2U> valuedEntries = {original.entries[0], original.entries[1]};
	valuedEntries[0].value_or_child = 11U;
	valuedEntries[1].value_or_child = 13U;
	BTreePage valued = rebuildIntegrityBTreePage(original, valuedEntries, BTreeLeafValueLayout::BLVL_U64);
	replaceIntegrityScannerBTreePage(io, *graph.metadata, original.header.page_id, valued);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, ScansRelocatedAndRetiredCatalogQueueEntries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, integrityScannerInitialSuperblock());
	const std::array requests = {
			CatalogSlotReservationRequest{.segment_id = SegmentId(31U),
										  .kind = SegmentKind::SGK_CODEBOOK_EXTENT,
										  .owner_id = 701U,
										  .segment_generation = Generation(1U),
										  .state = SegmentState::SGS_SEALED,
										  .require_absent = true},
			CatalogSlotReservationRequest{.segment_id = SegmentId(32U),
										  .kind = SegmentKind::SGK_CODEBOOK_EXTENT,
										  .owner_id = 702U,
										  .segment_generation = Generation(1U),
										  .state = SegmentState::SGS_SEALED,
										  .require_absent = true},
	};
	ASSERT_TRUE(metadata->reserveFirstFreeAndUpsertCatalog(requests, PhysicalSlotId(40U), PhysicalSlotId(41U),
														   LogSequenceNumber(2U))
						.has_value());
	ASSERT_TRUE(metadata->relocateCatalogSegment(requests[0].segment_id, requests[0].segment_generation,
												 PhysicalSlotId(42U), PhysicalSlotId(42U), LogSequenceNumber(3U),
												 [](const CatalogEntry &, const CatalogEntry &) {})
						.has_value());
	static_cast<void>(metadata->retireCatalogSegment(requests[1].segment_id, requests[1].segment_generation,
													 LogSequenceNumber(4U)));

	const RelocatableIntegrityReport report = RelocatableIntegrityScanner(io, *metadata).scan();
	EXPECT_EQ(report.queued_retired_slots, 2U);
	EXPECT_EQ(report.catalog_entries, requests.size() + 1U);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsStaleNativeEntityPageFreeSpaceMetadata) {
	NativeEntityIntegrityGraph graph = createNativeEntityIntegrityGraph(io);
	MetadataSnapshotPin snapshot = graph.metadata->openSnapshot();
	PageLocation location = graph.metadata->findPageLocation(snapshot, graph.node_page);
	++location.free_bytes;
	snapshot.release();
	static_cast<void>(graph.metadata->upsertLocations(std::span(&location, 1U), {},
													  integrityScannerNextCheckpointLsn(*graph.metadata)));

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *graph.metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsEntityPageWithNonEntityOwnerKind) {
	NativeEntityIntegrityGraph graph = createNativeEntityIntegrityGraph(io);
	MetadataSnapshotPin snapshot = graph.metadata->openSnapshot();
	SharedPagePoolAllocator pages(io, *graph.metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
	const SharedPagePoolContents contents = pages.read(snapshot, graph.node_page);
	snapshot.release();
	PageOwnerHeader wrongKind = contents.owner;
	wrongKind.kind = PageOwnerKind::POK_STATE;
	overwriteIntegrityScannerPageOwner(io, *graph.metadata, wrongKind);

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *graph.metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsStaleOrForeignLabelSetPageMetadata) {
	NativeEntityIntegrityGraph graph = createNativeEntityIntegrityGraph(io);
	MetadataSnapshotPin snapshot = graph.metadata->openSnapshot();
	const EntityPageLocation nodeLocation = graph.metadata->findNodePageLocation(snapshot, 1001U);
	SharedPagePoolAllocator pages(io, *graph.metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
	const SharedPagePoolContents nodeContents = pages.read(snapshot, nodeLocation.page_id);
	const EntityPage nodePage = EntityPage::open(nodeContents.owner, nodeContents.payload);
	const NodeEntityRecord storedNode = *nodePage.findNode(nodeLocation.slot_id, nodeLocation.slot_generation);
	ASSERT_TRUE(storedNode.label_overflow.isValid());
	PageLocation labelLocation = graph.metadata->findPageLocation(snapshot, storedNode.label_overflow.page_id);
	const SharedPagePoolContents labelContents = pages.read(snapshot, storedNode.label_overflow.page_id);
	++labelLocation.free_bytes;
	snapshot.release();
	static_cast<void>(graph.metadata->upsertLocations(std::span(&labelLocation, 1U), {},
													  integrityScannerNextCheckpointLsn(*graph.metadata)));

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *graph.metadata).scan()), FormatException);
	--labelLocation.free_bytes;
	static_cast<void>(graph.metadata->upsertLocations(std::span(&labelLocation, 1U), {},
													  integrityScannerNextCheckpointLsn(*graph.metadata)));
	PageOwnerHeader foreignOwner = labelContents.owner;
	++foreignOwner.owner_id;
	overwriteIntegrityScannerPageOwner(io, *graph.metadata, foreignOwner);
	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *graph.metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsShortEntityPropertyPayload) {
	NativeEntityIntegrityGraph graph = createNativeEntityIntegrityGraph(io);
	MetadataSnapshotPin snapshot = graph.metadata->openSnapshot();
	const EntityPropertyLocation property = graph.metadata->findEntityPropertyLocation(snapshot, graph.property_key);
	SharedPagePoolAllocator pages(io, *graph.metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
	const SharedPagePoolContents contents = pages.read(snapshot, property.page_id);
	PageLocation location = graph.metadata->findPageLocation(snapshot, property.page_id);
	snapshot.release();
	SlottedPage page = SlottedPage::open(contents.owner, contents.payload);
	const std::array<std::byte, 1U> malformed{std::byte{0x01}};
	page.overwrite({property.page_id, property.slot_id, property.slot_generation}, malformed);
	overwriteIntegrityScannerPagePayload(io, *graph.metadata, property.page_id, page.serialize());
	location.free_bytes = page.stats().contiguous_free_bytes;
	static_cast<void>(graph.metadata->upsertLocations(std::span(&location, 1U), {},
													  integrityScannerNextCheckpointLsn(*graph.metadata)));

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *graph.metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsInconsistentNativeVectorDeltaSemantics) {
	NativeVectorDeltaIntegrityGraph graph = createNativeVectorDeltaIntegrityGraph(io);
	RelocatableIntegrityScanner scanner(io, *graph.metadata);
	const auto replaceDelta = [&](std::span<const BTreePageEntry> entries) {
		BTreePageHeader replacementHeader = graph.delta_header;
		replacementHeader.entry_count = 0U;
		BTreePage replacement = BTreePage::create(graph.delta_owner, replacementHeader);
		for (const BTreePageEntry &entry: entries) {
			replacement.insert(entry);
		}
		replaceIntegrityScannerBTreePage(io, *graph.metadata, graph.delta_header.page_id, replacement);
	};
	const auto replaceManifest = [&](const VectorManifest &manifest) {
		overwriteIntegrityScannerPagePayload(io, *graph.metadata, graph.manifest.page_id,
											 VectorManifestPage::create(graph.manifest_owner, manifest).serialize());
	};
	const auto findOperation = [](std::vector<BTreePageEntry> &entries, VectorDeltaOperation operation) {
		return std::ranges::find_if(entries, [operation](const BTreePageEntry &entry) {
			return VectorDeltaMapping::decodeEntry(entry).operation == operation;
		});
	};

	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));

	VectorManifest wrongDeltaRootPage = graph.manifest;
	wrongDeltaRootPage.delta_mapping_root_page_id = PageId(graph.manifest.delta_mapping_root_page_id.value() + 1U);
	replaceManifest(wrongDeltaRootPage);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceManifest(graph.manifest);

	VectorManifest wrongDeltaRootGeneration = graph.manifest;
	wrongDeltaRootGeneration.delta_mapping_root_generation =
			Generation(graph.manifest.delta_mapping_root_generation.value() + 1U);
	replaceManifest(wrongDeltaRootGeneration);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceManifest(graph.manifest);

	VectorManifest wrongNodeMappingRootPage = graph.manifest;
	wrongNodeMappingRootPage.node_mapping_root_page_id = PageId(graph.manifest.node_mapping_root_page_id.value() + 1U);
	replaceManifest(wrongNodeMappingRootPage);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceManifest(graph.manifest);

	VectorManifest wrongNodeMappingRootGeneration = graph.manifest;
	wrongNodeMappingRootGeneration.node_mapping_root_generation =
			Generation(graph.manifest.node_mapping_root_generation.value() + 1U);
	replaceManifest(wrongNodeMappingRootGeneration);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceManifest(graph.manifest);

	VectorManifest incompleteDelta = graph.manifest;
	++incompleteDelta.delta_entry_count;
	replaceManifest(incompleteDelta);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceManifest(graph.manifest);

	MetadataSnapshotPin deltaRootSnapshot = graph.metadata->openSnapshot();
	const IndexRootLocation deltaRoot =
			graph.metadata->findIndexRoot(deltaRootSnapshot, graph.manifest.delta_mapping_index_id);
	deltaRootSnapshot.release();
	const std::array<uint64_t, 1U> erasedDeltaRoot{graph.manifest.delta_mapping_index_id};
	SlottedPagePool indexPages(io, *graph.metadata);
	BTreePageStore indexTrees(indexPages, *graph.metadata);
	static_cast<void>(indexTrees.publishRoots({}, erasedDeltaRoot, integrityScannerNextCheckpointLsn(*graph.metadata)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	static_cast<void>(
			indexTrees.publishRoots(std::span(&deltaRoot, 1U), {}, integrityScannerNextCheckpointLsn(*graph.metadata)));
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));

	const std::array<uint64_t, 1U> erasedCurrentRaw{graph.current_raw_location.vector_id};
	static_cast<void>(graph.metadata->upsertVectorLocations({}, erasedCurrentRaw,
															integrityScannerNextCheckpointLsn(*graph.metadata)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	static_cast<void>(graph.metadata->upsertVectorLocations(std::span(&graph.current_raw_location, 1U), {},
															integrityScannerNextCheckpointLsn(*graph.metadata)));
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));

	std::vector<BTreePageEntry> eraseStillCurrent = graph.delta_entries;
	auto eraseEntry = findOperation(eraseStillCurrent, VectorDeltaOperation::VDO_ERASE);
	ASSERT_NE(eraseEntry, eraseStillCurrent.end());
	eraseEntry->key = VectorDeltaMapping::key(graph.current_raw_location.vector_id, VectorDeltaOperation::VDO_ERASE);
	replaceDelta(eraseStillCurrent);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceDelta(graph.delta_entries);

	std::vector<BTreePageEntry> mismatchedInsertOwner = graph.delta_entries;
	auto insertEntry = findOperation(mismatchedInsertOwner, VectorDeltaOperation::VDO_INSERT);
	ASSERT_NE(insertEntry, mismatchedInsertOwner.end());
	insertEntry->value_or_child = graph.node_id + 1U;
	replaceDelta(mismatchedInsertOwner);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceDelta(graph.delta_entries);

	std::vector<BTreePageEntry> eraseWithoutReplacementDelta = graph.delta_entries;
	const auto replacementInsert = findOperation(eraseWithoutReplacementDelta, VectorDeltaOperation::VDO_INSERT);
	ASSERT_NE(replacementInsert, eraseWithoutReplacementDelta.end());
	eraseWithoutReplacementDelta.erase(replacementInsert);
	VectorManifest oneEntryManifest = graph.manifest;
	oneEntryManifest.delta_entry_count = 1U;
	replaceDelta(eraseWithoutReplacementDelta);
	replaceManifest(oneEntryManifest);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceDelta(graph.delta_entries);
	replaceManifest(graph.manifest);

	VectorManifest missingSemanticRoots = graph.manifest;
	missingSemanticRoots.node_mapping_root_page_id = {};
	missingSemanticRoots.node_mapping_root_generation = {};
	missingSemanticRoots.vector_node_mapping_index_id = 0U;
	missingSemanticRoots.vector_node_mapping_root_page_id = {};
	missingSemanticRoots.vector_node_mapping_root_generation = {};
	replaceManifest(missingSemanticRoots);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceManifest(graph.manifest);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsNativeEntityAndLabelSetOrphans) {
	NativeEntityIntegrityGraph graph = createNativeEntityIntegrityGraph(io);
	MetadataSnapshotPin snapshot = graph.metadata->openSnapshot();
	const EntityPageLocation location = graph.metadata->findNodePageLocation(snapshot, 1001U);
	SharedPagePoolAllocator pages(io, *graph.metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
	const SharedPagePoolContents contents = pages.read(snapshot, graph.node_page);
	EntityPage page = EntityPage::open(contents.owner, contents.payload);
	NodeEntityRecord detached = *page.findNode(location.slot_id, location.slot_generation);
	detached.inline_label_ids.fill(0U);
	detached.inline_label_count = 0U;
	detached.label_overflow = {};
	page.replace(location.slot_id, location.slot_generation, detached);
	snapshot.release();
	overwriteIntegrityScannerPagePayload(io, *graph.metadata, graph.node_page, page.serialize());

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *graph.metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsOwnerKeyedPropertyRootWithoutDirectoryEntry) {
	NativeEntityIntegrityGraph graph = createNativeEntityIntegrityGraph(io);
	const std::array<EntityPropertyKey, 1U> erased{graph.property_key};
	static_cast<void>(graph.metadata->upsertEntityPropertyLocations(
			{}, erased, integrityScannerNextCheckpointLsn(*graph.metadata)));

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *graph.metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsNativeEntityMapRelationshipCorruption) {
	NativeEntityIntegrityGraph graph = createNativeEntityIntegrityGraph(io);
	RelocatableIntegrityScanner scanner(io, *graph.metadata);
	MetadataSnapshotPin snapshot = graph.metadata->openSnapshot();
	const EntityPageLocation node = graph.metadata->findNodePageLocation(snapshot, 1001U);
	const EntityPageLocation edge = graph.metadata->findEdgePageLocation(snapshot, 2001U);
	const EntityPropertyLocation property = graph.metadata->findEntityPropertyLocation(snapshot, graph.property_key);
	snapshot.release();
	const auto publish = [&](std::span<const EntityPageLocation> nodes, std::span<const uint64_t> erasedNodes,
							 std::span<const EntityPageLocation> edges, std::span<const uint64_t> erasedEdges) {
		static_cast<void>(graph.metadata->upsertEntityPageLocations(
				nodes, erasedNodes, edges, erasedEdges, integrityScannerNextCheckpointLsn(*graph.metadata)));
	};

	const std::array<uint64_t, 1U> erasedNode{node.entity_id};
	publish({}, erasedNode, {}, {});
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	publish(std::span(&node, 1U), {}, {}, {});

	EntityPageLocation wrongKind = node;
	wrongKind.page_id = edge.page_id;
	wrongKind.slot_id = edge.slot_id;
	wrongKind.slot_generation = edge.slot_generation;
	publish(std::span(&wrongKind, 1U), {}, {}, {});
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	publish(std::span(&node, 1U), {}, {}, {});

	EntityPageLocation staleGeneration = node;
	++staleGeneration.slot_generation;
	publish(std::span(&staleGeneration, 1U), {}, {}, {});
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	publish(std::span(&node, 1U), {}, {}, {});

	EntityPageLocation duplicate = node;
	duplicate.entity_id = node.entity_id + 1U;
	publish(std::span(&duplicate, 1U), {}, {}, {});
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	const std::array<uint64_t, 1U> erasedDuplicate{duplicate.entity_id};
	publish({}, erasedDuplicate, {}, {});

	EntityPageLocation foreignEdgePage = edge;
	foreignEdgePage.page_id = property.page_id;
	foreignEdgePage.slot_id = property.slot_id;
	foreignEdgePage.slot_generation = property.slot_generation;
	MetadataSnapshotPin edgeRootSnapshot = graph.metadata->openSnapshot();
	const PhysicalPageRef edgeDirectoryRoot = edgeRootSnapshot.superblock().edge_entity_directory_root;
	edgeRootSnapshot.release();
	IntegrityLocationLeafImage edgeLeaf =
			readIntegrityLocationLeaf(io, *graph.metadata, edgeDirectoryRoot, edge.entity_id);
	const IntegrityLocationLeafImage originalEdgeLeaf = edgeLeaf;
	LocationLeafEntry &edgeEntry = edgeLeaf.leaf.entries[edgeLeaf.entry_index];
	edgeEntry.segment_id = SegmentId(foreignEdgePage.page_id.value());
	edgeEntry.generation = Generation(foreignEdgePage.slot_generation);
	edgeEntry.first_page = foreignEdgePage.slot_id;
	overwriteIntegrityLocationLeaf(io, edgeLeaf);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwriteIntegrityLocationLeaf(io, originalEdgeLeaf);

	const std::array<uint64_t, 1U> erasedEdge{edge.entity_id};
	publish({}, {}, {}, erasedEdge);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsEntityPropertyDirectoryRelationshipCorruption) {
	NativeEntityIntegrityGraph graph = createNativeEntityIntegrityGraph(io);
	RelocatableIntegrityScanner scanner(io, *graph.metadata);
	MetadataSnapshotPin snapshot = graph.metadata->openSnapshot();
	const EntityPropertyLocation original = graph.metadata->findEntityPropertyLocation(snapshot, graph.property_key);
	snapshot.release();

	EntityPropertyLocation stale = original;
	++stale.slot_generation;
	static_cast<void>(graph.metadata->upsertEntityPropertyLocations(
			std::span(&stale, 1U), {}, integrityScannerNextCheckpointLsn(*graph.metadata)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	static_cast<void>(graph.metadata->upsertEntityPropertyLocations(
			std::span(&original, 1U), {}, integrityScannerNextCheckpointLsn(*graph.metadata)));

	EntityPropertyLocation foreignKey = original;
	foreignKey.key.entity_id = original.key.entity_id + 1U;
	static_cast<void>(graph.metadata->upsertEntityPropertyLocations(
			std::span(&foreignKey, 1U), {}, integrityScannerNextCheckpointLsn(*graph.metadata)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsAdjacencyPageWithForeignOwnerIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, integrityScannerInitialSuperblock());
	const PageOwnerHeader owner{PageId(820U),
								VectorAdjacencyPageStore::derivedOwnerId(17U),
								PageOwnerKind::POK_VECTOR_ADJACENCY,
								0U,
								RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								Generation(1U),
								LogSequenceNumber(2U)};
	SlottedPage page = SlottedPage::create(owner);
	static_cast<void>(page.insert(VectorAdjacencyCodec::encode({701U, {}})));
	SharedPagePoolAllocationRequest request;
	request.segment_id = SegmentId(85U);
	request.pool_kind = SegmentKind::SGK_VECTOR_ADJACENCY_POOL;
	request.pool_owner_id = 17U;
	request.generation = owner.generation;
	request.page_id = owner.page_id;
	request.page_owner_id = owner.owner_id;
	request.page_owner_kind = owner.kind;
	request.page_free_bytes = page.stats().contiguous_free_bytes;
	const std::vector<std::byte> payload = page.serialize();
	request.payload = payload;
	SharedPagePoolAllocator pages(io, *metadata);
	ASSERT_TRUE(pages.allocateNewPoolPage(request, PhysicalSlotId(40U), PhysicalSlotId(40U), owner.committed_lsn)
						.has_value());
	RelocatableIntegrityScanner scanner(io, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	PageLocation location = metadata->findPageLocation(snapshot, owner.page_id);
	snapshot.release();
	const PageLocation originalLocation = location;
	++location.free_bytes;
	static_cast<void>(
			metadata->upsertLocations(std::span(&location, 1U), {}, integrityScannerNextCheckpointLsn(*metadata)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	static_cast<void>(metadata->upsertLocations(std::span(&originalLocation, 1U), {},
												integrityScannerNextCheckpointLsn(*metadata)));

	PageOwnerHeader foreign = owner;
	foreign.owner_id = 18U;
	const auto bytes = PageOwnerHeaderCodec::encode(foreign);
	io->writeAt(integrityScannerPagePayloadOffset(*metadata, owner.page_id) - RSS_PAGE_OWNER_HEADER_BYTES, bytes.data(),
				bytes.size());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsStateReferenceToTextLargeValue) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, integrityScannerInitialSuperblock());
	LargeValueStore values(io, *metadata);
	LargeValueWriteOptions writeOptions;
	writeOptions.first_data_slot = PhysicalSlotId(40U);
	writeOptions.last_data_slot = PhysicalSlotId(80U);
	writeOptions.first_manifest_slot = PhysicalSlotId(81U);
	writeOptions.last_manifest_slot = PhysicalSlotId(100U);
	writeOptions.kind = LargeValueKind::LVK_UTF8_TEXT;
	writeOptions.media_type = RSS_STATE_LARGE_VALUE_MEDIA_TYPE;
	const std::array payload{std::byte{'s'}, std::byte{'t'}, std::byte{'a'}, std::byte{'t'}, std::byte{'e'}};
	const LargeValueWriteResult written = values.write(payload, writeOptions, LogSequenceNumber(2U));
	SlottedPagePool pages(io, *metadata);
	StateRecordStore states(pages, *metadata);
	static_cast<void>(
			states.ensure({17U, "system.text", StateRecordStorageKind::SRS_LARGE_VALUE, written.reference.value_id, {}},
						  {PhysicalSlotId(101U), PhysicalSlotId(101U), 808U}));

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsDerivedAdjacencyTreeAndOwnershipCorruption) {
	constexpr uint64_t INDEX_ID = 91U;
	constexpr uint64_t MAPPING_INDEX_ID = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | INDEX_ID;
	auto metadata = RelocatableMetadataStore::initializeNew(io, integrityScannerInitialSuperblock());

	VectorPageHeader rawHeader;
	rawHeader.index_id = INDEX_ID;
	rawHeader.page_id = PageId(901U);
	rawHeader.generation = Generation(1U);
	rawHeader.kind = VectorStorageKind::VSK_RAW;
	rawHeader.record_bytes = sizeof(uint64_t) + sizeof(uint16_t);
	rawHeader.slot_capacity = 2U;
	rawHeader.bitmap_bytes = 1U;
	rawHeader.committed_lsn = LogSequenceNumber(2U);
	const PageOwnerHeader rawOwner{rawHeader.page_id,
								   INDEX_ID,
								   PageOwnerKind::POK_VECTOR_RAW,
								   0U,
								   RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								   rawHeader.generation,
								   rawHeader.committed_lsn};
	VectorPage rawPage = VectorPage::create(rawOwner, rawHeader);
	std::array<std::byte, sizeof(uint64_t) + sizeof(uint16_t)> rawRecord{};
	LittleEndianCodec::writeU64(rawRecord, 0U, 701U);
	const uint16_t firstRawSlot = rawPage.insert(rawRecord);
	LittleEndianCodec::writeU64(rawRecord, 0U, 702U);
	const uint16_t secondRawSlot = rawPage.insert(rawRecord);
	VectorPageStore vectors(io, *metadata);
	SharedPagePoolAllocationRequest rawRequest;
	rawRequest.segment_id = SegmentId(91U);
	rawRequest.pool_kind = SegmentKind::SGK_VECTOR_RAW_POOL;
	rawRequest.pool_owner_id = INDEX_ID;
	rawRequest.generation = rawHeader.generation;
	rawRequest.page_id = rawHeader.page_id;
	rawRequest.page_owner_id = INDEX_ID;
	rawRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_RAW;
	ASSERT_TRUE(vectors.allocate(rawRequest, rawPage, PhysicalSlotId(40U), PhysicalSlotId(40U), rawHeader.committed_lsn)
						.has_value());
	const std::array rawLocations = {
			VectorLocation{701U, rawHeader.page_id, firstRawSlot, rawPage.slotGeneration(firstRawSlot)},
			VectorLocation{702U, rawHeader.page_id, secondRawSlot, rawPage.slotGeneration(secondRawSlot)},
	};
	static_cast<void>(metadata->upsertVectorLocations(rawLocations, {}, LogSequenceNumber(3U)));

	const PageOwnerHeader adjacencyOwner{PageId(902U),
										 VectorAdjacencyPageStore::derivedOwnerId(INDEX_ID),
										 PageOwnerKind::POK_VECTOR_ADJACENCY,
										 0U,
										 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										 Generation(1U),
										 LogSequenceNumber(4U)};
	SlottedPage adjacency = SlottedPage::create(adjacencyOwner);
	static_cast<void>(adjacency.insert(VectorAdjacencyCodec::encode({701U, {702U}})));
	static_cast<void>(adjacency.insert(VectorAdjacencyCodec::encode({702U, {}})));
	SharedPagePoolAllocationRequest adjacencyRequest;
	adjacencyRequest.segment_id = SegmentId(92U);
	adjacencyRequest.pool_kind = SegmentKind::SGK_VECTOR_ADJACENCY_POOL;
	adjacencyRequest.pool_owner_id = INDEX_ID;
	adjacencyRequest.generation = adjacencyOwner.generation;
	adjacencyRequest.page_id = adjacencyOwner.page_id;
	adjacencyRequest.page_owner_id = adjacencyOwner.owner_id;
	adjacencyRequest.page_owner_kind = adjacencyOwner.kind;
	adjacencyRequest.page_free_bytes = adjacency.stats().contiguous_free_bytes;
	const std::vector<std::byte> adjacencyPayload = adjacency.serialize();
	adjacencyRequest.payload = adjacencyPayload;
	SharedPagePoolAllocator sharedPages(io, *metadata);
	ASSERT_TRUE(sharedPages
						.allocateNewPoolPage(adjacencyRequest, PhysicalSlotId(41U), PhysicalSlotId(41U),
											 adjacencyOwner.committed_lsn)
						.has_value());

	SlottedPagePool indexPages(io, *metadata);
	BTreePageStore trees(indexPages, *metadata);
	const auto allocateIndexPage = [&](SegmentId segmentId, PageId pageId, PhysicalSlotId slot,
									   const PageOwnerHeader &owner, const BTreePage &page) {
		SlottedPagePoolAllocationRequest request;
		request.segment_id = segmentId;
		request.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		request.pool_owner_id = MAPPING_INDEX_ID;
		request.generation = Generation(1U);
		request.page_id = pageId;
		request.page_owner_id = MAPPING_INDEX_ID;
		request.page_owner_kind = PageOwnerKind::POK_INDEX;
		ASSERT_TRUE(trees.allocate(request, page, slot, slot, owner.committed_lsn).has_value());
	};
	const PageOwnerHeader leftOwner{
			PageId(903U),	MAPPING_INDEX_ID,	  PageOwnerKind::POK_INDEX, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
			Generation(1U), LogSequenceNumber(5U)};
	BTreePageHeader leftHeader;
	leftHeader.index_id = MAPPING_INDEX_ID;
	leftHeader.page_id = leftOwner.page_id;
	leftHeader.generation = Generation(1U);
	leftHeader.right_sibling = PageId(904U);
	BTreePage left = BTreePage::create(leftOwner, leftHeader);
	left.insert({VectorAdjacencyMapping::key(701U), adjacencyOwner.page_id.value()});
	allocateIndexPage(SegmentId(93U), leftOwner.page_id, PhysicalSlotId(42U), leftOwner, left);
	const PageOwnerHeader rightOwner{
			PageId(904U),	MAPPING_INDEX_ID,	  PageOwnerKind::POK_INDEX, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
			Generation(1U), LogSequenceNumber(6U)};
	BTreePageHeader rightHeader;
	rightHeader.index_id = MAPPING_INDEX_ID;
	rightHeader.page_id = rightOwner.page_id;
	rightHeader.generation = Generation(1U);
	rightHeader.left_sibling = leftOwner.page_id;
	BTreePage right = BTreePage::create(rightOwner, rightHeader);
	right.insert({VectorAdjacencyMapping::key(702U), adjacencyOwner.page_id.value()});
	allocateIndexPage(SegmentId(94U), rightOwner.page_id, PhysicalSlotId(43U), rightOwner, right);
	const PageOwnerHeader rootOwner{
			PageId(905U),	MAPPING_INDEX_ID,	  PageOwnerKind::POK_INDEX, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
			Generation(1U), LogSequenceNumber(7U)};
	BTreePageHeader rootHeader;
	rootHeader.index_id = MAPPING_INDEX_ID;
	rootHeader.page_id = rootOwner.page_id;
	rootHeader.generation = Generation(1U);
	rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
	rootHeader.level = 1U;
	rootHeader.leftmost_child = leftOwner.page_id;
	BTreePage root = BTreePage::create(rootOwner, rootHeader);
	root.insert({VectorAdjacencyMapping::key(702U), rightOwner.page_id.value()});
	allocateIndexPage(SegmentId(95U), rootOwner.page_id, PhysicalSlotId(44U), rootOwner, root);
	const IndexRootLocation mappingRoot{MAPPING_INDEX_ID, rootOwner.page_id, Generation(1U)};
	static_cast<void>(trees.publishRoots(std::span(&mappingRoot, 1U), {}, LogSequenceNumber(8U)));

	VectorManifest manifest;
	manifest.index_id = INDEX_ID;
	manifest.page_id = PageId(906U);
	manifest.generation = Generation(1U);
	manifest.committed_lsn = LogSequenceNumber(9U);
	manifest.dimensions = 1U;
	manifest.raw_write_tail_page_id = rawHeader.page_id;
	manifest.entry_point_vector_id = 701U;
	manifest.adjacency_generation = Generation(1U);
	manifest.adjacency_mapping_index_id = MAPPING_INDEX_ID;
	manifest.adjacency_mapping_root_page_id = rootOwner.page_id;
	manifest.adjacency_mapping_root_generation = mappingRoot.root_generation;
	manifest.entries = {{rawHeader.page_id, VectorStorageKind::VSK_RAW, 2U}};
	const PageOwnerHeader manifestOwner{manifest.page_id,
										INDEX_ID,
										PageOwnerKind::POK_VECTOR_MANIFEST,
										0U,
										RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										manifest.generation,
										manifest.committed_lsn};
	SharedPagePoolAllocationRequest manifestRequest;
	manifestRequest.segment_id = SegmentId(96U);
	manifestRequest.pool_kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
	manifestRequest.pool_owner_id = INDEX_ID;
	manifestRequest.generation = manifest.generation;
	manifestRequest.page_id = manifest.page_id;
	manifestRequest.page_owner_id = INDEX_ID;
	manifestRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
	VectorManifestStore manifests(io, *metadata);
	ASSERT_TRUE(manifests
						.allocate(manifestRequest, VectorManifestPage::create(manifestOwner, manifest),
								  PhysicalSlotId(45U), PhysicalSlotId(45U), manifest.committed_lsn)
						.has_value());

	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));
	const auto replaceManifest = [&](const VectorManifest &replacement) {
		overwriteIntegrityScannerPagePayload(io, *metadata, manifest.page_id,
											 VectorManifestPage::create(manifestOwner, replacement).serialize());
	};

	VectorManifest missingRoot = manifest;
	missingRoot.adjacency_mapping_index_id = MAPPING_INDEX_ID + 1U;
	replaceManifest(missingRoot);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceManifest(manifest);
	VectorManifest wrongRootPage = manifest;
	wrongRootPage.adjacency_mapping_root_page_id = leftOwner.page_id;
	replaceManifest(wrongRootPage);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceManifest(manifest);

	BTreePage malformedRight = BTreePage::create(rightOwner, rightHeader);
	malformedRight.insert({VectorAdjacencyMapping::key(701U), adjacencyOwner.page_id.value()});
	replaceIntegrityScannerBTreePage(io, *metadata, rightOwner.page_id, malformedRight);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceIntegrityScannerBTreePage(io, *metadata, rightOwner.page_id, right);

	BTreePageHeader brokenLeftHeader = leftHeader;
	brokenLeftHeader.right_sibling = PageId{};
	BTreePage brokenLeft = BTreePage::create(leftOwner, brokenLeftHeader);
	brokenLeft.insert({VectorAdjacencyMapping::key(701U), adjacencyOwner.page_id.value()});
	replaceIntegrityScannerBTreePage(io, *metadata, leftOwner.page_id, brokenLeft);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceIntegrityScannerBTreePage(io, *metadata, leftOwner.page_id, left);
	BTreePageHeader brokenRightHeader = rightHeader;
	brokenRightHeader.left_sibling = PageId{};
	BTreePage brokenRight = BTreePage::create(rightOwner, brokenRightHeader);
	brokenRight.insert({VectorAdjacencyMapping::key(702U), adjacencyOwner.page_id.value()});
	replaceIntegrityScannerBTreePage(io, *metadata, rightOwner.page_id, brokenRight);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceIntegrityScannerBTreePage(io, *metadata, rightOwner.page_id, right);

	const std::array<uint64_t, 2U> erasedRawVectorIds = {701U, 702U};
	static_cast<void>(
			metadata->upsertVectorLocations({}, erasedRawVectorIds, integrityScannerNextCheckpointLsn(*metadata)));
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	static_cast<void>(metadata->upsertVectorLocations(rawLocations, {}, integrityScannerNextCheckpointLsn(*metadata)));

	BTreePage wrongPoolLeft = BTreePage::create(leftOwner, leftHeader);
	wrongPoolLeft.insert({VectorAdjacencyMapping::key(701U), rawHeader.page_id.value()});
	replaceIntegrityScannerBTreePage(io, *metadata, leftOwner.page_id, wrongPoolLeft);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceIntegrityScannerBTreePage(io, *metadata, leftOwner.page_id, left);

	const LogSequenceNumber foreignAdjacencyLsn = integrityScannerNextCheckpointLsn(*metadata);
	const PageOwnerHeader foreignAdjacencyOwner{PageId(907U),
												VectorAdjacencyPageStore::derivedOwnerId(INDEX_ID + 1U),
												PageOwnerKind::POK_VECTOR_ADJACENCY,
												0U,
												RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
												Generation(1U),
												foreignAdjacencyLsn};
	SlottedPage foreignAdjacency = SlottedPage::create(foreignAdjacencyOwner);
	static_cast<void>(foreignAdjacency.insert(VectorAdjacencyCodec::encode({701U, {702U}})));
	SharedPagePoolAllocationRequest foreignAdjacencyRequest;
	foreignAdjacencyRequest.segment_id = SegmentId(97U);
	foreignAdjacencyRequest.pool_kind = SegmentKind::SGK_VECTOR_ADJACENCY_POOL;
	foreignAdjacencyRequest.pool_owner_id = INDEX_ID + 1U;
	foreignAdjacencyRequest.generation = foreignAdjacencyOwner.generation;
	foreignAdjacencyRequest.page_id = foreignAdjacencyOwner.page_id;
	foreignAdjacencyRequest.page_owner_id = foreignAdjacencyOwner.owner_id;
	foreignAdjacencyRequest.page_owner_kind = foreignAdjacencyOwner.kind;
	foreignAdjacencyRequest.page_free_bytes = foreignAdjacency.stats().contiguous_free_bytes;
	foreignAdjacencyRequest.payload = foreignAdjacency.serialize();
	ASSERT_TRUE(sharedPages
						.allocateNewPoolPage(foreignAdjacencyRequest, PhysicalSlotId(46U), PhysicalSlotId(46U),
											 foreignAdjacencyLsn)
						.has_value());
	BTreePage foreignOwnerLeft = BTreePage::create(leftOwner, leftHeader);
	foreignOwnerLeft.insert({VectorAdjacencyMapping::key(701U), foreignAdjacencyOwner.page_id.value()});
	replaceIntegrityScannerBTreePage(io, *metadata, leftOwner.page_id, foreignOwnerLeft);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceIntegrityScannerBTreePage(io, *metadata, leftOwner.page_id, left);

	PageOwnerHeader legacyOwner = adjacencyOwner;
	legacyOwner.owner_id = INDEX_ID;
	const auto legacyOwnerBytes = PageOwnerHeaderCodec::encode(legacyOwner);
	io->writeAt(integrityScannerPagePayloadOffset(*metadata, adjacencyOwner.page_id) - RSS_PAGE_OWNER_HEADER_BYTES,
				legacyOwnerBytes.data(), legacyOwnerBytes.size());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	const auto derivedOwnerBytes = PageOwnerHeaderCodec::encode(adjacencyOwner);
	io->writeAt(integrityScannerPagePayloadOffset(*metadata, adjacencyOwner.page_id) - RSS_PAGE_OWNER_HEADER_BYTES,
				derivedOwnerBytes.data(), derivedOwnerBytes.size());
}


TEST_F(RelocatableIntegrityScannerTestFixture, RejectsCodebookManifestHeaderGenerationMismatch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, integrityScannerInitialSuperblock());
	PhysicalSegmentAllocator physical(io, *metadata);
	VectorCodebookStore codebooks(physical, *metadata);
	VectorCodebookWriteRequest request;
	request.codebook_id = 72U;
	request.manifest_segment_id = SegmentId(111U);
	request.generation = Generation(1U);
	request.element_type = VectorCodebookElementType::VCET_FLOAT32;
	request.dimensions = 2U;
	request.subspace_count = 1U;
	request.centroid_count = 3U;
	const std::vector<std::byte> payload(24U, std::byte{0x5A});
	static_cast<void>(codebooks.writeImmutable(request, payload, PhysicalSlotId(40U), PhysicalSlotId(40U),
											   LogSequenceNumber(2U)));

	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PhysicalSegmentContents contents = physical.read(snapshot, request.manifest_segment_id);
	const VectorCodebookExtentHeader original = VectorCodebookExtentHeaderCodec::decode(contents.header_extension);
	VectorCodebookExtentHeader wrongGeneration = original;
	wrongGeneration.codebook_generation = Generation(2U);
	const VectorCodebookExtentHeaderCodec::Bytes wrongGenerationBytes =
			VectorCodebookExtentHeaderCodec::encode(wrongGeneration);
	io->writeAt(integrityScannerExtentHeaderOffset(*metadata, request.manifest_segment_id), wrongGenerationBytes.data(),
				wrongGenerationBytes.size());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsOrphanedCodebookDataExtent) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, integrityScannerInitialSuperblock());
	PhysicalSegmentAllocator physical(io, *metadata);
	VectorCodebookStore codebooks(physical, *metadata);
	constexpr uint64_t CODEBOOK_ID = 71U;
	constexpr SegmentId MANIFEST_SEGMENT(101U);
	const size_t payloadBytes = static_cast<size_t>(RSS_VECTOR_CODEBOOK_INLINE_PAYLOAD_BYTES) + 1U;
	const std::vector dataExtents = {
			VectorCodebookDataExtent{VectorCodebookExtentIdCodec::dataExtentId(SegmentId(102U)), SegmentId(102U)},
	};
	VectorCodebookWriteRequest request;
	request.codebook_id = CODEBOOK_ID;
	request.manifest_segment_id = MANIFEST_SEGMENT;
	request.data_extents.assign(dataExtents.begin(), dataExtents.end());
	request.generation = Generation(1U);
	request.element_type = VectorCodebookElementType::VCET_UINT8;
	request.dimensions = 1U;
	request.subspace_count = 1U;
	request.centroid_count = static_cast<uint32_t>(payloadBytes);
	const std::vector<std::byte> payload(payloadBytes, std::byte{0x39});
	static_cast<void>(codebooks.writeImmutable(request, payload, PhysicalSlotId(40U), PhysicalSlotId(42U),
											   LogSequenceNumber(2U)));

	const SegmentId orphanSegment(104U);
	const std::array<std::byte, 1> orphanPayload = {std::byte{0x5A}};
	VectorCodebookExtentHeader orphanHeader;
	orphanHeader.kind = VectorCodebookExtentKind::VCEK_DATA;
	orphanHeader.codebook_id = CODEBOOK_ID;
	orphanHeader.codebook_generation = request.generation;
	orphanHeader.extent_id = VectorCodebookExtentIdCodec::dataExtentId(orphanSegment);
	orphanHeader.ordinal = 2U;
	orphanHeader.block_bytes = static_cast<uint32_t>(orphanPayload.size());
	orphanHeader.block_crc = graph::utils::calculateCrc(orphanPayload.data(), orphanPayload.size());
	const VectorCodebookExtentHeaderCodec::Bytes orphanHeaderBytes =
			VectorCodebookExtentHeaderCodec::encode(orphanHeader);
	PhysicalSegmentAllocationRequest orphanRequest;
	orphanRequest.segment_id = orphanSegment;
	orphanRequest.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	orphanRequest.owner_id = CODEBOOK_ID;
	orphanRequest.generation = request.generation;
	orphanRequest.header_bytes = static_cast<uint16_t>(RSS_ENVELOPE_BYTES + RSS_VECTOR_CODEBOOK_EXTENT_HEADER_BYTES);
	orphanRequest.header_extension.assign(orphanHeaderBytes.begin(), orphanHeaderBytes.end());
	orphanRequest.payload.assign(orphanPayload.begin(), orphanPayload.end());
	const std::array orphanLocations = {
			ExtentLocation{orphanHeader.extent_id, orphanSegment, RSS_PAGE_POOL_FIRST_DATA_FRAME,
						   RSS_PAGE_POOL_DATA_FRAME_COUNT, request.generation},
	};
	ASSERT_TRUE(physical.allocateExtentSegments(std::span(&orphanRequest, 1U), orphanLocations, PhysicalSlotId(43U),
												PhysicalSlotId(43U), LogSequenceNumber(3U))
						.has_value());

	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()), FormatException);
}

TEST_F(RelocatableIntegrityScannerTestFixture, RejectsNoncanonicalBidirectionalVectorMappings) {
	constexpr uint64_t INDEX_ID = 81U;
	constexpr uint64_t REVERSE_INDEX_ID = RSS_NODE_MAPPING_INTERNAL_INDEX_PREFIX | INDEX_ID;
	auto metadata = RelocatableMetadataStore::initializeNew(io, integrityScannerInitialSuperblock());
	const std::vector<SegmentId> poolSegmentIds = metadata->allocateLogicalSegmentIds(4U, LogSequenceNumber(2U));

	VectorPageHeader rawHeader;
	rawHeader.index_id = INDEX_ID;
	rawHeader.page_id = PageId(201U);
	rawHeader.generation = Generation(1U);
	rawHeader.kind = VectorStorageKind::VSK_RAW;
	rawHeader.record_bytes = sizeof(uint64_t) + sizeof(uint16_t);
	rawHeader.slot_capacity = 2U;
	rawHeader.bitmap_bytes = 1U;
	rawHeader.committed_lsn = LogSequenceNumber(3U);
	const PageOwnerHeader rawOwner{rawHeader.page_id,
								   INDEX_ID,
								   PageOwnerKind::POK_VECTOR_RAW,
								   0U,
								   RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								   rawHeader.generation,
								   rawHeader.committed_lsn};
	VectorPage rawPage = VectorPage::create(rawOwner, rawHeader);
	std::array<std::byte, sizeof(uint64_t) + sizeof(uint16_t)> rawRecord{};
	LittleEndianCodec::writeU64(rawRecord, 0U, 701U);
	const uint16_t rawSlot = rawPage.insert(rawRecord);
	VectorPageStore vectors(io, *metadata);
	SharedPagePoolAllocationRequest rawRequest;
	rawRequest.segment_id = poolSegmentIds[0];
	rawRequest.pool_kind = SegmentKind::SGK_VECTOR_RAW_POOL;
	rawRequest.pool_owner_id = INDEX_ID;
	rawRequest.generation = rawHeader.generation;
	rawRequest.page_id = rawHeader.page_id;
	rawRequest.page_owner_id = INDEX_ID;
	rawRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_RAW;
	ASSERT_TRUE(vectors.allocate(rawRequest, rawPage, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(3U))
						.has_value());
	const VectorLocation rawLocation{701U, rawHeader.page_id, rawSlot, rawPage.slotGeneration(rawSlot)};
	static_cast<void>(metadata->upsertVectorLocations(std::span(&rawLocation, 1U), {}, LogSequenceNumber(4U)));

	SlottedPagePool pages(io, *metadata);
	BTreePageStore trees(pages, *metadata);
	const auto allocateRoot = [&](uint64_t indexId, SegmentId segmentId, PageId pageId, PhysicalSlotId slot,
								  LogSequenceNumber allocationLsn, std::span<const BTreePageEntry> entries) {
		SlottedPagePoolAllocationRequest request;
		request.segment_id = segmentId;
		request.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		request.pool_owner_id = indexId;
		request.generation = Generation(1U);
		request.page_id = pageId;
		request.page_owner_id = indexId;
		request.page_owner_kind = PageOwnerKind::POK_INDEX;
		const PageOwnerHeader owner{
				pageId,			indexId,	  PageOwnerKind::POK_INDEX, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				Generation(1U), allocationLsn};
		BTreePageHeader header;
		header.index_id = indexId;
		header.page_id = pageId;
		header.generation = Generation(1U);
		BTreePage page = BTreePage::create(owner, header);
		for (const BTreePageEntry &entry: entries) {
			page.insert(entry);
		}
		EXPECT_TRUE(trees.allocate(request, page, slot, slot, allocationLsn).has_value());
		const IndexRootLocation root{indexId, pageId, Generation(1U)};
		static_cast<void>(trees.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(allocationLsn.value() + 1U)));
		return std::pair{owner, header};
	};
	const std::array nodeEntries = {BTreePageEntry{VectorSemanticCodec::nodeMappingKey(101U), 701U}};
	const auto [nodeOwner, nodeHeader] = allocateRoot(INDEX_ID, poolSegmentIds[1], PageId(202U), PhysicalSlotId(41U),
													  LogSequenceNumber(5U), nodeEntries);
	const std::array reverseEntries = {BTreePageEntry{VectorNodeMapping::key(701U), 101U}};
	const auto [reverseOwner, reverseHeader] = allocateRoot(REVERSE_INDEX_ID, poolSegmentIds[2], PageId(203U),
															PhysicalSlotId(42U), LogSequenceNumber(7U), reverseEntries);

	VectorManifest manifest;
	manifest.index_id = INDEX_ID;
	manifest.page_id = PageId(204U);
	manifest.generation = Generation(1U);
	manifest.committed_lsn = LogSequenceNumber(9U);
	manifest.dimensions = 1U;
	manifest.node_mapping_root_page_id = nodeHeader.page_id;
	manifest.node_mapping_root_generation = nodeHeader.generation;
	manifest.vector_node_mapping_index_id = REVERSE_INDEX_ID;
	manifest.vector_node_mapping_root_page_id = reverseHeader.page_id;
	manifest.vector_node_mapping_root_generation = reverseHeader.generation;
	manifest.entries = {{rawHeader.page_id, VectorStorageKind::VSK_RAW, 1U}};
	const PageOwnerHeader manifestOwner{manifest.page_id,
										INDEX_ID,
										PageOwnerKind::POK_VECTOR_MANIFEST,
										0U,
										RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										manifest.generation,
										manifest.committed_lsn};
	SharedPagePoolAllocationRequest manifestRequest;
	manifestRequest.segment_id = poolSegmentIds[3];
	manifestRequest.pool_kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
	manifestRequest.pool_owner_id = INDEX_ID;
	manifestRequest.generation = manifest.generation;
	manifestRequest.page_id = manifest.page_id;
	manifestRequest.page_owner_id = INDEX_ID;
	manifestRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
	VectorManifestStore manifests(io, *metadata);
	ASSERT_TRUE(manifests
						.allocate(manifestRequest, VectorManifestPage::create(manifestOwner, manifest),
								  PhysicalSlotId(43U), PhysicalSlotId(43U), manifest.committed_lsn)
						.has_value());

	const auto replaceLeaf = [&](const PageOwnerHeader &owner, const BTreePageHeader &header,
								 std::span<const BTreePageEntry> entries) {
		BTreePage page = BTreePage::create(owner, header);
		for (const BTreePageEntry &entry: entries) {
			page.insert(entry);
		}
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		PageLocation location = metadata->findPageLocation(snapshot, header.page_id);
		location.free_bytes = page.slottedPage().stats().contiguous_free_bytes;
		overwriteIntegrityScannerPagePayload(io, *metadata, header.page_id, page.serialize());
		static_cast<void>(
				metadata->upsertLocations(std::span(&location, 1U), {}, integrityScannerNextCheckpointLsn(*metadata)));
	};
	RelocatableIntegrityScanner scanner(io, *metadata);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));
	VectorManifest mismatchedReverseRoot = manifest;
	mismatchedReverseRoot.vector_node_mapping_root_generation = Generation(2U);
	overwriteIntegrityScannerPagePayload(io, *metadata, manifest.page_id,
										 VectorManifestPage::create(manifestOwner, mismatchedReverseRoot).serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwriteIntegrityScannerPagePayload(io, *metadata, manifest.page_id,
										 VectorManifestPage::create(manifestOwner, manifest).serialize());

	const std::array malformedNodeKey = {BTreePageEntry{{std::byte{0x01}}, 701U}};
	replaceLeaf(nodeOwner, nodeHeader, malformedNodeKey);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceLeaf(nodeOwner, nodeHeader, nodeEntries);
	const std::array zeroNode = {BTreePageEntry{std::vector<std::byte>(sizeof(uint64_t)), 701U}};
	replaceLeaf(nodeOwner, nodeHeader, zeroNode);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceLeaf(nodeOwner, nodeHeader, nodeEntries);
	const std::array unknownVector = {BTreePageEntry{VectorSemanticCodec::nodeMappingKey(101U), 702U}};
	replaceLeaf(nodeOwner, nodeHeader, unknownVector);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceLeaf(nodeOwner, nodeHeader, nodeEntries);
	const std::array<BTreePageEntry, 0U> emptyEntries{};
	replaceLeaf(nodeOwner, nodeHeader, emptyEntries);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceLeaf(nodeOwner, nodeHeader, nodeEntries);
	replaceLeaf(reverseOwner, reverseHeader, emptyEntries);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceLeaf(reverseOwner, reverseHeader, reverseEntries);
	const std::array mismatchedReverseNode = {BTreePageEntry{VectorNodeMapping::key(701U), 102U}};
	replaceLeaf(reverseOwner, reverseHeader, mismatchedReverseNode);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceLeaf(reverseOwner, reverseHeader, reverseEntries);
	const std::array<uint64_t, 1U> erasedRawVectorIds = {rawLocation.vector_id};
	static_cast<void>(
			metadata->upsertVectorLocations({}, erasedRawVectorIds, integrityScannerNextCheckpointLsn(*metadata)));
	replaceLeaf(nodeOwner, nodeHeader, emptyEntries);
	replaceLeaf(reverseOwner, reverseHeader, emptyEntries);
	EXPECT_NO_THROW(static_cast<void>(scanner.scan()));
	static_cast<void>(metadata->upsertVectorLocations(std::span(&rawLocation, 1U), {},
													  integrityScannerNextCheckpointLsn(*metadata)));
	replaceLeaf(nodeOwner, nodeHeader, nodeEntries);
	replaceLeaf(reverseOwner, reverseHeader, reverseEntries);
	VectorPageHeader foreignTailHeader = rawHeader;
	foreignTailHeader.index_id = INDEX_ID + 1U;
	foreignTailHeader.page_id = PageId(205U);
	const LogSequenceNumber foreignTailLsn = integrityScannerNextCheckpointLsn(*metadata);
	foreignTailHeader.committed_lsn = foreignTailLsn;
	const PageOwnerHeader foreignTailOwner{
			foreignTailHeader.page_id,		  foreignTailHeader.index_id,	PageOwnerKind::POK_VECTOR_RAW,	0U,
			RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, foreignTailHeader.generation, foreignTailHeader.committed_lsn};
	VectorPage foreignTail = VectorPage::create(foreignTailOwner, foreignTailHeader);
	SharedPagePoolAllocationRequest foreignTailRequest;
	foreignTailRequest.segment_id = SegmentId(205U);
	foreignTailRequest.pool_kind = SegmentKind::SGK_VECTOR_RAW_POOL;
	foreignTailRequest.pool_owner_id = foreignTailHeader.index_id;
	foreignTailRequest.generation = foreignTailHeader.generation;
	foreignTailRequest.page_id = foreignTailHeader.page_id;
	foreignTailRequest.page_owner_id = foreignTailHeader.index_id;
	foreignTailRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_RAW;
	ASSERT_TRUE(
			vectors.allocate(foreignTailRequest, foreignTail, PhysicalSlotId(50U), PhysicalSlotId(60U), foreignTailLsn)
					.has_value());
	VectorManifest foreignTailManifest = manifest;
	foreignTailManifest.raw_write_tail_page_id = foreignTailHeader.page_id;
	overwriteIntegrityScannerPagePayload(io, *metadata, manifest.page_id,
										 VectorManifestPage::create(manifestOwner, foreignTailManifest).serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwriteIntegrityScannerPagePayload(io, *metadata, manifest.page_id,
										 VectorManifestPage::create(manifestOwner, manifest).serialize());
	VectorPageHeader pqTailHeader = rawHeader;
	pqTailHeader.page_id = PageId(206U);
	pqTailHeader.kind = VectorStorageKind::VSK_PQ;
	pqTailHeader.record_bytes = sizeof(uint64_t);
	pqTailHeader.codebook_generation = Generation(1U);
	const LogSequenceNumber pqTailLsn = integrityScannerNextCheckpointLsn(*metadata);
	pqTailHeader.committed_lsn = pqTailLsn;
	const PageOwnerHeader pqTailOwner{pqTailHeader.page_id,
									  pqTailHeader.index_id,
									  PageOwnerKind::POK_VECTOR_PQ,
									  0U,
									  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									  pqTailHeader.generation,
									  pqTailHeader.committed_lsn};
	VectorPage pqTail = VectorPage::create(pqTailOwner, pqTailHeader);
	SharedPagePoolAllocationRequest pqTailRequest;
	pqTailRequest.segment_id = SegmentId(206U);
	pqTailRequest.pool_kind = SegmentKind::SGK_VECTOR_PQ_POOL;
	pqTailRequest.pool_owner_id = INDEX_ID;
	pqTailRequest.generation = pqTailHeader.generation;
	pqTailRequest.page_id = pqTailHeader.page_id;
	pqTailRequest.page_owner_id = INDEX_ID;
	pqTailRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_PQ;
	ASSERT_TRUE(
			vectors.allocate(pqTailRequest, pqTail, PhysicalSlotId(61U), PhysicalSlotId(70U), pqTailLsn).has_value());
	VectorManifest pqTailManifest = manifest;
	pqTailManifest.raw_write_tail_page_id = pqTailHeader.page_id;
	overwriteIntegrityScannerPagePayload(io, *metadata, manifest.page_id,
										 VectorManifestPage::create(manifestOwner, pqTailManifest).serialize());
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	overwriteIntegrityScannerPagePayload(io, *metadata, manifest.page_id,
										 VectorManifestPage::create(manifestOwner, manifest).serialize());
	const std::array excessiveNodeEntries = {
			BTreePageEntry{VectorSemanticCodec::nodeMappingKey(101U), 701U},
			BTreePageEntry{VectorSemanticCodec::nodeMappingKey(102U), 701U},
	};
	replaceLeaf(nodeOwner, nodeHeader, excessiveNodeEntries);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceLeaf(nodeOwner, nodeHeader, nodeEntries);
	const std::array malformedReverseKey = {BTreePageEntry{{std::byte{0x01}}, 101U}};
	replaceLeaf(reverseOwner, reverseHeader, malformedReverseKey);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
	replaceLeaf(reverseOwner, reverseHeader, reverseEntries);
	const std::array unknownReverseVector = {BTreePageEntry{VectorNodeMapping::key(702U), 101U}};
	replaceLeaf(reverseOwner, reverseHeader, unknownReverseVector);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);

	VectorPageHeader secondRawHeader = rawHeader;
	secondRawHeader.page_id = PageId(207U);
	secondRawHeader.committed_lsn = integrityScannerNextCheckpointLsn(*metadata);
	const PageOwnerHeader secondRawOwner{secondRawHeader.page_id,		   INDEX_ID,
										 PageOwnerKind::POK_VECTOR_RAW,	   0U,
										 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, secondRawHeader.generation,
										 secondRawHeader.committed_lsn};
	VectorPage secondRawPage = VectorPage::create(secondRawOwner, secondRawHeader);
	std::array<std::byte, sizeof(uint64_t) + sizeof(uint16_t)> secondRawRecord{};
	LittleEndianCodec::writeU64(secondRawRecord, 0U, 702U);
	const uint16_t secondRawSlot = secondRawPage.insert(secondRawRecord);
	SharedPagePoolAllocationRequest secondRawRequest = rawRequest;
	secondRawRequest.segment_id = SegmentId(207U);
	secondRawRequest.page_id = secondRawHeader.page_id;
	ASSERT_TRUE(vectors.allocate(secondRawRequest, secondRawPage, PhysicalSlotId(100U), PhysicalSlotId(100U),
								 secondRawHeader.committed_lsn)
						.has_value());
	const VectorLocation secondRawLocation{702U, secondRawHeader.page_id, secondRawSlot,
										   secondRawPage.slotGeneration(secondRawSlot)};
	static_cast<void>(metadata->upsertVectorLocations(std::span(&secondRawLocation, 1U), {},
													  integrityScannerNextCheckpointLsn(*metadata)));
	VectorManifest twoRawVectors = manifest;
	twoRawVectors.entries.push_back({secondRawHeader.page_id, VectorStorageKind::VSK_RAW, 1U});
	overwriteIntegrityScannerPagePayload(io, *metadata, manifest.page_id,
										 VectorManifestPage::create(manifestOwner, twoRawVectors).serialize());
	const std::array duplicateNodeMapping = {
			BTreePageEntry{VectorSemanticCodec::nodeMappingKey(101U), 701U},
			BTreePageEntry{VectorSemanticCodec::nodeMappingKey(102U), 701U},
	};
	replaceLeaf(nodeOwner, nodeHeader, duplicateNodeMapping);
	EXPECT_THROW(static_cast<void>(scanner.scan()), FormatException);
}
