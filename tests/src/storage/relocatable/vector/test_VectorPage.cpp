/**
 * @file test_VectorPage.cpp
 * @brief Fixed-record raw/PQ vector page tests for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPage.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	VectorPageHeader header(VectorStorageKind kind = VectorStorageKind::VSK_RAW) {
		VectorPageHeader result;
		result.index_id = 17;
		result.page_id = PageId(101);
		result.generation = Generation(1);
		result.kind = kind;
		result.record_bytes = 16;
		result.slot_capacity = 3;
		result.bitmap_bytes = 1;
		result.committed_lsn = LogSequenceNumber(2);
		if (kind == VectorStorageKind::VSK_PQ) {
			result.codebook_generation = Generation(1);
		}
		return result;
	}

	PageOwnerHeader owner(const VectorPageHeader &header) {
		PageOwnerHeader result;
		result.page_id = header.page_id;
		result.owner_id = header.index_id;
		result.kind = header.kind == VectorStorageKind::VSK_RAW ? PageOwnerKind::POK_VECTOR_RAW
																: PageOwnerKind::POK_VECTOR_PQ;
		result.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		result.generation = header.generation;
		result.committed_lsn = header.committed_lsn;
		return result;
	}

	std::vector<std::byte> record(std::byte marker) { return std::vector<std::byte>(16U, marker); }

	void recalculateChecksums(std::vector<std::byte> &bytes) {
		LittleEndianCodec::writeU32(bytes, 48U,
								 graph::utils::calculateCrc(bytes.data() + RSS_VECTOR_PAGE_HEADER_BYTES,
															bytes.size() - RSS_VECTOR_PAGE_HEADER_BYTES));
		LittleEndianCodec::writeU32(bytes, 52U, 0U);
		LittleEndianCodec::writeU32(bytes, 52U,
								 graph::utils::calculateCrc(bytes.data(), RSS_VECTOR_PAGE_HEADER_BYTES));
	}

} // namespace

TEST(RelocatableVectorPageTest, StoresFixedRawRecordsWithStableSlotOrdinals) {
	const VectorPageHeader initial = header();
	VectorPage page = VectorPage::create(owner(initial), initial);
	const uint16_t first = page.insert(record(std::byte{0x11}));
	const uint16_t second = page.insert(record(std::byte{0x22}));
	EXPECT_EQ(first, 0U);
	EXPECT_EQ(second, 1U);
	EXPECT_EQ(page.slotGeneration(first), 1U);
	EXPECT_EQ(page.slotGeneration(second), 1U);
	EXPECT_EQ(page.read(first), record(std::byte{0x11}));
	EXPECT_EQ(page.read(first, 1), record(std::byte{0x11}));
	page.overwrite(second, record(std::byte{0x33}));
	EXPECT_EQ(page.read(second), record(std::byte{0x33}));
	page.erase(first);
	EXPECT_EQ(page.header().live_count, 1U);
	EXPECT_EQ(page.insert(record(std::byte{0x44})), first);
	EXPECT_EQ(page.slotGeneration(first), 2U);
	expectFormatError([&] { static_cast<void>(page.read(first, 1)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { page.overwrite(first, 1, record(std::byte{0x55})); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { page.erase(first, 1); }, FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(page.read(first, 2), record(std::byte{0x44}));
	expectFormatError([&] { page.overwrite(first, 2, std::vector<std::byte>(15U)); }, FormatError::FERR_INVALID_HEADER);
	page.overwrite(first, 2, record(std::byte{0x66}));
	EXPECT_EQ(page.read(first, 2), record(std::byte{0x66}));

	VectorPage reopened = VectorPage::open(owner(initial), page.serialize());
	EXPECT_EQ(reopened.header().live_count, 2U);
	EXPECT_EQ(reopened.slotGeneration(first), 2U);
	EXPECT_EQ(reopened.read(first, 2), record(std::byte{0x66}));
	EXPECT_EQ(reopened.read(second), record(std::byte{0x33}));
}

TEST(RelocatableVectorPageTest, EnforcesKindStrideCapacityAndOwnerIdentity) {
	VectorPageHeader raw = header();
	VectorPage page = VectorPage::create(owner(raw), raw);
	expectFormatError([&] { static_cast<void>(page.insert(std::vector<std::byte>(15U))); },
					  FormatError::FERR_INVALID_HEADER);
	static_cast<void>(page.insert(record(std::byte{0x11})));
	expectFormatError([&] { static_cast<void>(page.read(1)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(page.read(3)); }, FormatError::FERR_INVALID_STATE);
	static_cast<void>(page.insert(record(std::byte{0x22})));
	static_cast<void>(page.insert(record(std::byte{0x33})));
	expectFormatError([&] { static_cast<void>(page.insert(record(std::byte{0x44}))); },
					  FormatError::FERR_INVALID_STATE);

	VectorPageHeader pq = header(VectorStorageKind::VSK_PQ);
	VectorPage pqPage = VectorPage::create(owner(pq), pq);
	EXPECT_EQ(pqPage.insert(record(std::byte{0x55})), 0U);
	pq.codebook_generation = {};
	expectFormatError([&] { static_cast<void>(VectorPageHeaderCodec::encode(pq)); }, FormatError::FERR_INVALID_HEADER);
	raw.codebook_generation = Generation(1);
	expectFormatError([&] { static_cast<void>(VectorPageHeaderCodec::encode(raw)); }, FormatError::FERR_INVALID_HEADER);
	pq = header(VectorStorageKind::VSK_PQ);
	raw = header();
	PageOwnerHeader wrongOwner = owner(pq);
	wrongOwner.kind = PageOwnerKind::POK_VECTOR_RAW;
	expectFormatError([&] { static_cast<void>(VectorPage::create(wrongOwner, pq)); }, FormatError::FERR_INVALID_STATE);
	PageOwnerHeader retiredOwner = owner(raw);
	retiredOwner.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
	expectFormatError([&] { static_cast<void>(VectorPage::create(retiredOwner, raw)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(VectorPage::open(retiredOwner, VectorPage::create(owner(raw), raw).serialize()).header(), raw);
	retiredOwner.flags = static_cast<uint16_t>(RSS_PAGE_OWNER_KNOWN_FLAGS | 0x2U);
	expectFormatError(
			[&] { static_cast<void>(VectorPage::open(retiredOwner, VectorPage::create(owner(raw), raw).serialize())); },
			FormatError::FERR_INVALID_STATE);

	VectorPageHeader oversized = header();
	oversized.slot_capacity = 256;
	oversized.bitmap_bytes = 32;
	expectFormatError([&] { static_cast<void>(VectorPageHeaderCodec::encode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);
	VectorPageHeader unknownKind = header();
	unknownKind.kind = static_cast<VectorStorageKind>(99);
	expectFormatError([&] { static_cast<void>(VectorPageHeaderCodec::encode(unknownKind)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableVectorPageTest, RejectsCorruptChecksumsBitmapAndTrailingPadding) {
	const VectorPageHeader initial = header();
	VectorPage page = VectorPage::create(owner(initial), initial);
	static_cast<void>(page.insert(record(std::byte{0x7F})));
	std::vector<std::byte> corrupt = page.serialize();
	corrupt.back() = std::byte{1};
	expectFormatError([&] { static_cast<void>(VectorPage::open(owner(initial), corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = page.serialize();
	corrupt[RSS_VECTOR_PAGE_HEADER_BYTES] =
			static_cast<std::byte>(std::to_integer<uint8_t>(corrupt[RSS_VECTOR_PAGE_HEADER_BYTES]) | 0xF8U);
	// Restore checksums so validation reaches the unused bitmap-bit invariant.
	const uint32_t payloadCrc = graph::utils::calculateCrc(corrupt.data() + RSS_VECTOR_PAGE_HEADER_BYTES,
														   corrupt.size() - RSS_VECTOR_PAGE_HEADER_BYTES);
	LittleEndianCodec::writeU32(corrupt, 48, payloadCrc);
	LittleEndianCodec::writeU32(corrupt, 52, 0);
	LittleEndianCodec::writeU32(corrupt, 52,
								graph::utils::calculateCrc(corrupt.data(), RSS_VECTOR_PAGE_HEADER_BYTES));
	expectFormatError([&] { static_cast<void>(VectorPage::open(owner(initial), corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = page.serialize();
	LittleEndianCodec::writeU32(corrupt, RSS_VECTOR_PAGE_HEADER_BYTES + 1U, 0);
	const uint32_t zeroGenerationPayloadCrc = graph::utils::calculateCrc(
			corrupt.data() + RSS_VECTOR_PAGE_HEADER_BYTES, corrupt.size() - RSS_VECTOR_PAGE_HEADER_BYTES);
	LittleEndianCodec::writeU32(corrupt, 48, zeroGenerationPayloadCrc);
	LittleEndianCodec::writeU32(corrupt, 52, 0);
	LittleEndianCodec::writeU32(corrupt, 52,
								graph::utils::calculateCrc(corrupt.data(), RSS_VECTOR_PAGE_HEADER_BYTES));
	expectFormatError([&] { static_cast<void>(VectorPage::open(owner(initial), corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	VectorPage empty = VectorPage::create(owner(initial), initial);
	corrupt = empty.serialize();
	LittleEndianCodec::writeU32(corrupt, RSS_VECTOR_PAGE_HEADER_BYTES + 1U, std::numeric_limits<uint32_t>::max());
	const uint32_t exhaustedPayloadCrc = graph::utils::calculateCrc(corrupt.data() + RSS_VECTOR_PAGE_HEADER_BYTES,
																	corrupt.size() - RSS_VECTOR_PAGE_HEADER_BYTES);
	LittleEndianCodec::writeU32(corrupt, 48, exhaustedPayloadCrc);
	LittleEndianCodec::writeU32(corrupt, 52, 0);
	LittleEndianCodec::writeU32(corrupt, 52,
								graph::utils::calculateCrc(corrupt.data(), RSS_VECTOR_PAGE_HEADER_BYTES));
	VectorPage exhausted = VectorPage::open(owner(initial), corrupt);
	expectFormatError([&] { static_cast<void>(exhausted.insert(record(std::byte{0x33}))); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST(RelocatableVectorPageTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	for (const VectorStorageKind kind : {VectorStorageKind::VSK_RAW, VectorStorageKind::VSK_PQ}) {
		const VectorPageHeader initial = header(kind);
		VectorPage page = VectorPage::create(owner(initial), initial);
		static_cast<void>(page.insert(record(std::byte{0x11U})));
		static_cast<void>(page.insert(record(std::byte{0x22U})));
		const std::vector<std::byte> canonical = page.serialize();
		ASSERT_EQ(canonical.size(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);

		for (size_t byteOffset = 0U; byteOffset < canonical.size(); ++byteOffset) {
			for (uint8_t bit = 0U; bit < 8U; ++bit) {
				auto mutation = canonical;
				mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
				recalculateChecksums(mutation);
				try {
					const VectorPage decoded = VectorPage::open(owner(initial), mutation);
					if (decoded.serialize() != mutation) {
						ADD_FAILURE() << "vector page mutation was normalized for "
									  << (kind == VectorStorageKind::VSK_RAW ? "raw" : "PQ") << " vector page at byte "
									  << byteOffset << ", bit " << static_cast<unsigned>(bit);
						return;
					}
				} catch (const FormatException &) {
					// Every non-canonical page image must fail at the typed format boundary.
				}
			}
		}
	}
}

TEST(RelocatableVectorPageTest, RejectsMalformedLayoutsAndSupportsMoveOnlyOwnership) {
	VectorPageHeader invalid = header();
	invalid.slot_capacity = 0;
	expectFormatError([&] { static_cast<void>(VectorPageHeaderCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);

	const VectorPageHeader initial = header();
	const VectorPageHeaderCodec::Bytes encoded = VectorPageHeaderCodec::encode(initial);
	expectFormatError(
			[&] {
				static_cast<void>(
						VectorPageHeaderCodec::decode(std::span(encoded).first(RSS_VECTOR_PAGE_HEADER_BYTES - 1U)));
			},
			FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> oversized(encoded.begin(), encoded.end());
	oversized.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(VectorPageHeaderCodec::decode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);
	VectorPageHeaderCodec::Bytes reserved = encoded;
	LittleEndianCodec::writeU16(reserved, 10, 1);
	expectFormatError([&] { static_cast<void>(VectorPageHeaderCodec::decode(reserved)); },
					  FormatError::FERR_RESERVED_BITS);

	VectorPage page = VectorPage::create(owner(initial), initial);
	static_cast<void>(page.insert(record(std::byte{0x12})));
	expectFormatError([&] { page.overwrite(0, std::vector<std::byte>(15U)); }, FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> truncated = page.serialize();
	truncated.pop_back();
	expectFormatError([&] { static_cast<void>(VectorPage::open(owner(initial), truncated)); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> expanded = page.serialize();
	expanded.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(VectorPage::open(owner(initial), expanded)); },
					  FormatError::FERR_INVALID_HEADER);

	VectorPage moved = std::move(page);
	VectorPage assigned = VectorPage::create(owner(initial), initial);
	assigned = std::move(moved);
	const auto moveAssign = [](VectorPage &destination, VectorPage &source) { destination = std::move(source); };
	moveAssign(assigned, assigned);
	EXPECT_EQ(assigned.read(0), record(std::byte{0x12}));

	VectorPageHeader byteAligned = initial;
	byteAligned.slot_capacity = 8;
	byteAligned.bitmap_bytes = 1;
	VectorPage aligned = VectorPage::create(owner(byteAligned), byteAligned);
	EXPECT_EQ(aligned.insert(record(std::byte{0x23})), 0U);
}

TEST(RelocatableVectorPageTest, MaterializesAndReopensARawVectorPageThroughOneSharedPool) {
	const auto uuid = boost::uuids::random_generator()();
	const std::filesystem::path path =
			std::filesystem::temp_directory_path() / ("relocatable_vector_page_" + to_string(uuid));
	auto file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(file->is_open());
	auto io = std::make_shared<graph::storage::StorageIO>(file, graph::storage::INVALID_FILE_HANDLE,
											  graph::storage::INVALID_FILE_HANDLE);
	Superblock superblock;
	superblock.database_id = DatabaseId(0x1020304050607080ULL);
	superblock.generation = Generation(1);
	superblock.checkpoint_lsn = LogSequenceNumber(1);
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock);
	VectorPageStore store(io, *metadata);
	const VectorPageHeader initial = header();
	VectorPage page = VectorPage::create(owner(initial), initial);
	const uint16_t vectorSlot = page.insert(record(std::byte{0x66}));
	SharedPagePoolAllocationRequest request;
	request.segment_id = SegmentId(71);
	request.pool_kind = SegmentKind::SGK_VECTOR_RAW_POOL;
	request.pool_owner_id = 17;
	request.generation = Generation(1);
	request.page_id = initial.page_id;
	request.page_owner_id = initial.index_id;
	request.page_owner_kind = PageOwnerKind::POK_VECTOR_RAW;
	ASSERT_TRUE(
			store.allocate(request, page, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)).has_value());
	MetadataSnapshotPin beforeVectorLocation = metadata->openSnapshot();
	const VectorLocation vectorLocation{500, initial.page_id, vectorSlot, page.slotGeneration(vectorSlot)};
	static_cast<void>(metadata->upsertVectorLocations(std::span(&vectorLocation, 1), {}, LogSequenceNumber(3)));
	EXPECT_FALSE(metadata->findVectorLocationOptional(beforeVectorLocation, vectorLocation.vector_id).has_value());
	beforeVectorLocation.release();
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findVectorLocation(snapshot, vectorLocation.vector_id), vectorLocation);
	EXPECT_EQ(store.read(snapshot, initial.page_id).read(0), record(std::byte{0x66}));
	EXPECT_EQ(store.read(snapshot, initial.page_id).read(vectorSlot, vectorLocation.slot_generation),
			  record(std::byte{0x66}));
	snapshot.release();

	VectorManifest manifest;
	manifest.index_id = initial.index_id;
	manifest.page_id = PageId(102);
	manifest.generation = Generation(1);
	manifest.committed_lsn = LogSequenceNumber(4);
	manifest.dimensions = 16;
	manifest.entries = {{initial.page_id, VectorStorageKind::VSK_RAW, 1}};
	PageOwnerHeader manifestOwner;
	manifestOwner.page_id = manifest.page_id;
	manifestOwner.owner_id = manifest.index_id;
	manifestOwner.kind = PageOwnerKind::POK_VECTOR_MANIFEST;
	manifestOwner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	manifestOwner.generation = manifest.generation;
	manifestOwner.committed_lsn = manifest.committed_lsn;
	const VectorManifestPage manifestPage = VectorManifestPage::create(manifestOwner, manifest);
	VectorManifestStore manifestStore(io, *metadata);
	SharedPagePoolAllocationRequest manifestRequest;
	manifestRequest.segment_id = SegmentId(72);
	manifestRequest.pool_kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
	manifestRequest.pool_owner_id = initial.index_id;
	manifestRequest.generation = Generation(1);
	manifestRequest.page_id = manifest.page_id;
	manifestRequest.page_owner_id = initial.index_id;
	manifestRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_MANIFEST;
	ASSERT_TRUE(manifestStore
						.allocate(manifestRequest, manifestPage, PhysicalSlotId(41), PhysicalSlotId(41),
								  LogSequenceNumber(4))
						.has_value());
	const VectorManifestLocation manifestLocation{initial.index_id, manifest.page_id, manifest.generation};
	MetadataSnapshotPin manifestSnapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findVectorManifest(manifestSnapshot, initial.index_id), manifestLocation);
	EXPECT_EQ(manifestStore.read(manifestSnapshot, initial.index_id).manifest(), manifest);
	manifestSnapshot.release();
	MetadataSnapshotPin oldVectorSnapshot = metadata->openSnapshot();
	VectorPageHeader destinationHeader = initial;
	destinationHeader.page_id = PageId(103);
	destinationHeader.generation = Generation(2);
	destinationHeader.committed_lsn = LogSequenceNumber(5);
	VectorPage destinationPage = VectorPage::create(owner(destinationHeader), destinationHeader);
	SharedPagePoolAllocationRequest destinationRequest;
	destinationRequest.segment_id = SegmentId(73);
	destinationRequest.pool_kind = SegmentKind::SGK_VECTOR_RAW_POOL;
	destinationRequest.pool_owner_id = initial.index_id;
	destinationRequest.generation = destinationHeader.generation;
	destinationRequest.page_id = destinationHeader.page_id;
	destinationRequest.page_owner_id = initial.index_id;
	destinationRequest.page_owner_kind = PageOwnerKind::POK_VECTOR_RAW;
	ASSERT_TRUE(store.allocate(destinationRequest, destinationPage, PhysicalSlotId(42), PhysicalSlotId(42),
							   LogSequenceNumber(5))
						.has_value());
	const VectorPage sourcePage = store.read(oldVectorSnapshot, initial.page_id);
	const SharedPagePoolAllocation relocation =
			store.evacuate(sourcePage, destinationRequest.segment_id, LogSequenceNumber(6));
	EXPECT_EQ(relocation.location.segment_id, destinationRequest.segment_id);
	EXPECT_EQ(relocation.location.generation, destinationHeader.generation);
	MetadataSnapshotPin relocatedSnapshot = metadata->openSnapshot();
	EXPECT_EQ(relocation.location, metadata->findPageLocation(relocatedSnapshot, initial.page_id));
	EXPECT_EQ(store.read(oldVectorSnapshot, initial.page_id).header().generation, initial.generation);
	EXPECT_EQ(store.read(oldVectorSnapshot, initial.page_id).read(vectorSlot, vectorLocation.slot_generation),
			  record(std::byte{0x66}));
	EXPECT_EQ(metadata->findVectorLocation(relocatedSnapshot, vectorLocation.vector_id), vectorLocation);
	EXPECT_EQ(metadata->findPageLocation(relocatedSnapshot, initial.page_id).segment_id, destinationRequest.segment_id);
	EXPECT_EQ(store.read(relocatedSnapshot, initial.page_id).header().generation, destinationHeader.generation);
	EXPECT_EQ(store.read(relocatedSnapshot, initial.page_id).header().committed_lsn, LogSequenceNumber(6));
	EXPECT_EQ(store.read(relocatedSnapshot, initial.page_id).slotGeneration(vectorSlot),
			  vectorLocation.slot_generation);
	EXPECT_EQ(store.read(relocatedSnapshot, initial.page_id).read(vectorSlot, vectorLocation.slot_generation),
			  record(std::byte{0x66}));
	EXPECT_EQ(manifestStore.read(relocatedSnapshot, initial.index_id).manifest(), manifest);
	expectFormatError([&] { static_cast<void>(store.evacuate(sourcePage, {}, LogSequenceNumber(7))); },
					  FormatError::FERR_INVALID_HEADER);
	oldVectorSnapshot.release();
	relocatedSnapshot.release();
	const VectorManifestLocation rawVectorAsManifest{99, initial.page_id, Generation(1)};
	expectFormatError(
			[&] {
				static_cast<void>(
						metadata->upsertVectorManifests(std::span(&rawVectorAsManifest, 1), {}, LogSequenceNumber(5)));
			},
			FormatError::FERR_INVALID_STATE);
	const VectorLocation invalidGeneration{501, initial.page_id, vectorSlot, 0};
	expectFormatError(
			[&] {
				static_cast<void>(
						metadata->upsertVectorLocations(std::span(&invalidGeneration, 1), {}, LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_STATE);
	const VectorLocation missingPage{501, PageId(999), vectorSlot, 1};
	expectFormatError(
			[&] {
				static_cast<void>(
						metadata->upsertVectorLocations(std::span(&missingPage, 1), {}, LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_STATE);
	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	VectorPageStore reopenedStore(io, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findVectorLocation(reopened, vectorLocation.vector_id), vectorLocation);
	EXPECT_EQ(reopenedMetadata->findVectorManifest(reopened, initial.index_id), manifestLocation);
	EXPECT_EQ(reopenedStore.read(reopened, initial.page_id).read(0), record(std::byte{0x66}));
	reopened.release();
	reopenedMetadata.reset();
	io.reset();
	std::error_code error;
	file->close();
	std::filesystem::remove(path, error);
}
