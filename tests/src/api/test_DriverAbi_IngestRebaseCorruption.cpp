/**
 * @file test_DriverAbi_IngestRebaseCorruption.cpp
 * @brief Public Driver ABI coverage for rebased streamed-ingest source corruption.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "zyx/zyx_driver_abi.h"

namespace fs = std::filesystem;
using namespace graph::storage;

namespace {

	constexpr size_t MANIFEST_RECORD_COUNT_OFFSET = 16U;
	constexpr size_t MANIFEST_LEVEL_OFFSET = 10U;
	constexpr size_t MANIFEST_REFERENCE_COUNT_OFFSET = 12U;
	constexpr size_t MANIFEST_REFERENCES_CRC_OFFSET = 56U;
	constexpr size_t MANIFEST_HEADER_CRC_OFFSET = 60U;
	constexpr size_t MANIFEST_REFERENCE_BYTES = 16U;
	constexpr size_t RUN_RECORD_COUNT_OFFSET = 12U;
	constexpr size_t RUN_RECORD_BYTES_OFFSET = 16U;
	constexpr size_t RUN_RECORDS_CRC_OFFSET = 20U;
	constexpr size_t RUN_HEADER_CRC_OFFSET = 32U;
	constexpr size_t RUN_FRAMED_RECORD_HEADER_BYTES = 8U;

	std::string uniqueDatabasePath() {
		static std::atomic<uint64_t> counter{0U};
		const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
		return (fs::temp_directory_path() / ("zyx_driver_abi_ingest_rebase_corruption_" + std::to_string(now) + "_" +
															std::to_string(counter.fetch_add(1U, std::memory_order_relaxed))))
				.string();
	}

	void materializeEmptyRelocatableDatabase(const std::string &path) {
		auto stream = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(stream->is_open());
		const file_handle_t writeFd = portable_open_rw(path.c_str());
		const file_handle_t readFd = portable_open(path.c_str(), O_RDONLY);
		ASSERT_NE(writeFd, INVALID_FILE_HANDLE);
		ASSERT_NE(readFd, INVALID_FILE_HANDLE);

		auto io = std::make_shared<StorageIO>(stream, writeFd, readFd);
		Superblock superblock;
		superblock.database_id = DatabaseId(0x1A2B3C4D5E6F7082ULL);
		superblock.generation = Generation(1U);
		superblock.checkpoint_lsn = LogSequenceNumber(1U);
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock);
		metadata.reset();
		io->sync();
		io.reset();
		EXPECT_EQ(portable_close_rw(writeFd), 0);
		EXPECT_EQ(portable_close(readFd), 0);
		stream->close();
	}

	ArrowSchema scalarSchema(const char *format, const char *name) {
		return ArrowSchema{format, name, nullptr, ARROW_FLAG_NULLABLE, 0, nullptr, nullptr, nullptr, nullptr};
	}

	struct EdgeSchema {
		ArrowSchema source = scalarSchema("l", "source_id");
		ArrowSchema target = scalarSchema("l", "target_id");
		ArrowSchema properties{ "+s", "properties", nullptr, ARROW_FLAG_NULLABLE, 0, nullptr, nullptr, nullptr, nullptr };
		std::array<ArrowSchema *, 3> rootFields{&source, &target, &properties};
		ArrowSchema root{"+s", "edges", nullptr, 0, static_cast<int64_t>(rootFields.size()), rootFields.data(), nullptr,
						 nullptr, nullptr};
	};

	struct EdgeBatch {
		EdgeBatch(std::vector<int64_t> sources, std::vector<int64_t> targets) : sources(std::move(sources)), targets(std::move(targets)) {
			const int64_t rows = static_cast<int64_t>(this->sources.size());
			sourceBuffers = {nullptr, this->sources.data()};
			targetBuffers = {nullptr, this->targets.data()};
			structBuffers = {nullptr};
			source = ArrowArray{rows, 0, 0, 2, 0, sourceBuffers.data(), nullptr, nullptr, nullptr, nullptr};
			target = ArrowArray{rows, 0, 0, 2, 0, targetBuffers.data(), nullptr, nullptr, nullptr, nullptr};
			properties = ArrowArray{rows, 0, 0, 1, 0, structBuffers.data(), nullptr, nullptr, nullptr, nullptr};
			rootChildren = {&source, &target, &properties};
			root = ArrowArray{rows, 0, 0, 1, static_cast<int64_t>(rootChildren.size()), structBuffers.data(),
						  rootChildren.data(), nullptr, nullptr, nullptr};
		}

		std::vector<int64_t> sources;
		std::vector<int64_t> targets;
		std::array<const void *, 2> sourceBuffers{};
		std::array<const void *, 2> targetBuffers{};
		std::array<const void *, 1> structBuffers{};
		ArrowArray source{};
		ArrowArray target{};
		ArrowArray properties{};
		std::array<ArrowArray *, 3> rootChildren{};
		ArrowArray root{};
	};

	void resealManifestHeader(std::span<std::byte> manifest) {
		LittleEndianCodec::writeU32(manifest, MANIFEST_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(
				manifest, MANIFEST_HEADER_CRC_OFFSET,
				graph::utils::calculateCrc(manifest.data(), RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES));
	}

	void resealSegmentPayload(SegmentEnvelope envelope, std::span<const std::byte> payload,
			std::span<std::byte> segment) {
		envelope.payload_crc = graph::utils::calculateCrc(payload.data(), payload.size());
		const auto resealed = SegmentEnvelopeCodec::encode(envelope);
		std::copy(resealed.begin(), resealed.end(), segment.begin());
	}

	bool swapFirstTwoManifestReferences(std::span<std::byte> manifest) {
		if (manifest.size() < RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES ||
			LittleEndianCodec::readU16(manifest, MANIFEST_LEVEL_OFFSET) != 0U ||
			LittleEndianCodec::readU32(manifest, MANIFEST_REFERENCE_COUNT_OFFSET) < 2U) {
			return false;
		}
		const size_t referenceBytes =
				static_cast<size_t>(LittleEndianCodec::readU32(manifest, MANIFEST_REFERENCE_COUNT_OFFSET)) *
				MANIFEST_REFERENCE_BYTES;
		if (referenceBytes > manifest.size() - RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES) {
			return false;
		}
		auto references = manifest.subspan(RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES, referenceBytes);
		std::array<std::byte, MANIFEST_REFERENCE_BYTES> first{};
		std::copy_n(references.begin(), first.size(), first.begin());
		std::copy_n(references.begin() + static_cast<std::ptrdiff_t>(MANIFEST_REFERENCE_BYTES), first.size(),
					references.begin());
		std::copy(first.begin(), first.end(), references.begin() + static_cast<std::ptrdiff_t>(MANIFEST_REFERENCE_BYTES));
		LittleEndianCodec::writeU32(manifest, MANIFEST_REFERENCES_CRC_OFFSET,
				graph::utils::calculateCrc(references.data(), references.size()));
		resealManifestHeader(manifest);
		return true;
	}

	bool swapFirstTwoRunRecords(std::span<std::byte> run) {
		if (run.size() < RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES ||
			LittleEndianCodec::readU32(run, RUN_RECORD_COUNT_OFFSET) < 2U) {
			return false;
		}
		const size_t recordBytes = LittleEndianCodec::readU32(run, RUN_RECORD_BYTES_OFFSET);
		if (recordBytes > run.size() - RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES ||
			recordBytes < RUN_FRAMED_RECORD_HEADER_BYTES) {
			return false;
		}
		auto records = run.subspan(RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES, recordBytes);
		const size_t firstBytes = RUN_FRAMED_RECORD_HEADER_BYTES + LittleEndianCodec::readU32(records, 0U);
		if (firstBytes > records.size() || records.size() - firstBytes < RUN_FRAMED_RECORD_HEADER_BYTES) {
			return false;
		}
		const size_t secondBytes = RUN_FRAMED_RECORD_HEADER_BYTES + LittleEndianCodec::readU32(records, firstBytes);
		if (secondBytes > records.size() - firstBytes) {
			return false;
		}

		const std::vector<std::byte> first(records.begin(), records.begin() + static_cast<std::ptrdiff_t>(firstBytes));
		const std::vector<std::byte> second(records.begin() + static_cast<std::ptrdiff_t>(firstBytes),
				records.begin() + static_cast<std::ptrdiff_t>(firstBytes + secondBytes));
		std::copy(second.begin(), second.end(), records.begin());
		std::copy(first.begin(), first.end(), records.begin() + static_cast<std::ptrdiff_t>(secondBytes));
		LittleEndianCodec::writeU32(run, RUN_RECORDS_CRC_OFFSET,
				graph::utils::calculateCrc(records.data(), records.size()));
		LittleEndianCodec::writeU32(run, RUN_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(run, RUN_HEADER_CRC_OFFSET,
				graph::utils::calculateCrc(run.data(), RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES));
		return true;
	}

} // namespace

class RelocatableDriverAbiIngestRebaseCorruptionTest : public ::testing::Test {
protected:
	std::string databasePath;
	zyx_driver_db_t *database = nullptr;
	zyx_driver_error_t *error = nullptr;

	void SetUp() override {
		databasePath = uniqueDatabasePath();
		cleanup();
		materializeEmptyRelocatableDatabase(databasePath);
		ASSERT_EQ(zyx_driver_db_open(databasePath.c_str(), &database, &error), ZYX_DRIVER_OK);
		ASSERT_NE(database, nullptr);
	}

	void TearDown() override {
		if (database != nullptr) {
			EXPECT_EQ(zyx_driver_db_close(database, &error), ZYX_DRIVER_OK);
			database = nullptr;
		}
		freeError();
		cleanup();
	}

	void freeError() {
		zyx_driver_error_free(error);
		error = nullptr;
	}

	void cleanup() {
		std::error_code ignored;
		fs::remove(databasePath, ignored);
	}

	std::pair<int64_t, int64_t> createEndpoints() {
		int64_t source = 0;
		int64_t target = 0;
		EXPECT_EQ(zyx_driver_db_create_node(database, "RebaseEndpoint", nullptr, &source, &error), ZYX_DRIVER_OK);
		EXPECT_EQ(zyx_driver_db_create_node(database, "RebaseEndpoint", nullptr, &target, &error), ZYX_DRIVER_OK);
		return {source, target};
	}

	int64_t countEdges(const char *type) {
		zyx_driver_result_t *result = nullptr;
		const std::string query = "MATCH ()-[e:" + std::string(type) + "]->() RETURN count(e)";
		if (zyx_driver_db_execute(database, query.c_str(), nullptr, &result, &error) != ZYX_DRIVER_OK) {
			ADD_FAILURE() << "count edge query failed";
			return -1;
		}
		int64_t count = -1;
		EXPECT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
		EXPECT_EQ(zyx_driver_result_get_int64(result, 0, &count, &error), ZYX_DRIVER_OK);
		zyx_driver_result_free(result);
		return count;
	}
};

// Rebase corruption cases intentionally cross sealed scratch-run boundaries.
// Keep them available as an explicit scale gate instead of running the
// 1,024/4,096-Edge fault workloads in every quick unit-test invocation.
class DISABLED_RelocatableDriverAbiIngestRebaseCorruptionScaleTest
		: public RelocatableDriverAbiIngestRebaseCorruptionTest {};

TEST_F(DISABLED_RelocatableDriverAbiIngestRebaseCorruptionScaleTest,
		AuthenticatedManifestCountDivergenceAfterRebasePoisonsSessionAndPreservesExternalGraph) {
	const auto [source, target] = createEndpoints();
	EdgeSchema schema;
	constexpr size_t stagedEdgeCount = 1024U;
	EdgeBatch batch(std::vector<int64_t>(stagedEdgeCount, source), std::vector<int64_t>(stagedEdgeCount, target));
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(database, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_REBASE_CORRUPTED_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);

	zyx_driver_db_t *concurrentWriter = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(databasePath.c_str(), &concurrentWriter, &error), ZYX_DRIVER_OK);
	int64_t externalEdge = 0;
	ASSERT_EQ(zyx_driver_db_create_edge(concurrentWriter, source, target, "ABI_REBASE_EXTERNAL_REL", nullptr,
										&externalEdge, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_db_close(concurrentWriter, &error), ZYX_DRIVER_OK);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeReadHookForTesting corruptRebasedManifestCount(
				[&injected](file_handle_t fd, void *bytes, size_t byteCount, pwrite_off_t offset) {
					const pwrite_ssize_t read = portable_pread(fd, bytes, byteCount, offset);
					if (read != static_cast<pwrite_ssize_t>(RSS_SEGMENT_SIZE) || injected.load(std::memory_order_relaxed)) {
						return read;
					}
					try {
						auto segment = std::span<std::byte>(static_cast<std::byte *>(bytes), byteCount);
						const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
						if (envelope.kind != SegmentKind::SGK_BULK_SCRATCH ||
							envelope.header_bytes + RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES > segment.size()) {
							return read;
						}
						auto manifest = segment.subspan(envelope.header_bytes, RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES);
						const uint64_t recordCount = LittleEndianCodec::readU64(manifest, MANIFEST_RECORD_COUNT_OFFSET);
						if (LittleEndianCodec::readU32(manifest, 0U) != RSS_NATIVE_BULK_EDGE_MANIFEST_MAGIC ||
							recordCount < 2U) {
							return read;
						}
						// Preserve the outer checksum so publication reaches the semantic count check.
						LittleEndianCodec::writeU64(manifest, MANIFEST_RECORD_COUNT_OFFSET, recordCount - 1U);
						resealManifestHeader(manifest);
						injected.store(true, std::memory_order_relaxed);
					} catch (const FormatException &) {
						// Ignore non-scratch reads while locating the sealed source manifest.
					}
					return read;
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_EXECUTION_ERROR);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_EXECUTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;

	ASSERT_EQ(zyx_driver_db_close(database, &error), ZYX_DRIVER_OK);
	database = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(databasePath.c_str(), &database, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("ABI_REBASE_EXTERNAL_REL"), 1);
	EXPECT_EQ(countEdges("ABI_REBASE_CORRUPTED_REL"), 0);
	EXPECT_FALSE(fs::exists(databasePath + ".wal"));
	EXPECT_FALSE(fs::exists(databasePath + "-wal"));

	EdgeBatch retryBatch({source}, {target});
	ASSERT_EQ(zyx_driver_ingest_begin(database, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_REBASE_CORRUPTED_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &retryBatch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("ABI_REBASE_CORRUPTED_REL"), 1);
}

TEST_F(DISABLED_RelocatableDriverAbiIngestRebaseCorruptionScaleTest,
		AuthenticatedRunOrderingDivergenceAfterRebasePoisonsSessionAndPreservesExternalGraph) {
	const auto [source, target] = createEndpoints();
	EdgeSchema schema;
	constexpr size_t stagedEdgeCount = 1024U;
	EdgeBatch batch(std::vector<int64_t>(stagedEdgeCount, source), std::vector<int64_t>(stagedEdgeCount, target));
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(database, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_REBASE_UNORDERED_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);

	zyx_driver_db_t *concurrentWriter = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(databasePath.c_str(), &concurrentWriter, &error), ZYX_DRIVER_OK);
	int64_t externalEdge = 0;
	ASSERT_EQ(zyx_driver_db_create_edge(concurrentWriter, source, target, "ABI_REBASE_ORDER_EXTERNAL_REL", nullptr,
										&externalEdge, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_db_close(concurrentWriter, &error), ZYX_DRIVER_OK);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeReadHookForTesting reorderRebasedRun(
				[&injected](file_handle_t fd, void *bytes, size_t byteCount, pwrite_off_t offset) {
					const pwrite_ssize_t read = portable_pread(fd, bytes, byteCount, offset);
					if (read != static_cast<pwrite_ssize_t>(RSS_SEGMENT_SIZE) || injected.load(std::memory_order_relaxed)) {
						return read;
					}
					try {
						auto segment = std::span<std::byte>(static_cast<std::byte *>(bytes), byteCount);
						const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
						if (envelope.kind != SegmentKind::SGK_BULK_SCRATCH ||
							envelope.header_bytes > segment.size() ||
							envelope.payload_bytes > segment.size() - envelope.header_bytes ||
							envelope.payload_bytes < RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES) {
							return read;
						}
						auto payload = segment.subspan(envelope.header_bytes, envelope.payload_bytes);
						if (LittleEndianCodec::readU32(payload, 0U) == RSS_NATIVE_BULK_EDGE_RUN_MAGIC &&
							swapFirstTwoRunRecords(payload)) {
							resealSegmentPayload(envelope, payload, segment);
							injected.store(true, std::memory_order_relaxed);
						}
					} catch (const FormatException &) {
						// Ignore non-scratch reads while locating the sealed source run.
					}
					return read;
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_EXECUTION_ERROR);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_EXECUTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;

	ASSERT_EQ(zyx_driver_db_close(database, &error), ZYX_DRIVER_OK);
	database = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(databasePath.c_str(), &database, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("ABI_REBASE_ORDER_EXTERNAL_REL"), 1);
	EXPECT_EQ(countEdges("ABI_REBASE_UNORDERED_REL"), 0);

	EdgeBatch retryBatch({source}, {target});
	ASSERT_EQ(zyx_driver_ingest_begin(database, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_REBASE_UNORDERED_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &retryBatch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("ABI_REBASE_UNORDERED_REL"), 1);
}

TEST_F(DISABLED_RelocatableDriverAbiIngestRebaseCorruptionScaleTest,
		AuthenticatedCrossRunOrderingDivergenceAfterRebasePoisonsSessionAndPreservesExternalGraph) {
	const auto [source, target] = createEndpoints();
	EdgeSchema schema;
	// The public ABI cannot tune private run bounds. This workload exceeds the
	// fixed 128 KiB scratch-run capacity and therefore creates multiple runs.
	constexpr size_t stagedEdgeCount = 4'096U;
	EdgeBatch batch(std::vector<int64_t>(stagedEdgeCount, source), std::vector<int64_t>(stagedEdgeCount, target));
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(database, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_REBASE_CROSS_RUN_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);

	zyx_driver_db_t *concurrentWriter = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(databasePath.c_str(), &concurrentWriter, &error), ZYX_DRIVER_OK);
	int64_t externalEdge = 0;
	ASSERT_EQ(zyx_driver_db_create_edge(concurrentWriter, source, target, "ABI_REBASE_CROSS_RUN_EXTERNAL_REL", nullptr,
										&externalEdge, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_db_close(concurrentWriter, &error), ZYX_DRIVER_OK);

	std::atomic<bool> injected{false};
	std::atomic<uint32_t> rootManifestReads{0U};
	{
		StorageIO::ScopedNativeReadHookForTesting reorderRebasedManifestRuns(
				[&injected, &rootManifestReads](file_handle_t fd, void *bytes, size_t byteCount, pwrite_off_t offset) {
					const pwrite_ssize_t read = portable_pread(fd, bytes, byteCount, offset);
					if (read != static_cast<pwrite_ssize_t>(RSS_SEGMENT_SIZE) || injected.load(std::memory_order_relaxed)) {
						return read;
					}
					try {
						auto segment = std::span<std::byte>(static_cast<std::byte *>(bytes), byteCount);
						const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
						if (envelope.kind != SegmentKind::SGK_BULK_SCRATCH ||
							envelope.header_bytes > segment.size() ||
							envelope.payload_bytes > segment.size() - envelope.header_bytes ||
							envelope.payload_bytes < RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES) {
							return read;
						}
						auto payload = segment.subspan(envelope.header_bytes, envelope.payload_bytes);
						if (LittleEndianCodec::readU32(payload, 0U) == RSS_NATIVE_BULK_EDGE_MANIFEST_MAGIC &&
							rootManifestReads.fetch_add(1U, std::memory_order_relaxed) + 1U == 2U &&
							swapFirstTwoManifestReferences(payload)) {
							resealSegmentPayload(envelope, payload, segment);
							injected.store(true, std::memory_order_relaxed);
						}
					} catch (const FormatException &) {
						// Ignore non-scratch reads while locating the sealed source manifest.
					}
					return read;
				});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_EXECUTION_ERROR);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_GE(rootManifestReads.load(std::memory_order_relaxed), 2U);
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_EXECUTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;

	ASSERT_EQ(zyx_driver_db_close(database, &error), ZYX_DRIVER_OK);
	database = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(databasePath.c_str(), &database, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("ABI_REBASE_CROSS_RUN_EXTERNAL_REL"), 1);
	EXPECT_EQ(countEdges("ABI_REBASE_CROSS_RUN_REL"), 0);
	EXPECT_FALSE(fs::exists(databasePath + ".wal"));
	EXPECT_FALSE(fs::exists(databasePath + "-wal"));

	EdgeBatch retryBatch({source}, {target});
	ASSERT_EQ(zyx_driver_ingest_begin(database, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_REBASE_CROSS_RUN_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &retryBatch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("ABI_REBASE_CROSS_RUN_REL"), 1);
}

TEST_F(RelocatableDriverAbiIngestRebaseCorruptionTest,
		PostPublicationScratchCleanupFailureKeepsCommitVisibleUntilReopen) {
	const auto [source, target] = createEndpoints();
	EdgeSchema schema;
	EdgeBatch batch({source}, {target});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(database, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_CLEANUP_FAILURE_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);

	std::atomic<bool> graphPublicationWritten{false};
	std::atomic<bool> cleanupWriteFailed{false};
	{
		StorageIO::ScopedNativeWriteHookForTesting failPostPublicationCleanupPage(
				[&graphPublicationWritten, &cleanupWriteFailed](file_handle_t fd, const void *bytes, size_t byteCount,
																				pwrite_off_t offset) {
			if (byteCount == RSS_PAGE_SIZE &&
				(offset == SuperblockFile::slotOffset(0U) || offset == SuperblockFile::slotOffset(1U))) {
				try {
					const Superblock candidate = SuperblockCodec::decode(
							std::span<const std::byte>(static_cast<const std::byte *>(bytes), byteCount));
					if (candidate.edge_entity_directory_root.isValid()) {
						graphPublicationWritten.store(true, std::memory_order_relaxed);
					}
				} catch (const FormatException &) {
					// Only an authenticated graph-publication Superblock arms cleanup fault injection.
				}
			} else if (byteCount == RSS_PAGE_SIZE && graphPublicationWritten.load(std::memory_order_relaxed) &&
					   !cleanupWriteFailed.exchange(true, std::memory_order_relaxed)) {
#ifdef _WIN32
				SetLastError(ERROR_WRITE_FAULT);
#else
				errno = EIO;
#endif
				return static_cast<pwrite_ssize_t>(-1);
			}
			return portable_pwrite(fd, bytes, byteCount, offset);
		});
		EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	}
	EXPECT_TRUE(graphPublicationWritten.load(std::memory_order_relaxed));
	EXPECT_TRUE(cleanupWriteFailed.load(std::memory_order_relaxed));
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;

	EXPECT_EQ(countEdges("ABI_CLEANUP_FAILURE_REL"), 1);
	int64_t blockedNode = 0;
	EXPECT_EQ(zyx_driver_db_create_node(database, "MustReopenAfterCleanupFailure", nullptr, &blockedNode, &error),
			  ZYX_DRIVER_EXECUTION_ERROR);
	freeError();
	ASSERT_EQ(zyx_driver_db_close(database, &error), ZYX_DRIVER_OK);
	database = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(databasePath.c_str(), &database, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("ABI_CLEANUP_FAILURE_REL"), 1);
	int64_t writableNode = 0;
	EXPECT_EQ(zyx_driver_db_create_node(database, "WritableAfterCleanupReopen", nullptr, &writableNode, &error),
			  ZYX_DRIVER_OK);
	EXPECT_GT(writableNode, target);
}

TEST_F(RelocatableDriverAbiIngestRebaseCorruptionTest,
		StreamedPublicationConflictRollsBackAndReleasesScratch) {
	const auto [source, target] = createEndpoints();
	EdgeSchema schema;
	EdgeBatch batch({source}, {target});
	zyx_driver_ingest_t *ingest = nullptr;
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(database, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_PUBLIC_CONFLICT_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &batch.root, nullptr, &error), ZYX_DRIVER_OK);

	// Delete the staged target through a separate public handle so commit observes a real graph conflict.
	zyx_driver_db_t *concurrentWriter = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(databasePath.c_str(), &concurrentWriter, &error), ZYX_DRIVER_OK);
	const std::string deleteQuery = "MATCH (n:RebaseEndpoint) WHERE id(n) = " + std::to_string(target) + " DELETE n";
	zyx_driver_result_t *deleteResult = nullptr;
	ASSERT_EQ(zyx_driver_db_execute(concurrentWriter, deleteQuery.c_str(), nullptr, &deleteResult, &error), ZYX_DRIVER_OK);
	zyx_driver_result_free(deleteResult);
	deleteResult = nullptr;
	ASSERT_EQ(zyx_driver_db_close(concurrentWriter, &error), ZYX_DRIVER_OK);
	concurrentWriter = nullptr;

	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_EXECUTION_ERROR);
	ASSERT_NE(error, nullptr);
	EXPECT_EQ(zyx_driver_error_code(error), ZYX_DRIVER_EXECUTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_TRANSACTION_ERROR);
	freeError();
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	ingestor = nullptr;
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	ingest = nullptr;

	EXPECT_EQ(countEdges("ABI_PUBLIC_CONFLICT_REL"), 0);
	EXPECT_FALSE(fs::exists(databasePath + ".wal"));
	EXPECT_FALSE(fs::exists(databasePath + "-wal"));

	ASSERT_EQ(zyx_driver_db_close(database, &error), ZYX_DRIVER_OK);
	database = nullptr;
	ASSERT_EQ(zyx_driver_db_open_if_exists(databasePath.c_str(), &database, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("ABI_PUBLIC_CONFLICT_REL"), 0);
	int64_t writableNode = 0;
	EXPECT_EQ(zyx_driver_db_create_node(database, "WritableAfterPublicConflict", nullptr, &writableNode, &error),
			  ZYX_DRIVER_OK);
	EXPECT_GT(writableNode, source);

	int64_t retryTarget = 0;
	ASSERT_EQ(zyx_driver_db_create_node(database, "RetryEndpoint", nullptr, &retryTarget, &error), ZYX_DRIVER_OK);
	EdgeBatch retryBatch({source}, {retryTarget});
	ASSERT_EQ(zyx_driver_ingest_begin(database, &ingest, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "ABI_PUBLIC_CONFLICT_RETRY_REL", &schema.root, &ingestor, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_edge_ingestor_write(ingestor, &retryBatch.root, nullptr, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_ingest_commit(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(countEdges("ABI_PUBLIC_CONFLICT_RETRY_REL"), 1);
}
