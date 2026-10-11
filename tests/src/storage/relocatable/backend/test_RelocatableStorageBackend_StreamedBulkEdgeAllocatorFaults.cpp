/**
 * @file test_RelocatableStorageBackend_StreamedBulkEdgeAllocatorFaults.cpp
 * @brief Allocator-page write fault contracts for streamed bulk Edge publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include "graph/storage/relocatable/allocation/AllocatorPage.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <exception>
#include <memory>
#include <span>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace graph::storage;
using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeAllocatorCandidateDiskFullRetainsOldGraphAndAllowsRetry) {
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
			bulk->appendPersistedBulkEdges({"BULK_ALLOCATOR_DISK_FULL", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnAllocatorLeaf(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							static_cast<void>(AllocatorLeafPageCodec::decode(page));
							injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						} catch (const std::exception &) {
							// Only authenticated allocator bitmap leaves are this fault point.
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
			retryBulk->appendPersistedBulkEdges({"BULK_ALLOCATOR_DISK_FULL_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[1], nodes[0], "BULK_ALLOCATOR_DISK_FULL_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeAllocatorCandidateShortWriteRetainsOldGraphAndAllowsRetry) {
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
			bulk->appendPersistedBulkEdges({"BULK_ALLOCATOR_SHORT_WRITE", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortWriteAllocatorLeaf(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							static_cast<void>(AllocatorLeafPageCodec::decode(page));
							injected.store(true, std::memory_order_relaxed);
							return portable_pwrite(fd, bytes, 16U, offset);
						} catch (const std::exception &) {
							// Only authenticated allocator bitmap leaves are this fault point.
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
			retryBulk->appendPersistedBulkEdges({"BULK_ALLOCATOR_SHORT_WRITE_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[1], nodes[0], "BULK_ALLOCATOR_SHORT_WRITE_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}
