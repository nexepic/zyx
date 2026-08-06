/**
 * @file test_EntityMap.cpp
 * @brief Native Entity Map tests for stable Page ID and slot-generation lookups.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>
#include <stdexcept>

namespace {

	PageLocation entityPage(PageId pageId, SegmentId segmentId, Generation generation) {
		return {pageId, segmentId, RSS_PAGE_POOL_FIRST_DATA_FRAME, generation};
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest, EntityMapKeepsStablePageIdentityAcrossSnapshotsDeletionAndReopen) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.require_absent = true;
	PageLocation page = entityPage(PageId(91U), request.segment_id, request.segment_generation);
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(
							 std::span(&request, 1), std::span(&page, 1), {}, PhysicalSlotId(40U), PhysicalSlotId(40U),
							 LogSequenceNumber(2U), [](std::span<const CatalogEntry>) {})
						.has_value());
	MetadataSnapshotPin before = store->openSnapshot();
	const std::array nodes = {EntityPageLocation{101U, page.page_id, 3U, 1U},
							  EntityPageLocation{102U, page.page_id, 4U, 1U}};
	const std::array edges = {EntityPageLocation{201U, page.page_id, 8U, 1U}};
	const Superblock published = store->upsertEntityPageLocations(nodes, {}, edges, {}, LogSequenceNumber(3U));
	EXPECT_TRUE(published.node_entity_directory_root.isValid());
	EXPECT_TRUE(published.edge_entity_directory_root.isValid());
	expectFormatError([&] { static_cast<void>(store->findNodePageLocation(before, 101U)); },
					  FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin original = store->openSnapshot();
	EXPECT_EQ(store->findNodePageLocation(original, 101U), nodes.front());
	EXPECT_EQ(store->findEdgePageLocation(original, 201U), edges.front());
	const EntityPageLocation replacement{101U, page.page_id, 7U, 2U};
	static_cast<void>(store->upsertEntityPageLocations(std::span(&replacement, 1), {}, {}, {}, LogSequenceNumber(4U)));
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findNodePageLocation(original, 101U), nodes.front());
	EXPECT_EQ(store->findNodePageLocation(current, 101U), replacement);
	EXPECT_EQ(store->findNodePageLocation(current, 102U), nodes.back());

	const std::array<uint64_t, 1U> erasedNodes{102U};
	static_cast<void>(store->upsertEntityPageLocations({}, erasedNodes, {}, {}, LogSequenceNumber(5U)));
	MetadataSnapshotPin afterErase = store->openSnapshot();
	EXPECT_TRUE(store->findNodePageLocationOptional(afterErase, 102U).has_value() == false);
	EXPECT_EQ(store->findNodePageLocation(current, 102U), nodes.back());
	before.release();
	original.release();
	current.release();
	afterErase.release();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopened->findNodePageLocation(reopenedSnapshot, 101U), replacement);
	EXPECT_FALSE(reopened->findNodePageLocationOptional(reopenedSnapshot, 102U).has_value());
	EXPECT_EQ(reopened->findEdgePageLocation(reopenedSnapshot, 201U), edges.front());
}

TEST_F(RelocatableMetadataStoreTest, EntityMapRejectsMissingWrongKindAndMalformedPageReferences) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const EntityPageLocation missing{1U, PageId(91U), 0U, 1U};
	expectFormatError(
			[&] {
				static_cast<void>(
						store->upsertEntityPageLocations(std::span(&missing, 1), {}, {}, {}, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);

	CatalogSlotReservationRequest propertyRequest = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	propertyRequest.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	propertyRequest.require_absent = true;
	const PageLocation propertyPage =
			entityPage(PageId(92U), propertyRequest.segment_id, propertyRequest.segment_generation);
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(
							 std::span(&propertyRequest, 1), std::span(&propertyPage, 1), {}, PhysicalSlotId(40U),
							 PhysicalSlotId(40U), LogSequenceNumber(2U), [](std::span<const CatalogEntry>) {})
						.has_value());
	const EntityPageLocation wrongKind{1U, propertyPage.page_id, 0U, 1U};
	expectFormatError(
			[&] {
				static_cast<void>(
						store->upsertEntityPageLocations(std::span(&wrongKind, 1), {}, {}, {}, LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);

	CatalogSlotReservationRequest entityRequest = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U, 1U);
	entityRequest.require_absent = true;
	const PageLocation page = entityPage(PageId(93U), entityRequest.segment_id, entityRequest.segment_generation);
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(
							 std::span(&entityRequest, 1), std::span(&page, 1), {}, PhysicalSlotId(41U),
							 PhysicalSlotId(41U), LogSequenceNumber(3U), [](std::span<const CatalogEntry>) {})
						.has_value());
	const EntityPageLocation noGeneration{2U, page.page_id, 0U, 0U};
	expectFormatError(
			[&] {
				static_cast<void>(store->upsertEntityPageLocations(std::span(&noGeneration, 1), {}, {}, {},
																   LogSequenceNumber(4U)));
			},
			FormatError::FERR_INVALID_STATE);
	const EntityPageLocation invalidSlot{2U, page.page_id, std::numeric_limits<uint16_t>::max(), 1U};
	expectFormatError(
			[&] {
				static_cast<void>(store->upsertEntityPageLocations(std::span(&invalidSlot, 1), {}, {}, {},
																   LogSequenceNumber(4U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, EntityPageAndEntityMapPublishTogetherAcrossCowReplacement) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.require_absent = true;
	const std::optional<CatalogSlotReservations> reservation =
			store->reserveFirstFreeAndUpsertCatalog(std::span(&request, 1), PhysicalSlotId(40U), PhysicalSlotId(40U),
													LogSequenceNumber(2U), [](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(reservation.has_value());
	const EntityPageLocation firstNode{101U, PageId(91U), 0U, 1U};
	PageLocation firstPage{firstNode.page_id, reservation->entries.front().segment_id, 0U,
						   reservation->entries.front().generation};
	static_cast<void>(store->appendEntityPageToExistingPool(
			reservation->entries.front(), firstPage, std::span(&firstNode, 1), {}, LogSequenceNumber(3U),
			[](const CatalogEntry &, PageLocation &page) { page.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME; }));
	const MetadataSnapshotPin original = store->openSnapshot();
	const PageLocation expected = store->findPageLocation(original, firstPage.page_id);
	EXPECT_EQ(store->findNodePageLocation(original, firstNode.entity_id), firstNode);

	const EntityPageLocation secondNode{102U, firstNode.page_id, 1U, 1U};
	const std::array<uint64_t, 1U> erasedNodes{firstNode.entity_id};
	PageLocation replacement{firstNode.page_id, expected.segment_id, 0U, expected.generation};
	static_cast<void>(store->replaceEntityPageInExistingPool(
			reservation->entries.front(), expected, replacement, std::span(&secondNode, 1), erasedNodes, {}, {},
			LogSequenceNumber(4U), [](const CatalogEntry &, const PageLocation &, PageLocation &page) {
				page.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
			}));
	const MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findNodePageLocation(original, firstNode.entity_id), firstNode);
	EXPECT_FALSE(store->findNodePageLocationOptional(original, secondNode.entity_id).has_value());
	EXPECT_FALSE(store->findNodePageLocationOptional(current, firstNode.entity_id).has_value());
	EXPECT_EQ(store->findNodePageLocation(current, secondNode.entity_id), secondNode);
	EXPECT_NE(store->findPageLocation(original, firstPage.page_id).page_index,
			  store->findPageLocation(current, firstPage.page_id).page_index);
}

TEST_F(RelocatableMetadataStoreTest, EntityPageReservationAtomicallyPublishesCatalogPageAndEntityMap) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest request = reservationRequest(31U, 1U);
	request.require_absent = true;
	PageLocation page = entityPage(PageId(91U), request.segment_id, request.segment_generation);
	const std::array nodes = {EntityPageLocation{101U, page.page_id, 0U, 1U},
							  EntityPageLocation{102U, page.page_id, 1U, 1U}};
	MetadataSnapshotPin before = store->openSnapshot();
	bool prepared = false;
	const std::optional<CatalogSlotReservations> reservation =
			store->reserveFirstFreeAndUpsertCatalogAndEntityPageLocations(
					std::span(&request, 1), std::span(&page, 1), nodes, {}, PhysicalSlotId(40U), PhysicalSlotId(40U),
					LogSequenceNumber(2U),
					[&prepared](std::span<const CatalogEntry> entries, std::span<PageLocation> locations) {
						prepared = entries.size() == 1U && entries.front().physical_slot == PhysicalSlotId(40U);
						locations.front().page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
					});
	ASSERT_TRUE(reservation.has_value());
	EXPECT_TRUE(prepared);
	EXPECT_EQ(reservation->superblock.generation, Generation(2U));
	expectFormatError([&] { static_cast<void>(store->findCatalog(before, request.segment_id)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findPageLocation(before, page.page_id)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findNodePageLocation(before, nodes.front().entity_id)); },
					  FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_TRUE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(40U)));
	const CatalogEntry visible = store->findCatalog(current, request.segment_id);
	EXPECT_EQ(visible.segment_id, reservation->entries.front().segment_id);
	EXPECT_EQ(visible.physical_slot, reservation->entries.front().physical_slot);
	EXPECT_EQ(visible.generation, reservation->entries.front().generation);
	EXPECT_EQ(store->findPageLocation(current, page.page_id), page);
	EXPECT_EQ(store->findNodePageLocation(current, nodes.front().entity_id), nodes.front());
	EXPECT_EQ(store->findNodePageLocation(current, nodes.back().entity_id), nodes.back());
	before.release();
	current.release();

	auto reopened = RelocatableMetadataStore::open(io);
	const MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopened->findPageLocation(reopenedSnapshot, page.page_id), page);
	EXPECT_EQ(reopened->findNodePageLocation(reopenedSnapshot, nodes.front().entity_id), nodes.front());
}

TEST_F(RelocatableMetadataStoreTest, EntityPageReservationRejectsInvalidMapBeforePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest request = reservationRequest(31U, 1U);
	request.require_absent = true;
	PageLocation page = entityPage(PageId(91U), request.segment_id, request.segment_generation);
	const EntityPageLocation foreignPage{101U, PageId(92U), 0U, 1U};
	bool prepared = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndEntityPageLocations(
						std::span(&request, 1), std::span(&page, 1), std::span(&foreignPage, 1), {},
						PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U),
						[&prepared](std::span<const CatalogEntry>, std::span<PageLocation>) { prepared = true; }));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(prepared);
	MetadataSnapshotPin before = store->openSnapshot();
	EXPECT_EQ(before.superblock().generation, Generation(1U));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(before, PhysicalSlotId(40U)));
	EXPECT_FALSE(store->findNodePageLocationOptional(before, foreignPage.entity_id).has_value());
	before.release();

	const EntityPageLocation valid{101U, page.page_id, 0U, 1U};
	EXPECT_THROW(static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndEntityPageLocations(
						 std::span(&request, 1), std::span(&page, 1), std::span(&valid, 1), {}, PhysicalSlotId(40U),
						 PhysicalSlotId(40U), LogSequenceNumber(2U),
						 [](std::span<const CatalogEntry>, std::span<PageLocation>) {
							 throw std::runtime_error("injected Entity page write failure");
						 })),
				 std::runtime_error);
	const MetadataSnapshotPin after = store->openSnapshot();
	EXPECT_EQ(after.superblock().generation, Generation(1U));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(after, PhysicalSlotId(40U)));
	EXPECT_FALSE(store->findNodePageLocationOptional(after, valid.entity_id).has_value());
}

TEST_F(RelocatableMetadataStoreTest, EntityPageRetirementRejectsStaleOrForeignMappingsWithoutPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest request = reservationRequest(31U, 1U);
	request.require_absent = true;
	const std::optional<CatalogSlotReservations> reservation =
			store->reserveFirstFreeAndUpsertCatalog(std::span(&request, 1), PhysicalSlotId(40U), PhysicalSlotId(40U),
													LogSequenceNumber(2U), [](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(reservation.has_value());
	const EntityPageLocation node{101U, PageId(91U), 0U, 1U};
	PageLocation page{node.page_id, reservation->entries.front().segment_id, 0U,
					  reservation->entries.front().generation};
	static_cast<void>(store->appendEntityPageToExistingPool(reservation->entries.front(), page, std::span(&node, 1), {},
															LogSequenceNumber(3U),
															[](const CatalogEntry &, PageLocation &prepared) {
																prepared.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
															}));
	MetadataSnapshotPin original = store->openSnapshot();
	const PageLocation currentPage = store->findPageLocation(original, node.page_id);
	bool prepared = false;
	const std::array<uint64_t, 2U> nonCanonical{node.entity_id, node.entity_id};
	expectFormatError(
			[&] {
				static_cast<void>(store->retireEntityPageInExistingPool(
						reservation->entries.front(), currentPage, nonCanonical, {}, LogSequenceNumber(4U),
						[&prepared](const CatalogEntry &, const PageLocation &) { prepared = true; }));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_FALSE(prepared);

	CatalogEntry stalePool = reservation->entries.front();
	stalePool.owner_id += 1U;
	const std::array<uint64_t, 1U> erased{node.entity_id};
	expectFormatError(
			[&] {
				static_cast<void>(
						store->retireEntityPageInExistingPool(stalePool, currentPage, erased, {}, LogSequenceNumber(4U),
															  [](const CatalogEntry &, const PageLocation &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	const std::array<uint64_t, 1U> foreign{node.entity_id + 1U};
	expectFormatError(
			[&] {
				static_cast<void>(store->retireEntityPageInExistingPool(
						reservation->entries.front(), currentPage, foreign, {}, LogSequenceNumber(4U),
						[](const CatalogEntry &, const PageLocation &) {}));
			},
			FormatError::FERR_INVALID_STATE);

	prepared = false;
	EXPECT_THROW(static_cast<void>(store->retireEntityPageInExistingPool(
						 reservation->entries.front(), currentPage, erased, {}, LogSequenceNumber(4U),
						 [&prepared](const CatalogEntry &, const PageLocation &) { prepared = true; },
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	EXPECT_TRUE(prepared);
	MetadataSnapshotPin afterInterrupted = store->openSnapshot();
	EXPECT_EQ(store->findNodePageLocation(afterInterrupted, node.entity_id), node);
	EXPECT_EQ(store->findPageLocation(afterInterrupted, node.page_id), currentPage);

	static_cast<void>(store->retireEntityPageInExistingPool(reservation->entries.front(), currentPage, erased, {},
															LogSequenceNumber(4U),
															[](const CatalogEntry &, const PageLocation &) {}));
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findNodePageLocation(original, node.entity_id), node);
	EXPECT_FALSE(store->findNodePageLocationOptional(current, node.entity_id).has_value());
	EXPECT_FALSE(store->findPageLocationOptional(current, node.page_id).has_value());
	original.release();
	afterInterrupted.release();
	current.release();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_FALSE(reopened->findNodePageLocationOptional(reopenedSnapshot, node.entity_id).has_value());
	EXPECT_FALSE(reopened->findPageLocationOptional(reopenedSnapshot, node.page_id).has_value());
}

TEST_F(RelocatableMetadataStoreTest, EntityPageBatchReplacementPublishesSeveralPagesAndMapsAtomically) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest nodeRequest = reservationRequest(31U, 1U);
	nodeRequest.require_absent = true;
	const std::optional<CatalogSlotReservations> nodeReservation = store->reserveFirstFreeAndUpsertCatalog(
			std::span(&nodeRequest, 1), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U),
			[](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(nodeReservation.has_value());
	CatalogSlotReservationRequest edgeRequest = reservationRequest(32U, 1U);
	edgeRequest.require_absent = true;
	const std::optional<CatalogSlotReservations> edgeReservation = store->reserveFirstFreeAndUpsertCatalog(
			std::span(&edgeRequest, 1), PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(3U),
			[](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(edgeReservation.has_value());
	const EntityPageLocation firstNode{101U, PageId(91U), 0U, 1U};
	PageLocation nodePage{firstNode.page_id, nodeReservation->entries.front().segment_id, 0U,
						  nodeReservation->entries.front().generation};
	static_cast<void>(store->appendEntityPageToExistingPool(nodeReservation->entries.front(), nodePage,
															std::span(&firstNode, 1), {}, LogSequenceNumber(4U),
															[](const CatalogEntry &, PageLocation &prepared) {
																prepared.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
															}));
	const EntityPageLocation firstEdge{201U, PageId(92U), 0U, 1U};
	PageLocation edgePage{firstEdge.page_id, edgeReservation->entries.front().segment_id, 0U,
						  edgeReservation->entries.front().generation};
	static_cast<void>(store->appendEntityPageToExistingPool(edgeReservation->entries.front(), edgePage, {},
															std::span(&firstEdge, 1), LogSequenceNumber(5U),
															[](const CatalogEntry &, PageLocation &prepared) {
																prepared.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
															}));
	MetadataSnapshotPin original = store->openSnapshot();
	const PageLocation originalNodePage = store->findPageLocation(original, firstNode.page_id);
	const PageLocation originalEdgePage = store->findPageLocation(original, firstEdge.page_id);

	EntityPageReplacementRequest nodeMutation;
	nodeMutation.expected_pool = nodeReservation->entries.front();
	nodeMutation.expected_location = originalNodePage;
	nodeMutation.inserted_nodes.push_back({102U, firstNode.page_id, 1U, 1U});
	nodeMutation.erased_node_ids.push_back(firstNode.entity_id);
	EntityPageReplacementRequest edgeMutation;
	edgeMutation.expected_pool = edgeReservation->entries.front();
	edgeMutation.expected_location = originalEdgePage;
	edgeMutation.inserted_edges.push_back({202U, firstEdge.page_id, 1U, 1U});
	edgeMutation.erased_edge_ids.push_back(firstEdge.entity_id);
	const std::array mutations{nodeMutation, edgeMutation};
	bool prepared = false;
	static_cast<void>(store->replaceEntityPagesInExistingPools(
			mutations, LogSequenceNumber(6U), [&prepared](std::span<EntityPageReplacementMaterialization> pages) {
				prepared = pages.size() == 2U;
				for (EntityPageReplacementMaterialization &page: pages) {
					page.replacement.page_index = static_cast<uint8_t>(page.expected_location.page_index + 1U);
				}
			}));
	EXPECT_TRUE(prepared);
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findNodePageLocation(original, firstNode.entity_id), firstNode);
	EXPECT_EQ(store->findEdgePageLocation(original, firstEdge.entity_id), firstEdge);
	EXPECT_FALSE(store->findNodePageLocationOptional(original, 102U).has_value());
	EXPECT_FALSE(store->findEdgePageLocationOptional(original, 202U).has_value());
	EXPECT_FALSE(store->findNodePageLocationOptional(current, firstNode.entity_id).has_value());
	EXPECT_FALSE(store->findEdgePageLocationOptional(current, firstEdge.entity_id).has_value());
	EXPECT_EQ(store->findNodePageLocation(current, 102U), (EntityPageLocation{102U, firstNode.page_id, 1U, 1U}));
	EXPECT_EQ(store->findEdgePageLocation(current, 202U), (EntityPageLocation{202U, firstEdge.page_id, 1U, 1U}));
	EXPECT_NE(store->findPageLocation(current, firstNode.page_id).page_index, originalNodePage.page_index);
	EXPECT_NE(store->findPageLocation(current, firstEdge.page_id).page_index, originalEdgePage.page_index);

	const std::array duplicate{nodeMutation, nodeMutation};
	prepared = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->replaceEntityPagesInExistingPools(
						duplicate, LogSequenceNumber(7U),
						[&prepared](std::span<EntityPageReplacementMaterialization>) { prepared = true; }));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(prepared);
	EXPECT_EQ(store->findNodePageLocation(current, 102U), (EntityPageLocation{102U, firstNode.page_id, 1U, 1U}));

	const PageLocation currentNodePage = store->findPageLocation(current, firstNode.page_id);
	const PageLocation currentEdgePage = store->findPageLocation(current, firstEdge.page_id);
	EntityPageReplacementRequest nodeRetirement;
	nodeRetirement.expected_pool = nodeReservation->entries.front();
	nodeRetirement.expected_location = currentNodePage;
	nodeRetirement.erased_node_ids.push_back(102U);
	nodeRetirement.retire_page = true;
	EntityPageReplacementRequest edgeRetirement;
	edgeRetirement.expected_pool = edgeReservation->entries.front();
	edgeRetirement.expected_location = currentEdgePage;
	edgeRetirement.erased_edge_ids.push_back(202U);
	edgeRetirement.retire_page = true;
	prepared = false;
	static_cast<void>(store->replaceEntityPagesInExistingPools(
			std::array{nodeRetirement, edgeRetirement}, LogSequenceNumber(7U),
			[&prepared](std::span<EntityPageReplacementMaterialization> pages) {
				prepared = pages.size() == 2U && pages[0].retire_page && pages[1].retire_page &&
						   !pages[0].replacement.page_id.isValid() && !pages[1].replacement.page_id.isValid();
			}));
	EXPECT_TRUE(prepared);
	MetadataSnapshotPin retired = store->openSnapshot();
	EXPECT_TRUE(store->findNodePageLocationOptional(current, 102U).has_value());
	EXPECT_TRUE(store->findEdgePageLocationOptional(current, 202U).has_value());
	EXPECT_FALSE(store->findNodePageLocationOptional(retired, 102U).has_value());
	EXPECT_FALSE(store->findEdgePageLocationOptional(retired, 202U).has_value());
	EXPECT_FALSE(store->findPageLocationOptional(retired, firstNode.page_id).has_value());
	EXPECT_FALSE(store->findPageLocationOptional(retired, firstEdge.page_id).has_value());
}

TEST_F(RelocatableMetadataStoreTest, EntityPageBatchMutationAtomicallyReplacesAndAppendsAcrossPools) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest nodeRequest = reservationRequest(31U, 1U);
	nodeRequest.require_absent = true;
	const std::optional<CatalogSlotReservations> nodeReservation = store->reserveFirstFreeAndUpsertCatalog(
			std::span(&nodeRequest, 1), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U),
			[](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(nodeReservation.has_value());
	CatalogSlotReservationRequest edgeRequest = reservationRequest(32U, 1U);
	edgeRequest.require_absent = true;
	const std::optional<CatalogSlotReservations> edgeReservation = store->reserveFirstFreeAndUpsertCatalog(
			std::span(&edgeRequest, 1), PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(3U),
			[](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(edgeReservation.has_value());

	const EntityPageLocation firstNode{101U, PageId(91U), 0U, 1U};
	PageLocation nodePage{firstNode.page_id, nodeReservation->entries.front().segment_id, 0U,
						  nodeReservation->entries.front().generation};
	static_cast<void>(store->appendEntityPageToExistingPool(nodeReservation->entries.front(), nodePage,
															std::span(&firstNode, 1), {}, LogSequenceNumber(4U),
															[](const CatalogEntry &, PageLocation &prepared) {
																prepared.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
															}));
	const EntityPageLocation firstEdge{201U, PageId(92U), 0U, 1U};
	PageLocation edgePage{firstEdge.page_id, edgeReservation->entries.front().segment_id, 0U,
						  edgeReservation->entries.front().generation};
	static_cast<void>(store->appendEntityPageToExistingPool(edgeReservation->entries.front(), edgePage, {},
															std::span(&firstEdge, 1), LogSequenceNumber(5U),
															[](const CatalogEntry &, PageLocation &prepared) {
																prepared.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
															}));

	MetadataSnapshotPin original = store->openSnapshot();
	EntityPageReplacementRequest nodeReplacement;
	nodeReplacement.expected_pool = nodeReservation->entries.front();
	nodeReplacement.expected_location = store->findPageLocation(original, firstNode.page_id);
	nodeReplacement.inserted_nodes.push_back({102U, firstNode.page_id, 1U, 1U});
	nodeReplacement.erased_node_ids.push_back(firstNode.entity_id);
	EntityPageAppendBatchRequest edgeAppend;
	edgeAppend.expected_pool = edgeReservation->entries.front();
	edgeAppend.page_id = PageId(93U);
	edgeAppend.inserted_edges.push_back({202U, edgeAppend.page_id, 0U, 1U});

	bool prepared = false;
	const Superblock published = store->mutateEntityPagesInExistingPools(
			std::span(&nodeReplacement, 1), std::span(&edgeAppend, 1), LogSequenceNumber(6U),
			[&prepared](std::span<EntityPageReplacementMaterialization> replacements,
						std::span<EntityPageAppendBatchMaterialization> appends) {
				prepared = replacements.size() == 1U && appends.size() == 1U;
				replacements.front().replacement.page_index =
						static_cast<uint8_t>(replacements.front().expected_location.page_index + 1U);
				// Frames are scoped to a pool, so both pools may independently use frame 2.
				appends.front().appended_location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
			});
	EXPECT_TRUE(prepared);
	EXPECT_EQ(published.generation, Generation(6U));
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findNodePageLocation(original, firstNode.entity_id), firstNode);
	EXPECT_EQ(store->findEdgePageLocation(original, firstEdge.entity_id), firstEdge);
	EXPECT_FALSE(store->findNodePageLocationOptional(original, 102U).has_value());
	EXPECT_FALSE(store->findEdgePageLocationOptional(original, 202U).has_value());
	EXPECT_FALSE(store->findPageLocationOptional(original, edgeAppend.page_id).has_value());
	EXPECT_FALSE(store->findNodePageLocationOptional(current, firstNode.entity_id).has_value());
	EXPECT_EQ(store->findNodePageLocation(current, 102U), (EntityPageLocation{102U, firstNode.page_id, 1U, 1U}));
	EXPECT_EQ(store->findEdgePageLocation(current, firstEdge.entity_id), firstEdge);
	EXPECT_EQ(store->findEdgePageLocation(current, 202U), (EntityPageLocation{202U, edgeAppend.page_id, 0U, 1U}));
	EXPECT_EQ(store->findPageLocation(current, edgeAppend.page_id).page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);

	EntityPageAppendBatchRequest duplicatePage = edgeAppend;
	duplicatePage.page_id = firstNode.page_id;
	bool rejectedPreparer = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateEntityPagesInExistingPools(
						{}, std::span(&duplicatePage, 1), LogSequenceNumber(7U),
						[&rejectedPreparer](std::span<EntityPageReplacementMaterialization>,
											std::span<EntityPageAppendBatchMaterialization>) {
							rejectedPreparer = true;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(rejectedPreparer);

	EntityPageAppendBatchRequest duplicateEntity = edgeAppend;
	duplicateEntity.page_id = PageId(94U);
	duplicateEntity.inserted_edges = {{202U, duplicateEntity.page_id, 1U, 1U}};
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateEntityPagesInExistingPools(
						{}, std::span(&duplicateEntity, 1), LogSequenceNumber(7U),
						[](std::span<EntityPageReplacementMaterialization>,
						   std::span<EntityPageAppendBatchMaterialization>) {}));
			},
			FormatError::FERR_INVALID_STATE);

	EntityPageAppendBatchRequest interruptedAppend;
	interruptedAppend.expected_pool = edgeReservation->entries.front();
	interruptedAppend.page_id = PageId(95U);
	interruptedAppend.inserted_edges.push_back({203U, interruptedAppend.page_id, 0U, 1U});
	EXPECT_THROW(static_cast<void>(store->mutateEntityPagesInExistingPools(
						 {}, std::span(&interruptedAppend, 1), LogSequenceNumber(7U),
						 [](std::span<EntityPageReplacementMaterialization>,
							std::span<EntityPageAppendBatchMaterialization> appends) {
							 appends.front().appended_location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 2U;
						 },
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin afterInterrupted = store->openSnapshot();
	EXPECT_FALSE(store->findPageLocationOptional(afterInterrupted, interruptedAppend.page_id).has_value());
	EXPECT_FALSE(store->findEdgePageLocationOptional(afterInterrupted, 203U).has_value());
}

TEST_F(RelocatableMetadataStoreTest, EntityPoolTransactionPublishesLabelPagesWithoutEntityMapDeltas) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest request = reservationRequest(31U, 1U);
	request.require_absent = true;
	const std::optional<CatalogSlotReservations> reservation =
			store->reserveFirstFreeAndUpsertCatalog(std::span(&request, 1), PhysicalSlotId(40U), PhysicalSlotId(40U),
													LogSequenceNumber(2U), [](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(reservation.has_value());
	const EntityPageLocation node{101U, PageId(91U), 0U, 1U};
	PageLocation nodePage{node.page_id, reservation->entries.front().segment_id, 0U,
						  reservation->entries.front().generation};
	static_cast<void>(store->appendEntityPageToExistingPool(reservation->entries.front(), nodePage, std::span(&node, 1),
															{}, LogSequenceNumber(3U),
															[](const CatalogEntry &, PageLocation &prepared) {
																prepared.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
															}));

	MetadataSnapshotPin original = store->openSnapshot();
	EntityPageReplacementRequest nodeReplacement;
	nodeReplacement.expected_pool = reservation->entries.front();
	nodeReplacement.expected_location = store->findPageLocation(original, node.page_id);
	EntityPageAppendBatchRequest labelAppend;
	labelAppend.expected_pool = reservation->entries.front();
	labelAppend.page_id = PageId(92U);
	labelAppend.owner_kind = PageOwnerKind::POK_ENTITY_LABEL;

	const Superblock published = store->mutateEntityPagesInExistingPools(
			std::span(&nodeReplacement, 1), std::span(&labelAppend, 1), LogSequenceNumber(4U),
			[](std::span<EntityPageReplacementMaterialization> replacements,
			   std::span<EntityPageAppendBatchMaterialization> appends) {
				ASSERT_EQ(replacements.front().owner_kind, PageOwnerKind::POK_ENTITY);
				ASSERT_EQ(appends.front().owner_kind, PageOwnerKind::POK_ENTITY_LABEL);
				replacements.front().replacement.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
				appends.front().appended_location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 2U;
			});
	EXPECT_EQ(published.generation, Generation(4U));
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findNodePageLocation(current, node.entity_id), node);
	EXPECT_EQ(store->findPageLocation(original, node.page_id).page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
	EXPECT_FALSE(store->findPageLocationOptional(original, labelAppend.page_id).has_value());
	EXPECT_EQ(store->findPageLocation(current, node.page_id).page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);
	EXPECT_EQ(store->findPageLocation(current, labelAppend.page_id).page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME + 2U);

	EntityPageAppendBatchRequest invalidLabelAppend = labelAppend;
	invalidLabelAppend.page_id = PageId(93U);
	invalidLabelAppend.inserted_nodes.push_back({102U, invalidLabelAppend.page_id, 0U, 1U});
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateEntityPagesInExistingPools(
						{}, std::span(&invalidLabelAppend, 1), LogSequenceNumber(5U),
						[](std::span<EntityPageReplacementMaterialization>,
						   std::span<EntityPageAppendBatchMaterialization>) {}));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(store->findPageLocationOptional(current, invalidLabelAppend.page_id).has_value());
}

TEST_F(RelocatableMetadataStoreTest, EntityPageTransactionAtomicallyReplacesAndAllocatesNewPool) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest originalRequest = reservationRequest(31U, 1U);
	originalRequest.require_absent = true;
	const std::optional<CatalogSlotReservations> originalReservation = store->reserveFirstFreeAndUpsertCatalog(
			std::span(&originalRequest, 1), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U),
			[](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(originalReservation.has_value());
	const EntityPageLocation originalNode{101U, PageId(91U), 0U, 1U};
	PageLocation originalPage{originalNode.page_id, originalReservation->entries.front().segment_id, 0U,
							  originalReservation->entries.front().generation};
	static_cast<void>(store->appendEntityPageToExistingPool(originalReservation->entries.front(), originalPage,
															std::span(&originalNode, 1), {}, LogSequenceNumber(3U),
															[](const CatalogEntry &, PageLocation &prepared) {
																prepared.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
															}));
	MetadataSnapshotPin original = store->openSnapshot();

	EntityPageReplacementRequest replacement;
	replacement.expected_pool = originalReservation->entries.front();
	replacement.expected_location = store->findPageLocation(original, originalNode.page_id);
	replacement.inserted_nodes.push_back({102U, originalNode.page_id, 1U, 1U});
	replacement.erased_node_ids.push_back(originalNode.entity_id);
	EntityPageNewPoolBatchRequest newPool;
	newPool.reservation = reservationRequest(32U, 1U);
	newPool.reservation.owner_id = 29U;
	newPool.reservation.require_absent = true;
	newPool.page_id = PageId(92U);
	newPool.first_slot = PhysicalSlotId(40U);
	newPool.last_slot = PhysicalSlotId(41U);
	newPool.inserted_edges.push_back({201U, newPool.page_id, 0U, 1U});

	bool prepared = false;
	const Superblock published = store->mutateEntityPages(
			std::span(&replacement, 1), {}, std::span(&newPool, 1), LogSequenceNumber(4U),
			[&prepared](std::span<EntityPageReplacementMaterialization> replacements,
						std::span<EntityPageAppendBatchMaterialization> appends,
						std::span<EntityPageNewPoolBatchMaterialization> pools) {
				prepared = replacements.size() == 1U && appends.empty() && pools.size() == 1U &&
						   pools.front().pool.physical_slot == PhysicalSlotId(41U);
				replacements.front().replacement.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
				pools.front().initial_location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
			});
	EXPECT_TRUE(prepared);
	EXPECT_EQ(published.generation, Generation(4U));
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findNodePageLocation(original, originalNode.entity_id), originalNode);
	EXPECT_FALSE(store->findNodePageLocationOptional(original, 102U).has_value());
	EXPECT_FALSE(store->findEdgePageLocationOptional(original, 201U).has_value());
	expectFormatError([&] { static_cast<void>(store->findCatalog(original, SegmentId(32U))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(store->findNodePageLocationOptional(current, originalNode.entity_id).has_value());
	EXPECT_EQ(store->findNodePageLocation(current, 102U), (EntityPageLocation{102U, originalNode.page_id, 1U, 1U}));
	EXPECT_EQ(store->findCatalog(current, SegmentId(32U)).physical_slot, PhysicalSlotId(41U));
	EXPECT_TRUE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(41U)));
	EXPECT_EQ(store->findPageLocation(current, newPool.page_id),
			  (PageLocation{newPool.page_id, SegmentId(32U), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U)}));
	EXPECT_EQ(store->findEdgePageLocation(current, 201U), (EntityPageLocation{201U, newPool.page_id, 0U, 1U}));

	EntityPageNewPoolBatchRequest interruptedPool = newPool;
	interruptedPool.reservation.segment_id = SegmentId(33U);
	interruptedPool.page_id = PageId(93U);
	interruptedPool.last_slot = PhysicalSlotId(42U);
	interruptedPool.inserted_edges = {{202U, interruptedPool.page_id, 0U, 1U}};
	EXPECT_THROW(
			static_cast<void>(store->mutateEntityPages(
					{}, {}, std::span(&interruptedPool, 1), LogSequenceNumber(5U),
					[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
					   std::span<EntityPageNewPoolBatchMaterialization> pools) {
						pools.front().initial_location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
					},
					withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
			BootstrapPublishInterrupted);
	MetadataSnapshotPin afterInterrupted = store->openSnapshot();
	EXPECT_EQ(afterInterrupted.superblock(), current.superblock());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(afterInterrupted, PhysicalSlotId(42U)));
	EXPECT_FALSE(store->findPageLocationOptional(afterInterrupted, interruptedPool.page_id).has_value());
	EXPECT_FALSE(store->findEdgePageLocationOptional(afterInterrupted, 202U).has_value());
}

TEST_F(RelocatableMetadataStoreTest, NativeGraphPageTransactionPublishesEntityAndPropertyDirectoriesTogether) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest entityRequest = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	entityRequest.require_absent = true;
	const std::optional<CatalogSlotReservations> entityReservation = store->reserveFirstFreeAndUpsertCatalog(
			std::span(&entityRequest, 1), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U),
			[](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(entityReservation.has_value());
	const EntityPageLocation node{101U, PageId(91U), 0U, 1U};
	PageLocation entityPageLocation{node.page_id, entityReservation->entries.front().segment_id, 0U,
									entityReservation->entries.front().generation, 0U};
	static_cast<void>(store->appendEntityPageToExistingPool(
			entityReservation->entries.front(), entityPageLocation, std::span(&node, 1), {}, LogSequenceNumber(3U),
			[](const CatalogEntry &, PageLocation &page) { page.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME; }));

	const EntityPropertyKey propertyKey{EntityPropertyOwnerKind::EPOK_NODE, node.entity_id};
	const std::optional<EntityPropertyPagePoolMaterialization> propertyMaterialization =
			store->materializeEntityPropertyPagePool(
					701U, propertyKey, PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(4U),
					[](const CatalogEntry &pool, PageId pageId, PageLocation &page, EntityPropertyLocation &property) {
						page = {pageId, pool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, pool.generation, 0U};
						property = {property.key, pageId, 0U, 1U};
					});
	ASSERT_TRUE(propertyMaterialization.has_value());
	MetadataSnapshotPin original = store->openSnapshot();
	const PageLocation originalEntityPage = store->findPageLocation(original, node.page_id);
	const PageLocation originalPropertyPage = store->findPageLocation(original, propertyMaterialization->page.page_id);
	const EntityPropertyLocation originalProperty = store->findEntityPropertyLocation(original, propertyKey);

	EntityPageReplacementRequest entityReplacement;
	entityReplacement.expected_pool = entityReservation->entries.front();
	entityReplacement.expected_location = originalEntityPage;
	NativeGraphPropertyPageReplacementRequest propertyReplacement;
	propertyReplacement.expected_pool = propertyMaterialization->pool;
	propertyReplacement.expected_location = originalPropertyPage;
	propertyReplacement.expected_page_owner_id = 701U;
	propertyReplacement.expected_owner_lsn = LogSequenceNumber(4U);
	propertyReplacement.upserted_properties.push_back(
			{propertyKey, originalPropertyPage.page_id, static_cast<uint16_t>(originalProperty.slot_id + 1U), 2U});

	bool prepared = false;
	const Superblock published = store->mutateNativeGraphPages(
			std::span(&entityReplacement, 1), {}, {}, std::span(&propertyReplacement, 1), {}, LogSequenceNumber(5U),
			[&prepared](std::span<EntityPageReplacementMaterialization> entities,
						std::span<EntityPageAppendBatchMaterialization> appends,
						std::span<EntityPageNewPoolBatchMaterialization> newPools,
						std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
						std::span<NativeGraphPropertyPageBatchMaterialization> propertyPools) {
				prepared = entities.size() == 1U && appends.empty() && newPools.empty() && properties.size() == 1U;
				EXPECT_TRUE(propertyPools.empty());
				entities.front().replacement.page_index =
						static_cast<uint8_t>(entities.front().expected_location.page_index + 1U);
				properties.front().replacement.page_index =
						static_cast<uint8_t>(properties.front().expected_location.page_index + 1U);
			});
	EXPECT_TRUE(prepared);
	EXPECT_EQ(published.generation, Generation(5U));
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findNodePageLocation(original, node.entity_id), node);
	EXPECT_EQ(store->findEntityPropertyLocation(original, propertyKey), originalProperty);
	EXPECT_EQ(store->findPageLocation(original, node.page_id), originalEntityPage);
	EXPECT_EQ(store->findPageLocation(original, originalPropertyPage.page_id), originalPropertyPage);
	EXPECT_EQ(store->findNodePageLocation(current, node.entity_id), node);
	EXPECT_EQ(store->findEntityPropertyLocation(current, propertyKey),
			  (EntityPropertyLocation{propertyKey, originalPropertyPage.page_id,
									  static_cast<uint16_t>(originalProperty.slot_id + 1U), 2U}));
	EXPECT_NE(store->findPageLocation(current, node.page_id).page_index, originalEntityPage.page_index);
	EXPECT_NE(store->findPageLocation(current, originalPropertyPage.page_id).page_index,
			  originalPropertyPage.page_index);

	EntityPageReplacementRequest deletedNode = entityReplacement;
	deletedNode.expected_location = store->findPageLocation(current, node.page_id);
	deletedNode.erased_node_ids = {node.entity_id};
	NativeGraphPropertyPageReplacementRequest duplicatePropertyDeletion = propertyReplacement;
	duplicatePropertyDeletion.expected_location = store->findPageLocation(current, originalPropertyPage.page_id);
	duplicatePropertyDeletion.expected_owner_lsn = LogSequenceNumber(5U);
	duplicatePropertyDeletion.upserted_properties.clear();
	// Entity deletion also removes its owner-keyed Property directory entry. That
	// automatic cleanup is intentionally coalesced with one explicit page erase;
	// reject only a duplicate explicit key in the same Property request.
	duplicatePropertyDeletion.erased_properties = {propertyKey, propertyKey};
	bool rejectedPreparer = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPages(
						std::span(&deletedNode, 1), {}, {}, std::span(&duplicatePropertyDeletion, 1), {},
						LogSequenceNumber(6U),
						[&rejectedPreparer](std::span<EntityPageReplacementMaterialization>,
											std::span<EntityPageAppendBatchMaterialization>,
											std::span<EntityPageNewPoolBatchMaterialization>,
											std::span<NativeGraphPropertyPageReplacementMaterialization>,
											std::span<NativeGraphPropertyPageBatchMaterialization>) {
							rejectedPreparer = true;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(rejectedPreparer);
	MetadataSnapshotPin afterRejected = store->openSnapshot();
	EXPECT_EQ(afterRejected.superblock(), current.superblock());
	EXPECT_EQ(store->findNodePageLocation(afterRejected, node.entity_id), node);
	EXPECT_EQ(store->findEntityPropertyLocation(afterRejected, propertyKey),
			  store->findEntityPropertyLocation(current, propertyKey));
}

TEST_F(RelocatableMetadataStoreTest, NativeGraphPageTransactionPublishesOnlyCompleteEntityAndPropertyGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest entityRequest = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	entityRequest.require_absent = true;
	const std::optional<CatalogSlotReservations> entityReservation = store->reserveFirstFreeAndUpsertCatalog(
			std::span(&entityRequest, 1), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U),
			[](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(entityReservation.has_value());
	const EntityPageLocation node{101U, PageId(91U), 0U, 1U};
	PageLocation entityPageLocation{node.page_id, entityReservation->entries.front().segment_id, 0U,
									entityReservation->entries.front().generation, 0U};
	static_cast<void>(store->appendEntityPageToExistingPool(
			entityReservation->entries.front(), entityPageLocation, std::span(&node, 1), {}, LogSequenceNumber(3U),
			[](const CatalogEntry &, PageLocation &page) { page.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME; }));

	const EntityPropertyKey propertyKey{EntityPropertyOwnerKind::EPOK_NODE, node.entity_id};
	const std::optional<EntityPropertyPagePoolMaterialization> propertyMaterialization =
			store->materializeEntityPropertyPagePool(
					701U, propertyKey, PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(4U),
					[](const CatalogEntry &pool, PageId pageId, PageLocation &page, EntityPropertyLocation &property) {
						page = {pageId, pool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, pool.generation, 0U};
						property = {property.key, pageId, 0U, 1U};
					});
	ASSERT_TRUE(propertyMaterialization.has_value());
	const MetadataSnapshotPin original = store->openSnapshot();
	const PageLocation originalEntityPage = store->findPageLocation(original, node.page_id);
	const PageLocation originalPropertyPage = store->findPageLocation(original, propertyMaterialization->page.page_id);
	const EntityPropertyLocation originalProperty = store->findEntityPropertyLocation(original, propertyKey);

	EntityPageReplacementRequest entityReplacement;
	entityReplacement.expected_pool = entityReservation->entries.front();
	entityReplacement.expected_location = originalEntityPage;
	NativeGraphPropertyPageReplacementRequest propertyReplacement;
	propertyReplacement.expected_pool = propertyMaterialization->pool;
	propertyReplacement.expected_location = originalPropertyPage;
	propertyReplacement.expected_page_owner_id = 701U;
	propertyReplacement.expected_owner_lsn = LogSequenceNumber(4U);
	propertyReplacement.upserted_properties = {{propertyKey, originalPropertyPage.page_id, 1U, 2U}};
	const auto prepareBothPages = [](std::span<EntityPageReplacementMaterialization> entities,
										std::span<EntityPageAppendBatchMaterialization> appends,
										std::span<EntityPageNewPoolBatchMaterialization> newPools,
										std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
										std::span<NativeGraphPropertyPageBatchMaterialization> propertyPools) {
		ASSERT_EQ(entities.size(), 1U);
		EXPECT_TRUE(appends.empty());
		EXPECT_TRUE(newPools.empty());
		ASSERT_EQ(properties.size(), 1U);
		EXPECT_TRUE(propertyPools.empty());
		entities.front().replacement.page_index =
				static_cast<uint8_t>(entities.front().expected_location.page_index + 1U);
		properties.front().replacement.page_index =
				static_cast<uint8_t>(properties.front().expected_location.page_index + 1U);
	};

	EXPECT_THROW(static_cast<void>(store->mutateNativeGraphPages(
					 std::span(&entityReplacement, 1), {}, {}, std::span(&propertyReplacement, 1), {},
					 LogSequenceNumber(5U), prepareBothPages,
					 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
			 BootstrapPublishInterrupted);
	MetadataSnapshotPin beforePublication = store->openSnapshot();
	EXPECT_EQ(beforePublication.superblock(), original.superblock());
	EXPECT_EQ(store->findPageLocation(beforePublication, node.page_id), originalEntityPage);
	EXPECT_EQ(store->findPageLocation(beforePublication, originalPropertyPage.page_id), originalPropertyPage);
	EXPECT_EQ(store->findNodePageLocation(beforePublication, node.entity_id), node);
	EXPECT_EQ(store->findEntityPropertyLocation(beforePublication, propertyKey), originalProperty);
	beforePublication.release();

	EXPECT_THROW(static_cast<void>(store->mutateNativeGraphPages(
					 std::span(&entityReplacement, 1), {}, {}, std::span(&propertyReplacement, 1), {},
					 LogSequenceNumber(5U), prepareBothPages,
					 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
			 BootstrapPublishInterrupted);
	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin published = reopened->openSnapshot();
	EXPECT_EQ(published.superblock().generation, Generation(5U));
	EXPECT_NE(reopened->findPageLocation(published, node.page_id).page_index, originalEntityPage.page_index);
	EXPECT_NE(reopened->findPageLocation(published, originalPropertyPage.page_id).page_index,
			  originalPropertyPage.page_index);
	EXPECT_EQ(reopened->findNodePageLocation(published, node.entity_id), node);
	EXPECT_EQ(reopened->findEntityPropertyLocation(published, propertyKey),
			  (EntityPropertyLocation{propertyKey, originalPropertyPage.page_id, 1U, 2U}));
}

TEST_F(RelocatableMetadataStoreTest, NativeGraphPropertyPoolBecomesReachableOnlyWithPublishedGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest entityRequest = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	entityRequest.require_absent = true;
	const std::optional<CatalogSlotReservations> entityReservation = store->reserveFirstFreeAndUpsertCatalog(
			std::span(&entityRequest, 1), PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(2U),
			[](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(entityReservation.has_value());
	const EntityPageLocation node{101U, PageId(91U), 0U, 1U};
	PageLocation entityPageLocation{node.page_id, entityReservation->entries.front().segment_id, 0U,
									entityReservation->entries.front().generation, 0U};
	static_cast<void>(store->appendEntityPageToExistingPool(
			entityReservation->entries.front(), entityPageLocation, std::span(&node, 1), {}, LogSequenceNumber(3U),
			[](const CatalogEntry &, PageLocation &page) { page.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME; }));
	const EntityPropertyKey propertyKey{EntityPropertyOwnerKind::EPOK_NODE, 101U};
	NativeGraphPropertyPageBatchRequest request;
	request.page_owner_id = 701U;
	request.first_slot = PhysicalSlotId(40U);
	request.last_slot = PhysicalSlotId(40U);
	request.page_property_keys = {{propertyKey}};
	std::optional<NativeGraphPropertyPageBatchMaterialization> candidate;
	const auto prepareNewPool = [&candidate](std::span<EntityPageReplacementMaterialization> entities,
										  std::span<EntityPageAppendBatchMaterialization> appends,
										  std::span<EntityPageNewPoolBatchMaterialization> newPools,
										  std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
										  std::span<NativeGraphPropertyPageBatchMaterialization> propertyPools) {
		EXPECT_TRUE(entities.empty());
		EXPECT_TRUE(appends.empty());
		EXPECT_TRUE(newPools.empty());
		EXPECT_TRUE(properties.empty());
		ASSERT_EQ(propertyPools.size(), 1U);
		ASSERT_EQ(propertyPools.front().pages.size(), 1U);
		auto &initial = propertyPools.front().pages.front();
		initial.location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
		initial.upserted_properties.front() =
				{initial.upserted_properties.front().key, initial.location.page_id, 0U, 1U};
		candidate = propertyPools.front();
	};

	EXPECT_THROW(static_cast<void>(store->mutateNativeGraphPages(
					 {}, {}, {}, {}, std::span(&request, 1), LogSequenceNumber(4U), prepareNewPool,
					 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
			 BootstrapPublishInterrupted);
	ASSERT_TRUE(candidate.has_value());
	MetadataSnapshotPin beforePublication = store->openSnapshot();
	EXPECT_EQ(beforePublication.superblock().generation, Generation(3U));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(beforePublication, PhysicalSlotId(40U)));
	EXPECT_FALSE(store->findPageLocationOptional(beforePublication, candidate->pages.front().location.page_id)
					 .has_value());
	EXPECT_FALSE(store->findEntityPropertyLocationOptional(beforePublication, propertyKey).has_value());
	expectFormatError([&] { static_cast<void>(store->findCatalog(beforePublication, candidate->pool.segment_id)); },
				  FormatError::FERR_INVALID_STATE);
	beforePublication.release();

	candidate.reset();
	EXPECT_THROW(static_cast<void>(store->mutateNativeGraphPages(
					 {}, {}, {}, {}, std::span(&request, 1), LogSequenceNumber(4U), prepareNewPool,
					 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
			 BootstrapPublishInterrupted);
	ASSERT_TRUE(candidate.has_value());
	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin published = reopened->openSnapshot();
	EXPECT_EQ(published.superblock().generation, Generation(4U));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(published, PhysicalSlotId(40U)));
	const CatalogEntry visiblePool = reopened->findCatalog(published, candidate->pool.segment_id);
	EXPECT_EQ(visiblePool.segment_id, candidate->pool.segment_id);
	EXPECT_EQ(visiblePool.physical_slot, candidate->pool.physical_slot);
	EXPECT_EQ(visiblePool.kind, candidate->pool.kind);
	EXPECT_EQ(visiblePool.owner_id, candidate->pool.owner_id);
	EXPECT_EQ(visiblePool.generation, candidate->pool.generation);
	EXPECT_EQ(visiblePool.state, candidate->pool.state);
	EXPECT_EQ(visiblePool.committed_lsn, candidate->pool.committed_lsn);
	EXPECT_EQ(reopened->findPageLocation(published, candidate->pages.front().location.page_id),
			  candidate->pages.front().location);
	EXPECT_EQ(reopened->findEntityPropertyLocation(published, propertyKey),
			  candidate->pages.front().upserted_properties.front());
}

TEST_F(RelocatableMetadataStoreTest, NativeGraphTransactionRejectsPropertyRootWithoutSurvivingEntityOwner) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const EntityPropertyKey missingOwner{EntityPropertyOwnerKind::EPOK_NODE, 101U};
	NativeGraphPropertyPageBatchRequest request;
	request.page_owner_id = 701U;
	request.first_slot = PhysicalSlotId(40U);
	request.last_slot = PhysicalSlotId(40U);
	request.page_property_keys = {{missingOwner}};
	bool prepared = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPages(
						{}, {}, {}, {}, std::span(&request, 1), LogSequenceNumber(2U),
						[&prepared](std::span<EntityPageReplacementMaterialization>,
									std::span<EntityPageAppendBatchMaterialization>,
									std::span<EntityPageNewPoolBatchMaterialization>,
									std::span<NativeGraphPropertyPageReplacementMaterialization>,
									std::span<NativeGraphPropertyPageBatchMaterialization>) { prepared = true; }));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(prepared);
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(current.superblock().generation, Generation(1U));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(40U)));
	EXPECT_FALSE(store->findEntityPropertyLocationOptional(current, missingOwner).has_value());
}
