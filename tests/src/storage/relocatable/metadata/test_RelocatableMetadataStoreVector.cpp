/**
 * @file test_RelocatableMetadataStoreVector.cpp
 * @brief Direct public-contract tests for Vector metadata publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookExtent.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"

namespace graph::storage {

	// The public VectorManifestStore covers semantic materialization. This narrow
	// access shim exercises the metadata coordinator's defensive callback contract.
	class RelocatableMetadataStoreVectorTestAccess final {
	public:
		[[nodiscard]] static Superblock materialize(RelocatableMetadataStore &store, uint64_t indexId,
													PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
													LogSequenceNumber checkpointLsn,
													const NewVectorManifestPagePreparer &preparer,
													const MetadataPublishOptions &options = {}) {
			return store.materializeNewVectorManifestPage(indexId, firstSlot, lastSlot, checkpointLsn, preparer,
														  options);
		}

		[[nodiscard]] static Superblock materializeRaw(RelocatableMetadataStore &store,
													   std::span<const VectorRawMaterializationPoolRequest> pools,
													   uint64_t indexId, LogSequenceNumber checkpointLsn,
													   const VectorRawMultiPoolMaterializationPreparer &preparer,
													   const MetadataPublishOptions &options = {}) {
			return store.materializeNewVectorRawPagesAcrossPools(pools, indexId, checkpointLsn, preparer, options);
		}

		[[nodiscard]] static Superblock materializePq(RelocatableMetadataStore &store,
													  std::span<const VectorPqMaterializationPoolRequest> pools,
													  uint64_t indexId, LogSequenceNumber checkpointLsn,
													  const VectorPqMultiPoolMaterializationPreparer &preparer,
													  const MetadataPublishOptions &options = {}) {
			return store.materializeNewVectorPqPagesAcrossPools(pools, indexId, checkpointLsn, preparer, options);
		}

		[[nodiscard]] static Superblock
		materializeAdjacency(RelocatableMetadataStore &store,
							 std::span<const VectorAdjacencyMaterializationPoolRequest> pools, uint64_t indexId,
							 LogSequenceNumber checkpointLsn,
							 const VectorAdjacencyMultiPoolMaterializationPreparer &preparer,
							 const MetadataPublishOptions &options = {}) {
			return store.materializeNewVectorAdjacencyPagesAcrossPools(pools, indexId, checkpointLsn, preparer,
																	   options);
		}

		[[nodiscard]] static Superblock publishOfflineRebuild(
				RelocatableMetadataStore &store, const OfflineVectorDerivedRebuildMetadataMutation &mutation,
				LogSequenceNumber checkpointLsn, const OfflineVectorDerivedRebuildMetadataPreparer &preparer,
				const MetadataPublishOptions &options = {}) {
			return store.publishOfflineVectorDerivedRebuild(mutation, checkpointLsn, preparer, options);
		}
	};

} // namespace graph::storage

namespace {

	NewVectorManifestPagePreparer validManifestPreparer() {
		return [](const CatalogEntry &pool, PageId pageId, PageLocation &location) {
			location = {pageId, pool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, pool.generation, 0U};
		};
	}

	VectorRawMaterializationPoolRequest rawPool(PhysicalSlotId slot) { return {slot, slot, 1U}; }

	VectorPqMaterializationPoolRequest pqPool(PhysicalSlotId slot) { return {slot, slot, 1U}; }

	VectorAdjacencyMaterializationPoolRequest adjacencyPool(PhysicalSlotId slot) { return {slot, slot, 1U}; }

	CatalogSlotReservationRequest codebookReservation(uint64_t ownerId) {
		CatalogSlotReservationRequest result;
		result.segment_id = SegmentId(100U);
		result.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
		result.owner_id = ownerId;
		result.segment_generation = Generation(1U);
		result.state = SegmentState::SGS_SEALED;
		result.require_absent = true;
		return result;
	}

	ExtentLocation codebookExtent() {
		return {VectorCodebookExtentIdCodec::manifestExtentId(17U), SegmentId(100U),
				RSS_PAGE_POOL_FIRST_DATA_FRAME, RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1U)};
	}

	PageRetirement pqRetirement(PageId pageId) {
		return {{pageId, SegmentId(101U), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U},
				17U,
				PageOwnerKind::POK_VECTOR_PQ,
				LogSequenceNumber(2U)};
	}

	PageRetirement adjacencyRetirement(PageId pageId) {
		return {{pageId, SegmentId(102U), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U},
				17U | (UINT64_C(1) << 63U),
				PageOwnerKind::POK_VECTOR_ADJACENCY,
				LogSequenceNumber(2U)};
	}

	IndexTreeMutationPoolRequest mappingRetirementRequest(PageId pageId) {
		IndexTreeMutationPoolRequest result;
		result.segment_id = SegmentId(101U);
		result.expected_retirements.push_back(
				{{pageId, result.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U},
				 LogSequenceNumber(2U)});
		return result;
	}

	void assignValidOfflineRebuildFrames(OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		const auto assignDataFrames = [](auto &pools) {
			for (auto &pool: pools) {
				for (size_t ordinal = 0U; ordinal < pool.locations.size(); ++ordinal) {
					pool.locations[ordinal].page_index =
							static_cast<uint16_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + ordinal);
				}
			}
		};
		const auto assignTreeFrames = [](auto &pools) {
			for (auto &pool: pools) {
				for (size_t ordinal = 0U; ordinal < pool.appended_locations.size(); ++ordinal) {
					pool.appended_locations[ordinal].page_index =
							static_cast<uint16_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + ordinal);
				}
			}
		};
		assignDataFrames(materialization.pq_pools);
		assignDataFrames(materialization.adjacency_pools);
		assignTreeFrames(materialization.mapping_tree_pools);
		assignTreeFrames(materialization.adjacency_mapping_tree_pools);
		materialization.manifest_replacement.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest, VectorManifestMaterializationRejectsInvalidPlansBeforeReservation) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const NewVectorManifestPagePreparer valid = validManifestPreparer();
	const auto expectInvalid = [&](uint64_t indexId, PhysicalSlotId first, PhysicalSlotId last,
								   LogSequenceNumber checkpointLsn, const NewVectorManifestPagePreparer &preparer) {
		expectFormatError(
				[&] {
					static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materialize(
							*store, indexId, first, last, checkpointLsn, preparer));
				},
				FormatError::FERR_INVALID_HEADER);
	};
	expectInvalid(0U, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U), valid);
	expectInvalid(17U, PhysicalSlotId{}, PhysicalSlotId(40U), LogSequenceNumber(2U), valid);
	expectInvalid(17U, PhysicalSlotId(40U), PhysicalSlotId{}, LogSequenceNumber(2U), valid);
	expectInvalid(17U, PhysicalSlotId(41U), PhysicalSlotId(40U), LogSequenceNumber(2U), valid);
	expectInvalid(17U, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber{}, valid);
	expectInvalid(17U, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U), {});

	const MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1U));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U)));
}

TEST_F(RelocatableMetadataStoreTest, VectorManifestMaterializationRejectsIncompletePlansWithoutPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40U), true, LogSequenceNumber(2U)));
	const NewVectorManifestPagePreparer valid = validManifestPreparer();
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materialize(
						*store, 17U, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(3U), valid));
			},
			FormatError::FERR_INVALID_STATE);

	const MetadataSnapshotPin beforeInvalidCallback = store->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materialize(
						*store, 17U, PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(3U),
						[](const CatalogEntry &pool, PageId pageId, PageLocation &location) {
							location = {pageId, pool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U, pool.generation,
										0U};
						}));
			},
			FormatError::FERR_INVALID_STATE);
	const MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), beforeInvalidCallback.superblock());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(41U)));

	// A plausible location is insufficient: the callback must have initialized
	// the pool envelope and typed Manifest frame before publication.
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materialize(
						*store, 17U, PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(3U), valid));
			},
			FormatError::FERR_BUFFER_TOO_SMALL);
	const MetadataSnapshotPin afterNoOp = store->openSnapshot();
	EXPECT_EQ(afterNoOp.superblock(), beforeInvalidCallback.superblock());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(afterNoOp, PhysicalSlotId(41U)));
	EXPECT_FALSE(store->findVectorManifestOptional(afterNoOp, 17U).has_value());
}

TEST_F(RelocatableMetadataStoreTest, VectorRawMaterializationRejectsIncompletePlansWithoutPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const std::array<VectorRawMaterializationPoolRequest, 1> pools = {rawPool(PhysicalSlotId(40U))};
	const VectorRawMultiPoolMaterializationPreparer noOp = [](std::vector<VectorRawMaterializationPoolBatch> &,
															  std::vector<VectorLocation> &) {};

	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeRaw(
						*store, {}, 17U, LogSequenceNumber(2U), noOp));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeRaw(
						*store, pools, 0U, LogSequenceNumber(2U), noOp));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array<VectorRawMaterializationPoolRequest, 1> emptyPool = {
			VectorRawMaterializationPoolRequest{PhysicalSlotId(41U), PhysicalSlotId(41U), 0U}};
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeRaw(
						*store, emptyPool, 17U, LogSequenceNumber(2U), noOp));
			},
			FormatError::FERR_INVALID_HEADER);

	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40U), true, LogSequenceNumber(2U)));
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeRaw(
						*store, pools, 17U, LogSequenceNumber(3U), noOp));
			},
			FormatError::FERR_INVALID_STATE);
	const std::array<VectorRawMaterializationPoolRequest, 1> malformedPool = {rawPool(PhysicalSlotId(41U))};
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeRaw(
						*store, malformedPool, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorRawMaterializationPoolBatch> &batches, std::vector<VectorLocation> &) {
							batches.front().locations.clear();
						}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeRaw(
						*store, malformedPool, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorRawMaterializationPoolBatch> &batches,
						   std::vector<VectorLocation> &locations) {
							PageLocation &page = batches.front().locations.front();
							page.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							locations.push_back({1U, page.page_id, 0U, 1U});
						}));
			},
			FormatError::FERR_BUFFER_TOO_SMALL);
	const std::array<VectorRawMaterializationPoolRequest, 2> duplicatePagePools = {
			VectorRawMaterializationPoolRequest{PhysicalSlotId(42U), PhysicalSlotId(42U), 1U},
			VectorRawMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeRaw(
						*store, duplicatePagePools, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorRawMaterializationPoolBatch> &batches, std::vector<VectorLocation> &) {
							for (VectorRawMaterializationPoolBatch &pool: batches) {
								pool.locations.front().page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							}
							batches[1].page_ids.front() = batches[0].page_ids.front();
							batches[1].locations.front().page_id = batches[0].page_ids.front();
						}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeRaw(
						*store, malformedPool, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorRawMaterializationPoolBatch> &batches, std::vector<VectorLocation> &) {
							batches.front().locations.front().page_index = 0U;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeRaw(
						*store, malformedPool, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorRawMaterializationPoolBatch> &batches,
						   std::vector<VectorLocation> &locations) {
							PageLocation &page = batches.front().locations.front();
							page.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							locations = {{1U, page.page_id, 0U, 1U}, {2U, page.page_id, 0U, 1U}};
						}));
			},
			FormatError::FERR_INVALID_STATE);
	const std::array<VectorRawMaterializationPoolRequest, 1> duplicateFramePool = {
			VectorRawMaterializationPoolRequest{PhysicalSlotId(42U), PhysicalSlotId(42U), 2U}};
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeRaw(
						*store, duplicateFramePool, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorRawMaterializationPoolBatch> &batches, std::vector<VectorLocation> &) {
							for (PageLocation &location: batches.front().locations) {
								location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							}
						}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeRaw(
						*store, malformedPool, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorRawMaterializationPoolBatch> &batches,
						   std::vector<VectorLocation> &locations) {
							PageLocation &page = batches.front().locations.front();
							page.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							locations = {{1U, page.page_id, 0U, 1U}, {1U, page.page_id, 1U, 1U}};
						}));
			},
			FormatError::FERR_INVALID_STATE);

	const MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(2U));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(41U)));
}

TEST_F(RelocatableMetadataStoreTest, VectorPqMaterializationRejectsIncompletePlansWithoutPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const std::array<VectorPqMaterializationPoolRequest, 1> pools = {pqPool(PhysicalSlotId(40U))};
	const VectorPqMultiPoolMaterializationPreparer noOp = [](std::vector<VectorPqMaterializationPoolBatch> &) {};

	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializePq(
						*store, {}, 17U, LogSequenceNumber(2U), noOp));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializePq(
						*store, pools, 0U, LogSequenceNumber(2U), noOp));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array<VectorPqMaterializationPoolRequest, 1> emptyPool = {
			VectorPqMaterializationPoolRequest{PhysicalSlotId(41U), PhysicalSlotId(41U), 0U}};
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializePq(
						*store, emptyPool, 17U, LogSequenceNumber(2U), noOp));
			},
			FormatError::FERR_INVALID_HEADER);

	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40U), true, LogSequenceNumber(2U)));
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializePq(
						*store, pools, 17U, LogSequenceNumber(3U), noOp));
			},
			FormatError::FERR_INVALID_STATE);
	const std::array<VectorPqMaterializationPoolRequest, 1> malformedPool = {pqPool(PhysicalSlotId(41U))};
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializePq(
						*store, malformedPool, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorPqMaterializationPoolBatch> &batches) {
							batches.front().locations.clear();
						}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializePq(
						*store, malformedPool, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorPqMaterializationPoolBatch> &batches) {
							batches.front().locations.front().page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
						}));
			},
			FormatError::FERR_BUFFER_TOO_SMALL);
	const std::array<VectorPqMaterializationPoolRequest, 2> duplicatePagePools = {
			VectorPqMaterializationPoolRequest{PhysicalSlotId(42U), PhysicalSlotId(42U), 1U},
			VectorPqMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializePq(
						*store, duplicatePagePools, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorPqMaterializationPoolBatch> &batches) {
							for (VectorPqMaterializationPoolBatch &pool: batches) {
								pool.locations.front().page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							}
							batches[1].page_ids.front() = batches[0].page_ids.front();
							batches[1].locations.front().page_id = batches[0].page_ids.front();
						}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializePq(
						*store, malformedPool, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorPqMaterializationPoolBatch> &batches) {
							batches.front().locations.front().page_index = 0U;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializePq(
						*store,
						std::array<VectorPqMaterializationPoolRequest, 1>{
								VectorPqMaterializationPoolRequest{PhysicalSlotId(42U), PhysicalSlotId(42U), 2U}},
						17U, LogSequenceNumber(3U), [](std::vector<VectorPqMaterializationPoolBatch> &batches) {
							for (PageLocation &location: batches.front().locations) {
								location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							}
						}));
			},
			FormatError::FERR_INVALID_STATE);

	const MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(2U));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(41U)));
}

TEST_F(RelocatableMetadataStoreTest, VectorAdjacencyMaterializationRejectsIncompletePlansWithoutPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const std::array<VectorAdjacencyMaterializationPoolRequest, 1> pools = {adjacencyPool(PhysicalSlotId(40U))};
	const VectorAdjacencyMultiPoolMaterializationPreparer noOp =
			[](std::vector<VectorAdjacencyMaterializationPoolBatch> &) {};

	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeAdjacency(
						*store, {}, 17U, LogSequenceNumber(2U), noOp));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeAdjacency(
						*store, pools, 0U, LogSequenceNumber(2U), noOp));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array<VectorAdjacencyMaterializationPoolRequest, 1> emptyPool = {
			VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(41U), PhysicalSlotId(41U), 0U}};
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeAdjacency(
						*store, emptyPool, 17U, LogSequenceNumber(2U), noOp));
			},
			FormatError::FERR_INVALID_HEADER);

	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40U), true, LogSequenceNumber(2U)));
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeAdjacency(
						*store, pools, 17U, LogSequenceNumber(3U), noOp));
			},
			FormatError::FERR_INVALID_STATE);
	const std::array<VectorAdjacencyMaterializationPoolRequest, 1> malformedPool = {adjacencyPool(PhysicalSlotId(41U))};
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeAdjacency(
						*store, malformedPool, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorAdjacencyMaterializationPoolBatch> &batches) {
							batches.front().locations.clear();
						}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeAdjacency(
						*store, malformedPool, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorAdjacencyMaterializationPoolBatch> &batches) {
							batches.front().locations.front().page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
						}));
			},
			FormatError::FERR_BUFFER_TOO_SMALL);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeAdjacency(
						*store, malformedPool, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorAdjacencyMaterializationPoolBatch> &batches) {
							batches.front().locations.front().page_index = 0U;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	const std::array<VectorAdjacencyMaterializationPoolRequest, 2> duplicatePagePools = {
			VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(42U), PhysicalSlotId(42U), 1U},
			VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeAdjacency(
						*store, duplicatePagePools, 17U, LogSequenceNumber(3U),
						[](std::vector<VectorAdjacencyMaterializationPoolBatch> &batches) {
							for (VectorAdjacencyMaterializationPoolBatch &pool: batches) {
								pool.locations.front().page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							}
							batches[1].page_ids.front() = batches[0].page_ids.front();
							batches[1].locations.front().page_id = batches[0].page_ids.front();
						}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::materializeAdjacency(
						*store,
						std::array<VectorAdjacencyMaterializationPoolRequest, 1>{
								VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(42U), PhysicalSlotId(42U),
																		  2U}},
						17U, LogSequenceNumber(3U), [](std::vector<VectorAdjacencyMaterializationPoolBatch> &batches) {
							for (PageLocation &location: batches.front().locations) {
								location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							}
						}));
			},
			FormatError::FERR_INVALID_STATE);

	const MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(2U));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(41U)));
}

TEST_F(RelocatableMetadataStoreTest, OfflineVectorRebuildRejectsEveryPreparerAttemptToChangeWriterOwnedIdentities) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	VectorManifestStore manifests(io, *store);
	VectorManifest initialManifest;
	initialManifest.index_id = 17U;
	initialManifest.dimensions = 4U;
	static_cast<void>(
			manifests.materialize(initialManifest, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	const VectorManifest base = manifests.read(snapshot, 17U).manifest();
	const VectorManifestLocation manifestDirectory = store->findVectorManifest(snapshot, 17U);
	const PageLocation manifestLocation = store->findPageLocation(snapshot, base.page_id);
	const CatalogEntry manifestPool = store->findCatalog(snapshot, manifestLocation.segment_id);
	const Generation generation = snapshot.superblock().generation;
	snapshot.release();
	const auto assignUsableFrames = [](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		const auto assign = [](auto &pools) {
			for (auto &pool: pools) {
				for (size_t ordinal = 0U; ordinal < pool.locations.size(); ++ordinal) {
					pool.locations[ordinal].page_index =
							static_cast<uint16_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + ordinal);
				}
			}
		};
		assign(materialization.pq_pools);
		assign(materialization.adjacency_pools);
		const auto assignTree = [](auto &pools) {
			for (auto &pool: pools) {
				for (size_t ordinal = 0U; ordinal < pool.appended_locations.size(); ++ordinal) {
					pool.appended_locations[ordinal].page_index =
							static_cast<uint16_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + ordinal);
				}
			}
		};
		assignTree(materialization.mapping_tree_pools);
		assignTree(materialization.adjacency_mapping_tree_pools);
	};

	const auto rejectPreparer = [&](const OfflineVectorDerivedRebuildMetadataPreparer &preparer,
									bool reserveCodebook = false,
									FormatError expectedError = FormatError::FERR_INVALID_STATE) {
		std::array<VectorPqMaterializationPoolRequest, 1U> pqPools = {
				VectorPqMaterializationPoolRequest{PhysicalSlotId(41U), PhysicalSlotId(41U), 1U}};
		std::array<CatalogSlotReservationRequest, 1U> codebookReservations = {codebookReservation(17U)};
		std::array<ExtentLocation, 1U> codebookExtents = {codebookExtent()};
		OfflineVectorDerivedRebuildMetadataMutation mutation;
		mutation.index_id = 17U;
		mutation.expected_manifest_pool = manifestPool;
		mutation.expected_manifest_location = manifestLocation;
		mutation.expected_manifest = manifestDirectory;
		mutation.expected_manifest_owner_lsn = base.committed_lsn;
		mutation.pq_pools = pqPools;
		mutation.mapping_tree.index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(42U), PhysicalSlotId(42U), mutation.mapping_tree.index_id, 1U});
		mutation.mapping_tree.replacement_root = {0U, 0U, true};
		if (reserveCodebook) {
			mutation.codebook_first_slot = PhysicalSlotId(43U);
			mutation.codebook_last_slot = PhysicalSlotId(43U);
			mutation.codebook_reservations = codebookReservations;
			mutation.codebook_extents = codebookExtents;
		}

		expectFormatError(
				[&] {
					static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::publishOfflineRebuild(
							*store, mutation, LogSequenceNumber(3U), preparer));
				},
				expectedError);
		MetadataSnapshotPin afterFailure = store->openSnapshot();
		EXPECT_EQ(afterFailure.superblock().generation, generation);
		EXPECT_EQ(store->findVectorManifest(afterFailure, 17U), manifestDirectory);
		EXPECT_FALSE(store->findIndexRootOptional(afterFailure, mutation.mapping_tree.index_id).has_value());
		afterFailure.release();
	};

	rejectPreparer([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.codebook_extents.push_back({});
	});
	rejectPreparer(
			[](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
				materialization.codebook_extents.front().segment_id = SegmentId(101U);
			},
			true);
	rejectPreparer(
			[](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
				materialization.codebook_extents.front().physical_slot = PhysicalSlotId(44U);
			},
			true);
	rejectPreparer(
			[](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
				materialization.codebook_extents.front().kind = SegmentKind::SGK_VECTOR_PQ_POOL;
			},
			true);
	rejectPreparer(
			[](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
				materialization.codebook_extents.front().owner_id = 18U;
			},
			true);
	rejectPreparer(
			[](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
				materialization.codebook_extents.front().generation = Generation(2U);
			},
			true);
	rejectPreparer(
			[](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
				materialization.codebook_extents.front().state = SegmentState::SGS_RETIRED;
			},
			true);
	rejectPreparer(
			[](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
				materialization.codebook_extents.front().committed_lsn = LogSequenceNumber(4U);
			},
			true);
	rejectPreparer([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.mapping_root = {};
	});
	rejectPreparer([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.pq_pools.front().locations.clear();
	});
	rejectPreparer([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.mapping_tree_pools.front().appended_locations.clear();
	});
	rejectPreparer([&assignUsableFrames](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignUsableFrames(materialization);
		materialization.mapping_tree_pools.front().appended_locations.clear();
	});
	rejectPreparer([&assignUsableFrames](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignUsableFrames(materialization);
		materialization.mapping_tree_pools.front().appended_locations.front().page_index = 0U;
	});
	rejectPreparer([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.pq_retirement_pools.push_back({});
	});
	rejectPreparer([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.mapping_retirement_pools.push_back({});
	});
	rejectPreparer([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.adjacency_mapping_retirement_pools.push_back({});
	});
	rejectPreparer([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.delta_mapping_retirement_pools.push_back({});
	});
	rejectPreparer([&assignUsableFrames,
					manifestLocation](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignUsableFrames(materialization);
		materialization.manifest_replacement.page_index = manifestLocation.page_index;
	});
	// Writer-owned locations alone cannot publish derived state. None of these
	// assigned frames is durable, so authentication must fail before routes move.
	rejectPreparer(
			[&assignUsableFrames](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
				assignUsableFrames(materialization);
				materialization.manifest_replacement.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
			},
			false, FormatError::FERR_BUFFER_TOO_SMALL);

	const auto rejectAdjacencyPreparer = [&](const OfflineVectorDerivedRebuildMetadataPreparer &preparer) {
		std::array<VectorPqMaterializationPoolRequest, 1U> pqPools = {
				VectorPqMaterializationPoolRequest{PhysicalSlotId(41U), PhysicalSlotId(41U), 1U}};
		std::array<VectorAdjacencyMaterializationPoolRequest, 1U> adjacencyPools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
		OfflineVectorDerivedRebuildMetadataMutation mutation;
		mutation.index_id = 17U;
		mutation.expected_manifest_pool = manifestPool;
		mutation.expected_manifest_location = manifestLocation;
		mutation.expected_manifest = manifestDirectory;
		mutation.expected_manifest_owner_lsn = base.committed_lsn;
		mutation.pq_pools = pqPools;
		mutation.mapping_tree.index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(42U), PhysicalSlotId(42U), mutation.mapping_tree.index_id, 1U});
		mutation.mapping_tree.replacement_root = {0U, 0U, true};
		mutation.adjacency_pools = adjacencyPools;
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(44U), PhysicalSlotId(44U), mutation.adjacency_mapping_tree.index_id, 1U});
		mutation.adjacency_mapping_tree.replacement_root = {0U, 0U, true};

		expectFormatError(
				[&] {
					static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::publishOfflineRebuild(
							*store, mutation, LogSequenceNumber(3U), preparer));
				},
				FormatError::FERR_INVALID_STATE);
		MetadataSnapshotPin afterFailure = store->openSnapshot();
		EXPECT_EQ(afterFailure.superblock().generation, generation);
		EXPECT_EQ(store->findVectorManifest(afterFailure, 17U), manifestDirectory);
		EXPECT_FALSE(store->findIndexRootOptional(afterFailure, mutation.mapping_tree.index_id).has_value());
		EXPECT_FALSE(store->findIndexRootOptional(afterFailure, mutation.adjacency_mapping_tree.index_id).has_value());
	};
	rejectAdjacencyPreparer([&assignUsableFrames](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignUsableFrames(materialization);
		materialization.adjacency_pools.front().locations.clear();
	});
	rejectAdjacencyPreparer([&assignUsableFrames](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignUsableFrames(materialization);
		materialization.adjacency_pools.front().locations.front().page_index = 0U;
	});
	rejectAdjacencyPreparer([&assignUsableFrames](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignUsableFrames(materialization);
		materialization.adjacency_mapping_tree_pools.front().appended_locations.clear();
	});
	rejectAdjacencyPreparer([&assignUsableFrames](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignUsableFrames(materialization);
		materialization.adjacency_mapping_tree_pools.front().appended_locations.front().page_index = 0U;
	});
}

TEST_F(RelocatableMetadataStoreTest, OfflineVectorRebuildMatchesCodebookExtentsBySegmentIdentity) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	VectorManifestStore manifests(io, *store);
	VectorManifest initialManifest;
	initialManifest.index_id = 17U;
	initialManifest.dimensions = 2U;
	static_cast<void>(
			manifests.materialize(initialManifest, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	const VectorManifest manifest = manifests.read(snapshot, 17U).manifest();
	const VectorManifestLocation manifestDirectory = store->findVectorManifest(snapshot, 17U);
	const PageLocation manifestLocation = store->findPageLocation(snapshot, manifest.page_id);
	const CatalogEntry manifestPool = store->findCatalog(snapshot, manifestLocation.segment_id);
	const Generation publishedGeneration = snapshot.superblock().generation;
	snapshot.release();

	// Prepared Codebooks sort physical writes by SegmentId and directory entries
	// by ExtentId. The coordinator must join them by SegmentId, never ordinal.
	std::array<CatalogSlotReservationRequest, 2U> codebookReservations = {
			codebookReservation(17U), codebookReservation(17U)};
	codebookReservations[0].segment_id = SegmentId(60U);
	codebookReservations[1].segment_id = SegmentId(100U);
	std::array<ExtentLocation, 2U> codebookExtents = {codebookExtent(), codebookExtent()};
	codebookExtents[0].segment_id = SegmentId(100U);
	codebookExtents[1].extent_id = VectorCodebookExtentIdCodec::dataExtentId(SegmentId(60U));
	codebookExtents[1].segment_id = SegmentId(60U);

	const std::array<VectorPqMaterializationPoolRequest, 1U> pqPools = {
			VectorPqMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
	OfflineVectorDerivedRebuildMetadataMutation mutation;
	mutation.index_id = 17U;
	mutation.expected_manifest_pool = manifestPool;
	mutation.expected_manifest_location = manifestLocation;
	mutation.expected_manifest = manifestDirectory;
	mutation.expected_manifest_owner_lsn = manifest.committed_lsn;
	mutation.codebook_first_slot = PhysicalSlotId(41U);
	mutation.codebook_last_slot = PhysicalSlotId(42U);
	mutation.codebook_reservations = codebookReservations;
	mutation.codebook_extents = codebookExtents;
	mutation.pq_pools = pqPools;
	mutation.mapping_tree.index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
	mutation.mapping_tree.new_pool_requests.push_back(
			{PhysicalSlotId(44U), PhysicalSlotId(44U), mutation.mapping_tree.index_id, 1U});
	mutation.mapping_tree.replacement_root = {0U, 0U, true};

	bool preparerCalled = false;
	expectFormatError(
			[&] {
				static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::publishOfflineRebuild(
						*store, mutation, LogSequenceNumber(3U),
						[&preparerCalled](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
							preparerCalled = true;
							materialization.codebook_extents.front().owner_id = 18U;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(preparerCalled);
	MetadataSnapshotPin afterFailure = store->openSnapshot();
	EXPECT_EQ(afterFailure.superblock().generation, publishedGeneration);
	EXPECT_FALSE(store->isPhysicalSlotAllocated(afterFailure, PhysicalSlotId(41U)));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(afterFailure, PhysicalSlotId(42U)));
	EXPECT_FALSE(store->findExtentLocationOptional(
					 afterFailure, VectorCodebookExtentIdCodec::manifestExtentId(17U))
					 .has_value());
}

TEST_F(RelocatableMetadataStoreTest, OfflineVectorRebuildRejectsMalformedOrStalePlansBeforePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	VectorManifestStore manifests(io, *store);
	VectorManifest initialManifest;
	initialManifest.index_id = 17U;
	initialManifest.dimensions = 4U;
	static_cast<void>(
			manifests.materialize(initialManifest, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	const VectorManifest manifest = manifests.read(snapshot, 17U).manifest();
	const VectorManifestLocation manifestDirectory = store->findVectorManifest(snapshot, 17U);
	const PageLocation manifestLocation = store->findPageLocation(snapshot, manifest.page_id);
	const CatalogEntry manifestPool = store->findCatalog(snapshot, manifestLocation.segment_id);
	const Generation generation = snapshot.superblock().generation;
	snapshot.release();
	const std::array<VectorPqMaterializationPoolRequest, 1U> pqPools = {
			VectorPqMaterializationPoolRequest{PhysicalSlotId(41U), PhysicalSlotId(41U), 1U}};

	const auto validMutation = [&] {
		OfflineVectorDerivedRebuildMetadataMutation mutation;
		mutation.index_id = 17U;
		mutation.expected_manifest_pool = manifestPool;
		mutation.expected_manifest_location = manifestLocation;
		mutation.expected_manifest = manifestDirectory;
		mutation.expected_manifest_owner_lsn = manifest.committed_lsn;
		mutation.pq_pools = pqPools;
		mutation.mapping_tree.index_id = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(42U), PhysicalSlotId(42U), mutation.mapping_tree.index_id, 1U});
		mutation.mapping_tree.replacement_root = {0U, 0U, true};
		return mutation;
	};
	const auto expectRejected = [&](const OfflineVectorDerivedRebuildMetadataMutation &mutation, FormatError error) {
		expectFormatError(
				[&] {
					static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::publishOfflineRebuild(
							*store, mutation, LogSequenceNumber(3U),
							[](OfflineVectorDerivedRebuildMetadataMaterialization &) {}));
				},
				error);
		MetadataSnapshotPin afterFailure = store->openSnapshot();
		EXPECT_EQ(afterFailure.superblock().generation, generation);
		EXPECT_EQ(store->findVectorManifest(afterFailure, 17U), manifestDirectory);
		EXPECT_FALSE(store->findIndexRootOptional(afterFailure, RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1))
							 .has_value());
	};
	const auto expectMalformedMutation = [&](const auto &mutate) {
		auto mutation = validMutation();
		mutate(mutation);
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	};
	const auto expectMalformedAdjacencyMutation = [&](const auto &mutate) {
		auto mutation = validMutation();
		const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> adjacencyPools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
		mutation.adjacency_pools = adjacencyPools;
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(44U), PhysicalSlotId(44U), mutation.adjacency_mapping_tree.index_id, 1U});
		mutation.adjacency_mapping_tree.replacement_root = {0U, 0U, true};
		mutate(mutation);
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	};
	const auto expectMalformedExistingMapping = [&](const IndexRootLocation &expectedRoot, bool clearPqRetirements,
													bool clearMappingRetirements) {
		auto mutation = validMutation();
		const std::array<PageRetirement, 1U> pqRetirements = {pqRetirement(PageId(1U))};
		const std::array<IndexTreeMutationPoolRequest, 1U> mappingRetirements = {mappingRetirementRequest(PageId(10U))};
		mutation.expected_mapping_root = expectedRoot;
		if (!clearPqRetirements) {
			mutation.pq_retirements = pqRetirements;
		}
		if (!clearMappingRetirements) {
			mutation.mapping_retirement_pools = mappingRetirements;
		}
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	};

	expectMalformedMutation(
			[](OfflineVectorDerivedRebuildMetadataMutation &mutation) { mutation.mapping_tree.index_id = 0U; });
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.mapping_tree.expected_root = IndexRootLocation{1U, PageId(1U), Generation(1U)};
	});
	expectMalformedMutation(
			[](OfflineVectorDerivedRebuildMetadataMutation &mutation) { mutation.mapping_tree.retire_root = true; });
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.mapping_tree.pool_requests.push_back({});
	});
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.mapping_tree.new_pool_requests.clear();
	});
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.mapping_tree.replacement_root.reset();
	});
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.mapping_tree.replacement_root->newly_reserved = false;
	});
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.mapping_tree.stable_replacement_root = PageId(1U);
	});
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		const std::array<PageRetirement, 1U> retirements = {pqRetirement(PageId(1U))};
		mutation.pq_retirements = retirements;
	});
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
	});
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.expected_delta_mapping_root = {1U, PageId(1U), Generation(1U)};
		const std::array<IndexTreeMutationPoolRequest, 1U> retirements = {mappingRetirementRequest(PageId(1U))};
		mutation.delta_mapping_retirement_pools = retirements;
	});
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.mapping_tree.new_pool_requests.front().owner_id = 17U;
	});
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.mapping_tree.new_pool_requests.front().first_slot = {};
	});
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.mapping_tree.new_pool_requests.front().first_slot = PhysicalSlotId(43U);
	});

	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.index_id = 0U;
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.index_id = 17U;
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.expected_root = IndexRootLocation{1U, PageId(1U), Generation(1U)};
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.retire_root = true;
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.pool_requests.push_back({});
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.new_pool_requests.clear();
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.replacement_root.reset();
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.replacement_root->newly_reserved = false;
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.stable_replacement_root = PageId(1U);
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		const std::array<PageRetirement, 1U> retirements = {adjacencyRetirement(PageId(1U))};
		mutation.adjacency_retirements = retirements;
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.new_pool_requests.front().owner_id = 17U;
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.new_pool_requests.front().first_slot = PhysicalSlotId(45U);
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.expected_adjacency_mapping_root = {1U, PageId(1U), Generation(1U)};
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.expected_adjacency_mapping_root = {mutation.adjacency_mapping_tree.index_id, PageId{}, Generation(1U)};
	});
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.expected_adjacency_mapping_root = {mutation.adjacency_mapping_tree.index_id, PageId(1U), Generation{}};
	});
	expectMalformedExistingMapping(IndexRootLocation{1U, PageId(10U), Generation(1U)}, false, false);
	expectMalformedExistingMapping(
			IndexRootLocation{RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1), PageId{}, Generation(1U)}, false,
			false);
	expectMalformedExistingMapping(
			IndexRootLocation{RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1), PageId(10U), Generation{}}, false,
			false);
	expectMalformedExistingMapping(
			IndexRootLocation{RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1), PageId(10U), Generation(1U)}, true,
			false);
	expectMalformedExistingMapping(
			IndexRootLocation{RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1), PageId(10U), Generation(1U)}, false,
			true);

	{
		auto mutation = validMutation();
		mutation.index_id = 0U;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		mutation.expected_manifest_owner_lsn = {};
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorPqMaterializationPoolRequest, 1U> invalidPqPools = {
				VectorPqMaterializationPoolRequest{PhysicalSlotId(41U), PhysicalSlotId(41U), 0U}};
		mutation.pq_pools = invalidPqPools;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		mutation.mapping_tree.index_id = 17U;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		mutation.mapping_tree.new_pool_requests.clear();
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		mutation.mapping_tree.new_pool_requests.front().appended_page_count = 0U;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<PageRetirement, 1U> invalidPqRetirements = {pqRetirement(PageId{})};
		const std::array<IndexTreeMutationPoolRequest, 1U> mappingRetirements = {mappingRetirementRequest(PageId(10U))};
		mutation.expected_mapping_root = {mutation.mapping_tree.index_id, PageId(10U), Generation(1U)};
		mutation.pq_retirements = invalidPqRetirements;
		mutation.mapping_retirement_pools = mappingRetirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<PageRetirement, 1U> pqRetirements = {pqRetirement(PageId(1U))};
		std::array<IndexTreeMutationPoolRequest, 1U> invalidMappingRetirements = {
				mappingRetirementRequest(PageId(10U))};
		invalidMappingRetirements.front().segment_id = {};
		mutation.expected_mapping_root = {mutation.mapping_tree.index_id, PageId(10U), Generation(1U)};
		mutation.pq_retirements = pqRetirements;
		mutation.mapping_retirement_pools = invalidMappingRetirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<PageRetirement, 1U> pqRetirements = {pqRetirement(PageId(1U))};
		std::array<IndexTreeMutationPoolRequest, 1U> invalidMappingRetirements = {
				mappingRetirementRequest(PageId(10U))};
		invalidMappingRetirements.front().expected_retirements.front().location.page_id = {};
		mutation.expected_mapping_root = {mutation.mapping_tree.index_id, PageId(10U), Generation(1U)};
		mutation.pq_retirements = pqRetirements;
		mutation.mapping_retirement_pools = invalidMappingRetirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<PageRetirement, 1U> pqRetirements = {pqRetirement(PageId(1U))};
		const std::array<IndexTreeMutationPoolRequest, 1U> mappingRetirements = {mappingRetirementRequest(PageId(9U))};
		mutation.expected_mapping_root = {mutation.mapping_tree.index_id, PageId(10U), Generation(1U)};
		mutation.pq_retirements = pqRetirements;
		mutation.mapping_retirement_pools = mappingRetirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> adjacencyPools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
		const std::array<PageRetirement, 1U> invalidAdjacencyRetirements = {adjacencyRetirement(PageId{})};
		const std::array<IndexTreeMutationPoolRequest, 1U> adjacencyMappingRetirements = {
				mappingRetirementRequest(PageId(10U))};
		mutation.adjacency_pools = adjacencyPools;
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(44U), PhysicalSlotId(44U), mutation.adjacency_mapping_tree.index_id, 1U});
		mutation.adjacency_mapping_tree.replacement_root = {0U, 0U, true};
		mutation.expected_adjacency_mapping_root = {mutation.adjacency_mapping_tree.index_id, PageId(10U),
													Generation(1U)};
		mutation.adjacency_retirements = invalidAdjacencyRetirements;
		mutation.adjacency_mapping_retirement_pools = adjacencyMappingRetirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> adjacencyPools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
		const std::array<PageRetirement, 1U> adjacencyRetirements = {adjacencyRetirement(PageId(9U))};
		std::array<IndexTreeMutationPoolRequest, 1U> invalidAdjacencyMappingRetirements = {
				mappingRetirementRequest(PageId(10U))};
		invalidAdjacencyMappingRetirements.front().segment_id = {};
		mutation.adjacency_pools = adjacencyPools;
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(44U), PhysicalSlotId(44U), mutation.adjacency_mapping_tree.index_id, 1U});
		mutation.adjacency_mapping_tree.replacement_root = {0U, 0U, true};
		mutation.expected_adjacency_mapping_root = {mutation.adjacency_mapping_tree.index_id, PageId(10U),
													Generation(1U)};
		mutation.adjacency_retirements = adjacencyRetirements;
		mutation.adjacency_mapping_retirement_pools = invalidAdjacencyMappingRetirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> adjacencyPools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
		const std::array<PageRetirement, 1U> adjacencyRetirements = {adjacencyRetirement(PageId(9U))};
		const std::array<IndexTreeMutationPoolRequest, 1U> adjacencyMappingRetirements = {
				mappingRetirementRequest(PageId(11U))};
		mutation.adjacency_pools = adjacencyPools;
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(44U), PhysicalSlotId(44U), mutation.adjacency_mapping_tree.index_id, 1U});
		mutation.adjacency_mapping_tree.replacement_root = {0U, 0U, true};
		mutation.expected_adjacency_mapping_root = {mutation.adjacency_mapping_tree.index_id, PageId(10U),
													Generation(1U)};
		mutation.adjacency_retirements = adjacencyRetirements;
		mutation.adjacency_mapping_retirement_pools = adjacencyMappingRetirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		std::array<IndexTreeMutationPoolRequest, 1U> invalidDeltaMappingRetirements = {
				mappingRetirementRequest(PageId(12U))};
		invalidDeltaMappingRetirements.front().expected_retirements.front().location.page_id = {};
		mutation.expected_delta_mapping_root = {RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1), PageId(12U),
												Generation(1U)};
		mutation.delta_mapping_retirement_pools = invalidDeltaMappingRetirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<IndexTreeMutationPoolRequest, 1U> deltaMappingRetirements = {
				mappingRetirementRequest(PageId(13U))};
		mutation.expected_delta_mapping_root = {RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1), PageId(12U),
												Generation(1U)};
		mutation.delta_mapping_retirement_pools = deltaMappingRetirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		mutation.mapping_tree.replacement_root = {0U, 0U, false};
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		mutation.mapping_tree.replacement_root = {1U, 0U, true};
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		mutation.expected_delta_mapping_root =
				IndexRootLocation{RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1), PageId(1U), Generation(1U)};
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<CatalogSlotReservationRequest, 1U> invalidCodebookReservations = {codebookReservation(0U)};
		const std::array<ExtentLocation, 1U> codebookExtents = {codebookExtent()};
		mutation.codebook_first_slot = PhysicalSlotId(43U);
		mutation.codebook_last_slot = PhysicalSlotId(43U);
		mutation.codebook_reservations = invalidCodebookReservations;
		mutation.codebook_extents = codebookExtents;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<CatalogSlotReservationRequest, 1U> codebookReservations = {codebookReservation(17U)};
		auto codebookExtents = std::array<ExtentLocation, 1U>{codebookExtent()};
		codebookExtents.front().extent_id = ExtentId(999U);
		mutation.codebook_first_slot = PhysicalSlotId(43U);
		mutation.codebook_last_slot = PhysicalSlotId(43U);
		mutation.codebook_reservations = codebookReservations;
		mutation.codebook_extents = codebookExtents;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		auto codebookReservations = std::array<CatalogSlotReservationRequest, 2U>{codebookReservation(17U),
																								codebookReservation(17U)};
		codebookReservations[1].segment_id = SegmentId(101U);
		auto codebookExtents = std::array<ExtentLocation, 2U>{codebookExtent(), codebookExtent()};
		codebookExtents[0].extent_id = VectorCodebookExtentIdCodec::dataExtentId(SegmentId(100U));
		codebookExtents[1].extent_id = VectorCodebookExtentIdCodec::dataExtentId(SegmentId(101U));
		codebookExtents[1].segment_id = SegmentId(101U);
		mutation.codebook_first_slot = PhysicalSlotId(43U);
		mutation.codebook_last_slot = PhysicalSlotId(44U);
		mutation.codebook_reservations = codebookReservations;
		mutation.codebook_extents = codebookExtents;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<CatalogSlotReservationRequest, 2U> duplicateReservations = {
				codebookReservation(17U), codebookReservation(17U)};
		auto codebookExtents = std::array<ExtentLocation, 2U>{codebookExtent(), codebookExtent()};
		codebookExtents[1].extent_id = VectorCodebookExtentIdCodec::dataExtentId(SegmentId(100U));
		mutation.codebook_first_slot = PhysicalSlotId(43U);
		mutation.codebook_last_slot = PhysicalSlotId(44U);
		mutation.codebook_reservations = duplicateReservations;
		mutation.codebook_extents = codebookExtents;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		auto codebookReservations = std::array<CatalogSlotReservationRequest, 2U>{codebookReservation(17U),
																								codebookReservation(18U)};
		codebookReservations[1].segment_id = SegmentId(101U);
		auto codebookExtents = std::array<ExtentLocation, 2U>{codebookExtent(), codebookExtent()};
		codebookExtents[1].extent_id = VectorCodebookExtentIdCodec::dataExtentId(SegmentId(101U));
		codebookExtents[1].segment_id = SegmentId(101U);
		mutation.codebook_first_slot = PhysicalSlotId(43U);
		mutation.codebook_last_slot = PhysicalSlotId(44U);
		mutation.codebook_reservations = codebookReservations;
		mutation.codebook_extents = codebookExtents;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		auto codebookReservations = std::array<CatalogSlotReservationRequest, 2U>{codebookReservation(17U),
																								codebookReservation(17U)};
		codebookReservations[1].segment_id = SegmentId(101U);
		codebookReservations[1].segment_generation = Generation(2U);
		auto codebookExtents = std::array<ExtentLocation, 2U>{codebookExtent(), codebookExtent()};
		codebookExtents[1].extent_id = VectorCodebookExtentIdCodec::dataExtentId(SegmentId(101U));
		codebookExtents[1].segment_id = SegmentId(101U);
		codebookExtents[1].generation = Generation(2U);
		mutation.codebook_first_slot = PhysicalSlotId(43U);
		mutation.codebook_last_slot = PhysicalSlotId(44U);
		mutation.codebook_reservations = codebookReservations;
		mutation.codebook_extents = codebookExtents;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<CatalogSlotReservationRequest, 1U> codebookReservations = {codebookReservation(17U)};
		auto unmatchedExtent = std::array<ExtentLocation, 1U>{codebookExtent()};
		unmatchedExtent.front().segment_id = SegmentId(101U);
		unmatchedExtent.front().extent_id = VectorCodebookExtentIdCodec::dataExtentId(SegmentId(101U));
		mutation.codebook_first_slot = PhysicalSlotId(43U);
		mutation.codebook_last_slot = PhysicalSlotId(43U);
		mutation.codebook_reservations = codebookReservations;
		mutation.codebook_extents = unmatchedExtent;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<CatalogSlotReservationRequest, 1U> codebookReservations = {codebookReservation(17U)};
		auto staleExtent = std::array<ExtentLocation, 1U>{codebookExtent()};
		staleExtent.front().generation = Generation(2U);
		mutation.codebook_first_slot = PhysicalSlotId(43U);
		mutation.codebook_last_slot = PhysicalSlotId(43U);
		mutation.codebook_reservations = codebookReservations;
		mutation.codebook_extents = staleExtent;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<CatalogSlotReservationRequest, 1U> codebookReservations = {codebookReservation(17U)};
		const std::array<ExtentLocation, 1U> codebookExtents = {codebookExtent()};
		mutation.codebook_first_slot = PhysicalSlotId(40U);
		mutation.codebook_last_slot = PhysicalSlotId(40U);
		mutation.codebook_reservations = codebookReservations;
		mutation.codebook_extents = codebookExtents;
		expectRejected(mutation, FormatError::FERR_INVALID_STATE);
	}
	{
		auto mutation = validMutation();
		const std::array<CatalogSlotReservationRequest, 1U> codebookReservations = {codebookReservation(17U)};
		const std::array<ExtentLocation, 1U> codebookExtents = {codebookExtent()};
		mutation.codebook_last_slot = PhysicalSlotId(43U);
		mutation.codebook_reservations = codebookReservations;
		mutation.codebook_extents = codebookExtents;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<CatalogSlotReservationRequest, 1U> codebookReservations = {codebookReservation(17U)};
		const std::array<ExtentLocation, 1U> codebookExtents = {codebookExtent()};
		mutation.codebook_first_slot = PhysicalSlotId(43U);
		mutation.codebook_reservations = codebookReservations;
		mutation.codebook_extents = codebookExtents;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<CatalogSlotReservationRequest, 1U> codebookReservations = {codebookReservation(17U)};
		const std::array<ExtentLocation, 1U> codebookExtents = {codebookExtent()};
		mutation.codebook_first_slot = PhysicalSlotId(44U);
		mutation.codebook_last_slot = PhysicalSlotId(43U);
		mutation.codebook_reservations = codebookReservations;
		mutation.codebook_extents = codebookExtents;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		mutation.expected_manifest_pool.physical_slot = PhysicalSlotId(41U);
		expectRejected(mutation, FormatError::FERR_INVALID_STATE);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorPqMaterializationPoolRequest, 1U> exhaustedPqPools = {
				VectorPqMaterializationPoolRequest{PhysicalSlotId(40U), PhysicalSlotId(40U), 1U}};
		mutation.pq_pools = exhaustedPqPools;
		expectRejected(mutation, FormatError::FERR_INVALID_STATE);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> invalidAdjacencyPools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 0U}};
		mutation.adjacency_pools = invalidAdjacencyPools;
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(44U), PhysicalSlotId(44U), mutation.adjacency_mapping_tree.index_id, 1U});
		mutation.adjacency_mapping_tree.replacement_root = {0U, 0U, true};
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> adjacencyPools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
		mutation.adjacency_pools = adjacencyPools;
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(44U), PhysicalSlotId(44U), mutation.adjacency_mapping_tree.index_id, 0U});
		mutation.adjacency_mapping_tree.replacement_root = {0U, 0U, true};
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> adjacencyPools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
		mutation.adjacency_pools = adjacencyPools;
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(44U), PhysicalSlotId(44U), mutation.adjacency_mapping_tree.index_id, 1U});
		mutation.adjacency_mapping_tree.replacement_root = {1U, 0U, true};
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		mutation.mapping_tree.new_pool_requests.front().first_slot = PhysicalSlotId(40U);
		mutation.mapping_tree.new_pool_requests.front().last_slot = PhysicalSlotId(40U);
		expectRejected(mutation, FormatError::FERR_INVALID_STATE);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> exhaustedAdjacencyPools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(40U), PhysicalSlotId(40U), 1U}};
		mutation.adjacency_pools = exhaustedAdjacencyPools;
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(43U), PhysicalSlotId(43U), mutation.adjacency_mapping_tree.index_id, 1U});
		mutation.adjacency_mapping_tree.replacement_root = {0U, 0U, true};
		expectRejected(mutation, FormatError::FERR_INVALID_STATE);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorPqMaterializationPoolRequest, 1U> relocatedPqPools = {
				VectorPqMaterializationPoolRequest{PhysicalSlotId(41U), PhysicalSlotId(41U), 1U}};
		const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> adjacencyPools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(42U), PhysicalSlotId(42U), 1U}};
		mutation.pq_pools = relocatedPqPools;
		mutation.mapping_tree.new_pool_requests.front().first_slot = PhysicalSlotId(43U);
		mutation.mapping_tree.new_pool_requests.front().last_slot = PhysicalSlotId(43U);
		mutation.adjacency_pools = adjacencyPools;
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(40U), PhysicalSlotId(40U), mutation.adjacency_mapping_tree.index_id, 1U});
		mutation.adjacency_mapping_tree.replacement_root = {0U, 0U, true};
		expectRejected(mutation, FormatError::FERR_INVALID_STATE);
	}
	// Exercise each conditional part independently so malformed plans cannot hide
	// behind an earlier short-circuit predicate.
	{
		auto mutation = validMutation();
		const std::array<IndexTreeMutationPoolRequest, 1U> retirements = {mappingRetirementRequest(PageId(10U))};
		mutation.mapping_retirement_pools = retirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> adjacencyPools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
		const std::array<IndexTreeMutationPoolRequest, 1U> retirements = {mappingRetirementRequest(PageId(10U))};
		mutation.adjacency_pools = adjacencyPools;
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(1);
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(44U), PhysicalSlotId(44U), mutation.adjacency_mapping_tree.index_id, 1U});
		mutation.adjacency_mapping_tree.replacement_root = {0U, 0U, true};
		mutation.adjacency_mapping_retirement_pools = retirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> adjacencyPools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U}};
		const std::array<PageRetirement, 1U> retirements = {adjacencyRetirement(PageId(1U))};
		mutation.adjacency_pools = adjacencyPools;
		mutation.adjacency_mapping_tree.index_id = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(1);
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(44U), PhysicalSlotId(44U), mutation.adjacency_mapping_tree.index_id, 1U});
		mutation.adjacency_mapping_tree.replacement_root = {0U, 0U, true};
		mutation.expected_adjacency_mapping_root =
				IndexRootLocation{mutation.adjacency_mapping_tree.index_id, PageId(10U), Generation(1U)};
		mutation.adjacency_retirements = retirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		const std::array<PageRetirement, 1U> retirements = {adjacencyRetirement(PageId(1U))};
		mutation.adjacency_retirements = retirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.expected_adjacency_mapping_root = IndexRootLocation{1U, PageId(1U), Generation(1U)};
	});
	{
		auto mutation = validMutation();
		const std::array<IndexTreeMutationPoolRequest, 1U> retirements = {mappingRetirementRequest(PageId(10U))};
		mutation.adjacency_mapping_retirement_pools = retirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.index_id = 1U;
	});
	expectMalformedMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.new_pool_requests.push_back(
				{PhysicalSlotId(43U), PhysicalSlotId(43U), 1U, 1U});
	});
	const auto expectMalformedDeltaRoot = [&](const IndexRootLocation &root) {
		auto mutation = validMutation();
		const std::array<IndexTreeMutationPoolRequest, 1U> retirements = {mappingRetirementRequest(PageId(10U))};
		mutation.expected_delta_mapping_root = root;
		mutation.delta_mapping_retirement_pools = retirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	};
	expectMalformedDeltaRoot(IndexRootLocation{0U, PageId(1U), Generation(1U)});
	expectMalformedDeltaRoot(
			IndexRootLocation{RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(1), PageId{}, Generation(1U)});
	expectMalformedDeltaRoot(
			IndexRootLocation{RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(1), PageId(1U), Generation{}});
	{
		auto mutation = validMutation();
		const std::array<IndexTreeMutationPoolRequest, 1U> retirements = {mappingRetirementRequest(PageId(10U))};
		mutation.delta_mapping_retirement_pools = retirements;
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	{
		auto mutation = validMutation();
		mutation.mapping_tree.replacement_root = {0U, 1U, true};
		expectRejected(mutation, FormatError::FERR_INVALID_HEADER);
	}
	expectMalformedAdjacencyMutation([](OfflineVectorDerivedRebuildMetadataMutation &mutation) {
		mutation.adjacency_mapping_tree.replacement_root = {0U, 1U, true};
	});
}

TEST_F(RelocatableMetadataStoreTest, OfflineVectorRebuildRejectsStaleOrForeignPublishedDerivedSources) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	VectorManifestStore manifests(io, *store);
	VectorPageStore vectors(io, *store);
	VectorAdjacencyPageStore adjacencies(io, *store);
	SlottedPagePool slotted(io, *store);
	BTreePageStore trees(slotted, *store);
	PhysicalSegmentAllocator segments(io, *store);
	VectorCodebookStore codebooks(segments, *store);
	VectorManifest initialManifest;
	initialManifest.index_id = 17U;
	initialManifest.dimensions = 2U;
	static_cast<void>(
			manifests.materialize(initialManifest, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U)));
	const std::array<PqVectorMaterializationRecord, 1U> sourceRecords = {{{1U, {0U, 1U}}}};
	const VectorPqMaterializationOptions sourcePqOptions = {PhysicalSlotId(41U), PhysicalSlotId(41U), Generation(1U),
															16U, 1U};
	const VectorPqMaterializationResult sourcePq =
			vectors.materializePq(17U, sourceRecords, sourcePqOptions, LogSequenceNumber(3U));
	ASSERT_EQ(sourcePq.pages.size(), 1U);
	const uint64_t mappingIndexId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
	VectorPqMappingBuildOptions sourceMappingOptions;
	sourceMappingOptions.tree = {PhysicalSlotId(42U), PhysicalSlotId(42U), mappingIndexId, 16U, 1U};
	sourceMappingOptions.maximum_entries = 16U;
	static_cast<void>(VectorPqMapping::build(trees, *store, mappingIndexId, sourcePq.mappings, sourceMappingOptions,
											 LogSequenceNumber(4U)));
	const std::array<VectorAdjacencyMaterializationRecord, 1U> sourceAdjacencyRecords = {{{1U, {}}}};
	const VectorAdjacencyMaterializationOptions sourceAdjacencyOptions = {PhysicalSlotId(43U), PhysicalSlotId(43U), 16U,
																		  1U};
	const VectorAdjacencyMaterializationResult sourceAdjacency =
			adjacencies.materialize(17U, sourceAdjacencyRecords, sourceAdjacencyOptions, LogSequenceNumber(5U));
	ASSERT_EQ(sourceAdjacency.pages.size(), 1U);
	const uint64_t adjacencyMappingIndexId = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
	VectorAdjacencyMappingBuildOptions sourceAdjacencyMappingOptions;
	sourceAdjacencyMappingOptions.tree = {PhysicalSlotId(44U), PhysicalSlotId(44U), adjacencyMappingIndexId, 16U, 1U};
	sourceAdjacencyMappingOptions.maximum_entries = 16U;
	static_cast<void>(VectorAdjacencyMapping::build(trees, *store, adjacencyMappingIndexId, sourceAdjacency.mappings,
													sourceAdjacencyMappingOptions, LogSequenceNumber(6U)));
	const uint64_t deltaMappingIndexId = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x1);
	const std::array<VectorDeltaEntry, 1U> sourceDeltaEntries = {
			VectorDeltaEntry{1U, 1U, VectorDeltaOperation::VDO_INSERT}};
	VectorDeltaMappingBuildOptions sourceDeltaMappingOptions;
	sourceDeltaMappingOptions.tree = {PhysicalSlotId(49U), PhysicalSlotId(49U), deltaMappingIndexId, 16U, 1U};
	sourceDeltaMappingOptions.maximum_entries = 16U;
	static_cast<void>(VectorDeltaMapping::build(trees, *store, deltaMappingIndexId, sourceDeltaEntries,
													 sourceDeltaMappingOptions, LogSequenceNumber(7U)));
	const VectorPqMaterializationOptions foreignPqOptions = {PhysicalSlotId(50U), PhysicalSlotId(50U), Generation(1U),
																			 16U, 1U};
	const VectorPqMaterializationResult foreignPq =
			vectors.materializePq(18U, sourceRecords, foreignPqOptions, LogSequenceNumber(8U));
	ASSERT_EQ(foreignPq.pages.size(), 1U);
	const VectorAdjacencyMaterializationOptions foreignAdjacencyOptions = {PhysicalSlotId(51U), PhysicalSlotId(51U),
																		   16U, 1U};
	const VectorAdjacencyMaterializationResult foreignAdjacency =
			adjacencies.materialize(18U, sourceAdjacencyRecords, foreignAdjacencyOptions, LogSequenceNumber(9U));
	ASSERT_EQ(foreignAdjacency.pages.size(), 1U);
	VectorCodebookWriteRequest codebookRequest;
	codebookRequest.codebook_id = 17U;
	codebookRequest.manifest_segment_id = SegmentId(70U);
	codebookRequest.generation = Generation(1U);
	codebookRequest.element_type = VectorCodebookElementType::VCET_FLOAT32;
	codebookRequest.dimensions = 2U;
	codebookRequest.subspace_count = 1U;
	codebookRequest.centroid_count = 3U;
	const std::array<std::byte, 24U> codebookPayload{};
	static_cast<void>(codebooks.writeImmutable(codebookRequest, codebookPayload, PhysicalSlotId(52U),
											 PhysicalSlotId(52U), LogSequenceNumber(10U)));
	const uint64_t foreignMappingIndexId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x2);
	VectorPqMappingBuildOptions foreignMappingOptions;
	foreignMappingOptions.tree = {PhysicalSlotId(53U), PhysicalSlotId(53U), foreignMappingIndexId, 16U, 1U};
	foreignMappingOptions.maximum_entries = 16U;
	static_cast<void>(VectorPqMapping::build(trees, *store, foreignMappingIndexId, sourcePq.mappings,
													  foreignMappingOptions, LogSequenceNumber(11U)));

	MetadataSnapshotPin sourceSnapshot = store->openSnapshot();
	const VectorManifest manifest = manifests.read(sourceSnapshot, 17U).manifest();
	const VectorManifestLocation manifestDirectory = store->findVectorManifest(sourceSnapshot, 17U);
	const PageLocation manifestLocation = store->findPageLocation(sourceSnapshot, manifest.page_id);
	const CatalogEntry manifestPool = store->findCatalog(sourceSnapshot, manifestLocation.segment_id);
	const std::optional<DerivedVectorPqPage> sourcePage =
			vectors.inspectDerivedPqPage(sourceSnapshot, sourcePq.pages.front().page_id);
	ASSERT_TRUE(sourcePage.has_value());
	const BTreeTreeRetirementPlan sourceMapping = trees.planTreeRetirement(sourceSnapshot, mappingIndexId, 1U);
	const std::optional<DerivedVectorAdjacencyPage> sourceAdjacencyPage =
			adjacencies.inspectDerivedPage(sourceSnapshot, sourceAdjacency.pages.front().page_id);
	ASSERT_TRUE(sourceAdjacencyPage.has_value());
	const BTreeTreeRetirementPlan sourceAdjacencyMapping =
			trees.planTreeRetirement(sourceSnapshot, adjacencyMappingIndexId, 1U);
	const BTreeTreeRetirementPlan sourceDeltaMapping =
			trees.planTreeRetirement(sourceSnapshot, deltaMappingIndexId, 1U);
	const BTreeTreeRetirementPlan foreignMapping =
			trees.planTreeRetirement(sourceSnapshot, foreignMappingIndexId, 1U);
	const std::optional<DerivedVectorPqPage> foreignPqPage =
			vectors.inspectDerivedPqPage(sourceSnapshot, foreignPq.pages.front().page_id);
	ASSERT_TRUE(foreignPqPage.has_value());
	const std::optional<DerivedVectorAdjacencyPage> foreignAdjacencyPage =
			adjacencies.inspectDerivedPage(sourceSnapshot, foreignAdjacency.pages.front().page_id);
	ASSERT_TRUE(foreignAdjacencyPage.has_value());
	const ExtentId publishedCodebookExtent = VectorCodebookExtentIdCodec::manifestExtentId(codebookRequest.codebook_id);
	const ExtentLocation publishedCodebookLocation = store->findExtentLocation(sourceSnapshot, publishedCodebookExtent);
	const CatalogEntry publishedCodebookCatalog = store->findCatalog(sourceSnapshot, codebookRequest.manifest_segment_id);
	const Generation publishedGeneration = sourceSnapshot.superblock().generation;
	sourceSnapshot.release();
	const std::array<VectorPqMaterializationPoolRequest, 1U> replacementPqPools = {
			VectorPqMaterializationPoolRequest{PhysicalSlotId(400U), PhysicalSlotId(400U), 1U}};
	const std::array<PageRetirement, 1U> sourceRetirements = {
			{{sourcePage->location, sourcePage->index_id, PageOwnerKind::POK_VECTOR_PQ, sourcePage->owner_lsn}}};
	const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> replacementAdjacencyPools = {
			VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(402U), PhysicalSlotId(402U), 1U}};
	const std::array<PageRetirement, 1U> sourceAdjacencyRetirements = {
			{{sourceAdjacencyPage->location, sourceAdjacencyPage->index_id | (UINT64_C(1) << 63U),
			  PageOwnerKind::POK_VECTOR_ADJACENCY, sourceAdjacencyPage->owner_lsn}}};
	const std::array<PageRetirement, 1U> foreignPqRetirements = {
			{{foreignPqPage->location, 17U, PageOwnerKind::POK_VECTOR_PQ, foreignPqPage->owner_lsn}}};
	const std::array<PageRetirement, 1U> foreignAdjacencyRetirements = {
			{{foreignAdjacencyPage->location, 17U | (UINT64_C(1) << 63U), PageOwnerKind::POK_VECTOR_ADJACENCY,
			  foreignAdjacencyPage->owner_lsn}}};

	const auto validMutation = [&] {
		OfflineVectorDerivedRebuildMetadataMutation mutation;
		mutation.index_id = 17U;
		mutation.expected_manifest_pool = manifestPool;
		mutation.expected_manifest_location = manifestLocation;
		mutation.expected_manifest = manifestDirectory;
		mutation.expected_manifest_owner_lsn = manifest.committed_lsn;
		mutation.pq_pools = replacementPqPools;
		mutation.pq_retirements = sourceRetirements;
		mutation.expected_mapping_root = sourceMapping.expected_root;
		mutation.mapping_retirement_pools = sourceMapping.pool_requests;
		mutation.mapping_tree.index_id = mappingIndexId;
			mutation.mapping_tree.new_pool_requests.push_back(
					{PhysicalSlotId(401U), PhysicalSlotId(401U), mappingIndexId, 1U});
		mutation.mapping_tree.replacement_root = {0U, 0U, true};
		mutation.adjacency_pools = replacementAdjacencyPools;
		mutation.adjacency_retirements = sourceAdjacencyRetirements;
		mutation.expected_adjacency_mapping_root = sourceAdjacencyMapping.expected_root;
		mutation.adjacency_mapping_retirement_pools = sourceAdjacencyMapping.pool_requests;
		mutation.adjacency_mapping_tree.index_id = adjacencyMappingIndexId;
			mutation.adjacency_mapping_tree.new_pool_requests.push_back(
					{PhysicalSlotId(403U), PhysicalSlotId(403U), adjacencyMappingIndexId, 1U});
		mutation.adjacency_mapping_tree.replacement_root = {0U, 0U, true};
		mutation.expected_delta_mapping_root = sourceDeltaMapping.expected_root;
		mutation.delta_mapping_retirement_pools = sourceDeltaMapping.pool_requests;
		return mutation;
	};
	const auto expectRejected = [&](const OfflineVectorDerivedRebuildMetadataMutation &mutation,
										 bool *preparerCalled = nullptr) {
		expectFormatError(
				[&] {
			static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::publishOfflineRebuild(
							*store, mutation, LogSequenceNumber(12U),
							[preparerCalled](OfflineVectorDerivedRebuildMetadataMaterialization &) {
								if (preparerCalled != nullptr) {
									*preparerCalled = true;
								}
							}));
				},
				FormatError::FERR_INVALID_STATE);
		MetadataSnapshotPin afterFailure = store->openSnapshot();
		EXPECT_EQ(afterFailure.superblock().generation, publishedGeneration);
		EXPECT_EQ(store->findIndexRoot(afterFailure, mappingIndexId), sourceMapping.expected_root);
		const CatalogEntry visibleCodebookCatalog =
				store->findCatalog(afterFailure, codebookRequest.manifest_segment_id);
		EXPECT_EQ(visibleCodebookCatalog.segment_id, publishedCodebookCatalog.segment_id);
		EXPECT_EQ(visibleCodebookCatalog.physical_slot, publishedCodebookCatalog.physical_slot);
		EXPECT_EQ(visibleCodebookCatalog.kind, publishedCodebookCatalog.kind);
		EXPECT_EQ(visibleCodebookCatalog.owner_id, publishedCodebookCatalog.owner_id);
		EXPECT_EQ(visibleCodebookCatalog.generation, publishedCodebookCatalog.generation);
		EXPECT_EQ(visibleCodebookCatalog.state, publishedCodebookCatalog.state);
		EXPECT_EQ(visibleCodebookCatalog.committed_lsn, publishedCodebookCatalog.committed_lsn);
		EXPECT_EQ(store->findExtentLocation(afterFailure, publishedCodebookExtent), publishedCodebookLocation);
		EXPECT_FALSE(store->isPhysicalSlotAllocated(afterFailure, PhysicalSlotId(400U)));
		EXPECT_FALSE(store->isPhysicalSlotAllocated(afterFailure, PhysicalSlotId(401U)));
		EXPECT_FALSE(store->isPhysicalSlotAllocated(afterFailure, PhysicalSlotId(402U)));
		EXPECT_FALSE(store->isPhysicalSlotAllocated(afterFailure, PhysicalSlotId(403U)));
	};
	const auto expectPreparerRejected = [&](const OfflineVectorDerivedRebuildMetadataPreparer &preparer) {
		const auto mutation = validMutation();
		try {
			static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::publishOfflineRebuild(
					*store, mutation, LogSequenceNumber(12U), preparer));
			FAIL() << "Expected a preparer immutability rejection";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE) << error.what();
		}
		MetadataSnapshotPin afterFailure = store->openSnapshot();
		EXPECT_EQ(afterFailure.superblock().generation, publishedGeneration);
		EXPECT_EQ(store->findIndexRoot(afterFailure, mappingIndexId), sourceMapping.expected_root);
		EXPECT_EQ(store->findIndexRoot(afterFailure, adjacencyMappingIndexId), sourceAdjacencyMapping.expected_root);
		EXPECT_EQ(store->findIndexRoot(afterFailure, deltaMappingIndexId), sourceDeltaMapping.expected_root);
	};
	{
		auto mutation = validMutation();
		auto staleRetirements = sourceRetirements;
		staleRetirements.front().location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
		mutation.pq_retirements = staleRetirements;
		expectRejected(mutation);
	}
	{
		auto mutation = validMutation();
		auto staleRetirements = sourceRetirements;
		staleRetirements.front().expected_owner_lsn = LogSequenceNumber(11U);
		mutation.pq_retirements = staleRetirements;
		bool preparerCalled = false;
		expectRejected(mutation, &preparerCalled);
		EXPECT_FALSE(preparerCalled);
	}
	{
		auto mutation = validMutation();
		auto staleMappingRetirements = sourceMapping.pool_requests;
		staleMappingRetirements.front().expected_retirements.front().location.page_index =
				RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
		mutation.mapping_retirement_pools = staleMappingRetirements;
		expectRejected(mutation);
	}
	{
		auto mutation = validMutation();
		auto foreignMappingRetirements = sourceMapping.pool_requests;
		foreignMappingRetirements.insert(foreignMappingRetirements.end(), foreignMapping.pool_requests.begin(),
													foreignMapping.pool_requests.end());
		mutation.mapping_retirement_pools = std::move(foreignMappingRetirements);
		bool preparerCalled = false;
		expectRejected(mutation, &preparerCalled);
		EXPECT_FALSE(preparerCalled);
	}
	{
		auto mutation = validMutation();
		auto staleRetirements = sourceAdjacencyRetirements;
		staleRetirements.front().location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
		mutation.adjacency_retirements = staleRetirements;
		expectRejected(mutation);
	}
	{
		auto mutation = validMutation();
		auto staleRetirements = sourceAdjacencyRetirements;
		staleRetirements.front().expected_owner_lsn = LogSequenceNumber(11U);
		mutation.adjacency_retirements = staleRetirements;
		bool preparerCalled = false;
		expectRejected(mutation, &preparerCalled);
		EXPECT_FALSE(preparerCalled);
	}
	{
		auto mutation = validMutation();
		auto staleMappingRetirements = sourceAdjacencyMapping.pool_requests;
		staleMappingRetirements.front().expected_retirements.front().location.page_index =
				RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
		mutation.adjacency_mapping_retirement_pools = staleMappingRetirements;
		expectRejected(mutation);
	}
	{
		auto mutation = validMutation();
		auto staleMappingRetirements = sourceDeltaMapping.pool_requests;
		staleMappingRetirements.front().expected_retirements.front().location.page_index =
				RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
		mutation.delta_mapping_retirement_pools = staleMappingRetirements;
		expectRejected(mutation);
	}
	{
		auto mutation = validMutation();
		mutation.pq_retirements = foreignPqRetirements;
		expectRejected(mutation);
	}
	{
		auto mutation = validMutation();
		mutation.adjacency_retirements = foreignAdjacencyRetirements;
		expectRejected(mutation);
	}
	{
		auto mutation = validMutation();
		auto reusedCatalog = std::array<CatalogSlotReservationRequest, 2U>{
				codebookReservation(codebookRequest.codebook_id), codebookReservation(codebookRequest.codebook_id)};
		reusedCatalog[0].segment_id = codebookRequest.manifest_segment_id;
		reusedCatalog[1].segment_id = SegmentId(72U);
		auto distinctExtents = std::array<ExtentLocation, 2U>{codebookExtent(), codebookExtent()};
		distinctExtents[0].segment_id = SegmentId(72U);
		distinctExtents[1].extent_id = VectorCodebookExtentIdCodec::dataExtentId(codebookRequest.manifest_segment_id);
		distinctExtents[1].segment_id = codebookRequest.manifest_segment_id;
		mutation.codebook_first_slot = PhysicalSlotId(405U);
		mutation.codebook_last_slot = PhysicalSlotId(406U);
		mutation.codebook_reservations = reusedCatalog;
		mutation.codebook_extents = distinctExtents;
		expectRejected(mutation);
	}
	{
		auto mutation = validMutation();
		auto distinctCatalog = std::array<CatalogSlotReservationRequest, 1U>{codebookReservation(codebookRequest.codebook_id)};
		distinctCatalog.front().segment_id = SegmentId(71U);
		auto reusedExtent = std::array<ExtentLocation, 1U>{codebookExtent()};
		reusedExtent.front().extent_id = publishedCodebookExtent;
		reusedExtent.front().segment_id = SegmentId(71U);
		mutation.codebook_first_slot = PhysicalSlotId(405U);
		mutation.codebook_last_slot = PhysicalSlotId(405U);
		mutation.codebook_reservations = distinctCatalog;
		mutation.codebook_extents = reusedExtent;
		expectRejected(mutation);
	}
	{
		auto mutation = validMutation();
		auto retirements = sourceMapping.pool_requests;
		IndexTreeMutationPoolRequest foreignPool;
		foreignPool.segment_id = codebookRequest.manifest_segment_id;
		foreignPool.expected_retirements.push_back(
				{{PageId(900U), foreignPool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U},
				 LogSequenceNumber(1U)});
		retirements.push_back(std::move(foreignPool));
		mutation.mapping_retirement_pools = retirements;
		expectRejected(mutation);
	}
	{
		auto mutation = validMutation();
		auto retirements = sourceAdjacencyMapping.pool_requests;
		IndexTreeMutationPoolRequest foreignPool;
		foreignPool.segment_id = codebookRequest.manifest_segment_id;
		foreignPool.expected_retirements.push_back(
				{{PageId(900U), foreignPool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U},
				 LogSequenceNumber(1U)});
		retirements.push_back(std::move(foreignPool));
		mutation.adjacency_mapping_retirement_pools = retirements;
		expectRejected(mutation);
	}
	{
		auto mutation = validMutation();
		auto retirements = sourceMapping.pool_requests;
		auto foreignRetirements = foreignMapping.pool_requests;
		foreignRetirements.front().expected_retirements.front().location.page_index++;
		retirements.insert(retirements.end(), foreignRetirements.begin(), foreignRetirements.end());
		mutation.mapping_retirement_pools = std::move(retirements);
		expectRejected(mutation);
	}
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.pq_retirement_pools.front().pool.physical_slot = PhysicalSlotId(52U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.pq_retirement_pools.front().retirements.clear();
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.mapping_retirement_pools.front().pool.owner_id = 1U;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.pq_retirement_pools.front().retirements.front().location.page_id = PageId(999U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.pq_retirement_pools.front().retirements.front().expected_owner_id = 18U;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.pq_retirement_pools.front().retirements.front().expected_owner_kind =
				PageOwnerKind::POK_VECTOR_RAW;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.pq_retirement_pools.front().retirements.front().expected_owner_lsn = LogSequenceNumber(11U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.adjacency_retirement_pools.front().retirements.front().expected_owner_id =
				18U | (UINT64_C(1) << 63U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.mapping_retirement_pools.front().expected_retirements.front().expected_owner_lsn =
				LogSequenceNumber(11U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.adjacency_mapping_retirement_pools.front().expected_replacements.push_back({});
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.delta_mapping_retirement_pools.front().newly_reserved = true;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		PageLocation &location = materialization.pq_pools.front().locations.front();
		location.page_id = PageId(999U);
		location.segment_id = SegmentId(999U);
		location.generation = Generation(2U);
		location.page_index = 0U;
		location.free_bytes = static_cast<uint16_t>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 1U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		PageLocation &location = materialization.adjacency_pools.front().locations.front();
		location.page_id = PageId(999U);
		location.segment_id = SegmentId(999U);
		location.generation = Generation(2U);
		location.page_index = 0U;
		location.free_bytes = static_cast<uint16_t>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 1U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		PageLocation &location = materialization.mapping_tree_pools.front().appended_locations.front();
		location.page_id = PageId(999U);
		location.segment_id = SegmentId(999U);
		location.generation = Generation(2U);
		location.page_index = 0U;
		location.free_bytes = static_cast<uint16_t>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 1U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		PageLocation &location = materialization.adjacency_mapping_tree_pools.front().appended_locations.front();
		location.page_id = PageId(999U);
		location.segment_id = SegmentId(999U);
		location.generation = Generation(2U);
		location.page_index = 0U;
		location.free_bytes = static_cast<uint16_t>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 1U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.mapping_root.index_id = 1U;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.pq_pools.clear();
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.mapping_tree_pools.clear();
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.adjacency_mapping_root.index_id = 1U;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.adjacency_pools.clear();
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		materialization.adjacency_mapping_tree_pools.clear();
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.pq_pools.front().pool.owner_id = 18U;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.pq_pools.front().page_ids.front() = PageId(999U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.pq_pools.front().locations.clear();
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.adjacency_pools.front().pool.owner_id = 18U;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.adjacency_pools.front().page_ids.front() = PageId(999U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.adjacency_pools.front().locations.clear();
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.mapping_tree_pools.front().pool.owner_id = 1U;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.mapping_tree_pools.front().newly_reserved = false;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.mapping_tree_pools.front().appended_page_ids.front() = PageId(999U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.mapping_tree_pools.front().appended_locations.clear();
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.adjacency_mapping_tree_pools.front().pool.owner_id = 1U;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.adjacency_mapping_tree_pools.front().newly_reserved = false;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.adjacency_mapping_tree_pools.front().appended_page_ids.front() = PageId(999U);
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		assignValidOfflineRebuildFrames(materialization);
		materialization.adjacency_mapping_tree_pools.front().appended_locations.clear();
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		auto &pool = materialization.mapping_retirement_pools.front();
		pool.appended_page_ids.push_back(PageId(1U));
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		auto &pool = materialization.mapping_retirement_pools.front();
		pool.expected_retirements.front().location.page_index++;
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		auto &pool = materialization.mapping_retirement_pools.front();
		pool.replacement_locations.push_back({});
	});
	expectPreparerRejected([](OfflineVectorDerivedRebuildMetadataMaterialization &materialization) {
		auto &pool = materialization.mapping_retirement_pools.front();
		pool.appended_locations.push_back({});
	});
	{
		auto mutation = validMutation();
		const std::array<VectorPqMaterializationPoolRequest, 1U> pools = {
				VectorPqMaterializationPoolRequest{PhysicalSlotId(410U), PhysicalSlotId(410U), 2U}};
		mutation.pq_pools = pools;
		expectFormatError(
				[&] {
					static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::publishOfflineRebuild(
							*store, mutation, LogSequenceNumber(12U), [](auto &materialization) {
								assignValidOfflineRebuildFrames(materialization);
								materialization.pq_pools.front().locations[1].page_index =
										materialization.pq_pools.front().locations.front().page_index;
							}));
				},
				FormatError::FERR_INVALID_STATE);
	}
	{
		auto mutation = validMutation();
		const std::array<VectorAdjacencyMaterializationPoolRequest, 1U> pools = {
				VectorAdjacencyMaterializationPoolRequest{PhysicalSlotId(411U), PhysicalSlotId(411U), 2U}};
		mutation.adjacency_pools = pools;
		expectFormatError(
				[&] {
					static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::publishOfflineRebuild(
							*store, mutation, LogSequenceNumber(12U), [](auto &materialization) {
								assignValidOfflineRebuildFrames(materialization);
								materialization.adjacency_pools.front().locations[1].page_index =
										materialization.adjacency_pools.front().locations.front().page_index;
							}));
				},
				FormatError::FERR_INVALID_STATE);
	}
	{
		auto mutation = validMutation();
		mutation.mapping_tree.new_pool_requests.front().appended_page_count = 2U;
		expectFormatError(
				[&] {
					static_cast<void>(graph::storage::RelocatableMetadataStoreVectorTestAccess::publishOfflineRebuild(
							*store, mutation, LogSequenceNumber(12U), [](auto &materialization) {
								assignValidOfflineRebuildFrames(materialization);
								materialization.mapping_tree_pools.front().appended_locations[1].page_index =
										materialization.mapping_tree_pools.front().appended_locations.front().page_index;
							}));
				},
				FormatError::FERR_INVALID_STATE);
	}
}
