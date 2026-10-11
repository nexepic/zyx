/**
 * @file test_RelocatableProcessCrash_OrdinaryEdge.cpp
 * @brief Real process-crash recovery for ordinary public Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 */

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/index/BTreePage.hpp"
#include "storage/relocatable/format/RelocatableProcessCrashSupport.hpp"

using namespace graph::storage;

namespace {

	constexpr std::string_view CRASH_CHILD_ORDINARY_EDGE_BEFORE_PUBLICATION =
			"ordinary-edge-before-publication";
	constexpr std::string_view CRASH_CHILD_ORDINARY_EDGE_AFTER_PUBLICATION =
			"ordinary-edge-after-publication";
	constexpr std::string_view CRASH_CHILD_ORDINARY_EDGE_DELETE_BEFORE_PUBLICATION =
			"ordinary-edge-delete-before-publication";
	constexpr std::string_view CRASH_CHILD_ORDINARY_EDGE_DELETE_AFTER_PUBLICATION =
			"ordinary-edge-delete-after-publication";
	constexpr std::string_view CRASH_CHILD_ORDINARY_MULTI_EDGE_DELETE_BEFORE_PUBLICATION =
			"ordinary-multi-edge-delete-before-publication";
	constexpr std::string_view CRASH_CHILD_ORDINARY_MULTI_EDGE_DELETE_AFTER_PUBLICATION =
			"ordinary-multi-edge-delete-after-publication";
	constexpr std::string_view CRASH_CHILD_ORDINARY_DETACH_DELETE_BEFORE_PUBLICATION =
			"ordinary-detach-delete-before-publication";
	constexpr std::string_view CRASH_CHILD_ORDINARY_DETACH_DELETE_AFTER_PUBLICATION =
			"ordinary-detach-delete-after-publication";

	using graph::test::relocatable_crash::CrashChild;
	using graph::test::relocatable_crash::waitToBeKilled;
	using graph::test::relocatable_crash::writeFailureMarker;
	using graph::test::relocatable_crash::writeMarker;

} // namespace

namespace graph::test {

	int runRelocatableOrdinaryEdgeCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
																	 const std::filesystem::path &markerPath) {
		if (mode != CRASH_CHILD_ORDINARY_EDGE_BEFORE_PUBLICATION &&
			mode != CRASH_CHILD_ORDINARY_EDGE_AFTER_PUBLICATION &&
			mode != CRASH_CHILD_ORDINARY_EDGE_DELETE_BEFORE_PUBLICATION &&
			mode != CRASH_CHILD_ORDINARY_EDGE_DELETE_AFTER_PUBLICATION &&
			mode != CRASH_CHILD_ORDINARY_MULTI_EDGE_DELETE_BEFORE_PUBLICATION &&
			mode != CRASH_CHILD_ORDINARY_MULTI_EDGE_DELETE_AFTER_PUBLICATION &&
			mode != CRASH_CHILD_ORDINARY_DETACH_DELETE_BEFORE_PUBLICATION &&
			mode != CRASH_CHILD_ORDINARY_DETACH_DELETE_AFTER_PUBLICATION) {
			return -1;
		}
		try {
			const bool deleting = mode == CRASH_CHILD_ORDINARY_EDGE_DELETE_BEFORE_PUBLICATION ||
								 mode == CRASH_CHILD_ORDINARY_EDGE_DELETE_AFTER_PUBLICATION ||
								 mode == CRASH_CHILD_ORDINARY_MULTI_EDGE_DELETE_BEFORE_PUBLICATION ||
								 mode == CRASH_CHILD_ORDINARY_MULTI_EDGE_DELETE_AFTER_PUBLICATION ||
								 mode == CRASH_CHILD_ORDINARY_DETACH_DELETE_BEFORE_PUBLICATION ||
								 mode == CRASH_CHILD_ORDINARY_DETACH_DELETE_AFTER_PUBLICATION;
			const bool multiEdgeDelete = mode == CRASH_CHILD_ORDINARY_MULTI_EDGE_DELETE_BEFORE_PUBLICATION ||
										 mode == CRASH_CHILD_ORDINARY_MULTI_EDGE_DELETE_AFTER_PUBLICATION;
			const bool detachDelete = mode == CRASH_CHILD_ORDINARY_DETACH_DELETE_BEFORE_PUBLICATION ||
									  mode == CRASH_CHILD_ORDINARY_DETACH_DELETE_AFTER_PUBLICATION;
			const bool beforePublication = mode == CRASH_CHILD_ORDINARY_EDGE_BEFORE_PUBLICATION ||
											 mode == CRASH_CHILD_ORDINARY_EDGE_DELETE_BEFORE_PUBLICATION ||
											 mode == CRASH_CHILD_ORDINARY_MULTI_EDGE_DELETE_BEFORE_PUBLICATION ||
											 mode == CRASH_CHILD_ORDINARY_DETACH_DELETE_BEFORE_PUBLICATION;
			PhysicalPageRef priorEdgeDirectoryRoot;
			{
				auto data = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
				if (!data->is_open()) return 108;
				auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
				const Superblock prior = SuperblockFile(io).recover();
				priorEdgeDirectoryRoot = prior.edge_entity_directory_root;
			}

			RelocatableStorageBackendOptions options;
			options.data_path = databasePath;
			options.open_mode = OpenMode::OPEN_EXISTING_FILE;
			RelocatableStorageBackend backend(std::move(options));
			backend.open();

			if (beforePublication) {
				std::atomic<bool> observedCandidate{false};
				StorageIO::ScopedNativeWriteHookForTesting pauseAtEdgeCandidate(
						[deleting, multiEdgeDelete, &observedCandidate, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
																									 pwrite_off_t offset) {
							if (byteCount == RSS_PAGE_SIZE && !observedCandidate.load(std::memory_order_relaxed)) {
								if (deleting && !multiEdgeDelete &&
									(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
									observedCandidate.store(true, std::memory_order_relaxed);
									writeMarker(markerPath);
									waitToBeKilled();
								}
								try {
									const auto page =
											std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
									const PageOwnerHeader owner =
											PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
									const bool matchesCreateCandidate = !deleting && owner.kind == PageOwnerKind::POK_ENTITY;
									bool matchesMultiEdgeDeleteCandidate = false;
									if (deleting && multiEdgeDelete && owner.kind == PageOwnerKind::POK_INDEX) {
										const BTreePage tree = BTreePage::open(
												owner, page.subspan(RSS_PAGE_OWNER_HEADER_BYTES, owner.payload_bytes));
										matchesMultiEdgeDeleteCandidate = tree.header().index_id == RSS_GLOBAL_ADJACENCY_INDEX_ID;
									}
									if (matchesCreateCandidate || matchesMultiEdgeDeleteCandidate) {
										observedCandidate.store(true, std::memory_order_relaxed);
										writeMarker(markerPath);
										waitToBeKilled();
									}
								} catch (const FormatException &) {
									// Only authenticated Entity candidates are an ordinary Edge publication boundary.
								}
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				if (detachDelete) {
					backend.deleteLogicalNodes({{1U, true, 1U}});
				} else if (deleting) {
					backend.deleteLogicalEdges({{1U}});
				} else {
					static_cast<void>(backend.createLogicalEdges({{1U, 2U, "CRASH_EDGE", {}}}));
				}
				return 102;
			}

			std::atomic<bool> observedPublication{false};
			StorageIO::ScopedNativeWriteHookForTesting pauseAfterEdgePublication(
					[priorEdgeDirectoryRoot, &observedPublication, &markerPath](file_handle_t fd, const void *bytes,
							size_t byteCount, pwrite_off_t offset) {
						if (byteCount == RSS_PAGE_SIZE &&
							(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U)) &&
							!observedPublication.load(std::memory_order_relaxed)) {
							try {
								const Superblock candidate = SuperblockCodec::decode(
										std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
								if (candidate.edge_entity_directory_root != priorEdgeDirectoryRoot) {
									observedPublication.store(true, std::memory_order_relaxed);
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount)) return written;
									writeMarker(markerPath);
									waitToBeKilled();
								}
							} catch (const FormatException &) {
								// Ignore non-Superblock writes while locating the public Edge route publication.
							}
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			if (detachDelete) {
				backend.deleteLogicalNodes({{1U, true, 1U}});
			} else if (deleting) {
				backend.deleteLogicalEdges({{1U}});
			} else {
				static_cast<void>(backend.createLogicalEdges({{1U, 2U, "CRASH_EDGE", {}}}));
			}
			return 102;
		} catch (const std::exception &error) {
			writeFailureMarker(markerPath, error.what());
			return 101;
		}
	}

} // namespace graph::test

class RelocatableProcessCrashOrdinaryEdgeTest : public ::testing::Test {
protected:
	std::filesystem::path databasePath;
	std::filesystem::path markerPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		databasePath = std::filesystem::temp_directory_path() /
				("relocatable_ordinary_edge_" + to_string(uuid) + ".db");
		markerPath = databasePath.string() + ".marker";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(databasePath, error);
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
	}

	void createDatabaseWithEndpoints() const {
		RelocatableStorageBackendOptions options;
		options.data_path = databasePath;
		options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
		auto created = RelocatableStorageBackend::initializeNew(std::move(options));
		ASSERT_NE(created, nullptr);
		created->open();
		EXPECT_EQ(created->createLogicalNodes({{{"CrashSource"}, {}}, {{"CrashTarget"}, {}}}),
				  (std::vector<uint64_t>{1U, 2U}));
		created->close();
	}

	void createDatabaseWithEdge() const {
		createDatabaseWithEndpoints();
		RelocatableStorageBackendOptions options;
		options.data_path = databasePath;
		options.open_mode = OpenMode::OPEN_EXISTING_FILE;
		RelocatableStorageBackend backend(std::move(options));
		backend.open();
		EXPECT_EQ(backend.createLogicalEdges({{1U, 2U, "CRASH_EDGE", {}}}), (std::vector<uint64_t>{1U}));
		backend.close();
	}

	void createDatabaseWithTwoEdges() const {
		createDatabaseWithEndpoints();
		RelocatableStorageBackendOptions options;
		options.data_path = databasePath;
		options.open_mode = OpenMode::OPEN_EXISTING_FILE;
		RelocatableStorageBackend backend(std::move(options));
		backend.open();
		EXPECT_EQ(backend.createLogicalEdges({{1U, 2U, "CRASH_EDGE", {}}, {1U, 2U, "RETAINED_EDGE", {}}}),
				  (std::vector<uint64_t>{1U, 2U}));
		backend.close();
	}

};

TEST_F(RelocatableProcessCrashOrdinaryEdgeTest, KillBeforeOrdinaryEdgePublicationRetainsOldGraphAndCanRetry) {
	createDatabaseWithEndpoints();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ORDINARY_EDGE_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 0U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(1U)), FormatException);
	snapshot.reset();
	const std::vector<uint64_t> retried = reopened.createLogicalEdges({{1U, 2U, "CRASH_EDGE_RETRY", {}}});
	ASSERT_EQ(retried.size(), 1U);
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(retried.front()), LogicalGraphEdge(retried.front(), 1U, 2U, "CRASH_EDGE_RETRY"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableProcessCrashOrdinaryEdgeTest, KillAfterOrdinaryEdgePublicationRecoversCompleteEdgeAndAdjacency) {
	createDatabaseWithEndpoints();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ORDINARY_EDGE_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(1U), LogicalGraphEdge(1U, 1U, 2U, "CRASH_EDGE"));
	EXPECT_EQ(snapshot->scanAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U).edges,
			  (std::vector<LogicalGraphEdge>{{1U, 1U, 2U, "CRASH_EDGE"}}));
	EXPECT_EQ(snapshot->scanAdjacentEdges(2U, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U).edges,
			  (std::vector<LogicalGraphEdge>{{1U, 1U, 2U, "CRASH_EDGE"}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableProcessCrashOrdinaryEdgeTest, KillBeforeOrdinaryEdgeDeletionPublicationRetainsOldEdgeAndAdjacency) {
	createDatabaseWithEdge();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ORDINARY_EDGE_DELETE_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(1U), LogicalGraphEdge(1U, 1U, 2U, "CRASH_EDGE"));
	EXPECT_EQ(snapshot->scanAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U).edges,
			  (std::vector<LogicalGraphEdge>{{1U, 1U, 2U, "CRASH_EDGE"}}));
	EXPECT_EQ(snapshot->scanAdjacentEdges(2U, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U).edges,
			  (std::vector<LogicalGraphEdge>{{1U, 1U, 2U, "CRASH_EDGE"}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableProcessCrashOrdinaryEdgeTest, KillAfterOrdinaryEdgeDeletionPublicationRecoversNoPartialAdjacency) {
	createDatabaseWithEdge();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ORDINARY_EDGE_DELETE_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 0U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(1U)), FormatException);
	EXPECT_TRUE(snapshot->scanAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U)
						.edges.empty());
	EXPECT_TRUE(snapshot->scanAdjacentEdges(2U, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U)
						.edges.empty());
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableProcessCrashOrdinaryEdgeTest,
		KillBeforeMultiEdgeDeletionPublicationRetainsBothEdgesAndAllowsRetry) {
	createDatabaseWithTwoEdges();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ORDINARY_MULTI_EDGE_DELETE_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 2U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(1U), LogicalGraphEdge(1U, 1U, 2U, "CRASH_EDGE"));
	EXPECT_EQ(snapshot->readEdge(2U), LogicalGraphEdge(2U, 1U, 2U, "RETAINED_EDGE"));
	snapshot.reset();
	reopened.deleteLogicalEdges({{1U}});
	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(1U)), FormatException);
	EXPECT_EQ(snapshot->readEdge(2U), LogicalGraphEdge(2U, 1U, 2U, "RETAINED_EDGE"));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableProcessCrashOrdinaryEdgeTest,
		KillAfterMultiEdgeDeletionPublicationRetainsOnlyTheRequestedEdgeDeletion) {
	createDatabaseWithTwoEdges();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ORDINARY_MULTI_EDGE_DELETE_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(1U)), FormatException);
	EXPECT_EQ(snapshot->readEdge(2U), LogicalGraphEdge(2U, 1U, 2U, "RETAINED_EDGE"));
	EXPECT_EQ(snapshot->scanAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U).edges,
			  (std::vector<LogicalGraphEdge>{{2U, 1U, 2U, "RETAINED_EDGE"}}));
	EXPECT_EQ(snapshot->scanAdjacentEdges(2U, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U).edges,
			  (std::vector<LogicalGraphEdge>{{2U, 1U, 2U, "RETAINED_EDGE"}}));
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableProcessCrashOrdinaryEdgeTest,
	   KillBeforeDetachDeletePublicationRetainsTheNodeAndItsIncidentAdjacency) {
	createDatabaseWithEdge();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ORDINARY_DETACH_DELETE_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({2U, 1U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNode(1U).node_id, 1U);
	EXPECT_EQ(snapshot->readEdge(1U), LogicalGraphEdge(1U, 1U, 2U, "CRASH_EDGE"));
	EXPECT_EQ(snapshot->scanAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 8U).edges,
			  (std::vector<LogicalGraphEdge>{{1U, 1U, 2U, "CRASH_EDGE"}}));
	snapshot.reset();
	reopened.deleteLogicalNodes({{1U, true, 1U}});
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 0U}));
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableProcessCrashOrdinaryEdgeTest,
	   KillAfterDetachDeletePublicationRemovesTheNodeAndBothAdjacencyDirections) {
	createDatabaseWithEdge();
	CrashChild child(databasePath, markerPath, CRASH_CHILD_ORDINARY_DETACH_DELETE_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	RelocatableStorageBackendOptions reopenOptions;
	reopenOptions.data_path = databasePath;
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	reopened.open();
	EXPECT_EQ(reopened.readLogicalGraphCardinalityStatistics(),
			  std::optional<LogicalGraphCardinalityStatistics>({1U, 0U}));
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->readNode(1U)), FormatException);
	EXPECT_THROW(static_cast<void>(snapshot->readEdge(1U)), FormatException);
	EXPECT_TRUE(
			snapshot->scanAdjacentEdges(2U, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 8U).edges.empty());
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}
