/**
 * @file test_RelocatableProcessCrash_Ddl.cpp
 * @brief Real process-crash recovery for public native schema publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 */

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "storage/relocatable/format/RelocatableProcessCrashSupport.hpp"

using namespace graph::storage;

namespace {

	constexpr std::string_view CRASH_CHILD_DDL_SCALAR_BEFORE_PUBLICATION = "ddl-scalar-before-publication";
	constexpr std::string_view CRASH_CHILD_DDL_SCALAR_AFTER_PUBLICATION = "ddl-scalar-after-publication";
	constexpr std::string_view CRASH_CHILD_DDL_UNIQUE_BEFORE_PUBLICATION = "ddl-unique-before-publication";
	constexpr std::string_view CRASH_CHILD_DDL_UNIQUE_AFTER_PUBLICATION = "ddl-unique-after-publication";
	constexpr std::string_view CRASH_CHILD_DDL_VECTOR_BEFORE_PUBLICATION = "ddl-vector-before-publication";
	constexpr std::string_view CRASH_CHILD_DDL_VECTOR_AFTER_PUBLICATION = "ddl-vector-after-publication";

	enum class DdlKind { SCALAR, UNIQUE, VECTOR };

	using graph::test::relocatable_crash::CrashChild;
	using graph::test::relocatable_crash::waitToBeKilled;
	using graph::test::relocatable_crash::writeFailureMarker;
	using graph::test::relocatable_crash::writeMarker;

	[[nodiscard]] RelocatableStorageBackendOptions backendOptions(const std::filesystem::path &databasePath,
																			 const OpenMode openMode) {
		RelocatableStorageBackendOptions options;
		options.data_path = databasePath;
		options.open_mode = openMode;
		options.maximum_index_definitions = 32U;
		options.maximum_online_index_entries = 1024U;
		options.maximum_online_index_pages = 128U;
		options.online_index_sort_run_bytes = 4096U;
		options.maximum_online_index_sort_runs = 8U;
		return options;
	}

	[[nodiscard]] graph::PropertyValue vectorValue() {
		return graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(1.0), graph::PropertyValue(-2.0)});
	}

	void createDdl(RelocatableStorageBackend &backend, const DdlKind kind) {
		switch (kind) {
		case DdlKind::SCALAR:
			static_cast<void>(backend.createLogicalNodeIndex({"person_score", "Person", {"score"}}));
			return;
		case DdlKind::UNIQUE:
			static_cast<void>(backend.createLogicalConstraint(
					{"user_email_unique", LogicalGraphConstraintEntityKind::LGCEK_NODE,
					 LogicalGraphConstraintKind::LGCK_UNIQUE, "User", {"email"},
					 LogicalGraphConstraintPropertyType::LGCPT_NONE}));
			return;
		case DdlKind::VECTOR:
			static_cast<void>(backend.createLogicalNodeVectorIndex({"embedding_value", "Embedding", "value", 2U, "l2"}));
			return;
		}
		throw std::logic_error("unknown public DDL crash-test kind");
	}

	[[nodiscard]] bool isBeforePublication(const std::string_view mode) {
		return mode == CRASH_CHILD_DDL_SCALAR_BEFORE_PUBLICATION || mode == CRASH_CHILD_DDL_UNIQUE_BEFORE_PUBLICATION ||
			   mode == CRASH_CHILD_DDL_VECTOR_BEFORE_PUBLICATION;
	}

	[[nodiscard]] std::optional<DdlKind> ddlKindForMode(const std::string_view mode) {
		if (mode == CRASH_CHILD_DDL_SCALAR_BEFORE_PUBLICATION || mode == CRASH_CHILD_DDL_SCALAR_AFTER_PUBLICATION) {
			return DdlKind::SCALAR;
		}
		if (mode == CRASH_CHILD_DDL_UNIQUE_BEFORE_PUBLICATION || mode == CRASH_CHILD_DDL_UNIQUE_AFTER_PUBLICATION) {
			return DdlKind::UNIQUE;
		}
		if (mode == CRASH_CHILD_DDL_VECTOR_BEFORE_PUBLICATION || mode == CRASH_CHILD_DDL_VECTOR_AFTER_PUBLICATION) {
			return DdlKind::VECTOR;
		}
		return std::nullopt;
	}

} // namespace

namespace graph::test {

	int runRelocatableDdlCrashChild(const std::string_view mode, const std::filesystem::path &databasePath,
													 const std::filesystem::path &markerPath) {
		const std::optional<DdlKind> kind = ddlKindForMode(mode);
		if (!kind.has_value()) {
			return -1;
		}
		try {
			PhysicalPageRef priorIndexRootDirectory;
			{
				auto data = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
				if (!data->is_open()) return 108;
				auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
				priorIndexRootDirectory = SuperblockFile(io).recover().index_root_directory_root;
			}

			RelocatableStorageBackend backend(backendOptions(databasePath, OpenMode::OPEN_EXISTING_FILE));
			backend.open();
			if (isBeforePublication(mode)) {
				std::atomic<bool> observedCandidate{false};
				StorageIO::ScopedNativeWriteHookForTesting pauseAfterDurableCandidate(
						[&observedCandidate, &markerPath](const file_handle_t fd, const void *bytes, const size_t byteCount,
																					 const pwrite_off_t offset) {
							if (byteCount == RSS_PAGE_SIZE && !observedCandidate.exchange(true, std::memory_order_relaxed)) {
								const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
								if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) return written;
								writeMarker(markerPath);
								waitToBeKilled();
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				createDdl(backend, *kind);
				return 102;
			}

			std::atomic<bool> observedPublication{false};
			StorageIO::ScopedNativeWriteHookForTesting pauseAfterDdlPublication(
					[priorIndexRootDirectory, &observedPublication, &markerPath](const file_handle_t fd, const void *bytes,
																						const size_t byteCount, const pwrite_off_t offset) {
						if (byteCount == RSS_SUPERBLOCK_BYTES &&
							(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U)) &&
							!observedPublication.load(std::memory_order_relaxed)) {
							try {
								const Superblock candidate = SuperblockCodec::decode(
										std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
								if (candidate.index_root_directory_root != priorIndexRootDirectory) {
									observedPublication.store(true, std::memory_order_relaxed);
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount) || portable_fsync(fd) != 0) return written;
									writeMarker(markerPath);
									waitToBeKilled();
								}
							} catch (const FormatException &) {
								// Ignore unrelated control writes while locating the DDL publication.
							}
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			createDdl(backend, *kind);
			return 103;
		} catch (const std::exception &error) {
			writeFailureMarker(markerPath, error.what());
			return 101;
		}
	}

} // namespace graph::test

class RelocatableProcessCrashDdlTest : public ::testing::Test {
protected:
	std::filesystem::path databasePath;
	std::filesystem::path markerPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		databasePath = std::filesystem::temp_directory_path() / ("relocatable_ddl_" + to_string(uuid) + ".db");
		markerPath = databasePath.string() + ".marker";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(databasePath, error);
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
	}

	void createDatabase(const DdlKind kind) const {
		auto created = RelocatableStorageBackend::initializeNew(backendOptions(databasePath, OpenMode::OPEN_CREATE_NEW_FILE));
		ASSERT_NE(created, nullptr);
		created->open();
		switch (kind) {
		case DdlKind::SCALAR:
			ASSERT_EQ(created->createLogicalNodes({{{"Person"}, {{"score", graph::PropertyValue(int64_t{17})}}}}),
					  (std::vector<uint64_t>{1U}));
			break;
		case DdlKind::UNIQUE:
			ASSERT_EQ(created->createLogicalNodes(
						  {{{"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}}}),
					  (std::vector<uint64_t>{1U}));
			break;
		case DdlKind::VECTOR:
			ASSERT_EQ(created->createLogicalNodes({{{"Embedding"}, {{"value", vectorValue()}}}}),
					  (std::vector<uint64_t>{1U}));
			break;
		}
		created->close();
	}

	void verifyPublishedDdl(RelocatableStorageBackend &backend, const DdlKind kind) const {
		switch (kind) {
		case DdlKind::SCALAR:
			EXPECT_EQ(backend.listLogicalIndexes(8U).size(), 1U);
			break;
		case DdlKind::UNIQUE:
			EXPECT_EQ(backend.listLogicalConstraints(8U).size(), 1U);
			EXPECT_THROW(static_cast<void>(backend.createLogicalNodes(
						 {{{"User"}, {{"email", graph::PropertyValue(std::string("ada@example.test"))}}}})),
					 std::runtime_error);
			break;
		case DdlKind::VECTOR: {
			ASSERT_EQ(backend.listLogicalIndexes(8U).size(), 1U);
			auto snapshot = backend.openLogicalGraphVectorSearchSnapshot();
			ASSERT_NE(snapshot, nullptr);
			LogicalGraphVectorSearchRequest request;
			request.index_name = "embedding_value";
			request.query_vector = {1.0F, -2.0F};
			request.top_k = 1U;
			request.mode = LogicalGraphVectorSearchMode::LGVSM_EXACT;
			request.maximum_mapping_entries = 4U;
			request.maximum_raw_vector_reads = 4U;
			const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(request);
			ASSERT_EQ(result.hits.size(), 1U);
			EXPECT_EQ(result.hits.front().node_id, 1U);
			break;
		}
		}
		EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	}

	void expectCrashBoundary(const DdlKind kind, const std::string_view mode, const bool expectOldGeneration) {
		createDatabase(kind);
		CrashChild child(databasePath, markerPath, mode);
		ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
		child.terminate();

		RelocatableStorageBackend reopened(backendOptions(databasePath, OpenMode::OPEN_EXISTING_FILE));
		reopened.open();
		if (expectOldGeneration) {
			if (kind == DdlKind::UNIQUE) {
				EXPECT_TRUE(reopened.listLogicalConstraints(8U).empty());
			} else {
				EXPECT_TRUE(reopened.listLogicalIndexes(8U).empty());
			}
			createDdl(reopened, kind);
		}
		verifyPublishedDdl(reopened, kind);
		reopened.close();
	}
};

TEST_F(RelocatableProcessCrashDdlTest, KillAfterScalarIndexDdlCandidateRetainsOldSchemaAndCanRetry) {
	expectCrashBoundary(DdlKind::SCALAR, CRASH_CHILD_DDL_SCALAR_BEFORE_PUBLICATION, true);
}

TEST_F(RelocatableProcessCrashDdlTest, KillAfterScalarIndexDdlPublicationRecoversCompleteSchema) {
	expectCrashBoundary(DdlKind::SCALAR, CRASH_CHILD_DDL_SCALAR_AFTER_PUBLICATION, false);
}

TEST_F(RelocatableProcessCrashDdlTest, KillAfterUniqueConstraintDdlCandidateRetainsOldSchemaAndCanRetry) {
	expectCrashBoundary(DdlKind::UNIQUE, CRASH_CHILD_DDL_UNIQUE_BEFORE_PUBLICATION, true);
}

TEST_F(RelocatableProcessCrashDdlTest, KillAfterUniqueConstraintDdlPublicationRecoversCompleteSupportTree) {
	expectCrashBoundary(DdlKind::UNIQUE, CRASH_CHILD_DDL_UNIQUE_AFTER_PUBLICATION, false);
}

TEST_F(RelocatableProcessCrashDdlTest, KillAfterVectorIndexDdlCandidateRetainsOldSchemaAndCanRetry) {
	expectCrashBoundary(DdlKind::VECTOR, CRASH_CHILD_DDL_VECTOR_BEFORE_PUBLICATION, true);
}

TEST_F(RelocatableProcessCrashDdlTest, KillAfterVectorIndexDdlPublicationRecoversCompleteManifestAndSearchRoute) {
	expectCrashBoundary(DdlKind::VECTOR, CRASH_CHILD_DDL_VECTOR_AFTER_PUBLICATION, false);
}
