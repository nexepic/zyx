#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "storage/relocatable/format/RelocatableProcessCrashSupport.hpp"
#include "zyx/zyx_driver_abi.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace graph::storage;

namespace {

	std::string uniqueIngestDbPath() {
		static std::atomic<uint64_t> counter{0};
		const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
		return (fs::temp_directory_path() / ("zyx_driver_abi_ingest_" + std::to_string(now) + "_" +
											 std::to_string(counter.fetch_add(1, std::memory_order_relaxed))))
				.string();
	}

	Superblock initialRelocatableSuperblock() {
		Superblock superblock;
		superblock.database_id = DatabaseId(0x1A2B3C4D5E6F7081ULL);
		superblock.generation = Generation(1U);
		superblock.checkpoint_lsn = LogSequenceNumber(1U);
		return superblock;
	}

	void materializeEmptyRelocatableDatabase(const std::string &databasePath) {
		auto data = std::make_shared<std::fstream>(databasePath,
												   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(data->is_open());
		const file_handle_t dataWrite = portable_open_rw(databasePath.c_str());
		const file_handle_t dataRead = portable_open(databasePath.c_str(), O_RDONLY);
		ASSERT_NE(dataWrite, INVALID_FILE_HANDLE);
		ASSERT_NE(dataRead, INVALID_FILE_HANDLE);

		auto io = std::make_shared<StorageIO>(data, dataWrite, dataRead);
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialRelocatableSuperblock());
		metadata.reset();
		io->sync();
		io.reset();
		static_cast<void>(portable_close_rw(dataWrite));
		static_cast<void>(portable_close(dataRead));
		data->close();
	}

	Superblock recoverPublishedSuperblock(const std::filesystem::path &databasePath) {
		auto data = std::make_shared<std::fstream>(databasePath, std::ios::binary | std::ios::in | std::ios::out);
		if (!data->is_open()) {
			throw std::runtime_error("crash-test child cannot open database Superblocks");
		}
		const auto databasePathString = databasePath.string();
		const file_handle_t dataWrite = portable_open_rw(databasePathString.c_str());
		const file_handle_t dataRead = portable_open(databasePathString.c_str(), O_RDONLY);
		if (dataWrite == INVALID_FILE_HANDLE || dataRead == INVALID_FILE_HANDLE) {
			if (dataWrite != INVALID_FILE_HANDLE) {
				static_cast<void>(portable_close_rw(dataWrite));
			}
			if (dataRead != INVALID_FILE_HANDLE) {
				static_cast<void>(portable_close(dataRead));
			}
			throw std::runtime_error("crash-test child cannot open Superblock descriptors");
		}
		try {
			auto io = std::make_shared<StorageIO>(data, dataWrite, dataRead);
			const Superblock superblock = SuperblockFile(io).recover();
			io.reset();
			static_cast<void>(portable_close_rw(dataWrite));
			static_cast<void>(portable_close(dataRead));
			data->close();
			return superblock;
		} catch (...) {
			static_cast<void>(portable_close_rw(dataWrite));
			static_cast<void>(portable_close(dataRead));
			data->close();
			throw;
		}
	}

	ArrowSchema scalarSchema(const char *format, const char *name) {
		return ArrowSchema{format, name, nullptr, ARROW_FLAG_NULLABLE, 0, nullptr, nullptr, nullptr, nullptr};
	}

	struct IngestEdgeSchema {
		std::array<ArrowSchema, 2> propertyFields{scalarSchema("l", "since"), scalarSchema("u", "name")};
		std::array<ArrowSchema *, 2> propertyFieldPointers{&propertyFields[0], &propertyFields[1]};
		ArrowSchema source = scalarSchema("l", "source_id");
		ArrowSchema target = scalarSchema("l", "target_id");
		ArrowSchema properties{"+s",
							   "properties",
							   nullptr,
							   ARROW_FLAG_NULLABLE,
							   static_cast<int64_t>(propertyFieldPointers.size()),
							   propertyFieldPointers.data(),
							   nullptr,
							   nullptr,
							   nullptr};
		std::array<ArrowSchema *, 3> rootFieldPointers{&source, &target, &properties};
		ArrowSchema root{
				"+s",	 "edges", nullptr, 0, static_cast<int64_t>(rootFieldPointers.size()), rootFieldPointers.data(),
				nullptr, nullptr, nullptr};
	};

	struct IngestEdgeBatch {
		explicit IngestEdgeBatch(std::vector<int64_t> sources, std::vector<int64_t> targets,
								 std::vector<int64_t> sinceValues, std::vector<std::string> names) :
			sourceValues(std::move(sources)), targetValues(std::move(targets)), sinceValues(std::move(sinceValues)) {
			nameOffsets.reserve(names.size() + 1);
			nameOffsets.push_back(0);
			for (const auto &name: names) {
				nameBytes += name;
				nameOffsets.push_back(static_cast<int32_t>(nameBytes.size()));
			}
			initialize();
		}

		void initialize() {
			const int64_t rows = static_cast<int64_t>(sourceValues.size());
			sourceBuffers = {nullptr, sourceValues.data()};
			targetBuffers = {nullptr, targetValues.data()};
			sinceBuffers = {nullptr, sinceValues.data()};
			nameBuffers = {nullptr, nameOffsets.data(), nameBytes.data()};
			propertyStructBuffers = {nullptr};
			rootStructBuffers = {nullptr};

			source = ArrowArray{rows, 0, 0, 2, 0, sourceBuffers.data(), nullptr, nullptr, nullptr, nullptr};
			target = ArrowArray{rows, 0, 0, 2, 0, targetBuffers.data(), nullptr, nullptr, nullptr, nullptr};
			since = ArrowArray{rows, 0, 0, 2, 0, sinceBuffers.data(), nullptr, nullptr, nullptr, nullptr};
			name = ArrowArray{rows, 0, 0, 3, 0, nameBuffers.data(), nullptr, nullptr, nullptr, nullptr};
			propertyChildren = {&since, &name};
			properties = ArrowArray{
					rows, 0, 0, 1, 2, propertyStructBuffers.data(), propertyChildren.data(), nullptr, nullptr, nullptr};
			rootChildren = {&source, &target, &properties};
			root = ArrowArray{rows,	   0,		0,		1, 3, rootStructBuffers.data(), rootChildren.data(),
							  nullptr, nullptr, nullptr};
		}

		std::vector<int64_t> sourceValues;
		std::vector<int64_t> targetValues;
		std::vector<int64_t> sinceValues;
		std::vector<int32_t> nameOffsets;
		std::string nameBytes;
		std::array<const void *, 2> sourceBuffers{};
		std::array<const void *, 2> targetBuffers{};
		std::array<const void *, 2> sinceBuffers{};
		std::array<const void *, 3> nameBuffers{};
		std::array<const void *, 1> propertyStructBuffers{};
		std::array<const void *, 1> rootStructBuffers{};
		ArrowArray source{};
		ArrowArray target{};
		ArrowArray since{};
		ArrowArray name{};
		std::array<ArrowArray *, 2> propertyChildren{};
		ArrowArray properties{};
		std::array<ArrowArray *, 3> rootChildren{};
		ArrowArray root{};
	};

} // namespace

class DriverAbiIngestTest : public ::testing::Test {
protected:
	std::string dbPath;
	zyx_driver_db_t *db = nullptr;
	zyx_driver_error_t *error = nullptr;

	void SetUp() override {
		dbPath = uniqueIngestDbPath();
		cleanupFiles();
		ASSERT_EQ(zyx_driver_db_open(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
		ASSERT_NE(db, nullptr);
	}

	void TearDown() override {
		if (db != nullptr) {
			EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
			db = nullptr;
		}
		freeError();
		cleanupFiles();
	}

	void freeError() {
		zyx_driver_error_free(error);
		error = nullptr;
	}

	void cleanupFiles() {
		std::error_code ec;
		fs::remove_all(dbPath, ec);
	}

	std::pair<int64_t, int64_t> createEndpoints() {
		int64_t source = 0;
		int64_t target = 0;
		EXPECT_EQ(zyx_driver_db_create_node(db, "IngestEndpoint", nullptr, &source, &error), ZYX_DRIVER_OK);
		EXPECT_EQ(zyx_driver_db_create_node(db, "IngestEndpoint", nullptr, &target, &error), ZYX_DRIVER_OK);
		return {source, target};
	}

	int64_t countEdges(const char *type) { return countEdges(db, type); }

	int64_t countEdges(zyx_driver_db_t *database, const char *type) {
		zyx_driver_result_t *result = nullptr;
		const std::string query = "MATCH ()-[e:" + std::string(type) + "]->() RETURN count(e)";
		const zyx_driver_status_t status = zyx_driver_db_execute(database, query.c_str(), nullptr, &result, &error);
		if (status != ZYX_DRIVER_OK) {
			const char *message = zyx_driver_error_message(error);
			ADD_FAILURE() << "count edge query failed: " << (message != nullptr ? message : "unknown error");
			return -1;
		}
		int64_t count = -1;
		if (result != nullptr && zyx_driver_result_next(result, &error) == ZYX_DRIVER_ROW) {
			EXPECT_EQ(zyx_driver_result_get_int64(result, 0, &count, &error), ZYX_DRIVER_OK);
		}
		zyx_driver_result_free(result);
		return count;
	}

	template<typename Mutator>
	void expectBatchError(int64_t source, int64_t target, Mutator &&mutator, zyx_driver_status_t expectedStatus,
						  const char *expectedField = nullptr) {
		IngestEdgeSchema schema;
		IngestEdgeBatch batch({source}, {target}, {1}, {"value"});
		mutator(batch);
		zyx_driver_ingest_t *ingest = nullptr;
		zyx_driver_edge_ingestor_t *ingestor = nullptr;
		ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
		ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "INVALID_BATCH_REL", &schema.root, &ingestor, &error),
				  ZYX_DRIVER_OK);
		EXPECT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), expectedStatus);
		ASSERT_NE(error, nullptr);
		if (expectedField != nullptr) {
			EXPECT_STREQ(zyx_driver_error_field_path(error), expectedField);
		}
		freeError();
		EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
		EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
		EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	}
};

class RelocatableDriverAbiIngestTest : public DriverAbiIngestTest {
protected:
	void SetUp() override {
		dbPath = uniqueIngestDbPath();
		cleanupFiles();
		materializeEmptyRelocatableDatabase(dbPath);
		ASSERT_FALSE(fs::exists(dbPath + ".wal"));
		ASSERT_EQ(zyx_driver_db_open(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
		ASSERT_NE(db, nullptr);
	}
};

class DISABLED_RelocatableDriverAbiIngestScaleTest : public RelocatableDriverAbiIngestTest {};

namespace graph::test {

	int runDriverAbiIngestProcessCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
											const std::filesystem::path &markerPath) {
		const bool crashBeforePublication = mode == "driver-abi-ingest-before-publication";
		const bool crashAfterPublication = mode == "driver-abi-ingest-after-publication";
		const bool crashBeforeRebasedPublication = mode == "driver-abi-ingest-before-rebased-publication";
		const bool crashAfterRebasedPublication = mode == "driver-abi-ingest-after-rebased-publication";
		const bool completeRebasedPublication = mode == "driver-abi-ingest-rebased-publication";
		if (!crashBeforePublication && !crashAfterPublication && !crashBeforeRebasedPublication &&
			!crashAfterRebasedPublication && !completeRebasedPublication) {
			return -1;
		}

		zyx_driver_db_t *childDb = nullptr;
		zyx_driver_ingest_t *ingest = nullptr;
		zyx_driver_edge_ingestor_t *ingestor = nullptr;
		zyx_driver_error_t *childError = nullptr;
		const auto releaseError = [&childError] {
			zyx_driver_error_free(childError);
			childError = nullptr;
		};
		try {
			const auto requireOk = [&childError, &releaseError](zyx_driver_status_t status,
																std::string_view operation) {
				if (status == ZYX_DRIVER_OK) {
					return;
				}
				const char *const message = zyx_driver_error_message(childError);
				const std::string reason = std::string(operation) +
										   " failed: " + (message != nullptr ? message : "unknown Driver ABI error");
				releaseError();
				throw std::runtime_error(reason);
			};

			requireOk(zyx_driver_db_open_if_exists(databasePath.string().c_str(), &childDb, &childError),
					  "open database");
			IngestEdgeSchema schema;
			const bool rebasedStaleHandle =
					crashBeforeRebasedPublication || crashAfterRebasedPublication || completeRebasedPublication;
			const size_t stagedEdgeCount = rebasedStaleHandle ? 1024U : 1U;
			IngestEdgeBatch batch(
					std::vector<int64_t>(stagedEdgeCount, 1), std::vector<int64_t>(stagedEdgeCount, 2),
					std::vector<int64_t>(stagedEdgeCount, 2026),
					std::vector<std::string>(stagedEdgeCount, rebasedStaleHandle ? std::string(192U, 's') : "crash"));
			requireOk(zyx_driver_ingest_begin(childDb, &ingest, &childError), "begin ingest");
			const char *const relationship = completeRebasedPublication
													 ? "CROSS_PROCESS_REBASE_REL"
													 : (rebasedStaleHandle ? "CRASH_REBASE_REL" : "CRASH_REL");
			requireOk(zyx_driver_ingest_prepare_edges(ingest, relationship, &schema.root, &ingestor, &childError),
					  "prepare edge ingest");
			requireOk(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &childError), "write edge batch");

			if (rebasedStaleHandle) {
				graph::test::relocatable_crash::writeMarker(markerPath);
				const std::filesystem::path resumeMarker = markerPath.string() + ".resume";
				if (!graph::test::relocatable_crash::waitForMarker(resumeMarker)) {
					throw std::runtime_error("parent did not publish the external Edge before crash-test timeout");
				}
				if (completeRebasedPublication) {
					requireOk(zyx_driver_ingest_commit(ingest, &childError), "commit rebased ingest");
					requireOk(zyx_driver_edge_ingestor_close(ingestor, &childError), "close edge ingestor");
					ingestor = nullptr;
					requireOk(zyx_driver_ingest_close(ingest, &childError), "close ingest");
					ingest = nullptr;
					requireOk(zyx_driver_db_close(childDb, &childError), "close database");
					childDb = nullptr;
					graph::test::relocatable_crash::writeMarker(markerPath.string() + ".complete");
					return 0;
				}
				const std::filesystem::path crashMarker = markerPath.string() + ".crash";
				const Superblock externalPublication = recoverPublishedSuperblock(databasePath);
				if (crashAfterRebasedPublication) {
					std::atomic<bool> observedPublication{false};
					StorageIO::ScopedNativeWriteHookForTesting pauseAfterRebasedGraphPublication(
							[&observedPublication, &crashMarker, externalPublication](file_handle_t fd, const void *bytes,
																							  size_t byteCount, pwrite_off_t offset) {
								if (byteCount == RSS_PAGE_SIZE &&
									(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U)) &&
									!observedPublication.load(std::memory_order_relaxed)) {
									try {
										const Superblock candidate = SuperblockCodec::decode(
												std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
										if (candidate.generation > externalPublication.generation &&
											candidate.edge_entity_directory_root != externalPublication.edge_entity_directory_root) {
											observedPublication.store(true, std::memory_order_relaxed);
											const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
											if (written != static_cast<pwrite_ssize_t>(byteCount)) {
												return written;
											}
											graph::test::relocatable_crash::writeMarker(crashMarker);
											graph::test::relocatable_crash::waitToBeKilled();
										}
									} catch (const FormatException &) {
										// Only the new authenticated graph publication arms this crash point.
									}
								}
								return portable_pwrite(fd, bytes, byteCount, offset);
							});
					requireOk(zyx_driver_ingest_commit(ingest, &childError), "commit rebased ingest");
					return 102;
				}
				std::atomic<bool> observedPublicEntity{false};
				StorageIO::ScopedNativeWriteHookForTesting pauseBeforeRebasedGraphPublication(
						[&observedPublicEntity, &crashMarker](file_handle_t fd, const void *bytes, size_t byteCount,
															  pwrite_off_t offset) {
							if (byteCount == RSS_PAGE_SIZE && !observedPublicEntity.load(std::memory_order_relaxed)) {
								try {
									const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes),
																				 byteCount);
									if (PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES)).kind ==
										PageOwnerKind::POK_ENTITY) {
										observedPublicEntity.store(true, std::memory_order_relaxed);
										graph::test::relocatable_crash::writeMarker(crashMarker);
										graph::test::relocatable_crash::waitToBeKilled();
									}
								} catch (const FormatException &) {
									// Only an authenticated public Entity page arms this crash point.
								}
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				requireOk(zyx_driver_ingest_commit(ingest, &childError), "commit rebased ingest");
				return 102;
			}

			if (crashBeforePublication) {
				std::atomic<bool> observedPublicEntity{false};
				StorageIO::ScopedNativeWriteHookForTesting pauseBeforeGraphPublication(
						[&observedPublicEntity, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
															 pwrite_off_t offset) {
							if (byteCount == RSS_PAGE_SIZE && !observedPublicEntity.load(std::memory_order_relaxed)) {
								try {
									const auto page = std::span<const std::byte>(static_cast<const std::byte *>(bytes),
																				 byteCount);
									if (PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES)).kind ==
										PageOwnerKind::POK_ENTITY) {
										observedPublicEntity.store(true, std::memory_order_relaxed);
										graph::test::relocatable_crash::writeMarker(markerPath);
										graph::test::relocatable_crash::waitToBeKilled();
									}
								} catch (const FormatException &) {
									// Only an authenticated public Entity page arms this crash point.
								}
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				requireOk(zyx_driver_ingest_commit(ingest, &childError), "commit ingest");
				return 102;
			}

			std::atomic<bool> observedPublication{false};
			StorageIO::ScopedNativeWriteHookForTesting pauseAfterGraphPublication(
					[&observedPublication, &markerPath](file_handle_t fd, const void *bytes, size_t byteCount,
														pwrite_off_t offset) {
						if (byteCount == RSS_PAGE_SIZE &&
							(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U)) &&
							!observedPublication.load(std::memory_order_relaxed)) {
							try {
								const Superblock candidate = SuperblockCodec::decode(
										std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
								if (candidate.edge_entity_directory_root.isValid()) {
									observedPublication.store(true, std::memory_order_relaxed);
									const pwrite_ssize_t written = portable_pwrite(fd, bytes, byteCount, offset);
									if (written != static_cast<pwrite_ssize_t>(byteCount)) {
										return written;
									}
									graph::test::relocatable_crash::writeMarker(markerPath);
									graph::test::relocatable_crash::waitToBeKilled();
								}
							} catch (const FormatException &) {
								// Ignore non-Superblock control writes while locating the final route.
							}
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			requireOk(zyx_driver_ingest_commit(ingest, &childError), "commit ingest");
			return 102;
		} catch (const std::exception &exception) {
			releaseError();
			if (ingestor != nullptr) {
				static_cast<void>(zyx_driver_edge_ingestor_close(ingestor, nullptr));
			}
			if (ingest != nullptr) {
				static_cast<void>(zyx_driver_ingest_close(ingest, nullptr));
			}
			if (childDb != nullptr) {
				static_cast<void>(zyx_driver_db_close(childDb, nullptr));
			}
			graph::test::relocatable_crash::writeFailureMarker(markerPath, exception.what());
			return 101;
		}
	}

} // namespace graph::test

TEST_F(DriverAbiIngestTest, ArrowEdgeIngestCommitsMultipleBatchesWithContiguousIds) {
	const auto [source, target] = createEndpoints();
	IngestEdgeSchema schema;
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ARROW_REL", &schema.root, &ingestor, &error), ZYX_DRIVER_OK);

	IngestEdgeBatch first({source, target}, {target, source}, {2025, 2026}, {"first", "second"});
	zyx_driver_id_range_t firstIds{};
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &first.root, &firstIds, &error), ZYX_DRIVER_OK);
	EXPECT_GT(firstIds.first_id, 0);
	EXPECT_EQ(firstIds.count, 2);

	IngestEdgeBatch second({source}, {target}, {2027}, {"third"});
	zyx_driver_id_range_t secondIds{};
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &second.root, &secondIds, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(secondIds.first_id, firstIds.first_id + firstIds.count);
	EXPECT_EQ(secondIds.count, 1);

	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	EXPECT_EQ(countEdges("ARROW_REL"), 3);

	zyx_driver_result_t *result = nullptr;
	ASSERT_EQ(zyx_driver_db_execute(db, "MATCH ()-[e:ARROW_REL]->() RETURN e.since, e.name ORDER BY e.since", nullptr,
									&result, &error),
			  ZYX_DRIVER_OK);
	for (int64_t expected = 2025; expected <= 2027; ++expected) {
		ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
		int64_t since = 0;
		ASSERT_EQ(zyx_driver_result_get_int64(result, 0, &since, &error), ZYX_DRIVER_OK);
		EXPECT_EQ(since, expected);
	}
	zyx_driver_result_free(result);
}

TEST_F(DriverAbiIngestTest, RollbackDiscardsArrowBatch) {
	const auto [source, target] = createEndpoints();
	IngestEdgeSchema schema;
	IngestEdgeBatch batch({source}, {target}, {2026}, {"rolled-back"});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ROLLBACK_REL", &schema.root, &ingestor, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("ROLLBACK_REL"), 0);
}

TEST_F(RelocatableDriverAbiIngestTest, ArrowEdgeIngestStagesRowsUntilCommitAndDropsRolledBackRows) {
	const auto [source, target] = createEndpoints();
	IngestEdgeSchema schema;
	zyx_driver_db_t *reader = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &reader, &error), ZYX_DRIVER_OK);
	ASSERT_NE(reader, nullptr);

	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	IngestEdgeBatch committedBatch({source}, {target}, {2026}, {"committed"});
	zyx_driver_id_range_t committedIds{};
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "RSS_INGEST_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &committedBatch.root, &committedIds, &error), ZYX_DRIVER_OK);
	EXPECT_GT(committedIds.first_id, 0);
	EXPECT_EQ(committedIds.count, 1);
	EXPECT_EQ(countEdges(reader, "RSS_INGEST_REL"), 0);

	const zyx_driver_status_t commitStatus = zyx_driver_ingest_commit(ingest, &error);
	ASSERT_EQ(commitStatus, ZYX_DRIVER_OK) << zyx_driver_error_message(error);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	EXPECT_EQ(zyx_driver_db_close(reader, &error), ZYX_DRIVER_OK);
	reader = nullptr;

	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &reader, &error), ZYX_DRIVER_OK);
	ASSERT_NE(reader, nullptr);
	EXPECT_EQ(countEdges(reader, "RSS_INGEST_REL"), 1);

	IngestEdgeBatch rolledBackBatch({target}, {source}, {2027}, {"rolled-back"});
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "RSS_INGEST_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &rolledBackBatch.root, nullptr, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges(reader, "RSS_INGEST_REL"), 1);
	ASSERT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	EXPECT_EQ(countEdges(reader, "RSS_INGEST_REL"), 1);
	EXPECT_EQ(zyx_driver_db_close(reader, &error), ZYX_DRIVER_OK);
}

TEST_F(RelocatableDriverAbiIngestTest, StreamedIngestRebasesOverAConcurrentAbiEdgePublication) {
	const auto [source, target] = createEndpoints();
	zyx_driver_db_t *concurrentWriter = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &concurrentWriter, &error), ZYX_DRIVER_OK);
	ASSERT_NE(concurrentWriter, nullptr);

	IngestEdgeSchema schema;
	// Exceed one sealed scratch run so the rebase covers durable private staging,
	// not only an in-memory Arrow batch.
	constexpr size_t stagedEdgeCount = 1024U;
	IngestEdgeBatch batch(std::vector<int64_t>(stagedEdgeCount, source), std::vector<int64_t>(stagedEdgeCount, target),
					  std::vector<int64_t>(stagedEdgeCount, 2026),
					  std::vector<std::string>(stagedEdgeCount, std::string(192U, 's')));
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_REBASED_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);

	int64_t concurrentEdge = 0;
	ASSERT_EQ(zyx_driver_db_create_edge(concurrentWriter, source, target, "ABI_CONCURRENT_REL", nullptr,
								  &concurrentEdge, &error),
			  ZYX_DRIVER_OK);
	EXPECT_GT(concurrentEdge, 0);
	ASSERT_EQ(zyx_driver_db_close(concurrentWriter, &error), ZYX_DRIVER_OK);
	concurrentWriter = nullptr;

	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("ABI_REBASED_REL"), static_cast<int64_t>(stagedEdgeCount));
	EXPECT_EQ(countEdges("ABI_CONCURRENT_REL"), 1);
	EXPECT_FALSE(fs::exists(dbPath + ".wal"));
	EXPECT_FALSE(fs::exists(dbPath + "-wal"));
}

TEST_F(DISABLED_RelocatableDriverAbiIngestScaleTest,
	StreamedIngestRebaseFailurePreservesExternalPublicationAndAllowsRetry) {
	const auto [source, target] = createEndpoints();
	zyx_driver_db_t *concurrentWriter = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &concurrentWriter, &error), ZYX_DRIVER_OK);
	ASSERT_NE(concurrentWriter, nullptr);

	IngestEdgeSchema schema;
	constexpr size_t stagedEdgeCount = 1024U;
	IngestEdgeBatch stagedBatch(std::vector<int64_t>(stagedEdgeCount, source),
								std::vector<int64_t>(stagedEdgeCount, target),
								std::vector<int64_t>(stagedEdgeCount, 2026),
								std::vector<std::string>(stagedEdgeCount, std::string(192U, 's')));
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_REBASE_RETRY_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &stagedBatch.root, nullptr, &error), ZYX_DRIVER_OK);

	int64_t concurrentEdge = 0;
	ASSERT_EQ(zyx_driver_db_create_edge(concurrentWriter, source, target, "ABI_REBASE_EXTERNAL_REL", nullptr,
										&concurrentEdge, &error),
			  ZYX_DRIVER_OK);
	EXPECT_GT(concurrentEdge, 0);
	ASSERT_EQ(zyx_driver_db_close(concurrentWriter, &error), ZYX_DRIVER_OK);
	concurrentWriter = nullptr;

	std::atomic<bool> sawPublicEntityPage{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observePublicEntityPage(
				[&sawPublicEntityPage](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							if (PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES)).kind ==
								PageOwnerKind::POK_ENTITY) {
								sawPublicEntityPage.store(true, std::memory_order_relaxed);
							}
						} catch (const FormatException &) {
							// Only an authenticated public Entity page arms this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFirstRebasedDataFence(
				[&sawPublicEntityPage, &failedDataFence](file_handle_t fd) {
					if (!sawPublicEntityPage.load(std::memory_order_relaxed) ||
						failedDataFence.exchange(true, std::memory_order_relaxed)) {
						return portable_fsync(fd);
					}
#ifdef _WIN32
					SetLastError(ERROR_WRITE_FAULT);
#else
					errno = EIO;
#endif
					return -1;
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_IO_ERROR);
	}
	EXPECT_TRUE(sawPublicEntityPage.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_IO_ERROR);
	freeError();

	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	ASSERT_FALSE(fs::exists(dbPath + ".wal"));
	ASSERT_FALSE(fs::exists(dbPath + "-wal"));
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("ABI_REBASE_RETRY_REL"), 0);
	EXPECT_EQ(countEdges("ABI_REBASE_EXTERNAL_REL"), 1);

	IngestEdgeBatch retryBatch({source}, {target}, {2027}, {"retry"});
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_REBASE_RETRY_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &retryBatch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	EXPECT_EQ(countEdges("ABI_REBASE_RETRY_REL"), 1);
	EXPECT_EQ(countEdges("ABI_REBASE_EXTERNAL_REL"), 1);
}

TEST_F(DISABLED_RelocatableDriverAbiIngestScaleTest,
	   StreamedIngestRebaseGraphSuperblockShortWritePreservesExternalPublicationAndAllowsRetry) {
	const auto [source, target] = createEndpoints();
	zyx_driver_db_t *concurrentWriter = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &concurrentWriter, &error), ZYX_DRIVER_OK);
	ASSERT_NE(concurrentWriter, nullptr);

	IngestEdgeSchema schema;
	constexpr size_t stagedEdgeCount = 1024U;
	IngestEdgeBatch stagedBatch(std::vector<int64_t>(stagedEdgeCount, source),
								std::vector<int64_t>(stagedEdgeCount, target),
								std::vector<int64_t>(stagedEdgeCount, 2026),
								std::vector<std::string>(stagedEdgeCount, std::string(192U, 's')));
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_REBASE_SUPERBLOCK_SHORT_WRITE_REL", &schema.root, &ingestor,
											  &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &stagedBatch.root, nullptr, &error), ZYX_DRIVER_OK);

	int64_t concurrentEdge = 0;
	ASSERT_EQ(zyx_driver_db_create_edge(concurrentWriter, source, target, "ABI_REBASE_SUPERBLOCK_EXTERNAL_REL", nullptr,
										&concurrentEdge, &error),
			  ZYX_DRIVER_OK);
	EXPECT_GT(concurrentEdge, 0);
	ASSERT_EQ(zyx_driver_db_close(concurrentWriter, &error), ZYX_DRIVER_OK);
	concurrentWriter = nullptr;
	const Superblock externalPublication = recoverPublishedSuperblock(dbPath);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortRebasedGraphPublicationSuperblock(
				[&injected, externalPublication](file_handle_t fd, const void *bytes, size_t byteCount,
												 pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U)) &&
						!injected.load(std::memory_order_relaxed)) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							if (candidate.generation > externalPublication.generation &&
								candidate.edge_entity_directory_root !=
										externalPublication.edge_entity_directory_root) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const FormatException &) {
							// Only the rebased authenticated graph publication arms this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_IO_ERROR);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_IO_ERROR);
	freeError();

	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	ASSERT_FALSE(fs::exists(dbPath + ".wal"));
	ASSERT_FALSE(fs::exists(dbPath + "-wal"));
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("ABI_REBASE_SUPERBLOCK_SHORT_WRITE_REL"), 0);
	EXPECT_EQ(countEdges("ABI_REBASE_SUPERBLOCK_EXTERNAL_REL"), 1);

	IngestEdgeBatch retryBatch({source}, {target}, {2027}, {"retry"});
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_REBASE_SUPERBLOCK_SHORT_WRITE_REL", &schema.root, &ingestor,
											  &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &retryBatch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	EXPECT_EQ(countEdges("ABI_REBASE_SUPERBLOCK_SHORT_WRITE_REL"), 1);
	EXPECT_EQ(countEdges("ABI_REBASE_SUPERBLOCK_EXTERNAL_REL"), 1);
}

TEST_F(DISABLED_RelocatableDriverAbiIngestScaleTest, StreamedIngestRebaseFinalFenceFailureRecoversBothPublications) {
	const auto [source, target] = createEndpoints();
	zyx_driver_db_t *concurrentWriter = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &concurrentWriter, &error), ZYX_DRIVER_OK);
	ASSERT_NE(concurrentWriter, nullptr);

	IngestEdgeSchema schema;
	constexpr size_t stagedEdgeCount = 1024U;
	IngestEdgeBatch stagedBatch(std::vector<int64_t>(stagedEdgeCount, source),
								std::vector<int64_t>(stagedEdgeCount, target),
								std::vector<int64_t>(stagedEdgeCount, 2026),
								std::vector<std::string>(stagedEdgeCount, std::string(192U, 's')));
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_REBASE_FINAL_FENCE_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &stagedBatch.root, nullptr, &error), ZYX_DRIVER_OK);

	int64_t concurrentEdge = 0;
	ASSERT_EQ(zyx_driver_db_create_edge(concurrentWriter, source, target, "ABI_REBASE_FINAL_EXTERNAL_REL", nullptr,
										&concurrentEdge, &error),
			  ZYX_DRIVER_OK);
	EXPECT_GT(concurrentEdge, 0);
	ASSERT_EQ(zyx_driver_db_close(concurrentWriter, &error), ZYX_DRIVER_OK);
	concurrentWriter = nullptr;
	// The external Edge has already made the edge directory valid, so distinguish
	// the final rebased graph publication by its newer generation and new root.
	auto superblockStream = std::make_shared<std::fstream>(dbPath, std::ios::binary | std::ios::in | std::ios::out);
	ASSERT_TRUE(superblockStream->is_open());
	const file_handle_t superblockWrite = portable_open_rw(dbPath.c_str());
	const file_handle_t superblockRead = portable_open(dbPath.c_str(), O_RDONLY);
	ASSERT_NE(superblockWrite, INVALID_FILE_HANDLE);
	ASSERT_NE(superblockRead, INVALID_FILE_HANDLE);
	auto superblockIo = std::make_shared<StorageIO>(superblockStream, superblockWrite, superblockRead);
	const Superblock externalPublication = SuperblockFile(superblockIo).recover();
	superblockIo.reset();
	EXPECT_EQ(portable_close_rw(superblockWrite), 0);
	EXPECT_EQ(portable_close(superblockRead), 0);
	superblockStream->close();

	std::atomic<bool> sawGraphPublication{false};
	std::atomic<bool> failedFinalFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeGraphPublication(
				[&sawGraphPublication, externalPublication](file_handle_t fd, const void *bytes, size_t byteCount,
															pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						try {
							const Superblock candidate = SuperblockCodec::decode(
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
							if (candidate.generation > externalPublication.generation &&
								candidate.edge_entity_directory_root !=
										externalPublication.edge_entity_directory_root) {
								sawGraphPublication.store(true, std::memory_order_relaxed);
							}
						} catch (const FormatException &) {
							// Only an authenticated graph-publication Superblock arms this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failRebasedFinalPublicationFence(
				[&sawGraphPublication, &failedFinalFence](file_handle_t fd) {
					if (!sawGraphPublication.load(std::memory_order_relaxed) ||
						failedFinalFence.exchange(true, std::memory_order_relaxed)) {
						return portable_fsync(fd);
					}
#ifdef _WIN32
					SetLastError(ERROR_WRITE_FAULT);
#else
					errno = EIO;
#endif
					return -1;
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_IO_ERROR);
	}
	EXPECT_TRUE(sawGraphPublication.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedFinalFence.load(std::memory_order_relaxed));
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_IO_ERROR);
	freeError();

	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	int64_t unexpectedNode = 0;
	EXPECT_EQ(zyx_driver_db_create_node(db, "MustReopen", nullptr, &unexpectedNode, &error),
			  ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	ASSERT_FALSE(fs::exists(dbPath + ".wal"));
	ASSERT_FALSE(fs::exists(dbPath + "-wal"));
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("ABI_REBASE_FINAL_FENCE_REL"), static_cast<int64_t>(stagedEdgeCount));
	EXPECT_EQ(countEdges("ABI_REBASE_FINAL_EXTERNAL_REL"), 1);
	int64_t newNode = 0;
	EXPECT_EQ(zyx_driver_db_create_node(db, "WritableAfterRebaseFinalFence", nullptr, &newNode, &error), ZYX_DRIVER_OK);
	EXPECT_GT(newNode, target);
}

TEST_F(RelocatableDriverAbiIngestTest, PublicShortWritePoisonsTheAbiSessionAndPreservesTheOldGraph) {
	const auto [source, target] = createEndpoints();
	IngestEdgeSchema schema;
	IngestEdgeBatch batch({source}, {target}, {2026}, {"public-short-write"});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "PUBLIC_SHORT_WRITE_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortPublicEntityPage(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE && !injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							if (PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES)).kind ==
								PageOwnerKind::POK_ENTITY) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const FormatException &) {
							// Only an authenticated public Entity page arms this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_IO_ERROR);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_IO_ERROR);
	freeError();

	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	ASSERT_FALSE(fs::exists(dbPath + ".wal"));
	ASSERT_FALSE(fs::exists(dbPath + "-wal"));
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("PUBLIC_SHORT_WRITE_REL"), 0);

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "PUBLIC_SHORT_WRITE_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("PUBLIC_SHORT_WRITE_REL"), 1);
}

TEST_F(RelocatableDriverAbiIngestTest, PrivateScratchShortWritePoisonsTheAbiSessionAndPreservesTheOldGraph) {
	const auto [source, target] = createEndpoints();
	IngestEdgeSchema schema;
	IngestEdgeBatch batch({source}, {target}, {2026}, {"private-short-write"});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "PRIVATE_SHORT_WRITE_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
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
							// Only an authenticated sealed scratch image arms this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_IO_ERROR);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_IO_ERROR);
	freeError();

	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	ASSERT_FALSE(fs::exists(dbPath + ".wal"));
	ASSERT_FALSE(fs::exists(dbPath + "-wal"));
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("PRIVATE_SHORT_WRITE_REL"), 0);

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "PRIVATE_SHORT_WRITE_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("PRIVATE_SHORT_WRITE_REL"), 1);
}

TEST_F(RelocatableDriverAbiIngestTest, PublicDataFenceFailurePoisonsTheAbiSessionAndPreservesTheOldGraph) {
	const auto [source, target] = createEndpoints();
	IngestEdgeSchema schema;
	IngestEdgeBatch batch({source}, {target}, {2026}, {"public-data-fence"});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "PUBLIC_DATA_FENCE_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	std::atomic<bool> sawPublicEntityPage{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observePublicEntityPage(
				[&sawPublicEntityPage](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							if (PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES)).kind ==
								PageOwnerKind::POK_ENTITY) {
								sawPublicEntityPage.store(true, std::memory_order_relaxed);
							}
						} catch (const FormatException &) {
							// Only an authenticated public Entity page arms this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFirstPublicDataFence(
				[&sawPublicEntityPage, &failedDataFence](file_handle_t fd) {
					if (!sawPublicEntityPage.load(std::memory_order_relaxed) ||
						failedDataFence.exchange(true, std::memory_order_relaxed)) {
						return portable_fsync(fd);
					}
#ifdef _WIN32
					SetLastError(ERROR_DISK_FULL);
#else
					errno = ENOSPC;
#endif
					return -1;
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_IO_ERROR);
	}
	EXPECT_TRUE(sawPublicEntityPage.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_IO_ERROR);
	freeError();

	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	ASSERT_FALSE(fs::exists(dbPath + ".wal"));
	ASSERT_FALSE(fs::exists(dbPath + "-wal"));
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("PUBLIC_DATA_FENCE_REL"), 0);

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "PUBLIC_DATA_FENCE_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("PUBLIC_DATA_FENCE_REL"), 1);
}

TEST_F(RelocatableDriverAbiIngestTest, DiskFullDuringCommitPoisonsTheAbiSessionAndPreservesTheOldGraph) {
	const auto [source, target] = createEndpoints();
	IngestEdgeSchema schema;
	IngestEdgeBatch batch({source}, {target}, {2026}, {"disk-full"});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "DISK_FULL_REL", &schema.root, &ingestor, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	std::atomic<bool> sawPrivateScratchSegment{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observePrivateScratchSegment(
				[&sawPrivateScratchSegment](file_handle_t fd, const void *bytes, size_t byteCount,
											pwrite_off_t offset) {
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
		StorageIO::ScopedNativeSyncHookForTesting failFirstPublicationFence(
				[&sawPrivateScratchSegment, &failedDataFence](file_handle_t fd) {
					if (!sawPrivateScratchSegment.load(std::memory_order_relaxed) ||
						failedDataFence.exchange(true, std::memory_order_relaxed)) {
						return portable_fsync(fd);
					}
#ifdef _WIN32
					SetLastError(ERROR_DISK_FULL);
#else
					errno = ENOSPC;
#endif
					return -1;
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_IO_ERROR);
	}
	EXPECT_TRUE(sawPrivateScratchSegment.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_IO_ERROR);
	freeError();

	// The failed publication is terminal for this session; a caller must release
	// it and reopen rather than trying to continue from an indeterminate write.
	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	ASSERT_FALSE(fs::exists(dbPath + ".wal"));
	ASSERT_FALSE(fs::exists(dbPath + "-wal"));
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("DISK_FULL_REL"), 0);

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "DISK_FULL_REL", &schema.root, &ingestor, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	EXPECT_EQ(countEdges("DISK_FULL_REL"), 1);
}

TEST_F(RelocatableDriverAbiIngestTest, PrivateScratchDataFenceIoFailurePoisonsTheAbiSessionAndPreservesTheOldGraph) {
	const auto [source, target] = createEndpoints();
	IngestEdgeSchema schema;
	IngestEdgeBatch batch({source}, {target}, {2026}, {"private-scratch-data-fence"});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "PRIVATE_SCRATCH_DATA_FENCE_REL", &schema.root, &ingestor,
									  &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	std::atomic<bool> sawPrivateScratchSegment{false};
	std::atomic<bool> failedDataFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observePrivateScratchSegment(
				[&sawPrivateScratchSegment](file_handle_t fd, const void *bytes, size_t byteCount,
													 pwrite_off_t offset) {
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
		StorageIO::ScopedNativeSyncHookForTesting failPrivateScratchDataFence(
				[&sawPrivateScratchSegment, &failedDataFence](file_handle_t fd) {
					if (!sawPrivateScratchSegment.load(std::memory_order_relaxed) ||
						failedDataFence.exchange(true, std::memory_order_relaxed)) {
						return portable_fsync(fd);
					}
#ifdef _WIN32
					SetLastError(ERROR_WRITE_FAULT);
#else
					errno = EIO;
#endif
					return -1;
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_IO_ERROR);
	}
	EXPECT_TRUE(sawPrivateScratchSegment.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedDataFence.load(std::memory_order_relaxed));
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_IO_ERROR);
	freeError();

	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	ASSERT_FALSE(fs::exists(dbPath + ".wal"));
	ASSERT_FALSE(fs::exists(dbPath + "-wal"));
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("PRIVATE_SCRATCH_DATA_FENCE_REL"), 0);

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "PRIVATE_SCRATCH_DATA_FENCE_REL", &schema.root, &ingestor,
									  &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	EXPECT_EQ(countEdges("PRIVATE_SCRATCH_DATA_FENCE_REL"), 1);
}

TEST_F(RelocatableDriverAbiIngestTest, GraphSuperblockShortWritePoisonsTheAbiSessionAndPreservesTheOldGraph) {
	const auto [source, target] = createEndpoints();
	IngestEdgeSchema schema;
	IngestEdgeBatch batch({source}, {target}, {2026}, {"graph-superblock-short-write"});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "GRAPH_SUPERBLOCK_SHORT_WRITE_REL", &schema.root, &ingestor,
									  &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting shortGraphPublicationSuperblock(
				[&injected](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U)) &&
						!injected.load(std::memory_order_relaxed)) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							if (SuperblockCodec::decode(page).edge_entity_directory_root.isValid()) {
								injected.store(true, std::memory_order_relaxed);
								return portable_pwrite(fd, bytes, 16U, offset);
							}
						} catch (const FormatException &) {
							// Only an authenticated graph-publication Superblock arms this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_IO_ERROR);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_IO_ERROR);
	freeError();

	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	ASSERT_FALSE(fs::exists(dbPath + ".wal"));
	ASSERT_FALSE(fs::exists(dbPath + "-wal"));
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("GRAPH_SUPERBLOCK_SHORT_WRITE_REL"), 0);

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "GRAPH_SUPERBLOCK_SHORT_WRITE_REL", &schema.root, &ingestor,
									  &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	EXPECT_EQ(countEdges("GRAPH_SUPERBLOCK_SHORT_WRITE_REL"), 1);
}

TEST_F(RelocatableDriverAbiIngestTest, FinalFenceFailurePoisonsTheAbiSessionAndReopenRecoversTheCompleteGeneration) {
	const auto [source, target] = createEndpoints();
	IngestEdgeSchema schema;
	IngestEdgeBatch batch({source}, {target}, {2026}, {"final-fence"});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "FINAL_FENCE_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	std::atomic<bool> sawGraphPublication{false};
	std::atomic<bool> failedFinalFence{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting observeGraphPublication(
				[&sawGraphPublication](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
					if (byteCount == RSS_PAGE_SIZE &&
						(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
						try {
							const auto page =
									std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount);
							if (SuperblockCodec::decode(page).edge_entity_directory_root.isValid()) {
								sawGraphPublication.store(true, std::memory_order_relaxed);
							}
						} catch (const FormatException &) {
							// Only an authenticated graph-publication Superblock arms this fault point.
						}
					}
					return portable_pwrite(fd, bytes, byteCount, offset);
				});
		StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence(
				[&sawGraphPublication, &failedFinalFence](file_handle_t fd) {
					if (!sawGraphPublication.load(std::memory_order_relaxed) ||
						failedFinalFence.exchange(true, std::memory_order_relaxed)) {
						return portable_fsync(fd);
					}
#ifdef _WIN32
					SetLastError(ERROR_WRITE_FAULT);
#else
					errno = EIO;
#endif
					return -1;
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_IO_ERROR);
	}
	EXPECT_TRUE(sawGraphPublication.load(std::memory_order_relaxed));
	EXPECT_TRUE(failedFinalFence.load(std::memory_order_relaxed));
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_IO_ERROR);
	freeError();

	// A failed final durability fence has an indeterminate outcome. The old handle
	// must reject further writes until it is closed and the file is reopened.
	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	int64_t unexpectedNode = 0;
	EXPECT_EQ(zyx_driver_db_create_node(db, "MustReopen", nullptr, &unexpectedNode, &error),
			  ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;
	EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	ASSERT_FALSE(fs::exists(dbPath + ".wal"));
	ASSERT_FALSE(fs::exists(dbPath + "-wal"));
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("FINAL_FENCE_REL"), 1);
}

TEST_F(RelocatableDriverAbiIngestTest, ProcessCrashBeforePublicationRetainsTheOldGraphAndAllowsRetry) {
	const auto [source, target] = createEndpoints();
	ASSERT_EQ(source, 1);
	ASSERT_EQ(target, 2);
	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	const fs::path markerPath = dbPath + ".crash-marker";
	graph::test::relocatable_crash::CrashChild child(dbPath, markerPath, "driver-abi-ingest-before-publication");
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("CRASH_REL"), 0);
	EXPECT_FALSE(fs::exists(dbPath + ".wal"));
	EXPECT_FALSE(fs::exists(dbPath + "-wal"));

	IngestEdgeSchema schema;
	IngestEdgeBatch batch({source}, {target}, {2026}, {"retry"});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "CRASH_REL", &schema.root, &ingestor, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("CRASH_REL"), 1);
}

TEST_F(DISABLED_RelocatableDriverAbiIngestScaleTest,
	ProcessCrashBeforeRebasedPublicationRetainsExternalGraphAndAllowsRetry) {
	const auto [source, target] = createEndpoints();
	ASSERT_EQ(source, 1);
	ASSERT_EQ(target, 2);

	const fs::path stagedMarker = dbPath + ".rebase-staged-marker";
	const fs::path resumeMarker = stagedMarker.string() + ".resume";
	const fs::path crashMarker = stagedMarker.string() + ".crash";
	graph::test::relocatable_crash::CrashChild child(dbPath, stagedMarker,
													 "driver-abi-ingest-before-rebased-publication");
	ASSERT_TRUE(child.waitForMarker(stagedMarker)) << child.failureReason();

	int64_t externalEdge = 0;
	ASSERT_EQ(
			zyx_driver_db_create_edge(db, source, target, "CRASH_REBASE_EXTERNAL_REL", nullptr, &externalEdge, &error),
			ZYX_DRIVER_OK);
	EXPECT_GT(externalEdge, 0);
	graph::test::relocatable_crash::writeMarker(resumeMarker);
	ASSERT_TRUE(child.waitForMarker(crashMarker)) << child.failureReason();
	child.terminate();

	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("CRASH_REBASE_EXTERNAL_REL"), 1);
	EXPECT_EQ(countEdges("CRASH_REBASE_REL"), 0);
	EXPECT_FALSE(fs::exists(dbPath + ".wal"));
	EXPECT_FALSE(fs::exists(dbPath + "-wal"));

	IngestEdgeSchema schema;
	IngestEdgeBatch retryBatch({source}, {target}, {2027}, {"retry"});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "CRASH_REBASE_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &retryBatch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("CRASH_REBASE_REL"), 1);
}

TEST_F(DISABLED_RelocatableDriverAbiIngestScaleTest,
	ProcessCompletesRebasedPublicationAcrossProcessesAndRemainsWritable) {
	const auto [source, target] = createEndpoints();
	ASSERT_EQ(source, 1);
	ASSERT_EQ(target, 2);

	const fs::path stagedMarker = dbPath + ".rebase-staged-marker";
	const fs::path resumeMarker = stagedMarker.string() + ".resume";
	const fs::path completeMarker = stagedMarker.string() + ".complete";
	graph::test::relocatable_crash::CrashChild child(dbPath, stagedMarker, "driver-abi-ingest-rebased-publication");
	ASSERT_TRUE(child.waitForMarker(stagedMarker)) << child.failureReason();

	int64_t externalEdge = 0;
	ASSERT_EQ(zyx_driver_db_create_edge(db, source, target, "CROSS_PROCESS_REBASE_EXTERNAL_REL", nullptr, &externalEdge,
										&error),
			  ZYX_DRIVER_OK);
	EXPECT_GT(externalEdge, 0);
	graph::test::relocatable_crash::writeMarker(resumeMarker);
	ASSERT_TRUE(child.waitForSuccessfulExit()) << child.failureReason();
	ASSERT_TRUE(child.waitForMarker(completeMarker)) << child.failureReason();

	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("CROSS_PROCESS_REBASE_EXTERNAL_REL"), 1);
	EXPECT_EQ(countEdges("CROSS_PROCESS_REBASE_REL"), 1024);
	EXPECT_FALSE(fs::exists(dbPath + ".wal"));
	EXPECT_FALSE(fs::exists(dbPath + "-wal"));
	int64_t newNode = 0;
	EXPECT_EQ(zyx_driver_db_create_node(db, "WritableAfterCrossProcessRebase", nullptr, &newNode, &error),
			  ZYX_DRIVER_OK);
	EXPECT_GT(newNode, target);
}

TEST_F(DISABLED_RelocatableDriverAbiIngestScaleTest,
	   ProcessCrashAfterRebasedPublicationBeforeFinalSyncRecoversBothGraphsAndRemainsWritable) {
	const auto [source, target] = createEndpoints();
	ASSERT_EQ(source, 1);
	ASSERT_EQ(target, 2);

	const fs::path stagedMarker = dbPath + ".rebase-staged-marker";
	const fs::path resumeMarker = stagedMarker.string() + ".resume";
	const fs::path crashMarker = stagedMarker.string() + ".crash";
	graph::test::relocatable_crash::CrashChild child(dbPath, stagedMarker,
													 "driver-abi-ingest-after-rebased-publication");
	ASSERT_TRUE(child.waitForMarker(stagedMarker)) << child.failureReason();

	int64_t externalEdge = 0;
	ASSERT_EQ(
			zyx_driver_db_create_edge(db, source, target, "CRASH_REBASE_EXTERNAL_REL", nullptr, &externalEdge, &error),
			ZYX_DRIVER_OK);
	EXPECT_GT(externalEdge, 0);
	graph::test::relocatable_crash::writeMarker(resumeMarker);
	ASSERT_TRUE(child.waitForMarker(crashMarker)) << child.failureReason();
	child.terminate();

	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("CRASH_REBASE_EXTERNAL_REL"), 1);
	EXPECT_EQ(countEdges("CRASH_REBASE_REL"), 1024);
	EXPECT_FALSE(fs::exists(dbPath + ".wal"));
	EXPECT_FALSE(fs::exists(dbPath + "-wal"));
	int64_t newNode = 0;
	EXPECT_EQ(zyx_driver_db_create_node(db, "WritableAfterRebasedCrash", nullptr, &newNode, &error), ZYX_DRIVER_OK);
	EXPECT_GT(newNode, target);
}

TEST_F(RelocatableDriverAbiIngestTest,
	   ProcessCrashAfterPublicationBeforeFinalSyncRecoversTheCompleteGraphAndRemainsWritable) {
	const auto [source, target] = createEndpoints();
	ASSERT_EQ(source, 1);
	ASSERT_EQ(target, 2);
	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;

	const fs::path markerPath = dbPath + ".crash-marker";
	graph::test::relocatable_crash::CrashChild child(dbPath, markerPath, "driver-abi-ingest-after-publication");
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();

	ASSERT_EQ(zyx_driver_db_open_if_exists(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_NE(db, nullptr);
	EXPECT_EQ(countEdges("CRASH_REL"), 1);
	EXPECT_FALSE(fs::exists(dbPath + ".wal"));
	EXPECT_FALSE(fs::exists(dbPath + "-wal"));
	int64_t newNode = 0;
	EXPECT_EQ(zyx_driver_db_create_node(db, "WritableAfterCrash", nullptr, &newNode, &error), ZYX_DRIVER_OK);
	EXPECT_GT(newNode, target);
}

TEST_F(DriverAbiIngestTest, InvalidOffsetsPoisonSessionAndReportRowAndField) {
	const auto [source, target] = createEndpoints();
	IngestEdgeSchema schema;
	IngestEdgeBatch batch({source, target}, {target, source}, {1, 2}, {"valid", "also-valid"});
	batch.nameOffsets = {0, 5, 3};
	batch.initialize();
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "BAD_ARROW_REL", &schema.root, &ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_row_index(error), 1);
	EXPECT_STREQ(zyx_driver_error_field_path(error), "record_batch.properties.name");
	freeError();

	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("BAD_ARROW_REL"), 0);
}

TEST_F(DriverAbiIngestTest, MissingEndpointReportsInputRowAndRollsBack) {
	const auto [source, target] = createEndpoints();
	IngestEdgeSchema schema;
	IngestEdgeBatch batch({source, source}, {target, 999999999}, {1, 2}, {"valid", "missing"});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "MISSING_ENDPOINT_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_NOT_FOUND);
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_row_index(error), 1);
	EXPECT_STREQ(zyx_driver_error_field_path(error), "target_id");
	freeError();

	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("MISSING_ENDPOINT_REL"), 0);
}

TEST_F(DriverAbiIngestTest, ActiveIngestPreventsDatabaseClose) {
	zyx_driver_ingest_t *ingest = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;
}

TEST_F(DriverAbiIngestTest, SchemaPreparationRejectsInvalidShapeWithoutPoisoningSession) {
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);

	IngestEdgeSchema invalidSchema;
	invalidSchema.source.format = "i";
	EXPECT_EQ(zyx_driver_ingest_prepare_edges(ingest, "INVALID_SCHEMA_REL", &invalidSchema.root, &ingestor, &error),
			  ZYX_DRIVER_TYPE_MISMATCH);
	EXPECT_EQ(ingestor, nullptr);
	EXPECT_STREQ(zyx_driver_error_field_path(error), "source_id");
	freeError();

	IngestEdgeSchema validSchema;
	EXPECT_EQ(zyx_driver_ingest_prepare_edges(ingest, "VALID_SCHEMA_REL", &validSchema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
}

TEST_F(DriverAbiIngestTest, NestedListBoolDoubleAndDictionaryPropertiesRoundTrip) {
	const auto [source, target] = createEndpoints();

	ArrowSchema tagValueSchema = scalarSchema("u", "item");
	std::array<ArrowSchema *, 1> tagChildren{&tagValueSchema};
	ArrowSchema tagsSchema{"+l",	"tags",	 nullptr, ARROW_FLAG_NULLABLE, 1, tagChildren.data(),
						   nullptr, nullptr, nullptr};
	ArrowSchema largeTagsSchema{"+L",	 "large_tags", nullptr, ARROW_FLAG_NULLABLE, 1, tagChildren.data(),
								nullptr, nullptr,	   nullptr};
	ArrowSchema dictionaryValueSchema = scalarSchema("u", "dictionary");
	ArrowSchema categorySchema = scalarSchema("i", "category");
	categorySchema.dictionary = &dictionaryValueSchema;
	std::array<ArrowSchema, 2> scalarPropertySchemas{scalarSchema("g", "score"), scalarSchema("b", "active")};
	std::array<ArrowSchema *, 5> propertySchemaPointers{&scalarPropertySchemas[0], &scalarPropertySchemas[1],
														&tagsSchema, &largeTagsSchema, &categorySchema};
	ArrowSchema propertiesSchema{"+s",
								 "properties",
								 nullptr,
								 ARROW_FLAG_NULLABLE,
								 static_cast<int64_t>(propertySchemaPointers.size()),
								 propertySchemaPointers.data(),
								 nullptr,
								 nullptr,
								 nullptr};
	ArrowSchema sourceSchema = scalarSchema("l", "source_id");
	ArrowSchema targetSchema = scalarSchema("l", "target_id");
	std::array<ArrowSchema *, 3> rootSchemaPointers{&sourceSchema, &targetSchema, &propertiesSchema};
	ArrowSchema rootSchema{"+s", "edges", nullptr, 0, 3, rootSchemaPointers.data(), nullptr, nullptr, nullptr};

	std::array<int64_t, 2> sourceIds{source, target};
	std::array<int64_t, 2> targetIds{target, source};
	std::array<double, 2> scores{1.5, 2.5};
	std::array<uint8_t, 1> activeBits{0x01};
	std::array<int32_t, 3> tagOffsets{0, 2, 3};
	std::array<int64_t, 3> largeTagOffsets{0, 2, 3};
	std::array<int32_t, 4> tagValueOffsets{0, 1, 2, 3};
	const std::string tagBytes = "abc";
	std::array<int32_t, 2> categoryIndices{0, 1};
	std::array<int32_t, 3> dictionaryOffsets{0, 3, 7};
	const std::string dictionaryBytes = "redblue";

	std::array<const void *, 2> sourceBuffers{nullptr, sourceIds.data()};
	std::array<const void *, 2> targetBuffers{nullptr, targetIds.data()};
	std::array<const void *, 2> scoreBuffers{nullptr, scores.data()};
	std::array<const void *, 2> activeBuffers{nullptr, activeBits.data()};
	std::array<const void *, 2> tagBuffers{nullptr, tagOffsets.data()};
	std::array<const void *, 2> largeTagBuffers{nullptr, largeTagOffsets.data()};
	std::array<const void *, 3> tagValueBuffers{nullptr, tagValueOffsets.data(), tagBytes.data()};
	std::array<const void *, 2> categoryBuffers{nullptr, categoryIndices.data()};
	std::array<const void *, 3> dictionaryBuffers{nullptr, dictionaryOffsets.data(), dictionaryBytes.data()};
	std::array<const void *, 1> structBuffers{nullptr};

	ArrowArray sourceArray{2, 0, 0, 2, 0, sourceBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray targetArray{2, 0, 0, 2, 0, targetBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray scoreArray{2, 0, 0, 2, 0, scoreBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray activeArray{2, 0, 0, 2, 0, activeBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray tagValueArray{3, 0, 0, 3, 0, tagValueBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 1> tagArrayChildren{&tagValueArray};
	ArrowArray tagsArray{2, 0, 0, 2, 1, tagBuffers.data(), tagArrayChildren.data(), nullptr, nullptr, nullptr};
	ArrowArray largeTagsArray{2,	   0,		0,		2, 1, largeTagBuffers.data(), tagArrayChildren.data(),
							  nullptr, nullptr, nullptr};
	ArrowArray dictionaryArray{2, 0, 0, 3, 0, dictionaryBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray categoryArray{2, 0, 0, 2, 0, categoryBuffers.data(), nullptr, &dictionaryArray, nullptr, nullptr};
	std::array<ArrowArray *, 5> propertyArrayChildren{&scoreArray, &activeArray, &tagsArray, &largeTagsArray,
													  &categoryArray};
	ArrowArray propertiesArray{2,
							   0,
							   0,
							   1,
							   static_cast<int64_t>(propertyArrayChildren.size()),
							   structBuffers.data(),
							   propertyArrayChildren.data(),
							   nullptr,
							   nullptr,
							   nullptr};
	std::array<ArrowArray *, 3> rootArrayChildren{&sourceArray, &targetArray, &propertiesArray};
	ArrowArray rootArray{2, 0, 0, 1, 3, structBuffers.data(), rootArrayChildren.data(), nullptr, nullptr, nullptr};

	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "NESTED_ARROW_REL", &rootSchema, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &rootArray, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);

	zyx_driver_result_t *result = nullptr;
	ASSERT_EQ(zyx_driver_db_execute(
					  db,
					  "MATCH ()-[e:NESTED_ARROW_REL]->() RETURN e.score, e.active, e.tags, e.category, e.large_tags "
					  "ORDER BY e.score",
					  nullptr, &result, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	double score = 0.0;
	bool active = false;
	uint32_t tagCount = 0;
	const char *tag = nullptr;
	const char *category = nullptr;
	EXPECT_EQ(zyx_driver_result_get_double(result, 0, &score, &error), ZYX_DRIVER_OK);
	EXPECT_DOUBLE_EQ(score, 1.5);
	EXPECT_EQ(zyx_driver_result_get_bool(result, 1, &active, &error), ZYX_DRIVER_OK);
	EXPECT_TRUE(active);
	ASSERT_EQ(zyx_driver_result_get_list_count(result, 2, &tagCount, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(tagCount, 2u);
	ASSERT_EQ(zyx_driver_result_get_list_string(result, 2, 1, &tag, &error), ZYX_DRIVER_OK);
	EXPECT_STREQ(tag, "b");
	ASSERT_EQ(zyx_driver_result_get_string(result, 3, &category, &error), ZYX_DRIVER_OK);
	EXPECT_STREQ(category, "red");
	ASSERT_EQ(zyx_driver_result_get_list_count(result, 4, &tagCount, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(tagCount, 2u);

	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	EXPECT_EQ(zyx_driver_result_get_bool(result, 1, &active, &error), ZYX_DRIVER_OK);
	EXPECT_FALSE(active);
	ASSERT_EQ(zyx_driver_result_get_string(result, 3, &category, &error), ZYX_DRIVER_OK);
	EXPECT_STREQ(category, "blue");
	zyx_driver_result_free(result);

	categoryIndices[0] = 2;
	ingest = nullptr;
	ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "BAD_DICTIONARY_REL", &rootSchema, &ingestor, &error),
			  ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_write(ingestor, &rootArray, nullptr, &error), ZYX_DRIVER_OUT_OF_RANGE);
	EXPECT_EQ(zyx_driver_error_row_index(error), 0);
	EXPECT_STREQ(zyx_driver_error_field_path(error), "properties.category");
	freeError();
	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
}

TEST_F(DriverAbiIngestTest, LifecycleValidatesHandlesAndFinalizedStates) {
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	IngestEdgeSchema schema;

	EXPECT_EQ(zyx_driver_ingest_begin(nullptr, &ingest, &error), ZYX_DRIVER_INVALID_ARGUMENT);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_begin(db, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
	freeError();
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);

	zyx_driver_ingest_t *second = nullptr;
	EXPECT_EQ(zyx_driver_ingest_begin(db, &second, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	EXPECT_EQ(second, nullptr);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_prepare_edges(nullptr, "REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_INVALID_ARGUMENT);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_prepare_edges(ingest, nullptr, &schema.root, &ingestor, &error),
			  ZYX_DRIVER_INVALID_ARGUMENT);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_prepare_edges(ingest, "", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_INVALID_ARGUMENT);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_prepare_edges(ingest, "REL", &schema.root, nullptr, &error),
			  ZYX_DRIVER_INVALID_ARGUMENT);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_write(nullptr, nullptr, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
	freeError();

	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "LIFECYCLE_REL", &schema.root, &ingestor, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_write(ingestor, nullptr, nullptr, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(nullptr, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(nullptr, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_error_row_index(nullptr), -1);
	EXPECT_STREQ(zyx_driver_error_field_path(nullptr), "");
}

TEST_F(DriverAbiIngestTest, EmptyRecordBatchCommitsWithoutAllocatingIds) {
	IngestEdgeSchema schema;
	IngestEdgeBatch batch({}, {}, {}, {});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	zyx_driver_id_range_t ids{99, 99};

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "EMPTY_ARROW_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, &ids, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(ids.first_id, 0);
	EXPECT_EQ(ids.count, 0);
	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("EMPTY_ARROW_REL"), 0);
}

TEST_F(DriverAbiIngestTest, PrimitiveArrowFormatsShareOneColumnarWritePath) {
	const auto [source, target] = createEndpoints();
	const std::array<const char *, 13> formats{"c", "C", "s", "S", "i", "I", "l", "L", "f", "g", "b", "u", "U"};
	const std::array<const char *, 13> names{"i8",	"u8",  "i16", "u16",  "i32",  "u32",	   "i64",
											 "u64", "f32", "f64", "flag", "utf8", "large_utf8"};
	std::array<ArrowSchema, 14> propertySchemas{};
	std::array<ArrowSchema *, 14> propertySchemaPointers{};
	for (size_t index = 0; index < formats.size(); ++index) {
		propertySchemas[index] = scalarSchema(formats[index], names[index]);
		propertySchemaPointers[index] = &propertySchemas[index];
	}
	propertySchemas[13] = scalarSchema("n", "nothing");
	propertySchemaPointers[13] = &propertySchemas[13];

	ArrowSchema sourceSchema = scalarSchema("l", "source_id");
	ArrowSchema targetSchema = scalarSchema("l", "target_id");
	ArrowSchema propertiesSchema{"+s",	  "properties", nullptr, ARROW_FLAG_NULLABLE, 14, propertySchemaPointers.data(),
								 nullptr, nullptr,		nullptr};
	std::array<ArrowSchema *, 3> rootSchemaPointers{&sourceSchema, &targetSchema, &propertiesSchema};
	ArrowSchema rootSchema{"+s", "edges", nullptr, 0, 3, rootSchemaPointers.data(), nullptr, nullptr, nullptr};

	const int8_t i8 = -8;
	const uint8_t u8 = 8;
	const int16_t i16 = -16;
	const uint16_t u16 = 16;
	const int32_t i32 = -32;
	const uint32_t u32 = 32;
	const int64_t i64 = -64;
	uint64_t u64 = 64;
	const float f32 = 3.25F;
	const double f64 = 6.5;
	const uint8_t boolBits = 0x01;
	const std::array<int32_t, 2> utf8Offsets{0, 4};
	const std::array<int64_t, 2> largeUtf8Offsets{0, 5};
	const std::string utf8 = "text";
	const std::string largeUtf8 = "large";
	const int64_t sourceId = source;
	const int64_t targetId = target;

	std::array<const void *, 2> sourceBuffers{nullptr, &sourceId};
	std::array<const void *, 2> targetBuffers{nullptr, &targetId};
	std::array<std::array<const void *, 3>, 13> propertyBuffers{};
	propertyBuffers[0] = {nullptr, &i8, nullptr};
	propertyBuffers[1] = {nullptr, &u8, nullptr};
	propertyBuffers[2] = {nullptr, &i16, nullptr};
	propertyBuffers[3] = {nullptr, &u16, nullptr};
	propertyBuffers[4] = {nullptr, &i32, nullptr};
	propertyBuffers[5] = {nullptr, &u32, nullptr};
	propertyBuffers[6] = {nullptr, &i64, nullptr};
	propertyBuffers[7] = {nullptr, &u64, nullptr};
	propertyBuffers[8] = {nullptr, &f32, nullptr};
	propertyBuffers[9] = {nullptr, &f64, nullptr};
	propertyBuffers[10] = {nullptr, &boolBits, nullptr};
	propertyBuffers[11] = {nullptr, utf8Offsets.data(), utf8.data()};
	propertyBuffers[12] = {nullptr, largeUtf8Offsets.data(), largeUtf8.data()};
	std::array<const void *, 1> structBuffers{nullptr};

	ArrowArray sourceArray{1, 0, 0, 2, 0, sourceBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray targetArray{1, 0, 0, 2, 0, targetBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	std::array<ArrowArray, 14> propertyArrays{};
	for (size_t index = 0; index < propertyBuffers.size(); ++index) {
		const int64_t bufferCount = index >= 11 ? 3 : 2;
		propertyArrays[index] =
				ArrowArray{1, 0, 0, bufferCount, 0, propertyBuffers[index].data(), nullptr, nullptr, nullptr, nullptr};
	}
	propertyArrays[13] = ArrowArray{1, 1, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 14> propertyArrayPointers{};
	for (size_t index = 0; index < propertyArrays.size(); ++index) {
		propertyArrayPointers[index] = &propertyArrays[index];
	}
	ArrowArray propertiesArray{1,		0,		 0,		 1, 14, structBuffers.data(), propertyArrayPointers.data(),
							   nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 3> rootArrayPointers{&sourceArray, &targetArray, &propertiesArray};
	ArrowArray rootArray{1, 0, 0, 1, 3, structBuffers.data(), rootArrayPointers.data(), nullptr, nullptr, nullptr};

	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "PRIMITIVE_ARROW_REL", &rootSchema, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &rootArray, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("PRIMITIVE_ARROW_REL"), 1);

	u64 = (std::numeric_limits<uint64_t>::max)();
	ingest = nullptr;
	ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "UINT64_RANGE_REL", &rootSchema, &ingestor, &error),
			  ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_write(ingestor, &rootArray, nullptr, &error), ZYX_DRIVER_OUT_OF_RANGE);
	EXPECT_EQ(zyx_driver_error_row_index(error), 0);
	EXPECT_STREQ(zyx_driver_error_field_path(error), "properties.u64");
	freeError();
	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
}

TEST_F(DriverAbiIngestTest, FixedSizeListAndMapPropertiesRoundTrip) {
	const auto [source, target] = createEndpoints();

	ArrowSchema vectorValueSchema = scalarSchema("f", "item");
	std::array<ArrowSchema *, 1> vectorChildren{&vectorValueSchema};
	ArrowSchema vectorSchema{"+w:2",  "embedding", nullptr, ARROW_FLAG_NULLABLE, 1, vectorChildren.data(),
							 nullptr, nullptr,	   nullptr};
	ArrowSchema mapKeySchema = scalarSchema("u", "key");
	ArrowSchema mapValueSchema = scalarSchema("l", "value");
	std::array<ArrowSchema *, 2> entryChildren{&mapKeySchema, &mapValueSchema};
	ArrowSchema entrySchema{"+s", "entries", nullptr, 0, 2, entryChildren.data(), nullptr, nullptr, nullptr};
	std::array<ArrowSchema *, 1> mapChildren{&entrySchema};
	ArrowSchema mapSchema{"+m",	   "metadata", nullptr, ARROW_FLAG_NULLABLE, 1, mapChildren.data(),
						  nullptr, nullptr,	   nullptr};
	std::array<ArrowSchema *, 2> propertySchemaPointers{&vectorSchema, &mapSchema};
	ArrowSchema propertiesSchema{"+s",	  "properties", nullptr, 0, 2, propertySchemaPointers.data(),
								 nullptr, nullptr,		nullptr};
	ArrowSchema sourceSchema = scalarSchema("l", "source_id");
	ArrowSchema targetSchema = scalarSchema("l", "target_id");
	std::array<ArrowSchema *, 3> rootSchemaPointers{&sourceSchema, &targetSchema, &propertiesSchema};
	ArrowSchema rootSchema{"+s", "edges", nullptr, 0, 3, rootSchemaPointers.data(), nullptr, nullptr, nullptr};

	const std::array<int64_t, 2> sourceIds{source, target};
	const std::array<int64_t, 2> targetIds{target, source};
	const std::array<float, 4> vectorValues{1.0F, 2.0F, 3.0F, 4.0F};
	const std::array<int32_t, 3> mapOffsets{0, 2, 3};
	const std::array<int32_t, 4> keyOffsets{0, 1, 2, 3};
	const std::string keyBytes = "xyz";
	const std::array<int64_t, 3> mapValues{10, 20, 30};
	std::array<const void *, 2> sourceBuffers{nullptr, sourceIds.data()};
	std::array<const void *, 2> targetBuffers{nullptr, targetIds.data()};
	std::array<const void *, 1> structBuffers{nullptr};
	std::array<const void *, 2> vectorValueBuffers{nullptr, vectorValues.data()};
	std::array<const void *, 2> mapBuffers{nullptr, mapOffsets.data()};
	std::array<const void *, 3> keyBuffers{nullptr, keyOffsets.data(), keyBytes.data()};
	std::array<const void *, 2> mapValueBuffers{nullptr, mapValues.data()};

	ArrowArray sourceArray{2, 0, 0, 2, 0, sourceBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray targetArray{2, 0, 0, 2, 0, targetBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray vectorValueArray{4, 0, 0, 2, 0, vectorValueBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 1> vectorArrayChildren{&vectorValueArray};
	ArrowArray vectorArray{2, 0, 0, 1, 1, structBuffers.data(), vectorArrayChildren.data(), nullptr, nullptr, nullptr};
	ArrowArray keyArray{3, 0, 0, 3, 0, keyBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray mapValueArray{3, 0, 0, 2, 0, mapValueBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 2> entryArrayChildren{&keyArray, &mapValueArray};
	ArrowArray entryArray{3, 0, 0, 1, 2, structBuffers.data(), entryArrayChildren.data(), nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 1> mapArrayChildren{&entryArray};
	ArrowArray mapArray{2, 0, 0, 2, 1, mapBuffers.data(), mapArrayChildren.data(), nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 2> propertyArrayChildren{&vectorArray, &mapArray};
	ArrowArray propertiesArray{2,		0,		 0,		 1, 2, structBuffers.data(), propertyArrayChildren.data(),
							   nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 3> rootArrayChildren{&sourceArray, &targetArray, &propertiesArray};
	ArrowArray rootArray{2, 0, 0, 1, 3, structBuffers.data(), rootArrayChildren.data(), nullptr, nullptr, nullptr};

	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "COMPLEX_ARROW_REL", &rootSchema, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &rootArray, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);

	zyx_driver_result_t *result = nullptr;
	ASSERT_EQ(zyx_driver_db_execute(db, "MATCH ()-[e:COMPLEX_ARROW_REL]->() RETURN e.embedding, e.metadata", nullptr,
									&result, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	uint32_t vectorSize = 0;
	double vectorValue = 0.0;
	ASSERT_EQ(zyx_driver_result_get_list_count(result, 0, &vectorSize, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(vectorSize, 2u);
	ASSERT_EQ(zyx_driver_result_get_list_double(result, 0, 1, &vectorValue, &error), ZYX_DRIVER_OK);
	EXPECT_DOUBLE_EQ(vectorValue, 2.0);
	zyx_driver_value_ref_t metadata{};
	ASSERT_EQ(zyx_driver_result_get_value(result, 1, &metadata, &error), ZYX_DRIVER_OK);
	int64_t x = 0;
	int64_t y = 0;
	EXPECT_EQ(zyx_driver_value_ref_map_get_int64(&metadata, "x", &x, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_value_ref_map_get_int64(&metadata, "y", &y, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(x, 10);
	EXPECT_EQ(y, 20);
	zyx_driver_result_free(result);

	vectorValueArray.length = 3;
	ingest = nullptr;
	ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "SHORT_FIXED_REL", &rootSchema, &ingestor, &error),
			  ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_write(ingestor, &rootArray, nullptr, &error), ZYX_DRIVER_OUT_OF_RANGE);
	EXPECT_STREQ(zyx_driver_error_field_path(error), "record_batch.properties.embedding");
	freeError();
	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
}

TEST_F(DriverAbiIngestTest, TenThousandEdgesUseOnePreparedBatchAndOneIdRange) {
	const auto [source, target] = createEndpoints();
	constexpr int64_t rowCount = 10'000;

	ArrowSchema sourceSchema = scalarSchema("l", "source_id");
	ArrowSchema targetSchema = scalarSchema("l", "target_id");
	ArrowSchema propertiesSchema{"+s", "properties", nullptr, 0, 0, nullptr, nullptr, nullptr, nullptr};
	std::array<ArrowSchema *, 3> rootSchemaPointers{&sourceSchema, &targetSchema, &propertiesSchema};
	ArrowSchema rootSchema{"+s", "edges", nullptr, 0, 3, rootSchemaPointers.data(), nullptr, nullptr, nullptr};

	std::vector<int64_t> sourceIds(static_cast<size_t>(rowCount), source);
	std::vector<int64_t> targetIds(static_cast<size_t>(rowCount), target);
	std::array<const void *, 2> sourceBuffers{nullptr, sourceIds.data()};
	std::array<const void *, 2> targetBuffers{nullptr, targetIds.data()};
	std::array<const void *, 1> structBuffers{nullptr};
	ArrowArray sourceArray{rowCount, 0, 0, 2, 0, sourceBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray targetArray{rowCount, 0, 0, 2, 0, targetBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray propertiesArray{rowCount, 0, 0, 1, 0, structBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 3> rootArrayPointers{&sourceArray, &targetArray, &propertiesArray};
	ArrowArray rootArray{rowCount, 0,		0,		1, 3, structBuffers.data(), rootArrayPointers.data(),
						 nullptr,  nullptr, nullptr};

	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	zyx_driver_id_range_t ids{};
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "TEN_K_ARROW_REL", &rootSchema, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &rootArray, &ids, &error), ZYX_DRIVER_OK);
	EXPECT_GT(ids.first_id, 0);
	EXPECT_EQ(ids.count, rowCount);
	const zyx_driver_status_t commitStatus = zyx_driver_ingest_commit(ingest, &error);
	ASSERT_EQ(commitStatus, ZYX_DRIVER_OK) << zyx_driver_error_message(error);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("TEN_K_ARROW_REL"), rowCount);
}

TEST_F(DriverAbiIngestTest, SchemaCompilerRejectsMalformedArrowContracts) {
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);

	const auto expectSchemaError = [&](ArrowSchema *schema, zyx_driver_status_t expected) {
		EXPECT_EQ(zyx_driver_ingest_prepare_edges(ingest, "BAD_SCHEMA_REL", schema, &ingestor, &error), expected);
		EXPECT_EQ(ingestor, nullptr);
		freeError();
	};

	expectSchemaError(nullptr, ZYX_DRIVER_INVALID_ARGUMENT);

	IngestEdgeSchema wrongRoot;
	wrongRoot.root.format = "i";
	wrongRoot.root.n_children = 0;
	wrongRoot.root.children = nullptr;
	expectSchemaError(&wrongRoot.root, ZYX_DRIVER_TYPE_MISMATCH);

	IngestEdgeSchema wrongCount;
	wrongCount.root.n_children = 2;
	expectSchemaError(&wrongCount.root, ZYX_DRIVER_TYPE_MISMATCH);

	IngestEdgeSchema wrongTarget;
	wrongTarget.target.format = "i";
	expectSchemaError(&wrongTarget.root, ZYX_DRIVER_TYPE_MISMATCH);

	IngestEdgeSchema wrongProperties;
	wrongProperties.properties.format = "u";
	expectSchemaError(&wrongProperties.root, ZYX_DRIVER_INVALID_ARGUMENT);

	IngestEdgeSchema scalarProperties;
	scalarProperties.properties.format = "u";
	scalarProperties.properties.n_children = 0;
	scalarProperties.properties.children = nullptr;
	expectSchemaError(&scalarProperties.root, ZYX_DRIVER_TYPE_MISMATCH);

	IngestEdgeSchema unsupported;
	unsupported.propertyFields[0].format = "z";
	expectSchemaError(&unsupported.root, ZYX_DRIVER_TYPE_MISMATCH);

	IngestEdgeSchema emptyName;
	emptyName.propertyFields[0].name = "";
	expectSchemaError(&emptyName.root, ZYX_DRIVER_INVALID_ARGUMENT);

	IngestEdgeSchema duplicateName;
	duplicateName.propertyFields[1].name = "since";
	expectSchemaError(&duplicateName.root, ZYX_DRIVER_INVALID_ARGUMENT);

	IngestEdgeSchema missingChildren;
	missingChildren.properties.children = nullptr;
	expectSchemaError(&missingChildren.root, ZYX_DRIVER_INVALID_ARGUMENT);

	IngestEdgeSchema primitiveWithChild;
	ArrowSchema nestedScalar = scalarSchema("l", "nested");
	std::array<ArrowSchema *, 1> nestedPointer{&nestedScalar};
	primitiveWithChild.propertyFields[0].n_children = 1;
	primitiveWithChild.propertyFields[0].children = nestedPointer.data();
	expectSchemaError(&primitiveWithChild.root, ZYX_DRIVER_INVALID_ARGUMENT);

	IngestEdgeSchema invalidFixed;
	invalidFixed.propertyFields[0].format = "+w:0";
	invalidFixed.propertyFields[0].n_children = 1;
	invalidFixed.propertyFields[0].children = nestedPointer.data();
	expectSchemaError(&invalidFixed.root, ZYX_DRIVER_INVALID_ARGUMENT);

	IngestEdgeSchema invalidDictionary;
	ArrowSchema dictionary = scalarSchema("u", "dictionary");
	invalidDictionary.propertyFields[0].format = "g";
	invalidDictionary.propertyFields[0].dictionary = &dictionary;
	expectSchemaError(&invalidDictionary.root, ZYX_DRIVER_TYPE_MISMATCH);

	IngestEdgeSchema dictionaryWithChild;
	dictionaryWithChild.propertyFields[0].format = "i";
	dictionaryWithChild.propertyFields[0].dictionary = &dictionary;
	dictionaryWithChild.propertyFields[0].n_children = 1;
	dictionaryWithChild.propertyFields[0].children = nestedPointer.data();
	expectSchemaError(&dictionaryWithChild.root, ZYX_DRIVER_INVALID_ARGUMENT);

	IngestEdgeSchema invalidMapShape;
	invalidMapShape.propertyFields[0].format = "+m";
	invalidMapShape.propertyFields[0].n_children = 1;
	invalidMapShape.propertyFields[0].children = nestedPointer.data();
	expectSchemaError(&invalidMapShape.root, ZYX_DRIVER_TYPE_MISMATCH);

	ArrowSchema integerKey = scalarSchema("l", "key");
	ArrowSchema mapValue = scalarSchema("l", "value");
	std::array<ArrowSchema *, 2> mapEntryChildren{&integerKey, &mapValue};
	ArrowSchema mapEntries{"+s", "entries", nullptr, 0, 2, mapEntryChildren.data(), nullptr, nullptr, nullptr};
	std::array<ArrowSchema *, 1> mapChildren{&mapEntries};
	IngestEdgeSchema invalidMapKey;
	invalidMapKey.propertyFields[0].format = "+m";
	invalidMapKey.propertyFields[0].n_children = 1;
	invalidMapKey.propertyFields[0].children = mapChildren.data();
	expectSchemaError(&invalidMapKey.root, ZYX_DRIVER_TYPE_MISMATCH);

	IngestEdgeSchema excessiveChildren;
	excessiveChildren.properties.n_children = 1'025;
	expectSchemaError(&excessiveChildren.root, ZYX_DRIVER_OUT_OF_RANGE);

	constexpr size_t nestedDepth = 66;
	ArrowSchema nestedLeaf = scalarSchema("l", "item");
	std::array<ArrowSchema, nestedDepth> nestedLists{};
	std::array<std::array<ArrowSchema *, 1>, nestedDepth> nestedListChildren{};
	ArrowSchema *nestedChild = &nestedLeaf;
	for (size_t reverseIndex = nestedDepth; reverseIndex > 0; --reverseIndex) {
		const size_t index = reverseIndex - 1;
		nestedListChildren[index][0] = nestedChild;
		nestedLists[index] = ArrowSchema{"+l",	 index == 0 ? "since" : "item",	   nullptr, 0,
										 1,		 nestedListChildren[index].data(), nullptr, nullptr,
										 nullptr};
		nestedChild = &nestedLists[index];
	}
	IngestEdgeSchema excessiveDepth;
	excessiveDepth.propertyFieldPointers[0] = &nestedLists[0];
	expectSchemaError(&excessiveDepth.root, ZYX_DRIVER_OUT_OF_RANGE);

	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
}

TEST_F(DriverAbiIngestTest, SchemaBudgetsRejectAdversarialInputsWithoutPoisoningSession) {
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);

	IngestEdgeSchema validSchema;
	std::vector<char> unterminatedType(64 * 1024, 'x');
	EXPECT_EQ(zyx_driver_ingest_prepare_edges(ingest, unterminatedType.data(), &validSchema.root, &ingestor, &error),
			  ZYX_DRIVER_OUT_OF_RANGE);
	EXPECT_STREQ(zyx_driver_error_field_path(error), "edge_type");
	freeError();

	constexpr size_t textFieldCount = 17;
	std::vector<std::string> longNames(textFieldCount, std::string((64 * 1024) - 1, 'n'));
	std::vector<ArrowSchema> textFields;
	std::vector<ArrowSchema *> textFieldPointers;
	textFields.reserve(textFieldCount);
	textFieldPointers.reserve(textFieldCount);
	for (size_t index = 0; index < textFieldCount; ++index) {
		longNames[index].back() = static_cast<char>('A' + index);
		textFields.push_back(scalarSchema("l", longNames[index].c_str()));
	}
	for (auto &field: textFields)
		textFieldPointers.push_back(&field);
	IngestEdgeSchema excessiveText;
	excessiveText.properties.n_children = static_cast<int64_t>(textFieldPointers.size());
	excessiveText.properties.children = textFieldPointers.data();
	EXPECT_EQ(zyx_driver_ingest_prepare_edges(ingest, "EXCESSIVE_TEXT_REL", &excessiveText.root, &ingestor, &error),
			  ZYX_DRIVER_OUT_OF_RANGE);
	freeError();

	constexpr size_t structCount = 5;
	constexpr size_t fieldsPerStruct = 1'024;
	std::vector<std::vector<std::string>> fieldNames(structCount);
	std::vector<std::vector<ArrowSchema>> fields(structCount);
	std::vector<std::vector<ArrowSchema *>> fieldPointers(structCount);
	std::array<std::string, structCount> structNames{};
	std::array<ArrowSchema, structCount> structs{};
	std::array<ArrowSchema *, structCount> structPointers{};
	for (size_t structIndex = 0; structIndex < structCount; ++structIndex) {
		fieldNames[structIndex].reserve(fieldsPerStruct);
		fields[structIndex].reserve(fieldsPerStruct);
		fieldPointers[structIndex].reserve(fieldsPerStruct);
		for (size_t fieldIndex = 0; fieldIndex < fieldsPerStruct; ++fieldIndex) {
			fieldNames[structIndex].push_back("f" + std::to_string(fieldIndex));
			fields[structIndex].push_back(scalarSchema("l", fieldNames[structIndex].back().c_str()));
		}
		for (auto &field: fields[structIndex])
			fieldPointers[structIndex].push_back(&field);
		structNames[structIndex] = "group" + std::to_string(structIndex);
		structs[structIndex] = ArrowSchema{"+s",
										   structNames[structIndex].c_str(),
										   nullptr,
										   0,
										   static_cast<int64_t>(fieldPointers[structIndex].size()),
										   fieldPointers[structIndex].data(),
										   nullptr,
										   nullptr,
										   nullptr};
		structPointers[structIndex] = &structs[structIndex];
	}
	IngestEdgeSchema excessiveNodes;
	excessiveNodes.properties.n_children = static_cast<int64_t>(structPointers.size());
	excessiveNodes.properties.children = structPointers.data();
	EXPECT_EQ(zyx_driver_ingest_prepare_edges(ingest, "EXCESSIVE_NODES_REL", &excessiveNodes.root, &ingestor, &error),
			  ZYX_DRIVER_OUT_OF_RANGE);
	freeError();

	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "VALID_AFTER_BUDGET_REL", &validSchema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
}

TEST_F(DriverAbiIngestTest, ArrayValidatorRejectsMalformedBuffersBeforeWriting) {
	const auto [source, target] = createEndpoints();
	static const uint8_t nullBitmap = 0;

	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.root.length = -1; }, ZYX_DRIVER_OUT_OF_RANGE,
			"record_batch");
	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.root.null_count = 2; }, ZYX_DRIVER_OUT_OF_RANGE,
			"record_batch");
	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.source.offset = (std::numeric_limits<int64_t>::max)(); },
			ZYX_DRIVER_OUT_OF_RANGE, "record_batch.source_id");
	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.root.n_children = 2; }, ZYX_DRIVER_INVALID_ARGUMENT,
			"record_batch");
	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.source.length = 0; }, ZYX_DRIVER_INVALID_ARGUMENT,
			"record_batch.source_id");
	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.source.n_buffers = 1; }, ZYX_DRIVER_INVALID_ARGUMENT,
			"record_batch.source_id");
	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.sourceBuffers[1] = nullptr; },
			ZYX_DRIVER_INVALID_ARGUMENT, "record_batch.source_id");
	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.nameBuffers[1] = nullptr; }, ZYX_DRIVER_INVALID_ARGUMENT,
			"record_batch.properties.name");
	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.nameBuffers[2] = nullptr; }, ZYX_DRIVER_INVALID_ARGUMENT,
			"record_batch.properties.name");
	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.name.dictionary = &batch.since; },
			ZYX_DRIVER_INVALID_ARGUMENT, "record_batch.properties.name");
	expectBatchError(
			source, target,
			[](IngestEdgeBatch &batch) {
				batch.name.null_count = 1;
				batch.nameBuffers[0] = nullptr;
			},
			ZYX_DRIVER_INVALID_ARGUMENT, "record_batch.properties.name");
	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.properties.n_children = 1; },
			ZYX_DRIVER_INVALID_ARGUMENT, "record_batch.properties");
	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.properties.children = nullptr; },
			ZYX_DRIVER_INVALID_ARGUMENT, "record_batch.properties");
	expectBatchError(
			source, target,
			[&](IngestEdgeBatch &batch) {
				batch.source.null_count = 1;
				batch.sourceBuffers[0] = &nullBitmap;
			},
			ZYX_DRIVER_INVALID_ARGUMENT, "source_id");
	expectBatchError(
			source, target,
			[&](IngestEdgeBatch &batch) {
				batch.target.null_count = 1;
				batch.targetBuffers[0] = &nullBitmap;
			},
			ZYX_DRIVER_INVALID_ARGUMENT, "target_id");
	expectBatchError(
			source, target,
			[&](IngestEdgeBatch &batch) {
				batch.root.null_count = 1;
				batch.rootStructBuffers[0] = &nullBitmap;
			},
			ZYX_DRIVER_INVALID_ARGUMENT, "record_batch");
	expectBatchError(
			source, target, [](IngestEdgeBatch &batch) { batch.nameOffsets[1] = (1 << 30) + 1; },
			ZYX_DRIVER_OUT_OF_RANGE, "record_batch.properties.name");

	IngestEdgeSchema schema;
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "NULL_BATCH_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_write(ingestor, nullptr, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
}

TEST_F(DriverAbiIngestTest, DecodeBudgetRejectsOversizedBatchBeforeReadingBuffers) {
	IngestEdgeSchema schema;
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "OVERSIZED_BATCH_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);

	ArrowArray oversized{10'000'000, 0, 0, 1, 0, nullptr, nullptr, nullptr, nullptr, nullptr};
	EXPECT_EQ(zyx_driver_edge_ingestor_write(ingestor, &oversized, nullptr, &error), ZYX_DRIVER_OUT_OF_RANGE);
	ASSERT_NE(error, nullptr);
	EXPECT_STREQ(zyx_driver_error_field_path(error), "record_batch");
	freeError();

	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
}

TEST_F(DriverAbiIngestTest, SlicedFixedSizeListAndNestedStructUsePhysicalOffsets) {
	const auto [source, target] = createEndpoints();

	ArrowSchema vectorItemSchema = scalarSchema("f", "item");
	std::array<ArrowSchema *, 1> vectorChildren{&vectorItemSchema};
	ArrowSchema vectorSchema{"+w:2", "embedding", nullptr, 0, 1, vectorChildren.data(), nullptr, nullptr, nullptr};
	ArrowSchema rankSchema = scalarSchema("l", "rank");
	std::array<ArrowSchema *, 1> detailChildren{&rankSchema};
	ArrowSchema detailSchema{"+s", "details", nullptr, 0, 1, detailChildren.data(), nullptr, nullptr, nullptr};
	std::array<ArrowSchema *, 2> propertySchemas{&vectorSchema, &detailSchema};
	ArrowSchema propertiesSchema{"+s", "properties", nullptr, 0, 2, propertySchemas.data(), nullptr, nullptr, nullptr};
	ArrowSchema sourceSchema = scalarSchema("l", "source_id");
	ArrowSchema targetSchema = scalarSchema("l", "target_id");
	std::array<ArrowSchema *, 3> rootSchemas{&sourceSchema, &targetSchema, &propertiesSchema};
	ArrowSchema rootSchema{"+s", "edges", nullptr, 0, 3, rootSchemas.data(), nullptr, nullptr, nullptr};

	const std::array<int64_t, 1> sourceIds{source};
	const std::array<int64_t, 1> targetIds{target};
	const std::array<float, 4> vectors{1.0F, 2.0F, 3.0F, 4.0F};
	const std::array<int64_t, 1> ranks{7};
	std::array<const void *, 2> sourceBuffers{nullptr, sourceIds.data()};
	std::array<const void *, 2> targetBuffers{nullptr, targetIds.data()};
	std::array<const void *, 2> vectorBuffers{nullptr, vectors.data()};
	std::array<const void *, 2> rankBuffers{nullptr, ranks.data()};
	std::array<const void *, 1> structBuffers{nullptr};
	ArrowArray sourceArray{1, 0, 0, 2, 0, sourceBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray targetArray{1, 0, 0, 2, 0, targetBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray vectorItems{4, 0, 0, 2, 0, vectorBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 1> vectorArrayChildren{&vectorItems};
	ArrowArray vectorArray{1, 0, 1, 1, 1, structBuffers.data(), vectorArrayChildren.data(), nullptr, nullptr, nullptr};
	ArrowArray rankArray{1, 0, 0, 2, 0, rankBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 1> detailArrayChildren{&rankArray};
	ArrowArray detailArray{1, 0, 0, 1, 1, structBuffers.data(), detailArrayChildren.data(), nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 2> propertyArrays{&vectorArray, &detailArray};
	ArrowArray propertiesArray{1, 0, 0, 1, 2, structBuffers.data(), propertyArrays.data(), nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 3> rootArrays{&sourceArray, &targetArray, &propertiesArray};
	ArrowArray rootArray{1, 0, 0, 1, 3, structBuffers.data(), rootArrays.data(), nullptr, nullptr, nullptr};

	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "SLICED_FIXED_REL", &rootSchema, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &rootArray, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);

	zyx_driver_result_t *result = nullptr;
	ASSERT_EQ(zyx_driver_db_execute(db, "MATCH ()-[e:SLICED_FIXED_REL]->() RETURN e.embedding, e.details", nullptr,
									&result, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	double second = 0.0;
	ASSERT_EQ(zyx_driver_result_get_list_double(result, 0, 1, &second, &error), ZYX_DRIVER_OK);
	EXPECT_DOUBLE_EQ(second, 4.0);
	zyx_driver_value_ref_t details{};
	ASSERT_EQ(zyx_driver_result_get_value(result, 1, &details, &error), ZYX_DRIVER_OK);
	int64_t rank = 0;
	EXPECT_EQ(zyx_driver_value_ref_map_get_int64(&details, "rank", &rank, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(rank, 7);
	zyx_driver_result_free(result);
}

TEST_F(DriverAbiIngestTest, DuplicateMapKeysAreRejectedWithoutSilentDataLoss) {
	const auto [source, target] = createEndpoints();

	ArrowSchema keySchema = scalarSchema("u", "key");
	ArrowSchema valueSchema = scalarSchema("l", "value");
	std::array<ArrowSchema *, 2> entrySchemas{&keySchema, &valueSchema};
	ArrowSchema entriesSchema{"+s", "entries", nullptr, 0, 2, entrySchemas.data(), nullptr, nullptr, nullptr};
	std::array<ArrowSchema *, 1> mapChildren{&entriesSchema};
	ArrowSchema mapSchema{"+m", "metadata", nullptr, 0, 1, mapChildren.data(), nullptr, nullptr, nullptr};
	std::array<ArrowSchema *, 1> propertySchemas{&mapSchema};
	ArrowSchema propertiesSchema{"+s", "properties", nullptr, 0, 1, propertySchemas.data(), nullptr, nullptr, nullptr};
	ArrowSchema sourceSchema = scalarSchema("l", "source_id");
	ArrowSchema targetSchema = scalarSchema("l", "target_id");
	std::array<ArrowSchema *, 3> rootSchemas{&sourceSchema, &targetSchema, &propertiesSchema};
	ArrowSchema rootSchema{"+s", "edges", nullptr, 0, 3, rootSchemas.data(), nullptr, nullptr, nullptr};

	const std::array<int64_t, 1> sourceIds{source};
	const std::array<int64_t, 1> targetIds{target};
	const std::array<int32_t, 2> mapOffsets{0, 2};
	const std::array<int32_t, 3> keyOffsets{0, 1, 2};
	const std::string keyBytes = "xx";
	const std::array<int64_t, 2> values{1, 2};
	std::array<const void *, 2> sourceBuffers{nullptr, sourceIds.data()};
	std::array<const void *, 2> targetBuffers{nullptr, targetIds.data()};
	std::array<const void *, 2> mapBuffers{nullptr, mapOffsets.data()};
	std::array<const void *, 3> keyBuffers{nullptr, keyOffsets.data(), keyBytes.data()};
	std::array<const void *, 2> valueBuffers{nullptr, values.data()};
	std::array<const void *, 1> structBuffers{nullptr};
	ArrowArray sourceArray{1, 0, 0, 2, 0, sourceBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray targetArray{1, 0, 0, 2, 0, targetBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray keyArray{2, 0, 0, 3, 0, keyBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	ArrowArray valueArray{2, 0, 0, 2, 0, valueBuffers.data(), nullptr, nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 2> entryArrays{&keyArray, &valueArray};
	ArrowArray entriesArray{2, 0, 0, 1, 2, structBuffers.data(), entryArrays.data(), nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 1> mapArrays{&entriesArray};
	ArrowArray mapArray{1, 0, 0, 2, 1, mapBuffers.data(), mapArrays.data(), nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 1> propertyArrays{&mapArray};
	ArrowArray propertiesArray{1, 0, 0, 1, 1, structBuffers.data(), propertyArrays.data(), nullptr, nullptr, nullptr};
	std::array<ArrowArray *, 3> rootArrays{&sourceArray, &targetArray, &propertiesArray};
	ArrowArray rootArray{1, 0, 0, 1, 3, structBuffers.data(), rootArrays.data(), nullptr, nullptr, nullptr};

	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "DUPLICATE_MAP_REL", &rootSchema, &ingestor, &error),
			  ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_write(ingestor, &rootArray, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_row_index(error), 0);
	EXPECT_STREQ(zyx_driver_error_field_path(error), "properties.metadata.key");
	freeError();
	EXPECT_EQ(zyx_driver_ingest_rollback(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
}

TEST_F(DriverAbiIngestTest, NullPropertiesStructProducesNullPropertyValues) {
	const auto [source, target] = createEndpoints();
	static const uint8_t nullBitmap = 0;
	IngestEdgeSchema schema;
	IngestEdgeBatch batch({source}, {target}, {2026}, {"ignored"});
	batch.properties.null_count = 1;
	batch.propertyStructBuffers[0] = &nullBitmap;
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;

	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "NULL_PROPERTIES_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);

	zyx_driver_result_t *result = nullptr;
	ASSERT_EQ(zyx_driver_db_execute(db, "MATCH ()-[e:NULL_PROPERTIES_REL]->() RETURN e.since, e.name", nullptr, &result,
									&error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	EXPECT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_NULL);
	EXPECT_EQ(zyx_driver_result_value_type(result, 1), ZYX_DRIVER_VALUE_NULL);
	zyx_driver_result_free(result);
}
