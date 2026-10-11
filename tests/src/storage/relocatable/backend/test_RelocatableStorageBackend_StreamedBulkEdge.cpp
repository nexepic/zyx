/**
 * @file test_RelocatableStorageBackend_StreamedBulkEdge.cpp
 * @brief Streamed bulk Edge publication contracts for native logical transactions.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/index/BTreePage.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <future>
#include <span>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;
using namespace relocatable_storage_backend_test;

class DISABLED_RelocatableStorageBackendScaleTest : public RelocatableStorageBackendTest {};

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeCommitPublishesTopologyAcrossBothAdjacencyDirections) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_bulk_edge_transaction_records = 4U;
	options.bulk_edge_id_lease_block_records = 2U;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}, {{"BulkIsolated"}, {}}});
	ASSERT_EQ(nodes.size(), 3U);

	// Consume one bounded lease first. The committed range must remain readable
	// even though its durable Edge IDs begin after a deliberately abandoned gap.
	{
		std::unique_ptr<LogicalGraphTransaction> abandoned = backend.beginLogicalGraphTransaction(false);
		auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(abandoned.get());
		ASSERT_NE(bulk, nullptr);
		const std::array<uint64_t, 1U> sources{nodes[0]};
		const std::array<uint64_t, 1U> targets{nodes[1]};
		const std::vector<uint64_t> discarded =
				bulk->appendPersistedBulkEdges({"BULK_DISCARDED", sources, targets, {}});
		ASSERT_EQ(discarded.size(), 1U);
		abandoned->rollback();
	}
	std::unique_ptr<LogicalGraphReadSnapshot> beforeCommit = backend.openLogicalGraphReadSnapshot();

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 3U> parallelSources{nodes[0], nodes[0], nodes[0]};
	const std::array<uint64_t, 3U> parallelTargets{nodes[1], nodes[1], nodes[0]};
	const std::vector<uint64_t> forward =
			bulk->appendPersistedBulkEdges({"BULK_PARALLEL", parallelSources, parallelTargets, {}});
	ASSERT_EQ(forward.size(), 3U);
	const std::array<uint64_t, 1U> reverseSources{nodes[1]};
	const std::array<uint64_t, 1U> reverseTargets{nodes[0]};
	const std::vector<uint64_t> reverse =
			bulk->appendPersistedBulkEdges({"BULK_REVERSE", reverseSources, reverseTargets, {}});
	ASSERT_EQ(reverse.size(), 1U);
	EXPECT_GT(forward.front(), 1U);
	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	transaction.reset();
	for (const uint64_t edgeId: {forward[0], forward[1], forward[2], reverse[0]}) {
		EXPECT_THROW(static_cast<void>(beforeCommit->readEdge(edgeId)), FormatException);
	}
	EXPECT_TRUE(beforeCommit
					->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U)
					.edges.empty());
	EXPECT_TRUE(beforeCommit
					->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U)
					.edges.empty());
	EXPECT_TRUE(beforeCommit
					->scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U)
					.edges.empty());
	EXPECT_TRUE(beforeCommit
					->scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U)
					.edges.empty());
	beforeCommit.reset();

	const auto assertTopology = [&](LogicalGraphReadSnapshot &snapshot) {
		for (const uint64_t edgeId: {forward[0], forward[1], forward[2], reverse[0]}) {
			EXPECT_TRUE(snapshot.readEdge(edgeId).properties.empty());
		}
		EXPECT_EQ(snapshot.readEdge(forward[0]), LogicalGraphEdge(forward[0], nodes[0], nodes[1], "BULK_PARALLEL"));
		EXPECT_EQ(snapshot.readEdge(forward[1]), LogicalGraphEdge(forward[1], nodes[0], nodes[1], "BULK_PARALLEL"));
		EXPECT_EQ(snapshot.readEdge(forward[2]), LogicalGraphEdge(forward[2], nodes[0], nodes[0], "BULK_PARALLEL"));
		EXPECT_EQ(snapshot.readEdge(reverse[0]), LogicalGraphEdge(reverse[0], nodes[1], nodes[0], "BULK_REVERSE"));

		const LogicalGraphAdjacencyPage sourceOut =
				snapshot.scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U);
		const LogicalGraphAdjacencyPage sourceIn =
				snapshot.scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U);
		const LogicalGraphAdjacencyPage targetOut =
				snapshot.scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U);
		const LogicalGraphAdjacencyPage targetIn =
				snapshot.scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U);
		const LogicalGraphAdjacencyPage isolatedOut =
				snapshot.scanAdjacentEdges(nodes[2], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U);
		EXPECT_FALSE(sourceOut.continuation.has_value());
		EXPECT_FALSE(sourceIn.continuation.has_value());
		EXPECT_FALSE(targetOut.continuation.has_value());
		EXPECT_FALSE(targetIn.continuation.has_value());
		EXPECT_EQ(sourceOut.edges,
				  (std::vector<LogicalGraphEdge>{LogicalGraphEdge(forward[0], nodes[0], nodes[1], "BULK_PARALLEL"),
												 LogicalGraphEdge(forward[1], nodes[0], nodes[1], "BULK_PARALLEL"),
												 LogicalGraphEdge(forward[2], nodes[0], nodes[0], "BULK_PARALLEL")}));
		EXPECT_EQ(sourceIn.edges,
				  (std::vector<LogicalGraphEdge>{LogicalGraphEdge(forward[2], nodes[0], nodes[0], "BULK_PARALLEL"),
												 LogicalGraphEdge(reverse[0], nodes[1], nodes[0], "BULK_REVERSE")}));
		EXPECT_EQ(targetOut.edges,
				  (std::vector<LogicalGraphEdge>{LogicalGraphEdge(reverse[0], nodes[1], nodes[0], "BULK_REVERSE")}));
		EXPECT_EQ(targetIn.edges,
				  (std::vector<LogicalGraphEdge>{LogicalGraphEdge(forward[0], nodes[0], nodes[1], "BULK_PARALLEL"),
												 LogicalGraphEdge(forward[1], nodes[0], nodes[1], "BULK_PARALLEL")}));
		EXPECT_TRUE(isolatedOut.edges.empty());
		EXPECT_FALSE(isolatedOut.continuation.has_value());
	};

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	assertTopology(*snapshot);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	assertTopology(*snapshot);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
	   StreamedBulkEdgeFinalSyncFailureIsIndeterminateAndReopenRecoversTheCompleteGeneration) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::array<PropertyValue, 1U> weights{PropertyValue(int64_t{13})};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"weight", weights}}};
	const std::vector<uint64_t> staged = bulk->appendPersistedBulkEdges({"BULK_UNCERTAIN", sources, targets, columns});
	ASSERT_EQ(staged.size(), 1U);

	std::atomic<uint32_t> inactiveSuperblockWrites{0U};
	std::atomic<uint32_t> graphPublicationSuperblockWrites{0U};
	std::atomic<uint32_t> syncCalls{0U};
	std::atomic<bool> failedFinalFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeInactiveSuperblock(
				[&inactiveSuperblockWrites, &graphPublicationSuperblockWrites](file_handle_t fd, const void *bytes,
																			   size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						inactiveSuperblockWrites.fetch_add(1U, std::memory_order_relaxed);
						const Superblock candidate = SuperblockCodec::decode(
								std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
						if (candidate.edge_entity_directory_root.isValid()) {
							graphPublicationSuperblockWrites.fetch_add(1U, std::memory_order_relaxed);
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&graphPublicationSuperblockWrites, &syncCalls, &failedFinalFence](file_handle_t fd) {
					syncCalls.fetch_add(1U, std::memory_order_relaxed);
					// Source and private candidates may advance allocator metadata before
					// final publication. Fail only after a Superblock actually routes an
					// Edge Entity directory, rather than relying on a fragile write count.
					if (graphPublicationSuperblockWrites.load(std::memory_order_relaxed) >= 1U &&
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
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::runtime_error);
	}
	EXPECT_GE(inactiveSuperblockWrites.load(std::memory_order_relaxed), 1U);
	EXPECT_GE(graphPublicationSuperblockWrites.load(std::memory_order_relaxed), 1U);
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_INDETERMINATE);
	transaction.reset();
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"MustReopenFirst"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(staged.front()), LogicalGraphEdge(staged.front(), nodes[0], nodes[1], "BULK_UNCERTAIN",
																   {{"weight", PropertyValue(int64_t{13})}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	EXPECT_EQ(reopened.createLogicalNodes({{{"WritableAfterReopen"}, {}}}).size(), 1U);
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
	   StreamedBulkEdgeGraphSuperblockShortWriteRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> existing = backend.createLogicalEdges({{nodes[0], nodes[1], "EXISTING_EDGE", {}}});
	ASSERT_EQ(existing.size(), 1U);

	const std::array<uint64_t, 1U> sources{nodes[1]};
	const std::array<uint64_t, 1U> targets{nodes[0]};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_SUPERBLOCK_SHORT_WRITE", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortGraphPublicationSuperblock(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed) &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							if (candidate.edge_entity_directory_root.isValid()) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const std::exception &) {
							// Only an authenticated graph-publication Superblock is this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()), LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE"));
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()), LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE"));
	snapshot.reset();
	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_SUPERBLOCK_SHORT_WRITE_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[1], nodes[0], "BULK_SUPERBLOCK_SHORT_WRITE_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
	   StreamedBulkEdgeGraphSuperblockDiskFullRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> existing = backend.createLogicalEdges({{nodes[0], nodes[1], "EXISTING_EDGE", {}}});
	ASSERT_EQ(existing.size(), 1U);

	const std::array<uint64_t, 1U> sources{nodes[1]};
	const std::array<uint64_t, 1U> targets{nodes[0]};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_SUPERBLOCK_DISK_FULL", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnGraphPublicationSuperblock(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed) &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							if (candidate.edge_entity_directory_root.isValid()) {
								injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
								SetLastError(ERROR_DISK_FULL);
#else
								errno = ENOSPC;
#endif
								return static_cast<pwrite_ssize_t>(-1);
							}
						} catch (const std::exception &) {
							// Only an authenticated graph-publication Superblock is this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()), LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE"));
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()), LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE"));
	snapshot.reset();
	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_SUPERBLOCK_DISK_FULL_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[1], nodes[0], "BULK_SUPERBLOCK_DISK_FULL_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
	   StreamedBulkEdgePostPublicationScratchCleanupFailureKeepsCommitVisibleAndRequiresReopen) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::vector<uint64_t> staged =
			bulk->appendPersistedBulkEdges({"BULK_CLEANUP_FAILURE", sources, targets, {}});
	ASSERT_EQ(staged.size(), 1U);

	std::atomic<bool> graphPublicationWritten{false};
	std::atomic<bool> cleanupWriteFailed{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting failPostPublicationCleanupMetadataWrite(
				[&graphPublicationWritten, &cleanupWriteFailed](file_handle_t fd, const void *bytes, size_t byteCount,
																								 pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						const Superblock candidate = SuperblockCodec::decode(
								std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
						if (candidate.edge_entity_directory_root.isValid()) {
							graphPublicationWritten.store(true, std::memory_order_relaxed);
						}
					} else if (byteCount == RSS_PAGE_SIZE && graphPublicationWritten.load(std::memory_order_relaxed) &&
							   !cleanupWriteFailed.exchange(true, std::memory_order_relaxed)) {
						// The graph Superblock has already been written. Fail only the next
						// metadata page from scratch retirement, not the final graph publication.
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	}
	EXPECT_TRUE(graphPublicationWritten.load(std::memory_order_relaxed));
	EXPECT_TRUE(cleanupWriteFailed.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	EXPECT_TRUE(backend.hasIndeterminateWriteOutcome());
	transaction.reset();

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(staged.front()),
			  LogicalGraphEdge(staged.front(), nodes[0], nodes[1], "BULK_CLEANUP_FAILURE"));
	snapshot.reset();
	EXPECT_THROW(static_cast<void>(backend.createLogicalNodes({{{"MustReopenFirst"}, {}}})), std::logic_error);
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_FALSE(reopened.hasIndeterminateWriteOutcome());
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(staged.front()),
			  LogicalGraphEdge(staged.front(), nodes[0], nodes[1], "BULK_CLEANUP_FAILURE"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	EXPECT_EQ(reopened.createLogicalNodes({{{"WritableAfterReopen"}, {}}}).size(), 1U);
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeCommitPublishesLargeValuePropertiesAndSurvivesReopen) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::string payload(RSS_SEGMENT_SIZE / 2U, 'x');
	const std::array<PropertyValue, 1U> payloads{PropertyValue(payload)};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", payloads}}};
	const std::vector<uint64_t> staged =
			bulk->appendPersistedBulkEdges({"BULK_LARGE_VALUE", sources, targets, columns});
	ASSERT_EQ(staged.size(), 1U);

	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	transaction.reset();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	const LogicalGraphEdge committed = snapshot->readEdge(staged.front());
	EXPECT_EQ(committed.source_node_id, nodes[0]);
	EXPECT_EQ(committed.target_node_id, nodes[1]);
	EXPECT_EQ(committed.type, "BULK_LARGE_VALUE");
	EXPECT_EQ(committed.properties, (std::vector<LogicalGraphPropertyEntry>{{"payload", PropertyValue(payload)}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(staged.front()), committed);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeCommitReusesNonConsecutiveDuplicateLargeValueProperties) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 3U> sources{nodes[0], nodes[0], nodes[0]};
	const std::array<uint64_t, 3U> targets{nodes[1], nodes[1], nodes[1]};
	const std::string duplicate(RSS_SEGMENT_SIZE / 2U, 'd');
	const std::string distinct(RSS_SEGMENT_SIZE / 2U, 'u');
	const std::array<PropertyValue, 3U> payloads{PropertyValue(duplicate), PropertyValue(distinct),
																									PropertyValue(duplicate)};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", payloads}}};
	const std::vector<uint64_t> staged =
			bulk->appendPersistedBulkEdges({"BULK_LARGE_VALUE_DEDUP", sources, targets, columns});
	ASSERT_EQ(staged.size(), 3U);
	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	transaction.reset();

	const auto assertProperties = [&](RelocatableStorageBackend &candidate) {
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = candidate.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->readEdge(staged[0]).properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"payload", PropertyValue(duplicate)}}));
		EXPECT_EQ(snapshot->readEdge(staged[1]).properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"payload", PropertyValue(distinct)}}));
		EXPECT_EQ(snapshot->readEdge(staged[2]).properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"payload", PropertyValue(duplicate)}}));
	};
	assertProperties(backend);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	assertProperties(reopened);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeCommitPublishesParallelSelfLoopAndDistinctTypesInOneGeneration) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}, {{"BulkSelf"}, {}}});
	ASSERT_EQ(nodes.size(), 3U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 2U> parallelSources{nodes[0], nodes[0]};
	const std::array<uint64_t, 2U> parallelTargets{nodes[1], nodes[1]};
	const std::array<PropertyValue, 2U> ranks{PropertyValue(int64_t{1}), PropertyValue(int64_t{2})};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"rank", ranks}}};
	const std::vector<uint64_t> parallel =
			bulk->appendPersistedBulkEdges({"BULK_PARALLEL", parallelSources, parallelTargets, columns});
	ASSERT_EQ(parallel.size(), 2U);

	const std::array<uint64_t, 1U> selfSources{nodes[2]};
	const std::array<uint64_t, 1U> selfTargets{nodes[2]};
	const std::vector<uint64_t> self = bulk->appendPersistedBulkEdges({"BULK_SELF_LOOP", selfSources, selfTargets, {}});
	ASSERT_EQ(self.size(), 1U);

	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	transaction.reset();

	const LogicalGraphEdge firstParallel(parallel[0], nodes[0], nodes[1], "BULK_PARALLEL",
										 {{"rank", PropertyValue(int64_t{1})}});
	const LogicalGraphEdge secondParallel(parallel[1], nodes[0], nodes[1], "BULK_PARALLEL",
										  {{"rank", PropertyValue(int64_t{2})}});
	const LogicalGraphEdge selfLoop(self[0], nodes[2], nodes[2], "BULK_SELF_LOOP");
	const auto assertPublishedTopology = [&](RelocatableStorageBackend &candidate) {
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = candidate.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->readEdge(parallel[0]), firstParallel);
		EXPECT_EQ(snapshot->readEdge(parallel[1]), secondParallel);
		EXPECT_EQ(snapshot->readEdge(self[0]), selfLoop);
		EXPECT_EQ(
				snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U).edges,
				(std::vector<LogicalGraphEdge>{firstParallel, secondParallel}));
		EXPECT_EQ(
				snapshot->scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U).edges,
				(std::vector<LogicalGraphEdge>{firstParallel, secondParallel}));
		EXPECT_EQ(
				snapshot->scanAdjacentEdges(nodes[2], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U).edges,
				(std::vector<LogicalGraphEdge>{selfLoop}));
		EXPECT_EQ(
				snapshot->scanAdjacentEdges(nodes[2], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U).edges,
				(std::vector<LogicalGraphEdge>{selfLoop}));
	};

	assertPublishedTopology(backend);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	assertPublishedTopology(reopened);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
	   StreamedBulkEdgeCommitReusesExistingAndPublishesNewRelationshipAndPropertyTokens) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}, {{"BulkOther"}, {}}});
	ASSERT_EQ(nodes.size(), 3U);

	// Establish both tokens through the ordinary native writer. The streamed
	// publication must reuse them while also atomically adding a new pair.
	const std::vector<uint64_t> existing = backend.createLogicalEdges(
			{{nodes[0], nodes[1], "BULK_REUSED_TYPE", {{"rank", PropertyValue(int64_t{9})}}}});
	ASSERT_EQ(existing.size(), 1U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 2U> reusedSources{nodes[0], nodes[0]};
	const std::array<uint64_t, 2U> reusedTargets{nodes[1], nodes[1]};
	const std::array<PropertyValue, 2U> reusedRanks{PropertyValue(int64_t{10}), PropertyValue(int64_t{11})};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> reusedColumns{{{"rank", reusedRanks}}};
	const std::vector<uint64_t> reused =
			bulk->appendPersistedBulkEdges({"BULK_REUSED_TYPE", reusedSources, reusedTargets, reusedColumns});
	ASSERT_EQ(reused.size(), 2U);

	const std::array<uint64_t, 1U> newSources{nodes[0]};
	const std::array<uint64_t, 1U> newTargets{nodes[2]};
	const std::array<PropertyValue, 1U> freshValues{PropertyValue("published-with-new-token")};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> newColumns{{{"fresh", freshValues}}};
	const std::vector<uint64_t> created =
			bulk->appendPersistedBulkEdges({"BULK_NEW_TYPE", newSources, newTargets, newColumns});
	ASSERT_EQ(created.size(), 1U);

	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	transaction.reset();

	const LogicalGraphEdge prior(existing[0], nodes[0], nodes[1], "BULK_REUSED_TYPE",
								 {{"rank", PropertyValue(int64_t{9})}});
	const LogicalGraphEdge firstReused(reused[0], nodes[0], nodes[1], "BULK_REUSED_TYPE",
									   {{"rank", PropertyValue(int64_t{10})}});
	const LogicalGraphEdge secondReused(reused[1], nodes[0], nodes[1], "BULK_REUSED_TYPE",
										{{"rank", PropertyValue(int64_t{11})}});
	const LogicalGraphEdge createdWithNewTokens(created[0], nodes[0], nodes[2], "BULK_NEW_TYPE",
												{{"fresh", PropertyValue("published-with-new-token")}});
	const auto assertPublishedTokens = [&](RelocatableStorageBackend &candidate) {
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = candidate.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->readEdge(existing[0]), prior);
		EXPECT_EQ(snapshot->readEdge(reused[0]), firstReused);
		EXPECT_EQ(snapshot->readEdge(reused[1]), secondReused);
		EXPECT_EQ(snapshot->readEdge(created[0]), createdWithNewTokens);
		EXPECT_EQ(
				snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U).edges,
				(std::vector<LogicalGraphEdge>{prior, firstReused, secondReused, createdWithNewTokens}));
		EXPECT_EQ(
				snapshot->scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U).edges,
				(std::vector<LogicalGraphEdge>{prior, firstReused, secondReused}));
		EXPECT_EQ(
				snapshot->scanAdjacentEdges(nodes[2], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U).edges,
				(std::vector<LogicalGraphEdge>{createdWithNewTokens}));
	};

	assertPublishedTokens(backend);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	assertPublishedTokens(reopened);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgePublicationConflictReleasesScratchAndLease) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::vector<uint64_t> staged = bulk->appendPersistedBulkEdges({"BULK_CONFLICT", sources, targets, {}});
	ASSERT_EQ(staged.size(), 1U);
	backend.deleteLogicalNodes({{nodes[1], false, 0U}});

	EXPECT_THROW(static_cast<void>(transaction->commit()), LogicalGraphMutationConflict);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(staged.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgePublicationRebasesOverDisjointAdjacencyPublication) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}, {{"ConcurrentTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 3U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::array<PropertyValue, 1U> bulkWeights{PropertyValue(int64_t{7})};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> bulkColumns{{{"weight", bulkWeights}}};
	const std::vector<uint64_t> staged =
			bulk->appendPersistedBulkEdges({"BULK_REBASED", sources, targets, bulkColumns});
	ASSERT_EQ(staged.size(), 1U);

	// This ordinary Edge advances the global adjacency and Edge directories after
	// bulk staging. The streamed publisher must rebase over both current roots.
	const std::vector<uint64_t> concurrent = backend.createLogicalEdges(
			{{nodes[0], nodes[2], "CONCURRENT_ADJACENCY", {{"rank", PropertyValue(int64_t{9})}}}});
	ASSERT_EQ(concurrent.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	transaction.reset();

	const auto assertPublishedEdges = [&](RelocatableStorageBackend &candidate) {
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = candidate.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->readEdge(staged.front()),
				  LogicalGraphEdge(staged.front(), nodes[0], nodes[1], "BULK_REBASED",
								   {{"weight", PropertyValue(int64_t{7})}}));
		EXPECT_EQ(snapshot->readEdge(concurrent.front()),
				  LogicalGraphEdge(concurrent.front(), nodes[0], nodes[2], "CONCURRENT_ADJACENCY",
								   {{"rank", PropertyValue(int64_t{9})}}));
		EXPECT_EQ(
				snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U).edges,
				(std::vector<LogicalGraphEdge>{LogicalGraphEdge(staged.front(), nodes[0], nodes[1], "BULK_REBASED",
																{{"weight", PropertyValue(int64_t{7})}}),
											   LogicalGraphEdge(concurrent.front(), nodes[0], nodes[2],
																"CONCURRENT_ADJACENCY",
																{{"rank", PropertyValue(int64_t{9})}})}));
		EXPECT_EQ(
				snapshot->scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U).edges,
				(std::vector<LogicalGraphEdge>{LogicalGraphEdge(staged.front(), nodes[0], nodes[1], "BULK_REBASED",
																{{"weight", PropertyValue(int64_t{7})}})}));
		EXPECT_EQ(
				snapshot->scanAdjacentEdges(nodes[2], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U).edges,
				(std::vector<LogicalGraphEdge>{LogicalGraphEdge(concurrent.front(), nodes[0], nodes[2],
																"CONCURRENT_ADJACENCY",
																{{"rank", PropertyValue(int64_t{9})}})}));
	};

	assertPublishedEdges(backend);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	assertPublishedEdges(reopened);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeRebasesAcrossHandlesWithMaintenanceEnabled) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maintenance = RelocatableMaintenanceOptions{};
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::vector<uint64_t> staged =
			bulk->appendPersistedBulkEdges({"BULK_MAINTENANCE_REBASE", sources, targets, {}});
	ASSERT_EQ(staged.size(), 1U);

	RelocatableStorageBackendOptions writerOptions = backendOptions();
	writerOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend concurrentWriter(std::move(writerOptions));
	concurrentWriter.open();
	const std::vector<uint64_t> external =
			concurrentWriter.createLogicalEdges({{nodes[0], nodes[1], "MAINTENANCE_EXTERNAL", {}}});
	ASSERT_EQ(external.size(), 1U);
	concurrentWriter.close();

	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	transaction.reset();
	EXPECT_TRUE(backend.nativeMaintenanceEnabled());

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(external.front()),
			  LogicalGraphEdge(external.front(), nodes[0], nodes[1], "MAINTENANCE_EXTERNAL", {}));
	EXPECT_EQ(snapshot->readEdge(staged.front()),
			  LogicalGraphEdge(staged.front(), nodes[0], nodes[1], "BULK_MAINTENANCE_REBASE", {}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> reopenedSnapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(reopenedSnapshot->readEdge(external.front()),
			  LogicalGraphEdge(external.front(), nodes[0], nodes[1], "MAINTENANCE_EXTERNAL", {}));
	EXPECT_EQ(reopenedSnapshot->readEdge(staged.front()),
			  LogicalGraphEdge(staged.front(), nodes[0], nodes[1], "BULK_MAINTENANCE_REBASE", {}));
	reopenedSnapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		StreamedBulkEdgeConcurrentSessionFailsBeforeScratchMutationAndRetrySucceeds) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions bootstrapOptions = backendOptions();
	bootstrapOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend bootstrap(std::move(bootstrapOptions));
	bootstrap.open();
	const std::vector<uint64_t> nodes = bootstrap.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	bootstrap.close();

	RelocatableStorageBackendOptions firstOptions = backendOptions();
	firstOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend firstWriter(std::move(firstOptions));
	firstWriter.open();
	RelocatableStorageBackendOptions secondOptions = backendOptions();
	secondOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend secondWriter(std::move(secondOptions));
	secondWriter.open();

	std::unique_ptr<LogicalGraphTransaction> firstTransaction = firstWriter.beginLogicalGraphTransaction(false);
	std::unique_ptr<LogicalGraphTransaction> secondTransaction = secondWriter.beginLogicalGraphTransaction(false);
	auto *const firstBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(firstTransaction.get());
	auto *const secondBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(secondTransaction.get());
	ASSERT_NE(firstBulk, nullptr);
	ASSERT_NE(secondBulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const LogicalGraphPersistedBulkEdgeAppendRequest firstRequest{"FIRST_BULK", sources, targets, {}};
	const LogicalGraphPersistedBulkEdgeAppendRequest secondRequest{"SECOND_BULK", sources, targets, {}};
	const std::vector<uint64_t> firstIds = firstBulk->appendPersistedBulkEdges(firstRequest);
	ASSERT_EQ(firstIds.size(), 1U);

	EXPECT_THROW(static_cast<void>(secondBulk->appendPersistedBulkEdges(secondRequest)), LogicalGraphMutationConflict);
	EXPECT_EQ(secondTransaction->state(), LogicalGraphTransactionState::LGTTS_ACTIVE);
	EXPECT_NO_THROW(secondTransaction->rollback());
	EXPECT_EQ(secondTransaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	secondTransaction.reset();

	EXPECT_NO_THROW(static_cast<void>(firstTransaction->commit()));
	EXPECT_EQ(firstTransaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	firstTransaction.reset();
	firstWriter.close();
	secondWriter.close();

	RelocatableStorageBackendOptions retryOptions = backendOptions();
	retryOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend retryWriter(std::move(retryOptions));
	retryWriter.open();
	std::unique_ptr<LogicalGraphTransaction> retryTransaction = retryWriter.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retryTransaction.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> retryIds = retryBulk->appendPersistedBulkEdges(secondRequest);
	ASSERT_EQ(retryIds.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retryTransaction->commit()));
	retryTransaction.reset();

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = retryWriter.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(firstIds.front()), LogicalGraphEdge(firstIds.front(), nodes[0], nodes[1], "FIRST_BULK", {}));
	EXPECT_EQ(snapshot->readEdge(retryIds.front()),
			  LogicalGraphEdge(retryIds.front(), nodes[0], nodes[1], "SECOND_BULK", {}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(retryWriter.verifyIntegrity()));
	retryWriter.close();
}

TEST_F(RelocatableStorageBackendTest,
		StreamedBulkEdgeHardLinkedDatabaseAliasSharesTheSameSessionLease) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions bootstrapOptions = backendOptions();
	bootstrapOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend bootstrap(std::move(bootstrapOptions));
	bootstrap.open();
	const std::vector<uint64_t> nodes = bootstrap.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	bootstrap.close();

	const std::filesystem::path aliasPath = data_path_.string() + ".bulk-session-alias";
	std::error_code error;
	std::filesystem::create_hard_link(data_path_, aliasPath, error);
	ASSERT_FALSE(error) << error.message();

	RelocatableStorageBackendOptions firstOptions = backendOptions();
	firstOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend firstWriter(std::move(firstOptions));
	firstWriter.open();
	RelocatableStorageBackendOptions aliasOptions = backendOptions();
	aliasOptions.data_path = aliasPath;
	aliasOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend aliasWriter(std::move(aliasOptions));
	aliasWriter.open();

	std::unique_ptr<LogicalGraphTransaction> firstTransaction = firstWriter.beginLogicalGraphTransaction(false);
	std::unique_ptr<LogicalGraphTransaction> aliasTransaction = aliasWriter.beginLogicalGraphTransaction(false);
	auto *const firstBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(firstTransaction.get());
	auto *const aliasBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(aliasTransaction.get());
	ASSERT_NE(firstBulk, nullptr);
	ASSERT_NE(aliasBulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const LogicalGraphPersistedBulkEdgeAppendRequest firstRequest{"PRIMARY_BULK", sources, targets, {}};
	const LogicalGraphPersistedBulkEdgeAppendRequest aliasRequest{"ALIAS_BULK", sources, targets, {}};
	const std::vector<uint64_t> firstIds = firstBulk->appendPersistedBulkEdges(firstRequest);
	ASSERT_EQ(firstIds.size(), 1U);

	EXPECT_THROW(static_cast<void>(aliasBulk->appendPersistedBulkEdges(aliasRequest)), LogicalGraphMutationConflict);
	EXPECT_NO_THROW(aliasTransaction->rollback());
	aliasTransaction.reset();
	EXPECT_NO_THROW(static_cast<void>(firstTransaction->commit()));
	firstTransaction.reset();
	firstWriter.close();
	aliasWriter.close();

	RelocatableStorageBackendOptions retryOptions = backendOptions();
	retryOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend retryWriter(std::move(retryOptions));
	retryWriter.open();
	std::unique_ptr<LogicalGraphTransaction> retryTransaction = retryWriter.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retryTransaction.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> retryIds = retryBulk->appendPersistedBulkEdges(aliasRequest);
	ASSERT_EQ(retryIds.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retryTransaction->commit()));
	retryTransaction.reset();
	EXPECT_NO_THROW(static_cast<void>(retryWriter.verifyIntegrity()));
	retryWriter.close();

	std::filesystem::remove(aliasPath, error);
	EXPECT_FALSE(error) << error.message();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeRebasePreservesAnOperatorPausedMaintenanceService) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maintenance = RelocatableMaintenanceOptions{};
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	backend.pauseMaintenance();
	ASSERT_FALSE(backend.nativeMaintenanceEnabled());
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::vector<uint64_t> staged =
			bulk->appendPersistedBulkEdges({"BULK_PAUSED_MAINTENANCE", sources, targets, {}});
	ASSERT_EQ(staged.size(), 1U);

	RelocatableStorageBackendOptions writerOptions = backendOptions();
	writerOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend concurrentWriter(std::move(writerOptions));
	concurrentWriter.open();
	const std::vector<uint64_t> external =
			concurrentWriter.createLogicalEdges({{nodes[0], nodes[1], "PAUSED_MAINTENANCE_EXTERNAL", {}}});
	ASSERT_EQ(external.size(), 1U);
	concurrentWriter.close();

	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	transaction.reset();
	EXPECT_FALSE(backend.nativeMaintenanceEnabled());
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(external.front()),
			  LogicalGraphEdge(external.front(), nodes[0], nodes[1], "PAUSED_MAINTENANCE_EXTERNAL", {}));
	EXPECT_EQ(snapshot->readEdge(staged.front()),
			  LogicalGraphEdge(staged.front(), nodes[0], nodes[1], "BULK_PAUSED_MAINTENANCE", {}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgePublicationRebasesOverConcurrentScalarIndexCreation) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes(
			{{{"Person"}, {{"score", PropertyValue(int64_t{7})}}}, {{"Person"}, {{"score", PropertyValue(int64_t{9})}}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::vector<uint64_t> staged = bulk->appendPersistedBulkEdges({"BULK_INDEX_REBASE", sources, targets, {}});
	ASSERT_EQ(staged.size(), 1U);

	// Schema DDL advances the same generation while bulk source data remains
	// sealed. The final bulk publication must retain the newly built scalar tree.
	const LogicalGraphIndexDefinition index =
			backend.createLogicalNodeIndex({"person_score", "Person", {"score"}});
	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	transaction.reset();

	const auto assertPublishedState = [&](RelocatableStorageBackend &candidate) {
		EXPECT_EQ(candidate.listLogicalIndexes(8U), std::vector<LogicalGraphIndexDefinition>{index});
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = candidate.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->readEdge(staged.front()),
				  LogicalGraphEdge(staged.front(), nodes[0], nodes[1], "BULK_INDEX_REBASE", {}));
	};
	assertPublishedState(backend);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	assertPublishedState(reopened);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(DISABLED_RelocatableStorageBackendScaleTest, StreamedBulkEdgePublishesAdjacencyAcrossLeavesAndIndexPools) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	// Every streamed Edge has one OUT and one IN adjacency key. This input is
	// deliberately large enough to exceed the 31-page index-pool boundary.
	constexpr size_t BULK_EDGE_COUNT = static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT) * 256U;
	const std::vector<uint64_t> sources(BULK_EDGE_COUNT, nodes[0]);
	const std::vector<uint64_t> targets(BULK_EDGE_COUNT, nodes[1]);
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> staged =
			bulk->appendPersistedBulkEdges({"BULK_CROSS_POOL", sources, targets, {}});
	ASSERT_EQ(staged.size(), BULK_EDGE_COUNT);
	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	transaction.reset();

	// Rebuild the now cross-pool root from a second sealed bulk source. This
	// exercises multi-pool retirement and replacement rather than fresh-tree
	// construction alone.
	transaction = backend.beginLogicalGraphTransaction(false);
	bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> rebaseSource{nodes[0]};
	const std::array<uint64_t, 1U> rebaseTarget{nodes[1]};
	const std::vector<uint64_t> rebased =
			bulk->appendPersistedBulkEdges({"BULK_CROSS_POOL_REBASE", rebaseSource, rebaseTarget, {}});
	ASSERT_EQ(rebased.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	transaction.reset();
	std::vector<uint64_t> expected = staged;
	expected.insert(expected.end(), rebased.begin(), rebased.end());

	const auto assertAdjacency = [&](RelocatableStorageBackend &candidate) {
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = candidate.openLogicalGraphReadSnapshot();
		const auto collect = [&](LogicalGraphReadDirection direction) {
			std::vector<uint64_t> ids;
			std::optional<LogicalGraphAdjacencyContinuation> continuation;
			size_t pageCount = 0U;
			do {
				const LogicalGraphAdjacencyPage page =
						snapshot->scanAdjacentEdges(nodes[direction == LogicalGraphReadDirection::LGRD_OUTGOING ? 0U : 1U],
											 direction, continuation, 257U);
				for (const LogicalGraphEdge &edge: page.edges) {
					ids.push_back(edge.edge_id);
				}
				continuation = page.continuation;
				++pageCount;
			} while (continuation.has_value());
			EXPECT_GT(pageCount, 1U);
			return ids;
		};
		EXPECT_EQ(collect(LogicalGraphReadDirection::LGRD_OUTGOING), expected);
		EXPECT_EQ(collect(LogicalGraphReadDirection::LGRD_INCOMING), expected);
	};
	assertAdjacency(backend);
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	// The public scan proves leaf traversal. Inspect the persisted owner boundary
	// separately to ensure this fixture really crossed index-pool placement.
	auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(data->is_open());
	auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin metadataSnapshot = metadata->openSnapshot();
	size_t adjacencyPoolCount = 0U;
	for (const CatalogEntry &pool: metadata->findCurrentIndexPagePools(metadataSnapshot)) {
		if (pool.owner_id == RSS_GLOBAL_ADJACENCY_INDEX_ID) {
			++adjacencyPoolCount;
		}
	}
	EXPECT_GE(adjacencyPoolCount, 2U);
	metadataSnapshot.release();
	metadata.reset();
	io.reset();
	data->close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	assertAdjacency(reopened);
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryFinalEdgeDeletionRetiresGlobalAdjacencyRootAndSurvivesReopen) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"AdjacencySource"}, {}}, {{"AdjacencyTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> edges = backend.createLogicalEdges({{nodes[0], nodes[1], "ONLY_EDGE", {}}});
	ASSERT_EQ(edges.size(), 1U);

	backend.deleteLogicalEdges({{edges.front()}});
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	// The final adjacency deletion must retire the root instead of publishing a
	// permanently reachable empty tree that would retain its COW page pool.
	auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(data->is_open());
	auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin rootSnapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(rootSnapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	rootSnapshot.release();
	metadata.reset();
	io.reset();
	data->close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(edges.front())), FormatException);
	EXPECT_TRUE(snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)
						.edges.empty());
	EXPECT_TRUE(snapshot->scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U)
						.edges.empty());
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeDeletionRetiresBothAdjacencyDirectionsAndSurvivesReopen) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"BulkAdjacencySource"}, {}}, {{"BulkAdjacencyTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 2U> sources{nodes[0], nodes[1]};
	const std::array<uint64_t, 2U> targets{nodes[1], nodes[0]};
	const std::vector<uint64_t> edges =
			bulk->appendPersistedBulkEdges({"BULK_RECLAIM", sources, targets, {}});
	ASSERT_EQ(edges.size(), 2U);
	ASSERT_NO_THROW(static_cast<void>(transaction->commit()));
	transaction.reset();

	backend.deleteLogicalEdges({{edges[0]}, {edges[1]}});
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 0U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(edges[0])), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(edges[1])), FormatException);
	EXPECT_TRUE(snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)
					.edges.empty());
	EXPECT_TRUE(snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U)
					.edges.empty());
	EXPECT_TRUE(snapshot->scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)
					.edges.empty());
	EXPECT_TRUE(snapshot->scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U)
					.edges.empty());
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	// Deleting the last bulk-published edges must retire the shared route rather
	// than leave an empty COW adjacency tree reachable after reopen.
	auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(data->is_open());
	auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto metadata = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin rootSnapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(rootSnapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	rootSnapshot.release();
	metadata.reset();
	io.reset();
	data->close();

	RelocatableStorageBackend reopened(backendOptions());
	reopened.open();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(edges[0])), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(edges[1])), FormatException);
	EXPECT_TRUE(snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)
					.edges.empty());
	EXPECT_TRUE(snapshot->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U)
					.edges.empty());
	EXPECT_TRUE(snapshot->scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)
					.edges.empty());
	EXPECT_TRUE(snapshot->scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U)
					.edges.empty());
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeRequiresFiniteSortAndCandidatePageBounds) {
	materializeNativeDatabase();
	const auto assertRejected = [this](const std::function<void(RelocatableStorageBackendOptions &)> &mutate) {
		RelocatableStorageBackendOptions options = backendOptions();
		options.open_mode = OpenMode::OPEN_EXISTING_FILE;
		mutate(options);
		RelocatableStorageBackend backend(std::move(options));
		EXPECT_THROW(backend.open(), std::invalid_argument);
	};
	assertRejected([](RelocatableStorageBackendOptions &options) { options.bulk_edge_id_lease_block_records = 0U; });
	assertRejected([](RelocatableStorageBackendOptions &options) { options.maximum_bulk_edge_append_records = 0U; });
	assertRejected([](RelocatableStorageBackendOptions &options) { options.maximum_bulk_edge_append_bytes = 0U; });
	assertRejected([](RelocatableStorageBackendOptions &options) { options.bulk_edge_sort_run_bytes = 0U; });
	assertRejected([](RelocatableStorageBackendOptions &options) { options.maximum_bulk_edge_sort_runs = 1U; });
	assertRejected([](RelocatableStorageBackendOptions &options) { options.maximum_bulk_edge_candidate_pages = 0U; });
	assertRejected([](RelocatableStorageBackendOptions &options) { options.maximum_bulk_edge_index_entries = 0U; });
	assertRejected([](RelocatableStorageBackendOptions &options) {
		options.maximum_bulk_edge_transaction_records = 2U;
		options.maximum_bulk_edge_index_entries = 3U;
	});
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeCandidateBudgetFailureRetainsOldGraphAndCleansUp) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_bulk_edge_transaction_records = 1U;
	options.bulk_edge_id_lease_block_records = 1U;
	options.maximum_bulk_edge_append_records = 1U;
	options.maximum_bulk_edge_candidate_pages = 1U;
	options.maximum_bulk_edge_index_entries = 2U;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_CANDIDATE_BUDGET", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	EXPECT_THROW(static_cast<void>(transaction->commit()), FormatException);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	EXPECT_NO_THROW(reopened.open());
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeCorruptSourceManifestRetainsOldGraphAndCleansUp) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	// Keep the source manifest out of the page cache so this public-path test
	// exercises the authenticated native read boundary.
	options.buffer_pool_bytes = RSS_BUFFER_POOL_PAGE_BYTES;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_CORRUPT_MANIFEST", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeReadHookForTesting corruptOneBulkManifest([&injected](file_handle_t fd, void *bytes,
																					 size_t byteCount,
																					 pwrite_off_t offset) {
			const pwrite_ssize_t read = portable_pread(fd, bytes, byteCount, offset);
			if (read == static_cast<pwrite_ssize_t>(RSS_SEGMENT_SIZE) && !injected.load(std::memory_order_relaxed)) {
				try {
					auto segment = std::span<std::byte>(static_cast<std::byte *>(bytes), byteCount);
					const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
					if (envelope.kind == SegmentKind::SGK_BULK_SCRATCH &&
						envelope.header_bytes + RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES <= segment.size() &&
						LittleEndianCodec::readU32(segment, envelope.header_bytes) ==
								RSS_NATIVE_BULK_EDGE_MANIFEST_MAGIC) {
						// Corrupt only this sequential read buffer. The subsequent cleanup
						// pass must still authenticate and retire the on-disk source.
						segment[envelope.header_bytes + 60U] ^= std::byte{0x01};
						injected.store(true, std::memory_order_relaxed);
					}
				} catch (const FormatException &) {
					// Other segment families are not part of this source-corruption point.
				}
			}
			return read;
		});
		EXPECT_THROW(static_cast<void>(transaction->commit()), FormatException);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	EXPECT_NO_THROW(reopened.open());
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeCorruptSourceRunRetainsOldGraphAndCleansUp) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.buffer_pool_bytes = RSS_BUFFER_POOL_PAGE_BYTES;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::vector<uint64_t> rejected = bulk->appendPersistedBulkEdges({"BULK_CORRUPT_RUN", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeReadHookForTesting corruptOneBulkRun([&injected](file_handle_t fd, void *bytes,
																				size_t byteCount, pwrite_off_t offset) {
			const pwrite_ssize_t read = portable_pread(fd, bytes, byteCount, offset);
			if (read == static_cast<pwrite_ssize_t>(RSS_SEGMENT_SIZE) && !injected.load(std::memory_order_relaxed)) {
				try {
					auto segment = std::span<std::byte>(static_cast<std::byte *>(bytes), byteCount);
					const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
					if (envelope.kind == SegmentKind::SGK_BULK_SCRATCH &&
						envelope.header_bytes + RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES < segment.size() &&
						LittleEndianCodec::readU32(segment, envelope.header_bytes) == RSS_NATIVE_BULK_EDGE_RUN_MAGIC) {
						// Alter the first framed record after its aggregate checksum was
						// authenticated into the run header. Cleanup reads the original disk
						// bytes after the hook is gone.
						segment[envelope.header_bytes + RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES] ^= std::byte{0x01};
						injected.store(true, std::memory_order_relaxed);
					}
				} catch (const FormatException &) {
					// Only fully authenticated scratch envelopes participate in this injection.
				}
			}
			return read;
		});
		EXPECT_THROW(static_cast<void>(transaction->commit()), FormatException);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	EXPECT_NO_THROW(reopened.open());
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeAppendBoundsRejectBeforeLeasingOrScratch) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_bulk_edge_transaction_records = 8U;
	options.maximum_bulk_edge_append_records = 1U;
	options.maximum_bulk_edge_append_bytes = 1'024U;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 2U> twoSources{nodes[0], nodes[0]};
	const std::array<uint64_t, 2U> twoTargets{nodes[1], nodes[1]};
	EXPECT_THROW(static_cast<void>(bulk->appendPersistedBulkEdges({"TOO_MANY", twoSources, twoTargets, {}})),
				 std::invalid_argument);

	const std::array<uint64_t, 1U> oneSource{nodes[0]};
	const std::array<uint64_t, 1U> oneTarget{nodes[1]};
	const std::array<PropertyValue, 1U> oversizedValue{PropertyValue(std::string(1'024U, 'x'))};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> oversizedColumn{{{"payload", oversizedValue}}};
	EXPECT_THROW(static_cast<void>(bulk->appendPersistedBulkEdges({"TOO_WIDE", oneSource, oneTarget, oversizedColumn})),
				 std::invalid_argument);

	const std::vector<uint64_t> accepted = bulk->appendPersistedBulkEdges({"BOUNDED", oneSource, oneTarget, {}});
	ASSERT_EQ(accepted.size(), 1U);
	EXPECT_NO_THROW(transaction->rollback());
	transaction.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeDuplicatePropertyKeysRejectBeforeLeasingOrScratch) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> source{nodes[0]};
	const std::array<uint64_t, 1U> target{nodes[1]};
	const std::array<PropertyValue, 1U> values{PropertyValue(int64_t{7})};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 2U> duplicateColumns{{
			{"rank", values}, {"rank", values}}};
	EXPECT_THROW(static_cast<void>(bulk->appendPersistedBulkEdges({"DUPLICATE_PROPERTY", source, target, duplicateColumns})),
				 std::invalid_argument);

	// Validation happens before the durable lease, so the first valid append keeps
	// the canonical next Edge ID and can commit normally.
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> validColumns{{{"rank", values}}};
	const std::vector<uint64_t> accepted =
			bulk->appendPersistedBulkEdges({"VALID_PROPERTY", source, target, validColumns});
	ASSERT_EQ(accepted, (std::vector<uint64_t>{1U}));
	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	transaction.reset();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(accepted.front()),
			  LogicalGraphEdge(accepted.front(), nodes[0], nodes[1], "VALID_PROPERTY",
							   {{"rank", PropertyValue(int64_t{7})}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeRejectedAppendPreservesAnActiveStream) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_bulk_edge_transaction_records = 4U;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes =
			backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}, {{"BulkOther"}, {}}});
	ASSERT_EQ(nodes.size(), 3U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 1U> source{nodes[0]};
	const std::array<uint64_t, 1U> target{nodes[1]};
	const std::vector<uint64_t> first = bulk->appendPersistedBulkEdges({"STREAM_KEEP", source, target, {}});
	ASSERT_EQ(first, (std::vector<uint64_t>{1U}));

	// A malformed later batch must fail before touching the existing scratch stream
	// or its durable lease, leaving the transaction usable for a retry.
	const std::vector<PropertyValue> malformedValues;
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> malformed{{{"weight", malformedValues}}};
	EXPECT_THROW(static_cast<void>(bulk->appendPersistedBulkEdges({"STREAM_BAD", source, target, malformed})),
				 std::invalid_argument);
	EXPECT_TRUE(bulk->hasStreamedBulkEdges());

	const std::array<uint64_t, 1U> secondTarget{nodes[2]};
	const std::vector<uint64_t> second =
				bulk->appendPersistedBulkEdges({"STREAM_KEEP", source, secondTarget, {}});
	ASSERT_EQ(second, (std::vector<uint64_t>{2U}));
	EXPECT_NO_THROW(static_cast<void>(transaction->commit()));
	transaction.reset();

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(first.front()),
			  LogicalGraphEdge(first.front(), nodes[0], nodes[1], "STREAM_KEEP", {}));
	EXPECT_EQ(snapshot->readEdge(second.front()),
			  LogicalGraphEdge(second.front(), nodes[0], nodes[2], "STREAM_KEEP", {}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}
