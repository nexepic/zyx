/**
 * @file test_PropertyPagePoolDurability.cpp
 * @brief Publication, recovery, and reclamation tests for native Property page-pool batches.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/core/Node.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;

namespace {

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x7353493287948321ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	EntityPageNewPoolRequest entityPoolRequest(PhysicalSlotId slot = PhysicalSlotId(40U),
										 SegmentId segmentId = SegmentId(31U), PageId pageId = PageId(101U)) {
		return {.segment_id = segmentId,
				.pool_owner_id = 901U,
				.generation = Generation(1U),
				.page_id = pageId,
				.first_slot = slot,
				.last_slot = slot};
	}

	PropertyCollectionRootStoreOptions propertyOptions() {
		return {PhysicalSlotId(41U), PhysicalSlotId(48U), 902U};
	}

	PropertyCollectionRoot sizedRoot(uint64_t nodeId, uint32_t token, size_t encodedBytes, char fill) {
		PropertyCollectionRoot result{
				nodeId, graph::Node::typeId, {{token, graph::PropertyValue(std::string{})}}};
		const size_t baseBytes = EntityPropertyCollectionRootCodec::encode(result).size();
		if (encodedBytes <= baseBytes) {
			throw std::logic_error("Property durability test requested an undersized root");
		}
		result.entries.front().value = graph::PropertyValue(std::string(encodedBytes - baseBytes, fill));
		if (EntityPropertyCollectionRootCodec::encode(result).size() != encodedBytes) {
			throw std::logic_error("Property durability test failed to derive an exact root size");
		}
		return result;
	}

	EntityPageBatchNewPool entityPool(std::span<const uint64_t> nodeIds) {
		EntityPageBatchNewPool result;
		result.request = entityPoolRequest();
		result.record_kind = EntityPageRecordKind::EPRK_NODE;
		result.node_insertions.reserve(nodeIds.size());
		for (const uint64_t nodeId: nodeIds) {
			result.node_insertions.push_back({nodeId, NodeEntityRecord{}});
		}
		return result;
	}

	EntityPropertyKey nodePropertyKey(uint64_t nodeId) {
		return {EntityPropertyOwnerKind::EPOK_NODE, nodeId};
	}

} // namespace

class PropertyPagePoolDurabilityTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<EntityPageStore> entities_;
	std::unique_ptr<SlottedPagePool> pages_;
	std::unique_ptr<PropertyCollectionRootStore> properties_;
	file_handle_t native_handle_ = INVALID_FILE_HANDLE;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
				("property_pool_durability_" + to_string(boost::uuids::random_generator()()));
		file_ = std::make_shared<std::fstream>(
				path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
		rebuildStores();
	}

	void TearDown() override {
		properties_.reset();
		pages_.reset();
		entities_.reset();
		metadata_.reset();
		io_.reset();
		if (native_handle_ != INVALID_FILE_HANDLE) {
			EXPECT_EQ(portable_close_rw(native_handle_), 0);
			native_handle_ = INVALID_FILE_HANDLE;
		}
		if (file_ && file_->is_open()) {
			file_->close();
		}
		file_.reset();
		std::error_code error;
		std::filesystem::remove(path_, error);
	}

	void rebuildStores() {
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
		pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*pages_, *metadata_);
	}

	void reopen() {
		properties_.reset();
		pages_.reset();
		entities_.reset();
		metadata_.reset();
		metadata_ = RelocatableMetadataStore::open(io_);
		rebuildStores();
	}

	void switchToNativeIo() {
		properties_.reset();
		pages_.reset();
		entities_.reset();
		metadata_.reset();
		io_.reset();
		file_->flush();
		native_handle_ = portable_open_rw(path_.string().c_str());
		ASSERT_NE(native_handle_, INVALID_FILE_HANDLE);
		io_ = std::make_shared<StorageIO>(file_, native_handle_, native_handle_);
		metadata_ = RelocatableMetadataStore::open(io_);
		rebuildStores();
	}

	EntityPageBatchResult publish(std::span<const uint64_t> nodeIds,
								  std::span<const PropertyCollectionRoot> roots, LogSequenceNumber checkpointLsn,
								  const MetadataPublishOptions &options = {}) {
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::vector<EntityPropertyRootMutationPlan> plans =
				properties_->planEntityRootReplacements(snapshot, roots, propertyOptions());
		const std::array pools{entityPool(nodeIds)};
		EntityPageBatchResult result = entities_->applyNativeGraphBatchTransaction(
				snapshot, {}, {}, pools, plans, *properties_, propertyOptions(), checkpointLsn, options);
		snapshot.release();
		return result;
	}
};

TEST_F(PropertyPagePoolDurabilityTest, ExistingPoolAppendPublishesOnlyAtACompleteSuperblockBoundary) {
	const std::array<uint64_t, 2U> nodeIds{1001U, 1002U};
	const PropertyCollectionRoot first = sizedRoot(nodeIds[0], 3U, 3000U, 'a');
	static_cast<void>(publish(nodeIds, std::span(&first, 1U), LogSequenceNumber(2U)));

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const std::vector<CatalogEntry> initialPools =
			metadata_->findCurrentPropertyPagePools(before, propertyOptions().page_owner_id);
	ASSERT_EQ(initialPools.size(), 1U);
	const uint8_t initialAvailable = pages_->availableFrameCount(before, initialPools.front().segment_id);
	const PropertyCollectionRoot second = sizedRoot(nodeIds[1], 5U, 2000U, 'b');
	const std::array roots{second};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(before, roots, propertyOptions());
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_EQ(plans.front().kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);

	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
					 before, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U), interrupted)),
				 BootstrapPublishInterrupted);
	before.release();

	reopen();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(recovered, nodePropertyKey(first.owner_entity_id)), first);
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, nodePropertyKey(second.owner_entity_id))
					 .has_value());
	const std::vector<CatalogEntry> recoveredPools =
			metadata_->findCurrentPropertyPagePools(recovered, propertyOptions().page_owner_id);
	ASSERT_EQ(recoveredPools.size(), 1U);
	EXPECT_EQ(recoveredPools.front().segment_id, initialPools.front().segment_id);
	EXPECT_EQ(pages_->availableFrameCount(recovered, recoveredPools.front().segment_id), initialAvailable);

	const std::vector<EntityPropertyRootMutationPlan> retryPlans =
			properties_->planEntityRootReplacements(recovered, roots, propertyOptions());
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
					 recovered, {}, {}, {}, retryPlans, *properties_, propertyOptions(), LogSequenceNumber(3U), interrupted)),
				 BootstrapPublishInterrupted);
	recovered.release();

	reopen();
	MetadataSnapshotPin published = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(published, nodePropertyKey(first.owner_entity_id)), first);
	EXPECT_EQ(properties_->readEntity(published, nodePropertyKey(second.owner_entity_id)), second);
	const std::vector<CatalogEntry> publishedPools =
			metadata_->findCurrentPropertyPagePools(published, propertyOptions().page_owner_id);
	ASSERT_EQ(publishedPools.size(), 1U);
	EXPECT_EQ(pages_->availableFrameCount(published, publishedPools.front().segment_id), initialAvailable - 1U);
}

TEST_F(PropertyPagePoolDurabilityTest, ExistingPoolAppendWriteFailureIsReconciledWithoutLeakingAFrame) {
	const std::array<uint64_t, 2U> nodeIds{1001U, 1002U};
	const PropertyCollectionRoot first = sizedRoot(nodeIds[0], 3U, 3000U, 'a');
	static_cast<void>(publish(nodeIds, std::span(&first, 1U), LogSequenceNumber(2U)));

	MetadataSnapshotPin initial = metadata_->openSnapshot();
	const std::vector<CatalogEntry> initialPools =
			metadata_->findCurrentPropertyPagePools(initial, propertyOptions().page_owner_id);
	ASSERT_EQ(initialPools.size(), 1U);
	const EntityPropertyLocation firstLocation =
			metadata_->findEntityPropertyLocation(initial, nodePropertyKey(first.owner_entity_id));
	ASSERT_EQ(metadata_->findPageLocation(initial, firstLocation.page_id).page_index,
			  RSS_PAGE_POOL_FIRST_DATA_FRAME);
	const uint8_t initialAvailable = pages_->availableFrameCount(initial, initialPools.front().segment_id);
	initial.release();

	switchToNativeIo();
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const uint64_t poolOffset = PhysicalSegmentAllocator::checkedSlotOffset(
			before.superblock().data_region_offset, initialPools.front().physical_slot);
	const uint64_t candidateOffset =
			poolOffset + static_cast<uint64_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U) * RSS_PAGE_SIZE;
	const PropertyCollectionRoot second = sizedRoot(nodeIds[1], 5U, 2000U, 'b');
	const std::array roots{second};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(before, roots, propertyOptions());
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_EQ(plans.front().kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	{
		StorageIO::ScopedNativeWriteHookForTesting failCandidatePage(
				[candidateOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
						 before, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U))),
					 std::system_error);
	}
	before.release();

	reopen();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(recovered, nodePropertyKey(first.owner_entity_id)), first);
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, nodePropertyKey(second.owner_entity_id))
					 .has_value());
	const std::vector<CatalogEntry> recoveredPools =
			metadata_->findCurrentPropertyPagePools(recovered, propertyOptions().page_owner_id);
	ASSERT_EQ(recoveredPools.size(), 1U);
	EXPECT_EQ(recoveredPools.front().segment_id, initialPools.front().segment_id);
	EXPECT_EQ(pages_->availableFrameCount(recovered, recoveredPools.front().segment_id), initialAvailable);
}

TEST_F(PropertyPagePoolDurabilityTest, ExistingPoolAppendDiskFullRetainsThePublishedRootAndCanRetryAfterReopen) {
	const std::array<uint64_t, 2U> nodeIds{1001U, 1002U};
	const PropertyCollectionRoot first = sizedRoot(nodeIds[0], 3U, 3000U, 'a');
	static_cast<void>(publish(nodeIds, std::span(&first, 1U), LogSequenceNumber(2U)));

	MetadataSnapshotPin initial = metadata_->openSnapshot();
	const std::vector<CatalogEntry> initialPools =
			metadata_->findCurrentPropertyPagePools(initial, propertyOptions().page_owner_id);
	ASSERT_EQ(initialPools.size(), 1U);
	const uint8_t initialAvailable = pages_->availableFrameCount(initial, initialPools.front().segment_id);
	initial.release();

	switchToNativeIo();
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const uint64_t poolOffset = PhysicalSegmentAllocator::checkedSlotOffset(
			before.superblock().data_region_offset, initialPools.front().physical_slot);
	const uint64_t candidateOffset =
			poolOffset + static_cast<uint64_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U) * RSS_PAGE_SIZE;
	const PropertyCollectionRoot second = sizedRoot(nodeIds[1], 5U, 2000U, 'b');
	const std::array roots{second};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(before, roots, propertyOptions());
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_EQ(plans.front().kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	{
		StorageIO::ScopedNativeWriteHookForTesting failCandidatePage(
				[candidateOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
						 before, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U))),
					 std::system_error);
	}
	before.release();

	reopen();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(recovered, nodePropertyKey(first.owner_entity_id)), first);
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, nodePropertyKey(second.owner_entity_id))
					 .has_value());
	const std::vector<CatalogEntry> recoveredPools =
			metadata_->findCurrentPropertyPagePools(recovered, propertyOptions().page_owner_id);
	ASSERT_EQ(recoveredPools.size(), 1U);
	EXPECT_EQ(recoveredPools.front().segment_id, initialPools.front().segment_id);
	EXPECT_EQ(pages_->availableFrameCount(recovered, recoveredPools.front().segment_id), initialAvailable);

	const std::vector<EntityPropertyRootMutationPlan> retryPlans =
			properties_->planEntityRootReplacements(recovered, roots, propertyOptions());
	static_cast<void>(entities_->applyNativeGraphBatchTransaction(
			recovered, {}, {}, {}, retryPlans, *properties_, propertyOptions(), LogSequenceNumber(3U)));
	recovered.release();

	MetadataSnapshotPin published = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(published, nodePropertyKey(first.owner_entity_id)), first);
	EXPECT_EQ(properties_->readEntity(published, nodePropertyKey(second.owner_entity_id)), second);
	EXPECT_EQ(pages_->availableFrameCount(published, initialPools.front().segment_id), initialAvailable - 1U);
	published.release();
}

TEST_F(PropertyPagePoolDurabilityTest, ExistingPoolAppendSyncFailureRetainsThePublishedRootAndCanRetryAfterReopen) {
	const std::array<uint64_t, 2U> nodeIds{1001U, 1002U};
	const PropertyCollectionRoot first = sizedRoot(nodeIds[0], 3U, 3000U, 'a');
	static_cast<void>(publish(nodeIds, std::span(&first, 1U), LogSequenceNumber(2U)));

	switchToNativeIo();
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const PropertyCollectionRoot second = sizedRoot(nodeIds[1], 5U, 2000U, 'b');
	const std::array roots{second};
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(before, roots, propertyOptions());
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_EQ(plans.front().kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
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
		EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
						 before, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U))),
					 std::runtime_error);
	}
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 1U);
	before.release();

	reopen();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(recovered, nodePropertyKey(first.owner_entity_id)), first);
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, nodePropertyKey(second.owner_entity_id))
					 .has_value());
	const std::vector<EntityPropertyRootMutationPlan> retryPlans =
			properties_->planEntityRootReplacements(recovered, roots, propertyOptions());
	static_cast<void>(entities_->applyNativeGraphBatchTransaction(
			recovered, {}, {}, {}, retryPlans, *properties_, propertyOptions(), LogSequenceNumber(3U)));
	recovered.release();

	MetadataSnapshotPin published = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(published, nodePropertyKey(first.owner_entity_id)), first);
	EXPECT_EQ(properties_->readEntity(published, nodePropertyKey(second.owner_entity_id)), second);
	published.release();
}

TEST_F(PropertyPagePoolDurabilityTest, ExistingPageReplacementDiskFullRetainsThePublishedRootAndCanRetryAfterReopen) {
	const uint64_t nodeId = 1001U;
	const PropertyCollectionRoot original = sizedRoot(nodeId, 3U, 1800U, 'a');
	static_cast<void>(publish(std::span(&nodeId, 1U), std::span(&original, 1U), LogSequenceNumber(2U)));

	switchToNativeIo();
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const EntityPropertyKey key = nodePropertyKey(nodeId);
	const EntityPropertyLocation oldLocation = metadata_->findEntityPropertyLocation(before, key);
	const PageLocation oldRoute = metadata_->findPageLocation(before, oldLocation.page_id);
	const CatalogEntry pool = metadata_->findCatalog(before, oldRoute.segment_id);
	const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
			before.superblock().data_region_offset, pool.physical_slot) +
			static_cast<uint64_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U) * RSS_PAGE_SIZE;
	const PropertyCollectionRoot replacement = sizedRoot(nodeId, 3U, 1800U, 'b');
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(before, std::span(&replacement, 1U), propertyOptions());
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_EQ(plans.front().kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	{
		StorageIO::ScopedNativeWriteHookForTesting failCandidatePage(
				[candidateOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
					 before, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U))),
				 std::system_error);
	}
	before.release();

	reopen();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(recovered, key), original);
	EXPECT_EQ(metadata_->findPageLocation(recovered, oldLocation.page_id), oldRoute);
	const std::vector<EntityPropertyRootMutationPlan> retryPlans =
			properties_->planEntityRootReplacements(recovered, std::span(&replacement, 1U), propertyOptions());
	static_cast<void>(entities_->applyNativeGraphBatchTransaction(
			recovered, {}, {}, {}, retryPlans, *properties_, propertyOptions(), LogSequenceNumber(3U)));
	recovered.release();

	MetadataSnapshotPin published = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(published, key), replacement);
	EXPECT_EQ(metadata_->findPageLocation(published, oldLocation.page_id).page_index,
			  RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);
	published.release();
}

TEST_F(PropertyPagePoolDurabilityTest, ExistingPageReplacementSyncFailureRetainsThePublishedRootAndCanRetryAfterReopen) {
	const uint64_t nodeId = 1001U;
	const PropertyCollectionRoot original = sizedRoot(nodeId, 3U, 1800U, 'a');
	static_cast<void>(publish(std::span(&nodeId, 1U), std::span(&original, 1U), LogSequenceNumber(2U)));

	switchToNativeIo();
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const EntityPropertyKey key = nodePropertyKey(nodeId);
	const EntityPropertyLocation oldLocation = metadata_->findEntityPropertyLocation(before, key);
	const PageLocation oldRoute = metadata_->findPageLocation(before, oldLocation.page_id);
	const PropertyCollectionRoot replacement = sizedRoot(nodeId, 3U, 1800U, 'b');
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(before, std::span(&replacement, 1U), propertyOptions());
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_EQ(plans.front().kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
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
		EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
					 before, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U))),
				 std::runtime_error);
	}
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 1U);
	before.release();

	reopen();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(recovered, key), original);
	EXPECT_EQ(metadata_->findPageLocation(recovered, oldLocation.page_id), oldRoute);
	const std::vector<EntityPropertyRootMutationPlan> retryPlans =
			properties_->planEntityRootReplacements(recovered, std::span(&replacement, 1U), propertyOptions());
	static_cast<void>(entities_->applyNativeGraphBatchTransaction(
			recovered, {}, {}, {}, retryPlans, *properties_, propertyOptions(), LogSequenceNumber(3U)));
	recovered.release();

	MetadataSnapshotPin published = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(published, key), replacement);
	EXPECT_EQ(metadata_->findPageLocation(published, oldLocation.page_id).page_index,
			  RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);
	published.release();
}

TEST_F(PropertyPagePoolDurabilityTest,
		   ExistingPageReplacementMetadataDiskFullRetainsThePublishedRootAndCanRetryAfterReopen) {
	const uint64_t nodeId = 1001U;
	const PropertyCollectionRoot original = sizedRoot(nodeId, 3U, 1800U, 'a');
	static_cast<void>(publish(std::span(&nodeId, 1U), std::span(&original, 1U), LogSequenceNumber(2U)));

	switchToNativeIo();
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const EntityPropertyKey key = nodePropertyKey(nodeId);
	const EntityPropertyLocation oldLocation = metadata_->findEntityPropertyLocation(before, key);
	const PageLocation oldRoute = metadata_->findPageLocation(before, oldLocation.page_id);
	const uint64_t metadataPoolOffset = before.superblock().data_region_offset;
	const PropertyCollectionRoot replacement = sizedRoot(nodeId, 3U, 1800U, 'b');
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(before, std::span(&replacement, 1U), propertyOptions());
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_EQ(plans.front().kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	{
		StorageIO::ScopedNativeWriteHookForTesting failMetadataCandidate(
				[metadataPoolOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
					if (absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
						absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE && byteCount == RSS_PAGE_SIZE) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
						 before, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U))),
				 std::runtime_error);
	}
	before.release();

	reopen();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(recovered, key), original);
	EXPECT_EQ(metadata_->findPageLocation(recovered, oldLocation.page_id), oldRoute);
	const std::vector<EntityPropertyRootMutationPlan> retryPlans =
			properties_->planEntityRootReplacements(recovered, std::span(&replacement, 1U), propertyOptions());
	static_cast<void>(entities_->applyNativeGraphBatchTransaction(
			recovered, {}, {}, {}, retryPlans, *properties_, propertyOptions(), LogSequenceNumber(3U)));
	recovered.release();

	MetadataSnapshotPin published = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(published, key), replacement);
	EXPECT_EQ(metadata_->findPageLocation(published, oldLocation.page_id).page_index,
			  RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);
}

TEST_F(PropertyPagePoolDurabilityTest,
		   ExistingPageReplacementFinalSyncFailureReportsUncertainDurabilityAndReopensTheCompleteRoot) {
	const uint64_t nodeId = 1001U;
	const PropertyCollectionRoot original = sizedRoot(nodeId, 3U, 1800U, 'a');
	static_cast<void>(publish(std::span(&nodeId, 1U), std::span(&original, 1U), LogSequenceNumber(2U)));

	switchToNativeIo();
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const EntityPropertyKey key = nodePropertyKey(nodeId);
	const EntityPropertyLocation oldLocation = metadata_->findEntityPropertyLocation(before, key);
	const PropertyCollectionRoot replacement = sizedRoot(nodeId, 3U, 1800U, 'b');
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(before, std::span(&replacement, 1U), propertyOptions());
	ASSERT_EQ(plans.size(), 1U);
	ASSERT_EQ(plans.front().kind, EntityPropertyRootMutationPlanKind::EPRMPK_EXISTING_PAGE);
	std::atomic<uint32_t> syncCalls{0U};
	{
		StorageIO::ScopedNativeSyncHookForTesting failFinalFence([&syncCalls](file_handle_t fd) {
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
		EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
						 before, {}, {}, {}, plans, *properties_, propertyOptions(), LogSequenceNumber(3U))),
				 std::runtime_error);
	}
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	before.release();

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(current, key), replacement);
	EXPECT_EQ(metadata_->findPageLocation(current, oldLocation.page_id).page_index,
			  RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);
	current.release();

	reopen();
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntity(reopened, key), replacement);
	EXPECT_EQ(metadata_->findPageLocation(reopened, oldLocation.page_id).page_index,
			  RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U);
}

TEST_F(PropertyPagePoolDurabilityTest, FreshMultiPagePoolIsRecoveredAsAllOrNothing) {
	const std::array<uint64_t, 2U> nodeIds{1001U, 1002U};
	const std::array roots{
			sizedRoot(nodeIds[0], 3U, 2500U, 'a'),
			sizedRoot(nodeIds[1], 5U, 2500U, 'b'),
	};
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const std::vector<EntityPropertyRootMutationPlan> plans =
			properties_->planEntityRootReplacements(before, roots, propertyOptions());
	ASSERT_EQ(plans.size(), roots.size());
	for (const EntityPropertyRootMutationPlan &plan: plans) {
		ASSERT_EQ(plan.kind, EntityPropertyRootMutationPlanKind::EPRMPK_NEW_PAGE_REQUIRED);
	}
	const std::array pools{entityPool(nodeIds)};
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
					 before, {}, {}, pools, plans, *properties_, propertyOptions(), LogSequenceNumber(2U), interrupted)),
				 BootstrapPublishInterrupted);
	before.release();

	reopen();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(recovered, nodeIds[0]).has_value());
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(recovered, nodeIds[1]).has_value());
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, nodePropertyKey(nodeIds[0])).has_value());
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, nodePropertyKey(nodeIds[1])).has_value());
	EXPECT_TRUE(metadata_->findCurrentPropertyPagePools(recovered, propertyOptions().page_owner_id).empty());
	EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(recovered, propertyOptions().first_slot));

	const std::vector<EntityPropertyRootMutationPlan> retryPlans =
			properties_->planEntityRootReplacements(recovered, roots, propertyOptions());
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
					 recovered, {}, {}, pools, retryPlans, *properties_, propertyOptions(), LogSequenceNumber(2U), interrupted)),
				 BootstrapPublishInterrupted);
	recovered.release();

	reopen();
	MetadataSnapshotPin published = metadata_->openSnapshot();
	const EntityPropertyLocation first =
			metadata_->findEntityPropertyLocation(published, nodePropertyKey(nodeIds[0]));
	const EntityPropertyLocation second =
			metadata_->findEntityPropertyLocation(published, nodePropertyKey(nodeIds[1]));
	EXPECT_NE(first.page_id, second.page_id);
	EXPECT_EQ(metadata_->findPageLocation(published, first.page_id).segment_id,
			  metadata_->findPageLocation(published, second.page_id).segment_id);
	EXPECT_EQ(properties_->readEntity(published, first.key), roots[0]);
	EXPECT_EQ(properties_->readEntity(published, second.key), roots[1]);
}

TEST_F(PropertyPagePoolDurabilityTest, MultiPagePoolReclaimsAfterSnapshotsDrainAndReusesSlotAcrossKinds) {
	const std::array<uint64_t, 2U> nodeIds{1001U, 1002U};
	const std::array roots{
			sizedRoot(nodeIds[0], 3U, 2500U, 'a'),
			sizedRoot(nodeIds[1], 5U, 2500U, 'b'),
	};
	static_cast<void>(publish(nodeIds, roots, LogSequenceNumber(2U)));

	MetadataSnapshotPin oldSnapshot = metadata_->openSnapshot();
	const EntityPropertyLocation first =
			metadata_->findEntityPropertyLocation(oldSnapshot, nodePropertyKey(nodeIds[0]));
	const EntityPropertyLocation second =
			metadata_->findEntityPropertyLocation(oldSnapshot, nodePropertyKey(nodeIds[1]));
	ASSERT_NE(first.page_id, second.page_id);
	const CatalogEntry propertyPool =
			metadata_->findCatalog(oldSnapshot, metadata_->findPageLocation(oldSnapshot, first.page_id).segment_id);
	ASSERT_EQ(propertyPool.segment_id, metadata_->findPageLocation(oldSnapshot, second.page_id).segment_id);
	const uint8_t unavailableBeforeReclaim =
			static_cast<uint8_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT - pages_->availableFrameCount(oldSnapshot, propertyPool.segment_id));
	ASSERT_EQ(unavailableBeforeReclaim, 2U);

	std::array<PropertyCollectionRoot, 2U> emptyRoots{roots[0], roots[1]};
	emptyRoots[0].entries.clear();
	emptyRoots[1].entries.clear();
	const std::vector<EntityPropertyRootMutationPlan> clearPlans =
			properties_->planEntityRootReplacements(oldSnapshot, emptyRoots, propertyOptions());
	ASSERT_EQ(clearPlans.size(), emptyRoots.size());
	for (const EntityPropertyRootMutationPlan &plan: clearPlans) {
		ASSERT_TRUE(plan.page_replacement.has_value());
		EXPECT_TRUE(plan.page_replacement->retire_page);
	}
	static_cast<void>(entities_->applyNativeGraphBatchTransaction(
			oldSnapshot, {}, {}, {}, clearPlans, *properties_, propertyOptions(), LogSequenceNumber(3U)));

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(current, first.key).has_value());
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(current, second.key).has_value());
	EXPECT_EQ(properties_->readEntity(oldSnapshot, first.key), roots[0]);
	EXPECT_EQ(properties_->readEntity(oldSnapshot, second.key), roots[1]);
	const uint8_t availableWhilePinned = pages_->availableFrameCount(current, propertyPool.segment_id);
	pages_->reclaimRetiredFrames(propertyPool.segment_id);
	EXPECT_EQ(pages_->availableFrameCount(current, propertyPool.segment_id), availableWhilePinned);
	current.release();
	oldSnapshot.release();

	pages_->reclaimRetiredFrames(propertyPool.segment_id);
	MetadataSnapshotPin reclaimed = metadata_->openSnapshot();
	EXPECT_EQ(pages_->availableFrameCount(reclaimed, propertyPool.segment_id), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	reclaimed.release();
	static_cast<void>(pages_->retireEmptyPool(
			propertyPool.segment_id, propertyPool.generation, LogSequenceNumber(4U)));
	static_cast<void>(pages_->releaseRetiredEmptyPool(
			propertyPool.segment_id, propertyPool.generation, LogSequenceNumber(5U)));

	MetadataSnapshotPin released = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->isPhysicalSlotAllocated(released, propertyPool.physical_slot));
	released.release();
	const EntityPageNewPoolRequest reused =
			entityPoolRequest(propertyPool.physical_slot, SegmentId(200U), PageId(300U));
	ASSERT_TRUE(entities_->allocateNode(reused, 1003U, NodeEntityRecord{}, LogSequenceNumber(6U)).has_value());
	MetadataSnapshotPin reusedSnapshot = metadata_->openSnapshot();
	const CatalogEntry reusedPool = metadata_->findCatalog(reusedSnapshot, reused.segment_id);
	EXPECT_EQ(reusedPool.physical_slot, propertyPool.physical_slot);
	EXPECT_EQ(reusedPool.kind, SegmentKind::SGK_ENTITY_PAGE_POOL);
}
