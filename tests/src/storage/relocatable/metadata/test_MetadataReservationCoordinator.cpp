/**
 * @file test_MetadataReservationCoordinator.cpp
 * @brief Direct validation for atomic Catalog-slot reservation coordination.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>
#include <limits>

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataReservationCoordinator.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/debug/PerfTrace.hpp"

namespace {

	class MetadataReservationCoordinatorComponents final {
	public:
		MetadataReservationCoordinatorComponents(std::shared_ptr<StorageIO> storage, const Superblock &current) :
			storage_(std::move(storage)), bootstrap(storage_), pages(storage_, current),
			pools(storage_, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER), catalog(pages, pools),
			locations(pages, pools), allocator(pages, pools), coordinator(catalog, allocator, locations) {}

		std::shared_ptr<StorageIO> storage_;
		SuperblockFile bootstrap;
		MetadataPageFile pages;
		MetadataPagePoolAllocator pools;
		CatalogTree catalog;
		LocationDirectory locations;
		AllocatorDirectory allocator;
		MetadataReservationCoordinator coordinator;
	};

	[[nodiscard]] Superblock initializeAndRecoverReservationStore(const std::shared_ptr<StorageIO> &io) {
		static_cast<void>(RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U)));
		SuperblockFile bootstrap(io);
		return bootstrap.recover();
	}

	[[nodiscard]] CatalogReservationPublicationRequest reservationInput(
			std::span<const CatalogSlotReservationRequest> reservations, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot) {
		CatalogReservationPublicationRequest input;
		input.reservations = reservations;
		input.first_slot = firstSlot;
		input.last_slot = lastSlot;
		return input;
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest, MetadataReservationCoordinatorStagesCatalogAllocatorAndPageDirectoryAtomically) {
	const Superblock current = initializeAndRecoverReservationStore(io);
	MetadataReservationCoordinatorComponents components(io, current);
	CatalogSlotReservationRequest pool = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 2U);
	pool.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	pool.owner_id = 77U;
	pool.require_absent = true;
	const PageLocation page{PageId(91U), pool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, pool.segment_generation, 100U};
	MetadataWriteBatch batch(components.pools, current, LogSequenceNumber(2U));
	CatalogReservationPublicationRequest input =
			reservationInput(std::span(&pool, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U));
	input.pages = std::span(&page, 1U);
	bool prepared = false;
	const CatalogSlotReservationPreparer dataPreparer = [&prepared](std::span<const CatalogEntry> entries) {
		ASSERT_EQ(entries.size(), 1U);
		EXPECT_EQ(entries.front().physical_slot, PhysicalSlotId(40U));
		prepared = true;
	};
	input.data_preparer = &dataPreparer;

	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset();
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	const std::optional<std::vector<CatalogEntry>> entries = components.coordinator.prepare(batch, input);
	const graph::debug::PerfTrace::Snapshot trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(entries.has_value());
	ASSERT_TRUE(prepared);
	ASSERT_EQ(entries->size(), 1U);
	EXPECT_EQ(entries->front().segment_id, pool.segment_id);
	EXPECT_EQ(entries->front().physical_slot, PhysicalSlotId(40U));
	EXPECT_TRUE(components.allocator.isAllocated(batch.candidate(), PhysicalSlotId(40U)));
	EXPECT_EQ(components.catalog.find(batch.candidate(), pool.segment_id).physical_slot, PhysicalSlotId(40U));
	EXPECT_EQ(components.locations.findPage(batch.candidate(), page.page_id), page);
	ASSERT_TRUE(trace.contains("native_graph.commit.metadata_reservation_page_index_builds"));
	EXPECT_EQ(trace.at("native_graph.commit.metadata_reservation_page_index_builds").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.commit.metadata_reservation_page_index_entries"));
	EXPECT_EQ(trace.at("native_graph.commit.metadata_reservation_page_index_entries").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.commit.directory_publication_pages"));
	EXPECT_GT(trace.at("native_graph.commit.directory_publication_pages").totalValue, 0);
}

TEST_F(RelocatableMetadataStoreTest, MetadataReservationCoordinatorRejectsMalformedRequestsBeforeAllocation) {
	const Superblock current = initializeAndRecoverReservationStore(io);
	MetadataReservationCoordinatorComponents components(io, current);
	MetadataWriteBatch batch(components.pools, current, LogSequenceNumber(2U));
	const std::array<CatalogSlotReservationRequest, 0> none{};
	expectFormatError(
			[&] {
				static_cast<void>(
						components.coordinator.prepare(batch, reservationInput(none, PhysicalSlotId(40U), PhysicalSlotId(40U))));
			},
			FormatError::FERR_INVALID_HEADER);

	CatalogSlotReservationRequest first = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U, 2U);
	CatalogSlotReservationRequest second = first;
	second.segment_id = first.segment_id;
	const std::array duplicates{first, second};
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.prepare(
						batch, reservationInput(duplicates, PhysicalSlotId(40U), PhysicalSlotId(41U))));
			},
			FormatError::FERR_INVALID_HEADER);

	CatalogSlotReservationRequest metadataPool = first;
	metadataPool.kind = SegmentKind::SGK_METADATA_PAGE_POOL;
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.prepare(
						batch, reservationInput(std::span(&metadataPool, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U))));
			},
			FormatError::FERR_INVALID_HEADER);

	const CatalogSlotReservationPreparer dataPreparer = [](std::span<const CatalogEntry>) {};
	const EntityPagePoolReservationPreparer entityPreparer = [](std::span<const CatalogEntry>, std::span<PageLocation>) {};
	CatalogReservationPublicationRequest badCallbacks =
			reservationInput(std::span(&first, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U));
	badCallbacks.data_preparer = &dataPreparer;
	badCallbacks.entity_page_preparer = &entityPreparer;
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, badCallbacks)); },
					  FormatError::FERR_INVALID_HEADER);

	PageLocation mutablePage{PageId(99U), first.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, first.segment_generation};
	CatalogReservationPublicationRequest strayMutable =
			reservationInput(std::span(&first, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U));
	strayMutable.mutable_entity_pages = std::span(&mutablePage, 1U);
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, strayMutable)); },
					  FormatError::FERR_INVALID_HEADER);

	CatalogReservationPublicationRequest mismatchedMutable =
			reservationInput(std::span(&first, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U));
	mismatchedMutable.pages = std::span(&mutablePage, 1U);
	mismatchedMutable.entity_page_preparer = &entityPreparer;
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, mismatchedMutable)); },
					  FormatError::FERR_INVALID_HEADER);

	const EntityPageLocation node{1U, mutablePage.page_id, 0U, 1U};
	const EntityPageLocation edge{2U, mutablePage.page_id, 0U, 1U};
	CatalogReservationPublicationRequest mixedEntities =
			reservationInput(std::span(&first, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U));
	mixedEntities.entity_page_nodes = std::span(&node, 1U);
	mixedEntities.entity_page_edges = std::span(&edge, 1U);
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, mixedEntities)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MetadataReservationCoordinatorRejectsForeignOrWrongTypedLocationTargets) {
	const Superblock current = initializeAndRecoverReservationStore(io);
	MetadataReservationCoordinatorComponents components(io, current);
	CatalogSlotReservationRequest raw = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U, 2U);
	raw.kind = SegmentKind::SGK_VECTOR_RAW_POOL;
	raw.owner_id = 8U;
	const PageLocation rawPage{PageId(11U), raw.segment_id, 0U, raw.segment_generation, 0U};
	MetadataWriteBatch batch(components.pools, current, LogSequenceNumber(2U));

	CatalogReservationPublicationRequest unrequested =
			reservationInput(std::span(&raw, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U));
	PageLocation foreign = rawPage;
	foreign.segment_id = SegmentId(raw.segment_id.value() + 1U);
	unrequested.pages = std::span(&foreign, 1U);
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, unrequested)); },
					  FormatError::FERR_INVALID_STATE);
	ExtentLocation foreignExtent{ExtentId(1U), foreign.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, 1U,
									 foreign.generation};
	CatalogReservationPublicationRequest unrequestedExtent =
			reservationInput(std::span(&raw, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U));
	unrequestedExtent.extents = std::span(&foreignExtent, 1U);
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, unrequestedExtent)); },
					  FormatError::FERR_INVALID_STATE);

	CatalogReservationPublicationRequest entityTarget =
			reservationInput(std::span(&raw, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U));
	entityTarget.pages = std::span(&rawPage, 1U);
	const EntityPageLocation entity{19U, rawPage.page_id, 0U, 1U};
	entityTarget.entity_page_nodes = std::span(&entity, 1U);
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, entityTarget)); },
					  FormatError::FERR_INVALID_STATE);
	EntityPageLocation invalidEntity = entity;
	invalidEntity.entity_id = 0U;
	entityTarget.entity_page_nodes = std::span(&invalidEntity, 1U);
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, entityTarget)); },
					  FormatError::FERR_INVALID_STATE);
	invalidEntity = entity;
	invalidEntity.page_id = {};
	entityTarget.entity_page_nodes = std::span(&invalidEntity, 1U);
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, entityTarget)); },
					  FormatError::FERR_INVALID_STATE);
	invalidEntity = entity;
	invalidEntity.slot_id = std::numeric_limits<uint16_t>::max();
	entityTarget.entity_page_nodes = std::span(&invalidEntity, 1U);
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, entityTarget)); },
					  FormatError::FERR_INVALID_STATE);
	invalidEntity = entity;
	invalidEntity.slot_generation = 0U;
	entityTarget.entity_page_nodes = std::span(&invalidEntity, 1U);
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, entityTarget)); },
					  FormatError::FERR_INVALID_STATE);

	CatalogReservationPublicationRequest manifestTarget =
			reservationInput(std::span(&raw, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U));
	manifestTarget.pages = std::span(&rawPage, 1U);
	const VectorManifestLocation manifest{8U, rawPage.page_id, rawPage.generation};
	manifestTarget.manifests = std::span(&manifest, 1U);
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, manifestTarget)); },
					  FormatError::FERR_INVALID_STATE);

}

TEST_F(RelocatableMetadataStoreTest, MetadataReservationCoordinatorRejectsDuplicatePageIdsBeforeAllocation) {
	const Superblock current = initializeAndRecoverReservationStore(io);
	MetadataReservationCoordinatorComponents components(io, current);
	CatalogSlotReservationRequest first = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 10U, 2U);
	CatalogSlotReservationRequest second = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 11U, 2U);
	first.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	second.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	const std::array requests{first, second};
	const PageLocation page{PageId(201U), first.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, first.segment_generation};
	PageLocation duplicate = page;
	duplicate.segment_id = second.segment_id;
	const std::array pages{page, duplicate};
	MetadataWriteBatch batch(components.pools, current, LogSequenceNumber(2U));
	CatalogReservationPublicationRequest input =
			reservationInput(requests, PhysicalSlotId(40U), PhysicalSlotId(41U));
	input.pages = pages;
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(batch, input)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(components.allocator.isAllocated(batch.candidate(), PhysicalSlotId(40U)));
}

TEST_F(RelocatableMetadataStoreTest, MetadataReservationCoordinatorRejectsDurableIdentityCollisionsAndUnavailableSlots) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest existing = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 2U);
	existing.require_absent = true;
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(existing, PhysicalSlotId(40U), PhysicalSlotId(40U),
																				 LogSequenceNumber(2U)));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();
	store.reset();

	MetadataReservationCoordinatorComponents components(io, current);
	MetadataWriteBatch collisionBatch(components.pools, current, LogSequenceNumber(3U));
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.prepare(
						collisionBatch, reservationInput(std::span(&existing, 1U), PhysicalSlotId(41U), PhysicalSlotId(41U))));
			},
			FormatError::FERR_INVALID_STATE);

	CatalogSlotReservationRequest fresh = reservationRequest(existing.segment_id.value() + 1U, 3U);
	fresh.require_absent = true;
	MetadataWriteBatch unavailableBatch(components.pools, current, LogSequenceNumber(3U));
	EXPECT_FALSE(components.coordinator.prepare(
				 unavailableBatch, reservationInput(std::span(&fresh, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U)))
				 .has_value());
}

TEST_F(RelocatableMetadataStoreTest, MetadataReservationCoordinatorStagesEdgeEntityLocations) {
	const Superblock current = initializeAndRecoverReservationStore(io);
	MetadataReservationCoordinatorComponents components(io, current);
	CatalogSlotReservationRequest pool = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 2U);
	pool.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	pool.require_absent = true;
	PageLocation page{PageId(121U), pool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, pool.segment_generation};
	const EntityPageLocation edge{17U, page.page_id, 1U, 1U};
	MetadataWriteBatch batch(components.pools, current, LogSequenceNumber(2U));
	CatalogReservationPublicationRequest input =
			reservationInput(std::span(&pool, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U));
	input.pages = std::span(&page, 1U);
	input.entity_page_edges = std::span(&edge, 1U);
	const EntityPagePoolReservationPreparer preparer = [](std::span<const CatalogEntry>, std::span<PageLocation> pages) {
		ASSERT_EQ(pages.size(), 1U);
	};
	input.entity_page_preparer = &preparer;
	input.mutable_entity_pages = std::span(&page, 1U);
	ASSERT_TRUE(components.coordinator.prepare(batch, input).has_value());
	EXPECT_EQ(components.locations.findEdgePageLocation(batch.candidate(), edge.entity_id), edge);
}

TEST_F(RelocatableMetadataStoreTest, MetadataReservationCoordinatorRejectsExistingPageAndEntityMappings) {
	const Superblock initial = initializeAndRecoverReservationStore(io);
	MetadataReservationCoordinatorComponents components(io, initial);
	CatalogSlotReservationRequest propertyPool = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 2U);
	propertyPool.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	propertyPool.require_absent = true;
	const PageLocation originalPage{PageId(131U), propertyPool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
										 propertyPool.segment_generation};
	MetadataWriteBatch pageSeed(components.pools, initial, LogSequenceNumber(2U));
	CatalogReservationPublicationRequest pageSeedInput =
			reservationInput(std::span(&propertyPool, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U));
	pageSeedInput.pages = std::span(&originalPage, 1U);
	ASSERT_TRUE(components.coordinator.prepare(pageSeed, pageSeedInput).has_value());
	pageSeed.publish(components.bootstrap);

	CatalogSlotReservationRequest replacementPool = propertyPool;
	replacementPool.segment_id = SegmentId(propertyPool.segment_id.value() + 1U);
	const PageLocation replacementPage{originalPage.page_id, replacementPool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
											 replacementPool.segment_generation};
	MetadataWriteBatch pageCollision(components.pools, pageSeed.candidate(), LogSequenceNumber(3U));
	CatalogReservationPublicationRequest pageCollisionInput =
			reservationInput(std::span(&replacementPool, 1U), PhysicalSlotId(41U), PhysicalSlotId(41U));
	pageCollisionInput.pages = std::span(&replacementPage, 1U);
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(pageCollision, pageCollisionInput)); },
					  FormatError::FERR_INVALID_STATE);

	CatalogSlotReservationRequest entityPool = reservationRequest(replacementPool.segment_id.value() + 1U, 3U);
	entityPool.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	entityPool.require_absent = true;
	PageLocation entityPage{PageId(132U), entityPool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
							 entityPool.segment_generation};
	const EntityPageLocation originalNode{41U, entityPage.page_id, 0U, 1U};
	MetadataWriteBatch nodeSeed(components.pools, pageSeed.candidate(), LogSequenceNumber(3U));
	CatalogReservationPublicationRequest nodeSeedInput =
			reservationInput(std::span(&entityPool, 1U), PhysicalSlotId(41U), PhysicalSlotId(41U));
	nodeSeedInput.pages = std::span(&entityPage, 1U);
	nodeSeedInput.entity_page_nodes = std::span(&originalNode, 1U);
	const EntityPagePoolReservationPreparer preparer = [](std::span<const CatalogEntry>, std::span<PageLocation>) {};
	nodeSeedInput.entity_page_preparer = &preparer;
	nodeSeedInput.mutable_entity_pages = std::span(&entityPage, 1U);
	ASSERT_TRUE(components.coordinator.prepare(nodeSeed, nodeSeedInput).has_value());
	nodeSeed.publish(components.bootstrap);

	CatalogSlotReservationRequest newEntityPool = entityPool;
	newEntityPool.segment_id = SegmentId(entityPool.segment_id.value() + 1U);
	PageLocation newEntityPage{PageId(133U), newEntityPool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
								newEntityPool.segment_generation};
	const EntityPageLocation duplicateNode{originalNode.entity_id, newEntityPage.page_id, 0U, 1U};
	MetadataWriteBatch nodeCollision(components.pools, nodeSeed.candidate(), LogSequenceNumber(4U));
	CatalogReservationPublicationRequest nodeCollisionInput =
			reservationInput(std::span(&newEntityPool, 1U), PhysicalSlotId(42U), PhysicalSlotId(42U));
	nodeCollisionInput.pages = std::span(&newEntityPage, 1U);
	nodeCollisionInput.entity_page_nodes = std::span(&duplicateNode, 1U);
	expectFormatError([&] { static_cast<void>(components.coordinator.prepare(nodeCollision, nodeCollisionInput)); },
					  FormatError::FERR_INVALID_STATE);
}
