/**
 * @file test_NativeGraphWritePlanner_Durability.cpp
 * @brief Native I/O durability coverage for compound graph transactions.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <atomic>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <system_error>

#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePage.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;
using graph::Edge;
using graph::Node;
using graph::PropertyValue;

namespace {

	NativeGraphWritePlannerOptions durabilityPlannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 901U;
		options.logical_id_lease_block_size = 1U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
		return options;
	}

	PropertyCollectionRoot propertyRoot(uint64_t ownerId, uint32_t ownerType, int64_t value) {
		return {ownerId, ownerType, {{17U, PropertyValue(value)}}};
	}

	MetadataPublishOptions withBootstrapFailpoint(BootstrapPublishFailpoint failpoint) {
		MetadataPublishOptions options;
		options.bootstrap_failpoint = failpoint;
		return options;
	}

	IndexRootLocation allocateEmptyIndexRoot(BTreePageStore &indexes) {
		const SlottedPagePoolAllocationRequest allocation{
				.segment_id = SegmentId(71U),
				.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL,
				.pool_owner_id = 17U,
				.generation = Generation(1U),
				.page_id = PageId(101U),
				.page_owner_id = 17U,
				.page_owner_kind = PageOwnerKind::POK_INDEX,
		};
		PageOwnerHeader owner;
		owner.page_id = allocation.page_id;
		owner.owner_id = allocation.page_owner_id;
		owner.kind = allocation.page_owner_kind;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = allocation.generation;
		owner.committed_lsn = LogSequenceNumber(2U);
		BTreePageHeader header;
		header.index_id = allocation.page_owner_id;
		header.page_id = allocation.page_id;
		header.generation = allocation.generation;
		if (!indexes.allocate(allocation, BTreePage::create(owner, header), PhysicalSlotId(40U), PhysicalSlotId(40U),
							  LogSequenceNumber(2U))
					 .has_value()) {
			throw std::runtime_error("test index root allocation unexpectedly ran out of space");
		}
		const IndexRootLocation root{17U, allocation.page_id, allocation.generation};
		static_cast<void>(indexes.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));
		return root;
	}

	NativeGraphDerivedStateMutation stableRootLeafMutation(const BTreeRootLeafMutationPlan &plan) {
		NativeGraphIndexTreeMutation treeMutation;
		treeMutation.index_id = plan.expected_root.index_id;
		treeMutation.expected_root = plan.expected_root;
		treeMutation.pool_requests.push_back({plan.expected_pool.segment_id, {plan.expected_location}, 0U, {}});
		treeMutation.stable_replacement_root = plan.expected_root.root_page_id;
		NativeGraphDerivedStateMutation result;
		result.index_tree_mutations.push_back(std::move(treeMutation));
		return result;
	}

	NativeGraphDerivedStatePreparer stableRootLeafPreparer(BTreePageStore &indexes,
														   const BTreeRootLeafMutationPlan &plan) {
		return [&indexes, plan](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
			if (batches.size() != 1U || batches.front().size() != 1U ||
				batches.front().front().expected_replacements.size() != 1U) {
				throw std::runtime_error("test root-leaf derived-state materialization received an invalid batch");
			}
			IndexTreeMutationPoolBatch &batch = batches.front().front();
			indexes.prepareRootLeafMutation(plan, batch.pool, batch.expected_replacements.front(),
											batch.replacement_locations.front(), LogSequenceNumber(4U));
		};
	}

	bool isCandidatePageWithOwnerKind(const void *bytes, size_t byteCount, PageOwnerKind ownerKind) {
		if (byteCount != RSS_PAGE_SIZE) {
			return false;
		}
		try {
			const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
			return PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES)).kind == ownerKind;
		} catch (const std::exception &) {
			return false;
		}
	}

	bool isCandidateIndexPageForId(const void *bytes, size_t byteCount, uint64_t indexId) {
		if (byteCount != RSS_PAGE_SIZE) {
			return false;
		}
		try {
			const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
			const PageOwnerHeader owner = PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
			if (owner.kind != PageOwnerKind::POK_INDEX) {
				return false;
			}
			return BTreePage::open(owner, page.subspan(RSS_PAGE_OWNER_HEADER_BYTES, owner.payload_bytes)).header().index_id ==
				   indexId;
		} catch (const std::exception &) {
			return false;
		}
	}

} // namespace

class NativeGraphWritePlannerDurabilityTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
	file_handle_t native_handle_ = INVALID_FILE_HANDLE;

	void switchToNativeIo() {
		vector_manifests_.reset();
		vector_pages_.reset();
		properties_.reset();
		labels_.reset();
		btree_pages_.reset();
		slotted_pages_.reset();
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

	void rebuildStores() {
		entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
		slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
		btree_pages_ = std::make_unique<BTreePageStore>(*slotted_pages_, *metadata_);
		labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_);
		properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);
		vector_pages_ = std::make_unique<VectorPageStore>(io_, *metadata_);
		vector_manifests_ = std::make_unique<VectorManifestStore>(io_, *metadata_);
	}

	void reopenNativeStores() {
		vector_manifests_.reset();
		vector_pages_.reset();
		properties_.reset();
		labels_.reset();
		btree_pages_.reset();
		slotted_pages_.reset();
		entities_.reset();
		metadata_.reset();
		metadata_ = RelocatableMetadataStore::open(io_);
		rebuildStores();
	}

	void TearDown() override {
		vector_manifests_.reset();
		vector_pages_.reset();
		properties_.reset();
		labels_.reset();
		btree_pages_.reset();
		slotted_pages_.reset();
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
};

TEST_F(NativeGraphWritePlannerDurabilityTest,
	   DiskFullDuringCompoundGraphCandidateRetainsTheOldGenerationAndCanRetryAfterReopen) {
	switchToNativeIo();
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									durabilityPlannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t firstNode = transaction.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNode = transaction.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t edge = transaction.createEdge(firstNode, secondNode, 11U);
	transaction.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
	transaction.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation baseGeneration = before.superblock().generation;
	const uint64_t candidateOffset =
			PhysicalSegmentAllocator::checkedSlotOffset(before.superblock().data_region_offset, PhysicalSlotId(40U)) +
			static_cast<uint64_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME) * RSS_PAGE_SIZE;
	before.release();
	{
		StorageIO::ScopedNativeWriteHookForTesting failFirstEntityCandidate(
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
		EXPECT_THROW(static_cast<void>(transaction.commit()), std::system_error);
	}
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	// Node and Edge IDs are leased before mutation materialization, so their
	// durable high-water publication legitimately precedes this failed batch.
	// The failed graph mutation must not advance beyond that pinned generation.
	EXPECT_EQ(recovered.superblock().generation, baseGeneration);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, firstNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, secondNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(recovered, edge)), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_FALSE(
			metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_NODE, firstNode})
					.has_value());
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, edge})
						 .has_value());
	recovered.release();

	NativeGraphWritePlanner retryPlanner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
										 durabilityPlannerOptions());
	NativeGraphWriteTransaction retry = retryPlanner.beginTransaction();
	const uint64_t retryFirstNode = retry.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t retrySecondNode = retry.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t retryEdge = retry.createEdge(retryFirstNode, retrySecondNode, 11U);
	retry.replaceNodeProperty(propertyRoot(retryFirstNode, Node::typeId, 42));
	retry.replaceEdgeProperty(propertyRoot(retryEdge, Edge::typeId, 84));
	ASSERT_TRUE(retry.commit().committed());

	MetadataSnapshotPin published = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_OUT, retryFirstNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_IN, retrySecondNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_NODE, retryFirstNode}),
			  propertyRoot(retryFirstNode, Node::typeId, 42));
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_EDGE, retryEdge}),
			  propertyRoot(retryEdge, Edge::typeId, 84));
	published.release();
}

TEST_F(NativeGraphWritePlannerDurabilityTest,
	   PrePublicationSyncFailureDuringCompoundGraphRetainsTheOldGenerationAndCanRetryAfterReopen) {
	switchToNativeIo();
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									durabilityPlannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t firstNode = transaction.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNode = transaction.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t edge = transaction.createEdge(firstNode, secondNode, 11U);
	transaction.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
	transaction.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation baseGeneration = before.superblock().generation;
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
		EXPECT_THROW(static_cast<void>(transaction.commit()), std::runtime_error);
	}
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 1U);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(current.superblock().generation, baseGeneration);
	EXPECT_THROW(static_cast<void>(entities_->readNode(current, firstNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readNode(current, secondNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(current, edge)), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(current, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_FALSE(
			metadata_->findEntityPropertyLocationOptional(current, {EntityPropertyOwnerKind::EPOK_NODE, firstNode})
					.has_value());
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(current, {EntityPropertyOwnerKind::EPOK_EDGE, edge})
					.has_value());
	current.release();

	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, baseGeneration);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, firstNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, secondNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(recovered, edge)), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_FALSE(
			metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_NODE, firstNode})
					.has_value());
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, edge})
					.has_value());
	recovered.release();

	NativeGraphWritePlanner retryPlanner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									 durabilityPlannerOptions());
	NativeGraphWriteTransaction retry = retryPlanner.beginTransaction();
	const uint64_t retryFirstNode = retry.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t retrySecondNode = retry.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t retryEdge = retry.createEdge(retryFirstNode, retrySecondNode, 11U);
	retry.replaceNodeProperty(propertyRoot(retryFirstNode, Node::typeId, 42));
	retry.replaceEdgeProperty(propertyRoot(retryEdge, Edge::typeId, 84));
	ASSERT_TRUE(retry.commit().committed());

	MetadataSnapshotPin published = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_OUT, retryFirstNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_IN, retrySecondNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_NODE, retryFirstNode}),
			  propertyRoot(retryFirstNode, Node::typeId, 42));
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_EDGE, retryEdge}),
			  propertyRoot(retryEdge, Edge::typeId, 84));
	published.release();
}

TEST_F(NativeGraphWritePlannerDurabilityTest,
	   DiskFullDuringCompoundGraphPropertyCandidateRetainsTheOldGenerationAndCanRetryAfterReopen) {
	switchToNativeIo();
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									durabilityPlannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t firstNode = transaction.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNode = transaction.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t edge = transaction.createEdge(firstNode, secondNode, 11U);
	transaction.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
	transaction.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation baseGeneration = before.superblock().generation;
	before.release();
	std::atomic_bool failedPropertyCandidate{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting failFirstPropertyCandidate(
				[&failedPropertyCandidate](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (isCandidatePageWithOwnerKind(bytes, byteCount, PageOwnerKind::POK_PROPERTY) &&
						!failedPropertyCandidate.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(transaction.commit()), std::system_error);
	}
	ASSERT_TRUE(failedPropertyCandidate.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, baseGeneration);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, firstNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, secondNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(recovered, edge)), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_FALSE(
			metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_NODE, firstNode})
					.has_value());
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, edge})
					.has_value());
	recovered.release();

	NativeGraphWritePlanner retryPlanner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									 durabilityPlannerOptions());
	NativeGraphWriteTransaction retry = retryPlanner.beginTransaction();
	const uint64_t retryFirstNode = retry.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t retrySecondNode = retry.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t retryEdge = retry.createEdge(retryFirstNode, retrySecondNode, 11U);
	retry.replaceNodeProperty(propertyRoot(retryFirstNode, Node::typeId, 42));
	retry.replaceEdgeProperty(propertyRoot(retryEdge, Edge::typeId, 84));
	ASSERT_TRUE(retry.commit().committed());

	MetadataSnapshotPin published = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_OUT, retryFirstNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_IN, retrySecondNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_NODE, retryFirstNode}),
			  propertyRoot(retryFirstNode, Node::typeId, 42));
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_EDGE, retryEdge}),
			  propertyRoot(retryEdge, Edge::typeId, 84));
	published.release();
}

TEST_F(NativeGraphWritePlannerDurabilityTest,
	   DiskFullDuringCompoundGraphLabelSetCandidateRetainsTheOldGenerationAndCanRetryAfterReopen) {
	switchToNativeIo();
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									durabilityPlannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<uint64_t, 4U> overflowLabels = {7U, 8U, 9U, 10U};
	const uint64_t firstNode = transaction.createNode(overflowLabels);
	const uint64_t secondNode = transaction.createNode(std::array<uint64_t, 1U>{11U});
	const uint64_t edge = transaction.createEdge(firstNode, secondNode, 12U);
	transaction.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
	transaction.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation baseGeneration = before.superblock().generation;
	before.release();
	std::atomic_bool failedLabelSetCandidate{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting failFirstLabelSetCandidate(
				[&failedLabelSetCandidate](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (isCandidatePageWithOwnerKind(bytes, byteCount, PageOwnerKind::POK_ENTITY_LABEL) &&
						!failedLabelSetCandidate.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(transaction.commit()), std::system_error);
	}
	ASSERT_TRUE(failedLabelSetCandidate.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, baseGeneration);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, firstNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, secondNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(recovered, edge)), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_FALSE(
			metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_NODE, firstNode})
					.has_value());
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, edge})
					 .has_value());
	recovered.release();

	NativeGraphWritePlanner retryPlanner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									 durabilityPlannerOptions());
	NativeGraphWriteTransaction retry = retryPlanner.beginTransaction();
	const uint64_t retryFirstNode = retry.createNode(overflowLabels);
	const uint64_t retrySecondNode = retry.createNode(std::array<uint64_t, 1U>{11U});
	const uint64_t retryEdge = retry.createEdge(retryFirstNode, retrySecondNode, 12U);
	retry.replaceNodeProperty(propertyRoot(retryFirstNode, Node::typeId, 42));
	retry.replaceEdgeProperty(propertyRoot(retryEdge, Edge::typeId, 84));
	ASSERT_TRUE(retry.commit().committed());

	MetadataSnapshotPin published = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(labels_->readNodeLabels(published, entities_->readNode(published, retryFirstNode)),
			  std::vector<uint64_t>(overflowLabels.begin(), overflowLabels.end()));
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_OUT, retryFirstNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_IN, retrySecondNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_NODE, retryFirstNode}),
			  propertyRoot(retryFirstNode, Node::typeId, 42));
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_EDGE, retryEdge}),
			  propertyRoot(retryEdge, Edge::typeId, 84));
	published.release();
}

TEST_F(NativeGraphWritePlannerDurabilityTest,
	   DiskFullDuringCompoundGraphAdjacencyCandidateRetainsTheOldGenerationAndCanRetryAfterReopen) {
	switchToNativeIo();
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									durabilityPlannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t firstNode = transaction.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNode = transaction.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t edge = transaction.createEdge(firstNode, secondNode, 11U);
	transaction.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
	transaction.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation baseGeneration = before.superblock().generation;
	before.release();
	std::atomic_bool failedAdjacencyCandidate{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting failFirstAdjacencyCandidate(
				[&failedAdjacencyCandidate](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (isCandidatePageWithOwnerKind(bytes, byteCount, PageOwnerKind::POK_INDEX) &&
						!failedAdjacencyCandidate.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(transaction.commit()), std::system_error);
	}
	ASSERT_TRUE(failedAdjacencyCandidate.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, baseGeneration);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, firstNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, secondNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(recovered, edge)), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_FALSE(
			metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_NODE, firstNode})
					.has_value());
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, edge})
					.has_value());
	recovered.release();

	NativeGraphWritePlanner retryPlanner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									 durabilityPlannerOptions());
	NativeGraphWriteTransaction retry = retryPlanner.beginTransaction();
	const uint64_t retryFirstNode = retry.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t retrySecondNode = retry.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t retryEdge = retry.createEdge(retryFirstNode, retrySecondNode, 11U);
	retry.replaceNodeProperty(propertyRoot(retryFirstNode, Node::typeId, 42));
	retry.replaceEdgeProperty(propertyRoot(retryEdge, Edge::typeId, 84));
	ASSERT_TRUE(retry.commit().committed());

	MetadataSnapshotPin published = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_OUT, retryFirstNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_IN, retrySecondNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_NODE, retryFirstNode}),
			  propertyRoot(retryFirstNode, Node::typeId, 42));
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_EDGE, retryEdge}),
			  propertyRoot(retryEdge, Edge::typeId, 84));
	published.release();
}

TEST_F(NativeGraphWritePlannerDurabilityTest,
	   DiskFullDuringCompoundGraphScalarIndexCandidateRetainsTheOldGenerationAndCanRetryAfterReopen) {
	constexpr uint64_t scalarIndexId = 17U;
	const std::array<std::byte, 1U> key = {std::byte{'s'}};
	switchToNativeIo();
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									durabilityPlannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t firstNode = transaction.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNode = transaction.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t edge = transaction.createEdge(firstNode, secondNode, 11U);
	transaction.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
	transaction.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));
	const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), firstNode}}};
	transaction.stageScalarIndexCreate(*btree_pages_, scalarIndexId, entries);

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation baseGeneration = before.superblock().generation;
	before.release();
	std::atomic_bool failedScalarIndexCandidate{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting failFirstScalarIndexCandidate(
				[&failedScalarIndexCandidate](file_handle_t fd, const void *bytes, size_t byteCount,
															 pwrite_off_t offset) {
					if (isCandidateIndexPageForId(bytes, byteCount, scalarIndexId) &&
						!failedScalarIndexCandidate.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(transaction.commit()), std::system_error);
	}
	ASSERT_TRUE(failedScalarIndexCandidate.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, baseGeneration);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, firstNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, secondNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(recovered, edge)), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, scalarIndexId).has_value());
	EXPECT_FALSE(
			metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_NODE, firstNode})
					.has_value());
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, edge})
					.has_value());
	recovered.release();

	NativeGraphWritePlanner retryPlanner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									 durabilityPlannerOptions());
	NativeGraphWriteTransaction retry = retryPlanner.beginTransaction();
	const uint64_t retryFirstNode = retry.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t retrySecondNode = retry.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t retryEdge = retry.createEdge(retryFirstNode, retrySecondNode, 11U);
	retry.replaceNodeProperty(propertyRoot(retryFirstNode, Node::typeId, 42));
	retry.replaceEdgeProperty(propertyRoot(retryEdge, Edge::typeId, 84));
	const std::array<BTreePageEntry, 1U> retryEntries{{
				{std::vector<std::byte>(key.begin(), key.end()), retryFirstNode}}};
	retry.stageScalarIndexCreate(*btree_pages_, scalarIndexId, retryEntries);
	ASSERT_TRUE(retry.commit().committed());

	MetadataSnapshotPin published = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	BTreeIndex scalarIndex(scalarIndexId, *btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_OUT, retryFirstNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_IN, retrySecondNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(scalarIndex.find(published, key), std::optional<uint64_t>(retryFirstNode));
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_NODE, retryFirstNode}),
			  propertyRoot(retryFirstNode, Node::typeId, 42));
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_EDGE, retryEdge}),
			  propertyRoot(retryEdge, Edge::typeId, 84));
	published.release();
}

TEST_F(NativeGraphWritePlannerDurabilityTest,
	   DiskFullDuringCompoundGraphMetadataCandidateRetainsTheOldGenerationAndCanRetryAfterReopen) {
	constexpr uint64_t scalarIndexId = 17U;
	const std::array<std::byte, 1U> key = {std::byte{'s'}};
	switchToNativeIo();
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									durabilityPlannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<uint64_t, 4U> overflowLabels = {7U, 8U, 9U, 10U};
	const uint64_t firstNode = transaction.createNode(overflowLabels);
	const uint64_t secondNode = transaction.createNode(std::array<uint64_t, 1U>{11U});
	const uint64_t edge = transaction.createEdge(firstNode, secondNode, 12U);
	transaction.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
	transaction.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));
	const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), firstNode}}};
	transaction.stageScalarIndexCreate(*btree_pages_, scalarIndexId, entries);

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation baseGeneration = before.superblock().generation;
	const uint64_t metadataPoolOffset = before.superblock().data_region_offset;
	before.release();
	std::atomic_bool failedMetadataCandidate{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting failFirstMetadataCandidate(
				[metadataPoolOffset, &failedMetadataCandidate](file_handle_t fd, const void *bytes, size_t byteCount,
																	pwrite_off_t offset) {
					const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
					if (byteCount == RSS_PAGE_SIZE &&
						absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
						absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE &&
						!failedMetadataCandidate.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(transaction.commit()), std::system_error);
	}
	ASSERT_TRUE(failedMetadataCandidate.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, baseGeneration);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, firstNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, secondNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(recovered, edge)), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, scalarIndexId).has_value());
	EXPECT_FALSE(
			metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_NODE, firstNode})
					.has_value());
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, edge})
					 .has_value());
	recovered.release();

	NativeGraphWritePlanner retryPlanner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									 durabilityPlannerOptions());
	NativeGraphWriteTransaction retry = retryPlanner.beginTransaction();
	const uint64_t retryFirstNode = retry.createNode(overflowLabels);
	const uint64_t retrySecondNode = retry.createNode(std::array<uint64_t, 1U>{11U});
	const uint64_t retryEdge = retry.createEdge(retryFirstNode, retrySecondNode, 12U);
	retry.replaceNodeProperty(propertyRoot(retryFirstNode, Node::typeId, 42));
	retry.replaceEdgeProperty(propertyRoot(retryEdge, Edge::typeId, 84));
	const std::array<BTreePageEntry, 1U> retryEntries{{
			{std::vector<std::byte>(key.begin(), key.end()), retryFirstNode}}};
	retry.stageScalarIndexCreate(*btree_pages_, scalarIndexId, retryEntries);
	ASSERT_TRUE(retry.commit().committed());

	MetadataSnapshotPin published = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	BTreeIndex scalarIndex(scalarIndexId, *btree_pages_, *metadata_);
	EXPECT_EQ(labels_->readNodeLabels(published, entities_->readNode(published, retryFirstNode)),
			  std::vector<uint64_t>(overflowLabels.begin(), overflowLabels.end()));
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_OUT, retryFirstNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_IN, retrySecondNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(scalarIndex.find(published, key), std::optional<uint64_t>(retryFirstNode));
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_NODE, retryFirstNode}),
			  propertyRoot(retryFirstNode, Node::typeId, 42));
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_EDGE, retryEdge}),
			  propertyRoot(retryEdge, Edge::typeId, 84));
	published.release();
}

TEST_F(NativeGraphWritePlannerDurabilityTest,
	   PostPublicationSyncFailureDuringCompoundGraphReportsUncertainDurabilityAndReopensCompleteGeneration) {
	switchToNativeIo();
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									durabilityPlannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t firstNode = transaction.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNode = transaction.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t edge = transaction.createEdge(firstNode, secondNode, 11U);
	transaction.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
	transaction.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));

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
		EXPECT_THROW(static_cast<void>(transaction.commit()), std::runtime_error);
	}
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	MetadataSnapshotPin current = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	NodeEntityRecord expectedFirstNode;
	expectedFirstNode.inline_label_ids.front() = 7U;
	expectedFirstNode.inline_label_count = 1U;
	NodeEntityRecord expectedSecondNode;
	expectedSecondNode.inline_label_ids.front() = 9U;
	expectedSecondNode.inline_label_count = 1U;
	const EdgeEntityRecord expectedEdge{firstNode, secondNode, 11U};
	EXPECT_EQ(entities_->readNode(current, firstNode), expectedFirstNode);
	EXPECT_EQ(entities_->readNode(current, secondNode), expectedSecondNode);
	EXPECT_EQ(entities_->readEdge(current, edge), expectedEdge);
	EXPECT_EQ(adjacency.scan(current, GlobalAdjacencyDirection::GAD_OUT, firstNode, 1U).edge_ids,
			  std::vector<uint64_t>{edge});
	EXPECT_EQ(adjacency.scan(current, GlobalAdjacencyDirection::GAD_IN, secondNode, 1U).edge_ids,
			  std::vector<uint64_t>{edge});
	EXPECT_EQ(properties_->readEntity(current, {EntityPropertyOwnerKind::EPOK_NODE, firstNode}),
			  propertyRoot(firstNode, Node::typeId, 42));
	EXPECT_EQ(properties_->readEntity(current, {EntityPropertyOwnerKind::EPOK_EDGE, edge}),
			  propertyRoot(edge, Edge::typeId, 84));
	current.release();

	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	GlobalAdjacencyIndex reopenedAdjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(entities_->readNode(recovered, firstNode), expectedFirstNode);
	EXPECT_EQ(entities_->readNode(recovered, secondNode), expectedSecondNode);
	EXPECT_EQ(entities_->readEdge(recovered, edge), expectedEdge);
	EXPECT_EQ(reopenedAdjacency.scan(recovered, GlobalAdjacencyDirection::GAD_OUT, firstNode, 1U).edge_ids,
			  std::vector<uint64_t>{edge});
	EXPECT_EQ(reopenedAdjacency.scan(recovered, GlobalAdjacencyDirection::GAD_IN, secondNode, 1U).edge_ids,
			  std::vector<uint64_t>{edge});
	EXPECT_EQ(properties_->readEntity(recovered, {EntityPropertyOwnerKind::EPOK_NODE, firstNode}),
			  propertyRoot(firstNode, Node::typeId, 42));
	EXPECT_EQ(properties_->readEntity(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, edge}),
			  propertyRoot(edge, Edge::typeId, 84));
	recovered.release();
}

TEST_F(NativeGraphWritePlannerDurabilityTest,
	   TornInactiveSuperblockDuringCompoundGraphPublicationRetainsTheOldGenerationAndCanRetryAfterReopen) {
	switchToNativeIo();
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									durabilityPlannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t firstNode = transaction.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNode = transaction.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t edge = transaction.createEdge(firstNode, secondNode, 11U);
	transaction.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
	transaction.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation baseGeneration = before.superblock().generation;
	before.release();
	EXPECT_THROW(static_cast<void>(transaction.commit(
				 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE))),
			 BootstrapPublishInterrupted);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);

	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, baseGeneration);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, firstNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, secondNode)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(recovered, edge)), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	EXPECT_FALSE(
			metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_NODE, firstNode})
					.has_value());
	EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(recovered, {EntityPropertyOwnerKind::EPOK_EDGE, edge})
					.has_value());
	recovered.release();

	NativeGraphWritePlanner retryPlanner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
									 durabilityPlannerOptions());
	NativeGraphWriteTransaction retry = retryPlanner.beginTransaction();
	const uint64_t retryFirstNode = retry.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t retrySecondNode = retry.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t retryEdge = retry.createEdge(retryFirstNode, retrySecondNode, 11U);
	retry.replaceNodeProperty(propertyRoot(retryFirstNode, Node::typeId, 42));
	retry.replaceEdgeProperty(propertyRoot(retryEdge, Edge::typeId, 84));
	ASSERT_TRUE(retry.commit().committed());

	MetadataSnapshotPin published = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(published, GlobalAdjacencyDirection::GAD_OUT, retryFirstNode, 1U).edge_ids,
			  std::vector<uint64_t>{retryEdge});
	EXPECT_EQ(properties_->readEntity(published, {EntityPropertyOwnerKind::EPOK_EDGE, retryEdge}),
			  propertyRoot(retryEdge, Edge::typeId, 84));
	published.release();
}

TEST_F(NativeGraphWritePlannerDurabilityTest,
	   RecoversOldEntityAndIndexWhenDerivedPublicationIsInterruptedBeforeSuperblock) {
	const std::array<std::byte, 1U> key = {std::byte{'b'}};
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const IndexRootLocation root = allocateEmptyIndexRoot(indexes);
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const BTreeRootLeafMutationPlan indexPlan = indexes.planRootLeafUpsert(before, root.index_id, key, 1001U);
	const NativeGraphDerivedStateMutation derived = stableRootLeafMutation(indexPlan);
	const NativeGraphDerivedStatePreparer prepare = stableRootLeafPreparer(indexes, indexPlan);
	const std::array<EntityPageBatchNewPool, 1U> newNodePool{{
			{.request = {SegmentId(81U), 901U, Generation(1U), PageId(201U), PhysicalSlotId(41U), PhysicalSlotId(41U)},
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, {}}}},
	}};
	const std::array<EntityPageBatchMutation, 0U> noMutations{};
	const std::array<EntityPageBatchAppend, 0U> noAppends{};
	const std::array<EntityPropertyRootMutationPlan, 0U> noPropertyPlans{};
	const EntityPageAuxiliaryMutationBatch noAuxiliaryPages;
	EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
						 before, noMutations, noAppends, newNodePool, noPropertyPlans, *properties_,
						 durabilityPlannerOptions().property_roots, noAuxiliaryPages, derived, prepare,
						 LogSequenceNumber(4U),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);

	BTreeIndex index(root.index_id, indexes, *metadata_);
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(current.superblock().generation, Generation(3U));
	EXPECT_THROW(static_cast<void>(entities_->readNode(current, 1001U)), FormatException);
	EXPECT_FALSE(index.find(current, key).has_value());
	current.release();
	before.release();

	reopenNativeStores();
	BTreePageStore reopenedIndexes(*slotted_pages_, *metadata_);
	BTreeIndex reopenedIndex(17U, reopenedIndexes, *metadata_);
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(3U));
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, 1001U)), FormatException);
	EXPECT_FALSE(reopenedIndex.find(recovered, key).has_value());
	recovered.release();
}

TEST_F(NativeGraphWritePlannerDurabilityTest,
	   RecoversNewEntityAndIndexWhenDerivedPublicationIsInterruptedAfterSuperblock) {
	const std::array<std::byte, 1U> key = {std::byte{'a'}};
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const IndexRootLocation root = allocateEmptyIndexRoot(indexes);
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const BTreeRootLeafMutationPlan indexPlan = indexes.planRootLeafUpsert(before, root.index_id, key, 1001U);
	const NativeGraphDerivedStateMutation derived = stableRootLeafMutation(indexPlan);
	const NativeGraphDerivedStatePreparer prepare = stableRootLeafPreparer(indexes, indexPlan);
	const std::array<EntityPageBatchNewPool, 1U> newNodePool{{
			{.request = {SegmentId(81U), 901U, Generation(1U), PageId(201U), PhysicalSlotId(41U), PhysicalSlotId(41U)},
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, {}}}},
	}};
	const std::array<EntityPageBatchMutation, 0U> noMutations{};
	const std::array<EntityPageBatchAppend, 0U> noAppends{};
	const std::array<EntityPropertyRootMutationPlan, 0U> noPropertyPlans{};
	const EntityPageAuxiliaryMutationBatch noAuxiliaryPages;
	EXPECT_THROW(static_cast<void>(entities_->applyNativeGraphBatchTransaction(
						 before, noMutations, noAppends, newNodePool, noPropertyPlans, *properties_,
						 durabilityPlannerOptions().property_roots, noAuxiliaryPages, derived, prepare,
						 LogSequenceNumber(4U),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);

	BTreeIndex index(root.index_id, indexes, *metadata_);
	EXPECT_THROW(static_cast<void>(entities_->readNode(before, 1001U)), FormatException);
	EXPECT_FALSE(index.find(before, key).has_value());
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(current.superblock().generation, Generation(4U));
	EXPECT_EQ(entities_->readNode(current, 1001U), newNodePool.front().node_insertions.front().record);
	EXPECT_EQ(index.find(current, key), std::optional<uint64_t>(1001U));
	current.release();
	before.release();

	reopenNativeStores();
	BTreePageStore reopenedIndexes(*slotted_pages_, *metadata_);
	BTreeIndex reopenedIndex(17U, reopenedIndexes, *metadata_);
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(4U));
	EXPECT_EQ(entities_->readNode(recovered, 1001U), NodeEntityRecord{});
	EXPECT_EQ(reopenedIndex.find(recovered, key), std::optional<uint64_t>(1001U));
	recovered.release();
}
