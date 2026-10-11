/**
 * @file test_LargeValueStore.cpp
 * @brief Durable publication, snapshot, range, and corruption tests for LargeValueStore.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/utils/Sha256.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

	[[nodiscard]] LargeValueWriteOptions writeOptions() {
		LargeValueWriteOptions result;
		result.first_data_slot = PhysicalSlotId(40U);
		result.last_data_slot = PhysicalSlotId(80U);
		result.first_manifest_slot = PhysicalSlotId(81U);
		result.last_manifest_slot = PhysicalSlotId(90U);
		result.kind = LargeValueKind::LVK_UTF8_TEXT;
		result.media_type = "text/plain";
		return result;
	}

	[[nodiscard]] std::vector<std::byte> bytes(size_t count) {
		std::vector<std::byte> result(count);
		for (size_t index = 0U; index < result.size(); ++index) {
			result[index] = static_cast<std::byte>((index * 37U + 11U) & 0xFFU);
		}
		return result;
	}

	// A deterministic high-entropy payload prevents a codec from relying on the
	// repeating test pattern to accidentally satisfy a compression policy.
	[[nodiscard]] std::vector<std::byte> incompressibleBytes(size_t count) {
		std::vector<std::byte> result(count);
		uint64_t state = 0x8C4F6B2D190EA753ULL;
		for (std::byte &byte: result) {
			state ^= state << 13U;
			state ^= state >> 7U;
			state ^= state << 17U;
			byte = static_cast<std::byte>(state & 0xFFU);
		}
		return result;
	}

	[[nodiscard]] uint64_t physicalPageOffset(const MetadataSnapshotPin &snapshot, RelocatableMetadataStore &metadata,
											  PageId pageId) {
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		return PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
														   pool.physical_slot) +
			   static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
	}

	void overwritePagePayload(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
							  const MetadataSnapshotPin &snapshot, PageId pageId, std::span<const std::byte> payload) {
		ASSERT_EQ(payload.size(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		const uint64_t offset = physicalPageOffset(snapshot, metadata, pageId) + RSS_PAGE_OWNER_HEADER_BYTES;
		io->writeAt(offset, payload.data(), payload.size());
		io->sync();
	}

	void overwritePageOwnerHeader(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
								  const MetadataSnapshotPin &snapshot, PageId pageId, const PageOwnerHeader &owner) {
		const auto bytes = PageOwnerHeaderCodec::encode(owner);
		io->writeAt(physicalPageOffset(snapshot, metadata, pageId), bytes.data(), bytes.size());
		io->sync();
	}

	void overwriteContentIndexEntries(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
									  const MetadataSnapshotPin &snapshot, std::vector<BTreePageEntry> entries) {
		const IndexRootLocation root = metadata.findIndexRoot(snapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
		SharedPagePoolAllocator pools(io, metadata);
		const SharedPagePoolContents contents = pools.read(snapshot, root.root_page_id);
		const BTreePage original = BTreePage::open(contents.owner, contents.payload);
		ASSERT_EQ(original.header().kind, BTreeNodeKind::BPN_LEAF);
		BTreePageHeader header = original.header();
		header.entry_count = 0U;
		BTreePage replacement = BTreePage::create(contents.owner, header);
		for (BTreePageEntry &entry: entries) {
			replacement.insert(std::move(entry));
		}
		overwritePagePayload(io, metadata, snapshot, root.root_page_id, replacement.serialize());
	}

	[[nodiscard]] LargeValueManifest largeMultiLeafManifest(LargeValueStore &store, RelocatableMetadataStore &metadata,
															LargeValueWriteResult &written) {
		LargeValueWriteOptions options = writeOptions();
		options.extent_class = LargeValueExtentClass::LVEC_16K;
		const size_t inputBytes =
				(static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) + 1U) * RSS_LARGE_VALUE_EXTENT_16K_BYTES +
				19U;
		written = store.write(bytes(inputBytes), options, LogSequenceNumber(2U));
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const LargeValueManifest root = store.manifest(snapshot, written.reference);
		snapshot.release();
		return root;
	}

	void allocateLargeValueDataDestination(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
										   const LargeValueRef &reference, SegmentId segmentId, PageId pageId,
										   Generation poolGeneration, PhysicalSlotId slot,
										   LogSequenceNumber checkpointLsn) {
		SharedPagePoolAllocator pools(io, metadata);
		const PageOwnerHeader owner{pageId,		  reference.value_id.value(),		PageOwnerKind::POK_LARGE_VALUE_DATA,
									0U,			  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, poolGeneration,
									checkpointLsn};
		const LargeValueDataPage spare{reference.value_id, pageId, reference.generation, 0U, 0U, 1U, 1U, 0U,
									   {std::byte{0xE1U}}};
		SharedPagePoolAllocationRequest request;
		request.segment_id = segmentId;
		request.pool_kind = SegmentKind::SGK_LARGE_VALUE_DATA_POOL;
		request.pool_owner_id = RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID;
		request.generation = poolGeneration;
		request.page_id = pageId;
		request.page_owner_id = reference.value_id.value();
		request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_DATA;
		const LargeValueDataFrame frame = LargeValueDataFrame::create(owner, spare);
		request.payload = frame.serialize();
		ASSERT_TRUE(pools.allocateNewPoolPage(request, slot, slot, checkpointLsn).has_value());
	}

	void allocateLargeValueManifestDestination(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
											   const LargeValueManifest &source, SegmentId segmentId, PageId pageId,
											   Generation poolGeneration, PhysicalSlotId slot,
											   LogSequenceNumber checkpointLsn) {
		SharedPagePoolAllocator pools(io, metadata);
		LargeValueManifest spare = source;
		spare.page_id = pageId;
		const PageOwnerHeader owner{
				pageId,		  source.value_id.value(),			PageOwnerKind::POK_LARGE_VALUE_MANIFEST,
				0U,			  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, poolGeneration,
				checkpointLsn};
		SharedPagePoolAllocationRequest request;
		request.segment_id = segmentId;
		request.pool_kind = SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
		request.pool_owner_id = RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
		request.generation = poolGeneration;
		request.page_id = pageId;
		request.page_owner_id = source.value_id.value();
		request.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
		const LargeValueManifestPage page = LargeValueManifestPage::create(owner, spare);
		request.payload = page.serialize();
		ASSERT_TRUE(pools.allocateNewPoolPage(request, slot, slot, checkpointLsn).has_value());
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest, LargeValueStorePublishesAnEmptyValueAndSurvivesReopen) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written = store.write({}, writeOptions(), LogSequenceNumber(2U));

	EXPECT_EQ(written.reference.value_id, LargeValueId(1U));
	EXPECT_EQ(written.manifest.logical_bytes, 0U);
	EXPECT_TRUE(written.manifest.chunks.empty());
	EXPECT_EQ(written.reference.content_hash, graph::utils::Sha256::digest({}));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findLargeValueManifest(snapshot, written.reference.value_id).manifest_page_id,
			  written.manifest.page_id);
	EXPECT_EQ(store.manifest(snapshot, written.reference), written.manifest);
	EXPECT_TRUE(store.read(snapshot, written.reference).empty());
	EXPECT_TRUE(store.readRange(snapshot, written.reference, 0U, 0U).empty());
	snapshot.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedStore(io, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_TRUE(reopenedStore.read(reopenedSnapshot, written.reference).empty());
}

TEST_F(RelocatableMetadataStoreTest, ResumesDurableLargeValueManifestTraversalAfterStableValueId) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult first = store.write(bytes(3U), writeOptions(), LogSequenceNumber(2U));
	const LargeValueWriteResult second = store.write(bytes(5U), writeOptions(), LogSequenceNumber(3U));
	static_cast<void>(store.write(bytes(7U), writeOptions(), LogSequenceNumber(4U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	std::vector<LargeValueManifestLocation> resumed;
	size_t metadataPages = 0U;
	EXPECT_FALSE(metadata->visitLargeValueManifestLocationsAfter(
			snapshot, first.reference.value_id.value(), 2U,
			[&resumed](const LargeValueManifestLocation &location) {
				resumed.push_back(location);
				return false;
			},
			&metadataPages));
	ASSERT_EQ(resumed.size(), 1U);
	EXPECT_EQ(resumed.front().value_id, second.reference.value_id);
	EXPECT_GT(metadataPages, 0U);
	snapshot.release();
}

TEST_F(RelocatableMetadataStoreTest,
	   LargeValueStoreFinalSyncFailureReportsUncertainDurabilityAndReopensTheCompleteImmutableValue) {
	file->flush();
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, superblock(1U, 1U));
		LargeValueStore store(nativeIo, *metadata);
		const std::vector<std::byte> first = bytes(17U);
		const std::vector<std::byte> second = bytes(19U);
		const LargeValueWriteResult firstWrite = store.write(first, writeOptions(), LogSequenceNumber(2U));

		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failFinalPublicationFence([&syncCalls](file_handle_t fd) {
				if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 1U) {
#ifdef _WIN32
					SetLastError(ERROR_DISK_FULL);
#else
					errno = ENOSPC;
#endif
					return -1;
				}
				return portable_fsync(fd);
			});
			EXPECT_THROW(static_cast<void>(store.write(second, writeOptions(), LogSequenceNumber(3U))),
						 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);

		// A final Superblock fence error has an indeterminate durability result. The
		// completed publication must still resolve as one immutable value, never a
		// visible Manifest with missing data or content-index state.
		MetadataSnapshotPin current = metadata->openSnapshot();
		const LargeValueManifest secondManifest = store.manifest(current, LargeValueId(2U));
		EXPECT_EQ(metadata->findLargeValueManifest(current, LargeValueId(1U)).manifest_page_id,
				  firstWrite.manifest.page_id);
		EXPECT_EQ(store.read(current, firstWrite.reference), first);
		EXPECT_EQ(store.read(current, secondManifest.reference()), second);
		EXPECT_EQ(current.superblock().next_large_value_id, 3U);
		current.release();

		const LargeValueWriteResult retry = store.write(second, writeOptions(), LogSequenceNumber(3U));
		EXPECT_TRUE(retry.content_deduplicated);
		EXPECT_EQ(retry.reference, secondManifest.reference());

		metadata.reset();
		auto reopenedMetadata = RelocatableMetadataStore::open(nativeIo);
		LargeValueStore reopenedStore(nativeIo, *reopenedMetadata);
		MetadataSnapshotPin recovered = reopenedMetadata->openSnapshot();
		EXPECT_EQ(reopenedStore.read(recovered, firstWrite.reference), first);
		EXPECT_EQ(reopenedStore.read(recovered, secondManifest.reference()), second);
		EXPECT_EQ(recovered.superblock().next_large_value_id, 3U);
		recovered.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreMetadataDiskFullRetainsPublishedValueAndCanRetryAfterReopen) {
	file->flush();
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, superblock(1U, 1U));
		LargeValueStore store(nativeIo, *metadata);
		const std::vector<std::byte> first = bytes(17U);
		const std::vector<std::byte> second = bytes(19U);
		const LargeValueWriteResult firstWrite = store.write(first, writeOptions(), LogSequenceNumber(2U));
		MetadataSnapshotPin before = metadata->openSnapshot();
		const uint64_t metadataPoolOffset = before.superblock().data_region_offset;
		before.release();

		{
			StorageIO::ScopedNativeWriteHookForTesting failMetadataCandidate(
					[metadataPoolOffset](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						const uint64_t absoluteOffset = static_cast<uint64_t>(offset);
						if (absoluteOffset >= metadataPoolOffset + RSS_PAGE_SIZE &&
							absoluteOffset < metadataPoolOffset + RSS_SEGMENT_SIZE && byteCount == RSS_PAGE_SIZE) {
#ifdef _WIN32
							SetLastError(ERROR_DISK_FULL);
#else
							errno = ENOSPC;
#endif
							return static_cast<pwrite_ssize_t>(-1);
						}
						return portable_pwrite(fd, bytes, byteCount, offset);
					});
			EXPECT_THROW(static_cast<void>(store.write(second, writeOptions(), LogSequenceNumber(3U))),
						 std::runtime_error);
		}

		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_EQ(store.read(current, firstWrite.reference), first);
		EXPECT_THROW(static_cast<void>(store.manifest(current, LargeValueId(2U))), FormatException);
		EXPECT_EQ(current.superblock().next_large_value_id, 2U);
		current.release();
		metadata.reset();

		auto reopenedMetadata = RelocatableMetadataStore::open(nativeIo);
		LargeValueStore reopenedStore(nativeIo, *reopenedMetadata);
		MetadataSnapshotPin recovered = reopenedMetadata->openSnapshot();
		EXPECT_EQ(reopenedStore.read(recovered, firstWrite.reference), first);
		EXPECT_THROW(static_cast<void>(reopenedStore.manifest(recovered, LargeValueId(2U))), FormatException);
		EXPECT_EQ(recovered.superblock().next_large_value_id, 2U);
		recovered.release();

		const LargeValueWriteResult retry = reopenedStore.write(second, writeOptions(), LogSequenceNumber(3U));
		EXPECT_FALSE(retry.content_deduplicated);
		EXPECT_EQ(retry.reference.value_id, LargeValueId(2U));
		MetadataSnapshotPin published = reopenedMetadata->openSnapshot();
		EXPECT_EQ(reopenedStore.read(published, firstWrite.reference), first);
		EXPECT_EQ(reopenedStore.read(published, retry.reference), second);
		published.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreAllocatesDataAndManifestPoolsFromOneCandidateRange) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.first_manifest_slot = options.first_data_slot;
	options.last_manifest_slot = options.last_data_slot;
	const LargeValueWriteResult written =
			store.write(bytes(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 1U), options, LogSequenceNumber(2U));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const LargeValueManifest manifest = store.manifest(snapshot, written.reference);
	ASSERT_FALSE(manifest.chunks.empty());
	const PageLocation dataLocation = metadata->findPageLocation(snapshot, manifest.chunks.front().first_data_page_id);
	const PageLocation manifestLocation = metadata->findPageLocation(snapshot, manifest.page_id);
	const CatalogEntry dataPool = metadata->findCatalog(snapshot, dataLocation.segment_id);
	const CatalogEntry manifestPool = metadata->findCatalog(snapshot, manifestLocation.segment_id);
	EXPECT_EQ(dataPool.kind, SegmentKind::SGK_LARGE_VALUE_DATA_POOL);
	EXPECT_EQ(manifestPool.kind, SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL);
	EXPECT_EQ(manifestPool.physical_slot.value(), dataPool.physical_slot.value() + 1U);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreDeduplicatesFullContentIdentityAcrossReopen) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input = bytes(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 71U);
	const LargeValueWriteResult first = store.write(input, writeOptions(), LogSequenceNumber(2U));
	EXPECT_FALSE(first.content_deduplicated);
	EXPECT_EQ(first.dedup_candidates_scanned, 0U);
	MetadataSnapshotPin beforeDedup = metadata->openSnapshot();
	const Superblock firstGeneration = beforeDedup.superblock();
	ASSERT_TRUE(metadata->findIndexRootOptional(beforeDedup, RSS_LARGE_VALUE_CONTENT_INDEX_ID).has_value());
	beforeDedup.release();

	LargeValueWriteOptions alternateCodec = writeOptions();
	alternateCodec.compression.codec = LargeValueCodec::LVC_ZSTD;
	const LargeValueWriteResult duplicate = store.write(input, alternateCodec, LogSequenceNumber(3U));
	EXPECT_EQ(duplicate.reference, first.reference);
	EXPECT_EQ(duplicate.manifest, first.manifest);
	EXPECT_EQ(duplicate.superblock, firstGeneration);
	EXPECT_TRUE(duplicate.content_deduplicated);
	EXPECT_EQ(duplicate.dedup_candidates_scanned, 1U);

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedStore(io, *reopened);
	const LargeValueWriteResult afterReopen = reopenedStore.write(input, writeOptions(), LogSequenceNumber(4U));
	EXPECT_EQ(afterReopen.reference, first.reference);
	EXPECT_EQ(afterReopen.manifest, first.manifest);
	EXPECT_TRUE(afterReopen.content_deduplicated);
	EXPECT_EQ(afterReopen.dedup_candidates_scanned, 1U);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(snapshot, afterReopen.reference), input);
	const BTreeIndex contentIndex(RSS_LARGE_VALUE_CONTENT_INDEX_ID, reopenedStore.contentIndex(), *reopened);
	EXPECT_EQ(contentIndex.scan(snapshot, std::nullopt, 2U).size(), 1U);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreGrowsThePersistentContentIndexForManyUniqueValues) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.last_data_slot = PhysicalSlotId(300U);
	options.first_manifest_slot = PhysicalSlotId(301U);
	options.last_manifest_slot = PhysicalSlotId(700U);
	constexpr size_t valueCount = 96U;
	std::vector<LargeValueWriteResult> written;
	written.reserve(valueCount);
	for (size_t ordinal = 0U; ordinal < valueCount; ++ordinal) {
		std::vector<std::byte> input = bytes(31U);
		input.front() = static_cast<std::byte>(ordinal);
		const LargeValueWriteResult result =
				store.write(input, options, LogSequenceNumber(static_cast<uint64_t>(ordinal) + 2U));
		EXPECT_FALSE(result.content_deduplicated);
		written.push_back(result);
	}

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const BTreeIndex contentIndex(RSS_LARGE_VALUE_CONTENT_INDEX_ID, store.contentIndex(), *metadata);
	EXPECT_EQ(contentIndex.scan(snapshot, std::nullopt, valueCount + 1U).size(), valueCount);
	const std::vector<std::byte> lastInput = [&] {
		std::vector<std::byte> input = bytes(31U);
		input.front() = static_cast<std::byte>(valueCount - 1U);
		return input;
	}();
	const LargeValueDedupLookupResult duplicate =
			store.findContentDuplicateWithStats(snapshot, lastInput, options.kind, options.media_type, 1U);
	ASSERT_TRUE(duplicate.reference.has_value());
	EXPECT_EQ(*duplicate.reference, written.back().reference);
	EXPECT_EQ(duplicate.candidates_scanned, 1U);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreDoesNotDeduplicateDifferentMediaTypesAndRejectsZeroBound) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input = bytes(41U);
	const LargeValueWriteResult text = store.write(input, writeOptions(), LogSequenceNumber(2U));
	LargeValueWriteOptions binary = writeOptions();
	binary.kind = LargeValueKind::LVK_BYTES;
	binary.media_type = "application/octet-stream";
	const LargeValueWriteResult bytesValue = store.write(input, binary, LogSequenceNumber(3U));
	EXPECT_NE(bytesValue.reference.value_id, text.reference.value_id);
	EXPECT_NE(bytesValue.reference.kind, text.reference.kind);
	EXPECT_NE(bytesValue.reference.media_type, text.reference.media_type);

	LargeValueWriteOptions invalid = writeOptions();
	invalid.maximum_dedup_candidates = 0U;
	expectFormatError([&] { static_cast<void>(store.write(input, invalid, LogSequenceNumber(4U))); },
					  FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(store.findContentDuplicateWithStats(snapshot, input, invalid.kind, invalid.media_type,
																	  std::numeric_limits<size_t>::max()));
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);
	snapshot.release();
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsMalformedPersistentContentIndexCandidateKeys) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input = bytes(41U);
	const LargeValueWriteResult written = store.write(input, writeOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const BTreeIndex contentIndex(RSS_LARGE_VALUE_CONTENT_INDEX_ID, store.contentIndex(), *metadata);
	std::vector<BTreePageEntry> entries = contentIndex.scan(snapshot, std::nullopt, 2U);
	ASSERT_EQ(entries.size(), 1U);
	entries.front().key.push_back(std::byte{0xA5U});
	overwriteContentIndexEntries(io, *metadata, snapshot, std::move(entries));

	expectFormatError(
			[&] {
				static_cast<void>(store.findContentDuplicateWithStats(snapshot, input, written.reference.kind,
																	  written.reference.media_type, 1U));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsContentIndexCandidateWithAnotherValuesImmutableIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> firstInput = bytes(37U);
	std::vector<std::byte> secondInput = bytes(43U);
	secondInput.front() ^= std::byte{0xFFU};
	const LargeValueWriteResult first = store.write(firstInput, writeOptions(), LogSequenceNumber(2U));
	const LargeValueWriteResult second = store.write(secondInput, writeOptions(), LogSequenceNumber(3U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const BTreeIndex contentIndex(RSS_LARGE_VALUE_CONTENT_INDEX_ID, store.contentIndex(), *metadata);
	std::vector<BTreePageEntry> entries = contentIndex.scan(snapshot, std::nullopt, 3U);
	ASSERT_EQ(entries.size(), 2U);
	const std::vector<std::byte> firstKey = store.contentIndexKey(first.reference);
	const std::vector<std::byte> secondKey = store.contentIndexKey(second.reference);
	auto firstEntry = std::find_if(entries.begin(), entries.end(),
								   [&firstKey](const BTreePageEntry &entry) { return entry.key == firstKey; });
	ASSERT_NE(firstEntry, entries.end());
	ASSERT_EQ(firstEntry->key.size(), secondKey.size());
	ASSERT_EQ(firstEntry->key.size(), firstKey.size());
	std::copy(secondKey.end() - static_cast<std::ptrdiff_t>(sizeof(uint64_t)), secondKey.end(),
			  firstEntry->key.end() - static_cast<std::ptrdiff_t>(sizeof(uint64_t)));
	firstEntry->value_or_child = second.reference.value_id.value();
	overwriteContentIndexEntries(io, *metadata, snapshot, std::move(entries));

	expectFormatError(
			[&] {
				static_cast<void>(store.findContentDuplicateWithStats(snapshot, firstInput, first.reference.kind,
																	  first.reference.media_type, 1U));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsContentIndexCandidateWhoseKeyAndValueDisagree) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input = bytes(41U);
	const LargeValueWriteResult written = store.write(input, writeOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const BTreeIndex contentIndex(RSS_LARGE_VALUE_CONTENT_INDEX_ID, store.contentIndex(), *metadata);
	std::vector<BTreePageEntry> entries = contentIndex.scan(snapshot, std::nullopt, 2U);
	ASSERT_EQ(entries.size(), 1U);
	ASSERT_LT(entries.front().value_or_child, std::numeric_limits<uint64_t>::max());
	++entries.front().value_or_child;
	overwriteContentIndexEntries(io, *metadata, snapshot, std::move(entries));

	expectFormatError(
			[&] {
				static_cast<void>(store.findContentDuplicateWithStats(snapshot, input, written.reference.kind,
																	  written.reference.media_type, 1U));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsInvalidWritePoliciesBeforePublishingMetadata) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input = bytes(19U);
	const auto expectRejected = [&](LargeValueWriteOptions options,
									LogSequenceNumber checkpointLsn = LogSequenceNumber(2U)) {
		expectFormatError([&] { static_cast<void>(store.write(input, options, checkpointLsn)); },
						  FormatError::FERR_INVALID_HEADER);
		const MetadataSnapshotPin snapshot = metadata->openSnapshot();
		EXPECT_EQ(snapshot.superblock().generation, Generation(1U));
	};

	LargeValueWriteOptions invalid = writeOptions();
	invalid.first_data_slot = PhysicalSlotId{};
	expectRejected(invalid);
	invalid = writeOptions();
	invalid.last_manifest_slot = PhysicalSlotId{};
	expectRejected(invalid);
	invalid = writeOptions();
	invalid.first_data_slot = PhysicalSlotId(81U);
	invalid.last_data_slot = PhysicalSlotId(80U);
	expectRejected(invalid);
	invalid = writeOptions();
	invalid.kind = static_cast<LargeValueKind>(0xFFFFU);
	expectRejected(invalid);
	expectRejected(writeOptions(), LogSequenceNumber{});

	invalid = writeOptions();
	invalid.media_type.clear();
	expectRejected(invalid);
	invalid = writeOptions();
	invalid.media_type.assign(RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES + 1U, 'a');
	expectRejected(invalid);
	invalid = writeOptions();
	invalid.media_type = "text/\x01plain";
	expectRejected(invalid);
	invalid = writeOptions();
	invalid.media_type = std::string{"text/"} + static_cast<char>(0x7FU);
	expectRejected(invalid);
	invalid = writeOptions();
	invalid.compression.minimum_savings_percent = 101U;
	expectRejected(invalid);
	invalid = writeOptions();
	invalid.compression.zstd_level = 0;
	expectRejected(invalid);
	invalid = writeOptions();
	invalid.compression.zstd_level = 23;
	expectRejected(invalid);
	invalid = writeOptions();
	invalid.compression.codec = static_cast<LargeValueCodec>(0xFFFFU);
	expectRejected(invalid);
	invalid = writeOptions();
	invalid.extent_class = static_cast<LargeValueExtentClass>(0xFFFFU);
	expectRejected(invalid);

	const LargeValueCompressionOptions compression{};
	EXPECT_EQ(store.plannedDataPageCount(input, compression, LargeValueExtentClass::LVEC_64K), 1U);
	EXPECT_EQ(store.plannedDataPageCount(input, compression, LargeValueExtentClass::LVEC_128K), 1U);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsInvalidPublicReferencesAndRetirementRequests) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input = bytes(29U);

	expectFormatError([&] { static_cast<void>(store.contentIndexKey({})); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.planReference(LargeValueId{}, Generation(1U), input, LargeValueKind::LVK_BYTES,
													  "application/octet-stream", {}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.planReference(LargeValueId(1U), Generation(1U), input,
													  static_cast<LargeValueKind>(0xFFFFU), "application/octet-stream",
													  {}));
			},
			FormatError::FERR_INVALID_HEADER);

	MetadataSnapshotPin emptySnapshot = metadata->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(store.findContentDuplicateWithStats(
						emptySnapshot, input, static_cast<LargeValueKind>(0xFFFFU), "application/octet-stream", 1U));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.findContentDuplicateWithStats(emptySnapshot, input, LargeValueKind::LVK_BYTES,
																	  "application/octet-stream", 0U));
			},
			FormatError::FERR_INVALID_HEADER);
	emptySnapshot.release();

	const LargeValueWriteResult written = store.write(input, writeOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	LargeValueRef staleReference = written.reference;
	staleReference.generation = Generation(written.reference.generation.value() + 1U);
	expectFormatError([&] { static_cast<void>(store.read(snapshot, staleReference)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(store.planPageRetirement(snapshot, PageId{}, written.reference.value_id,
														   PageOwnerKind::POK_LARGE_VALUE_DATA));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.planPageRetirement(snapshot, written.manifest.page_id,
														   written.reference.value_id,
														   PageOwnerKind::POK_LARGE_VALUE_DATA));
			},
			FormatError::FERR_INVALID_STATE);
	snapshot.release();

	expectFormatError([&] { static_cast<void>(store.evacuate(LargeValueId{}, SegmentId(71U), LogSequenceNumber(3U))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.evacuatePage(PageId{}, SegmentId(71U), LogSequenceNumber(3U))); },
					  FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin invalidKindSnapshot = metadata->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(store.planPageRetirement(invalidKindSnapshot, PageId(1U), LargeValueId(1U),
														   static_cast<PageOwnerKind>(0xFFFFU)));
			},
			FormatError::FERR_INVALID_HEADER);
	invalidKindSnapshot.release();
	const std::array<LargeValueId, 0U> noValueIds{};
	const std::array<PageRetirement, 0U> noRetirements{};
	expectFormatError(
			[&] {
				static_cast<void>(store.retireUnreachablePages(Generation{}, noValueIds, noRetirements,
															   PhysicalSlotId(40U), PhysicalSlotId(80U),
															   LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array<LargeValueId, 2U> duplicateValueIds{{LargeValueId(1U), LargeValueId(1U)}};
	const std::array<PageRetirement, 1U> placeholderRetirement{
			{{{PageId(1U), SegmentId(1U), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U},
			  1U,
			  PageOwnerKind::POK_LARGE_VALUE_DATA,
			  LogSequenceNumber(1U)}}};
	expectFormatError(
			[&] {
				static_cast<void>(store.retireUnreachablePages(Generation(1U), duplicateValueIds, placeholderRetirement,
															   PhysicalSlotId(40U), PhysicalSlotId(80U),
															   LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest,
	   LargeValueContentIndexPlannerValidatesTheGraphPublicationBatchBeforeReservingPages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> firstBytes = bytes(29U);
	const std::vector<std::byte> secondBytes = bytes(31U);
	const LargeValueRef first = store.planReference(LargeValueId(1U), Generation(1U), firstBytes,
													LargeValueKind::LVK_UTF8_TEXT, "text/plain", {});
	const LargeValueRef second = store.planReference(LargeValueId(2U), Generation(1U), secondBytes,
													 LargeValueKind::LVK_UTF8_TEXT, "text/plain", {});
	const NativeGraphLargeValueMutation firstMutation{
			first,
			firstBytes,
			PhysicalSlotId(40U),
			PhysicalSlotId(80U),
			PhysicalSlotId(40U),
			PhysicalSlotId(80U),
			{},
			LargeValueExtentClass::LVEC_32K,
			store.plannedDataPageCount(firstBytes, {}, LargeValueExtentClass::LVEC_32K),
			store.plannedManifestPageCount(firstBytes, {}, LargeValueExtentClass::LVEC_32K)};
	const NativeGraphLargeValueMutation secondMutation{
			second,
			secondBytes,
			PhysicalSlotId(40U),
			PhysicalSlotId(80U),
			PhysicalSlotId(40U),
			PhysicalSlotId(80U),
			{},
			LargeValueExtentClass::LVEC_32K,
			store.plannedDataPageCount(secondBytes, {}, LargeValueExtentClass::LVEC_32K),
			store.plannedManifestPageCount(secondBytes, {}, LargeValueExtentClass::LVEC_32K)};
	const std::array<NativeGraphLargeValueMutation, 0U> noValues{};
	const std::array<NativeGraphLargeValueMutation, 2U> values{{firstMutation, secondMutation}};
	const std::array<NativeGraphLargeValueMutation, 2U> duplicateValues{{firstMutation, firstMutation}};
	NativeGraphLargeValueMutation invalidIdentity = secondMutation;
	invalidIdentity.reference.value_id = LargeValueId{};
	const std::array<NativeGraphLargeValueMutation, 1U> invalidValues{{invalidIdentity}};
	MetadataSnapshotPin snapshot = metadata->openSnapshot();

	expectFormatError(
			[&] {
				static_cast<void>(store.planNativeGraphContentIndexMutation(snapshot, noValues, PhysicalSlotId(40U),
																			PhysicalSlotId(80U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.planNativeGraphContentIndexMutation(snapshot, values, PhysicalSlotId{},
																			PhysicalSlotId(80U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.planNativeGraphContentIndexMutation(snapshot, values, PhysicalSlotId(81U),
																			PhysicalSlotId(80U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.planNativeGraphContentIndexMutation(snapshot, duplicateValues,
																			PhysicalSlotId(40U), PhysicalSlotId(80U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.planNativeGraphContentIndexMutation(snapshot, invalidValues,
																			PhysicalSlotId(40U), PhysicalSlotId(80U)));
			},
			FormatError::FERR_INVALID_HEADER);

	const LargeValueContentIndexPublicationPlan plan =
			store.planNativeGraphContentIndexMutation(snapshot, values, PhysicalSlotId(40U), PhysicalSlotId(80U));
	EXPECT_EQ(plan.mutation.index_id, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
	EXPECT_FALSE(plan.mutation.expected_root.has_value());
	EXPECT_TRUE(plan.preparer);
	EXPECT_EQ(plan.mutation.new_pool_requests.size(), 1U);
	snapshot.release();
}

TEST_F(RelocatableMetadataStoreTest,
	   MetadataCoordinatorRejectsLargeValuePublicationWithoutTheRequiredContentIndexMutation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input = bytes(29U);
	const LargeValueRef unpublishedReference{LargeValueId(1U),
											 Generation(1U),
											 static_cast<uint64_t>(input.size()),
											 graph::utils::Sha256::digest(input),
											 LargeValueKind::LVK_UTF8_TEXT,
											 LargeValueCodec::LVC_RAW,
											 "text/plain"};
	const NativeGraphLargeValueMutation unpublishedMutation{unpublishedReference,
															input,
															PhysicalSlotId(40U),
															PhysicalSlotId(40U),
															PhysicalSlotId(41U),
															PhysicalSlotId(41U),
															{},
															LargeValueExtentClass::LVEC_16K,
															1U,
															1U};
	const std::array<NativeGraphLargeValueMutation, 1U> unpublishedValues{{unpublishedMutation}};
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const NativeGraphPageTransactionWithLargeValuesPreparer noMaterializations =
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {};
	const ExistingPoolPagesRetirementPreparer noRetirementMaterializations =
			[](std::span<const ExistingPoolPageRetirementBatch>) {};

	expectFormatError(
			[&] {
				static_cast<void>(metadata->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						unpublishedValues, {}, LogSequenceNumber(2U), noMaterializations, {}));
			},
			FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin beforeWrite = metadata->openSnapshot();
	EXPECT_EQ(beforeWrite.superblock().generation, Generation(1U));
	EXPECT_FALSE(metadata->findLargeValueManifestOptional(beforeWrite, unpublishedReference.value_id).has_value());
	beforeWrite.release();

	const LargeValueWriteResult written = store.write(input, writeOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const Generation generation = snapshot.superblock().generation;
	const PageRetirement dataRetirement =
			store.planPageRetirement(snapshot, written.manifest.chunks.front().first_data_page_id,
									 written.reference.value_id, PageOwnerKind::POK_LARGE_VALUE_DATA);
	const PageRetirement manifestRetirement = store.planPageRetirement(
			snapshot, written.manifest.page_id, written.reference.value_id, PageOwnerKind::POK_LARGE_VALUE_MANIFEST);
	snapshot.release();

	const std::array<NativeGraphLargeValueMutation, 0U> noNewValues{};
	const std::array<PageRetirement, 2U> retirements{{dataRetirement, manifestRetirement}};
	const std::array<LargeValueId, 1U> erasedValueIds{{written.reference.value_id}};
	expectFormatError(
			[&] {
				static_cast<void>(metadata->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noNewValues, {}, LogSequenceNumber(3U), noMaterializations, {}, {}, {}, retirements,
						erasedValueIds, noRetirementMaterializations));
			},
			FormatError::FERR_INVALID_HEADER);

	MetadataSnapshotPin unchanged = metadata->openSnapshot();
	EXPECT_EQ(unchanged.superblock().generation, generation);
	EXPECT_EQ(store.read(unchanged, written.reference), input);
	unchanged.release();
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedStore(io, *reopened);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, generation);
	EXPECT_EQ(reopenedStore.read(recovered, written.reference), input);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreWritesAcrossPoolsAndReadsOnlyTheRequestedRange) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const size_t inputBytes =
			static_cast<size_t>(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES) * (RSS_PAGE_POOL_DATA_FRAME_COUNT + 2U) +
			17U;
	const std::vector<std::byte> input = bytes(inputBytes);
	const LargeValueWriteResult written = store.write(input, writeOptions(), LogSequenceNumber(2U));
	ASSERT_FALSE(written.manifest.chunks.empty());
	EXPECT_TRUE(written.manifest.children.empty());
	size_t materializedDataPages = 0U;
	for (const LargeValueChunk &chunk: written.manifest.chunks) {
		materializedDataPages += chunk.data_page_count;
	}
	EXPECT_EQ(materializedDataPages,
			  store.plannedDataPageCount(input, writeOptions().compression, writeOptions().extent_class));
	EXPECT_EQ(store.plannedManifestPageCount(input, writeOptions().compression, writeOptions().extent_class), 1U);
	EXPECT_GT(written.superblock.generation, Generation(1U));

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	const uint64_t rangeOffset = RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES - 9U;
	const uint64_t rangeLength = RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 23U;
	const std::vector<std::byte> expected(input.begin() + static_cast<std::ptrdiff_t>(rangeOffset),
										  input.begin() + static_cast<std::ptrdiff_t>(rangeOffset + rangeLength));
	EXPECT_EQ(store.readRange(oldSnapshot, written.reference, rangeOffset, rangeLength), expected);
	EXPECT_EQ(store.read(oldSnapshot, written.reference), input);

	const LargeValueWriteResult later = store.write(bytes(19U), writeOptions(), LogSequenceNumber(3U));
	EXPECT_EQ(store.read(oldSnapshot, written.reference), input);
	EXPECT_EQ(later.reference.value_id, LargeValueId(2U));
	oldSnapshot.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedStore(io, *reopened);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(snapshot, written.reference), input);
	EXPECT_EQ(reopenedStore.read(snapshot, later.reference), bytes(19U));
	expectFormatError(
			[&] { static_cast<void>(reopenedStore.readRange(snapshot, written.reference, input.size(), 1U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(reopenedStore.readRange(snapshot, written.reference, input.size() + 1U, 0U)); },
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreReusesSharedDataAndManifestPoolsAcrossValuesAndReopen) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> firstBytes = bytes(17U);
	const LargeValueWriteResult first = store.write(firstBytes, writeOptions(), LogSequenceNumber(2U));
	ASSERT_EQ(first.manifest.chunks.size(), 1U);
	const PageId firstDataPage = first.manifest.chunks.front().first_data_page_id;

	MetadataSnapshotPin beforeSecond = metadata->openSnapshot();
	const PageLocation firstLocation = metadata->findPageLocation(beforeSecond, firstDataPage);
	const CatalogEntry firstPool = metadata->findCatalog(beforeSecond, firstLocation.segment_id);
	ASSERT_EQ(firstPool.kind, SegmentKind::SGK_LARGE_VALUE_DATA_POOL);
	ASSERT_EQ(firstPool.owner_id, RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID);
	const PageLocation firstManifestLocation = metadata->findPageLocation(beforeSecond, first.manifest.page_id);
	const CatalogEntry firstManifestPool = metadata->findCatalog(beforeSecond, firstManifestLocation.segment_id);
	ASSERT_EQ(firstManifestPool.kind, SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL);
	ASSERT_EQ(firstManifestPool.owner_id, RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID);
	EXPECT_EQ(store.read(beforeSecond, first.reference), firstBytes);

	const std::vector<std::byte> secondBytes = bytes(29U);
	const LargeValueWriteResult second = store.write(secondBytes, writeOptions(), LogSequenceNumber(3U));
	ASSERT_EQ(second.manifest.chunks.size(), 1U);
	const PageId secondDataPage = second.manifest.chunks.front().first_data_page_id;
	MetadataSnapshotPin current = metadata->openSnapshot();
	const PageLocation secondLocation = metadata->findPageLocation(current, secondDataPage);
	const PageLocation secondManifestLocation = metadata->findPageLocation(current, second.manifest.page_id);
	EXPECT_EQ(secondLocation.segment_id, firstLocation.segment_id);
	EXPECT_EQ(secondManifestLocation.segment_id, firstManifestLocation.segment_id);
	const CatalogEntry secondPool = metadata->findCatalog(current, secondLocation.segment_id);
	EXPECT_EQ(secondPool.segment_id, firstPool.segment_id);
	EXPECT_EQ(secondPool.physical_slot, firstPool.physical_slot);
	EXPECT_EQ(secondPool.kind, firstPool.kind);
	EXPECT_EQ(secondPool.owner_id, firstPool.owner_id);
	EXPECT_EQ(secondPool.generation, firstPool.generation);
	EXPECT_EQ(secondPool.state, firstPool.state);
	EXPECT_EQ(secondPool.committed_lsn, firstPool.committed_lsn);
	const std::vector<CatalogEntry> currentDataPools = metadata->findCurrentLargeValueDataPagePools(current);
	const std::vector<CatalogEntry> currentManifestPools = metadata->findCurrentLargeValueManifestPagePools(current);
	ASSERT_EQ(currentDataPools.size(), 1U);
	ASSERT_EQ(currentManifestPools.size(), 1U);
	EXPECT_EQ(currentDataPools.front().segment_id, firstPool.segment_id);
	EXPECT_EQ(currentManifestPools.front().segment_id, firstManifestPool.segment_id);
	EXPECT_EQ(store.read(current, first.reference), firstBytes);
	EXPECT_EQ(store.read(current, second.reference), secondBytes);
	EXPECT_EQ(store.read(beforeSecond, first.reference), firstBytes);
	EXPECT_EQ(RelocatableIntegrityScanner(io, *metadata).scan().large_value_data_pages, 2U);
	current.release();
	beforeSecond.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedStore(io, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, first.reference), firstBytes);
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, second.reference), secondBytes);
	const std::vector<CatalogEntry> reopenedDataPools = reopened->findCurrentLargeValueDataPagePools(reopenedSnapshot);
	const std::vector<CatalogEntry> reopenedManifestPools =
			reopened->findCurrentLargeValueManifestPagePools(reopenedSnapshot);
	ASSERT_EQ(reopenedDataPools.size(), 1U);
	ASSERT_EQ(reopenedManifestPools.size(), 1U);
	EXPECT_EQ(reopenedDataPools.front().segment_id, firstPool.segment_id);
	EXPECT_EQ(reopenedManifestPools.front().segment_id, firstManifestPool.segment_id);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueCapacityPlanningReusesTheExistingSharedDataPool) {
	io->enableBufferPool(512U * RSS_PAGE_SIZE);
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult first = store.write(bytes(17U), writeOptions(), LogSequenceNumber(2U));
	ASSERT_EQ(first.manifest.chunks.size(), 1U);
	MetadataSnapshotPin beforeSecond = metadata->openSnapshot();
	const PageLocation firstLocation =
			metadata->findPageLocation(beforeSecond, first.manifest.chunks.front().first_data_page_id);
	beforeSecond.release();
	const LargeValueWriteResult second = store.write(bytes(29U), writeOptions(), LogSequenceNumber(3U));
	ASSERT_EQ(second.manifest.chunks.size(), 1U);
	const MetadataSnapshotPin current = metadata->openSnapshot();
	const PageLocation secondLocation =
			metadata->findPageLocation(current, second.manifest.chunks.front().first_data_page_id);
	EXPECT_EQ(secondLocation.segment_id, firstLocation.segment_id);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueRangeReadRetainsOnlyItsFirstDataPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.extent_class = LargeValueExtentClass::LVEC_16K;
	options.compression.codec = LargeValueCodec::LVC_RAW;
	const std::vector<std::byte> input =
			incompressibleBytes(static_cast<size_t>(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES) * 3U + 1U);
	const LargeValueWriteResult written = store.write(input, options, LogSequenceNumber(2U));
	ASSERT_EQ(written.manifest.chunks.size(), 1U);
	const LargeValueChunk &chunk = written.manifest.chunks.front();
	ASSERT_GT(chunk.data_page_count, 1U);

	io->enableBufferPool(64U * RSS_PAGE_SIZE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	io->resetBufferPoolStats();
	EXPECT_EQ(store.readRange(snapshot, written.reference, 0U, input.size()), input);
	const BufferPoolStats afterFirst = io->bufferPoolStats();

	EXPECT_EQ(store.readRange(snapshot, written.reference, 0U, input.size()), input);
	const BufferPoolStats afterSecond = io->bufferPoolStats();
	const uint64_t streamedBytes =
			static_cast<uint64_t>(chunk.data_page_count - 1U) * static_cast<uint64_t>(RSS_PAGE_SIZE);
	EXPECT_EQ(afterSecond.disk_read_bytes, afterFirst.disk_read_bytes + streamedBytes);
	EXPECT_EQ(afterSecond.misses, afterFirst.misses + chunk.data_page_count - 1U);
	EXPECT_EQ(afterSecond.resident_bytes, afterFirst.resident_bytes);
	EXPECT_GT(afterSecond.hits, afterFirst.hits);
	snapshot.release();
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreFillsSharedPoolsBeforeAllocatingAnotherPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	SegmentId firstDataPool;
	SegmentId firstManifestPool;
	for (uint32_t ordinal = 0U; ordinal <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++ordinal) {
		const LargeValueWriteResult written =
				store.write(bytes(17U + ordinal), writeOptions(), LogSequenceNumber(2U + ordinal));
		ASSERT_EQ(written.manifest.chunks.size(), 1U);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const PageLocation data =
				metadata->findPageLocation(snapshot, written.manifest.chunks.front().first_data_page_id);
		const PageLocation manifest = metadata->findPageLocation(snapshot, written.manifest.page_id);
		if (ordinal == 0U) {
			firstDataPool = data.segment_id;
			firstManifestPool = manifest.segment_id;
		}
		if (ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT) {
			EXPECT_EQ(data.segment_id, firstDataPool);
			EXPECT_EQ(manifest.segment_id, firstManifestPool);
			EXPECT_EQ(data.page_index, ordinal + RSS_PAGE_POOL_FIRST_DATA_FRAME);
			EXPECT_EQ(manifest.page_index, ordinal + RSS_PAGE_POOL_FIRST_DATA_FRAME);
		} else {
			EXPECT_NE(data.segment_id, firstDataPool);
			EXPECT_NE(manifest.segment_id, firstManifestPool);
			EXPECT_EQ(data.page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
			EXPECT_EQ(manifest.page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);
		}
		snapshot.release();
	}
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findCurrentLargeValueDataPagePools(snapshot).size(), 2U);
	EXPECT_EQ(metadata->findCurrentLargeValueManifestPagePools(snapshot).size(), 2U);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreSpansMoreThanOneManifestPoolAndReopens) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.first_data_slot = PhysicalSlotId(40U);
	options.last_data_slot = PhysicalSlotId(240U);
	options.first_manifest_slot = PhysicalSlotId(241U);
	options.last_manifest_slot = PhysicalSlotId(250U);
	options.compression.codec = LargeValueCodec::LVC_LZ4;
	options.extent_class = LargeValueExtentClass::LVEC_16K;
	const size_t chunkCount =
			static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) * RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
	const std::vector<std::byte> input(chunkCount * RSS_LARGE_VALUE_EXTENT_16K_BYTES + 19U, std::byte{0x5AU});
	const size_t plannedManifestPages =
			store.plannedManifestPageCount(input, options.compression, options.extent_class);
	ASSERT_GT(plannedManifestPages, RSS_PAGE_POOL_DATA_FRAME_COUNT);
	const LargeValueWriteResult written = store.write(input, options, LogSequenceNumber(2U));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::vector<LargeValueManifest> nodes = store.manifestTree(snapshot, written.manifest);
	ASSERT_EQ(nodes.size(), plannedManifestPages);
	std::set<uint64_t> manifestPools;
	for (const LargeValueManifest &node: nodes) {
		manifestPools.insert(metadata->findPageLocation(snapshot, node.page_id).segment_id.value());
	}
	EXPECT_EQ(manifestPools.size(), 2U);
	EXPECT_EQ(metadata->findCurrentLargeValueManifestPagePools(snapshot).size(), 2U);
	const uint64_t rangeOffset = input.size() - 37U;
	EXPECT_EQ(store.readRange(snapshot, written.reference, rangeOffset, 37U),
			  std::vector<std::byte>(input.end() - 37, input.end()));
	snapshot.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedStore(io, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.manifestTree(reopenedSnapshot, written.manifest).size(), plannedManifestPages);
	EXPECT_EQ(reopenedStore.readRange(reopenedSnapshot, written.reference, rangeOffset, 37U),
			  std::vector<std::byte>(input.end() - 37, input.end()));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreUsesManifestLeavesBeyondRootChunkCapacity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.extent_class = LargeValueExtentClass::LVEC_16K;
	const size_t inputBytes =
			(static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) + 1U) * RSS_LARGE_VALUE_EXTENT_16K_BYTES + 19U;
	const std::vector<std::byte> input = bytes(inputBytes);
	const LargeValueWriteResult written = store.write(input, options, LogSequenceNumber(2U));

	EXPECT_EQ(written.manifest.node_kind, LargeValueManifestNodeKind::LVMNK_ROOT);
	EXPECT_EQ(written.manifest.node_level, 1U);
	EXPECT_TRUE(written.manifest.chunks.empty());
	ASSERT_EQ(written.manifest.children.size(), 2U);
	EXPECT_EQ(store.plannedManifestPageCount(input, options.compression, options.extent_class), 3U);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::vector<LargeValueManifest> nodes = store.manifestTree(snapshot, written.manifest);
	ASSERT_EQ(nodes.size(), 3U);
	EXPECT_EQ(nodes.front(), written.manifest);
	EXPECT_TRUE(nodes[1].isLeaf());
	EXPECT_TRUE(nodes[2].isLeaf());
	const uint64_t offset =
			static_cast<uint64_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) * RSS_LARGE_VALUE_EXTENT_16K_BYTES - 11U;
	const uint64_t length = 37U;
	EXPECT_EQ(store.readRange(snapshot, written.reference, offset, length),
			  std::vector<std::byte>(input.begin() + static_cast<std::ptrdiff_t>(offset),
									 input.begin() + static_cast<std::ptrdiff_t>(offset + length)));
	EXPECT_EQ(store.readRange(snapshot, written.reference, 0U, 1U),
			  std::vector<std::byte>(input.begin(), input.begin() + 1));
	EXPECT_EQ(store.read(snapshot, written.reference), input);
	snapshot.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedStore(io, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, written.reference), input);
}

TEST_F(RelocatableMetadataStoreTest, StreamedLargeValueMaterializationUsesBoundedManifestFrontier) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input = bytes(
			(static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) + 1U) * RSS_LARGE_VALUE_EXTENT_16K_BYTES + 19U);
	const LargeValueRef reference =
			store.planReference(LargeValueId(1U), Generation(1U), input, LargeValueKind::LVK_UTF8_TEXT, "text/plain",
								{}, LargeValueExtentClass::LVEC_16K);
	const size_t dataPageCount = store.plannedDataPageCount(input, {}, LargeValueExtentClass::LVEC_16K);
	const size_t manifestPageCount = store.plannedManifestPageCount(input, {}, LargeValueExtentClass::LVEC_16K);
	ASSERT_GT(dataPageCount, 0U);
	ASSERT_EQ(manifestPageCount, 3U);
	struct PreparedFrame {
		PageOwnerHeader owner;
		std::vector<std::byte> payload;
		PageOwnerKind kind = PageOwnerKind::POK_NONE;
	};
	std::vector<PreparedFrame> frames;
	frames.reserve(dataPageCount + manifestPageCount);
	const PageId firstDataPage(101U);
	const PageId firstManifestPage(1000U);
	store.materializeStreamedValue(
			{reference,
			 input,
			 {},
			 LargeValueExtentClass::LVEC_16K,
			 firstDataPage,
			 dataPageCount,
			 firstManifestPage,
			 manifestPageCount},
			LogSequenceNumber(2U),
			[](PageId pageId, PageOwnerKind kind) {
				return PageOwnerHeader{
						pageId, 1U, kind, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1U), LogSequenceNumber(2U)};
			},
			[&frames](const PageOwnerHeader &owner, std::vector<std::byte> payload, PageOwnerKind kind) {
				frames.push_back({owner, std::move(payload), kind});
			});
	ASSERT_EQ(frames.size(), dataPageCount + manifestPageCount);
	EXPECT_EQ(frames.front().owner.page_id, firstDataPage);
	const auto root = std::find_if(frames.begin(), frames.end(), [firstManifestPage](const PreparedFrame &frame) {
		return frame.kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST && frame.owner.page_id == firstManifestPage;
	});
	ASSERT_NE(root, frames.end());
	const LargeValueManifest decodedRoot = LargeValueManifestPage::open(root->owner, root->payload).manifest();
	EXPECT_TRUE(decodedRoot.isRoot());
	EXPECT_EQ(decodedRoot.children.size(), 2U);
	EXPECT_EQ(decodedRoot.children.front().page_id, PageId(firstManifestPage.value() + 1U));
	EXPECT_EQ(decodedRoot.children.back().page_id, PageId(firstManifestPage.value() + 2U));
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreReadsAValidManifestWithAnInternalChildLevel) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteResult written;
	const LargeValueManifest originalRoot = largeMultiLeafManifest(store, *metadata, written);
	ASSERT_EQ(originalRoot.node_level, 1U);
	ASSERT_EQ(originalRoot.children.size(), 2U);

	LargeValueManifest internal = originalRoot;
	internal.page_id = PageId(501U);
	internal.node_kind = LargeValueManifestNodeKind::LVMNK_INTERNAL;
	internal.node_level = 1U;
	internal.first_chunk_ordinal = 0U;
	internal.first_uncompressed_offset = 0U;
	internal.node_logical_bytes = originalRoot.logical_bytes;
	const PageOwnerHeader internalOwner{PageId(501U),
										originalRoot.value_id.value(),
										PageOwnerKind::POK_LARGE_VALUE_MANIFEST,
										0U,
										RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										Generation(1U),
										LogSequenceNumber(3U)};
	const LargeValueManifestPage internalPage = LargeValueManifestPage::create(internalOwner, internal);
	MetadataSnapshotPin layout = metadata->openSnapshot();
	const PageLocation rootLocation = metadata->findPageLocation(layout, originalRoot.page_id);
	const CatalogEntry rootPool = metadata->findCatalog(layout, rootLocation.segment_id);
	layout.release();
	SharedPagePoolAllocator pools(io, *metadata);
	SharedPagePoolAppendRequest append;
	append.segment_id = rootPool.segment_id;
	append.generation = rootPool.generation;
	append.page_id = PageId(501U);
	append.page_owner_id = originalRoot.value_id.value();
	append.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
	append.payload = internalPage.serialize();
	static_cast<void>(pools.appendPage(append, LogSequenceNumber(3U)));

	uint32_t totalChunks = 0U;
	for (const LargeValueManifestChild &child: originalRoot.children) {
		ASSERT_LE(child.chunk_count, std::numeric_limits<uint32_t>::max() - totalChunks);
		totalChunks += child.chunk_count;
	}
	LargeValueManifest expandedRoot = originalRoot;
	expandedRoot.node_level = 2U;
	expandedRoot.children = {{PageId(501U), 0U, totalChunks, 0U, expandedRoot.logical_bytes}};
	MetadataSnapshotPin mutated = metadata->openSnapshot();
	const SharedPagePoolContents rootContents = pools.read(mutated, expandedRoot.page_id);
	const LargeValueManifestPage expandedRootPage = LargeValueManifestPage::create(rootContents.owner, expandedRoot);
	overwritePagePayload(io, *metadata, mutated, expandedRoot.page_id, expandedRootPage.serialize());

	EXPECT_EQ(store.manifest(mutated, written.reference), expandedRoot);
	EXPECT_EQ(store.manifestTree(mutated, expandedRoot).size(), 4U);
	const std::vector<std::byte> expected = bytes(
			(static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) + 1U) * RSS_LARGE_VALUE_EXTENT_16K_BYTES + 19U);
	EXPECT_EQ(store.read(mutated, written.reference), expected);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStorePlansInternalManifestLevelsBeyondTheLeafFanout) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const size_t chunkCount =
			static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) * RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN + 1U;
	const std::vector<std::byte> input(chunkCount * RSS_LARGE_VALUE_EXTENT_16K_BYTES);
	const size_t leafCount =
			(chunkCount + RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS - 1U) / RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS;
	const size_t manifestPageCount = store.plannedManifestPageCount(input, {}, LargeValueExtentClass::LVEC_16K);
	// The root plus all leaves is insufficient once leaves exceed their own
	// fan-out; the planner must add at least one internal Manifest level.
	EXPECT_GT(manifestPageCount, leafCount + 1U);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreCompressesLz4ChunksAndPreservesRangeReadsAcrossReopen) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.compression.codec = LargeValueCodec::LVC_LZ4;
	const std::vector<std::byte> input(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES * 2U + 29U, std::byte{0x5AU});
	const LargeValueWriteResult written = store.write(input, options, LogSequenceNumber(2U));

	ASSERT_EQ(written.reference.codec, LargeValueCodec::LVC_LZ4);
	ASSERT_EQ(written.manifest.codec, LargeValueCodec::LVC_LZ4);
	ASSERT_EQ(written.manifest.chunks.size(), 1U);
	const LargeValueChunk &chunk = written.manifest.chunks.front();
	EXPECT_EQ(chunk.codec, LargeValueCodec::LVC_LZ4);
	EXPECT_LT(chunk.stored_bytes, chunk.uncompressed_bytes);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const uint64_t offset = RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES - 7U;
	const uint64_t length = RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 19U;
	EXPECT_EQ(store.readRange(snapshot, written.reference, offset, length),
			  std::vector<std::byte>(input.begin() + static_cast<std::ptrdiff_t>(offset),
									 input.begin() + static_cast<std::ptrdiff_t>(offset + length)));
	snapshot.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedStore(io, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, written.reference), input);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreCompressesAndReadsZstdChunksAcrossReopen) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.compression.codec = LargeValueCodec::LVC_ZSTD;
	const std::vector<std::byte> input(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES * 2U + 29U, std::byte{0x6BU});
	const LargeValueWriteResult written = store.write(input, options, LogSequenceNumber(2U));

	ASSERT_EQ(written.reference.codec, LargeValueCodec::LVC_ZSTD);
	ASSERT_EQ(written.manifest.codec, LargeValueCodec::LVC_ZSTD);
	ASSERT_EQ(written.manifest.chunks.size(), 1U);
	EXPECT_EQ(written.manifest.chunks.front().codec, LargeValueCodec::LVC_ZSTD);
	EXPECT_LT(written.manifest.chunks.front().stored_bytes, written.manifest.chunks.front().uncompressed_bytes);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const uint64_t offset = RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES - 7U;
	const uint64_t length = RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 19U;
	EXPECT_EQ(store.readRange(snapshot, written.reference, offset, length),
			  std::vector<std::byte>(input.begin() + static_cast<std::ptrdiff_t>(offset),
									 input.begin() + static_cast<std::ptrdiff_t>(offset + length)));
	snapshot.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedStore(io, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, written.reference), input);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreFallsBackToRawWhenZstdDoesNotMeetPolicy) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.compression.codec = LargeValueCodec::LVC_ZSTD;
	// A full saving requirement is intentionally unattainable for a nonempty
	// compressed chunk, making this fallback assertion deterministic.
	options.compression.minimum_savings_percent = 100U;
	const std::vector<std::byte> input = bytes(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 47U);
	const LargeValueWriteResult written = store.write(input, options, LogSequenceNumber(2U));

	ASSERT_EQ(written.reference.codec, LargeValueCodec::LVC_RAW);
	ASSERT_EQ(written.manifest.codec, LargeValueCodec::LVC_RAW);
	ASSERT_EQ(written.manifest.chunks.size(), 1U);
	EXPECT_EQ(written.manifest.chunks.front().codec, LargeValueCodec::LVC_RAW);
	EXPECT_EQ(written.manifest.chunks.front().stored_bytes, written.manifest.chunks.front().uncompressed_bytes);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.read(snapshot, written.reference), input);

	options.compression.allow_raw_fallback = false;
	std::vector<std::byte> distinctInput = input;
	distinctInput.front() ^= std::byte{0xFFU};
	// Full-content deduplication correctly precedes codec selection. Use a distinct
	// value so this write exercises the policy that forbids its required RAW fallback.
	EXPECT_THROW(static_cast<void>(store.write(distinctInput, options, LogSequenceNumber(3U))), FormatException);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreUsesMixedChunkCodecsForMixedCompressibility) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.extent_class = LargeValueExtentClass::LVEC_16K;
	options.compression.codec = LargeValueCodec::LVC_ZSTD;
	options.compression.minimum_savings_percent = 1U;
	std::vector<std::byte> input(RSS_LARGE_VALUE_EXTENT_16K_BYTES, std::byte{0x5AU});
	const std::vector<std::byte> incompressible = incompressibleBytes(RSS_LARGE_VALUE_EXTENT_16K_BYTES);
	input.insert(input.end(), incompressible.begin(), incompressible.end());

	const LargeValueWriteResult written = store.write(input, options, LogSequenceNumber(2U));
	ASSERT_EQ(written.manifest.chunks.size(), 2U);
	EXPECT_EQ(written.reference.codec, LargeValueCodec::LVC_MIXED);
	EXPECT_EQ(written.manifest.codec, LargeValueCodec::LVC_MIXED);
	EXPECT_EQ(written.manifest.chunks.front().codec, LargeValueCodec::LVC_ZSTD);
	EXPECT_EQ(written.manifest.chunks.back().codec, LargeValueCodec::LVC_RAW);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.read(snapshot, written.reference), input);
	snapshot.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedStore(io, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, written.reference), input);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreNeverExposesTheBeforeInactiveWriteGeneration) {
	const std::vector<std::byte> input = bytes(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 1U);
	const LargeValueWriteOptions options = writeOptions();
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	EXPECT_THROW(static_cast<void>(
						 store.write(input, options, LogSequenceNumber(2U),
									 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin before = metadata->openSnapshot();
	EXPECT_EQ(before.superblock().generation, Generation(1U));
	EXPECT_FALSE(metadata->findLargeValueManifestOptional(before, LargeValueId(1U)).has_value());
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(before, options.first_data_slot));
	EXPECT_FALSE(metadata->isPhysicalSlotAllocated(before, options.first_manifest_slot));
	before.release();

	metadata.reset();
	auto interrupted = RelocatableMetadataStore::open(io);
	LargeValueStore interruptedStore(io, *interrupted);
	EXPECT_THROW(static_cast<void>(interruptedStore.write(
						 input, options, LogSequenceNumber(2U),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	interrupted.reset();

	auto recovered = RelocatableMetadataStore::open(io);
	LargeValueStore recoveredStore(io, *recovered);
	MetadataSnapshotPin after = recovered->openSnapshot();
	const LargeValueRef reference{
			LargeValueId(1U), Generation(1U),			input.size(),	   graph::utils::Sha256::digest(input),
			options.kind,	  LargeValueCodec::LVC_RAW, options.media_type};
	EXPECT_EQ(recoveredStore.read(after, reference), input);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsCorruptedDataBeforeReturningBytes) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written =
			store.write(bytes(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 1U), writeOptions(), LogSequenceNumber(2U));
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const uint64_t payloadOffset = physicalPageOffset(snapshot, *metadata, dataPageId) + RSS_PAGE_OWNER_HEADER_BYTES +
								   RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES;
	const std::byte corruption{0x7FU};
	io->writeAt(payloadOffset, &corruption, sizeof(corruption));
	io->sync();
	expectFormatError([&] { static_cast<void>(store.read(snapshot, written.reference)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsRawDataWithInconsistentStoredLength) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written = store.write(bytes(73U), writeOptions(), LogSequenceNumber(2U));
	ASSERT_EQ(written.manifest.chunks.size(), 1U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	const SharedPagePoolContents dataContents = pools.read(snapshot, dataPageId);
	LargeValueDataPage corruptedData = LargeValueDataFrame::open(dataContents.owner, dataContents.payload).page();
	ASSERT_GT(corruptedData.payload.size(), 1U);
	corruptedData.payload.pop_back();
	--corruptedData.stored_chunk_bytes;
	const LargeValueDataFrame dataReplacement = LargeValueDataFrame::create(dataContents.owner, corruptedData);
	overwritePagePayload(io, *metadata, snapshot, dataPageId, dataReplacement.serialize());

	const SharedPagePoolContents manifestContents = pools.read(snapshot, written.manifest.page_id);
	LargeValueManifest corruptedManifest =
			LargeValueManifestPage::open(manifestContents.owner, manifestContents.payload).manifest();
	--corruptedManifest.chunks.front().stored_bytes;
	corruptedManifest.chunks.front().stored_crc =
			graph::utils::calculateCrc(corruptedData.payload.data(), corruptedData.payload.size());
	const LargeValueManifestPage manifestReplacement =
			LargeValueManifestPage::create(manifestContents.owner, corruptedManifest);
	overwritePagePayload(io, *metadata, snapshot, written.manifest.page_id, manifestReplacement.serialize());

	expectFormatError([&] { static_cast<void>(store.read(snapshot, written.reference)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsChunkWithAnInvalidStoredChecksum) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written = store.write(bytes(73U), writeOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	const SharedPagePoolContents contents = pools.read(snapshot, dataPageId);
	LargeValueDataPage corrupted = LargeValueDataFrame::open(contents.owner, contents.payload).page();
	corrupted.payload.front() ^= std::byte{0xFFU};
	const LargeValueDataFrame replacement = LargeValueDataFrame::create(contents.owner, corrupted);
	overwritePagePayload(io, *metadata, snapshot, dataPageId, replacement.serialize());

	expectFormatError([&] { static_cast<void>(store.read(snapshot, written.reference)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsChunkWithAnInvalidUncompressedChecksum) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written = store.write(bytes(73U), writeOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	const SharedPagePoolContents dataContents = pools.read(snapshot, dataPageId);
	LargeValueDataPage corruptedData = LargeValueDataFrame::open(dataContents.owner, dataContents.payload).page();
	corruptedData.payload.front() ^= std::byte{0xFFU};
	const LargeValueDataFrame dataReplacement = LargeValueDataFrame::create(dataContents.owner, corruptedData);
	overwritePagePayload(io, *metadata, snapshot, dataPageId, dataReplacement.serialize());

	const SharedPagePoolContents manifestContents = pools.read(snapshot, written.manifest.page_id);
	LargeValueManifest corruptedManifest =
			LargeValueManifestPage::open(manifestContents.owner, manifestContents.payload).manifest();
	corruptedManifest.chunks.front().stored_crc =
			graph::utils::calculateCrc(corruptedData.payload.data(), corruptedData.payload.size());
	const LargeValueManifestPage manifestReplacement =
			LargeValueManifestPage::create(manifestContents.owner, corruptedManifest);
	overwritePagePayload(io, *metadata, snapshot, written.manifest.page_id, manifestReplacement.serialize());

	expectFormatError([&] { static_cast<void>(store.read(snapshot, written.reference)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreVerifiesFullContentHashAfterChunkChecks) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written = store.write(bytes(73U), writeOptions(), LogSequenceNumber(2U));
	ASSERT_EQ(written.manifest.chunks.size(), 1U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	const SharedPagePoolContents dataContents = pools.read(snapshot, dataPageId);
	LargeValueDataPage corruptedData = LargeValueDataFrame::open(dataContents.owner, dataContents.payload).page();
	corruptedData.payload.front() ^= std::byte{0xFFU};
	const LargeValueDataFrame dataReplacement = LargeValueDataFrame::create(dataContents.owner, corruptedData);
	overwritePagePayload(io, *metadata, snapshot, dataPageId, dataReplacement.serialize());

	const SharedPagePoolContents manifestContents = pools.read(snapshot, written.manifest.page_id);
	LargeValueManifest corruptedManifest =
			LargeValueManifestPage::open(manifestContents.owner, manifestContents.payload).manifest();
	const uint32_t payloadCrc = graph::utils::calculateCrc(corruptedData.payload.data(), corruptedData.payload.size());
	corruptedManifest.chunks.front().stored_crc = payloadCrc;
	corruptedManifest.chunks.front().uncompressed_crc = payloadCrc;
	const LargeValueManifestPage manifestReplacement =
			LargeValueManifestPage::create(manifestContents.owner, corruptedManifest);
	overwritePagePayload(io, *metadata, snapshot, written.manifest.page_id, manifestReplacement.serialize());

	expectFormatError([&] { static_cast<void>(store.read(snapshot, written.reference)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsLz4PayloadThatPassesFrameChecksButCannotDecompress) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.compression.codec = LargeValueCodec::LVC_LZ4;
	options.compression.minimum_savings_percent = 1U;
	options.compression.allow_raw_fallback = false;
	const LargeValueWriteResult written = store.write(std::vector<std::byte>(2048U), options, LogSequenceNumber(2U));
	ASSERT_EQ(written.reference.codec, LargeValueCodec::LVC_LZ4);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	const SharedPagePoolContents dataContents = pools.read(snapshot, dataPageId);
	LargeValueDataPage corruptedData = LargeValueDataFrame::open(dataContents.owner, dataContents.payload).page();
	std::fill(corruptedData.payload.begin(), corruptedData.payload.end(), std::byte{0xFFU});
	const LargeValueDataFrame dataReplacement = LargeValueDataFrame::create(dataContents.owner, corruptedData);
	overwritePagePayload(io, *metadata, snapshot, dataPageId, dataReplacement.serialize());

	const SharedPagePoolContents manifestContents = pools.read(snapshot, written.manifest.page_id);
	LargeValueManifest corruptedManifest =
			LargeValueManifestPage::open(manifestContents.owner, manifestContents.payload).manifest();
	corruptedManifest.chunks.front().stored_crc =
			graph::utils::calculateCrc(corruptedData.payload.data(), corruptedData.payload.size());
	const LargeValueManifestPage manifestReplacement =
			LargeValueManifestPage::create(manifestContents.owner, corruptedManifest);
	overwritePagePayload(io, *metadata, snapshot, written.manifest.page_id, manifestReplacement.serialize());

	expectFormatError([&] { static_cast<void>(store.read(snapshot, written.reference)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsZstdPayloadThatPassesFrameChecksButCannotDecompress) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.compression.codec = LargeValueCodec::LVC_ZSTD;
	options.compression.minimum_savings_percent = 1U;
	options.compression.allow_raw_fallback = false;
	const LargeValueWriteResult written = store.write(std::vector<std::byte>(2048U), options, LogSequenceNumber(2U));
	ASSERT_EQ(written.reference.codec, LargeValueCodec::LVC_ZSTD);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	const SharedPagePoolContents dataContents = pools.read(snapshot, dataPageId);
	LargeValueDataPage corruptedData = LargeValueDataFrame::open(dataContents.owner, dataContents.payload).page();
	std::fill(corruptedData.payload.begin(), corruptedData.payload.end(), std::byte{0xFFU});
	const LargeValueDataFrame dataReplacement = LargeValueDataFrame::create(dataContents.owner, corruptedData);
	overwritePagePayload(io, *metadata, snapshot, dataPageId, dataReplacement.serialize());

	const SharedPagePoolContents manifestContents = pools.read(snapshot, written.manifest.page_id);
	LargeValueManifest corruptedManifest =
			LargeValueManifestPage::open(manifestContents.owner, manifestContents.payload).manifest();
	corruptedManifest.chunks.front().stored_crc =
			graph::utils::calculateCrc(corruptedData.payload.data(), corruptedData.payload.size());
	const LargeValueManifestPage manifestReplacement =
			LargeValueManifestPage::create(manifestContents.owner, corruptedManifest);
	overwritePagePayload(io, *metadata, snapshot, written.manifest.page_id, manifestReplacement.serialize());

	expectFormatError([&] { static_cast<void>(store.read(snapshot, written.reference)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsZstdPayloadWithAValidButWrongDecodedLength) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.compression.codec = LargeValueCodec::LVC_ZSTD;
	options.compression.minimum_savings_percent = 1U;
	options.compression.allow_raw_fallback = false;
	const LargeValueWriteResult written = store.write(std::vector<std::byte>(2048U), options, LogSequenceNumber(2U));
	ASSERT_EQ(written.reference.codec, LargeValueCodec::LVC_ZSTD);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const SharedPagePoolContents manifestContents = pools.read(snapshot, written.manifest.page_id);
	LargeValueManifest corrupted =
			LargeValueManifestPage::open(manifestContents.owner, manifestContents.payload).manifest();
	ASSERT_EQ(corrupted.node_level, 0U);
	ASSERT_EQ(corrupted.chunks.size(), 1U);
	ASSERT_LT(corrupted.chunks.front().uncompressed_bytes, std::numeric_limits<uint32_t>::max());
	++corrupted.logical_bytes;
	++corrupted.chunks.front().uncompressed_bytes;
	const LargeValueManifestPage replacement = LargeValueManifestPage::create(manifestContents.owner, corrupted);
	overwritePagePayload(io, *metadata, snapshot, written.manifest.page_id, replacement.serialize());

	LargeValueRef mismatchedReference = written.reference;
	++mismatchedReference.logical_bytes;
	expectFormatError([&] { static_cast<void>(store.read(snapshot, mismatchedReference)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsManifestAndDataFramesWithStalePhysicalGenerations) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written = store.write(bytes(73U), writeOptions(), LogSequenceNumber(2U));
	ASSERT_EQ(written.manifest.chunks.size(), 1U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	const SharedPagePoolContents manifestContents = pools.read(snapshot, written.manifest.page_id);
	PageOwnerHeader staleManifestOwner = manifestContents.owner;
	staleManifestOwner.generation = Generation(staleManifestOwner.generation.value() + 1U);
	overwritePageOwnerHeader(io, *metadata, snapshot, written.manifest.page_id, staleManifestOwner);
	expectFormatError([&] { static_cast<void>(store.manifest(snapshot, written.reference)); },
					  FormatError::FERR_INVALID_STATE);
	overwritePageOwnerHeader(io, *metadata, snapshot, written.manifest.page_id, manifestContents.owner);

	const SharedPagePoolContents dataContents = pools.read(snapshot, dataPageId);
	PageOwnerHeader staleDataOwner = dataContents.owner;
	staleDataOwner.generation = Generation(staleDataOwner.generation.value() + 1U);
	overwritePageOwnerHeader(io, *metadata, snapshot, dataPageId, staleDataOwner);
	expectFormatError([&] { static_cast<void>(store.read(snapshot, written.reference)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRequiresDirectoryRootManifestNode) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written = store.write(bytes(73U), writeOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const SharedPagePoolContents contents = pools.read(snapshot, written.manifest.page_id);
	LargeValueManifest corrupted = LargeValueManifestPage::open(contents.owner, contents.payload).manifest();
	corrupted.node_kind = LargeValueManifestNodeKind::LVMNK_LEAF;
	corrupted.node_logical_bytes = corrupted.logical_bytes;
	const LargeValueManifestPage replacement = LargeValueManifestPage::create(contents.owner, corrupted);
	overwritePagePayload(io, *metadata, snapshot, written.manifest.page_id, replacement.serialize());

	expectFormatError([&] { static_cast<void>(store.manifest(snapshot, written.reference)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsTraversalFromANonRootManifest) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written = store.write(bytes(73U), writeOptions(), LogSequenceNumber(2U));
	LargeValueManifest nonRoot = written.manifest;
	nonRoot.node_kind = LargeValueManifestNodeKind::LVMNK_LEAF;
	nonRoot.node_logical_bytes = nonRoot.logical_bytes;
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.manifestTree(snapshot, nonRoot)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsValidlyEncodedDataWithMismatchedManifestIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written =
			store.write(bytes(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES + 1U), writeOptions(), LogSequenceNumber(2U));
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const SharedPagePoolContents contents = pools.read(snapshot, dataPageId);
	LargeValueDataPage corrupted = LargeValueDataFrame::open(contents.owner, contents.payload).page();
	corrupted.generation = Generation(corrupted.generation.value() + 1U);
	const LargeValueDataFrame replacement = LargeValueDataFrame::create(contents.owner, corrupted);
	overwritePagePayload(io, *metadata, snapshot, dataPageId, replacement.serialize());

	expectFormatError([&] { static_cast<void>(store.read(snapshot, written.reference)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsValidlyEncodedManifestWithMismatchedReference) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written = store.write(bytes(17U), writeOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const SharedPagePoolContents contents = pools.read(snapshot, written.manifest.page_id);
	LargeValueManifest corrupted = LargeValueManifestPage::open(contents.owner, contents.payload).manifest();
	corrupted.kind = LargeValueKind::LVK_BYTES;
	const LargeValueManifestPage replacement = LargeValueManifestPage::create(contents.owner, corrupted);
	overwritePagePayload(io, *metadata, snapshot, written.manifest.page_id, replacement.serialize());

	expectFormatError([&] { static_cast<void>(store.manifest(snapshot, written.reference)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsDuplicateManifestChildrenWithValidPageChecksums) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteResult written;
	const LargeValueManifest root = largeMultiLeafManifest(store, *metadata, written);
	ASSERT_GT(root.children.size(), 1U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const SharedPagePoolContents contents = pools.read(snapshot, root.page_id);
	LargeValueManifest corrupted = LargeValueManifestPage::open(contents.owner, contents.payload).manifest();
	corrupted.children[1].page_id = corrupted.children.front().page_id;
	const LargeValueManifestPage replacement = LargeValueManifestPage::create(contents.owner, corrupted);
	overwritePagePayload(io, *metadata, snapshot, root.page_id, replacement.serialize());

	const LargeValueManifest corruptedRoot = store.manifest(snapshot, written.reference);
	expectFormatError([&] { static_cast<void>(store.manifestTree(snapshot, corruptedRoot)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsChildRangeWithIncorrectChunkCardinality) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteResult written;
	const LargeValueManifest root = largeMultiLeafManifest(store, *metadata, written);
	ASSERT_EQ(root.node_level, 1U);
	ASSERT_EQ(root.children.size(), 2U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const SharedPagePoolContents contents = pools.read(snapshot, root.page_id);
	LargeValueManifest corrupted = LargeValueManifestPage::open(contents.owner, contents.payload).manifest();
	ASSERT_LT(corrupted.children.front().chunk_count, std::numeric_limits<uint32_t>::max());
	ASSERT_LT(corrupted.children[1U].first_chunk_ordinal, std::numeric_limits<uint32_t>::max());
	// Preserve the root's local ordinal sequence while making its first child
	// claim one more chunk than the child page actually owns.
	++corrupted.children.front().chunk_count;
	++corrupted.children[1U].first_chunk_ordinal;
	const LargeValueManifestPage replacement = LargeValueManifestPage::create(contents.owner, corrupted);
	overwritePagePayload(io, *metadata, snapshot, root.page_id, replacement.serialize());

	expectFormatError([&] { static_cast<void>(store.read(snapshot, written.reference)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsManifestChildAtAnUnexpectedLevel) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteResult written;
	const LargeValueManifest root = largeMultiLeafManifest(store, *metadata, written);
	ASSERT_EQ(root.node_level, 1U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SharedPagePoolAllocator pools(io, *metadata);
	const SharedPagePoolContents contents = pools.read(snapshot, root.page_id);
	LargeValueManifest corrupted = LargeValueManifestPage::open(contents.owner, contents.payload).manifest();
	// The page codec admits a root with child entries at this level; traversal
	// must still reject the contradictory child-level relationship.
	corrupted.node_level = 2U;
	const LargeValueManifestPage replacement = LargeValueManifestPage::create(contents.owner, corrupted);
	overwritePagePayload(io, *metadata, snapshot, root.page_id, replacement.serialize());

	expectFormatError([&] { static_cast<void>(store.read(snapshot, written.reference)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsMultiLevelChildWithMismatchedImmutableIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteResult written;
	const LargeValueManifest root = largeMultiLeafManifest(store, *metadata, written);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::vector<LargeValueManifest> nodes = store.manifestTree(snapshot, root);
	ASSERT_GT(nodes.size(), 1U);
	const LargeValueManifest &child = nodes[1U];
	SharedPagePoolAllocator pools(io, *metadata);
	const SharedPagePoolContents contents = pools.read(snapshot, child.page_id);
	LargeValueManifest corrupted = LargeValueManifestPage::open(contents.owner, contents.payload).manifest();
	corrupted.generation = Generation(corrupted.generation.value() + 1U);
	const LargeValueManifestPage replacement = LargeValueManifestPage::create(contents.owner, corrupted);
	overwritePagePayload(io, *metadata, snapshot, child.page_id, replacement.serialize());

	expectFormatError([&] { static_cast<void>(store.read(snapshot, written.reference)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest,
	   LargeValueStoreRejectsIncompleteMultiLevelRetirementBeforeRemovingTheContentIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteResult written;
	const LargeValueManifest root = largeMultiLeafManifest(store, *metadata, written);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::vector<LargeValueManifest> nodes = store.manifestTree(snapshot, root);
	ASSERT_GT(nodes.size(), 1U);

	std::vector<PageRetirement> completeRetirements;
	for (const LargeValueManifest &node: nodes) {
		for (const LargeValueChunk &chunk: node.chunks) {
			for (uint16_t ordinal = 0U; ordinal < chunk.data_page_count; ++ordinal) {
				completeRetirements.push_back(
						store.planPageRetirement(snapshot, PageId(chunk.first_data_page_id.value() + ordinal),
												 written.reference.value_id, PageOwnerKind::POK_LARGE_VALUE_DATA));
			}
		}
		completeRetirements.push_back(store.planPageRetirement(snapshot, node.page_id, written.reference.value_id,
															   PageOwnerKind::POK_LARGE_VALUE_MANIFEST));
	}
	std::sort(completeRetirements.begin(), completeRetirements.end(),
			  [](const PageRetirement &left, const PageRetirement &right) {
				  return left.location.page_id.value() < right.location.page_id.value();
			  });

	std::vector<PageRetirement> incompleteRetirements = completeRetirements;
	const auto childManifest = std::find_if(
			incompleteRetirements.begin(), incompleteRetirements.end(), [&root](const PageRetirement &retirement) {
				return retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST &&
					   retirement.location.page_id != root.page_id;
			});
	ASSERT_NE(childManifest, incompleteRetirements.end());
	incompleteRetirements.erase(childManifest);

	const Generation expectedGeneration = snapshot.superblock().generation;
	const std::array<LargeValueId, 1U> erasedValueIds{{written.reference.value_id}};
	expectFormatError(
			[&] {
				static_cast<void>(store.retireUnreachablePages(expectedGeneration, erasedValueIds,
															   incompleteRetirements, PhysicalSlotId(40U),
															   PhysicalSlotId(80U), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(store.read(snapshot, written.reference),
			  bytes((static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) + 1U) * RSS_LARGE_VALUE_EXTENT_16K_BYTES +
					19U));

	const Superblock retired =
			store.retireUnreachablePages(expectedGeneration, erasedValueIds, completeRetirements, PhysicalSlotId(40U),
										 PhysicalSlotId(80U), LogSequenceNumber(3U));
	EXPECT_GT(retired.generation, expectedGeneration);
	snapshot.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findLargeValueManifestOptional(current, written.reference.value_id).has_value());
	const BTreeIndex contentIndex(RSS_LARGE_VALUE_CONTENT_INDEX_ID, store.contentIndex(), *metadata);
	EXPECT_TRUE(contentIndex.scan(current, std::nullopt, 1U).empty());
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsRetirementPlannedAgainstAStaleGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written = store.write(bytes(37U), writeOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const Generation staleGeneration = snapshot.superblock().generation;
	const PageRetirement dataRetirement =
			store.planPageRetirement(snapshot, written.manifest.chunks.front().first_data_page_id,
									 written.reference.value_id, PageOwnerKind::POK_LARGE_VALUE_DATA);
	const PageRetirement manifestRetirement = store.planPageRetirement(
			snapshot, written.manifest.page_id, written.reference.value_id, PageOwnerKind::POK_LARGE_VALUE_MANIFEST);
	snapshot.release();
	static_cast<void>(store.write(bytes(41U), writeOptions(), LogSequenceNumber(3U)));
	const std::array<LargeValueId, 1U> erasedValueIds{{written.reference.value_id}};
	const std::array<PageRetirement, 2U> retirements{{dataRetirement, manifestRetirement}};
	expectFormatError(
			[&] {
				static_cast<void>(store.retireUnreachablePages(staleGeneration, erasedValueIds, retirements,
															   PhysicalSlotId(40U), PhysicalSlotId(80U),
															   LogSequenceNumber(4U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreEvacuatesDataAndManifestFramesWithoutChangingLogicalGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input = bytes(31U);
	const LargeValueWriteResult written = store.write(input, writeOptions(), LogSequenceNumber(2U));
	ASSERT_EQ(written.manifest.chunks.size(), 1U);
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	allocateLargeValueDataDestination(io, *metadata, written.reference, SegmentId(71U), PageId(501U), Generation(2U),
									  PhysicalSlotId(100U), LogSequenceNumber(3U));
	allocateLargeValueManifestDestination(io, *metadata, written.manifest, SegmentId(72U), PageId(502U), Generation(2U),
										  PhysicalSlotId(101U), LogSequenceNumber(4U));

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	const PageLocation oldDataLocation = metadata->findPageLocation(oldSnapshot, dataPageId);
	const LargeValueManifestLocation oldManifestRoute =
			metadata->findLargeValueManifest(oldSnapshot, written.reference.value_id);
	const SharedPagePoolAllocation dataEvacuation =
			store.evacuatePage(dataPageId, SegmentId(71U), LogSequenceNumber(5U));
	EXPECT_EQ(dataEvacuation.location.segment_id, SegmentId(71U));
	EXPECT_EQ(dataEvacuation.location.generation, Generation(2U));
	EXPECT_EQ(store.read(oldSnapshot, written.reference), input);

	const SharedPagePoolAllocation manifestEvacuation =
			store.evacuate(written.reference.value_id, SegmentId(72U), LogSequenceNumber(6U));
	EXPECT_EQ(manifestEvacuation.location.segment_id, SegmentId(72U));
	EXPECT_EQ(manifestEvacuation.location.generation, Generation(2U));
	EXPECT_EQ(store.read(oldSnapshot, written.reference), input);
	EXPECT_EQ(metadata->findPageLocation(oldSnapshot, dataPageId), oldDataLocation);
	EXPECT_EQ(metadata->findLargeValueManifest(oldSnapshot, written.reference.value_id), oldManifestRoute);

	MetadataSnapshotPin current = metadata->openSnapshot();
	const LargeValueManifestLocation currentRoute =
			metadata->findLargeValueManifest(current, written.reference.value_id);
	EXPECT_EQ(currentRoute.generation, Generation(2U));
	EXPECT_EQ(metadata->findPageLocation(current, dataPageId).generation, Generation(2U));
	EXPECT_EQ(store.manifest(current, written.reference).generation, written.reference.generation);
	EXPECT_EQ(store.read(current, written.reference), input);
	oldSnapshot.release();
	current.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedStore(io, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopened->findLargeValueManifest(reopenedSnapshot, written.reference.value_id).generation,
			  Generation(2U));
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, written.reference), input);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreEvacuatesLaterChunksAndDirectoryRootThroughPageDispatch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input = bytes(RSS_LARGE_VALUE_EXTENT_32K_BYTES + 17U);
	const LargeValueWriteResult written = store.write(input, writeOptions(), LogSequenceNumber(2U));
	ASSERT_GE(written.manifest.chunks.size(), 2U);
	const PageId laterDataPage = written.manifest.chunks[1U].first_data_page_id;
	allocateLargeValueDataDestination(io, *metadata, written.reference, SegmentId(71U), PageId(501U), Generation(2U),
									  PhysicalSlotId(100U), LogSequenceNumber(3U));
	allocateLargeValueManifestDestination(io, *metadata, written.manifest, SegmentId(72U), PageId(502U), Generation(2U),
										  PhysicalSlotId(101U), LogSequenceNumber(4U));

	const SharedPagePoolAllocation dataEvacuation =
			store.evacuatePage(laterDataPage, SegmentId(71U), LogSequenceNumber(5U));
	EXPECT_EQ(dataEvacuation.location.segment_id, SegmentId(71U));
	const SharedPagePoolAllocation manifestEvacuation =
			store.evacuatePage(written.manifest.page_id, SegmentId(72U), LogSequenceNumber(6U));
	EXPECT_EQ(manifestEvacuation.location.segment_id, SegmentId(72U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.read(snapshot, written.reference), input);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreEvacuatesADataPageFromTheSecondManifestLeaf) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteResult written;
	const LargeValueManifest root = largeMultiLeafManifest(store, *metadata, written);
	MetadataSnapshotPin source = metadata->openSnapshot();
	const std::vector<LargeValueManifest> nodes = store.manifestTree(source, root);
	ASSERT_EQ(nodes.size(), 3U);
	ASSERT_TRUE(nodes[2U].isLeaf());
	const PageId dataPageId = nodes[2U].chunks.front().first_data_page_id;
	source.release();
	allocateLargeValueDataDestination(io, *metadata, written.reference, SegmentId(71U), PageId(501U), Generation(2U),
									  PhysicalSlotId(100U), LogSequenceNumber(3U));

	const SharedPagePoolAllocation moved = store.evacuatePage(dataPageId, SegmentId(71U), LogSequenceNumber(4U));
	EXPECT_EQ(moved.location.segment_id, SegmentId(71U));
	MetadataSnapshotPin current = metadata->openSnapshot();
	const std::vector<std::byte> expected = bytes(
			(static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) + 1U) * RSS_LARGE_VALUE_EXTENT_16K_BYTES + 19U);
	EXPECT_EQ(store.read(current, written.reference), expected);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsEvacuationToTheWrongTypedPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const LargeValueWriteResult written = store.write(bytes(31U), writeOptions(), LogSequenceNumber(2U));
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	allocateLargeValueDataDestination(io, *metadata, written.reference, SegmentId(71U), PageId(501U), Generation(2U),
									  PhysicalSlotId(100U), LogSequenceNumber(3U));
	allocateLargeValueManifestDestination(io, *metadata, written.manifest, SegmentId(72U), PageId(502U), Generation(2U),
										  PhysicalSlotId(101U), LogSequenceNumber(4U));

	expectFormatError(
			[&] {
				static_cast<void>(store.evacuate(written.reference.value_id, SegmentId(71U), LogSequenceNumber(5U)));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store.evacuatePage(dataPageId, SegmentId(72U), LogSequenceNumber(6U))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsUnreachableManifestAndForeignPageEvacuation) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteResult written;
	const LargeValueManifest root = largeMultiLeafManifest(store, *metadata, written);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::vector<LargeValueManifest> nodes = store.manifestTree(snapshot, root);
	ASSERT_GT(nodes.size(), 1U);
	ASSERT_TRUE(nodes[1U].isLeaf());
	const LargeValueManifest orphan = nodes[1U];
	const IndexRootLocation contentIndexRoot = metadata->findIndexRoot(snapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
	snapshot.release();

	// The page is validly encoded for this value but is not linked by the root;
	// evacuation must reject it instead of making an orphan physical frame live.
	LargeValueManifest expectedOrphan = orphan;
	expectedOrphan.page_id = PageId(501U);
	const PageOwnerHeader expectedOrphanOwner{PageId(501U),
											  orphan.value_id.value(),
											  PageOwnerKind::POK_LARGE_VALUE_MANIFEST,
											  0U,
											  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
											  Generation(2U),
											  LogSequenceNumber(3U)};
	const LargeValueManifestPage encodedOrphan = LargeValueManifestPage::create(expectedOrphanOwner, expectedOrphan);
	EXPECT_NO_THROW(static_cast<void>(LargeValueManifestPage::open(expectedOrphanOwner, encodedOrphan.serialize())));
	allocateLargeValueManifestDestination(io, *metadata, orphan, SegmentId(72U), PageId(502U), Generation(2U),
										  PhysicalSlotId(101U), LogSequenceNumber(4U));
	SharedPagePoolAllocator pools(io, *metadata);
	MetadataSnapshotPin prepared = metadata->openSnapshot();
	const PageLocation childLocation = metadata->findPageLocation(prepared, orphan.page_id);
	const CatalogEntry childPool = metadata->findCatalog(prepared, childLocation.segment_id);
	prepared.release();
	SharedPagePoolAppendRequest orphanRequest;
	orphanRequest.segment_id = childPool.segment_id;
	orphanRequest.generation = childPool.generation;
	orphanRequest.page_id = PageId(501U);
	orphanRequest.page_owner_id = orphan.value_id.value();
	orphanRequest.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
	orphanRequest.payload = encodedOrphan.serialize();
	static_cast<void>(pools.appendPage(orphanRequest, LogSequenceNumber(5U)));
	prepared = metadata->openSnapshot();
	const SharedPagePoolContents orphanContents = pools.read(prepared, PageId(501U));
	EXPECT_EQ(LargeValueManifestPage::open(orphanContents.owner, orphanContents.payload).manifest().page_id,
			  PageId(501U));
	EXPECT_NO_THROW(static_cast<void>(store.manifestTree(prepared, root)));
	prepared.release();
	expectFormatError(
			[&] { static_cast<void>(store.evacuatePage(PageId(501U), SegmentId(72U), LogSequenceNumber(6U))); },
			FormatError::FERR_INVALID_STATE);

	// A content-index frame is a valid generic page but not a LargeValue frame.
	expectFormatError(
			[&] {
				static_cast<void>(
						store.evacuatePage(contentIndexRoot.root_page_id, SegmentId(72U), LogSequenceNumber(7U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest,
	   LargeValueStoreEvacuatesANonRootManifestPageWithoutChangingDirectoryRouteOrSnapshots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.extent_class = LargeValueExtentClass::LVEC_16K;
	const size_t inputBytes =
			(static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) + 1U) * RSS_LARGE_VALUE_EXTENT_16K_BYTES + 19U;
	const std::vector<std::byte> input = bytes(inputBytes);
	const LargeValueWriteResult written = store.write(input, options, LogSequenceNumber(2U));
	ASSERT_EQ(written.manifest.node_level, 1U);
	MetadataSnapshotPin sourceSnapshot = metadata->openSnapshot();
	const std::vector<LargeValueManifest> nodes = store.manifestTree(sourceSnapshot, written.manifest);
	ASSERT_EQ(nodes.size(), 3U);
	ASSERT_TRUE(nodes[1].isLeaf());
	const LargeValueManifest child = nodes[1];
	const LargeValueManifestLocation sourceRoute =
			metadata->findLargeValueManifest(sourceSnapshot, written.reference.value_id);
	const PageLocation sourceLocation = metadata->findPageLocation(sourceSnapshot, child.page_id);
	sourceSnapshot.release();
	allocateLargeValueManifestDestination(io, *metadata, child, SegmentId(72U), PageId(502U), Generation(2U),
										  PhysicalSlotId(101U), LogSequenceNumber(3U));

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	const SharedPagePoolAllocation evacuation =
			store.evacuatePage(child.page_id, SegmentId(72U), LogSequenceNumber(4U));
	EXPECT_EQ(evacuation.location.segment_id, SegmentId(72U));
	EXPECT_EQ(store.read(oldSnapshot, written.reference), input);
	EXPECT_EQ(metadata->findPageLocation(oldSnapshot, child.page_id), sourceLocation);
	EXPECT_EQ(metadata->findLargeValueManifest(oldSnapshot, written.reference.value_id), sourceRoute);

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(metadata->findPageLocation(current, child.page_id).segment_id, SegmentId(72U));
	EXPECT_EQ(metadata->findLargeValueManifest(current, written.reference.value_id), sourceRoute);
	EXPECT_EQ(store.read(current, written.reference), input);
	oldSnapshot.release();
	current.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedStore(io, *reopened);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopened->findLargeValueManifest(reopenedSnapshot, written.reference.value_id), sourceRoute);
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, written.reference), input);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueNonRootManifestEvacuationRecoversAtSuperblockPublicationBoundaries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	LargeValueWriteOptions options = writeOptions();
	options.extent_class = LargeValueExtentClass::LVEC_16K;
	const size_t inputBytes =
			(static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) + 1U) * RSS_LARGE_VALUE_EXTENT_16K_BYTES + 19U;
	const std::vector<std::byte> input = bytes(inputBytes);
	const LargeValueWriteResult written = store.write(input, options, LogSequenceNumber(2U));
	MetadataSnapshotPin layout = metadata->openSnapshot();
	const std::vector<LargeValueManifest> nodes = store.manifestTree(layout, written.manifest);
	ASSERT_EQ(nodes.size(), 3U);
	const PageId childPageId = nodes[1].page_id;
	layout.release();
	allocateLargeValueManifestDestination(io, *metadata, nodes[1], SegmentId(72U), PageId(502U), Generation(2U),
										  PhysicalSlotId(101U), LogSequenceNumber(3U));

	EXPECT_THROW(static_cast<void>(store.evacuatePage(
						 childPageId, SegmentId(72U), LogSequenceNumber(4U),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin before = metadata->openSnapshot();
	EXPECT_NE(metadata->findPageLocation(before, childPageId).segment_id, SegmentId(72U));
	EXPECT_EQ(store.read(before, written.reference), input);
	before.release();

	metadata.reset();
	auto interrupted = RelocatableMetadataStore::open(io);
	LargeValueStore interruptedStore(io, *interrupted);
	EXPECT_THROW(static_cast<void>(interruptedStore.evacuatePage(
						 childPageId, SegmentId(72U), LogSequenceNumber(4U),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	interrupted.reset();

	auto recovered = RelocatableMetadataStore::open(io);
	LargeValueStore recoveredStore(io, *recovered);
	MetadataSnapshotPin after = recovered->openSnapshot();
	EXPECT_EQ(recovered->findPageLocation(after, childPageId).segment_id, SegmentId(72U));
	EXPECT_EQ(recoveredStore.read(after, written.reference), input);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueDataEvacuationRecoversAtSuperblockPublicationBoundaries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input = bytes(17U);
	const LargeValueWriteResult written = store.write(input, writeOptions(), LogSequenceNumber(2U));
	const PageId dataPageId = written.manifest.chunks.front().first_data_page_id;
	allocateLargeValueDataDestination(io, *metadata, written.reference, SegmentId(71U), PageId(501U), Generation(2U),
									  PhysicalSlotId(100U), LogSequenceNumber(3U));

	EXPECT_THROW(static_cast<void>(store.evacuatePage(
						 dataPageId, SegmentId(71U), LogSequenceNumber(4U),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin before = metadata->openSnapshot();
	EXPECT_NE(metadata->findPageLocation(before, dataPageId).segment_id, SegmentId(71U));
	EXPECT_EQ(store.read(before, written.reference), input);
	before.release();

	metadata.reset();
	auto interrupted = RelocatableMetadataStore::open(io);
	LargeValueStore interruptedStore(io, *interrupted);
	EXPECT_THROW(static_cast<void>(interruptedStore.evacuatePage(
						 dataPageId, SegmentId(71U), LogSequenceNumber(4U),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	interrupted.reset();

	auto recovered = RelocatableMetadataStore::open(io);
	LargeValueStore recoveredStore(io, *recovered);
	MetadataSnapshotPin after = recovered->openSnapshot();
	EXPECT_EQ(recovered->findPageLocation(after, dataPageId).segment_id, SegmentId(71U));
	EXPECT_EQ(recoveredStore.read(after, written.reference), input);
}
