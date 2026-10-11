/**
 * @file test_RelocatableProcessCrash_BTree.cpp
 * @brief Real child-process crash coverage for native B+Tree publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <cerrno>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/ScalarIndexTreeReachabilityReclaimer.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "storage/relocatable/format/RelocatableProcessCrashSupport.hpp"
#include "tests/src/storage/relocatable/index/RelocatableBTreePageStoreTestUtils.hpp"

using namespace graph::storage;
using namespace relocatable_btree_test;

namespace {

	constexpr std::string_view CRASH_CHILD_BTREE_MATERIALIZATION_CANDIDATE_SHORT_WRITE =
			"btree-materialization-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_BTREE_MATERIALIZATION_CANDIDATE = "btree-materialization-candidate";
	constexpr std::string_view CRASH_CHILD_BTREE_MATERIALIZATION_METADATA_CANDIDATE_SHORT_WRITE =
			"btree-materialization-metadata-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_BTREE_RETIREMENT_METADATA_CANDIDATE_SHORT_WRITE =
			"btree-retirement-metadata-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_BTREE_CROSS_POOL_RETIREMENT_METADATA_CANDIDATE_SHORT_WRITE =
			"btree-cross-pool-retirement-metadata-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_BTREE_STRUCTURAL_MUTATION_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC =
			"btree-structural-mutation-after-superblock-write-before-final-sync";
	constexpr std::string_view CRASH_CHILD_BTREE_STRUCTURAL_MUTATION_CANDIDATE_SHORT_WRITE =
			"btree-structural-mutation-candidate-short-write";
	constexpr std::string_view CRASH_CHILD_BTREE_ROOT_EVACUATION_CANDIDATE =
			"btree-root-evacuation-candidate";
	constexpr size_t STRUCTURAL_KEY_BYTES = 1800U;

	[[nodiscard]] std::array<IndexTreeMaterializationPoolOptions, 2U> crossPoolOptions() {
		return {{{PhysicalSlotId(40U), PhysicalSlotId(41U), 17U, RSS_PAGE_POOL_DATA_FRAME_COUNT},
				 {PhysicalSlotId(40U), PhysicalSlotId(41U), 17U, 2U}}};
	}

	[[nodiscard]] BTreePage buildCrossPoolTreePage(std::span<const IndexTreeMaterializedPool> pools,
														  size_t poolOrdinal, size_t pageOrdinal,
														  LogSequenceNumber checkpointLsn) {
		const IndexTreeMaterializedPool &pool = pools[poolOrdinal];
		const PageId pageId = pool.page_ids[pageOrdinal];
		BTreePageHeader header;
		header.index_id = 17U;
		header.page_id = pageId;
		header.generation = pool.pool.generation;
		if (poolOrdinal == 1U && pageOrdinal == 0U) {
			header.kind = BTreeNodeKind::BPN_INTERNAL;
			header.level = 1U;
			header.leftmost_child = pools[0].page_ids[0U];
			BTreePage root = BTreePage::create(treeOwner(pool.pool, pageId, checkpointLsn), header);
			root.insert({{std::byte{'m'}}, pools[1U].page_ids[1U].value()});
			return root;
		}
		if (poolOrdinal == 0U && pageOrdinal == 0U) {
			header.right_sibling = pools[1U].page_ids[1U];
			BTreePage leaf = BTreePage::create(treeOwner(pool.pool, pageId, checkpointLsn), header);
			leaf.insert({{std::byte{'a'}}, 1U});
			return leaf;
		}
		header.left_sibling = pools[0U].page_ids[0U];
		BTreePage leaf = BTreePage::create(treeOwner(pool.pool, pageId, checkpointLsn), header);
		leaf.insert({{std::byte{'z'}}, 2U});
		return leaf;
	}

	[[nodiscard]] std::vector<std::byte> structuralKey(uint16_t value) {
		std::vector<std::byte> key(STRUCTURAL_KEY_BYTES, std::byte{0x5AU});
		key[0] = static_cast<std::byte>(value >> 8U);
		key[1] = static_cast<std::byte>(value);
		return key;
	}

	[[nodiscard]] BTreePage buildStructuralCrossPoolTreePage(std::span<const IndexTreeMaterializedPool> pools,
				size_t poolOrdinal, size_t pageOrdinal, LogSequenceNumber checkpointLsn) {
		const IndexTreeMaterializedPool &pool = pools[poolOrdinal];
		const PageId pageId = pool.page_ids[pageOrdinal];
		const std::vector<std::byte> firstKey = structuralKey(0x0100U);
		const std::vector<std::byte> secondKey = structuralKey(0x0180U);
		const std::vector<std::byte> rightKey = structuralKey(0x0300U);
		BTreePageHeader header;
		header.index_id = 17U;
		header.page_id = pageId;
		header.generation = pool.pool.generation;
		if (poolOrdinal == 1U && pageOrdinal == 0U) {
			header.kind = BTreeNodeKind::BPN_INTERNAL;
			header.level = 1U;
			header.leftmost_child = pools[0U].page_ids[0U];
			BTreePage root = BTreePage::create(treeOwner(pool.pool, pageId, checkpointLsn), header);
			root.insert({rightKey, pools[1U].page_ids[1U].value()});
			return root;
		}
		if (poolOrdinal == 0U && pageOrdinal == 0U) {
			header.right_sibling = pools[1U].page_ids[1U];
			BTreePage leaf = BTreePage::create(treeOwner(pool.pool, pageId, checkpointLsn), header);
			leaf.insert({firstKey, 1U});
			leaf.insert({secondKey, 2U});
			return leaf;
		}
		header.left_sibling = pools[0U].page_ids[0U];
		BTreePage leaf = BTreePage::create(treeOwner(pool.pool, pageId, checkpointLsn), header);
		leaf.insert({rightKey, 3U});
		return leaf;
	}

	[[nodiscard]] LogSequenceNumber nextCheckpointLsn(RelocatableMetadataStore &metadata) {
		const MetadataSnapshotPin snapshot = metadata.openSnapshot();
		return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
	}

	[[nodiscard]] NativeGraphWritePlannerOptions nativeWriteOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(95U);
		options.entity_pool_owner_id = 9401U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(95U), 9402U};
		return options;
	}

	[[nodiscard]] ScalarIndexTreeReclaimOptions reclaimOptions() {
		return {8U, 8U, 8U, 16U, 16U, 16U};
	}

	[[nodiscard]] IndexDefinition createDroppedIndex(RelocatableMetadataStore &metadata, SlottedPagePool &pages,
			BTreePageStore &trees, SystemCatalog &catalog, NativeGraphWritePlanner &writes) {
		IndexDefinition definition{0U, "crash_retired_index", IndexDefinitionEntityKind::IDEK_NODE,
								 IndexDefinitionKind::IDK_PROPERTY, 7U, {"score"}, 0U, {}};
		NativeGraphWriteTransaction create = writes.beginTransaction();
		SystemCatalogWriteSession createSession = catalog.beginWrite(create);
		definition = createSession.createIndexDefinition(std::move(definition));
		createSession.finish();
		if (!create.commit().committed()) {
			throw std::runtime_error("crash-test child cannot publish scalar Index definition");
		}

		const LogSequenceNumber treeLsn = nextCheckpointLsn(metadata);
		const SlottedPagePoolAllocationRequest request{
				SegmentId(71U), SegmentKind::SGK_INDEX_PAGE_POOL, definition.index_id, Generation(1U), PageId(101U),
				definition.index_id, PageOwnerKind::POK_INDEX};
		PageOwnerHeader owner;
		owner.page_id = request.page_id;
		owner.owner_id = definition.index_id;
		owner.kind = request.page_owner_kind;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = request.generation;
		owner.committed_lsn = treeLsn;
		BTreePageHeader header;
		header.index_id = definition.index_id;
		header.page_id = request.page_id;
		header.generation = request.generation;
		BTreePage page = BTreePage::create(owner, header);
		page.insert({{std::byte{'a'}}, 71U});
		if (!trees.allocate(request, page, PhysicalSlotId(40U), PhysicalSlotId(95U), treeLsn).has_value()) {
			throw std::runtime_error("crash-test child cannot allocate obsolete scalar Index tree");
		}
		const IndexRootLocation root{definition.index_id, request.page_id, request.generation};
		static_cast<void>(trees.publishRoots(std::span(&root, 1U), {}, nextCheckpointLsn(metadata)));

		NativeGraphWriteTransaction drop = writes.beginTransaction();
		SystemCatalogWriteSession dropSession = catalog.beginWrite(drop);
		dropSession.dropIndexDefinition(definition);
		dropSession.finish();
		if (!drop.commit().committed()) {
			throw std::runtime_error("crash-test child cannot tombstone scalar Index definition");
		}
		return definition;
	}

	using graph::test::relocatable_crash::CrashChild;
	using graph::test::relocatable_crash::waitToBeKilled;
	using graph::test::relocatable_crash::writeFailureMarker;
	using graph::test::relocatable_crash::writeMarker;

} // namespace

namespace graph::test {

	int runRelocatableBTreeCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
									  const std::filesystem::path &markerPath) {
		const bool killMetadataCandidate = mode == CRASH_CHILD_BTREE_MATERIALIZATION_METADATA_CANDIDATE_SHORT_WRITE;
		const bool killCandidate = mode == CRASH_CHILD_BTREE_MATERIALIZATION_CANDIDATE;
		const bool killRetirementMetadataCandidate =
				mode == CRASH_CHILD_BTREE_RETIREMENT_METADATA_CANDIDATE_SHORT_WRITE;
		const bool killCrossPoolRetirementMetadataCandidate =
				mode == CRASH_CHILD_BTREE_CROSS_POOL_RETIREMENT_METADATA_CANDIDATE_SHORT_WRITE;
		const bool killStructuralMutationAfterSuperblockWrite =
				mode == CRASH_CHILD_BTREE_STRUCTURAL_MUTATION_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC;
		const bool killStructuralMutationCandidate = mode == CRASH_CHILD_BTREE_STRUCTURAL_MUTATION_CANDIDATE_SHORT_WRITE;
		const bool killRootEvacuationCandidate = mode == CRASH_CHILD_BTREE_ROOT_EVACUATION_CANDIDATE;
		if (mode != CRASH_CHILD_BTREE_MATERIALIZATION_CANDIDATE_SHORT_WRITE && !killCandidate && !killMetadataCandidate &&
			!killRetirementMetadataCandidate && !killCrossPoolRetirementMetadataCandidate &&
			!killStructuralMutationAfterSuperblockWrite && !killStructuralMutationCandidate && !killRootEvacuationCandidate)
			return -1;
		try {
			auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary |
																			 std::ios::trunc);
			if (!file->is_open())
				return 90;
			const file_handle_t nativeHandle = portable_open_rw(databasePath.string().c_str());
			if (nativeHandle == INVALID_FILE_HANDLE)
				return 91;
			auto io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
			auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
			SlottedPagePool pool(io, *metadata);
			BTreePageStore store(pool, *metadata);
			if (killRootEvacuationCandidate) {
				const SlottedPagePoolAllocationRequest sourceRequest = allocationRequest();
				if (!store.allocate(sourceRequest, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U),
													PhysicalSlotId(40U), LogSequenceNumber(2U))
							 .has_value()) {
					return 95;
				}
				const IndexRootLocation sourceRoot{17U, sourceRequest.page_id, sourceRequest.generation};
				static_cast<void>(store.publishRoots(std::span(&sourceRoot, 1U), {}, LogSequenceNumber(3U)));

				SlottedPagePoolAllocationRequest destinationRequest = sourceRequest;
				destinationRequest.segment_id = SegmentId(72U);
				destinationRequest.pool_owner_id = 78U;
				destinationRequest.generation = Generation(2U);
				destinationRequest.page_id = PageId(102U);
				PageOwnerHeader destinationOwner = pageOwner(LogSequenceNumber(4U));
				destinationOwner.page_id = destinationRequest.page_id;
				destinationOwner.generation = destinationRequest.generation;
				BTreePageHeader destinationHeader;
				destinationHeader.index_id = destinationOwner.owner_id;
				destinationHeader.page_id = destinationOwner.page_id;
				destinationHeader.generation = destinationOwner.generation;
				if (!store.allocate(destinationRequest, BTreePage::create(destinationOwner, destinationHeader),
													PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(4U))
							 .has_value()) {
					return 96;
				}
				MetadataSnapshotPin sourceSnapshot = metadata->openSnapshot();
				const BTreePage root = store.read(sourceSnapshot, sourceRequest.page_id);
				const CatalogEntry destinationPool = metadata->findCatalog(sourceSnapshot, destinationRequest.segment_id);
				const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
						sourceSnapshot.superblock().data_region_offset, destinationPool.physical_slot) +
						static_cast<uint64_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U) * RSS_PAGE_SIZE;
				sourceSnapshot.release();

				StorageIO::ScopedNativeWriteHookForTesting pauseAfterDurableCandidate(
						[candidateOffset, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
												  pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t persisted = portable_pwrite(fd, bytes, byteCount, offset);
								if (persisted != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) {
									return persisted;
								}
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				static_cast<void>(store.evacuate(root, destinationRequest.segment_id, LogSequenceNumber(5U)));
				return 97;
			}
			if (killCrossPoolRetirementMetadataCandidate) {
				static_cast<void>(store.materializeAndPublishTreeAcrossPools(
						crossPoolOptions(), 17U, {1U, 0U}, LogSequenceNumber(2U),
						[](std::span<const IndexTreeMaterializedPool> pools, size_t poolOrdinal, size_t pageOrdinal) {
							return buildCrossPoolTreePage(pools, poolOrdinal, pageOrdinal, LogSequenceNumber(2U));
						}));
				EntityPageStore entities(io, *metadata);
				EntityLabelSetStore labels(io, *metadata, pool);
				PropertyCollectionRootStore properties(pool, *metadata);
				NativeGraphWritePlanner writes(*metadata, entities, labels, properties, store, nativeWriteOptions());
				StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
						[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
							if (byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16U) return partial;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				NativeGraphWriteTransaction transaction = writes.beginTransaction();
				transaction.stageScalarIndexDrop(store, 17U, 3U);
				static_cast<void>(transaction.commit());
				return 93;
			}
			if (killRetirementMetadataCandidate) {
				EntityPageStore entities(io, *metadata);
				EntityLabelSetStore labels(io, *metadata, pool);
				PropertyCollectionRootStore properties(pool, *metadata);
				NativeGraphWritePlanner writes(*metadata, entities, labels, properties, store, nativeWriteOptions());
				SystemCatalog catalog(store, *metadata);
				VectorManifestStore vectorManifests(io, *metadata);
				const IndexDefinition definition = createDroppedIndex(*metadata, pool, store, catalog, writes);
				StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
						[&markerPath](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
							// The setup is complete before the hook is installed. Retiring an
							// existing B+Tree page only writes a 64-byte pool-control record
							// before this first full native metadata COW candidate.
							if (byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16U) return partial;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				ScalarIndexTreeReachabilityReclaimer reclaimer(store, catalog, vectorManifests, *metadata);
				static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata)));
				static_cast<void>(definition);
				return 93;
			}
			if (killStructuralMutationAfterSuperblockWrite || killStructuralMutationCandidate) {
				static_cast<void>(store.materializeAndPublishTreeAcrossPools(
						crossPoolOptions(), 17U, {1U, 0U}, LogSequenceNumber(2U),
						[](std::span<const IndexTreeMaterializedPool> pools, size_t poolOrdinal, size_t pageOrdinal) {
							return buildStructuralCrossPoolTreePage(pools, poolOrdinal, pageOrdinal,
																								 LogSequenceNumber(2U));
						}));
				const std::vector<std::byte> insertedKey = structuralKey(0x0140U);
				EntityPageStore entities(io, *metadata);
				EntityLabelSetStore labels(io, *metadata, pool);
				PropertyCollectionRootStore properties(pool, *metadata);
				NativeGraphWritePlanner writes(*metadata, entities, labels, properties, store, nativeWriteOptions());
				StorageIO::ScopedNativeWriteHookForTesting interruptStructuralMutation(
						[killStructuralMutationAfterSuperblockWrite, killStructuralMutationCandidate,
						 &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
																				pwrite_off_t offset) {
							// The staged operation is only this B+Tree split. Its first full
							// page write is therefore a fresh structural candidate, before
							// any metadata publication can make it reachable.
							if (killStructuralMutationCandidate && byteCount == RSS_PAGE_SIZE) {
								const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
								if (partial != 16U) return partial;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							if (killStructuralMutationAfterSuperblockWrite && offset >= 0 &&
								static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
								byteCount == RSS_SUPERBLOCK_BYTES) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount)) return written;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				NativeGraphWriteTransaction transaction = writes.beginTransaction();
				transaction.stageScalarIndexUpsert(store, 17U, insertedKey, 14U);
				static_cast<void>(transaction.commit());
				return 93;
			}
			const SlottedPagePoolAllocationRequest request = allocationRequest();
			if (!store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
								LogSequenceNumber(2U))
						 .has_value()) {
				return 92;
			}
			const IndexRootLocation oldRoot{17U, request.page_id, request.generation};
			static_cast<void>(store.publishRoots(std::span(&oldRoot, 1U), {}, LogSequenceNumber(3U)));
			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			const CatalogEntry indexPool = metadata->findCatalog(snapshot, request.segment_id);
			const uint64_t candidateOffset =
					PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																	indexPool.physical_slot) +
					static_cast<uint64_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U) * RSS_PAGE_SIZE;
			const uint64_t metadataPoolOffset = snapshot.superblock().data_region_offset;
			snapshot.release();
			StorageIO::ScopedNativeWriteHookForTesting shortCandidate(
					[candidateOffset, metadataPoolOffset, killCandidate, killMetadataCandidate, &markerPath](
							file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
						const bool reachesTarget =
								killMetadataCandidate
										? absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
												  absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE
										: absoluteOffset == candidateOffset;
						if (reachesTarget && byteCount == RSS_PAGE_SIZE) {
							if (killCandidate) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) return written;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
							if (partial != 16U)
								return partial;
							writeMarker(markerPath);
							waitToBeKilled();
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			static_cast<void>(
					store.materializeAndPublishTree(request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
													[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
														return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
													}));
			return 93;
		} catch (const std::exception &error) {
			writeFailureMarker(markerPath, error.what());
			return 94;
		}
	}

} // namespace graph::test

class RelocatableProcessCrashBTreeTest : public ::testing::Test {
protected:
	std::filesystem::path databasePath;
	std::filesystem::path markerPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		databasePath =
				std::filesystem::temp_directory_path() / ("relocatable_btree_process_crash_" + to_string(uuid) + ".db");
		markerPath = databasePath.string() + ".marker";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(databasePath, error);
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
	}
};

TEST_F(RelocatableProcessCrashBTreeTest, KillDuringTreeMaterializationCandidateShortWriteRetainsOldRootAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BTREE_MATERIALIZATION_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	const IndexRootLocation oldRoot{17U, request.page_id, request.generation};
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(recovered, 17U), oldRoot);
	EXPECT_FALSE(metadata->findPageLocationOptional(recovered, PageId(102U)).has_value());
	recovered.release();

	static_cast<void>(
			store.materializeAndPublishTree(request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
											[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
												return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
											}));
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(published, 17U).root_page_id, PageId(102U));
	BTreeIndex index(17U, store, *metadata);
	EXPECT_EQ(index.find(published, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
	published.release();
}

TEST_F(RelocatableProcessCrashBTreeTest, KillDuringTreeMaterializationCandidateRetainsOldRootAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BTREE_MATERIALIZATION_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(recovered, 17U), (IndexRootLocation{17U, request.page_id, request.generation}));
	EXPECT_FALSE(metadata->findPageLocationOptional(recovered, PageId(102U)).has_value());
	recovered.release();
	static_cast<void>(store.materializeAndPublishTree(request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) { return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U)); }));
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(published, 17U).root_page_id, PageId(102U));
}

TEST_F(RelocatableProcessCrashBTreeTest,
		KillAfterDurableRootEvacuationCandidateRetainsBothDirectoriesAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BTREE_ROOT_EVACUATION_CANDIDATE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest sourceRequest = allocationRequest();
	const IndexRootLocation sourceRoot{17U, sourceRequest.page_id, sourceRequest.generation};
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const BTreePage source = store.read(recovered, sourceRequest.page_id);
	EXPECT_EQ(metadata->findIndexRoot(recovered, 17U), sourceRoot);
	EXPECT_EQ(metadata->findPageLocation(recovered, sourceRequest.page_id).segment_id, sourceRequest.segment_id);
	EXPECT_EQ(source.header().generation, sourceRequest.generation);
	recovered.release();

	const SharedPagePoolAllocation retried = store.evacuate(source, SegmentId(72U), LogSequenceNumber(5U));
	EXPECT_EQ(retried.location.segment_id, SegmentId(72U));
	EXPECT_EQ(retried.location.generation, Generation(2U));
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(published, 17U),
			  (IndexRootLocation{17U, sourceRequest.page_id, Generation(2U)}));
	EXPECT_EQ(metadata->findPageLocation(published, sourceRequest.page_id).segment_id, SegmentId(72U));
	EXPECT_EQ(metadata->findPageLocation(published, sourceRequest.page_id).page_index,
			  RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);
	EXPECT_EQ(store.read(published, sourceRequest.page_id).header().generation, Generation(2U));
	published.release();

	auto reopenedFile = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(reopenedFile->is_open());
	auto reopenedIo = std::make_shared<StorageIO>(reopenedFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto reopenedMetadata = RelocatableMetadataStore::open(reopenedIo);
	SlottedPagePool reopenedPool(reopenedIo, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(reopened, 17U),
			  (IndexRootLocation{17U, sourceRequest.page_id, Generation(2U)}));
	EXPECT_EQ(reopenedStore.read(reopened, sourceRequest.page_id).header().generation, Generation(2U));
}

TEST_F(RelocatableProcessCrashBTreeTest,
		KillDuringTreeRetirementMetadataCandidateShortWriteRetainsOldRootAndCanRetryAtomically) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BTREE_RETIREMENT_METADATA_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SlottedPagePool pages(io, *metadata);
	BTreePageStore trees(pages, *metadata);
	SystemCatalog catalog(trees, *metadata);
	VectorManifestStore vectorManifests(io, *metadata);

	MetadataSnapshotPin recovered = metadata->openSnapshot();
	std::optional<IndexRootLocation> retiredRoot;
	metadata->visitIndexRoots(recovered, [&retiredRoot](const IndexRootLocation &root) {
		if (isSystemCatalogIndexDefinitionId(root.index_id)) retiredRoot = root;
	});
	ASSERT_TRUE(retiredRoot.has_value());
	EXPECT_EQ(BTreeIndex(retiredRoot->index_id, trees, *metadata).find(
				  recovered, std::vector<std::byte>{std::byte{'a'}}),
			  std::optional<uint64_t>(71U));
	recovered.release();

	ScalarIndexTreeReachabilityReclaimer reclaimer(trees, catalog, vectorManifests, *metadata);
	const ScalarIndexTreeReclaimReport retry = reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata));
	EXPECT_EQ(retry.dropped_index_roots_retired, 1U);
	EXPECT_EQ(retry.index_tree_pages_retired, 1U);
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(published, retiredRoot->index_id).has_value());
}

TEST_F(RelocatableProcessCrashBTreeTest,
		KillDuringCrossPoolTreeRetirementMetadataCandidateShortWriteRetainsOldRootAndCanRetryAtomically) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BTREE_CROSS_POOL_RETIREMENT_METADATA_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SlottedPagePool pages(io, *metadata);
	BTreePageStore trees(pages, *metadata);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	const IndexRootLocation oldRoot = metadata->findIndexRoot(recovered, 17U);
	const BTreePage oldRootPage = trees.read(recovered, oldRoot.root_page_id);
	EXPECT_EQ(oldRootPage.header().kind, BTreeNodeKind::BPN_INTERNAL);
	BTreeIndex index(17U, trees, *metadata);
	EXPECT_EQ(index.find(recovered, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
	EXPECT_EQ(index.find(recovered, std::vector<std::byte>{std::byte{'z'}}), std::optional<uint64_t>(2U));
	recovered.release();

	EntityPageStore entities(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, pages);
	PropertyCollectionRootStore properties(pages, *metadata);
	NativeGraphWritePlanner writes(*metadata, entities, labels, properties, trees, nativeWriteOptions());
	NativeGraphWriteTransaction retry = writes.beginTransaction();
	retry.stageScalarIndexDrop(trees, 17U, 3U);
	static_cast<void>(retry.commit());
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(published, 17U).has_value());
	published.release();
}

TEST_F(RelocatableProcessCrashBTreeTest,
		KillDuringStructuralMutationCandidateShortWriteRetainsTheCompleteOldCrossPoolTreeAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BTREE_STRUCTURAL_MUTATION_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SlottedPagePool pages(io, *metadata);
	BTreePageStore trees(pages, *metadata);
	BTreeIndex index(17U, trees, *metadata);
	const std::vector<std::byte> firstKey = structuralKey(0x0100U);
	const std::vector<std::byte> insertedKey = structuralKey(0x0140U);
	const std::vector<std::byte> secondKey = structuralKey(0x0180U);
	const std::vector<std::byte> rightKey = structuralKey(0x0300U);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(index.find(recovered, firstKey), std::optional<uint64_t>(1U));
	EXPECT_EQ(index.find(recovered, insertedKey), std::nullopt);
	EXPECT_EQ(index.find(recovered, secondKey), std::optional<uint64_t>(2U));
	EXPECT_EQ(index.find(recovered, rightKey), std::optional<uint64_t>(3U));
	EXPECT_EQ(index.scan(recovered, std::nullopt, 8U).size(), 3U);
	recovered.release();

	EntityPageStore entities(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, pages);
	PropertyCollectionRootStore properties(pages, *metadata);
	NativeGraphWritePlanner writes(*metadata, entities, labels, properties, trees, nativeWriteOptions());
	NativeGraphWriteTransaction retry = writes.beginTransaction();
	retry.stageScalarIndexUpsert(trees, 17U, insertedKey, 14U);
	ASSERT_TRUE(retry.commit().committed());
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(index.find(published, firstKey), std::optional<uint64_t>(1U));
	EXPECT_EQ(index.find(published, insertedKey), std::optional<uint64_t>(14U));
	EXPECT_EQ(index.find(published, secondKey), std::optional<uint64_t>(2U));
	EXPECT_EQ(index.find(published, rightKey), std::optional<uint64_t>(3U));
	EXPECT_EQ(index.scan(published, std::nullopt, 8U).size(), 4U);
	published.release();
}

TEST_F(RelocatableProcessCrashBTreeTest,
		KillAfterStructuralMutationSuperblockWriteBeforeFinalSyncRecoversOnlyACompleteTreeGeneration) {
	CrashChild child(databasePath, markerPath,
				 CRASH_CHILD_BTREE_STRUCTURAL_MUTATION_AFTER_SUPERBLOCK_WRITE_BEFORE_FINAL_SYNC);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SlottedPagePool pages(io, *metadata);
	BTreePageStore trees(pages, *metadata);
	BTreeIndex index(17U, trees, *metadata);
	const std::vector<std::byte> firstKey = structuralKey(0x0100U);
	const std::vector<std::byte> insertedKey = structuralKey(0x0140U);
	const std::vector<std::byte> secondKey = structuralKey(0x0180U);
	const std::vector<std::byte> rightKey = structuralKey(0x0300U);
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(index.find(recovered, firstKey), std::optional<uint64_t>(1U));
	EXPECT_EQ(index.find(recovered, secondKey), std::optional<uint64_t>(2U));
	EXPECT_EQ(index.find(recovered, rightKey), std::optional<uint64_t>(3U));
	const std::optional<uint64_t> recoveredInserted = index.find(recovered, insertedKey);
	EXPECT_TRUE(!recoveredInserted.has_value() || recoveredInserted == std::optional<uint64_t>(14U));
	const size_t recoveredEntries = index.scan(recovered, std::nullopt, 8U).size();
	EXPECT_TRUE(recoveredEntries == 3U || recoveredEntries == 4U);
	recovered.release();

	EntityPageStore entities(io, *metadata);
	EntityLabelSetStore labels(io, *metadata, pages);
	PropertyCollectionRootStore properties(pages, *metadata);
	NativeGraphWritePlanner writes(*metadata, entities, labels, properties, trees, nativeWriteOptions());
	NativeGraphWriteTransaction retry = writes.beginTransaction();
	retry.stageScalarIndexUpsert(trees, 17U, insertedKey, 14U);
	ASSERT_TRUE(retry.commit().committed());
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(index.find(published, firstKey), std::optional<uint64_t>(1U));
	EXPECT_EQ(index.find(published, insertedKey), std::optional<uint64_t>(14U));
	EXPECT_EQ(index.find(published, secondKey), std::optional<uint64_t>(2U));
	EXPECT_EQ(index.find(published, rightKey), std::optional<uint64_t>(3U));
	EXPECT_EQ(index.scan(published, std::nullopt, 8U).size(), 4U);
	published.release();
}

TEST_F(RelocatableProcessCrashBTreeTest,
		TreeRetirementMetadataDiskFullRetainsOldRootAndCanRetryAtomicallyAfterReopen) {
	{
		std::fstream create(databasePath, std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(create.is_open());
	}
	const file_handle_t nativeHandle = portable_open_rw(databasePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	IndexRootLocation retiredRoot;
	{
		auto file = std::make_shared<std::fstream>(databasePath,
				std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		auto io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore trees(pages, *metadata);
		EntityPageStore entities(io, *metadata);
		EntityLabelSetStore labels(io, *metadata, pages);
		PropertyCollectionRootStore properties(pages, *metadata);
		NativeGraphWritePlanner writes(*metadata, entities, labels, properties, trees, nativeWriteOptions());
		SystemCatalog catalog(trees, *metadata);
		VectorManifestStore vectorManifests(io, *metadata);

		const IndexDefinition definition = createDroppedIndex(*metadata, pages, trees, catalog, writes);
		MetadataSnapshotPin before = metadata->openSnapshot();
		retiredRoot = metadata->findIndexRoot(before, definition.index_id);
		EXPECT_EQ(BTreeIndex(definition.index_id, trees, *metadata).find(
				  before, std::vector<std::byte>{std::byte{'a'}}),
				  std::optional<uint64_t>(71U));
		before.release();

		{
			StorageIO::ScopedNativeWriteHookForTesting failMetadataCandidate(
					[](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						// Tree-page retirement changes pool control with a fixed-size record,
						// then COW-publishes the root removal through a full metadata page.
						if (byteCount == RSS_PAGE_SIZE) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			ScalarIndexTreeReachabilityReclaimer reclaimer(trees, catalog, vectorManifests, *metadata);
			EXPECT_THROW(static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))),
						 std::system_error);
		}

		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(metadata->findIndexRoot(current, definition.index_id), retiredRoot);
		EXPECT_EQ(BTreeIndex(definition.index_id, trees, *metadata).find(
				  current, std::vector<std::byte>{std::byte{'a'}}),
				  std::optional<uint64_t>(71U));
		current.release();
	}

	auto recoveredFile = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(recoveredFile->is_open());
	auto recoveredIo = std::make_shared<StorageIO>(recoveredFile, nativeHandle, nativeHandle);
	auto recoveredMetadata = RelocatableMetadataStore::open(recoveredIo);
	SlottedPagePool recoveredPages(recoveredIo, *recoveredMetadata);
	BTreePageStore recoveredTrees(recoveredPages, *recoveredMetadata);
	SystemCatalog recoveredCatalog(recoveredTrees, *recoveredMetadata);
	VectorManifestStore recoveredVectorManifests(recoveredIo, *recoveredMetadata);
	MetadataSnapshotPin recovered = recoveredMetadata->openSnapshot();
	EXPECT_EQ(recoveredMetadata->findIndexRoot(recovered, retiredRoot.index_id), retiredRoot);
	EXPECT_EQ(BTreeIndex(retiredRoot.index_id, recoveredTrees, *recoveredMetadata).find(
			  recovered, std::vector<std::byte>{std::byte{'a'}}),
			  std::optional<uint64_t>(71U));
	recovered.release();

	ScalarIndexTreeReachabilityReclaimer retryReclaimer(recoveredTrees, recoveredCatalog, recoveredVectorManifests,
															*recoveredMetadata);
	const ScalarIndexTreeReclaimReport retry = retryReclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*recoveredMetadata));
	EXPECT_EQ(retry.dropped_index_roots_retired, 1U);
	EXPECT_EQ(retry.index_tree_pages_retired, 1U);
	MetadataSnapshotPin published = recoveredMetadata->openSnapshot();
	EXPECT_FALSE(recoveredMetadata->findIndexRootOptional(published, retiredRoot.index_id).has_value());
	published.release();
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableProcessCrashBTreeTest,
		TreeRetirementFirstDataFenceFailureRetainsOldRootAndCanRetryAtomicallyAfterReopen) {
	{
		std::fstream create(databasePath, std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(create.is_open());
	}
	const file_handle_t nativeHandle = portable_open_rw(databasePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	IndexRootLocation retiredRoot;
	{
		auto file = std::make_shared<std::fstream>(databasePath,
				std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		auto io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore trees(pages, *metadata);
		EntityPageStore entities(io, *metadata);
		EntityLabelSetStore labels(io, *metadata, pages);
		PropertyCollectionRootStore properties(pages, *metadata);
		NativeGraphWritePlanner writes(*metadata, entities, labels, properties, trees, nativeWriteOptions());
		SystemCatalog catalog(trees, *metadata);
		VectorManifestStore vectorManifests(io, *metadata);

		const IndexDefinition definition = createDroppedIndex(*metadata, pages, trees, catalog, writes);
		MetadataSnapshotPin before = metadata->openSnapshot();
		retiredRoot = metadata->findIndexRoot(before, definition.index_id);
		before.release();

		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failFirstDataFence([&syncCalls](file_handle_t fd) {
				if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
#ifdef _WIN32
					SetLastError(ERROR_DISK_FULL);
#else
					errno = ENOSPC;
#endif
					return -1;
				}
				return portable_fsync(fd);
			});
			ScalarIndexTreeReachabilityReclaimer reclaimer(trees, catalog, vectorManifests, *metadata);
			EXPECT_THROW(static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))),
						 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 1U);
		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(metadata->findIndexRoot(current, definition.index_id), retiredRoot);
		current.release();
	}

	auto recoveredFile = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(recoveredFile->is_open());
	auto recoveredIo = std::make_shared<StorageIO>(recoveredFile, nativeHandle, nativeHandle);
	auto recoveredMetadata = RelocatableMetadataStore::open(recoveredIo);
	SlottedPagePool recoveredPages(recoveredIo, *recoveredMetadata);
	BTreePageStore recoveredTrees(recoveredPages, *recoveredMetadata);
	SystemCatalog recoveredCatalog(recoveredTrees, *recoveredMetadata);
	VectorManifestStore recoveredVectorManifests(recoveredIo, *recoveredMetadata);
	ScalarIndexTreeReachabilityReclaimer retryReclaimer(recoveredTrees, recoveredCatalog, recoveredVectorManifests,
															*recoveredMetadata);
	const ScalarIndexTreeReclaimReport retry = retryReclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*recoveredMetadata));
	EXPECT_EQ(retry.dropped_index_roots_retired, 1U);
	MetadataSnapshotPin published = recoveredMetadata->openSnapshot();
	EXPECT_FALSE(recoveredMetadata->findIndexRootOptional(published, retiredRoot.index_id).has_value());
	published.release();
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableProcessCrashBTreeTest,
		TreeRetirementFinalPublicationFenceReportsUncertainDurabilityAndReopensAtomically) {
	{
		std::fstream create(databasePath, std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(create.is_open());
	}
	const file_handle_t nativeHandle = portable_open_rw(databasePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	IndexRootLocation retiredRoot;
	{
		auto file = std::make_shared<std::fstream>(databasePath,
				std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		auto io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore trees(pages, *metadata);
		EntityPageStore entities(io, *metadata);
		EntityLabelSetStore labels(io, *metadata, pages);
		PropertyCollectionRootStore properties(pages, *metadata);
		NativeGraphWritePlanner writes(*metadata, entities, labels, properties, trees, nativeWriteOptions());
		SystemCatalog catalog(trees, *metadata);
		VectorManifestStore vectorManifests(io, *metadata);

		const IndexDefinition definition = createDroppedIndex(*metadata, pages, trees, catalog, writes);
		MetadataSnapshotPin before = metadata->openSnapshot();
		retiredRoot = metadata->findIndexRoot(before, definition.index_id);
		before.release();

		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence([&syncCalls](file_handle_t fd) {
				if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 1U) {
#ifdef _WIN32
					SetLastError(ERROR_DISK_FULL);
#else
					errno = ENOSPC;
#endif
					return -1;
				}
				return portable_fsync(fd);
			});
			ScalarIndexTreeReachabilityReclaimer reclaimer(trees, catalog, vectorManifests, *metadata);
			EXPECT_THROW(static_cast<void>(reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*metadata))),
						 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);

		// The final fence is after the complete inactive Superblock write. The caller
		// gets an error because durability is uncertain, while the published state is
		// nevertheless never a mixture of old root and retired tree frames.
		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_FALSE(metadata->findIndexRootOptional(current, definition.index_id).has_value());
		current.release();
	}

	auto recoveredFile = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(recoveredFile->is_open());
	auto recoveredIo = std::make_shared<StorageIO>(recoveredFile, nativeHandle, nativeHandle);
	auto recoveredMetadata = RelocatableMetadataStore::open(recoveredIo);
	SlottedPagePool recoveredPages(recoveredIo, *recoveredMetadata);
	BTreePageStore recoveredTrees(recoveredPages, *recoveredMetadata);
	SystemCatalog recoveredCatalog(recoveredTrees, *recoveredMetadata);
	VectorManifestStore recoveredVectorManifests(recoveredIo, *recoveredMetadata);
	MetadataSnapshotPin recovered = recoveredMetadata->openSnapshot();
	EXPECT_FALSE(recoveredMetadata->findIndexRootOptional(recovered, retiredRoot.index_id).has_value());
	recovered.release();
	ScalarIndexTreeReachabilityReclaimer reclaimer(recoveredTrees, recoveredCatalog, recoveredVectorManifests,
															  *recoveredMetadata);
	const ScalarIndexTreeReclaimReport second = reclaimer.reclaim(reclaimOptions(), nextCheckpointLsn(*recoveredMetadata));
	EXPECT_EQ(second.index_tree_pages_retired, 0U);
	EXPECT_EQ(second.published_generation, second.scanned_generation);
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableProcessCrashBTreeTest,
	   KillDuringTreeMaterializationMetadataCandidateShortWriteRetainsOldRootAndCanRetry) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BTREE_MATERIALIZATION_METADATA_CANDIDATE_SHORT_WRITE);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	const IndexRootLocation oldRoot{17U, request.page_id, request.generation};
	MetadataSnapshotPin recovered = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(recovered, 17U), oldRoot);
	EXPECT_FALSE(metadata->findPageLocationOptional(recovered, PageId(102U)).has_value());
	recovered.release();

	static_cast<void>(
			store.materializeAndPublishTree(request.segment_id, 17U, 2U, 0U, LogSequenceNumber(4U),
											[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
												return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4U));
											}));
	MetadataSnapshotPin published = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(published, 17U).root_page_id, PageId(102U));
	BTreeIndex index(17U, store, *metadata);
	EXPECT_EQ(index.find(published, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
	published.release();
}
