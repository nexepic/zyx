/**
 * @file test_NativeBulkRun.cpp
 * @brief Native single-file bulk scratch-run fragment codec tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <source_location>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeRun.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/index/NativeBulkRun.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected,
						   const std::source_location location = std::source_location::current()) {
		try {
			operation();
			ADD_FAILURE_AT(location.file_name(), static_cast<int>(location.line())) << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected) << error.what();
		}
	}

	[[nodiscard]] BTreePageEntry entry(uint8_t key, uint64_t value) { return {{std::byte{key}}, value}; }

	[[nodiscard]] NativeBulkRunFragment fragment() {
		return {{entry(0x10U, 11U), entry(0x20U, 22U), entry(0x30U, 33U)}};
	}

	[[nodiscard]] OrderedBTreeEntrySource source(std::vector<BTreePageEntry> entries) {
		return [entries = std::move(entries)] {
			return [entries, ordinal = size_t{0U}]() mutable -> std::optional<BTreePageEntry> {
				if (ordinal == entries.size()) {
					return std::nullopt;
				}
				return entries[ordinal++];
			};
		};
	}

	[[nodiscard]] Superblock superblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	[[nodiscard]] NativeBulkRunStoreOptions storeOptions() {
		return {PhysicalSlotId(40U), PhysicalSlotId(80U), RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES};
	}

	[[nodiscard]] uint32_t bulkRunCrc(std::span<const std::byte> bytes) {
		const std::byte empty{};
		return graph::utils::calculateCrc(bytes.empty() ? &empty : bytes.data(), bytes.size());
	}

	void refreshFragmentRecordChecksum(std::vector<std::byte> &bytes) {
		constexpr size_t RECORD_BYTES_OFFSET = 16U;
		constexpr size_t RECORD_CRC_OFFSET = 20U;
		constexpr size_t LOGICAL_BYTES_OFFSET = 24U;
		const std::span<const std::byte> records =
				std::span<const std::byte>(bytes).subspan(RSS_NATIVE_BULK_RUN_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, RECORD_BYTES_OFFSET, static_cast<uint32_t>(records.size()));
		LittleEndianCodec::writeU64(bytes, LOGICAL_BYTES_OFFSET, records.size());
		LittleEndianCodec::writeU32(bytes, RECORD_CRC_OFFSET, bulkRunCrc(records));
	}

} // namespace

class NativeBulkRunStoreTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;

	void SetUp() override {
		path_ = std::filesystem::temp_directory_path() /
				("native_bulk_run_" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".dat");
		file_ = std::make_shared<std::fstream>(path_,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io_.reset();
		if (file_ && file_->is_open()) {
			file_->close();
		}
		file_.reset();
		std::error_code error;
		std::filesystem::remove(path_, error);
	}
};

TEST(NativeBulkRunFragmentCodecTest, RoundTripsStrictlySortedEntriesWithIndependentRecordChecksums) {
	const NativeBulkRunFragment expected = fragment();
	const std::vector<std::byte> encoded = NativeBulkRunFragmentCodec::encode(expected);
	EXPECT_EQ(NativeBulkRunFragmentCodec::decode(encoded), expected);
}

TEST(NativeBulkRunValueObjectTest, ValidatesFragmentAndManifestIdentitiesAcrossBothManifestLevels) {
	const NativeBulkRunFragmentLocation valid{SegmentId(9U), Generation(3U), 71U};
	EXPECT_TRUE(valid.isValid());
	EXPECT_FALSE((NativeBulkRunFragmentLocation{}).isValid());
	EXPECT_FALSE((NativeBulkRunFragmentLocation{SegmentId(9U), Generation{}, 71U}).isValid());
	EXPECT_FALSE((NativeBulkRunFragmentLocation{SegmentId(9U), Generation(3U), 0U}).isValid());
	EXPECT_EQ(valid, valid);

	const NativeBulkRunManifest leaf{valid, 0U, {valid}, {}, 1U, 16U};
	EXPECT_TRUE(leaf.isValid());
	const NativeBulkRunManifest parent{valid, 1U, {}, {valid}, 1U, 16U};
	EXPECT_TRUE(parent.isValid());
	EXPECT_FALSE((NativeBulkRunManifest{{}, 0U, {valid}, {}, 1U, 16U}).isValid());
	EXPECT_FALSE((NativeBulkRunManifest{valid, 0U, {}, {}, 1U, 16U}).isValid());
	EXPECT_FALSE((NativeBulkRunManifest{valid, 0U, {valid}, {valid}, 1U, 16U}).isValid());
	EXPECT_FALSE((NativeBulkRunManifest{valid, 1U, {valid}, {}, 1U, 16U}).isValid());
	EXPECT_FALSE((NativeBulkRunManifest{valid, 1U, {}, {}, 1U, 16U}).isValid());
	EXPECT_FALSE((NativeBulkRunManifest{valid, 0U, {valid}, {}, 0U, 16U}).isValid());
	EXPECT_FALSE((NativeBulkRunManifest{valid, 0U, {valid}, {}, 1U, 0U}).isValid());
	EXPECT_EQ(leaf, leaf);
}

TEST_F(NativeBulkRunStoreTest, BindsScratchStorageToItsOwningMetadataCoordinator) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	EXPECT_TRUE(store.usesMetadataStore(*metadata));
}

TEST(NativeBulkRunFragmentCodecTest, RejectsUnsortedDuplicateAndEmptyInputBeforeItCanReachScratchStorage) {
	NativeBulkRunFragment unordered{{entry(0x20U, 22U), entry(0x10U, 11U)}};
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::encode(unordered)); },
					  FormatError::FERR_INVALID_HEADER);
	NativeBulkRunFragment duplicate{{entry(0x10U, 11U), entry(0x10U, 12U)}};
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::encode(duplicate)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::encode({})); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkRunFragmentCodecTest, RejectsEmptyAndUnownedWireFramesBeforeDecodingFields) {
	const NativeBulkRunFragmentLocation location{SegmentId(13U), Generation(1U), 71U};
	expectFormatError(
			[&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(std::span<const std::byte>{})); },
			FormatError::FERR_BUFFER_TOO_SMALL);
	expectFormatError(
			[&] { static_cast<void>(NativeBulkRunManifestCodec::decode(std::span<const std::byte>{}, location)); },
			FormatError::FERR_BUFFER_TOO_SMALL);

	const NativeBulkRunManifest manifest{
			{}, 0U, {{SegmentId(11U), Generation(1U), 71U}}, {}, 1U, 32U};
	const std::vector<std::byte> encoded = NativeBulkRunManifestCodec::encode(manifest);
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::decode(encoded, {})); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
}

TEST(NativeBulkRunFragmentCodecTest, FailsClosedForHeaderDimensionsAndRecordIntegrityCorruption) {
	const std::vector<std::byte> encoded = NativeBulkRunFragmentCodec::encode(fragment());

	auto corruptMagic = encoded;
	corruptMagic[0] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(corruptMagic)); },
					  FormatError::FERR_INVALID_HEADER);

	auto corruptAggregateChecksum = encoded;
	corruptAggregateChecksum[20] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(corruptAggregateChecksum)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	auto corruptRecord = encoded;
	corruptRecord.back() ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(corruptRecord)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	auto corruptCount = encoded;
	LittleEndianCodec::writeU32(corruptCount, 12U, 4U);
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(corruptCount)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkRunFragmentCodecTest, ExhaustivelyRejectsEverySingleBitWireCorruption) {
	const std::vector<std::byte> encoded = NativeBulkRunFragmentCodec::encode(fragment());
	for (size_t offset = 0U; offset < encoded.size(); ++offset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto corrupted = encoded;
			corrupted[offset] ^= static_cast<std::byte>(uint8_t{1U} << bit);
			EXPECT_THROW(static_cast<void>(NativeBulkRunFragmentCodec::decode(corrupted)), FormatException)
					<< "wire offset " << offset << ", bit " << static_cast<unsigned int>(bit);
		}
	}
}

TEST(NativeBulkRunFragmentCodecTest, RejectsTruncatedAndOversizedRecordFrames) {
	const std::vector<std::byte> encoded = NativeBulkRunFragmentCodec::encode(fragment());
	std::vector<std::byte> truncated(encoded.begin(), encoded.end() - 1);
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(truncated)); },
					  FormatError::FERR_INVALID_HEADER);

	auto oversized = encoded;
	LittleEndianCodec::writeU32(oversized, RSS_NATIVE_BULK_RUN_HEADER_BYTES, 4'196U);
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(oversized)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST(NativeBulkRunFragmentCodecTest, RejectsEachIndependentAggregateDimensionMismatch) {
	const std::vector<std::byte> encoded = NativeBulkRunFragmentCodec::encode(fragment());

	// Keep the header checksum valid so each assertion reaches the dimension
	// contract instead of being masked by aggregate-integrity validation.
	auto mismatchedRecordBytes = encoded;
	LittleEndianCodec::writeU32(mismatchedRecordBytes, 16U,
			LittleEndianCodec::readU32(mismatchedRecordBytes, 16U) - 1U);
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(mismatchedRecordBytes)); },
					  FormatError::FERR_INVALID_HEADER);

	auto mismatchedLogicalBytes = encoded;
	LittleEndianCodec::writeU64(mismatchedLogicalBytes, 24U,
			LittleEndianCodec::readU64(mismatchedLogicalBytes, 24U) + 1U);
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(mismatchedLogicalBytes)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkRunFragmentCodecTest, RejectsChecksumValidMalformedNestedRecordFrames) {
	constexpr size_t RECORD_LENGTH_OFFSET = 0U;
	constexpr size_t RECORD_CRC_OFFSET = 4U;
	const std::vector<std::byte> encoded = NativeBulkRunFragmentCodec::encode(fragment());
	const size_t firstRecordOffset = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
	const size_t firstEntryBytes = LittleEndianCodec::readU32(encoded, firstRecordOffset + RECORD_LENGTH_OFFSET);
	const size_t secondRecordOffset = firstRecordOffset + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + firstEntryBytes;

	auto truncatedHeader = encoded;
	truncatedHeader.resize(secondRecordOffset + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES - 1U);
	refreshFragmentRecordChecksum(truncatedHeader);
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(truncatedHeader)); },
					  FormatError::FERR_BUFFER_TOO_SMALL);

	auto invalidLength = encoded;
	LittleEndianCodec::writeU32(invalidLength, firstRecordOffset + RECORD_LENGTH_OFFSET, 1U);
	refreshFragmentRecordChecksum(invalidLength);
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(invalidLength)); },
					  FormatError::FERR_INVALID_HEADER);

	auto invalidRecordChecksum = encoded;
	invalidRecordChecksum.back() ^= std::byte{0x01U};
	refreshFragmentRecordChecksum(invalidRecordChecksum);
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(invalidRecordChecksum)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	auto duplicateKey = encoded;
	const std::vector<std::byte> firstEntry = BTreePageEntryCodec::encode(entry(0x10U, 22U));
	ASSERT_EQ(firstEntry.size(), firstEntryBytes);
	std::copy(firstEntry.begin(), firstEntry.end(),
				  duplicateKey.begin() + static_cast<std::ptrdiff_t>(secondRecordOffset + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES));
	LittleEndianCodec::writeU32(duplicateKey, secondRecordOffset + RECORD_CRC_OFFSET, bulkRunCrc(firstEntry));
	refreshFragmentRecordChecksum(duplicateKey);
	expectFormatError([&] { static_cast<void>(NativeBulkRunFragmentCodec::decode(duplicateKey)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(NativeBulkRunStoreTest, ReadsOnlyTheMatchingTransactionPrivateFragmentAndReleasesItsSlot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const NativeBulkRunFragmentLocation location = store.write(71U, fragment(), LogSequenceNumber(2U));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.read(snapshot, location), fragment());
	snapshot.release();

	NativeBulkRunFragmentLocation foreign = location;
	foreign.transaction_id = 72U;
	snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.read(snapshot, foreign)); }, FormatError::FERR_INVALID_STATE);
	snapshot.release();

	store.release(location, LogSequenceNumber(4U));
	snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U)));
	EXPECT_EQ(metadata->findCatalog(snapshot, location.segment_id).state, SegmentState::SGS_OBSOLETE);
}

TEST_F(NativeBulkRunStoreTest, PersistsASeparateCanonicalEdgeRunAsTransactionPrivateOpaqueScratch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	NativeBulkEdgeRecord first;
	first.edge_id = 41U;
	first.source_node_id = 7U;
	first.target_node_id = 8U;
	first.type = "LINK";
	first.properties = {{"weight", graph::PropertyValue(int64_t(3))}};
	NativeBulkEdgeRecord second = first;
	second.edge_id = 42U;
	second.source_node_id = 8U;
	second.target_node_id = 9U;
	const NativeBulkEdgeRun expected{{first, second}};
	const NativeBulkRunFragmentLocation location =
			store.writeOpaquePayload(71U, NativeBulkEdgeRunCodec::encode(expected), LogSequenceNumber(2U));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(NativeBulkEdgeRunCodec::decode(store.readOpaquePayload(snapshot, location)), expected);
	snapshot.release();

	NativeBulkRunFragmentLocation foreign = location;
	foreign.transaction_id = 72U;
	snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.readOpaquePayload(snapshot, foreign)); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();
	store.release(location, LogSequenceNumber(4U));
}

TEST_F(NativeBulkRunStoreTest, OpenOpaquePayloadReaderPinsAndAuthenticatesPrivateScratch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const std::vector<std::byte> expected{std::byte{0x11U}, std::byte{0x22U}};
	const NativeBulkRunFragmentLocation location =
			store.writeOpaquePayload(71U, expected, LogSequenceNumber(2U));

	{
		const NativeBulkRunPayloadReader reader = store.openOpaquePayloadReader();
		EXPECT_EQ(reader(location), expected);

		NativeBulkRunFragmentLocation foreign = location;
		foreign.transaction_id = 72U;
		expectFormatError([&] { static_cast<void>(reader(foreign)); }, FormatError::FERR_INVALID_STATE);
	}

	store.release(location, LogSequenceNumber(4U));
}

TEST_F(NativeBulkRunStoreTest, BatchWritePublishesOrderedPrivateFragmentsThroughOnePhysicalAllocation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const std::vector<NativeBulkRunFragment> fragments = {
			{{entry(0x10U, 11U)}}, {{entry(0x20U, 22U)}}, {{entry(0x30U, 33U)}}};
	const std::vector<NativeBulkRunFragmentLocation> locations =
			store.writeBatch(71U, fragments, LogSequenceNumber(2U));
	ASSERT_EQ(locations.size(), fragments.size());
	EXPECT_LT(locations[0U].segment_id, locations[1U].segment_id);
	EXPECT_LT(locations[1U].segment_id, locations[2U].segment_id);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	for (size_t ordinal = 0U; ordinal < locations.size(); ++ordinal) {
		EXPECT_EQ(store.read(snapshot, locations[ordinal]), fragments[ordinal]);
		EXPECT_EQ(metadata->findCatalog(snapshot, locations[ordinal].segment_id).physical_slot,
				  PhysicalSlotId(40U + ordinal));
	}
	// One high-water reservation plus one multi-segment Catalog publication is
	// required, rather than one Superblock generation for every fragment.
	EXPECT_EQ(snapshot.superblock().generation, Generation(3U));
	snapshot.release();

	store.releaseBatch(locations, LogSequenceNumber(4U));
	snapshot = metadata->openSnapshot();
	// The complete group uses one retirement generation and one reclamation
	// generation, independent of the number of fragments in the batch.
	EXPECT_EQ(snapshot.superblock().generation, Generation(5U));
	for (size_t ordinal = 0U; ordinal < locations.size(); ++ordinal) {
		EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U + ordinal)));
		EXPECT_EQ(metadata->findCatalog(snapshot, locations[ordinal].segment_id).state, SegmentState::SGS_OBSOLETE);
	}
}

TEST_F(NativeBulkRunStoreTest, BatchWriteRejectsMoreFragmentsThanItsExplicitMemoryBound) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStoreOptions options = storeOptions();
	options.maximum_batch_fragments = 2U;
	NativeBulkRunStore store(physical, *metadata, options);
	const std::vector<NativeBulkRunFragment> fragments = {
			{{entry(0x10U, 11U)}}, {{entry(0x20U, 22U)}}, {{entry(0x30U, 33U)}}};
	expectFormatError([&] { static_cast<void>(store.writeBatch(71U, fragments, LogSequenceNumber(2U))); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(NativeBulkRunStoreTest, RejectsInvalidStoreAndScratchBoundsBeforeMetadataMutation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);

	auto rejectStore = [&](NativeBulkRunStoreOptions options) {
		expectFormatError([&] { NativeBulkRunStore rejected(physical, *metadata, options); },
						  FormatError::FERR_INVALID_HEADER);
	};
	NativeBulkRunStoreOptions invalid = storeOptions();
	invalid.first_slot = {};
	rejectStore(invalid);
	invalid = storeOptions();
	invalid.last_slot = {};
	rejectStore(invalid);
	invalid = storeOptions();
	invalid.first_slot = PhysicalSlotId(80U);
	invalid.last_slot = PhysicalSlotId(40U);
	rejectStore(invalid);
	invalid = storeOptions();
	invalid.maximum_fragment_bytes = 0U;
	rejectStore(invalid);
	invalid = storeOptions();
	invalid.maximum_fragment_bytes = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES + 1U;
	rejectStore(invalid);
	invalid = storeOptions();
	invalid.maximum_batch_fragments = 0U;
	rejectStore(invalid);
	invalid = storeOptions();
	invalid.maximum_manifest_depth = 0U;
	rejectStore(invalid);
}

TEST_F(NativeBulkRunStoreTest, RejectsInvalidScratchWriteInputsWithoutPublishingAnything) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const NativeBulkRunFragment valid = fragment();
	const LogSequenceNumber checkpoint(2U);

	expectFormatError([&] { static_cast<void>(store.write(0U, valid, checkpoint)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.write(71U, valid, {})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.write(71U, {}, checkpoint)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.writeOpaquePayload(0U, {std::byte{1U}}, checkpoint)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.writeOpaquePayload(71U, {}, checkpoint)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.writeOpaquePayload(71U, {std::byte{1U}}, {})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.writeOpaquePayloadBatch(0U, {{std::byte{1U}}}, checkpoint)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.writeOpaquePayloadBatch(71U, {}, checkpoint)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.writeOpaquePayloadBatch(71U, {{ }}, checkpoint)); },
					  FormatError::FERR_INVALID_HEADER);
	const std::array oneFragment{valid};
	const std::array<NativeBulkRunFragment, 0U> noFragments{};
	expectFormatError([&] { static_cast<void>(store.writeBatch(0U, oneFragment, checkpoint)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.writeBatch(71U, noFragments, checkpoint)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.writeBatch(71U, oneFragment, {})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.writeOpaquePayloadBatch(71U, {{std::byte{1U}}}, {})); },
					  FormatError::FERR_INVALID_HEADER);

	NativeBulkRunStoreOptions shortPayloadOptions = storeOptions();
	shortPayloadOptions.maximum_fragment_bytes = 1U;
	NativeBulkRunStore shortPayloadStore(physical, *metadata, shortPayloadOptions);
	expectFormatError(
			[&] { static_cast<void>(shortPayloadStore.writeOpaquePayload(71U, {std::byte{1U}, std::byte{2U}}, checkpoint)); },
			FormatError::FERR_INVALID_HEADER);
	// The single-fragment path has its own bound check before it can reserve a
	// scratch segment; keep it aligned with the already-covered batch path.
	expectFormatError([&] { static_cast<void>(shortPayloadStore.write(71U, valid, checkpoint)); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(shortPayloadStore.writeBatch(71U, oneFragment, checkpoint)); },
				  FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin unchanged = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
}

TEST_F(NativeBulkRunStoreTest, RejectsEntriesThatExceedTheBoundedIndexKeyFrame) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	NativeBulkRunFragment oversized;
	oversized.entries.push_back(
			BTreePageEntry{std::vector<std::byte>(RSS_INDEX_KEY_MAX_BYTES + 1U, std::byte{0x5AU}), 71U});

	expectFormatError([&] { static_cast<void>(store.write(71U, oversized, LogSequenceNumber(2U))); },
					  FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U)));
}

TEST_F(NativeBulkRunStoreTest, RejectsInvalidReadAndManifestReleaseLocations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.read(snapshot, {})); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.readOpaquePayload(snapshot, {})); }, FormatError::FERR_INVALID_HEADER);
	snapshot.release();
	expectFormatError([&] { static_cast<void>(store.releaseManifest({}, LogSequenceNumber(2U))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.releaseManifest(
						NativeBulkRunFragmentLocation{SegmentId(1U), Generation(1U), 71U}, {})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.orderedSource({})); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(NativeBulkRunStore::orderedSourceFromPayloadReader(
						{}, 1U, {})); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(NativeBulkRunStoreTest, WritableOpenRecoveryReclaimsEveryUnpublishedScratchFragment) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const NativeBulkRunFragmentLocation first = store.write(71U, fragment(), LogSequenceNumber(2U));
	const NativeBulkRunFragmentLocation second = store.write(72U, fragment(), LogSequenceNumber(4U));

	store.reclaimUnpublishedOnOpen(LogSequenceNumber(6U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U)));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(41U)));
	EXPECT_EQ(metadata->findCatalog(snapshot, first.segment_id).state, SegmentState::SGS_OBSOLETE);
	EXPECT_EQ(metadata->findCatalog(snapshot, second.segment_id).state, SegmentState::SGS_OBSOLETE);
}

TEST_F(NativeBulkRunStoreTest, WritableOpenRecoveryContinuesAfterEachBoundedScratchBatch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStoreOptions options = storeOptions();
	options.maximum_batch_fragments = 1U;
	NativeBulkRunStore store(physical, *metadata, options);
	const NativeBulkRunFragmentLocation first = store.write(71U, fragment(), LogSequenceNumber(2U));
	const NativeBulkRunFragmentLocation second = store.write(72U, fragment(), LogSequenceNumber(4U));

	store.reclaimUnpublishedOnOpen(LogSequenceNumber(6U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U)));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(41U)));
	EXPECT_EQ(metadata->findCatalog(snapshot, first.segment_id).state, SegmentState::SGS_OBSOLETE);
	EXPECT_EQ(metadata->findCatalog(snapshot, second.segment_id).state, SegmentState::SGS_OBSOLETE);
}

TEST_F(NativeBulkRunStoreTest, ManifestProvidesARewindableOrderedSourceAcrossMultipleFragments) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const NativeBulkRunFragmentLocation first =
			store.write(71U, {{entry(0x10U, 11U), entry(0x20U, 22U)}}, LogSequenceNumber(2U));
	const NativeBulkRunFragmentLocation second =
			store.write(71U, {{entry(0x30U, 33U), entry(0x40U, 44U)}}, LogSequenceNumber(4U));
	const NativeBulkRunManifest manifest = store.writeManifest(71U, std::array{first, second}, LogSequenceNumber(6U));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.readManifest(snapshot, manifest.location), manifest);
	snapshot.release();

	const OrderedBTreeEntrySource source = store.orderedSource(manifest.location);
	for (size_t pass = 0U; pass < 2U; ++pass) {
		OrderedBTreeEntryCursor cursor = source();
		std::vector<BTreePageEntry> actual;
		for (std::optional<BTreePageEntry> next = cursor(); next.has_value(); next = cursor()) {
			actual.push_back(std::move(*next));
		}
		EXPECT_EQ(actual, (std::vector<BTreePageEntry>{entry(0x10U, 11U), entry(0x20U, 22U), entry(0x30U, 33U),
													   entry(0x40U, 44U)}));
	}
}

TEST_F(NativeBulkRunStoreTest, CandidatePayloadReaderProvidesTheSameRewindableManifestCursorWithoutMetadata) {
	const uint64_t transactionId = 71U;
	const NativeBulkRunFragmentLocation first{SegmentId(11U), Generation(1U), transactionId};
	const NativeBulkRunFragmentLocation second{SegmentId(12U), Generation(1U), transactionId};
	const NativeBulkRunFragmentLocation manifestLocation{SegmentId(13U), Generation(1U), transactionId};
	const NativeBulkRunFragment firstFragment{{entry(0x10U, 11U), entry(0x20U, 22U)}};
	const NativeBulkRunFragment secondFragment{{entry(0x30U, 33U), entry(0x40U, 44U)}};
	NativeBulkRunManifest manifest;
	manifest.fragments = {first, second};
	manifest.entry_count = 4U;
	manifest.encoded_bytes = NativeBulkRunFragmentCodec::encode(firstFragment).size() +
							 NativeBulkRunFragmentCodec::encode(secondFragment).size();
	const std::vector<std::byte> encodedManifest = NativeBulkRunManifestCodec::encode(manifest);
	const NativeBulkRunPayloadReader reader = [first, second, manifestLocation,
											   firstBytes = NativeBulkRunFragmentCodec::encode(firstFragment),
											   secondBytes = NativeBulkRunFragmentCodec::encode(secondFragment),
											   encodedManifest](const NativeBulkRunFragmentLocation &location) {
		if (location == first) {
			return firstBytes;
		}
		if (location == second) {
			return secondBytes;
		}
		if (location == manifestLocation) {
			return encodedManifest;
		}
		throw FormatException(FormatError::FERR_INVALID_STATE, "candidate reader received a foreign segment");
	};
	const OrderedBTreeEntrySource source =
			NativeBulkRunStore::orderedSourceFromPayloadReader(manifestLocation, 2U, reader);
	for (size_t pass = 0U; pass < 2U; ++pass) {
		OrderedBTreeEntryCursor cursor = source();
		std::vector<BTreePageEntry> actual;
		for (std::optional<BTreePageEntry> next = cursor(); next.has_value(); next = cursor()) {
			actual.push_back(std::move(*next));
		}
		EXPECT_EQ(actual, (std::vector<BTreePageEntry>{entry(0x10U, 11U), entry(0x20U, 22U), entry(0x30U, 33U),
													   entry(0x40U, 44U)}));
	}
}

TEST(NativeBulkRunCandidateSourceTest, RejectsEachMissingCandidateSourceContractBeforeReadingPayloads) {
	const NativeBulkRunFragmentLocation manifestLocation{SegmentId(13U), Generation(1U), 71U};
	const NativeBulkRunPayloadReader reader = [](const NativeBulkRunFragmentLocation &) {
		return std::vector<std::byte>{};
	};
	expectFormatError(
			[&] { static_cast<void>(NativeBulkRunStore::orderedSourceFromPayloadReader({}, 2U, reader)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(NativeBulkRunStore::orderedSourceFromPayloadReader(manifestLocation, 0U, reader)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(NativeBulkRunStore::orderedSourceFromPayloadReader(
						manifestLocation, 2U, NativeBulkRunPayloadReader{}));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkRunCandidateSourceTest, FailsClosedOnManifestCountAndHierarchyDivergence) {
	const uint64_t transactionId = 71U;
	const NativeBulkRunFragmentLocation fragmentLocation{SegmentId(11U), Generation(1U), transactionId};
	const NativeBulkRunFragmentLocation leafLocation{SegmentId(12U), Generation(1U), transactionId};
	const NativeBulkRunFragmentLocation rootLocation{SegmentId(13U), Generation(1U), transactionId};
	const std::vector<std::byte> fragmentBytes = NativeBulkRunFragmentCodec::encode({{entry(0x10U, 11U)}});

	NativeBulkRunManifest countDivergentLeaf;
	countDivergentLeaf.fragments = {fragmentLocation};
	countDivergentLeaf.entry_count = 2U;
	countDivergentLeaf.encoded_bytes = fragmentBytes.size();
	const std::vector<std::byte> countDivergentLeafBytes = NativeBulkRunManifestCodec::encode(countDivergentLeaf);
	const NativeBulkRunPayloadReader countDivergentReader = [fragmentLocation, leafLocation, fragmentBytes,
															 countDivergentLeafBytes](
																	const NativeBulkRunFragmentLocation &location) {
		if (location == fragmentLocation) {
			return fragmentBytes;
		}
		if (location == leafLocation) {
			return countDivergentLeafBytes;
		}
		throw FormatException(FormatError::FERR_INVALID_STATE, "candidate reader received an out-of-scope segment");
	};
	const OrderedBTreeEntryCursor countDivergentCursor =
			NativeBulkRunStore::orderedSourceFromPayloadReader(leafLocation, 2U, countDivergentReader)();
	ASSERT_TRUE(countDivergentCursor().has_value());
	expectFormatError([&] { static_cast<void>(countDivergentCursor()); }, FormatError::FERR_INVALID_STATE);

	NativeBulkRunManifest invalidChildLevel;
	invalidChildLevel.level = 1U;
	invalidChildLevel.children = {fragmentLocation};
	invalidChildLevel.entry_count = 1U;
	invalidChildLevel.encoded_bytes = fragmentBytes.size();
	NativeBulkRunManifest root;
	root.level = 1U;
	root.children = {leafLocation};
	root.entry_count = 1U;
	root.encoded_bytes = fragmentBytes.size();
	const std::vector<std::byte> invalidChildLevelBytes = NativeBulkRunManifestCodec::encode(invalidChildLevel);
	const std::vector<std::byte> rootBytes = NativeBulkRunManifestCodec::encode(root);
	const NativeBulkRunPayloadReader invalidHierarchyReader = [leafLocation, rootLocation, invalidChildLevelBytes,
															   rootBytes](
																	  const NativeBulkRunFragmentLocation &location) {
		if (location == leafLocation) {
			return invalidChildLevelBytes;
		}
		if (location == rootLocation) {
			return rootBytes;
		}
		throw FormatException(FormatError::FERR_INVALID_STATE, "candidate reader received an out-of-scope segment");
	};
	const OrderedBTreeEntryCursor invalidHierarchyCursor =
			NativeBulkRunStore::orderedSourceFromPayloadReader(rootLocation, 3U, invalidHierarchyReader)();
	expectFormatError([&] { static_cast<void>(invalidHierarchyCursor()); }, FormatError::FERR_INVALID_STATE);
}

TEST(NativeBulkRunCandidateSourceTest, FailsClosedWhenDecodedFragmentsBreakGlobalKeyOrder) {
	const uint64_t transactionId = 71U;
	const NativeBulkRunFragmentLocation firstLocation{SegmentId(11U), Generation(1U), transactionId};
	const NativeBulkRunFragmentLocation secondLocation{SegmentId(12U), Generation(1U), transactionId};
	const NativeBulkRunFragmentLocation manifestLocation{SegmentId(13U), Generation(1U), transactionId};
	const NativeBulkRunFragment first{{entry(0x30U, 33U)}};
	const NativeBulkRunFragment second{{entry(0x20U, 22U)}};
	const std::vector<std::byte> firstBytes = NativeBulkRunFragmentCodec::encode(first);
	const std::vector<std::byte> secondBytes = NativeBulkRunFragmentCodec::encode(second);
	NativeBulkRunManifest manifest;
	manifest.fragments = {firstLocation, secondLocation};
	manifest.entry_count = 2U;
	manifest.encoded_bytes = firstBytes.size() + secondBytes.size();
	const std::vector<std::byte> manifestBytes = NativeBulkRunManifestCodec::encode(manifest);

	const OrderedBTreeEntrySource source = NativeBulkRunStore::orderedSourceFromPayloadReader(
			manifestLocation, 1U,
			[firstLocation, secondLocation, manifestLocation, firstBytes, secondBytes, manifestBytes](
					const NativeBulkRunFragmentLocation &location) {
				if (location == firstLocation) {
					return firstBytes;
				}
				if (location == secondLocation) {
					return secondBytes;
				}
				if (location == manifestLocation) {
					return manifestBytes;
				}
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "candidate reader received an out-of-scope segment");
			});
	OrderedBTreeEntryCursor cursor = source();
	ASSERT_TRUE(cursor().has_value());
	expectFormatError([&] { static_cast<void>(cursor()); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(NativeBulkRunStoreTest, RejectsAManifestWhoseFragmentBoundariesAreNotGloballyOrdered) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const NativeBulkRunFragmentLocation first = store.write(71U, {{entry(0x30U, 33U)}}, LogSequenceNumber(2U));
	const NativeBulkRunFragmentLocation second = store.write(71U, {{entry(0x20U, 22U)}}, LogSequenceNumber(4U));
	expectFormatError(
			[&] { static_cast<void>(store.writeManifest(71U, std::array{first, second}, LogSequenceNumber(6U))); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(NativeBulkRunStoreTest, ParentManifestExtendsTheRunBeyondOneLeafManifestAndRemainsRewindable) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const NativeBulkRunFragmentLocation first = store.write(71U, {{entry(0x10U, 11U)}}, LogSequenceNumber(2U));
	const NativeBulkRunFragmentLocation second = store.write(71U, {{entry(0x20U, 22U)}}, LogSequenceNumber(4U));
	const NativeBulkRunFragmentLocation third = store.write(71U, {{entry(0x30U, 33U)}}, LogSequenceNumber(6U));
	const NativeBulkRunFragmentLocation fourth = store.write(71U, {{entry(0x40U, 44U)}}, LogSequenceNumber(8U));
	const NativeBulkRunManifest left = store.writeManifest(71U, std::array{first, second}, LogSequenceNumber(10U));
	const NativeBulkRunManifest right = store.writeManifest(71U, std::array{third, fourth}, LogSequenceNumber(12U));
	const NativeBulkRunManifest root =
			store.writeManifestParent(71U, std::array{left.location, right.location}, LogSequenceNumber(14U));
	EXPECT_EQ(root.level, 1U);
	EXPECT_EQ(root.entry_count, 4U);

	const OrderedBTreeEntrySource source = store.orderedSource(root.location);
	for (size_t pass = 0U; pass < 2U; ++pass) {
		OrderedBTreeEntryCursor cursor = source();
		std::vector<uint8_t> keys;
		for (std::optional<BTreePageEntry> next = cursor(); next.has_value(); next = cursor()) {
			keys.push_back(std::to_integer<uint8_t>(next->key.front()));
		}
		EXPECT_EQ(keys, (std::vector<uint8_t>{0x10U, 0x20U, 0x30U, 0x40U}));
	}
}

TEST_F(NativeBulkRunStoreTest, StreamingWriterUsesBoundedFragmentsAndBuildsAHierarchicalManifest) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStoreOptions options = storeOptions();
	options.maximum_fragment_bytes = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
	NativeBulkRunStore store(physical, *metadata, options);
	NativeBulkRunStreamingWriter writer(store, 71U, {.maximum_buffered_bytes = 80U, .maximum_entries = 8U},
										LogSequenceNumber(2U));
	for (uint8_t key = 0x10U; key < 0x60U; static_cast<void>(key += 0x10U)) {
		writer.add(entry(key, key));
	}
	const NativeBulkRunManifest manifest = writer.finish();
	EXPECT_EQ(manifest.level, 0U);
	EXPECT_EQ(manifest.entry_count, 5U);

	OrderedBTreeEntryCursor cursor = store.orderedSource(manifest.location)();
	std::vector<uint8_t> keys;
	for (std::optional<BTreePageEntry> next = cursor(); next.has_value(); next = cursor()) {
		keys.push_back(std::to_integer<uint8_t>(next->key.front()));
	}
	EXPECT_EQ(keys, (std::vector<uint8_t>{0x10U, 0x20U, 0x30U, 0x40U, 0x50U}));
}

TEST_F(NativeBulkRunStoreTest, StreamingWriterFlushesBoundedAllocationBatchesIntoAParentManifest) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStoreOptions options = storeOptions();
	options.maximum_batch_fragments = 2U;
	NativeBulkRunStore store(physical, *metadata, options);
	NativeBulkRunStreamingWriter writer(store, 71U, {.maximum_buffered_bytes = 41U, .maximum_entries = 5U},
										LogSequenceNumber(2U));
	for (uint8_t key = 0x10U; key <= 0x50U; static_cast<void>(key += 0x10U)) {
		writer.add(entry(key, key));
	}
	const NativeBulkRunManifest root = writer.finish();
	EXPECT_EQ(root.level, 1U);
	EXPECT_EQ(root.entry_count, 5U);

	OrderedBTreeEntryCursor cursor = store.orderedSource(root.location)();
	std::vector<uint8_t> keys;
	for (std::optional<BTreePageEntry> next = cursor(); next.has_value(); next = cursor()) {
		keys.push_back(std::to_integer<uint8_t>(next->key.front()));
	}
	EXPECT_EQ(keys, (std::vector<uint8_t>{0x10U, 0x20U, 0x30U, 0x40U, 0x50U}));
}

TEST_F(NativeBulkRunStoreTest, StreamingWriterLiftsASoleLeafRootBeneathAnExistingParentLevel) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStoreOptions options = storeOptions();
	options.maximum_fragment_bytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + 41U;
	options.maximum_batch_fragments = 2U;
	NativeBulkRunStore store(physical, *metadata, options);
	NativeBulkRunStreamingWriter writer(store, 71U, {.maximum_buffered_bytes = 41U, .maximum_entries = 5U},
										LogSequenceNumber(2U));
	for (uint8_t key = 0x10U; key <= 0x50U; static_cast<void>(key += 0x10U)) {
		writer.add(entry(key, key));
	}

	const NativeBulkRunManifest root = writer.finish();
	EXPECT_EQ(root.level, 2U);
	EXPECT_EQ(root.entry_count, 5U);

	OrderedBTreeEntryCursor cursor = store.orderedSource(root.location)();
	std::vector<uint8_t> keys;
	for (std::optional<BTreePageEntry> next = cursor(); next.has_value(); next = cursor()) {
		keys.push_back(std::to_integer<uint8_t>(next->key.front()));
	}
	EXPECT_EQ(keys, (std::vector<uint8_t>{0x10U, 0x20U, 0x30U, 0x40U, 0x50U}));
}

TEST_F(NativeBulkRunStoreTest, BoundedFanInMergeStreamsTwoIndependentRunsWithoutDuplicateKeys) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStoreOptions options = storeOptions();
	options.maximum_fragment_bytes = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
	NativeBulkRunStore store(physical, *metadata, options);
	NativeBulkRunStreamingWriter odd(store, 71U, {.maximum_buffered_bytes = 256U, .maximum_entries = 3U},
									 LogSequenceNumber(20U));
	NativeBulkRunStreamingWriter even(store, 71U, {.maximum_buffered_bytes = 256U, .maximum_entries = 3U},
									  LogSequenceNumber(100U));
	odd.add(entry(0x10U, 11U));
	odd.add(entry(0x30U, 33U));
	odd.add(entry(0x50U, 55U));
	even.add(entry(0x20U, 22U));
	even.add(entry(0x40U, 44U));
	even.add(entry(0x60U, 66U));
	const NativeBulkRunManifest oddRun = odd.finish();
	const NativeBulkRunManifest evenRun = even.finish();
	const NativeBulkRunManifest merged = NativeBulkRunMerger::merge(
			store, 71U, std::array{oddRun.location, evenRun.location},
			{.maximum_open_runs = 2U, .output = {.maximum_buffered_bytes = 256U, .maximum_entries = 6U}},
			LogSequenceNumber(200U));
	EXPECT_EQ(merged.entry_count, 6U);
	OrderedBTreeEntryCursor cursor = store.orderedSource(merged.location)();
	std::vector<uint8_t> keys;
	for (std::optional<BTreePageEntry> next = cursor(); next.has_value(); next = cursor()) {
		keys.push_back(std::to_integer<uint8_t>(next->key.front()));
	}
	EXPECT_EQ(keys, (std::vector<uint8_t>{0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U}));
}

TEST_F(NativeBulkRunStoreTest, BoundedFanInMergeRejectsDuplicateKeysAcrossInputRuns) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	NativeBulkRunStreamingWriter left(store, 71U, {.maximum_buffered_bytes = 256U, .maximum_entries = 2U},
									  LogSequenceNumber(20U));
	NativeBulkRunStreamingWriter right(store, 71U, {.maximum_buffered_bytes = 256U, .maximum_entries = 2U},
									   LogSequenceNumber(100U));
	left.add(entry(0x10U, 11U));
	right.add(entry(0x10U, 99U));
	const NativeBulkRunManifest leftRun = left.finish();
	const NativeBulkRunManifest rightRun = right.finish();
	expectFormatError(
			[&] {
				static_cast<void>(NativeBulkRunMerger::merge(
						store, 71U, std::array{leftRun.location, rightRun.location},
						{.maximum_open_runs = 2U, .output = {.maximum_buffered_bytes = 256U, .maximum_entries = 4U}},
						LogSequenceNumber(200U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(NativeBulkRunStoreTest, BoundedFanInMergeValidatesResourceLimitsAndOwnershipBeforeReadingScratch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const NativeBulkRunFragmentLocation candidate{SegmentId(11U), Generation(1U), 71U};
	const std::array candidates{candidate, candidate};
	const NativeBulkRunMergeOptions validOptions{.maximum_open_runs = 2U,
												 .output = {.maximum_buffered_bytes = 64U, .maximum_entries = 2U}};
	expectFormatError(
			[&] {
				static_cast<void>(
						NativeBulkRunMerger::merge(store, 0U, candidates, validOptions, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(NativeBulkRunMerger::merge(store, 71U, std::span(candidates).first(1U), validOptions,
															 LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(NativeBulkRunMerger::merge(
						store, 71U, candidates,
						{.maximum_open_runs = 1U, .output = {.maximum_buffered_bytes = 64U, .maximum_entries = 2U}},
						LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(NativeBulkRunMerger::merge(
						store, 71U, candidates,
						{.maximum_open_runs = 2U, .output = {.maximum_buffered_bytes = 0U, .maximum_entries = 2U}},
						LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(NativeBulkRunMerger::merge(
						store, 71U, candidates,
						{.maximum_open_runs = 2U, .output = {.maximum_buffered_bytes = 64U, .maximum_entries = 0U}},
						LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(NativeBulkRunMerger::merge(store, 71U, candidates, validOptions, {})); },
					  FormatError::FERR_INVALID_HEADER);

	const NativeBulkRunFragmentLocation first = store.write(71U, {{entry(0x10U, 11U)}}, LogSequenceNumber(2U));
	const NativeBulkRunFragmentLocation second = store.write(71U, {{entry(0x20U, 22U)}}, LogSequenceNumber(4U));
	const NativeBulkRunManifest firstRun = store.writeManifest(71U, std::array{first}, LogSequenceNumber(6U));
	const NativeBulkRunManifest secondRun = store.writeManifest(71U, std::array{second}, LogSequenceNumber(8U));
	NativeBulkRunFragmentLocation foreignRun = secondRun.location;
	foreignRun.transaction_id = 72U;
	expectFormatError(
			[&] {
				static_cast<void>(NativeBulkRunMerger::merge(store, 71U, std::array{firstRun.location, foreignRun},
															 validOptions, LogSequenceNumber(10U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST(NativeBulkRunMergerTest, RewindablyMergesCandidateInputsWithoutWritingScratch) {
	const std::array<OrderedBTreeEntrySource, 3U> inputs{source({entry(0x10U, 11U), entry(0x40U, 44U)}),
														 source({entry(0x20U, 22U)}),
														 source({entry(0x30U, 33U), entry(0x50U, 55U)})};
	const OrderedBTreeEntrySource merged = NativeBulkRunMerger::orderedSource(inputs, 3U, 5U);
	for (size_t pass = 0U; pass < 2U; ++pass) {
		OrderedBTreeEntryCursor cursor = merged();
		std::vector<uint8_t> keys;
		for (std::optional<BTreePageEntry> next = cursor(); next.has_value(); next = cursor()) {
			keys.push_back(std::to_integer<uint8_t>(next->key.front()));
		}
		EXPECT_EQ(keys, (std::vector<uint8_t>{0x10U, 0x20U, 0x30U, 0x40U, 0x50U}));
	}
}

TEST(NativeBulkRunMergerTest, OrdersSourcesWhoseInitialKeysArriveInReversePriorityOrder) {
	// The merge heap receives its cursors in caller order, which need not match
	// key order. This exercises the comparator's reverse-priority path while
	// preserving the public guarantee of a strictly ordered rewindable source.
	const std::array<OrderedBTreeEntrySource, 3U> inputs{source({entry(0x50U, 55U)}),
														 source({entry(0x10U, 11U), entry(0x40U, 44U)}),
														 source({entry(0x20U, 22U), entry(0x30U, 33U)})};
	const OrderedBTreeEntrySource merged = NativeBulkRunMerger::orderedSource(inputs, 3U, 5U);

	OrderedBTreeEntryCursor cursor = merged();
	std::vector<uint8_t> keys;
	for (std::optional<BTreePageEntry> next = cursor(); next.has_value(); next = cursor()) {
		keys.push_back(std::to_integer<uint8_t>(next->key.front()));
	}
	EXPECT_EQ(keys, (std::vector<uint8_t>{0x10U, 0x20U, 0x30U, 0x40U, 0x50U}));
}

TEST(NativeBulkRunMergerTest, RejectsDuplicateUnorderedAndOverboundCandidateInputs) {
	const std::array<OrderedBTreeEntrySource, 2U> duplicate{source({entry(0x10U, 11U)}), source({entry(0x10U, 22U)})};
	const OrderedBTreeEntrySource duplicateMerged = NativeBulkRunMerger::orderedSource(duplicate, 2U, 2U);
	expectFormatError(
			[&] {
				OrderedBTreeEntryCursor cursor = duplicateMerged();
				static_cast<void>(cursor());
				static_cast<void>(cursor());
			},
			FormatError::FERR_INVALID_STATE);

	const std::array<OrderedBTreeEntrySource, 1U> unordered{source({entry(0x20U, 22U), entry(0x10U, 11U)})};
	const OrderedBTreeEntrySource unorderedMerged = NativeBulkRunMerger::orderedSource(unordered, 1U, 2U);
	expectFormatError(
			[&] {
				OrderedBTreeEntryCursor cursor = unorderedMerged();
				static_cast<void>(cursor());
				static_cast<void>(cursor());
			},
			FormatError::FERR_INVALID_STATE);

	const std::array<OrderedBTreeEntrySource, 1U> twoEntries{source({entry(0x10U, 11U), entry(0x20U, 22U)})};
	const OrderedBTreeEntrySource overbound = NativeBulkRunMerger::orderedSource(twoEntries, 1U, 1U);
	expectFormatError(
			[&] {
				OrderedBTreeEntryCursor cursor = overbound();
				static_cast<void>(cursor());
				static_cast<void>(cursor());
			},
			FormatError::FERR_INVALID_STATE);
}

TEST(NativeBulkRunMergerTest, RejectsInvalidCandidateSourceFactoriesBeforeReadingScratch) {
	const std::array<OrderedBTreeEntrySource, 0U> noInputs;
	expectFormatError([&] { static_cast<void>(NativeBulkRunMerger::orderedSource(noInputs, 1U, 1U)); },
					  FormatError::FERR_INVALID_HEADER);

	const std::array<OrderedBTreeEntrySource, 1U> emptyFactory{};
	expectFormatError([&] { static_cast<void>(NativeBulkRunMerger::orderedSource(emptyFactory, 1U, 1U)); },
					  FormatError::FERR_INVALID_HEADER);

	const std::array<OrderedBTreeEntrySource, 1U> missingCursor{[] { return OrderedBTreeEntryCursor{}; }};
	const OrderedBTreeEntrySource merged = NativeBulkRunMerger::orderedSource(missingCursor, 1U, 1U);
	expectFormatError([&] { static_cast<void>(merged()); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(NativeBulkRunStoreTest, RejectsInvalidScratchWriteReleaseAndRecoveryInputsBeforeMutation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());

	expectFormatError([&] { static_cast<void>(store.write(0U, fragment(), LogSequenceNumber(2U))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.write(71U, fragment(), {})); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.writeOpaquePayload(71U, {}, LogSequenceNumber(2U))); },
					  FormatError::FERR_INVALID_HEADER);
	const std::vector<std::vector<std::byte>> invalidPayloads{{std::byte{0x01U}}, {}};
	expectFormatError(
			[&] { static_cast<void>(store.writeOpaquePayloadBatch(71U, invalidPayloads, LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_HEADER);

	const std::array<NativeBulkRunFragmentLocation, 0U> noLocations;
	expectFormatError([&] { store.releaseBatch(noLocations, LogSequenceNumber(2U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { store.reclaimUnpublishedOnOpen({}); }, FormatError::FERR_INVALID_HEADER);

	const NativeBulkRunFragmentLocation location = store.write(71U, fragment(), LogSequenceNumber(2U));
	const std::array duplicate{location, location};
	expectFormatError([&] { store.releaseBatch(duplicate, LogSequenceNumber(4U)); }, FormatError::FERR_INVALID_HEADER);
	NativeBulkRunStoreOptions oneItemBatchOptions = storeOptions();
	oneItemBatchOptions.maximum_batch_fragments = 1U;
	NativeBulkRunStore oneItemBatchStore(physical, *metadata, oneItemBatchOptions);
	const std::vector<std::vector<std::byte>> twoPayloads{{std::byte{0x01U}}, {std::byte{0x02U}}};
	expectFormatError(
			[&] {
				static_cast<void>(oneItemBatchStore.writeOpaquePayloadBatch(71U, twoPayloads, LogSequenceNumber(4U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { oneItemBatchStore.releaseBatch(duplicate, LogSequenceNumber(4U)); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { oneItemBatchStore.releaseBatch(std::array{location}, {}); },
				  FormatError::FERR_INVALID_HEADER);
	NativeBulkRunFragmentLocation foreign = location;
	foreign.transaction_id = 72U;
	expectFormatError([&] { store.release(foreign, LogSequenceNumber(4U)); }, FormatError::FERR_INVALID_STATE);
	store.release(location, LogSequenceNumber(4U));
}

TEST_F(NativeBulkRunStoreTest, RejectsCheckpointLsnOverflowBeforeScratchPublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());

	expectFormatError(
			[&] {
				static_cast<void>(store.write(71U, fragment(),
						LogSequenceNumber(std::numeric_limits<uint64_t>::max())));
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(NativeBulkRunStoreTest, RejectsTerminalCheckpointBeforeReservingScratchIdentifiers) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const Superblock before = snapshot.superblock();
	snapshot.release();
	expectFormatError(
			[&] {
				static_cast<void>(store.write(71U, fragment(),
						LogSequenceNumber(std::numeric_limits<uint64_t>::max())));
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);

	snapshot = metadata->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, before.generation);
	EXPECT_EQ(snapshot.superblock().checkpoint_lsn, before.checkpoint_lsn);
	EXPECT_EQ(snapshot.superblock().next_logical_segment_id, before.next_logical_segment_id);
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U)));
}

TEST_F(NativeBulkRunStoreTest, StreamingWriterRejectsInvalidInputAndOneShotReuse) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());

	expectFormatError(
			[&] {
				static_cast<void>(NativeBulkRunStreamingWriter(
						store, 0U, {.maximum_buffered_bytes = 64U, .maximum_entries = 2U}, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	NativeBulkRunStreamingWriter ordered(store, 71U, {.maximum_buffered_bytes = 64U, .maximum_entries = 2U},
										 LogSequenceNumber(2U));
	ordered.add(entry(0x20U, 22U));
	expectFormatError([&] { ordered.add(entry(0x10U, 11U)); }, FormatError::FERR_INVALID_STATE);
	static_cast<void>(ordered.finish());
	expectFormatError([&] { ordered.add(entry(0x30U, 33U)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(ordered.finish()); }, FormatError::FERR_INVALID_STATE);

	NativeBulkRunStreamingWriter tooSmall(store, 72U, {.maximum_buffered_bytes = 1U, .maximum_entries = 1U},
									  LogSequenceNumber(20U));
	expectFormatError([&] { tooSmall.add(entry(0x10U, 11U)); }, FormatError::FERR_INVALID_STATE);

	// A writer must not publish an empty run, and its logical entry budget is
	// independent from the byte-buffer bound above.
	NativeBulkRunStreamingWriter empty(store, 73U, {.maximum_buffered_bytes = 64U, .maximum_entries = 1U},
								   LogSequenceNumber(40U));
	expectFormatError([&] { static_cast<void>(empty.finish()); }, FormatError::FERR_INVALID_STATE);

	NativeBulkRunStreamingWriter bounded(store, 74U, {.maximum_buffered_bytes = 64U, .maximum_entries = 1U},
									 LogSequenceNumber(42U));
	bounded.add(entry(0x10U, 11U));
	expectFormatError([&] { bounded.add(entry(0x20U, 22U)); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(NativeBulkRunStoreTest, StreamingWriterValidatesEveryResourceBoundBeforeAllocatingScratch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	auto reject = [&](NativeBulkRunStore &store, NativeBulkRunStreamingOptions options,
					  uint64_t transactionId, LogSequenceNumber lsn) {
		expectFormatError([&] { NativeBulkRunStreamingWriter writer(store, transactionId, options, lsn); },
						  FormatError::FERR_INVALID_HEADER);
	};
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const NativeBulkRunStreamingOptions valid{64U, 2U};
	reject(store, {0U, valid.maximum_entries}, 71U, LogSequenceNumber(2U));
	reject(store, {valid.maximum_buffered_bytes, 0U}, 71U, LogSequenceNumber(2U));
	reject(store, valid, 71U, {});
	reject(store, {RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES + 1U, 2U}, 71U, LogSequenceNumber(2U));

	NativeBulkRunStoreOptions shortFragment = storeOptions();
	// A store with a valid constructor can still be too small for the writer's
	// minimum manifest fan-out when its bound is reduced to one frame.
	shortFragment.maximum_fragment_bytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + RSS_NATIVE_BULK_RUN_MANIFEST_ENTRY_BYTES;
	NativeBulkRunStore fanoutStore(physical, *metadata, shortFragment);
	reject(fanoutStore, valid, 71U, LogSequenceNumber(2U));
}

TEST_F(NativeBulkRunStoreTest, SourceRejectsAStoredManifestAboveItsConfiguredDepth) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStoreOptions deepOptions = storeOptions();
	deepOptions.maximum_fragment_bytes = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
	NativeBulkRunStore deepStore(physical, *metadata, deepOptions);
	const NativeBulkRunFragmentLocation first = deepStore.write(71U, {{entry(0x10U, 11U)}}, LogSequenceNumber(2U));
	const NativeBulkRunFragmentLocation second = deepStore.write(71U, {{entry(0x20U, 22U)}}, LogSequenceNumber(4U));
	const NativeBulkRunManifest leaf = deepStore.writeManifest(71U, std::array{first, second}, LogSequenceNumber(6U));
	const NativeBulkRunManifest root =
			deepStore.writeManifestParent(71U, std::array{leaf.location}, LogSequenceNumber(8U));

	NativeBulkRunStoreOptions shallowOptions = deepOptions;
	shallowOptions.maximum_manifest_depth = 1U;
	NativeBulkRunStore shallowStore(physical, *metadata, shallowOptions);
	expectFormatError([&] { static_cast<void>(shallowStore.orderedSource(root.location)()); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(NativeBulkRunStoreTest, ManifestWritersRejectEmptyForeignAndMixedLevelReferencesBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const std::array<NativeBulkRunFragmentLocation, 0U> noLocations;
	expectFormatError([&] { static_cast<void>(store.writeManifest(71U, noLocations, LogSequenceNumber(2U))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store.writeManifestParent(71U, noLocations, LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_HEADER);

	const NativeBulkRunFragmentLocation first = store.write(71U, {{entry(0x10U, 11U)}}, LogSequenceNumber(2U));
	const NativeBulkRunFragmentLocation second = store.write(71U, {{entry(0x20U, 22U)}}, LogSequenceNumber(4U));
	const NativeBulkRunFragmentLocation foreign = store.write(72U, {{entry(0x30U, 33U)}}, LogSequenceNumber(6U));
	expectFormatError(
			[&] { static_cast<void>(store.writeManifest(71U, std::array{first, foreign}, LogSequenceNumber(8U))); },
			FormatError::FERR_INVALID_STATE);
	const NativeBulkRunManifest leaf = store.writeManifest(71U, std::array{first, second}, LogSequenceNumber(8U));
	expectFormatError(
			[&] { static_cast<void>(store.writeManifestParent(71U, std::array{foreign}, LogSequenceNumber(10U))); },
			FormatError::FERR_INVALID_STATE);
	const NativeBulkRunManifest parent = store.writeManifestParent(71U, std::array{leaf.location}, LogSequenceNumber(10U));
	expectFormatError([&] {
		static_cast<void>(store.writeManifestParent(71U, std::array{leaf.location, parent.location}, LogSequenceNumber(12U)));
	}, FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.readManifest(snapshot, leaf.location).level, 0U);
	EXPECT_EQ(store.readManifest(snapshot, parent.location).level, 1U);
	snapshot.release();
}

TEST_F(NativeBulkRunStoreTest, ReleaseManifestRecursivelyReclaimsFragmentsAndParentManifests) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const NativeBulkRunFragmentLocation first = store.write(71U, {{entry(0x10U, 11U)}}, LogSequenceNumber(20U));
	const NativeBulkRunFragmentLocation second = store.write(71U, {{entry(0x20U, 22U)}}, LogSequenceNumber(24U));
	const NativeBulkRunManifest left = store.writeManifest(71U, std::array{first, second}, LogSequenceNumber(28U));
	const NativeBulkRunManifest root =
			store.writeManifestParent(71U, std::array{left.location}, LogSequenceNumber(32U));
	store.releaseManifest(root.location, LogSequenceNumber(36U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	for (const SegmentId segmentId:
		 {first.segment_id, second.segment_id, left.location.segment_id, root.location.segment_id}) {
		EXPECT_EQ(metadata->findCatalog(snapshot, segmentId).state, SegmentState::SGS_OBSOLETE);
	}
	EXPECT_EQ(metadata->isPhysicalSlotAllocated(snapshot,
													first.segment_id.isValid() ? PhysicalSlotId(40U) : PhysicalSlotId{}),
				  false);
}

TEST_F(NativeBulkRunStoreTest, ReleaseManifestRejectsAliasedFragmentsAndChildrenBeforeReclaimingScratch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const NativeBulkRunFragmentLocation fragment =
			store.write(71U, {{entry(0x10U, 11U)}}, LogSequenceNumber(20U));

	NativeBulkRunManifest aliasedLeaf;
	aliasedLeaf.fragments = {fragment, fragment};
	aliasedLeaf.entry_count = 2U;
	aliasedLeaf.encoded_bytes = NativeBulkRunFragmentCodec::encode({{entry(0x10U, 11U)}}).size() * 2U;
	const NativeBulkRunFragmentLocation aliasedLeafLocation = store.writeOpaquePayload(
			71U, NativeBulkRunManifestCodec::encode(aliasedLeaf), LogSequenceNumber(22U));
	expectFormatError([&] { store.releaseManifest(aliasedLeafLocation, LogSequenceNumber(24U)); },
				  FormatError::FERR_INVALID_STATE);

	const NativeBulkRunManifest validLeaf =
			store.writeManifest(71U, std::array{fragment}, LogSequenceNumber(26U));
	NativeBulkRunManifest aliasedParent;
	aliasedParent.level = 1U;
	aliasedParent.children = {validLeaf.location, validLeaf.location};
	aliasedParent.entry_count = 2U;
	aliasedParent.encoded_bytes = validLeaf.encoded_bytes * 2U;
	const NativeBulkRunFragmentLocation aliasedParentLocation = store.writeOpaquePayload(
			71U, NativeBulkRunManifestCodec::encode(aliasedParent), LogSequenceNumber(28U));
	expectFormatError([&] { store.releaseManifest(aliasedParentLocation, LogSequenceNumber(30U)); },
				  FormatError::FERR_INVALID_STATE);

	NativeBulkRunManifest wrongLevelChild;
	wrongLevelChild.level = 1U;
	wrongLevelChild.children = {validLeaf.location};
	wrongLevelChild.entry_count = 1U;
	wrongLevelChild.encoded_bytes = validLeaf.encoded_bytes;
	const NativeBulkRunFragmentLocation wrongLevelChildLocation = store.writeOpaquePayload(
			71U, NativeBulkRunManifestCodec::encode(wrongLevelChild), LogSequenceNumber(32U));
	NativeBulkRunManifest wrongLevelParent;
	wrongLevelParent.level = 1U;
	wrongLevelParent.children = {wrongLevelChildLocation};
	wrongLevelParent.entry_count = 1U;
	wrongLevelParent.encoded_bytes = validLeaf.encoded_bytes;
	const NativeBulkRunFragmentLocation wrongLevelParentLocation = store.writeOpaquePayload(
			71U, NativeBulkRunManifestCodec::encode(wrongLevelParent), LogSequenceNumber(34U));
	expectFormatError([&] { store.releaseManifest(wrongLevelParentLocation, LogSequenceNumber(36U)); },
				  FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	for (const NativeBulkRunFragmentLocation &location:
			 {fragment, aliasedLeafLocation, validLeaf.location, aliasedParentLocation, wrongLevelChildLocation,
			  wrongLevelParentLocation}) {
		EXPECT_EQ(metadata->findCatalog(snapshot, location.segment_id).state, SegmentState::SGS_SEALED);
	}
	snapshot.release();
}

TEST_F(NativeBulkRunStoreTest, RejectsCheckpointOverflowBeforeAnyReleaseMutation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	const LogSequenceNumber maximumLsn(std::numeric_limits<uint64_t>::max());

	const NativeBulkRunFragmentLocation standalone = store.write(71U, fragment(), LogSequenceNumber(20U));
	expectFormatError([&] { store.release(standalone, maximumLsn); }, FormatError::FERR_ARITHMETIC_OVERFLOW);

	const NativeBulkRunFragmentLocation first = store.write(71U, {{entry(0x10U, 11U)}}, LogSequenceNumber(22U));
	const NativeBulkRunFragmentLocation second = store.write(71U, {{entry(0x20U, 22U)}}, LogSequenceNumber(24U));
	const NativeBulkRunManifest manifest = store.writeManifest(71U, std::array{first, second}, LogSequenceNumber(26U));
	expectFormatError([&] { store.releaseManifest(manifest.location, maximumLsn); }, FormatError::FERR_ARITHMETIC_OVERFLOW);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	for (const NativeBulkRunFragmentLocation &location: {standalone, first, second, manifest.location}) {
		const CatalogEntry entry = metadata->findCatalog(snapshot, location.segment_id);
		EXPECT_EQ(entry.state, SegmentState::SGS_SEALED);
		EXPECT_TRUE(metadata->isPhysicalSlotAllocated(snapshot, entry.physical_slot));
	}
}

TEST_F(NativeBulkRunStoreTest, ReadOnlyIntegrityScanFailsClosedOnUnpublishedScratch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	static_cast<void>(store.write(71U, fragment(), LogSequenceNumber(20U)));
	EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io_, *metadata).scan()), FormatException);
}

TEST(NativeBulkRunManifestCodecTest, RejectsCorruptionForeignReferencesAndInvalidDimensions) {
	const NativeBulkRunManifest manifest{
			{}, 0U, {{SegmentId(11U), Generation(1U), 71U}, {SegmentId(12U), Generation(1U), 71U}}, {}, 4U, 128U};
	const std::vector<std::byte> encoded = NativeBulkRunManifestCodec::encode(manifest);
	const NativeBulkRunFragmentLocation location{SegmentId(13U), Generation(1U), 71U};
	NativeBulkRunManifest expected = manifest;
	expected.location = location;
	EXPECT_EQ(NativeBulkRunManifestCodec::decode(encoded, location), expected);

	auto corruptChecksum = encoded;
	corruptChecksum[32] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::decode(corruptChecksum, location)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
	auto foreignOwner = encoded;
	LittleEndianCodec::writeU64(foreignOwner, 40U, 72U);
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::decode(foreignOwner, location)); },
					  FormatError::FERR_INVALID_HEADER);
	auto nonCanonicalPadding = encoded;
	LittleEndianCodec::writeU32(nonCanonicalPadding, 36U, 1U);
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::decode(nonCanonicalPadding, location)); },
					  FormatError::FERR_INVALID_HEADER);
	auto selfReference = encoded;
	LittleEndianCodec::writeU64(selfReference, RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES, location.segment_id.value());
	LittleEndianCodec::writeU32(
			selfReference, 32U,
			graph::utils::calculateCrc(selfReference.data() + RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES,
									   selfReference.size() - RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES));
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::decode(selfReference, location)); },
					  FormatError::FERR_INVALID_HEADER);
	auto missingReference = encoded;
	LittleEndianCodec::writeU64(missingReference, RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES, 0U);
	LittleEndianCodec::writeU32(missingReference, 32U,
							bulkRunCrc(std::span<const std::byte>(missingReference).subspan(
										RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES)));
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::decode(missingReference, location)); },
					  FormatError::FERR_INVALID_HEADER);

	// Exercise the independent aggregate guards while retaining a valid
	// manifest shape and reference table.
	auto zeroEntryCount = encoded;
	LittleEndianCodec::writeU64(zeroEntryCount, 16U, 0U);
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::decode(zeroEntryCount, location)); },
					  FormatError::FERR_INVALID_HEADER);
	auto zeroEncodedBytes = encoded;
	LittleEndianCodec::writeU64(zeroEncodedBytes, 24U, 0U);
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::decode(zeroEncodedBytes, location)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkRunManifestCodecTest, DeterministicWireFuzzEitherFailsClosedOrRoundTripsCanonicalBytes) {
	const NativeBulkRunManifest manifest{
			{}, 0U, {{SegmentId(11U), Generation(1U), 71U}, {SegmentId(12U), Generation(1U), 71U}}, {}, 4U, 128U};
	const std::vector<std::byte> encoded = NativeBulkRunManifestCodec::encode(manifest);
	const NativeBulkRunFragmentLocation location{SegmentId(13U), Generation(1U), 71U};
	std::mt19937_64 random(0x4E41544956455F52ULL);
	for (size_t trial = 0U; trial < 256U; ++trial) {
		auto mutated = encoded;
		const size_t offset = static_cast<size_t>(random() % mutated.size());
		mutated[offset] ^= static_cast<std::byte>((random() & 0xFFU) | 1U);
		if (offset >= RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES) {
			LittleEndianCodec::writeU32(
					mutated, 32U,
					graph::utils::calculateCrc(mutated.data() + RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES,
											   mutated.size() - RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES));
		}
		try {
			NativeBulkRunManifest decoded = NativeBulkRunManifestCodec::decode(mutated, location);
			decoded.location = {};
			const std::vector<std::byte> recoded = NativeBulkRunManifestCodec::encode(decoded);
			ASSERT_EQ(recoded.size(), mutated.size()) << "fuzz trial " << trial << ", mutation offset " << offset;
			for (size_t byte = 0U; byte < recoded.size(); ++byte) {
				EXPECT_EQ(recoded[byte], mutated[byte])
						<< "fuzz trial " << trial << ", mutation offset " << offset << ", differing byte " << byte;
			}
		} catch (const FormatException &) {
			// Random wire changes may only be rejected or round-trip as a canonical manifest.
		}
	}
}

TEST(NativeBulkRunManifestCodecTest, RejectsNonCanonicalShapesBeforeEncoding) {
	NativeBulkRunManifest manifest;
	manifest.fragments = {{SegmentId(11U), Generation(1U), 71U}};
	manifest.entry_count = 1U;
	manifest.encoded_bytes = 32U;

	NativeBulkRunManifest mixed = manifest;
	mixed.children = {{SegmentId(12U), Generation(1U), 71U}};
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::encode(mixed)); },
					  FormatError::FERR_INVALID_HEADER);

	NativeBulkRunManifest missingOwner = manifest;
	missingOwner.fragments.front().transaction_id = 0U;
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::encode(missingOwner)); },
					  FormatError::FERR_INVALID_HEADER);

	NativeBulkRunManifest missingAggregate = manifest;
	missingAggregate.entry_count = 0U;
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::encode(missingAggregate)); },
					  FormatError::FERR_INVALID_HEADER);

	NativeBulkRunManifest selfLocated = manifest;
	selfLocated.location = {SegmentId(13U), Generation(1U), 71U};
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::encode(selfLocated)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkRunManifestCodecTest, RejectsMixedLevelOwnershipAndAggregateShapesBeforeEncoding) {
	const NativeBulkRunFragmentLocation first{SegmentId(11U), Generation(1U), 71U};
	const NativeBulkRunFragmentLocation second{SegmentId(12U), Generation(1U), 72U};
	NativeBulkRunManifest parent{{}, 1U, {first}, {first}, 1U, 32U};
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::encode(parent)); },
					  FormatError::FERR_INVALID_HEADER);

	NativeBulkRunManifest zeroBytes{{}, 0U, {first}, {}, 1U, 0U};
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::encode(zeroBytes)); },
					  FormatError::FERR_INVALID_HEADER);

	NativeBulkRunManifest mixedOwners{{}, 0U, {first, second}, {}, 2U, 64U};
	expectFormatError([&] { static_cast<void>(NativeBulkRunManifestCodec::encode(mixedOwners)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(NativeBulkRunStoreTest, RejectsAnUnadvanceableCheckpointBeforeAllocatingScratch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, superblock());
	PhysicalSegmentAllocator physical(io_, *metadata);
	NativeBulkRunStore store(physical, *metadata, storeOptions());
	expectFormatError(
			[&] { static_cast<void>(store.write(71U, fragment(), LogSequenceNumber(std::numeric_limits<uint64_t>::max()))); },
			FormatError::FERR_ARITHMETIC_OVERFLOW);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U)));
	snapshot.release();
}

TEST(NativeBulkRunMergerTest, OrderedSourceFailsClosedForInvalidAndIncoherentInputCursors) {
	const OrderedBTreeEntrySource first = source({entry(0x10U, 11U), entry(0x20U, 22U)});
	const OrderedBTreeEntrySource second = source({entry(0x30U, 33U)});
	const OrderedBTreeEntrySource noSource;
	const OrderedBTreeEntrySource noCursor = [] { return OrderedBTreeEntryCursor{}; };
	const std::array<OrderedBTreeEntrySource, 0U> noInputs{};
	expectFormatError(
			[&] { static_cast<void>(NativeBulkRunMerger::orderedSource(noInputs, 1U, 1U)); },
			FormatError::FERR_INVALID_HEADER);
	const std::array oneInput{first};
	expectFormatError(
			[&] { static_cast<void>(NativeBulkRunMerger::orderedSource(oneInput, 0U, 1U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(NativeBulkRunMerger::orderedSource(oneInput, 1U, 0U)); },
			FormatError::FERR_INVALID_HEADER);
	const std::array twoInputs{first, second};
	expectFormatError(
			[&] { static_cast<void>(NativeBulkRunMerger::orderedSource(twoInputs, 1U, 2U)); },
			FormatError::FERR_INVALID_HEADER);
	const std::array missingFactory{noSource};
	expectFormatError(
			[&] { static_cast<void>(NativeBulkRunMerger::orderedSource(missingFactory, 1U, 1U)); },
			FormatError::FERR_INVALID_HEADER);
	const std::array missingCursor{noCursor};
	const OrderedBTreeEntrySource cursorSource = NativeBulkRunMerger::orderedSource(missingCursor, 1U, 1U);
	expectFormatError([&] { static_cast<void>(cursorSource()); }, FormatError::FERR_INVALID_STATE);

	const std::array unorderedInputs{source({entry(0x20U, 22U), entry(0x10U, 11U)})};
	const OrderedBTreeEntrySource unordered = NativeBulkRunMerger::orderedSource(unorderedInputs, 1U, 2U);
	OrderedBTreeEntryCursor unorderedCursor = unordered();
	expectFormatError([&] { static_cast<void>(unorderedCursor()); }, FormatError::FERR_INVALID_STATE);

	const std::array duplicateInputs{source({entry(0x10U, 11U)}), source({entry(0x10U, 12U)})};
	const OrderedBTreeEntrySource duplicate = NativeBulkRunMerger::orderedSource(duplicateInputs, 2U, 2U);
	OrderedBTreeEntryCursor duplicateCursor = duplicate();
	ASSERT_TRUE(duplicateCursor().has_value());
	expectFormatError([&] { static_cast<void>(duplicateCursor()); }, FormatError::FERR_INVALID_STATE);

	const OrderedBTreeEntrySource bounded = NativeBulkRunMerger::orderedSource(oneInput, 1U, 1U);
	OrderedBTreeEntryCursor boundedCursor = bounded();
	ASSERT_TRUE(boundedCursor().has_value());
	expectFormatError([&] { static_cast<void>(boundedCursor()); }, FormatError::FERR_INVALID_STATE);
}

TEST(NativeBulkRunMergerTest, OrderedSourceRejectsAnEmptyBTreeKeyBeforeExposingIt) {
	const std::array inputs{source({BTreePageEntry{std::vector<std::byte>{}, 11U}})};
	const OrderedBTreeEntrySource merged = NativeBulkRunMerger::orderedSource(inputs, 1U, 1U);

	expectFormatError([&] { static_cast<void>(merged()); }, FormatError::FERR_INVALID_STATE);
}
