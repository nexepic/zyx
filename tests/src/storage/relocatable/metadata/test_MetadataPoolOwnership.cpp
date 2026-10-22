/**
 * @file test_MetadataPoolOwnership.cpp
 * @brief Direct recovery and publication tests for metadata-pool ownership.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataPoolOwnership.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/debug/PerfTrace.hpp"

namespace {

	class MetadataPoolOwnershipComponents final {
	public:
		MetadataPoolOwnershipComponents(std::shared_ptr<StorageIO> storage, const Superblock &current, bool readOnly,
										MetadataPagePoolOpenMode openMode = MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER) :
			storage_(std::move(storage)), pages(storage_, current), pools(storage_, current, openMode), catalog(pages, pools),
			allocator(pages, pools), ownership(catalog, allocator, pools, readOnly) {}

		std::shared_ptr<StorageIO> storage_;
		MetadataPageFile pages;
		MetadataPagePoolAllocator pools;
		CatalogTree catalog;
		AllocatorDirectory allocator;
		MetadataPoolOwnership ownership;
	};

	[[nodiscard]] Superblock initializeAndRecover(const std::shared_ptr<StorageIO> &io) {
		static_cast<void>(RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U)));
		SuperblockFile bootstrap(io);
		return bootstrap.recover();
	}

	[[nodiscard]] PhysicalPageRef appendUnownedMetadataPool(MetadataPagePoolAllocator &pools) {
		PhysicalPageRef page;
		while (!pools.isMetadataPoolSlot(PhysicalSlotId(3U))) {
			page = pools.allocatePage(Generation(1U));
		}
		return page;
	}

	[[nodiscard]] Superblock publishCatalogRootWithMetadataPool(const std::shared_ptr<StorageIO> &io) {
		auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		const CatalogEntry catalogEntry = entry(40U, 1U, 2U);
		static_cast<void>(store->upsertCatalog(std::span(&catalogEntry, 1U), LogSequenceNumber(2U)));
		MetadataSnapshotPin snapshot = store->openSnapshot();
		const Superblock current = snapshot.superblock();
		snapshot.release();
		return current;
	}

	[[nodiscard]] Superblock publishCatalogWithObsoleteMetadataPool(const std::shared_ptr<StorageIO> &io,
																 uint64_t obsoleteOwnerId) {
		const Superblock initial = initializeAndRecover(io);
		SuperblockFile bootstrap(io);
		MetadataPageFile pages(io, initial);
		MetadataPagePoolAllocator pools(io, initial);
		CatalogTree catalog(pages, pools);
		CatalogEntry bootstrapPool;
		bootstrapPool.segment_id = SegmentId(1U);
		bootstrapPool.physical_slot = PhysicalSlotId(1U);
		bootstrapPool.kind = SegmentKind::SGK_METADATA_PAGE_POOL;
		bootstrapPool.owner_id = bootstrapPool.physical_slot.value();
		bootstrapPool.generation = Generation(1U);
		bootstrapPool.state = SegmentState::SGS_SEALED;
		bootstrapPool.committed_lsn = LogSequenceNumber(1U);
		CatalogEntry obsoletePool = bootstrapPool;
		obsoletePool.segment_id = SegmentId(100U);
		obsoletePool.physical_slot = PhysicalSlotId(4U);
		obsoletePool.owner_id = obsoleteOwnerId;
		obsoletePool.state = SegmentState::SGS_OBSOLETE;
		obsoletePool.committed_lsn = LogSequenceNumber(2U);
		return catalog.upsertAndPublish(bootstrap, initial, std::array{bootstrapPool, obsoletePool}, LogSequenceNumber(2U));
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest, MetadataPoolOwnershipDirectlyRepairsOnlyAnEmptyUnpublishedPool) {
	const Superblock current = initializeAndRecover(io);
	MetadataPagePoolAllocator writerPools(io, current);
	const PhysicalPageRef orphan = appendUnownedMetadataPool(writerPools);
	ASSERT_EQ(orphan.physical_slot, PhysicalSlotId(3U));
	writerPools.abandonPage(orphan);
	io->sync();

	MetadataPoolOwnershipComponents components(io, current, false, MetadataPagePoolOpenMode::CPOM_RECOVER);
	ASSERT_TRUE(components.pools.isMetadataPoolSlot(orphan.physical_slot));
	components.ownership.verifyAfterOpen(current);
	EXPECT_FALSE(components.pools.isMetadataPoolSlot(orphan.physical_slot));
}

TEST_F(RelocatableMetadataStoreTest, MetadataPoolOwnershipRejectsReadOnlyUnownedPoolRepair) {
	const Superblock current = initializeAndRecover(io);
	MetadataPagePoolAllocator writerPools(io, current);
	const PhysicalPageRef orphan = appendUnownedMetadataPool(writerPools);
	ASSERT_EQ(orphan.physical_slot, PhysicalSlotId(3U));
	writerPools.abandonPage(orphan);
	io->sync();

	MetadataPoolOwnershipComponents readOnlyComponents(io, current, true, MetadataPagePoolOpenMode::CPOM_RECOVER);
	expectFormatError([&] { readOnlyComponents.ownership.verifyAfterOpen(current); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, MetadataPoolOwnershipRejectsReachableUnownedPool) {
	const Superblock current = initializeAndRecover(io);
	MetadataPagePoolAllocator writerPools(io, current);
	const PhysicalPageRef reachable = appendUnownedMetadataPool(writerPools);
	ASSERT_EQ(reachable.physical_slot, PhysicalSlotId(3U));

	io->sync();
	MetadataPoolOwnershipComponents writableComponents(io, current, false, MetadataPagePoolOpenMode::CPOM_DISCOVER_ONLY);
	EXPECT_FALSE(writableComponents.allocator.isAllocated(current, reachable.physical_slot));
	EXPECT_FALSE(writableComponents.pools.isMetadataPoolEmpty(reachable.physical_slot));
	expectFormatError([&] { writableComponents.ownership.verifyAfterOpen(current); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, MetadataPoolOwnershipRejectsReserveHeaderMismatchDuringDirectRestore) {
	const Superblock current = publishCatalogRootWithMetadataPool(io);
	MetadataPoolOwnershipComponents components(io, current, false);
	const PhysicalSlotId reserveSlot = current.metadata_emergency_reserve.physical_slot;
	MetadataPagePoolHeader malformed = components.pools.poolHeader(reserveSlot);
	malformed.segment_id = SegmentId(malformed.segment_id.value() + 1U);
	components.pools.replaceRegisteredPoolHeaderForTesting(reserveSlot, malformed);

	expectFormatError([&] { components.ownership.restoreCatalogOwnership(current); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, MetadataPoolOwnershipAcceptsAnObsoleteMetadataPoolEntry) {
	const Superblock current = publishCatalogWithObsoleteMetadataPool(io, PhysicalSlotId(4U).value());
	MetadataPoolOwnershipComponents components(io, current, false);
	EXPECT_NO_THROW(components.ownership.restoreCatalogOwnership(current));
}

TEST_F(RelocatableMetadataStoreTest, MetadataPoolOwnershipRejectsAnObsoleteMetadataPoolWithWrongIdentity) {
	const Superblock current = publishCatalogWithObsoleteMetadataPool(io, PhysicalSlotId(5U).value());
	MetadataPoolOwnershipComponents components(io, current, false);
	expectFormatError([&] { components.ownership.restoreCatalogOwnership(current); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, MetadataPoolOwnershipRejectsNewPoolThatCollidesWithExistingCatalogIdentity) {
	const Superblock current = publishCatalogRootWithMetadataPool(io);
	MetadataPoolOwnershipComponents components(io, current, false);
	MetadataWriteBatch batch(components.pools, current, LogSequenceNumber(3U));
	while (batch.createdPoolCount() == 0U) {
		static_cast<void>(batch.allocatePage());
	}
	const PhysicalSlotId createdSlot = batch.createdPoolSlot(0U);
	MetadataPagePoolHeader colliding = components.pools.poolHeader(createdSlot);
	colliding.segment_id = SegmentId(1U);
	components.pools.replaceRegisteredPoolHeaderForTesting(createdSlot, colliding);

	expectFormatError([&] { components.ownership.preparePublication(batch); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, MetadataPoolOwnershipRejectsPublishedPoolMissingFromTheCatalog) {
	const Superblock current = publishCatalogRootWithMetadataPool(io);
	MetadataPoolOwnershipComponents components(io, current, false);
	while (!components.pools.isMetadataPoolSlot(PhysicalSlotId(3U))) {
		static_cast<void>(components.pools.allocatePage(Generation(2U)));
	}
	MetadataWriteBatch batch(components.pools, current, LogSequenceNumber(3U));
	expectFormatError([&] { components.ownership.preparePublication(batch); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, MetadataPoolOwnershipRejectsReserveMismatchDuringPublication) {
	const Superblock current = publishCatalogRootWithMetadataPool(io);
	MetadataPoolOwnershipComponents components(io, current, false);
	const PhysicalSlotId reserveSlot = current.metadata_emergency_reserve.physical_slot;
	MetadataPagePoolHeader malformed = components.pools.poolHeader(reserveSlot);
	malformed.committed_lsn = LogSequenceNumber(malformed.committed_lsn.value() + 1U);
	components.pools.replaceRegisteredPoolHeaderForTesting(reserveSlot, malformed);
	MetadataWriteBatch batch(components.pools, current, LogSequenceNumber(3U));

	expectFormatError([&] { components.ownership.preparePublication(batch); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, MetadataPoolOwnershipPromotesTheConsumedEmergencyReserveInOneBatch) {
	const Superblock current = publishCatalogRootWithMetadataPool(io);
	MetadataPoolOwnershipComponents components(io, current, false);
	MetadataWriteBatch batch(components.pools, current, LogSequenceNumber(3U));
	while (batch.createdPoolCount() == 0U) {
		static_cast<void>(batch.allocatePage());
	}
	const PhysicalSlotId successorSlot = batch.createdPoolSlot(0U);
	const MetadataPagePoolHeader successorHeader = components.pools.poolHeader(successorSlot);
	MetadataEmergencyReserveDescriptor successor;
	successor.physical_slot = successorSlot;
	successor.pool_token = successorHeader.segment_id;
	successor.generation = successorHeader.generation;
	successor.committed_lsn = successorHeader.committed_lsn;
	ASSERT_TRUE(successor.isValid());
	batch.setMetadataEmergencyReserve(successor);

	EXPECT_NO_THROW(components.ownership.preparePublication(batch));
	EXPECT_EQ(batch.candidate().metadata_emergency_reserve, successor);
}

TEST_F(RelocatableMetadataStoreTest, MetadataPoolOwnershipSkipsAnObsoletePoolReleasedInTheCandidateGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	static_cast<void>(store->upsertCatalog(entriesThatRequireAnotherMetadataPool(), LogSequenceNumber(2U)));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();
	store.reset();

	MetadataPoolOwnershipComponents components(io, current, false);
	CatalogEntry released;
	components.catalog.visit(current, [&released](const CatalogEntry &candidate) {
		if (candidate.kind == SegmentKind::SGK_METADATA_PAGE_POOL && candidate.physical_slot.value() > 2U) {
			released = candidate;
		}
	});
	ASSERT_TRUE(released.segment_id.isValid());
	MetadataWriteBatch batch(components.pools, current, LogSequenceNumber(3U));
	released.state = SegmentState::SGS_OBSOLETE;
	components.catalog.prepareUpsert(batch, std::span(&released, 1U));
	components.allocator.prepareSetAllocated(batch, released.physical_slot, false);

	EXPECT_NO_THROW(components.ownership.preparePublication(batch));
}

TEST_F(RelocatableMetadataStoreTest, MetadataPoolOwnershipVisitsTouchedPoolsInsteadOfEveryPublishedPool) {
    for (const uint64_t leafCount : {900U}) {
        auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
        static_cast<void>(store->upsertCatalog(catalogEntriesForLeafCount(leafCount), LogSequenceNumber(2U)));
        auto pin = store->openSnapshot(); const auto current = pin.superblock(); pin.release(); store.reset();
        MetadataPoolOwnershipComponents components(io, current, false);
        components.ownership.restoreCatalogOwnership(current);
        MetadataWriteBatch batch(components.pools, current, LogSequenceNumber(3U));
        static_cast<void>(batch.allocatePage());
        const auto totalPools = components.pools.poolSlots().size();
        graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset();
        EXPECT_NO_THROW(components.ownership.preparePublication(batch));
        const auto counters = graph::debug::PerfTrace::snapshotAndReset();
        graph::debug::PerfTrace::setValueCollectionEnabled(false);
        EXPECT_FALSE(counters.contains("native_metadata.ownership.initial_pool_enumerations"));
        ASSERT_TRUE(counters.contains("native_metadata.ownership.visited_pools"));
        const auto visited = counters.at("native_metadata.ownership.visited_pools").totalValue;
        EXPECT_LE(visited, 3);
        EXPECT_LT(visited, totalPools + 1U);
        RecordProperty("total_pools_" + std::to_string(leafCount), std::to_string(totalPools));
        RecordProperty("visited_pools_" + std::to_string(leafCount), std::to_string(visited));
    }
}

TEST_F(RelocatableMetadataStoreTest, UnconfirmedOwnershipSurvivesBitmapChangesAndClearsOnAbandonment) {
    const Superblock current = initializeAndRecover(io);
    MetadataPagePoolAllocator pools(io, current);
    const auto orphan = appendUnownedMetadataPool(pools);
    auto contains = [&](PhysicalSlotId slot) {
        const auto pending = pools.unconfirmedOwnershipSlots();
        return std::ranges::find(pending, slot) != pending.end();
    };
    EXPECT_TRUE(contains(orphan.physical_slot));
    pools.abandonPage(orphan);
    EXPECT_TRUE(contains(orphan.physical_slot));
    pools.abandonUnpublishedPool(orphan.physical_slot);
    EXPECT_FALSE(contains(orphan.physical_slot));
}
