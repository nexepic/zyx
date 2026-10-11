/**
 * @file test_NativeBulkEdgeRunStore.cpp
 * @brief Bounded private Edge-run manifest regression tests.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected) << error.what();
		}
	}

	[[nodiscard]] Superblock superblock() {
		Superblock result;
		result.database_id = DatabaseId(0xD102030405060708ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	[[nodiscard]] NativeBulkEdgeRecord edge(uint64_t id) {
		NativeBulkEdgeRecord result;
		result.edge_id = id;
		result.source_node_id = id + 100U;
		result.target_node_id = id + 200U;
		result.type = "STREAM";
		result.properties = {{"rank", graph::PropertyValue(static_cast<int64_t>(id))}};
		return result;
	}

} // namespace

class NativeBulkEdgeRunStoreTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;
	std::unique_ptr<RelocatableMetadataStore> metadata_;
	std::unique_ptr<PhysicalSegmentAllocator> physical_;
	std::unique_ptr<NativeBulkRunStore> store_;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
				("native_bulk_edge_run_" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".dat");
		file_ = std::make_shared<std::fstream>(path_, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		metadata_ = RelocatableMetadataStore::initializeNew(io_, superblock());
		physical_ = std::make_unique<PhysicalSegmentAllocator>(io_, *metadata_);
		NativeBulkRunStoreOptions options{PhysicalSlotId(40U), PhysicalSlotId(120U), 512U, 2U, 4U};
		store_ = std::make_unique<NativeBulkRunStore>(*physical_, *metadata_, options);
	}

	void TearDown() override {
		store_.reset();
		physical_.reset();
		metadata_.reset();
		io_.reset();
		if (file_ && file_->is_open()) {
			file_->close();
		}
		file_.reset();
		std::error_code error;
		std::filesystem::remove(path_, error);
	}
};

TEST_F(NativeBulkEdgeRunStoreTest, StreamingWriterBuildsBoundedHierarchicalPrivateManifest) {
	NativeBulkEdgeRunStreamingWriter writer(*store_, 71U,
			{RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES + NativeBulkEdgeRecordCodec::encodedSize(edge(1U)) + 8U, 30U, 4U},
			LogSequenceNumber(2U));
	for (uint64_t id = 1U; id <= 30U; ++id) {
		writer.add(edge(id));
	}
	const NativeBulkEdgeRunManifest manifest = writer.finish();
	EXPECT_TRUE(manifest.isValid());
	EXPECT_EQ(manifest.record_count, 30U);
	EXPECT_EQ(manifest.first_edge_id, 1U);
	EXPECT_EQ(manifest.last_edge_id, 30U);
	EXPECT_GE(manifest.level, 1U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const NativeBulkEdgeRunManifest decoded = NativeBulkEdgeRunManifestCodec::decode(
			store_->readOpaquePayload(snapshot, manifest.location), manifest.location);
	EXPECT_EQ(decoded, manifest);
	// Thirty one-record runs are reserved in bounded pairs. Their fifteen batch
	// allocations plus three manifest allocations advance two generations each;
	// a per-run publisher would end at generation 67 instead of 37.
	EXPECT_EQ(snapshot.superblock().generation, Generation(37U));
	snapshot.release();
}

TEST_F(NativeBulkEdgeRunStoreTest, StreamingWriterRejectsOutOfOrderAndOversizedRecordsBeforeScratchPublication) {
	NativeBulkEdgeRunStreamingWriter writer(*store_, 71U,
			{RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES + NativeBulkEdgeRecordCodec::encodedSize(edge(1U)) + 8U, 3U, 4U},
			LogSequenceNumber(2U));
	writer.add(edge(2U));
	expectFormatError([&] { writer.add(edge(1U)); }, FormatError::FERR_INVALID_STATE);

	NativeBulkEdgeRunStreamingWriter tooSmall(*store_, 72U,
			{RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES + 1U, 3U, 4U}, LogSequenceNumber(2U));
	expectFormatError([&] { tooSmall.add(edge(1U)); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(NativeBulkEdgeRunStoreTest, RewindableSourceAndRecursiveReleasePreserveThenReclaimEveryPrivateRecord) {
	NativeBulkEdgeRunStreamingWriter writer(*store_, 71U,
			{RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES + NativeBulkEdgeRecordCodec::encodedSize(edge(1U)) + 8U, 12U, 4U},
			LogSequenceNumber(2U));
	for (uint64_t id = 1U; id <= 12U; ++id) {
		writer.add(edge(id));
	}
	const NativeBulkEdgeRunManifest manifest = writer.finish();
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const NativeBulkEdgeRecordSource source = nativeBulkEdgeRecordSource(
			manifest.location, 4U,
			[this, &snapshot](const NativeBulkRunFragmentLocation &location) {
				return store_->readOpaquePayload(snapshot, location);
			});
	for (size_t pass = 0U; pass < 2U; ++pass) {
		NativeBulkEdgeRecordCursor cursor = source();
		uint64_t expected = 1U;
		while (const std::optional<NativeBulkEdgeRecord> record = cursor()) {
			EXPECT_EQ(record->edge_id, expected++);
		}
		EXPECT_EQ(expected, 13U);
	}
	snapshot.release();

	releaseNativeBulkEdgeManifest(*store_, manifest.location, LogSequenceNumber(40U));
	snapshot = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findCatalog(snapshot, manifest.location.segment_id).state, SegmentState::SGS_OBSOLETE);
	snapshot.release();
}

TEST_F(NativeBulkEdgeRunStoreTest, RewindableSourceTraversesParentManifestAndRejectsAnInvalidSourceContract) {
	NativeBulkEdgeRunStreamingWriter writer(*store_, 71U,
			{RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES + NativeBulkEdgeRecordCodec::encodedSize(edge(1U)) + 8U, 30U, 4U},
			LogSequenceNumber(2U));
	for (uint64_t id = 1U; id <= 30U; ++id) {
		writer.add(edge(id));
	}
	const NativeBulkEdgeRunManifest manifest = writer.finish();
	ASSERT_GT(manifest.level, 0U);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const NativeBulkEdgeRecordSource source = nativeBulkEdgeRecordSource(
			manifest.location, 4U,
			[this, &snapshot](const NativeBulkRunFragmentLocation &location) {
				return store_->readOpaquePayload(snapshot, location);
			});
	NativeBulkEdgeRecordCursor cursor = source();
	uint64_t expected = 1U;
	while (const std::optional<NativeBulkEdgeRecord> record = cursor()) {
		EXPECT_EQ(record->edge_id, expected++);
	}
	EXPECT_EQ(expected, 31U);
	snapshot.release();

	releaseNativeBulkEdgeManifest(*store_, manifest.location, LogSequenceNumber(90U));
	snapshot = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findCatalog(snapshot, manifest.location.segment_id).state, SegmentState::SGS_OBSOLETE);
	snapshot.release();

	expectFormatError(
		[] { static_cast<void>(nativeBulkEdgeRecordSource({}, 4U, [](const auto &) { return std::vector<std::byte>{}; })); },
		FormatError::FERR_INVALID_HEADER);
}

TEST_F(NativeBulkEdgeRunStoreTest, PrivateCandidateReaderBuildsARewindableSourceWithoutAPublicScratchRoute) {
	const NativeGraphPrivateSegmentSet candidates(SegmentId(41U), Generation(3U), 3U, 71U);
	const NativeGraphPrivateSegmentMaterialization first = candidates[0U];
	const NativeGraphPrivateSegmentMaterialization second = candidates[1U];
	const NativeGraphPrivateSegmentMaterialization manifest = candidates[2U];
	const NativeBulkEdgeRun firstRun{{edge(1U)}};
	const NativeBulkEdgeRun secondRun{{edge(2U)}};
	NativeBulkEdgeRunManifest root;
	root.runs = {{first.segment_id, first.generation, first.owner_id},
				 {second.segment_id, second.generation, second.owner_id}};
	root.record_count = 2U;
	root.encoded_bytes = NativeBulkEdgeRunCodec::encodedSize(firstRun) + NativeBulkEdgeRunCodec::encodedSize(secondRun);
	root.first_edge_id = 1U;
	root.last_edge_id = 2U;
	const std::vector<std::byte> firstBytes = NativeBulkEdgeRunCodec::encode(firstRun);
	const std::vector<std::byte> secondBytes = NativeBulkEdgeRunCodec::encode(secondRun);
	const std::vector<std::byte> manifestBytes = NativeBulkEdgeRunManifestCodec::encode(root);
	const NativeGraphPrivateSegmentReader reader = [first, second, manifest, firstBytes, secondBytes, manifestBytes](
																					 const NativeGraphPrivateSegmentMaterialization &identity) {
		if (identity == first) {
			return firstBytes;
		}
		if (identity == second) {
			return secondBytes;
		}
		if (identity == manifest) {
			return manifestBytes;
		}
		throw FormatException(FormatError::FERR_INVALID_STATE, "test reader received a foreign private scratch identity");
	};

	const NativeBulkEdgeRecordSource source =
			nativeBulkEdgeRecordSourceFromPrivateReader(candidates, manifest, 1U, reader);
	for (size_t pass = 0U; pass < 2U; ++pass) {
		NativeBulkEdgeRecordCursor cursor = source();
		const std::optional<NativeBulkEdgeRecord> firstRecord = cursor();
		const std::optional<NativeBulkEdgeRecord> secondRecord = cursor();
		ASSERT_TRUE(firstRecord.has_value());
		ASSERT_TRUE(secondRecord.has_value());
		EXPECT_EQ(firstRecord->edge_id, 1U);
		EXPECT_EQ(secondRecord->edge_id, 2U);
		EXPECT_FALSE(cursor().has_value());
	}

	NativeGraphPrivateSegmentMaterialization foreign = manifest;
	++foreign.owner_id;
	EXPECT_THROW(static_cast<void>(nativeBulkEdgeRecordSourceFromPrivateReader(candidates, foreign, 1U, reader)),
			 FormatException);
}

TEST_F(NativeBulkEdgeRunStoreTest, AbortEagerlyReclaimsASealedStreamingHierarchy) {
	NativeBulkEdgeRunStreamingWriter writer(*store_, 71U,
			{RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES + NativeBulkEdgeRecordCodec::encodedSize(edge(1U)) + 8U, 12U, 4U},
			LogSequenceNumber(2U));
	for (uint64_t id = 1U; id <= 12U; ++id) {
		writer.add(edge(id));
	}
	writer.abort(LogSequenceNumber(40U));
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	bool foundLiveScratch = false;
	metadata_->visitCatalogEntries(snapshot, [&foundLiveScratch](const CatalogEntry &entry) {
		foundLiveScratch = foundLiveScratch ||
			(entry.kind == SegmentKind::SGK_BULK_SCRATCH && entry.state == SegmentState::SGS_SEALED);
	});
	EXPECT_FALSE(foundLiveScratch);
	snapshot.release();
	expectFormatError([&] { writer.abort(LogSequenceNumber(80U)); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(NativeBulkEdgeRunStoreTest, ReleaseRejectsAcyclicManifestHierarchyBeyondTheConfiguredDepth) {
	NativeBulkEdgeRunManifest malformed;
	malformed.level = store_->maximumManifestDepth();
	malformed.children = {{SegmentId(71U), Generation(1U), 71U}};
	malformed.record_count = 1U;
	malformed.encoded_bytes = RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES;
	malformed.first_edge_id = 1U;
	malformed.last_edge_id = 1U;
	const NativeBulkRunFragmentLocation root = store_->writeOpaquePayload(
			71U, NativeBulkEdgeRunManifestCodec::encode(malformed), LogSequenceNumber(20U));

	expectFormatError([&] { releaseNativeBulkEdgeManifest(*store_, root, LogSequenceNumber(30U)); },
			FormatError::FERR_INVALID_STATE);
}

TEST(NativeBulkEdgeRunManifestCodecTest, RejectsChecksumAndSelfReferenceCorruption) {
	NativeBulkEdgeRunManifest manifest;
	manifest.level = 0U;
	manifest.runs = {{SegmentId(9U), Generation(1U), 71U}};
	manifest.record_count = 1U;
	manifest.encoded_bytes = 88U;
	manifest.first_edge_id = 3U;
	manifest.last_edge_id = 3U;
	const std::vector<std::byte> encoded = NativeBulkEdgeRunManifestCodec::encode(manifest);
	const NativeBulkRunFragmentLocation location{SegmentId(10U), Generation(1U), 71U};
	EXPECT_EQ(NativeBulkEdgeRunManifestCodec::decode(encoded, location).record_count, 1U);

	auto corrupt = encoded;
	corrupt[60U] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunManifestCodec::decode(corrupt, location)); },
		FormatError::FERR_CHECKSUM_MISMATCH);

	auto selfReference = encoded;
	LittleEndianCodec::writeU64(selfReference, RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES, location.segment_id.value());
	LittleEndianCodec::writeU32(selfReference, 56U,
		graph::utils::calculateCrc(selfReference.data() + RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES,
			selfReference.size() - RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES));
	LittleEndianCodec::writeU32(selfReference, 60U, 0U);
	LittleEndianCodec::writeU32(selfReference, 60U,
		graph::utils::calculateCrc(selfReference.data(), RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES));
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunManifestCodec::decode(selfReference, location)); },
		FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkEdgeWireCodecTest, RejectsEverySingleBitCorruptionOfRunAndManifestWireImages) {
	const NativeBulkEdgeRun run{{edge(3U), edge(4U)}};
	const std::vector<std::byte> encodedRun = NativeBulkEdgeRunCodec::encode(run);
	for (size_t byte = 0U; byte < encodedRun.size(); ++byte) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			SCOPED_TRACE(::testing::Message() << "run byte=" << byte << ", bit=" << static_cast<unsigned>(bit));
			auto corrupt = encodedRun;
			corrupt[byte] ^= std::byte{static_cast<unsigned char>(1U << bit)};
			EXPECT_THROW(static_cast<void>(NativeBulkEdgeRunCodec::decode(corrupt)), FormatException);
		}
	}

	NativeBulkEdgeRunManifest manifest;
	manifest.runs = {{SegmentId(9U), Generation(1U), 71U}};
	manifest.record_count = run.records.size();
	manifest.encoded_bytes = NativeBulkEdgeRunCodec::encodedSize(run);
	manifest.first_edge_id = run.records.front().edge_id;
	manifest.last_edge_id = run.records.back().edge_id;
	const std::vector<std::byte> encodedManifest = NativeBulkEdgeRunManifestCodec::encode(manifest);
	const NativeBulkRunFragmentLocation location{SegmentId(10U), Generation(1U), 71U};
	for (size_t byte = 0U; byte < encodedManifest.size(); ++byte) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			SCOPED_TRACE(::testing::Message() << "manifest byte=" << byte << ", bit="
										  << static_cast<unsigned>(bit));
			auto corrupt = encodedManifest;
			corrupt[byte] ^= std::byte{static_cast<unsigned char>(1U << bit)};
			EXPECT_THROW(static_cast<void>(NativeBulkEdgeRunManifestCodec::decode(corrupt, location)), FormatException);
		}
	}
}

TEST(NativeBulkEdgeRunManifestCodecTest, RewindableSourceRejectsAggregateDivergenceAfterValidatingDescendants) {
	const NativeBulkRunFragmentLocation runLocation{SegmentId(9U), Generation(1U), 71U};
	const NativeBulkRunFragmentLocation manifestLocation{SegmentId(10U), Generation(1U), 71U};
	const NativeBulkEdgeRun run{{edge(3U)}};
	const std::vector<std::byte> runBytes = NativeBulkEdgeRunCodec::encode(run);

	NativeBulkEdgeRunManifest manifest;
	manifest.runs = {runLocation};
	manifest.record_count = 1U;
	manifest.encoded_bytes = NativeBulkEdgeRunCodec::encodedSize(run) + 1U;
	manifest.first_edge_id = 3U;
	manifest.last_edge_id = 3U;
	const std::vector<std::byte> manifestBytes = NativeBulkEdgeRunManifestCodec::encode(manifest);

	const NativeBulkEdgeRecordSource source = nativeBulkEdgeRecordSource(
			manifestLocation, 1U,
			[runLocation, manifestLocation, runBytes, manifestBytes](const NativeBulkRunFragmentLocation &location) {
				if (location == runLocation) {
					return runBytes;
				}
				if (location == manifestLocation) {
					return manifestBytes;
				}
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "test reader received a foreign native bulk Edge scratch location");
			});
	NativeBulkEdgeRecordCursor cursor = source();
	ASSERT_TRUE(cursor().has_value());
	EXPECT_THROW(static_cast<void>(cursor()), FormatException);
}

TEST(NativeBulkEdgeRunManifestCodecTest, RewindableSourceRejectsDivergentChildManifestAggregate) {
	const NativeBulkRunFragmentLocation runLocation{SegmentId(9U), Generation(1U), 71U};
	const NativeBulkRunFragmentLocation childLocation{SegmentId(10U), Generation(1U), 71U};
	const NativeBulkRunFragmentLocation rootLocation{SegmentId(11U), Generation(1U), 71U};
	const NativeBulkEdgeRun run{{edge(3U)}};
	const std::vector<std::byte> runBytes = NativeBulkEdgeRunCodec::encode(run);

	NativeBulkEdgeRunManifest child;
	child.runs = {runLocation};
	child.record_count = 1U;
	// The child remains CRC-valid, so traversal must verify its decoded
	// descendants instead of trusting the parent's matching aggregate.
	child.encoded_bytes = NativeBulkEdgeRunCodec::encodedSize(run) + 1U;
	child.first_edge_id = 3U;
	child.last_edge_id = 3U;
	const std::vector<std::byte> childBytes = NativeBulkEdgeRunManifestCodec::encode(child);

	NativeBulkEdgeRunManifest root;
	root.level = 1U;
	root.children = {childLocation};
	root.record_count = 1U;
	root.encoded_bytes = child.encoded_bytes;
	root.first_edge_id = 3U;
	root.last_edge_id = 3U;
	const std::vector<std::byte> rootBytes = NativeBulkEdgeRunManifestCodec::encode(root);

	const NativeBulkEdgeRecordSource source = nativeBulkEdgeRecordSource(
			rootLocation, 2U,
			[runLocation, childLocation, rootLocation, runBytes, childBytes, rootBytes](
					const NativeBulkRunFragmentLocation &location) {
				if (location == runLocation) {
					return runBytes;
				}
				if (location == childLocation) {
					return childBytes;
				}
				if (location == rootLocation) {
					return rootBytes;
				}
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "test reader received a foreign native bulk Edge scratch location");
			});
	NativeBulkEdgeRecordCursor cursor = source();
	ASSERT_TRUE(cursor().has_value());
	EXPECT_THROW(static_cast<void>(cursor()), FormatException);
}
