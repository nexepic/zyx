/**
 * @file test_RelocatableStorageBackend_StreamedBulkEdgeFaults.cpp
 * @brief Streamed bulk Edge publication fault and recovery contracts.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include "graph/storage/relocatable/allocation/AllocatorPage.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityFreeSpaceIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
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

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgePublicDataDiskFullRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected = bulk->appendPersistedBulkEdges({"BULK_DISK_FULL", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnPublicEntityPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_ENTITY) {
								injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
								SetLastError(ERROR_DISK_FULL);
#else
								errno = ENOSPC;
#endif
								return static_cast<pwrite_ssize_t>(-1);
							}
						} catch (const std::exception &) {
							// Non-page writes are outside the public Entity-page failure point.
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
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphTransaction> retry = backend.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_DISK_FULL_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_DISK_FULL_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgePropertyPageDiskFullRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> existing =
			backend.createLogicalEdges({{nodes[0], nodes[1], "EXISTING_EDGE", {{"rank", PropertyValue(int64_t{1})}}}});
	ASSERT_EQ(existing.size(), 1U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::array<PropertyValue, 1U> rejectedValues{PropertyValue("rejected-property")};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> rejectedColumns{{{"payload", rejectedValues}}};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_PROPERTY_DISK_FULL", sources, targets, rejectedColumns});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnPublicPropertyPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_PROPERTY) {
								injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
								SetLastError(ERROR_DISK_FULL);
#else
								errno = ENOSPC;
#endif
								return static_cast<pwrite_ssize_t>(-1);
							}
						} catch (const std::exception &) {
							// Non-page writes are outside the public Property-page failure point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()),
			  LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE", {{"rank", PropertyValue(int64_t{1})}}));
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()),
			  LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE", {{"rank", PropertyValue(int64_t{1})}}));
	snapshot.reset();
	const std::array<PropertyValue, 1U> committedValues{PropertyValue("committed-property")};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> committedColumns{{{"payload", committedValues}}};
	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_PROPERTY_DISK_FULL_RETRY", sources, targets, committedColumns});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_PROPERTY_DISK_FULL_RETRY",
						   {{"payload", PropertyValue("committed-property")}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgePropertyPageShortWriteRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> existing = backend.createLogicalEdges({{nodes[0], nodes[1], "EXISTING_EDGE", {}}});
	ASSERT_EQ(existing.size(), 1U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::array<PropertyValue, 1U> values{PropertyValue("property-short-write")};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", values}}};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_PROPERTY_SHORT_WRITE", sources, targets, columns});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortPublicPropertyPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_PROPERTY) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const std::exception &) {
							// Only an authenticated Property page is this partial-write point.
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
	// Pin the pre-retry generation across the streamed publication and verify
	// the new edge and its derived metadata never leak into the old snapshot.
	auto preRetrySnapshot = reopened.openLogicalGraphReadSnapshot();
	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_PROPERTY_SHORT_WRITE_RETRY", sources, targets, columns});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_PROPERTY_SHORT_WRITE_RETRY",
						   {{"payload", PropertyValue("property-short-write")}}));
	snapshot.reset();
	EXPECT_THROW(static_cast<void>(preRetrySnapshot->readEdge(committed.front())), FormatException);
	EXPECT_EQ(preRetrySnapshot->readEdge(existing.front()),
	          LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE"));
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	preRetrySnapshot.reset();
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeLargeValuePageDiskFullRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> existing = backend.createLogicalEdges({{nodes[0], nodes[1], "EXISTING_EDGE", {}}});
	ASSERT_EQ(existing.size(), 1U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::string payload(RSS_SEGMENT_SIZE / 2U, 'x');
	const std::array<PropertyValue, 1U> values{PropertyValue(payload)};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", values}}};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_LARGE_VALUE_DISK_FULL", sources, targets, columns});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnLargeValueDataPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_LARGE_VALUE_DATA) {
								injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
								SetLastError(ERROR_DISK_FULL);
#else
								errno = ENOSPC;
#endif
								return static_cast<pwrite_ssize_t>(-1);
							}
						} catch (const std::exception &) {
							// Only native LargeValue data pages are this fault point.
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
			retryBulk->appendPersistedBulkEdges({"BULK_LARGE_VALUE_DISK_FULL_RETRY", sources, targets, columns});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_LARGE_VALUE_DISK_FULL_RETRY",
						   {{"payload", PropertyValue(payload)}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeLargeValuePageShortWriteRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> existing = backend.createLogicalEdges({{nodes[0], nodes[1], "EXISTING_EDGE", {}}});
	ASSERT_EQ(existing.size(), 1U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::string payload(RSS_SEGMENT_SIZE / 2U, 'x');
	const std::array<PropertyValue, 1U> values{PropertyValue(payload)};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", values}}};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_LARGE_VALUE_SHORT_WRITE", sources, targets, columns});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortLargeValueDataPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_LARGE_VALUE_DATA) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const std::exception &) {
							// Only an authenticated LargeValue data page is this partial-write point.
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
			retryBulk->appendPersistedBulkEdges({"BULK_LARGE_VALUE_SHORT_WRITE_RETRY", sources, targets, columns});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_LARGE_VALUE_SHORT_WRITE_RETRY",
						   {{"payload", PropertyValue(payload)}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeLargeValueManifestDiskFullRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> existing = backend.createLogicalEdges({{nodes[0], nodes[1], "EXISTING_EDGE", {}}});
	ASSERT_EQ(existing.size(), 1U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::string payload(RSS_SEGMENT_SIZE / 2U, 'x');
	const std::array<PropertyValue, 1U> values{PropertyValue(payload)};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", values}}};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_LARGE_VALUE_MANIFEST_DISK_FULL", sources, targets, columns});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnLargeValueManifestPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST) {
								injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
								SetLastError(ERROR_DISK_FULL);
#else
								errno = ENOSPC;
#endif
								return static_cast<pwrite_ssize_t>(-1);
							}
						} catch (const std::exception &) {
							// Only native LargeValue manifest pages are this fault point.
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
	const std::vector<uint64_t> committed = retryBulk->appendPersistedBulkEdges(
			{"BULK_LARGE_VALUE_MANIFEST_DISK_FULL_RETRY", sources, targets, columns});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_LARGE_VALUE_MANIFEST_DISK_FULL_RETRY",
						   {{"payload", PropertyValue(payload)}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeLargeValueManifestShortWriteRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> existing = backend.createLogicalEdges({{nodes[0], nodes[1], "EXISTING_EDGE", {}}});
	ASSERT_EQ(existing.size(), 1U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::string payload(RSS_SEGMENT_SIZE / 2U, 'x');
	const std::array<PropertyValue, 1U> values{PropertyValue(payload)};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", values}}};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_LARGE_VALUE_MANIFEST_SHORT_WRITE", sources, targets, columns});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortLargeValueManifestPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const std::exception &) {
							// Only an authenticated LargeValue manifest page is this partial-write point.
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
	const std::vector<uint64_t> committed = retryBulk->appendPersistedBulkEdges(
			{"BULK_LARGE_VALUE_MANIFEST_SHORT_WRITE_RETRY", sources, targets, columns});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_LARGE_VALUE_MANIFEST_SHORT_WRITE_RETRY",
						   {{"payload", PropertyValue(payload)}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		StreamedBulkEdgeDeduplicatedLargeValueSecondManifestShortWriteRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> existing = backend.createLogicalEdges({{nodes[0], nodes[1], "EXISTING_EDGE", {}}});
	ASSERT_EQ(existing.size(), 1U);

	const std::array<uint64_t, 3U> sources{nodes[0], nodes[0], nodes[0]};
	const std::array<uint64_t, 3U> targets{nodes[1], nodes[1], nodes[1]};
	const std::string duplicate(RSS_SEGMENT_SIZE / 2U, 'd');
	const std::string distinct(RSS_SEGMENT_SIZE / 2U, 'u');
	const std::array<PropertyValue, 3U> values{PropertyValue(duplicate), PropertyValue(distinct),
															 PropertyValue(duplicate)};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", values}}};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_DEDUP_MANIFEST_SHORT_WRITE", sources, targets, columns});
	ASSERT_EQ(rejected.size(), 3U);

	std::atomic<size_t> manifestWrites{0U};
	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortSecondCanonicalManifest(
				[&manifestWrites, &injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST &&
								manifestWrites.fetch_add(1U, std::memory_order_relaxed) == 1U) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const std::exception &) {
							// Only authenticated LargeValue manifest pages participate in this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(manifestWrites.load(std::memory_order_relaxed), 2U);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()), LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE"));
	for (const uint64_t edgeId: rejected) {
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(edgeId)), FormatException);
	}
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_DEDUP_MANIFEST_SHORT_WRITE_RETRY", sources, targets, columns});
	ASSERT_EQ(committed.size(), 3U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed[0]).properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"payload", PropertyValue(duplicate)}}));
	EXPECT_EQ(snapshot->readEdge(committed[1]).properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"payload", PropertyValue(distinct)}}));
	EXPECT_EQ(snapshot->readEdge(committed[2]).properties,
			  (std::vector<LogicalGraphPropertyEntry>{{"payload", PropertyValue(duplicate)}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeAdjacencyIndexDiskFullRetainsOldGraphAndAllowsRetry) {
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
			bulk->appendPersistedBulkEdges({"BULK_ADJACENCY_DISK_FULL", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnGlobalAdjacencyIndexPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_INDEX &&
								owner.owner_id == RSS_GLOBAL_ADJACENCY_INDEX_ID) {
								injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
								SetLastError(ERROR_DISK_FULL);
#else
								errno = ENOSPC;
#endif
								return static_cast<pwrite_ssize_t>(-1);
							}
						} catch (const std::exception &) {
							// Only the global adjacency B+Tree is this fault point.
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
			retryBulk->appendPersistedBulkEdges({"BULK_ADJACENCY_DISK_FULL_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[1], nodes[0], "BULK_ADJACENCY_DISK_FULL_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeAdjacencyIndexShortWriteRetainsOldGraphAndAllowsRetry) {
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
			bulk->appendPersistedBulkEdges({"BULK_ADJACENCY_SHORT_WRITE", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortGlobalAdjacencyIndexPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_INDEX &&
								owner.owner_id == RSS_GLOBAL_ADJACENCY_INDEX_ID) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const std::exception &) {
							// Only the global adjacency B+Tree is this partial-write point.
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
			retryBulk->appendPersistedBulkEdges({"BULK_ADJACENCY_SHORT_WRITE_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[1], nodes[0], "BULK_ADJACENCY_SHORT_WRITE_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeMetadataPageDiskFullRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	uint64_t metadataPoolOffset = 0U;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		metadataPoolOffset = snapshot.superblock().data_region_offset;
	}

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
			bulk->appendPersistedBulkEdges({"BULK_METADATA_DISK_FULL", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnMetadataPage(
				[metadataPoolOffset, &injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
					if (byteCount == RSS_PAGE_SIZE && absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
						absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE &&
						!injected.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
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
			retryBulk->appendPersistedBulkEdges({"BULK_METADATA_DISK_FULL_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[1], nodes[0], "BULK_METADATA_DISK_FULL_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeMetadataPageShortWriteRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	uint64_t metadataPoolOffset = 0U;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		metadataPoolOffset = snapshot.superblock().data_region_offset;
	}

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
			bulk->appendPersistedBulkEdges({"BULK_METADATA_SHORT_WRITE", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortMetadataPage(
				[metadataPoolOffset, &injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
					if (byteCount == RSS_PAGE_SIZE && absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
						absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE &&
						!injected.exchange(true, std::memory_order_relaxed)) {
						return portable_pwrite(fd, bytes, 16U, offset);
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
			retryBulk->appendPersistedBulkEdges({"BULK_METADATA_SHORT_WRITE_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[1], nodes[0], "BULK_METADATA_SHORT_WRITE_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeShortPrivateScratchWriteRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_PRIVATE_SHORT_WRITE", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortPrivateScratchSegment(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_SEGMENT_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto segment =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const SegmentEnvelope envelope =
									SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
							if (envelope.kind == SegmentKind::SGK_BULK_SCRATCH &&
								envelope.state == SegmentState::SGS_SEALED && envelope.payload_bytes != 0U) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const FormatException &) {
							// Only an authenticated private scratch image is this fault point.
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
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphTransaction> retry = backend.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_PRIVATE_SHORT_WRITE_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_PRIVATE_SHORT_WRITE_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgePrivateScratchDiskFullRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_PRIVATE_DISK_FULL", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnPrivateScratchSegment(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_SEGMENT_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto segment =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const SegmentEnvelope envelope =
									SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
							if (envelope.kind == SegmentKind::SGK_BULK_SCRATCH &&
								envelope.state == SegmentState::SGS_SEALED && envelope.payload_bytes != 0U) {
								injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
								SetLastError(ERROR_DISK_FULL);
#else
								errno = ENOSPC;
#endif
								return static_cast<pwrite_ssize_t>(-1);
							}
						} catch (const FormatException &) {
							// Only an authenticated private scratch image is this fault point.
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
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphTransaction> retry = backend.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_PRIVATE_DISK_FULL_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_PRIVATE_DISK_FULL_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgePublicDataSyncFailureRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected = bulk->appendPersistedBulkEdges({"BULK_DATA_SYNC", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> sawPublicEntityPage{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observePublicEntityPage(
				[&sawPublicEntityPage](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_ENTITY) {
								sawPublicEntityPage.store(true, std::memory_order_relaxed);
							}
						} catch (const std::exception &) {
							// Only complete public page images participate in this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFirstPublicDataFence(
				[&sawPublicEntityPage, &failedDataFence](file_handle_t fd) {
					if (sawPublicEntityPage.load(std::memory_order_relaxed) &&
						!failedDataFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::runtime_error);
	}
	EXPECT_TRUE(sawPublicEntityPage.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 0U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphTransaction> retry = backend.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_DATA_SYNC_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_DATA_SYNC_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgePropertyDataSyncFailureRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> existing =
			backend.createLogicalEdges({{nodes[0], nodes[1], "EXISTING_EDGE", {{"rank", PropertyValue(int64_t{1})}}}});
	ASSERT_EQ(existing.size(), 1U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::array<PropertyValue, 1U> values{PropertyValue("property-data-sync")};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", values}}};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_PROPERTY_DATA_SYNC", sources, targets, columns});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> sawPublicPropertyPage{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observePublicPropertyPage(
				[&sawPublicPropertyPage](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_PROPERTY) {
								sawPublicPropertyPage.store(true, std::memory_order_relaxed);
							}
						} catch (const std::exception &) {
							// Only authenticated public Property pages arm this durability fence.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFirstPropertyDataFence(
				[&sawPublicPropertyPage, &failedDataFence](file_handle_t fd) {
					if (sawPublicPropertyPage.load(std::memory_order_relaxed) &&
						!failedDataFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::runtime_error);
	}
	EXPECT_TRUE(sawPublicPropertyPage.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()),
			  LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE", {{"rank", PropertyValue(int64_t{1})}}));
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()),
			  LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE", {{"rank", PropertyValue(int64_t{1})}}));
	snapshot.reset();
	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_PROPERTY_DATA_SYNC_RETRY", sources, targets, columns});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_PROPERTY_DATA_SYNC_RETRY",
						   {{"payload", PropertyValue("property-data-sync")}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeLargeValueDataSyncFailureRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> existing = backend.createLogicalEdges({{nodes[0], nodes[1], "EXISTING_EDGE", {}}});
	ASSERT_EQ(existing.size(), 1U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::string payload(RSS_SEGMENT_SIZE / 2U, 'x');
	const std::array<PropertyValue, 1U> values{PropertyValue(payload)};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", values}}};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_LARGE_VALUE_DATA_SYNC", sources, targets, columns});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> sawLargeValueDataPage{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeLargeValueDataPage(
				[&sawLargeValueDataPage](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_LARGE_VALUE_DATA) {
								sawLargeValueDataPage.store(true, std::memory_order_relaxed);
							}
						} catch (const std::exception &) {
							// Only authenticated public LargeValue pages arm this durability fence.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFirstLargeValueDataFence(
				[&sawLargeValueDataPage, &failedDataFence](file_handle_t fd) {
					if (sawLargeValueDataPage.load(std::memory_order_relaxed) &&
						!failedDataFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::runtime_error);
	}
	EXPECT_TRUE(sawLargeValueDataPage.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
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
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()), LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE"));
	snapshot.reset();
	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_LARGE_VALUE_DATA_SYNC_RETRY", sources, targets, columns});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_LARGE_VALUE_DATA_SYNC_RETRY",
							   {{"payload", PropertyValue(payload)}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeLargeValueManifestDataSyncFailureRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> existing = backend.createLogicalEdges({{nodes[0], nodes[1], "EXISTING_EDGE", {}}});
	ASSERT_EQ(existing.size(), 1U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	const std::string payload(RSS_SEGMENT_SIZE / 2U, 'x');
	const std::array<PropertyValue, 1U> values{PropertyValue(payload)};
	const std::array<LogicalGraphBulkEdgePropertyColumn, 1U> columns{{{"payload", values}}};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_LARGE_VALUE_MANIFEST_DATA_SYNC", sources, targets, columns});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> sawLargeValueManifestPage{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeLargeValueManifestPage(
				[&sawLargeValueManifestPage](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST) {
								sawLargeValueManifestPage.store(true, std::memory_order_relaxed);
							}
						} catch (const std::exception &) {
							// Only authenticated public LargeValue manifest pages arm this durability fence.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFirstLargeValueManifestDataFence(
				[&sawLargeValueManifestPage, &failedDataFence](file_handle_t fd) {
					if (sawLargeValueManifestPage.load(std::memory_order_relaxed) &&
						!failedDataFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::runtime_error);
	}
	EXPECT_TRUE(sawLargeValueManifestPage.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
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
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()), LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE"));
	snapshot.reset();
	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed = retryBulk->appendPersistedBulkEdges(
			{"BULK_LARGE_VALUE_MANIFEST_DATA_SYNC_RETRY", sources, targets, columns});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_LARGE_VALUE_MANIFEST_DATA_SYNC_RETRY",
							   {{"payload", PropertyValue(payload)}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeAdjacencyDataSyncFailureRetainsOldGraphAndAllowsRetry) {
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
			bulk->appendPersistedBulkEdges({"BULK_ADJACENCY_DATA_SYNC", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> sawGlobalAdjacencyIndexPage{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeGlobalAdjacencyIndexPage(
				[&sawGlobalAdjacencyIndexPage](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_INDEX &&
								owner.owner_id == RSS_GLOBAL_ADJACENCY_INDEX_ID) {
								sawGlobalAdjacencyIndexPage.store(true, std::memory_order_relaxed);
							}
						} catch (const std::exception &) {
							// Only the global adjacency B+Tree arms this durability fence.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFirstAdjacencyDataFence(
				[&sawGlobalAdjacencyIndexPage, &failedDataFence](file_handle_t fd) {
					if (sawGlobalAdjacencyIndexPage.load(std::memory_order_relaxed) &&
						!failedDataFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::runtime_error);
	}
	EXPECT_TRUE(sawGlobalAdjacencyIndexPage.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
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
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()), LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE"));
	snapshot.reset();
	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_ADJACENCY_DATA_SYNC_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[1], nodes[0], "BULK_ADJACENCY_DATA_SYNC_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeMetadataDataSyncFailureRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	uint64_t metadataPoolOffset = 0U;
	{
		auto data = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
		ASSERT_TRUE(data->is_open());
		auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(io);
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		metadataPoolOffset = snapshot.superblock().data_region_offset;
	}

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
			bulk->appendPersistedBulkEdges({"BULK_METADATA_DATA_SYNC", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> sawMetadataPage{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeMetadataPage(
				[metadataPoolOffset, &sawMetadataPage](file_handle_t fd, const void *bytes, size_t byteCount,
														  pwrite_off_t offset) {
					const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
					if (byteCount == RSS_PAGE_SIZE && absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
						absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE) {
						sawMetadataPage.store(true, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFirstMetadataDataFence(
				[&sawMetadataPage, &failedDataFence](file_handle_t fd) {
					if (sawMetadataPage.load(std::memory_order_relaxed) &&
						!failedDataFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::runtime_error);
	}
	EXPECT_TRUE(sawMetadataPage.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
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
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()), LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE"));
	snapshot.reset();
	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_METADATA_DATA_SYNC_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[1], nodes[0], "BULK_METADATA_DATA_SYNC_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeAllocatorDataSyncFailureRetainsOldGraphAndAllowsRetry) {
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
			bulk->appendPersistedBulkEdges({"BULK_ALLOCATOR_DATA_SYNC", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> sawAllocatorLeaf{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeAllocatorLeaf(
				[&sawAllocatorLeaf](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							static_cast<void>(AllocatorLeafPageCodec::decode(page));
							sawAllocatorLeaf.store(true, std::memory_order_relaxed);
						} catch (const std::exception &) {
							// Only authenticated allocator bitmap leaves arm this durability fence.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFirstAllocatorDataFence(
				[&sawAllocatorLeaf, &failedDataFence](file_handle_t fd) {
					if (sawAllocatorLeaf.load(std::memory_order_relaxed) &&
						!failedDataFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::runtime_error);
	}
	EXPECT_TRUE(sawAllocatorLeaf.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
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
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(existing.front()), LogicalGraphEdge(existing.front(), nodes[0], nodes[1], "EXISTING_EDGE"));
	snapshot.reset();
	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_ALLOCATOR_DATA_SYNC_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[1], nodes[0], "BULK_ALLOCATOR_DATA_SYNC_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgePrivateScratchSyncFailureRetainsOldGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	const std::array<uint64_t, 1U> sources{nodes[0]};
	const std::array<uint64_t, 1U> targets{nodes[1]};
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_PRIVATE_SYNC", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 1U);

	std::atomic<bool> sawPrivateScratchSegment{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observePrivateScratchSegment(
				[&sawPrivateScratchSegment](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_SEGMENT_SIZE) {
						try {
							const auto segment =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const SegmentEnvelope envelope =
									SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
							if (envelope.kind == SegmentKind::SGK_BULK_SCRATCH &&
								envelope.state == SegmentState::SGS_SEALED && envelope.payload_bytes != 0U) {
								sawPrivateScratchSegment.store(true, std::memory_order_relaxed);
							}
						} catch (const FormatException &) {
							// Only authenticated sealed scratch images arm this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFirstScratchDataFence(
				[&sawPrivateScratchSegment, &failedDataFence](file_handle_t fd) {
					if (sawPrivateScratchSegment.load(std::memory_order_relaxed) &&
						!failedDataFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), std::runtime_error);
	}
	EXPECT_TRUE(sawPrivateScratchSegment.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 0U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(rejected.front())), FormatException);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphTransaction> retry = backend.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> committed =
			retryBulk->appendPersistedBulkEdges({"BULK_PRIVATE_SYNC_RETRY", sources, targets, {}});
	ASSERT_EQ(committed.size(), 1U);
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	retry.reset();
	EXPECT_EQ(backend.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(committed.front()),
			  LogicalGraphEdge(committed.front(), nodes[0], nodes[1], "BULK_PRIVATE_SYNC_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeRebuildsStaleFreeSummaryInItsPublication) {
    materializeNativeDatabase();
    auto options = backendOptions();
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    std::vector<uint64_t> nodes;
    uint64_t oldEdge = 0U;
    uint64_t expectedFreePages = 0U;
    {
        RelocatableStorageBackend backend(options);
        backend.open();
        nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
        ASSERT_EQ(nodes.size(), 2U);
        oldEdge = backend.createLogicalEdges({{nodes[0], nodes[1], "OLD", {}}}).at(0);
        ASSERT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
        backend.close();
    }
    // Preserve the existing entries but make their source-directory marker
    // stale, as happens after directory relocation. Reopen exercises the real
    // metadata writer and its budget rather than a mock summary provider.
    {
        auto stream = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(stream->is_open());
        auto io = std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io);
        SlottedPagePool pools(io, *metadata);
        BTreePageStore trees(pools, *metadata);
        auto pin = metadata->openSnapshot();
        const auto proof = EntityFreeSpaceIndex::readProof(*metadata, trees, pin, EntityPageRecordKind::EPRK_EDGE);
        ASSERT_TRUE(proof.has_value());
        ASSERT_TRUE(proof->fresh);
        expectedFreePages = proof->free_pages + 1U;
        const auto checkpoint = LogSequenceNumber(pin.superblock().checkpoint_lsn.value() + 1U);
        pin.release();
        static_cast<void>(trees.upsert(RSS_EDGE_FREE_SPACE_INDEX_ID, EntityFreeSpaceIndex::generationKey(), 1U, checkpoint));
    }
    RelocatableStorageBackend backend(options);
    backend.open();
    ASSERT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
    auto oldSnapshot = backend.openLogicalGraphReadSnapshot();
    auto transaction = backend.beginLogicalGraphTransaction(false);
    auto *bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
    ASSERT_NE(bulk, nullptr);
    const std::array<uint64_t, 1U> sources{nodes[0]}, targets{nodes[1]};
    const auto added = bulk->appendPersistedBulkEdges({"NEW", sources, targets, {}});
    ASSERT_EQ(added.size(), 1U);
    ASSERT_NO_THROW(static_cast<void>(transaction->commit()));
    transaction.reset();
    EXPECT_NE(oldEdge, added.front());
    EXPECT_THROW(static_cast<void>(oldSnapshot->readEdge(added.front())), FormatException);
    EXPECT_EQ(oldSnapshot->readEdge(oldEdge).target_node_id, nodes[1]);
    auto currentSnapshot = backend.openLogicalGraphReadSnapshot();
    EXPECT_NO_THROW(static_cast<void>(currentSnapshot->readEdge(oldEdge)));
    EXPECT_NO_THROW(static_cast<void>(currentSnapshot->readEdge(added.front())));
    currentSnapshot.reset();
    EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
    oldSnapshot.reset();
    backend.close();
    {
        auto stream = std::make_shared<std::fstream>(data_path_, std::ios::in | std::ios::out | std::ios::binary);
        auto io = std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        auto metadata = RelocatableMetadataStore::open(io);
        SlottedPagePool pools(io, *metadata);
        BTreePageStore trees(pools, *metadata);
        auto pin = metadata->openSnapshot();
        const auto proof = EntityFreeSpaceIndex::readProof(*metadata, trees, pin, EntityPageRecordKind::EPRK_EDGE);
        ASSERT_TRUE(proof.has_value());
        EXPECT_TRUE(proof->fresh);
        EXPECT_EQ(proof->free_pages, expectedFreePages);
    }
    backend.open();
    EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
    backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedBulkEdgeRetainsFreshSummaryInItsPublication) {
    materializeNativeDatabase();
    auto options = backendOptions();
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    std::vector<uint64_t> nodes;
    uint64_t oldEdge = 0U;
    {
        RelocatableStorageBackend backend(options);
        backend.open();
        nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
        ASSERT_EQ(nodes.size(), 2U);
        oldEdge = backend.createLogicalEdges({{nodes[0], nodes[1], "OLD", {}}}).at(0);
        ASSERT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
        backend.close();
    }
    RelocatableStorageBackend backend(options);
    backend.open();
    ASSERT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
    auto oldSnapshot = backend.openLogicalGraphReadSnapshot();
    auto transaction = backend.beginLogicalGraphTransaction(false);
    auto *bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
    ASSERT_NE(bulk, nullptr);
    const std::array<uint64_t, 1U> sources{nodes[0]}, targets{nodes[1]};
    const auto added = bulk->appendPersistedBulkEdges({"NEW", sources, targets, {}});
    ASSERT_EQ(added.size(), 1U);
    ASSERT_NO_THROW(static_cast<void>(transaction->commit()));
    transaction.reset();
    EXPECT_NE(oldEdge, added.front());
    EXPECT_THROW(static_cast<void>(oldSnapshot->readEdge(added.front())), FormatException);
    EXPECT_EQ(oldSnapshot->readEdge(oldEdge).target_node_id, nodes[1]);
    auto currentSnapshot = backend.openLogicalGraphReadSnapshot();
    EXPECT_NO_THROW(static_cast<void>(currentSnapshot->readEdge(oldEdge)));
    EXPECT_NO_THROW(static_cast<void>(currentSnapshot->readEdge(added.front())));
    currentSnapshot.reset();
    EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
    oldSnapshot.reset();
    backend.close();
    backend.open();
    EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
    auto reopenedSnapshot = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(reopenedSnapshot->readEdge(oldEdge).target_node_id, nodes[1]);
    EXPECT_EQ(reopenedSnapshot->readEdge(added.front()).target_node_id, nodes[1]);
    reopenedSnapshot.reset();
    backend.close();
}
