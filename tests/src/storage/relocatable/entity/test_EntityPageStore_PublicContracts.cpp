/**
 * @file test_EntityPageStore_PublicContracts.cpp
 * @brief Public EntityPageStore read, scan, and typed auxiliary-page contracts.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "storage/relocatable/entity/EntityPageStoreTestFixture.hpp"

#include <atomic>
#include <cerrno>

#include "graph/storage/PwriteHelper.hpp"

namespace {

	enum class LabelPageCandidateWriteFailure {
		LPWF_DISK_FULL,
		LPWF_SHORT_WRITE,
	};

	void expectNativeLabelPageEvacuationCandidateFailure(
			const std::filesystem::path &path, const std::shared_ptr<std::fstream> &file,
			std::shared_ptr<StorageIO> &io, LabelPageCandidateWriteFailure failure) {
		const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
		ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
		io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		EntityPageStore store(io, *metadata);
		const std::optional<EntityPageInsertResult> source =
				store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(0U, 0U), LogSequenceNumber(2U));
		ASSERT_TRUE(source.has_value());
		ASSERT_TRUE(
				store.allocateNode(newPool(32U, 901U, 201U, 41U), 2001U, node(0U, 0U), LogSequenceNumber(3U)).has_value());

		MetadataSnapshotPin beforeLabelPublication = metadata->openSnapshot();
		const CatalogEntry sourcePool = metadata->findCatalog(beforeLabelPublication, SegmentId(31U));
		const std::vector<uint64_t> labels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
		const LabelPagePayload labelPayload = labelPagePayload(sourcePool, PageId(102U), LogSequenceNumber(4U),
															 std::span(labels).subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
		const NodeEntityRecord updated = nodeWithOverflow(labels, labelPayload.overflow);
		const std::array<EntityPageBatchMutation, 1U> mutations{{
				{.page_id = PageId(101U),
				 .record_kind = EntityPageRecordKind::EPRK_NODE,
				 .node_updates = {{1001U, updated}}},
		}};
		EntityPageAuxiliaryMutationBatch auxiliary;
		auxiliary.appends.push_back(
				{sourcePool, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL, labelPayload.payload, labelPayload.free_bytes});
		auxiliary.node_update_bindings.push_back({1001U, source->location, updated});
		static_cast<void>(store.applyAuxiliaryEntityBatchTransaction(beforeLabelPublication, mutations, {}, {}, auxiliary,
																					 LogSequenceNumber(4U)));
		beforeLabelPublication.release();

		MetadataSnapshotPin beforeEvacuation = metadata->openSnapshot();
		const PageLocation sourceLocation = metadata->findPageLocation(beforeEvacuation, PageId(102U));
		const CatalogEntry destinationPool = metadata->findCatalog(beforeEvacuation, SegmentId(32U));
		const uint64_t destinationOffset = PhysicalSegmentAllocator::checkedSlotOffset(
				beforeEvacuation.superblock().data_region_offset, destinationPool.physical_slot);
		bool injected = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting failDestinationLabelPage(
					[&injected, destinationOffset, failure](file_handle_t fd, const void *bytes, size_t byteCount,
																											 pwrite_off_t offset) {
						if (!injected && offset >= 0 && byteCount == RSS_PAGE_SIZE &&
							static_cast<uint64_t>(offset) >= destinationOffset &&
							static_cast<uint64_t>(offset) < destinationOffset + RSS_SEGMENT_SIZE) {
							try {
								const auto page =
										std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
								if (PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES)).kind ==
									PageOwnerKind::POK_ENTITY_LABEL) {
									injected = true;
									if (failure == LabelPageCandidateWriteFailure::LPWF_SHORT_WRITE) {
										const pwrite_ssize_t partial = portable_pwrite(fd, bytes, 16U, offset);
										return partial == 16U ? partial : static_cast<pwrite_ssize_t>(-1);
									}
#ifdef _WIN32
									SetLastError(ERROR_DISK_FULL);
#else
									errno = ENOSPC;
#endif
									return static_cast<pwrite_ssize_t>(-1);
								}
							} catch (const FormatException &) {
							}
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(
						 store.evacuatePoolPage(beforeEvacuation, PageId(102U), SegmentId(32U), LogSequenceNumber(5U))),
					 std::system_error);
		}
		EXPECT_TRUE(injected);
		EXPECT_EQ(metadata->findPageLocation(beforeEvacuation, PageId(102U)), sourceLocation);
		beforeEvacuation.release();

		metadata.reset();
		io.reset();
		EXPECT_EQ(portable_close_rw(nativeHandle), 0);
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto reopened = RelocatableMetadataStore::open(io);
		EntityPageStore reopenedStore(io, *reopened);
		MetadataSnapshotPin recovered = reopened->openSnapshot();
		EXPECT_EQ(reopened->findPageLocation(recovered, PageId(102U)), sourceLocation);
		const EntityPageMutationResult retried =
				reopenedStore.evacuatePoolPage(recovered, PageId(102U), SegmentId(32U), LogSequenceNumber(5U));
		EXPECT_EQ(retried.page.segment_id, SegmentId(32U));
		recovered.release();

		MetadataSnapshotPin published = reopened->openSnapshot();
		EXPECT_EQ(reopened->findPageLocation(published, PageId(102U)).segment_id, SegmentId(32U));
		EXPECT_EQ(reopenedStore.readNode(published, 1001U), updated);
		published.release();
	}

} // namespace

TEST_F(RelocatableEntityPageStoreTest, ReadsDecodedLocationsAndBoundsEntityScans) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord nodeRecord = node(0U, 0U);
	const EdgeEntityRecord edgeRecord = edge(1001U, 1002U, 21U);
	const std::optional<EntityPageInsertResult> allocatedNode =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, nodeRecord, LogSequenceNumber(2U));
	const std::optional<EntityPageInsertResult> allocatedEdge =
			store.allocateEdge(newPool(32U, 902U, 201U, 41U), 2001U, edgeRecord, LogSequenceNumber(3U));
	ASSERT_TRUE(allocatedNode.has_value());
	ASSERT_TRUE(allocatedEdge.has_value());

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.readNodeAt(snapshot, allocatedNode->location), nodeRecord);
	EXPECT_EQ(store.readEdgeAt(snapshot, allocatedEdge->location), edgeRecord);
	const EntityPageLocation invalidNodeLocation{1001U, PageId{}, 0U, 1U};
	const EntityPageLocation invalidEdgeLocation{2001U, PageId(201U), 0U, 0U};
	expectFormatError([&] { static_cast<void>(store.readNodeAt(snapshot, invalidNodeLocation)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.readEdgeAt(snapshot, invalidEdgeLocation)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<uint64_t> nodeIds;
	EXPECT_FALSE(store.visitNodesUntil(snapshot, 1U, [&](uint64_t nodeId, const NodeEntityRecord &record) {
		nodeIds.push_back(nodeId);
		EXPECT_EQ(record, nodeRecord);
		return true;
	}));
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1001U}));
	std::vector<uint64_t> edgeIds;
	EXPECT_FALSE(store.visitEdgesUntil(snapshot, 1U, [&](uint64_t edgeId, const EdgeEntityRecord &record) {
		edgeIds.push_back(edgeId);
		EXPECT_EQ(record, edgeRecord);
		return true;
	}));
	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{2001U}));

	size_t visitedMetadataPages = 0U;
	EXPECT_FALSE(store.visitNodesAfter(
			snapshot, 0U, 1U, 4U,
			[&](uint64_t nodeId, const NodeEntityRecord &record) {
				EXPECT_EQ(nodeId, 1001U);
				EXPECT_EQ(record, nodeRecord);
				return true;
			},
			&visitedMetadataPages));
	EXPECT_GT(visitedMetadataPages, 0U);
	EXPECT_FALSE(store.visitNodesUntil(snapshot, 2U, [](uint64_t, const NodeEntityRecord &) { return false; }));
	EXPECT_FALSE(store.visitEdgesUntil(snapshot, 2U, [](uint64_t, const EdgeEntityRecord &) { return false; }));
	EXPECT_FALSE(store.visitNodesAfter(snapshot, 0U, 2U, 4U, [](uint64_t, const NodeEntityRecord &) { return false; }));

	const std::function<bool(uint64_t, const NodeEntityRecord &)> nodeVisitor = [](uint64_t, const NodeEntityRecord &) {
		return true;
	};
	const std::function<bool(uint64_t, const EdgeEntityRecord &)> edgeVisitor = [](uint64_t, const EdgeEntityRecord &) {
		return true;
	};
	expectFormatError([&] { static_cast<void>(store.visitNodesUntil(snapshot, 0U, nodeVisitor)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.visitEdgesUntil(snapshot, 0U, edgeVisitor)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.visitNodesAfter(snapshot, 0U, 0U, 1U, nodeVisitor)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.visitNodesAfter(snapshot, 0U, 1U, 0U, nodeVisitor)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableEntityPageStoreTest, RejectsUntypedOrInvalidAuxiliaryPageRequestsBeforeIO) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	expectFormatError(
			[&] { static_cast<void>(store.readAuxiliaryPage(snapshot, PageId{}, PageOwnerKind::POK_ENTITY_LABEL)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store.readAuxiliaryPage(snapshot, PageId(101U), PageOwnerKind::POK_ENTITY)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { store.prefetchAuxiliaryPage(snapshot, PageId{}); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableEntityPageStoreTest, PublishesAndReadsTypedLabelAuxiliaryPagesWithTheirNodeMutation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord initial = node(0U, 0U);
	const std::optional<EntityPageInsertResult> allocated =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, initial, LogSequenceNumber(2U));
	ASSERT_TRUE(allocated.has_value());

	MetadataSnapshotPin before = metadata->openSnapshot();
	const CatalogEntry pool = metadata->findCatalog(before, SegmentId(31U));
	const std::vector<uint64_t> labels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	const LabelPagePayload labelPayload = labelPagePayload(pool, PageId(102U), LogSequenceNumber(3U),
														   std::span(labels).subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
	const NodeEntityRecord updated = nodeWithOverflow(labels, labelPayload.overflow);
	const std::array<EntityPageBatchMutation, 1U> mutations{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, updated}}},
	}};
	EntityPageAuxiliaryMutationBatch auxiliary;
	auxiliary.appends.push_back(
			{pool, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL, labelPayload.payload, labelPayload.free_bytes});
	auxiliary.node_update_bindings.push_back({1001U, allocated->location, updated});
	static_cast<void>(
			store.applyAuxiliaryEntityBatchTransaction(before, mutations, {}, {}, auxiliary, LogSequenceNumber(3U)));

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(before, 1001U), initial);
	EXPECT_EQ(store.readNode(current, 1001U), updated);
	const SharedPagePoolContents contents =
			store.readAuxiliaryPage(current, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL);
	EXPECT_EQ(contents.owner.page_id, PageId(102U));
	EXPECT_EQ(contents.owner.kind, PageOwnerKind::POK_ENTITY_LABEL);
	EXPECT_EQ(contents.pool.segment_id, SegmentId(31U));
	store.prefetchAuxiliaryPage(current, PageId(102U));
	before.release();
	current.release();
}

TEST_F(RelocatableEntityPageStoreTest, EvacuatesTypedLabelPageThroughThePublicPoolDispatcher) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const std::optional<EntityPageInsertResult> first =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(0U, 0U), LogSequenceNumber(2U));
	ASSERT_TRUE(first.has_value());
	const std::optional<EntityPageInsertResult> destination =
			store.allocateNode(newPool(32U, 901U, 201U, 41U), 2001U, node(0U, 0U), LogSequenceNumber(3U));
	ASSERT_TRUE(destination.has_value());

	MetadataSnapshotPin beforeLabelPublication = metadata->openSnapshot();
	const CatalogEntry sourcePool = metadata->findCatalog(beforeLabelPublication, SegmentId(31U));
	const std::vector<uint64_t> labels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	const LabelPagePayload labelPayload = labelPagePayload(sourcePool, PageId(102U), LogSequenceNumber(4U),
														   std::span(labels).subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
	const NodeEntityRecord updated = nodeWithOverflow(labels, labelPayload.overflow);
	const std::array<EntityPageBatchMutation, 1U> mutations{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, updated}}},
	}};
	EntityPageAuxiliaryMutationBatch auxiliary;
	auxiliary.appends.push_back(
			{sourcePool, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL, labelPayload.payload, labelPayload.free_bytes});
	auxiliary.node_update_bindings.push_back({1001U, first->location, updated});
	static_cast<void>(store.applyAuxiliaryEntityBatchTransaction(beforeLabelPublication, mutations, {}, {}, auxiliary,
																 LogSequenceNumber(4U)));
	beforeLabelPublication.release();

	MetadataSnapshotPin beforeEvacuation = metadata->openSnapshot();
	const PageLocation sourceLocation = metadata->findPageLocation(beforeEvacuation, PageId(102U));
	ASSERT_EQ(sourceLocation.segment_id, SegmentId(31U));
	const SharedPagePoolContents sourceContents =
			store.readAuxiliaryPage(beforeEvacuation, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL);
	const EntityPageMutationResult relocated =
			store.evacuatePoolPage(beforeEvacuation, PageId(102U), SegmentId(32U), LogSequenceNumber(5U));
	EXPECT_EQ(relocated.pool.segment_id, SegmentId(32U));
	EXPECT_EQ(relocated.page.page_id, PageId(102U));

	MetadataSnapshotPin current = metadata->openSnapshot();
	const PageLocation destinationLocation = metadata->findPageLocation(current, PageId(102U));
	EXPECT_EQ(destinationLocation.segment_id, SegmentId(32U));
	EXPECT_NE(destinationLocation, sourceLocation);
	const SharedPagePoolContents destinationContents =
			store.readAuxiliaryPage(current, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL);
	EXPECT_EQ(destinationContents.owner.owner_id, 901U);
	EXPECT_EQ(destinationContents.owner.kind, PageOwnerKind::POK_ENTITY_LABEL);
	EXPECT_EQ(SlottedPage::open(destinationContents.owner, destinationContents.payload).liveSlots().size(), 1U);
	EXPECT_EQ(store.readNode(beforeEvacuation, 1001U), updated);
	EXPECT_EQ(store.readNode(current, 1001U), updated);
	EXPECT_EQ(sourceContents.payload,
			  store.readAuxiliaryPage(beforeEvacuation, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL).payload);

	beforeEvacuation.release();
	current.release();
	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore reopenedStore(io, *reopened);
	const MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(reopened->findPageLocation(recovered, PageId(102U)).segment_id, SegmentId(32U));
	EXPECT_EQ(reopenedStore.readNode(recovered, 1001U), updated);
	EXPECT_EQ(reopenedStore.readAuxiliaryPage(recovered, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL).owner.kind,
			  PageOwnerKind::POK_ENTITY_LABEL);
}

TEST_F(RelocatableEntityPageStoreTest,
	   NativeLabelPageEvacuationDiskFullRetainsTheSourceAndCanRetryAfterReopen) {
	expectNativeLabelPageEvacuationCandidateFailure(path, file, io, LabelPageCandidateWriteFailure::LPWF_DISK_FULL);
}

TEST_F(RelocatableEntityPageStoreTest,
	   NativeLabelPageEvacuationShortWriteRetainsTheSourceAndCanRetryAfterReopen) {
	expectNativeLabelPageEvacuationCandidateFailure(path, file, io, LabelPageCandidateWriteFailure::LPWF_SHORT_WRITE);
}

TEST_F(RelocatableEntityPageStoreTest,
	   NativeLabelPageEvacuationFinalFenceFailureReportsUncertainDurabilityAndReopensTheDestination) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const std::optional<EntityPageInsertResult> source =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(0U, 0U), LogSequenceNumber(2U));
	ASSERT_TRUE(source.has_value());
	ASSERT_TRUE(
			store.allocateNode(newPool(32U, 901U, 201U, 41U), 2001U, node(0U, 0U), LogSequenceNumber(3U)).has_value());

	MetadataSnapshotPin beforeLabelPublication = metadata->openSnapshot();
	const CatalogEntry sourcePool = metadata->findCatalog(beforeLabelPublication, SegmentId(31U));
	const std::vector<uint64_t> labels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	const LabelPagePayload labelPayload = labelPagePayload(sourcePool, PageId(102U), LogSequenceNumber(4U),
														   std::span(labels).subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
	const NodeEntityRecord updated = nodeWithOverflow(labels, labelPayload.overflow);
	const std::array<EntityPageBatchMutation, 1U> mutations{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, updated}}},
	}};
	EntityPageAuxiliaryMutationBatch auxiliary;
	auxiliary.appends.push_back(
			{sourcePool, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL, labelPayload.payload, labelPayload.free_bytes});
	auxiliary.node_update_bindings.push_back({1001U, source->location, updated});
	static_cast<void>(store.applyAuxiliaryEntityBatchTransaction(beforeLabelPublication, mutations, {}, {}, auxiliary,
																 LogSequenceNumber(4U)));
	beforeLabelPublication.release();

	MetadataSnapshotPin beforeEvacuation = metadata->openSnapshot();
	std::atomic<uint32_t> inactiveSuperblockWrites{0U};
	std::atomic<uint32_t> syncCalls{0U};
	std::atomic<bool> failedFinalFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWrites](file_handle_t fd, const void *bytes, size_t byteCount,
																									 pwrite_off_t offset) {
					if (offset >= 0 && static_cast<uint64_t>(offset) < RSS_FILE_BOOTSTRAP_BYTES &&
						byteCount == RSS_SUPERBLOCK_BYTES) {
						inactiveSuperblockWrites.fetch_add(1U, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalFence(
				[&inactiveSuperblockWrites, &syncCalls, &failedFinalFence](file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					// Candidate pages can require several data fences. Fail only after the
					// A/B write that publishes the relocated LabelSet route.
					if (inactiveSuperblockWrites.load(std::memory_order_relaxed) >= 1U &&
						!failedFinalFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(store.evacuatePoolPage(beforeEvacuation, PageId(102U), SegmentId(32U),
															  LogSequenceNumber(5U))),
					 std::runtime_error);
	}
	EXPECT_GE(inactiveSuperblockWrites.load(std::memory_order_relaxed), 1U);
	EXPECT_TRUE(failedFinalFence.load(std::memory_order_relaxed));
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	beforeEvacuation.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(current, PageId(102U)).segment_id, SegmentId(32U));
	EXPECT_EQ(store.readNode(current, 1001U), updated);
	current.release();

	metadata.reset();
	io.reset();
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
	io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore reopenedStore(io, *reopened);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(reopened->findPageLocation(recovered, PageId(102U)).segment_id, SegmentId(32U));
	EXPECT_EQ(reopenedStore.readNode(recovered, 1001U), updated);
	recovered.release();
}

TEST_F(RelocatableEntityPageStoreTest, RejectsCorruptTypedLabelPayloadDuringPublicEvacuation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const std::optional<EntityPageInsertResult> first =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(0U, 0U), LogSequenceNumber(2U));
	ASSERT_TRUE(first.has_value());
	const std::optional<EntityPageInsertResult> destination =
			store.allocateNode(newPool(32U, 901U, 201U, 41U), 2001U, node(0U, 0U), LogSequenceNumber(3U));
	ASSERT_TRUE(destination.has_value());

	MetadataSnapshotPin before = metadata->openSnapshot();
	const CatalogEntry pool = metadata->findCatalog(before, SegmentId(31U));
	const std::vector<uint64_t> labels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	const LabelPagePayload labelPayload = labelPagePayload(pool, PageId(102U), LogSequenceNumber(4U),
														   std::span(labels).subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
	const NodeEntityRecord updated = nodeWithOverflow(labels, labelPayload.overflow);
	const std::array<EntityPageBatchMutation, 1U> mutations{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, updated}}},
	}};
	EntityPageAuxiliaryMutationBatch auxiliary;
	auxiliary.appends.push_back(
			{pool, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL, labelPayload.payload, labelPayload.free_bytes});
	auxiliary.node_update_bindings.push_back({1001U, first->location, updated});
	static_cast<void>(
			store.applyAuxiliaryEntityBatchTransaction(before, mutations, {}, {}, auxiliary, LogSequenceNumber(4U)));
	before.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	const PageLocation location = metadata->findPageLocation(current, PageId(102U));
	const uint64_t offset =
			PhysicalSegmentAllocator::checkedSlotOffset(current.superblock().data_region_offset,
														metadata->findCatalog(current, SegmentId(31U)).physical_slot) +
			static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE + RSS_PAGE_OWNER_HEADER_BYTES;
	const std::array<std::byte, 1U> corruption{std::byte{0x00U}};
	io->writeAt(offset, corruption.data(), corruption.size());

	expectFormatError(
			[&] {
				static_cast<void>(store.evacuatePoolPage(current, PageId(102U), SegmentId(32U), LogSequenceNumber(5U)));
			},
			FormatError::FERR_CHECKSUM_MISMATCH);
	EXPECT_EQ(metadata->findPageLocation(current, PageId(102U)), location);
}

TEST_F(RelocatableEntityPageStoreTest, PublishesAReachableLabelSetWithItsNewNodeInsertion) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const std::vector<uint64_t> labels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	CatalogEntry labelPool;
	labelPool.segment_id = SegmentId(31U);
	labelPool.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	labelPool.owner_id = 901U;
	labelPool.generation = Generation(1U);
	labelPool.state = SegmentState::SGS_SEALED;
	labelPool.committed_lsn = LogSequenceNumber(2U);
	const LabelPagePayload labelPayload = labelPagePayload(labelPool, PageId(101U), LogSequenceNumber(2U),
														   std::span(labels).subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
	const NodeEntityRecord record = nodeWithOverflow(labels, labelPayload.overflow);
	const std::array<EntityPageBatchNewPool, 1U> nodePool{{
			{.request = newPool(32U, 902U, 201U, 41U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, record}}},
	}};
	EntityPageAuxiliaryMutationBatch auxiliary;
	auxiliary.new_pools.push_back({SegmentId(31U), 901U, Generation(1U), PageId(101U), PhysicalSlotId(40U),
								   PhysicalSlotId(40U), PageOwnerKind::POK_ENTITY_LABEL, labelPayload.payload,
								   labelPayload.free_bytes});
	auxiliary.node_insertion_bindings.push_back({1001U, record});

	MetadataSnapshotPin before = metadata->openSnapshot();
	const EntityPageBatchResult result =
			store.applyAuxiliaryEntityBatchTransaction(before, {}, {}, nodePool, auxiliary, LogSequenceNumber(2U));
	EXPECT_EQ(result.new_pool_pages.size(), 1U);
	expectFormatError([&] { static_cast<void>(store.readNode(before, 1001U)); }, FormatError::FERR_INVALID_STATE);
	before.release();

	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(current, 1001U), record);
	const SharedPagePoolContents contents =
			store.readAuxiliaryPage(current, PageId(101U), PageOwnerKind::POK_ENTITY_LABEL);
	const SlottedPage labelPage = SlottedPage::open(contents.owner, contents.payload);
	ASSERT_EQ(labelPage.liveSlots().size(), 1U);
	const SlottedSlotReference slot{labelPayload.overflow.page_id, labelPayload.overflow.slot_id,
									labelPayload.overflow.slot_generation};
	EXPECT_EQ(EntityLabelSetCodec::decode(labelPage.read(slot)),
			  std::vector<uint64_t>(labels.begin() + RSS_ENTITY_INLINE_LABEL_CAPACITY, labels.end()));
}

TEST_F(RelocatableEntityPageStoreTest, RejectsUnboundOrMismatchedLabelSetNodeInsertionsBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const std::vector<uint64_t> labels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	CatalogEntry labelPool;
	labelPool.segment_id = SegmentId(31U);
	labelPool.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	labelPool.owner_id = 901U;
	labelPool.generation = Generation(1U);
	labelPool.state = SegmentState::SGS_SEALED;
	labelPool.committed_lsn = LogSequenceNumber(2U);
	const LabelPagePayload labelPayload = labelPagePayload(labelPool, PageId(101U), LogSequenceNumber(2U),
														   std::span(labels).subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
	const NodeEntityRecord record = nodeWithOverflow(labels, labelPayload.overflow);
	const std::array<EntityPageBatchNewPool, 1U> nodePool{{
			{.request = newPool(32U, 902U, 201U, 41U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, record}}},
	}};
	const auto auxiliaryPlan = [&] {
		EntityPageAuxiliaryMutationBatch auxiliary;
		auxiliary.new_pools.push_back({SegmentId(31U), 901U, Generation(1U), PageId(101U), PhysicalSlotId(40U),
									   PhysicalSlotId(40U), PageOwnerKind::POK_ENTITY_LABEL, labelPayload.payload,
									   labelPayload.free_bytes});
		return auxiliary;
	};

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EntityPageAuxiliaryMutationBatch noBinding = auxiliaryPlan();
	expectFormatError(
			[&] {
				static_cast<void>(store.applyAuxiliaryEntityBatchTransaction(snapshot, {}, {}, nodePool, noBinding,
																			 LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	EntityPageAuxiliaryMutationBatch emptyOverflow = auxiliaryPlan();
	emptyOverflow.node_insertion_bindings.push_back({1001U, node(0U, 0U)});
	expectFormatError(
			[&] {
				static_cast<void>(store.applyAuxiliaryEntityBatchTransaction(snapshot, {}, {}, nodePool, emptyOverflow,
																			 LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	EntityPageAuxiliaryMutationBatch missingInsertion = auxiliaryPlan();
	missingInsertion.node_insertion_bindings.push_back({1002U, record});
	expectFormatError(
			[&] {
				static_cast<void>(store.applyAuxiliaryEntityBatchTransaction(snapshot, {}, {}, nodePool,
																			 missingInsertion, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);
	EntityPageAuxiliaryMutationBatch mismatchedRecord = auxiliaryPlan();
	NodeEntityRecord differentRecord = record;
	differentRecord.inline_label_ids.front() = 5U;
	mismatchedRecord.node_insertion_bindings.push_back({1001U, differentRecord});
	expectFormatError(
			[&] {
				static_cast<void>(store.applyAuxiliaryEntityBatchTransaction(snapshot, {}, {}, nodePool,
																			 mismatchedRecord, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);
	EntityPageAuxiliaryMutationBatch duplicateBinding = auxiliaryPlan();
	duplicateBinding.node_insertion_bindings.push_back({1001U, record});
	duplicateBinding.node_insertion_bindings.push_back({1001U, record});
	expectFormatError(
			[&] {
				static_cast<void>(store.applyAuxiliaryEntityBatchTransaction(snapshot, {}, {}, nodePool,
																			 duplicateBinding, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U)));
	EXPECT_FALSE(metadata->findNodePageLocationOptional(snapshot, 1001U).has_value());
}

TEST_F(RelocatableEntityPageStoreTest, RejectsStaleLocationsWhoseSlotsWereDeletedFromLivePages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const std::optional<EntityPageInsertResult> firstNode =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(0U, 0U), LogSequenceNumber(2U));
	const std::optional<EntityPageInsertResult> firstEdge =
			store.allocateEdge(newPool(32U, 902U, 201U, 41U), 2001U, edge(1001U, 1002U, 21U), LogSequenceNumber(3U));
	ASSERT_TRUE(firstNode.has_value());
	ASSERT_TRUE(firstEdge.has_value());
	MetadataSnapshotPin afterInitial = metadata->openSnapshot();
	const EntityPageInsertResult secondNode =
			store.insertNode(afterInitial, PageId(101U), 1002U, node(0U, 0U, 17U), LogSequenceNumber(4U));
	afterInitial.release();
	MetadataSnapshotPin afterNodeInsert = metadata->openSnapshot();
	const EntityPageInsertResult secondEdge =
			store.insertEdge(afterNodeInsert, PageId(201U), 2002U, edge(1002U, 1001U, 22U), LogSequenceNumber(5U));
	afterNodeInsert.release();

	MetadataSnapshotPin beforeErase = metadata->openSnapshot();
	const EntityPageLocation deletedNodeLocation = metadata->findNodePageLocation(beforeErase, 1001U);
	const EntityPageLocation deletedEdgeLocation = metadata->findEdgePageLocation(beforeErase, 2001U);
	EXPECT_EQ(deletedNodeLocation, firstNode->location);
	EXPECT_EQ(deletedEdgeLocation, firstEdge->location);
	static_cast<void>(store.eraseNode(beforeErase, 1001U, LogSequenceNumber(6U)));
	beforeErase.release();
	MetadataSnapshotPin afterNodeErase = metadata->openSnapshot();
	static_cast<void>(store.eraseEdge(afterNodeErase, 2001U, LogSequenceNumber(7U)));
	afterNodeErase.release();

	const MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.readNode(current, 1002U), node(0U, 0U, 17U));
	EXPECT_EQ(store.readEdge(current, 2002U), edge(1002U, 1001U, 22U));
	EXPECT_EQ(secondNode.location.page_id, PageId(101U));
	EXPECT_EQ(secondEdge.location.page_id, PageId(201U));
	expectFormatError([&] { static_cast<void>(store.readNodeAt(current, deletedNodeLocation)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store.readEdgeAt(current, deletedEdgeLocation)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableEntityPageStoreTest, RejectsLabelPayloadFreeByteMismatchBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	const NodeEntityRecord initial = node(0U, 0U);
	const std::optional<EntityPageInsertResult> allocated =
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, initial, LogSequenceNumber(2U));
	ASSERT_TRUE(allocated.has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry pool = metadata->findCatalog(snapshot, SegmentId(31U));
	const std::vector<uint64_t> labels = orderedLabels(7U, RSS_ENTITY_INLINE_LABEL_CAPACITY + 1U);
	const LabelPagePayload payload = labelPagePayload(pool, PageId(102U), LogSequenceNumber(3U),
													  std::span(labels).subspan(RSS_ENTITY_INLINE_LABEL_CAPACITY));
	const NodeEntityRecord replacement = nodeWithOverflow(labels, payload.overflow);
	const std::array<EntityPageBatchMutation, 1U> mutations{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, replacement}}},
	}};
	EntityPageAuxiliaryMutationBatch auxiliary;
	auxiliary.appends.push_back(
			{pool, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL, payload.payload, payload.free_bytes + 1U});
	auxiliary.node_update_bindings.push_back({1001U, allocated->location, replacement});
	expectFormatError(
			[&] {
				static_cast<void>(store.applyAuxiliaryEntityBatchTransaction(snapshot, mutations, {}, {}, auxiliary,
																			 LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_EQ(store.readNode(snapshot, 1001U), initial);
	EXPECT_FALSE(metadata->findPageLocationOptional(snapshot, PageId(102U)).has_value());
}

TEST_F(RelocatableEntityPageStoreTest, RejectsWrongOwnerAndIncompletePoolEvacuationRequests) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EntityPageStore store(io, *metadata);
	ASSERT_TRUE(
			store.allocateNode(newPool(31U, 901U, 101U, 40U), 1001U, node(0U, 0U), LogSequenceNumber(2U)).has_value());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(store.evacuatePoolPage(snapshot, PageId{}, SegmentId(32U), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.evacuatePoolPage(snapshot, PageId(101U), SegmentId{}, LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.evacuatePoolPage(snapshot, PageId(101U), SegmentId(32U), {})); },
					  FormatError::FERR_INVALID_HEADER);
	const PageOwnerHeader wrongOwner{PageId(101U),
									 901U,
									 PageOwnerKind::POK_PROPERTY,
									 0U,
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									 Generation(1U),
									 LogSequenceNumber(2U)};
	overwritePageOwner(io, *metadata, PageId(101U), wrongOwner);
	expectFormatError(
			[&] {
				static_cast<void>(
						store.evacuatePoolPage(snapshot, PageId(101U), SegmentId(32U), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
}
