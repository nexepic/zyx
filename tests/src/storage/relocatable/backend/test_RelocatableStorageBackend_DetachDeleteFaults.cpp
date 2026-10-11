/**
 * @file test_RelocatableStorageBackend_DetachDeleteFaults.cpp
 * @brief Native I/O atomicity and recovery contracts for DETACH DELETE.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include <atomic>
#include <cerrno>
#include <span>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest,
		DetachDeleteShortAdjacencyPageRetainsTheCompleteOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> retainedEdges = backend.createLogicalEdges(
			{{fixture.unlabeled_node_id, fixture.unlabeled_node_id, "RETAINED_AFTER_DETACH", {}}});
	ASSERT_EQ(retainedEdges.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortAdjacencyPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_INDEX) {
								const BTreePage tree =
										BTreePage::open(owner, page.subspan(RSS_PAGE_OWNER_HEADER_BYTES, owner.payload_bytes));
								if (tree.header().index_id == RSS_GLOBAL_ADJACENCY_INDEX_ID) {
									injected.store(true, std::memory_order_relaxed);
									return portable_pwrite(fd, bytes, 16U, offset);
								}
							}
						} catch (const std::exception &) {
							// Only an authenticated global adjacency candidate is this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(backend.deleteLogicalNodes({{fixture.person_node_id, true, 2U}}), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 3U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
			  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
								{{"alpha", PropertyValue(int64_t(7))}}}));
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
								"RETAINED_AFTER_DETACH"));
	snapshot.reset();
	reopened.deleteLogicalNodes({{fixture.person_node_id, true, 2U}});
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readNode(fixture.unlabeled_node_id).node_id, fixture.unlabeled_node_id);
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
								"RETAINED_AFTER_DETACH"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		DetachDeleteAdjacencyPageDiskFullRetainsTheCompleteOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> retainedEdges = backend.createLogicalEdges(
			{{fixture.unlabeled_node_id, fixture.unlabeled_node_id, "RETAINED_AFTER_DETACH", {}}});
	ASSERT_EQ(retainedEdges.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnAdjacencyPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_INDEX) {
								const BTreePage tree =
										BTreePage::open(owner, page.subspan(RSS_PAGE_OWNER_HEADER_BYTES, owner.payload_bytes));
								if (tree.header().index_id == RSS_GLOBAL_ADJACENCY_INDEX_ID) {
									injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
									SetLastError(ERROR_DISK_FULL);
#else
									errno = ENOSPC;
#endif
									return static_cast<pwrite_ssize_t>(-1);
								}
							}
						} catch (const std::exception &) {
							// Only an authenticated global adjacency candidate is this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(backend.deleteLogicalNodes({{fixture.person_node_id, true, 2U}}), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 3U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
			  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
								{{"alpha", PropertyValue(int64_t(7))}}}));
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
								"RETAINED_AFTER_DETACH"));
	snapshot.reset();
	reopened.deleteLogicalNodes({{fixture.person_node_id, true, 2U}});
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readNode(fixture.unlabeled_node_id).node_id, fixture.unlabeled_node_id);
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
								"RETAINED_AFTER_DETACH"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		DetachDeleteAdjacencyDataSyncFailureRetainsTheCompleteOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> retainedEdges = backend.createLogicalEdges(
			{{fixture.unlabeled_node_id, fixture.unlabeled_node_id, "RETAINED_AFTER_DETACH_DATA_SYNC", {}}});
	ASSERT_EQ(retainedEdges.size(), 1U);

	std::atomic<bool> sawAdjacencyCandidate{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeAdjacencyCandidate(
				[&sawAdjacencyCandidate](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_INDEX) {
								const BTreePage tree =
										BTreePage::open(owner, page.subspan(RSS_PAGE_OWNER_HEADER_BYTES, owner.payload_bytes));
								if (tree.header().index_id == RSS_GLOBAL_ADJACENCY_INDEX_ID) {
									sawAdjacencyCandidate.store(true, std::memory_order_relaxed);
								}
							}
						} catch (const std::exception &) {
							// Only an authenticated global adjacency candidate arms this fence.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failAdjacencyDataFence(
				[&sawAdjacencyCandidate, &failedDataFence](file_handle_t fd) {
					if (sawAdjacencyCandidate.load(std::memory_order_relaxed) &&
						!failedDataFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(backend.deleteLogicalNodes({{fixture.person_node_id, true, 2U}}), std::runtime_error);
	}
	EXPECT_TRUE(sawAdjacencyCandidate.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 3U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
			  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
								{{"alpha", PropertyValue(int64_t(7))}}}));
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
							   "RETAINED_AFTER_DETACH_DATA_SYNC"));
	snapshot.reset();
	reopened.deleteLogicalNodes({{fixture.person_node_id, true, 2U}});
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readNode(fixture.unlabeled_node_id).node_id, fixture.unlabeled_node_id);
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
							   "RETAINED_AFTER_DETACH_DATA_SYNC"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		DetachDeleteShortEntityPageRetainsTheCompleteOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> retainedEdges = backend.createLogicalEdges(
			{{fixture.unlabeled_node_id, fixture.unlabeled_node_id, "RETAINED_AFTER_DETACH_ENTITY_FAULT", {}}});
	ASSERT_EQ(retainedEdges.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortEntityPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_ENTITY) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const std::exception &) {
							// Only authenticated Entity-page candidates are this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(backend.deleteLogicalNodes({{fixture.person_node_id, true, 2U}}), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 3U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(fixture.person_node_id).node_id, fixture.person_node_id);
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
			  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
								{{"alpha", PropertyValue(int64_t(7))}}}));
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
						   "RETAINED_AFTER_DETACH_ENTITY_FAULT"));
	snapshot.reset();
	reopened.deleteLogicalNodes({{fixture.person_node_id, true, 2U}});
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readNode(fixture.unlabeled_node_id).node_id, fixture.unlabeled_node_id);
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
						   "RETAINED_AFTER_DETACH_ENTITY_FAULT"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		DetachDeleteEntityPageDiskFullRetainsTheCompleteOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> retainedEdges = backend.createLogicalEdges(
			{{fixture.unlabeled_node_id, fixture.unlabeled_node_id, "RETAINED_AFTER_DETACH_ENTITY_DISK_FULL", {}}});
	ASSERT_EQ(retainedEdges.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnEntityPage(
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
							// Only authenticated Entity-page candidates are this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(backend.deleteLogicalNodes({{fixture.person_node_id, true, 2U}}), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 3U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(fixture.person_node_id).node_id, fixture.person_node_id);
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
			  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
								{{"alpha", PropertyValue(int64_t(7))}}}));
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
						   "RETAINED_AFTER_DETACH_ENTITY_DISK_FULL"));
	snapshot.reset();
	reopened.deleteLogicalNodes({{fixture.person_node_id, true, 2U}});
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readNode(fixture.unlabeled_node_id).node_id, fixture.unlabeled_node_id);
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
						   "RETAINED_AFTER_DETACH_ENTITY_DISK_FULL"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		DetachDeleteShortMetadataPageRetainsTheCompleteOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
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
	const std::vector<uint64_t> retainedEdges = backend.createLogicalEdges(
			{{fixture.unlabeled_node_id, fixture.unlabeled_node_id, "RETAINED_AFTER_DETACH_METADATA_FAULT", {}}});
	ASSERT_EQ(retainedEdges.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortMetadataPage(
				[metadataPoolOffset, &injected](file_handle_t fd, const void *bytes, size_t byteCount,
													 pwrite_off_t offset) {
					const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
					if (byteCount == RSS_PAGE_SIZE && absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
						absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE &&
						!injected.load(std::memory_order_relaxed)) {
						injected.store(true, std::memory_order_relaxed);
						return portable_pwrite(fd, bytes, 16U, offset);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(backend.deleteLogicalNodes({{fixture.person_node_id, true, 2U}}), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 3U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(fixture.person_node_id).node_id, fixture.person_node_id);
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
			  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
								{{"alpha", PropertyValue(int64_t(7))}}}));
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
						   "RETAINED_AFTER_DETACH_METADATA_FAULT"));
	snapshot.reset();
	reopened.deleteLogicalNodes({{fixture.person_node_id, true, 2U}});
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readNode(fixture.unlabeled_node_id).node_id, fixture.unlabeled_node_id);
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
						   "RETAINED_AFTER_DETACH_METADATA_FAULT"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		DetachDeleteMetadataPageDiskFullRetainsTheCompleteOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
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
	const std::vector<uint64_t> retainedEdges = backend.createLogicalEdges(
			{{fixture.unlabeled_node_id, fixture.unlabeled_node_id, "RETAINED_AFTER_DETACH_METADATA_DISK_FULL", {}}});
	ASSERT_EQ(retainedEdges.size(), 1U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting diskFullOnMetadataPage(
				[metadataPoolOffset, &injected](file_handle_t fd, const void *bytes, size_t byteCount,
														 pwrite_off_t offset) {
					const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
					if (byteCount == RSS_PAGE_SIZE && absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
						absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE &&
						!injected.load(std::memory_order_relaxed)) {
						injected.store(true, std::memory_order_relaxed);
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return static_cast<pwrite_ssize_t>(-1);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_THROW(backend.deleteLogicalNodes({{fixture.person_node_id, true, 2U}}), std::system_error);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 3U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(fixture.person_node_id).node_id, fixture.person_node_id);
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
			  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
								{{"alpha", PropertyValue(int64_t(7))}}}));
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
						   "RETAINED_AFTER_DETACH_METADATA_DISK_FULL"));
	snapshot.reset();
	reopened.deleteLogicalNodes({{fixture.person_node_id, true, 2U}});
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readNode(fixture.unlabeled_node_id).node_id, fixture.unlabeled_node_id);
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
						   "RETAINED_AFTER_DETACH_METADATA_DISK_FULL"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		DetachDeleteEntityDataSyncFailureRetainsTheCompleteOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> retainedEdges = backend.createLogicalEdges(
			{{fixture.unlabeled_node_id, fixture.unlabeled_node_id, "RETAINED_AFTER_DETACH_ENTITY_DATA_SYNC", {}}});
	ASSERT_EQ(retainedEdges.size(), 1U);

	std::atomic<bool> sawEntityCandidate{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeEntityCandidate(
				[&sawEntityCandidate](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE) {
						try {
							const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							const PageOwnerHeader owner =
									PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
							if (owner.kind == PageOwnerKind::POK_ENTITY) {
								sawEntityCandidate.store(true, std::memory_order_relaxed);
							}
						} catch (const std::exception &) {
							// Only authenticated Entity-page candidates arm this fence.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failEntityDataFence(
				[&sawEntityCandidate, &failedDataFence](file_handle_t fd) {
					if (sawEntityCandidate.load(std::memory_order_relaxed) &&
						!failedDataFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(backend.deleteLogicalNodes({{fixture.person_node_id, true, 2U}}), std::runtime_error);
	}
	EXPECT_TRUE(sawEntityCandidate.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 3U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(fixture.person_node_id).node_id, fixture.person_node_id);
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
			  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
								{{"alpha", PropertyValue(int64_t(7))}}}));
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
							   "RETAINED_AFTER_DETACH_ENTITY_DATA_SYNC"));
	snapshot.reset();
	reopened.deleteLogicalNodes({{fixture.person_node_id, true, 2U}});
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
							   "RETAINED_AFTER_DETACH_ENTITY_DATA_SYNC"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		DetachDeleteMetadataDataSyncFailureRetainsTheCompleteOldGraphAndAllowsRetryAfterReopen) {
	const NativeGraphFixture fixture = materializeNativeGraphFixture();
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
	const std::vector<uint64_t> retainedEdges = backend.createLogicalEdges(
			{{fixture.unlabeled_node_id, fixture.unlabeled_node_id, "RETAINED_AFTER_DETACH_METADATA_DATA_SYNC", {}}});
	ASSERT_EQ(retainedEdges.size(), 1U);

	std::atomic<bool> sawMetadataCandidate{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeMetadataCandidate(
				[metadataPoolOffset, &sawMetadataCandidate](file_handle_t fd, const void *bytes, size_t byteCount,
															pwrite_off_t offset) {
					const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
					if (byteCount == RSS_PAGE_SIZE && absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
						absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE) {
						sawMetadataCandidate.store(true, std::memory_order_relaxed);
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failMetadataDataFence(
				[&sawMetadataCandidate, &failedDataFence](file_handle_t fd) {
					if (sawMetadataCandidate.load(std::memory_order_relaxed) &&
						!failedDataFence.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
						SetLastError(ERROR_WRITE_FAULT);
#else
						errno = EIO;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
		EXPECT_THROW(backend.deleteLogicalNodes({{fixture.person_node_id, true, 2U}}), std::runtime_error);
	}
	EXPECT_TRUE(sawMetadataCandidate.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 3U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(fixture.person_node_id).node_id, fixture.person_node_id);
	EXPECT_EQ(snapshot->readEdge(fixture.knows_edge_id),
			  (LogicalGraphEdge{fixture.knows_edge_id, fixture.person_node_id, fixture.person_node_id, "KNOWS",
								{{"alpha", PropertyValue(int64_t(7))}}}));
	EXPECT_EQ(snapshot->readEdge(fixture.mentions_edge_id),
			  LogicalGraphEdge(fixture.mentions_edge_id, fixture.person_node_id, fixture.person_node_id, "MENTIONS"));
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
							   "RETAINED_AFTER_DETACH_METADATA_DATA_SYNC"));
	snapshot.reset();
	reopened.deleteLogicalNodes({{fixture.person_node_id, true, 2U}});
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 1U}));
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(fixture.person_node_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.knows_edge_id)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(fixture.mentions_edge_id)), FormatException);
	EXPECT_EQ(snapshot->readEdge(retainedEdges.front()),
			  LogicalGraphEdge(retainedEdges.front(), fixture.unlabeled_node_id, fixture.unlabeled_node_id,
							   "RETAINED_AFTER_DETACH_METADATA_DATA_SYNC"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}
