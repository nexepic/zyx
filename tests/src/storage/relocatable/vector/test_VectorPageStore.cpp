/**
 * @file test_VectorPageStore.cpp
 * @brief Durable raw/PQ VectorPage store integration and evacuation tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

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

	class ThrowOnCancellationCheck final : public CancellationCheckpoint {
	public:
		void check() const override { throw std::runtime_error("test cancellation"); }
	};

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	VectorPageHeader header(VectorStorageKind kind, PageId pageId, Generation generation, LogSequenceNumber lsn,
							uint64_t indexId = 17) {
		VectorPageHeader result;
		result.index_id = indexId;
		result.page_id = pageId;
		result.generation = generation;
		result.kind = kind;
		result.record_bytes = 16;
		result.slot_capacity = 3;
		result.bitmap_bytes = 1;
		result.committed_lsn = lsn;
		if (kind == VectorStorageKind::VSK_PQ) {
			result.codebook_generation = Generation(1);
		}
		return result;
	}

	PageOwnerHeader owner(const VectorPageHeader &header) {
		return {header.page_id,
				header.index_id,
				header.kind == VectorStorageKind::VSK_RAW ? PageOwnerKind::POK_VECTOR_RAW
														  : PageOwnerKind::POK_VECTOR_PQ,
				0,
				RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
				header.generation,
				header.committed_lsn};
	}

	VectorPage page(const VectorPageHeader &header, std::byte marker) {
		VectorPage result = VectorPage::create(owner(header), header);
		static_cast<void>(result.insert(std::vector<std::byte>(header.record_bytes, marker)));
		return result;
	}

	SharedPagePoolAllocationRequest allocationRequest(const VectorPageHeader &header, SegmentId segmentId) {
		SharedPagePoolAllocationRequest result;
		result.segment_id = segmentId;
		result.pool_kind = header.kind == VectorStorageKind::VSK_RAW ? SegmentKind::SGK_VECTOR_RAW_POOL
																	 : SegmentKind::SGK_VECTOR_PQ_POOL;
		result.pool_owner_id = header.index_id;
		result.generation = header.generation;
		result.page_id = header.page_id;
		result.page_owner_id = header.index_id;
		result.page_owner_kind = header.kind == VectorStorageKind::VSK_RAW ? PageOwnerKind::POK_VECTOR_RAW
																		   : PageOwnerKind::POK_VECTOR_PQ;
		return result;
	}

	SharedPagePoolAppendRequest appendRequest(const VectorPageHeader &header, SegmentId segmentId) {
		SharedPagePoolAppendRequest result;
		result.segment_id = segmentId;
		result.generation = header.generation;
		result.page_id = header.page_id;
		result.page_owner_id = header.index_id;
		return result;
	}

	RawVectorMaterializationRecord rawRecord(uint64_t vectorId, size_t bytes, std::byte marker) {
		RawVectorMaterializationRecord result;
		result.vector_id = vectorId;
		result.bytes.assign(bytes, marker);
		LittleEndianCodec::writeU64(result.bytes, 0, vectorId);
		return result;
	}

	VectorRawMaterializationOptions rawOptions() {
		VectorRawMaterializationOptions result;
		result.first_slot = PhysicalSlotId(40);
		result.last_slot = PhysicalSlotId(45);
		result.maximum_records = 64;
		result.maximum_pages = 8;
		return result;
	}

	PqVectorMaterializationRecord pqRecord(uint64_t vectorId, std::initializer_list<uint8_t> codes) {
		return {vectorId, std::vector<uint8_t>(codes)};
	}

	VectorPqMaterializationOptions pqOptions() {
		VectorPqMaterializationOptions result;
		result.first_slot = PhysicalSlotId(40);
		result.last_slot = PhysicalSlotId(45);
		result.codebook_generation = Generation(7);
		result.maximum_records = 64;
		result.maximum_pages = 8;
		return result;
	}

	void overwritePageOwner(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata, PageId pageId,
							const PageOwnerHeader &owner) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE;
		snapshot.release();
		const auto bytes = PageOwnerHeaderCodec::encode(owner);
		io->writeAt(offset, bytes.data(), bytes.size());
	}

} // namespace

class RelocatableVectorPageStoreTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<StorageIO> io;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_vector_page_store_" + to_string(uuid));
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		if (file && file->is_open()) {
			file->close();
		}
		std::error_code error;
		std::filesystem::remove(path, error);
	}

	void expectReplacementDiskFullRetainsPublishedPageAndCanRetry(VectorStorageKind kind, SegmentId segmentId) {
		const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
		ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
		{
			auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
			auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
			VectorPageStore store(nativeIo, *metadata);
			const VectorPageHeader originalHeader = header(kind, PageId(101U), Generation(1U), LogSequenceNumber(2U));
			ASSERT_TRUE(store.allocate(allocationRequest(originalHeader, segmentId),
									   page(originalHeader, std::byte{0x11}), PhysicalSlotId(40U), PhysicalSlotId(40U),
									   LogSequenceNumber(2U))
								.has_value());

			MetadataSnapshotPin before = metadata->openSnapshot();
			const CatalogEntry pool = metadata->findCatalog(before, segmentId);
			const uint64_t candidateOffset = PhysicalSegmentAllocator::checkedSlotOffset(
													 before.superblock().data_region_offset, pool.physical_slot) +
											 static_cast<uint64_t>(RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U) * RSS_PAGE_SIZE;
			before.release();
			const VectorPageHeader replacementHeader =
					header(kind, originalHeader.page_id, originalHeader.generation, LogSequenceNumber(3U));
			const VectorPage replacement = page(replacementHeader, std::byte{0x22});
			bool diskFullObserved = false;
			{
				StorageIO::ScopedNativeWriteHookForTesting failCandidate(
						[candidateOffset, &diskFullObserved](file_handle_t fd, const void *bytes, size_t byteCount,
															 pwrite_off_t offset) {
							if (static_cast<uint64_t>(offset) == candidateOffset && byteCount == RSS_PAGE_SIZE) {
								diskFullObserved = true;
#ifdef _WIN32
								SetLastError(ERROR_DISK_FULL);
#else
								errno = ENOSPC;
#endif
								return static_cast<pwrite_ssize_t>(-1);
							}
							return portable_pwrite(fd, bytes, byteCount, offset);
						});
				EXPECT_THROW(static_cast<void>(store.replace(replacement, segmentId, LogSequenceNumber(2U),
															 LogSequenceNumber(3U))),
							 std::system_error);
			}
			EXPECT_TRUE(diskFullObserved);
			MetadataSnapshotPin current = metadata->openSnapshot();
			EXPECT_EQ(store.read(current, originalHeader.page_id).read(0U),
					  std::vector<std::byte>(16U, std::byte{0x11}));
			EXPECT_EQ(metadata->findPageLocation(current, originalHeader.page_id).page_index,
					  RSS_PAGE_POOL_FIRST_DATA_FRAME);
			current.release();
			metadata.reset();

			auto recoveredMetadata = RelocatableMetadataStore::open(nativeIo);
			VectorPageStore recoveredStore(nativeIo, *recoveredMetadata);
			MetadataSnapshotPin recovered = recoveredMetadata->openSnapshot();
			EXPECT_EQ(recoveredStore.read(recovered, originalHeader.page_id).read(0U),
					  std::vector<std::byte>(16U, std::byte{0x11}));
			EXPECT_EQ(recoveredMetadata->findPageLocation(recovered, originalHeader.page_id).page_index,
					  RSS_PAGE_POOL_FIRST_DATA_FRAME);
			recovered.release();
			EXPECT_EQ(recoveredStore.replace(replacement, segmentId, LogSequenceNumber(2U), LogSequenceNumber(3U))
							  .location.page_index,
					  2U);
			MetadataSnapshotPin published = recoveredMetadata->openSnapshot();
			EXPECT_EQ(recoveredStore.read(published, originalHeader.page_id).read(0U),
					  std::vector<std::byte>(16U, std::byte{0x22}));
		}
		EXPECT_EQ(portable_close_rw(nativeHandle), 0);
	}

	void expectReplacementFinalSyncFailureReportsUncertainDurabilityAndReopensCompletePage(VectorStorageKind kind,
																						   SegmentId segmentId) {
		const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
		ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
		{
			auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
			auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
			VectorPageStore store(nativeIo, *metadata);
			const VectorPageHeader originalHeader = header(kind, PageId(101U), Generation(1U), LogSequenceNumber(2U));
			ASSERT_TRUE(store.allocate(allocationRequest(originalHeader, segmentId),
									   page(originalHeader, std::byte{0x11}), PhysicalSlotId(40U), PhysicalSlotId(40U),
									   LogSequenceNumber(2U))
								.has_value());
			const VectorPageHeader replacementHeader =
					header(kind, originalHeader.page_id, originalHeader.generation, LogSequenceNumber(3U));
			const VectorPage replacement = page(replacementHeader, std::byte{0x22});
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
				EXPECT_THROW(static_cast<void>(store.replace(replacement, segmentId, LogSequenceNumber(2U),
															 LogSequenceNumber(3U))),
							 std::runtime_error);
			}
			EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
			MetadataSnapshotPin current = metadata->openSnapshot();
			EXPECT_EQ(store.read(current, originalHeader.page_id).read(0U),
					  std::vector<std::byte>(16U, std::byte{0x22}));
			EXPECT_EQ(metadata->findPageLocation(current, originalHeader.page_id).page_index, 2U);
			current.release();
			metadata.reset();

			auto recoveredMetadata = RelocatableMetadataStore::open(nativeIo);
			VectorPageStore recoveredStore(nativeIo, *recoveredMetadata);
			MetadataSnapshotPin recovered = recoveredMetadata->openSnapshot();
			EXPECT_EQ(recoveredStore.read(recovered, originalHeader.page_id).read(0U),
					  std::vector<std::byte>(16U, std::byte{0x22}));
			EXPECT_EQ(recoveredMetadata->findPageLocation(recovered, originalHeader.page_id).page_index, 2U);
		}
		EXPECT_EQ(portable_close_rw(nativeHandle), 0);
	}

	void expectReplacementPrePublicationSyncFailureRetainsPublishedPageAndCanRetry(VectorStorageKind kind,
																				   SegmentId segmentId) {
		const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
		ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
		{
			auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
			auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
			VectorPageStore store(nativeIo, *metadata);
			const VectorPageHeader originalHeader = header(kind, PageId(101U), Generation(1U), LogSequenceNumber(2U));
			ASSERT_TRUE(store.allocate(allocationRequest(originalHeader, segmentId),
									   page(originalHeader, std::byte{0x11}), PhysicalSlotId(40U), PhysicalSlotId(40U),
									   LogSequenceNumber(2U))
								.has_value());
			const VectorPageHeader replacementHeader =
					header(kind, originalHeader.page_id, originalHeader.generation, LogSequenceNumber(3U));
			const VectorPage replacement = page(replacementHeader, std::byte{0x22});
			std::atomic<uint32_t> syncCalls{0U};
			{
				StorageIO::ScopedNativeSyncHookForTesting failDataFence([&syncCalls](file_handle_t fd) {
					if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 0U) {
#ifdef _WIN32
						SetLastError(ERROR_DISK_FULL);
#else
						errno = ENOSPC;
#endif
						return -1;
					}
					return portable_fsync(fd);
				});
				EXPECT_THROW(static_cast<void>(store.replace(replacement, segmentId, LogSequenceNumber(2U),
															 LogSequenceNumber(3U))),
							 std::runtime_error);
			}
			EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 1U);
			MetadataSnapshotPin current = metadata->openSnapshot();
			EXPECT_EQ(store.read(current, originalHeader.page_id).read(0U),
					  std::vector<std::byte>(16U, std::byte{0x11}));
			EXPECT_EQ(metadata->findPageLocation(current, originalHeader.page_id).page_index,
					  RSS_PAGE_POOL_FIRST_DATA_FRAME);
			current.release();
			metadata.reset();

			auto recoveredMetadata = RelocatableMetadataStore::open(nativeIo);
			VectorPageStore recoveredStore(nativeIo, *recoveredMetadata);
			MetadataSnapshotPin recovered = recoveredMetadata->openSnapshot();
			EXPECT_EQ(recoveredStore.read(recovered, originalHeader.page_id).read(0U),
					  std::vector<std::byte>(16U, std::byte{0x11}));
			EXPECT_EQ(recoveredMetadata->findPageLocation(recovered, originalHeader.page_id).page_index,
					  RSS_PAGE_POOL_FIRST_DATA_FRAME);
			recovered.release();
			EXPECT_EQ(recoveredStore.replace(replacement, segmentId, LogSequenceNumber(2U), LogSequenceNumber(3U))
							  .location.page_index,
					  2U);
		}
		EXPECT_EQ(portable_close_rw(nativeHandle), 0);
	}
};

TEST_F(RelocatableVectorPageStoreTest, MaterializesRawAndPqPagesThenAppendsAndReopensThem) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const VectorPageHeader rawHeader =
			header(VectorStorageKind::VSK_RAW, PageId(101), Generation(1), LogSequenceNumber(2));
	const VectorPage rawPage = page(rawHeader, std::byte{0x11});
	ASSERT_TRUE(store.allocate(allocationRequest(rawHeader, SegmentId(71)), rawPage, PhysicalSlotId(40),
							   PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	const VectorPageHeader appendedHeader =
			header(VectorStorageKind::VSK_RAW, PageId(102), Generation(1), LogSequenceNumber(3));
	const VectorPage appendedPage = page(appendedHeader, std::byte{0x22});
	static_cast<void>(store.append(appendRequest(appendedHeader, SegmentId(71)), appendedPage, LogSequenceNumber(3)));
	const VectorPageHeader pqHeader =
			header(VectorStorageKind::VSK_PQ, PageId(201), Generation(1), LogSequenceNumber(4));
	const VectorPage pqPage = page(pqHeader, std::byte{0x33});
	ASSERT_TRUE(store.allocate(allocationRequest(pqHeader, SegmentId(72)), pqPage, PhysicalSlotId(41),
							   PhysicalSlotId(41), LogSequenceNumber(4))
						.has_value());
	const VectorPageHeader appendedPqHeader =
			header(VectorStorageKind::VSK_PQ, PageId(202), Generation(1), LogSequenceNumber(5));
	static_cast<void>(store.append(appendRequest(appendedPqHeader, SegmentId(72)),
								   page(appendedPqHeader, std::byte{0x44}), LogSequenceNumber(5)));
	const VectorPageHeader replacedPqHeader =
			header(VectorStorageKind::VSK_PQ, PageId(202), Generation(1), LogSequenceNumber(6));
	static_cast<void>(store.replace(page(replacedPqHeader, std::byte{0x55}), SegmentId(72), LogSequenceNumber(5),
									LogSequenceNumber(6)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(store.read(snapshot, rawHeader.page_id).read(0), std::vector<std::byte>(16U, std::byte{0x11}));
	EXPECT_EQ(store.read(snapshot, appendedHeader.page_id).read(0), std::vector<std::byte>(16U, std::byte{0x22}));
	EXPECT_EQ(store.read(snapshot, pqHeader.page_id).read(0), std::vector<std::byte>(16U, std::byte{0x33}));
	EXPECT_EQ(store.read(snapshot, appendedPqHeader.page_id).read(0), std::vector<std::byte>(16U, std::byte{0x55}));
	snapshot.release();
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	VectorPageStore reopenedStore(io, *reopened);
	const MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	VectorPageHeader expectedAppended = appendedHeader;
	expectedAppended.live_count = 1;
	VectorPageHeader expectedPq = pqHeader;
	expectedPq.live_count = 1;
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, appendedHeader.page_id).header(), expectedAppended);
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, pqHeader.page_id).header(), expectedPq);
	VectorPageHeader expectedReplacedPq = replacedPqHeader;
	expectedReplacedPq.live_count = 1;
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, appendedPqHeader.page_id).header(), expectedReplacedPq);
}

TEST_F(RelocatableVectorPageStoreTest, RawReplacementDiskFullRetainsThePublishedPageAndCanRetryAfterReopen) {
	expectReplacementDiskFullRetainsPublishedPageAndCanRetry(VectorStorageKind::VSK_RAW, SegmentId(71U));
}

TEST_F(RelocatableVectorPageStoreTest, PqReplacementDiskFullRetainsThePublishedPageAndCanRetryAfterReopen) {
	expectReplacementDiskFullRetainsPublishedPageAndCanRetry(VectorStorageKind::VSK_PQ, SegmentId(72U));
}

TEST_F(RelocatableVectorPageStoreTest, RawReplacementFinalSyncFailureReopensTheCompletePublishedPage) {
	expectReplacementFinalSyncFailureReportsUncertainDurabilityAndReopensCompletePage(VectorStorageKind::VSK_RAW,
																					  SegmentId(71U));
}

TEST_F(RelocatableVectorPageStoreTest, PqReplacementFinalSyncFailureReopensTheCompletePublishedPage) {
	expectReplacementFinalSyncFailureReportsUncertainDurabilityAndReopensCompletePage(VectorStorageKind::VSK_PQ,
																					  SegmentId(72U));
}

TEST_F(RelocatableVectorPageStoreTest, RawReplacementPrePublicationSyncFailureRetainsThePublishedPageAndCanRetry) {
	expectReplacementPrePublicationSyncFailureRetainsPublishedPageAndCanRetry(VectorStorageKind::VSK_RAW,
																			  SegmentId(71U));
}

TEST_F(RelocatableVectorPageStoreTest, PqReplacementPrePublicationSyncFailureRetainsThePublishedPageAndCanRetry) {
	expectReplacementPrePublicationSyncFailureRetainsPublishedPageAndCanRetry(VectorStorageKind::VSK_PQ,
																			  SegmentId(72U));
}

TEST_F(RelocatableVectorPageStoreTest, SequentialReadDoesNotRetainAColdVectorPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const VectorPageHeader firstHeader =
			header(VectorStorageKind::VSK_RAW, PageId(101U), Generation(1U), LogSequenceNumber(2U));
	ASSERT_TRUE(store.allocate(allocationRequest(firstHeader, SegmentId(71U)), page(firstHeader, std::byte{0x11}),
							   PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
						.has_value());
	const VectorPageHeader secondHeader =
			header(VectorStorageKind::VSK_RAW, PageId(102U), Generation(1U), LogSequenceNumber(3U));
	static_cast<void>(store.append(appendRequest(secondHeader, SegmentId(71U)), page(secondHeader, std::byte{0x22}),
								   LogSequenceNumber(3U)));

	io->sync();
	io->enableBufferPool(64U * RSS_PAGE_SIZE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	static_cast<void>(store.read(snapshot, firstHeader.page_id, BufferPoolReadHint::BPRH_NORMAL));
	io->resetBufferPoolStats();
	const BufferPoolStats before = io->bufferPoolStats();

	EXPECT_EQ(store.read(snapshot, secondHeader.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL).read(0U),
			  std::vector<std::byte>(16U, std::byte{0x22}));
	const BufferPoolStats afterFirst = io->bufferPoolStats();
	EXPECT_EQ(afterFirst.disk_read_bytes, before.disk_read_bytes + RSS_PAGE_SIZE);
	EXPECT_EQ(afterFirst.resident_bytes, before.resident_bytes);

	EXPECT_EQ(store.read(snapshot, secondHeader.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL).read(0U),
			  std::vector<std::byte>(16U, std::byte{0x22}));
	const BufferPoolStats afterSecond = io->bufferPoolStats();
	EXPECT_EQ(afterSecond.disk_read_bytes, afterFirst.disk_read_bytes + RSS_PAGE_SIZE);
	EXPECT_EQ(afterSecond.resident_bytes, afterFirst.resident_bytes);
	EXPECT_GT(afterSecond.misses, afterFirst.misses);
	snapshot.release();
}

TEST_F(RelocatableVectorPageStoreTest, MaterializesRawVectorPagesAndLocationsAtomicallyAcrossPools) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	std::vector<RawVectorMaterializationRecord> records;
	for (uint64_t vectorId = 1; vectorId <= 33; ++vectorId) {
		records.push_back(rawRecord(vectorId, 2000, static_cast<std::byte>(vectorId)));
	}
	VectorRawMaterializationOptions options = rawOptions();
	options.maximum_pages = 40;
	const VectorRawMaterializationResult result = store.materializeRaw(17, records, options, LogSequenceNumber(2));
	EXPECT_EQ(result.record_count, records.size());
	ASSERT_EQ(result.pages.size(), 33U);
	EXPECT_LT(result.pages.front().page_id, result.pages.back().page_id);
	EXPECT_GT(result.superblock.generation, Generation(1));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	for (size_t index = 0; index < records.size(); ++index) {
		const VectorLocation location = metadata->findVectorLocation(snapshot, records[index].vector_id);
		const VectorPage page = store.read(snapshot, location.page_id);
		EXPECT_EQ(page.header().index_id, 17U);
		EXPECT_EQ(page.header().kind, VectorStorageKind::VSK_RAW);
		EXPECT_EQ(page.read(location.slot_id, location.slot_generation), records[index].bytes);
	}
	for (const VectorRawMaterializationPage &page: result.pages) {
		EXPECT_EQ(metadata->findPageLocation(snapshot, page.page_id).free_bytes, 0U);
	}
	EXPECT_EQ(snapshot.superblock().next_logical_vector_id, 34U);
	snapshot.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	VectorPageStore reopenedStore(io, *reopened);
	const MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	const VectorLocation location = reopened->findVectorLocation(reopenedSnapshot, 33);
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, location.page_id).read(location.slot_id, location.slot_generation),
			  records.back().bytes);
}

TEST_F(RelocatableVectorPageStoreTest, RetiresRawPagesAndTheirVectorLocationsInOneGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const std::array<RawVectorMaterializationRecord, 2U> records = {rawRecord(1U, 16U, std::byte{0x11}),
																	rawRecord(2U, 16U, std::byte{0x22})};
	const VectorRawMaterializationResult materialized =
			store.materializeRaw(17U, records, rawOptions(), LogSequenceNumber(2U));
	ASSERT_EQ(materialized.pages.size(), 1U);

	MetadataSnapshotPin pinned = metadata->openSnapshot();
	const PageId pageId = materialized.pages.front().page_id;
	const PageLocation location = metadata->findPageLocation(pinned, pageId);
	const VectorPage page = store.read(pinned, pageId);
	const LogSequenceNumber retirementLsn(pinned.superblock().checkpoint_lsn.value() + 1U);
	const std::array<RetiredRawVectorPage, 1U> pages = {{{location, 17U, page.header().committed_lsn}}};
	const std::array<uint64_t, 2U> erasedVectorIds = {1U, 2U};
	static_cast<void>(store.retireRawPages(pinned.superblock().generation, pages, erasedVectorIds, retirementLsn));

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findPageLocationOptional(current, pageId).has_value());
	EXPECT_FALSE(metadata->findVectorLocationOptional(current, 1U).has_value());
	EXPECT_FALSE(metadata->findVectorLocationOptional(current, 2U).has_value());
	current.release();
	EXPECT_EQ(metadata->findVectorLocation(pinned, 1U).page_id, pageId);
	EXPECT_EQ(store.read(pinned, pageId).header().index_id, 17U);
	pinned.release();
}

TEST_F(RelocatableVectorPageStoreTest, RetiresDerivedPqPagesWhilePinnedSnapshotsRetainTheirMapping) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const std::array<PqVectorMaterializationRecord, 2U> records = {pqRecord(1U, {1U, 2U}),
																		 pqRecord(2U, {3U, 4U})};
	const VectorPqMaterializationResult materialized =
			store.materializePq(17U, records, pqOptions(), LogSequenceNumber(2U));
	ASSERT_EQ(materialized.pages.size(), 1U);

	MetadataSnapshotPin pinned = metadata->openSnapshot();
	const PageId pageId = materialized.pages.front().page_id;
	const std::optional<DerivedVectorPqPage> observed = store.inspectDerivedPqPage(pinned, pageId);
	ASSERT_TRUE(observed.has_value());
	const LogSequenceNumber retirementLsn(pinned.superblock().checkpoint_lsn.value() + 1U);
	const std::array<DerivedVectorPqPage, 1U> pages{*observed};
	static_cast<void>(store.retireDerivedPqPages(pinned.superblock().generation, pages, retirementLsn));

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findPageLocationOptional(current, pageId).has_value());
	current.release();
	EXPECT_EQ(store.read(pinned, pageId).header().kind, VectorStorageKind::VSK_PQ);
	pinned.release();
}

TEST_F(RelocatableVectorPageStoreTest, MaterializesPqPagesWithoutPollutingTheRawVectorLocationDirectory) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const std::vector<PqVectorMaterializationRecord> records = {pqRecord(31, {1, 2, 3, 4}), pqRecord(32, {4, 3, 2, 1}),
																pqRecord(33, {8, 9, 10, 11})};
	const VectorPqMaterializationResult result = store.materializePq(17, records, pqOptions(), LogSequenceNumber(2));
	ASSERT_EQ(result.record_count, records.size());
	ASSERT_EQ(result.mappings.size(), records.size());
	ASSERT_EQ(result.pages.size(), 1U);
	EXPECT_EQ(result.mappings.front(), (VectorPqPageMapping{31, result.pages.front().page_id}));
	EXPECT_EQ(result.mappings.back(), (VectorPqPageMapping{33, result.pages.front().page_id}));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findVectorLocationOptional(snapshot, 31).has_value());
	const VectorPage page = store.read(snapshot, result.pages.front().page_id);
	EXPECT_EQ(metadata->findPageLocation(snapshot, result.pages.front().page_id).free_bytes,
			  static_cast<uint32_t>(page.header().slot_capacity - page.header().live_count) *
					  page.header().record_bytes);
	EXPECT_EQ(page.header().kind, VectorStorageKind::VSK_PQ);
	EXPECT_EQ(page.header().index_id, 17U);
	EXPECT_EQ(page.header().codebook_generation, pqOptions().codebook_generation);
	std::vector<std::byte> first(sizeof(uint64_t) + records.front().codes.size());
	LittleEndianCodec::writeU64(first, 0, records.front().vector_id);
	for (size_t index = 0; index < records.front().codes.size(); ++index) {
		first[sizeof(uint64_t) + index] = static_cast<std::byte>(records.front().codes[index]);
	}
	EXPECT_EQ(page.read(0), first);
	snapshot.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	VectorPageStore reopenedStore(io, *reopened);
	const MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, result.pages.front().page_id).header().codebook_generation,
			  pqOptions().codebook_generation);
}

TEST_F(RelocatableVectorPageStoreTest, RejectsMalformedPqMaterializationBeforePublishingPages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const std::vector<PqVectorMaterializationRecord> valid = {pqRecord(31, {1, 2}), pqRecord(32, {3, 4})};
	const std::vector<PqVectorMaterializationRecord> emptyCodes = {pqRecord(31, {})};
	expectFormatError(
			[&] { static_cast<void>(store.materializePq(17, emptyCodes, pqOptions(), LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_HEADER);
	VectorPqMaterializationOptions invalid = pqOptions();
	invalid.codebook_generation = {};
	expectFormatError([&] { static_cast<void>(store.materializePq(17, valid, invalid, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	std::vector<PqVectorMaterializationRecord> malformed = valid;
	malformed.back().codes.push_back(5);
	expectFormatError([&] { static_cast<void>(store.materializePq(17, malformed, pqOptions(), LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	malformed = valid;
	malformed.front().vector_id = 0U;
	expectFormatError(
			[&] { static_cast<void>(store.materializePq(17U, malformed, pqOptions(), LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_HEADER);
	malformed = valid;
	malformed.back().vector_id = malformed.front().vector_id;
	expectFormatError([&] { static_cast<void>(store.materializePq(17, malformed, pqOptions(), LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = pqOptions();
	invalid.maximum_pages = 0;
	expectFormatError([&] { static_cast<void>(store.materializePq(17, valid, invalid, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findVectorLocationOptional(snapshot, valid.front().vector_id).has_value());
}

TEST_F(RelocatableVectorPageStoreTest, RejectsMalformedPreparedPqBatchesAndDuplicateRetirementFrames) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const std::array<PqVectorMaterializationRecord, 1U> source{{pqRecord(1U, {1U, 2U})}};
	std::array<VectorPqMaterializationPoolBatch, 0U> noPools{};
	expectFormatError(
			[&] {
				static_cast<void>(
						store.preparePqNewPoolPages(17U, source, pqOptions(), noPools, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);
	std::array<VectorPqMaterializationPoolBatch, 1U> malformedPool{};
	expectFormatError(
			[&] {
				static_cast<void>(
						store.preparePqNewPoolPages(17U, source, pqOptions(), malformedPool, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);
	VectorPqMaterializationOptions singlePage = pqOptions();
	const uint16_t slotsPerPage = VectorPageStore::rawSlotCapacity(sizeof(uint64_t) + source.front().codes.size());
	singlePage.maximum_records = static_cast<size_t>(slotsPerPage) + 1U;
	singlePage.maximum_pages = 1U;
	std::vector<PqVectorMaterializationRecord> overflowingSinglePage;
	for (uint64_t vectorId = 1U; vectorId <= singlePage.maximum_records; ++vectorId) {
		overflowingSinglePage.push_back(pqRecord(vectorId, {1U, 2U}));
	}
	expectFormatError(
			[&] {
				static_cast<void>(store.preparePqNewPoolPages(17U, overflowingSinglePage, singlePage, noPools,
															  LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);
	std::array<VectorPqMaterializationPoolBatch, 1U> invalidLocationBatch{};
	invalidLocationBatch.front().pool = {
			SegmentId(71U),			  PhysicalSlotId(40U),	SegmentKind::SGK_VECTOR_PQ_POOL, 17U, Generation(1U),
			SegmentState::SGS_SEALED, LogSequenceNumber(2U)};
	invalidLocationBatch.front().page_ids = {PageId(101U)};
	invalidLocationBatch.front().locations = {PageLocation{}};
	expectFormatError(
			[&] {
				static_cast<void>(store.preparePqNewPoolPages(17U, source, pqOptions(), invalidLocationBatch,
															  LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);

	const std::array<ExistingPoolPageRetirementBatch, 0U> noRetirements{};
	expectFormatError([&] { store.prepareDerivedPqRetirements(noRetirements, LogSequenceNumber(2U)); },
					  FormatError::FERR_INVALID_HEADER);
	const std::array<ExistingPoolPageRetirementBatch, 1U> malformedRetirementPool{};
	expectFormatError([&] { store.prepareDerivedPqRetirements(malformedRetirementPool, LogSequenceNumber(2U)); },
					  FormatError::FERR_INVALID_STATE);

	const VectorPqMaterializationResult materialized =
			store.materializePq(17U, source, pqOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PageLocation location = metadata->findPageLocation(snapshot, materialized.pages.front().page_id);
	const CatalogEntry pool = metadata->findCatalog(snapshot, location.segment_id);
	snapshot.release();
	const PageRetirement retirement{location, 17U, PageOwnerKind::POK_VECTOR_PQ, LogSequenceNumber(2U)};
	const ExistingPoolPageRetirementBatch validPool{pool, {retirement}};
	expectFormatError([&] { store.prepareDerivedPqRetirements(std::span(&validPool, 1U), {}); },
					  FormatError::FERR_INVALID_HEADER);
	ExistingPoolPageRetirementBatch malformedRetirement{pool, {retirement}};
	malformedRetirement.retirements.front().location.page_index = 0U;
	expectFormatError(
			[&] {
				store.prepareDerivedPqRetirements(std::span(&malformedRetirement, 1U), LogSequenceNumber(3U));
			},
			FormatError::FERR_INVALID_STATE);
	const ExistingPoolPageRetirementBatch duplicatePage{pool, {retirement, retirement}};
	expectFormatError([&] { store.prepareDerivedPqRetirements(std::span(&duplicatePage, 1U), LogSequenceNumber(3U)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorPageStoreTest, CancellationPreventsPreparedPqMaterializationAndRetirement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const std::array<PqVectorMaterializationRecord, 1U> source{{pqRecord(1U, {1U, 2U})}};
	std::array<VectorPqMaterializationPoolBatch, 0U> noPools{};
	ThrowOnCancellationCheck cancellation;
	EXPECT_THROW(static_cast<void>(
				 store.preparePqNewPoolPages(17U, source, pqOptions(), noPools, LogSequenceNumber(2U), &cancellation)),
			 std::runtime_error);

	const VectorPqMaterializationResult materialized =
			store.materializePq(17U, source, pqOptions(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PageLocation location = metadata->findPageLocation(snapshot, materialized.pages.front().page_id);
	const CatalogEntry pool = metadata->findCatalog(snapshot, location.segment_id);
	EXPECT_TRUE(store.read(snapshot, materialized.pages.front().page_id).findVectorId(1U).has_value());
	snapshot.release();
	const ExistingPoolPageRetirementBatch retirementPool{
			pool, {{location, 17U, PageOwnerKind::POK_VECTOR_PQ, LogSequenceNumber(2U)}}};
	EXPECT_THROW(store.prepareDerivedPqRetirements(std::span(&retirementPool, 1U), LogSequenceNumber(3U), &cancellation),
			 std::runtime_error);

	const MetadataSnapshotPin afterCancellation = metadata->openSnapshot();
	EXPECT_TRUE(store.read(afterCancellation, materialized.pages.front().page_id).findVectorId(1U).has_value());
}

TEST_F(RelocatableVectorPageStoreTest, RejectsPqAndRawRecordsThatCannotFitWithinOneFixedPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const std::vector<PqVectorMaterializationRecord> overWidePq{
			{1, std::vector<uint8_t>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, 1U)}};
	expectFormatError(
			[&] { static_cast<void>(store.materializePq(17, overWidePq, pqOptions(), LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_HEADER);
	const std::vector<PqVectorMaterializationRecord> noSlotPq{
			{1, std::vector<uint8_t>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - sizeof(uint64_t), 1U)}};
	expectFormatError([&] { static_cast<void>(store.materializePq(17, noSlotPq, pqOptions(), LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	const std::vector<RawVectorMaterializationRecord> noSlotRaw{
			rawRecord(1, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, std::byte{0x11})};
	expectFormatError(
			[&] { static_cast<void>(store.materializeRaw(17, noSlotRaw, rawOptions(), LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_HEADER);
	const std::vector<RawVectorMaterializationRecord> overWideRaw{
			rawRecord(1, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 1U, std::byte{0x11})};
	expectFormatError(
			[&] { static_cast<void>(store.materializeRaw(17, overWideRaw, rawOptions(), LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_HEADER);
	const std::vector<PqVectorMaterializationRecord> multiPagePq{{1, std::vector<uint8_t>(2000U, 1U)},
																 {2, std::vector<uint8_t>(2000U, 2U)}};
	VectorPqMaterializationOptions onePage = pqOptions();
	onePage.maximum_pages = 1;
	expectFormatError([&] { static_cast<void>(store.materializePq(17, multiPagePq, onePage, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorPageStoreTest, RetainsUnknownPqOwnersAndRejectsRetiredPqFrames) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const std::vector<PqVectorMaterializationRecord> records{pqRecord(1, {1, 2})};
	const VectorPqMaterializationResult result = store.materializePq(17, records, pqOptions(), LogSequenceNumber(2));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PageLocation location = metadata->findPageLocation(snapshot, result.pages.front().page_id);
	ASSERT_TRUE(store.inspectDerivedPqPage(snapshot, result.pages.front().page_id).has_value());
	snapshot.release();
	const PageOwnerHeader unknown{result.pages.front().page_id,
								  99,
								  PageOwnerKind::POK_VECTOR_PQ,
								  0,
								  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
								  location.generation,
								  LogSequenceNumber(2)};
	overwritePageOwner(io, *metadata, result.pages.front().page_id, unknown);
	snapshot = metadata->openSnapshot();
	EXPECT_FALSE(store.inspectDerivedPqPage(snapshot, result.pages.front().page_id).has_value());
	snapshot.release();
	PageOwnerHeader retired = unknown;
	retired.owner_id = 17;
	retired.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
	overwritePageOwner(io, *metadata, result.pages.front().page_id, retired);
	snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.inspectDerivedPqPage(snapshot, result.pages.front().page_id)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorPageStoreTest, RetainsAHighBitPqOwnerNamespaceForForwardCompatibility) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const uint64_t futureIndexId = (UINT64_C(1) << 63U) | UINT64_C(17);
	const std::vector<PqVectorMaterializationRecord> records{pqRecord(1, {1, 2})};
	const VectorPqMaterializationResult result =
			store.materializePq(futureIndexId, records, pqOptions(), LogSequenceNumber(2));
	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(store.inspectDerivedPqPage(snapshot, result.pages.front().page_id).has_value());
}

TEST_F(RelocatableVectorPageStoreTest, BuildsAnIndependentImmutablePqMappingAndVerifiesThePageLocalId) {
	constexpr uint64_t mappingIndexId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x11);
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore vectors(io, *metadata);
	const std::vector<PqVectorMaterializationRecord> records = {pqRecord(31, {1, 2}), pqRecord(32, {3, 4}),
																pqRecord(33, {5, 6})};
	const VectorPqMaterializationResult pq = vectors.materializePq(17, records, pqOptions(), LogSequenceNumber(2));
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const BulkBTreeBuildOptions treeOptions{PhysicalSlotId(42), PhysicalSlotId(45), mappingIndexId, 16, 4};
	const VectorPqMappingBuildOptions mappingOptions{treeOptions, 16};
	const BulkBTreeBuildResult built =
			VectorPqMapping::build(trees, *metadata, mappingIndexId, pq.mappings, mappingOptions, LogSequenceNumber(3));
	EXPECT_EQ(built.entry_count, records.size());

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::optional<PageId> pageId = VectorPqMapping::findPage(snapshot, trees, *metadata, mappingIndexId, 32);
	ASSERT_TRUE(pageId.has_value());
	const VectorPage page = vectors.read(snapshot, *pageId);
	EXPECT_EQ(page.header().kind, VectorStorageKind::VSK_PQ);
	const std::optional<uint16_t> slot = page.findVectorId(32);
	ASSERT_TRUE(slot.has_value());
	EXPECT_EQ(LittleEndianCodec::readU64(page.read(*slot), 0), 32U);
	EXPECT_FALSE(VectorPqMapping::findPage(snapshot, trees, *metadata, mappingIndexId, 34).has_value());
	snapshot.release();

	expectFormatError(
			[&] {
				static_cast<void>(VectorPqMapping::build(trees, *metadata, mappingIndexId, pq.mappings, mappingOptions,
														 LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { VectorPqMapping::validateMappingIndexId(17); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorPageStoreTest, RejectsReuseBelowTheDurableVectorIdHighWaterMark) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const std::array first = {rawRecord(10U, 16U, std::byte{0x11})};
	static_cast<void>(store.materializeRaw(17U, first, rawOptions(), LogSequenceNumber(2U)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(snapshot.superblock().next_logical_vector_id, 11U);
	EXPECT_FALSE(metadata->findVectorLocationOptional(snapshot, 5U).has_value());
	snapshot.release();

	const std::array reused = {rawRecord(5U, 16U, std::byte{0x22})};
	expectFormatError(
			[&] { static_cast<void>(store.materializeRaw(17U, reused, rawOptions(), LogSequenceNumber(3U))); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorPageStoreTest, RejectsNonCanonicalOrUnboundedRawMaterializationInput) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	expectFormatError([&] { static_cast<void>(VectorPageStore::rawSlotCapacity(0U)); },
					  FormatError::FERR_INVALID_HEADER);
	const RawVectorMaterializationRecord valid = rawRecord(1, 16, std::byte{0x11});
	VectorRawMaterializationOptions options = rawOptions();
	expectFormatError([&] { static_cast<void>(store.materializeRaw(17, {}, options, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	const std::array<RawVectorMaterializationRecord, 1U> missingStableIdPrefix = {
			RawVectorMaterializationRecord{1U, std::vector<std::byte>(sizeof(uint64_t) - 1U, std::byte{0x11})}};
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeRaw(17U, missingStableIdPrefix, options, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	std::vector<RawVectorMaterializationRecord> mismatched{valid};
	LittleEndianCodec::writeU64(mismatched.front().bytes, 0, 2);
	expectFormatError([&] { static_cast<void>(store.materializeRaw(17, mismatched, options, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	std::vector<RawVectorMaterializationRecord> duplicate{valid, rawRecord(1, 16, std::byte{0x22})};
	expectFormatError([&] { static_cast<void>(store.materializeRaw(17, duplicate, options, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	options.maximum_records = 1;
	std::vector<RawVectorMaterializationRecord> two{valid, rawRecord(2, 16, std::byte{0x22})};
	expectFormatError([&] { static_cast<void>(store.materializeRaw(17, two, options, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	options = rawOptions();
	options.maximum_pages = 1;
	std::vector<RawVectorMaterializationRecord> tooMany;
	for (uint64_t vectorId = 1; vectorId <= 4; ++vectorId) {
		tooMany.push_back(rawRecord(vectorId, 2000, std::byte{0x33}));
	}
	expectFormatError([&] { static_cast<void>(store.materializeRaw(17, tooMany, options, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorPageStoreTest, ValidatesRawPreparationCapacityAndRetirementContracts) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const VectorPageHeader rawHeader =
			header(VectorStorageKind::VSK_RAW, PageId(101U), Generation(1U), LogSequenceNumber(2U));
	const VectorPage rawPage = page(rawHeader, std::byte{0x11});
	ASSERT_TRUE(store.allocate(allocationRequest(rawHeader, SegmentId(71U)), rawPage, PhysicalSlotId(40U),
							   PhysicalSlotId(40U), LogSequenceNumber(2U))
						.has_value());

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry pool = metadata->findCatalog(snapshot, SegmentId(71U));
	const PageLocation location = metadata->findPageLocation(snapshot, rawHeader.page_id);
	snapshot.release();
	CatalogEntry wrongKind = pool;
	wrongKind.kind = SegmentKind::SGK_VECTOR_PQ_POOL;
	EXPECT_FALSE(store.hasReplacementFrameCapacity(wrongKind, 1U));
	CatalogEntry wrongState = pool;
	wrongState.state = SegmentState::SGS_BUILDING;
	EXPECT_FALSE(store.hasReplacementFrameCapacity(wrongState, 1U));
	EXPECT_FALSE(store.hasReplacementFrameCapacity(pool, 0U));
	EXPECT_TRUE(store.hasReplacementFrameCapacity(pool, 1U));
	EXPECT_FALSE(store.hasReplacementFrameCapacity(
			pool, static_cast<size_t>(SharedPagePoolAllocator(io, *metadata).availableFrameCount(pool)) + 1U));

	PageLocation prepared;
	expectFormatError([&] { store.prepareRawReplacement({}, location, {}, rawPage, LogSequenceNumber(3U), prepared); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { store.prepareRawAppend({}, rawPage, LogSequenceNumber(3U), prepared); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { store.prepareRawNewPool({}, rawPage, LogSequenceNumber(3U), prepared); },
					  FormatError::FERR_INVALID_STATE);
	std::array<VectorPage, 1U> pages{page(rawHeader, std::byte{0x12})};
	std::array<PageLocation, 1U> locations{};
	expectFormatError([&] { store.prepareRawNewPoolPages({}, pages, locations, LogSequenceNumber(3U)); },
					  FormatError::FERR_INVALID_STATE);

	const std::array<RetiredRawVectorPage, 0U> noPages{};
	const std::array<uint64_t, 0U> noVectorIds{};
	const std::array<DerivedVectorPqPage, 0U> noDerivedPqPages{};
	expectFormatError(
			[&] {
				static_cast<void>(
						store.retireDerivedPqPages(Generation(1U), noDerivedPqPages, LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array<DerivedVectorPqPage, 1U> malformedDerivedPqPages = {
			DerivedVectorPqPage{PageLocation{}, 17U, LogSequenceNumber(2U)}};
	expectFormatError(
			[&] {
				static_cast<void>(store.retireDerivedPqPages(Generation(1U), malformedDerivedPqPages,
																	LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.retireRawPages(Generation(1U), noPages, noVectorIds, LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array<RetiredRawVectorPage, 1U> invalidPages = {{{location, 0U, LogSequenceNumber(2U)}}};
	expectFormatError(
			[&] {
				static_cast<void>(
						store.retireRawPages(Generation(1U), invalidPages, noVectorIds, LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store.retireEmptyRawPool(SegmentId(71U), Generation(2U), LogSequenceNumber(3U))); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(store.releaseRetiredRawPool(SegmentId(71U), Generation(1U), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorPageStoreTest, PreparesTheFirstRawPageBeforePublishingItsCatalogAndDirectoryEntries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const VectorPageHeader rawHeader =
			header(VectorStorageKind::VSK_RAW, PageId(101U), Generation(1U), LogSequenceNumber(2U));
	const VectorPage rawPage = page(rawHeader, std::byte{0x71});
	const CatalogSlotReservationRequest reservation{.segment_id = SegmentId(71U),
												 .kind = SegmentKind::SGK_VECTOR_RAW_POOL,
												 .owner_id = rawHeader.index_id,
												 .segment_generation = Generation(1U),
												 .state = SegmentState::SGS_SEALED,
												 .require_absent = true};
	PageLocation location{rawHeader.page_id, reservation.segment_id, 0U, Generation(1U), 0U};
	bool prepared = false;
	const std::optional<CatalogSlotReservations> published = metadata->reserveFirstFreeAndUpsertCatalogAndLocations(
			std::span(&reservation, 1U), std::span(&location, 1U), {}, PhysicalSlotId(40U), PhysicalSlotId(40U),
			LogSequenceNumber(2U), [&store, &rawPage, &location, &prepared](std::span<const CatalogEntry> entries) {
				ASSERT_EQ(entries.size(), 1U);
				store.prepareRawNewPool(entries.front(), rawPage, LogSequenceNumber(2U), location);
				prepared = true;
			});
	ASSERT_TRUE(published.has_value());
	EXPECT_TRUE(prepared);
	EXPECT_EQ(location.page_index, RSS_PAGE_POOL_FIRST_DATA_FRAME);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry visiblePool = metadata->findCatalog(snapshot, reservation.segment_id);
	EXPECT_EQ(visiblePool.segment_id, published->entries.front().segment_id);
	EXPECT_EQ(visiblePool.physical_slot, published->entries.front().physical_slot);
	EXPECT_EQ(visiblePool.kind, published->entries.front().kind);
	EXPECT_EQ(visiblePool.owner_id, published->entries.front().owner_id);
	EXPECT_EQ(visiblePool.generation, published->entries.front().generation);
	EXPECT_EQ(visiblePool.state, published->entries.front().state);
	EXPECT_EQ(visiblePool.committed_lsn, published->entries.front().committed_lsn);
	EXPECT_EQ(metadata->findPageLocation(snapshot, rawHeader.page_id), location);
	EXPECT_EQ(store.read(snapshot, rawHeader.page_id).read(0U),
			  std::vector<std::byte>(rawHeader.record_bytes, std::byte{0x71}));
	snapshot.release();

	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	VectorPageStore reopenedStore(io, *reopened);
	const MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.read(reopenedSnapshot, rawHeader.page_id).header(), rawPage.header());
}

TEST_F(RelocatableVectorPageStoreTest, PreparesRawReplacementAndAppendPagesWithStableIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const VectorPageHeader rawHeader =
			header(VectorStorageKind::VSK_RAW, PageId(101U), Generation(1U), LogSequenceNumber(2U));
	const VectorPage rawPage = page(rawHeader, std::byte{0x71});
	ASSERT_TRUE(store.allocate(allocationRequest(rawHeader, SegmentId(71U)), rawPage, PhysicalSlotId(40U),
							   PhysicalSlotId(40U), LogSequenceNumber(2U))
						.has_value());

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry pool = metadata->findCatalog(snapshot, SegmentId(71U));
	const PageLocation expected = metadata->findPageLocation(snapshot, rawHeader.page_id);
	snapshot.release();

	const VectorPageHeader replacementHeader =
			header(VectorStorageKind::VSK_RAW, rawHeader.page_id, Generation(1U), LogSequenceNumber(3U));
	PageLocation replacement;
	store.prepareRawReplacement(pool, expected, LogSequenceNumber(2U), page(replacementHeader, std::byte{0x72}),
							LogSequenceNumber(3U), replacement);
	EXPECT_EQ(replacement.page_id, rawHeader.page_id);
	EXPECT_EQ(replacement.segment_id, pool.segment_id);
	EXPECT_EQ(replacement.generation, pool.generation);
	EXPECT_GT(replacement.page_index, 0U);

	const VectorPageHeader appendedHeader =
			header(VectorStorageKind::VSK_RAW, PageId(102U), Generation(1U), LogSequenceNumber(4U));
	PageLocation appended{appendedHeader.page_id, pool.segment_id, 0U, pool.generation, 0U};
	store.prepareRawAppend(pool, page(appendedHeader, std::byte{0x73}), LogSequenceNumber(4U), appended);
	EXPECT_EQ(appended.page_id, appendedHeader.page_id);
	EXPECT_EQ(appended.segment_id, pool.segment_id);
	EXPECT_EQ(appended.generation, pool.generation);
	EXPECT_GT(appended.page_index, 0U);
	store.reclaimRetiredFrames(pool.segment_id);
}

TEST_F(RelocatableVectorPageStoreTest, RejectsAnInvalidInitialRawPageLocationWithoutPublishingThePool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const VectorPageHeader rawHeader =
			header(VectorStorageKind::VSK_RAW, PageId(101U), Generation(1U), LogSequenceNumber(2U));
	const std::array<VectorPage, 1U> pages{{page(rawHeader, std::byte{0x72})}};
	const CatalogSlotReservationRequest reservation{.segment_id = SegmentId(71U),
												 .kind = SegmentKind::SGK_VECTOR_RAW_POOL,
												 .owner_id = rawHeader.index_id,
												 .segment_generation = Generation(1U),
												 .state = SegmentState::SGS_SEALED,
												 .require_absent = true};
	std::array<PageLocation, 1U> locations{{
			{rawHeader.page_id, reservation.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U}}};

	expectFormatError(
			[&] {
				static_cast<void>(metadata->reserveFirstFreeAndUpsertCatalog(
						std::span(&reservation, 1U), PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U),
						[&store, &pages, &locations](std::span<const CatalogEntry> entries) {
							store.prepareRawNewPoolPages(entries.front(), pages, locations, LogSequenceNumber(2U));
						}));
			},
			FormatError::FERR_INVALID_STATE);

	const MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(metadata->findCatalog(snapshot, reservation.segment_id)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorPageStoreTest, RecoversRawVectorPageAndLocationPublicationAsOneGeneration) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const std::vector<RawVectorMaterializationRecord> records{rawRecord(1, 16, std::byte{0x11}),
															  rawRecord(2, 16, std::byte{0x22})};
	MetadataPublishOptions interrupted;
	interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.materializeRaw(17, records, rawOptions(), LogSequenceNumber(2), interrupted)),
				 BootstrapPublishInterrupted);
	metadata.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	const MetadataSnapshotPin failedSnapshot = reopened->openSnapshot();
	EXPECT_FALSE(reopened->findVectorLocationOptional(failedSnapshot, 1).has_value());
	EXPECT_FALSE(reopened->findVectorLocationOptional(failedSnapshot, 2).has_value());

	VectorPageStore reopenedStore(io, *reopened);
	const VectorRawMaterializationResult result =
			reopenedStore.materializeRaw(17, records, rawOptions(), LogSequenceNumber(2));
	const MetadataSnapshotPin successfulSnapshot = reopened->openSnapshot();
	EXPECT_TRUE(reopened->findVectorLocationOptional(successfulSnapshot, 1).has_value());
	EXPECT_TRUE(reopened->findVectorLocationOptional(successfulSnapshot, 2).has_value());
	EXPECT_EQ(result.record_count, 2U);
}

TEST_F(RelocatableVectorPageStoreTest, ReplacesAndEvacuatesPagesWhilePreservingPinnedSnapshots) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const VectorPageHeader originalHeader =
			header(VectorStorageKind::VSK_RAW, PageId(101), Generation(1), LogSequenceNumber(2));
	const VectorPage original = page(originalHeader, std::byte{0x11});
	ASSERT_TRUE(store.allocate(allocationRequest(originalHeader, SegmentId(71)), original, PhysicalSlotId(40),
							   PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	const VectorPageHeader replacementHeader =
			header(VectorStorageKind::VSK_RAW, PageId(101), Generation(1), LogSequenceNumber(3));
	const VectorPage replacement = page(replacementHeader, std::byte{0x22});
	static_cast<void>(store.replace(replacement, SegmentId(71), LogSequenceNumber(2), LogSequenceNumber(3)));
	EXPECT_EQ(store.read(oldSnapshot, originalHeader.page_id).read(0), std::vector<std::byte>(16U, std::byte{0x11}));
	expectFormatError(
			[&] {
				static_cast<void>(
						store.replace(replacement, SegmentId(71), LogSequenceNumber(2), LogSequenceNumber(4)));
			},
			FormatError::FERR_INVALID_STATE);

	const VectorPageHeader destinationHeader =
			header(VectorStorageKind::VSK_RAW, PageId(201), Generation(2), LogSequenceNumber(4));
	const VectorPage destination = page(destinationHeader, std::byte{0x33});
	ASSERT_TRUE(store.allocate(allocationRequest(destinationHeader, SegmentId(72)), destination, PhysicalSlotId(41),
							   PhysicalSlotId(41), LogSequenceNumber(4))
						.has_value());
	MetadataSnapshotPin beforeEvacuation = metadata->openSnapshot();
	const VectorPage replacementCurrent = store.read(beforeEvacuation, replacementHeader.page_id);
	const SharedPagePoolAllocation evacuation = store.evacuate(replacementCurrent, SegmentId(72), LogSequenceNumber(5));
	EXPECT_EQ(evacuation.location.segment_id, SegmentId(72));
	EXPECT_EQ(store.read(beforeEvacuation, replacementHeader.page_id).read(0),
			  std::vector<std::byte>(16U, std::byte{0x22}));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_EQ(store.read(current, replacementHeader.page_id).header().generation, Generation(2));
	EXPECT_EQ(store.read(current, replacementHeader.page_id).read(0), std::vector<std::byte>(16U, std::byte{0x22}));
	oldSnapshot.release();
	beforeEvacuation.release();
}

TEST_F(RelocatableVectorPageStoreTest, RejectsMismatchedRequestsAndIncompatibleEvacuationTargets) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorPageStore store(io, *metadata);
	const VectorPageHeader rawHeader =
			header(VectorStorageKind::VSK_RAW, PageId(101), Generation(1), LogSequenceNumber(2));
	const VectorPage rawPage = page(rawHeader, std::byte{0x11});
	SharedPagePoolAllocationRequest wrongAllocation = allocationRequest(rawHeader, SegmentId(71));
	wrongAllocation.pool_kind = SegmentKind::SGK_VECTOR_PQ_POOL;
	expectFormatError(
			[&] {
				static_cast<void>(store.allocate(wrongAllocation, rawPage, PhysicalSlotId(40), PhysicalSlotId(40),
												 LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
	ASSERT_TRUE(store.allocate(allocationRequest(rawHeader, SegmentId(71)), rawPage, PhysicalSlotId(40),
							   PhysicalSlotId(40), LogSequenceNumber(2))
						.has_value());
	const VectorPageHeader appendHeader =
			header(VectorStorageKind::VSK_RAW, PageId(102), Generation(1), LogSequenceNumber(3));
	SharedPagePoolAppendRequest wrongAppend = appendRequest(appendHeader, SegmentId(71));
	wrongAppend.page_owner_id = 99;
	expectFormatError(
			[&] {
				static_cast<void>(store.append(wrongAppend, page(appendHeader, std::byte{0x22}), LogSequenceNumber(3)));
			},
			FormatError::FERR_INVALID_STATE);

	const VectorPageHeader pqHeader =
			header(VectorStorageKind::VSK_PQ, PageId(201), Generation(1), LogSequenceNumber(3));
	ASSERT_TRUE(store.allocate(allocationRequest(pqHeader, SegmentId(72)), page(pqHeader, std::byte{0x33}),
							   PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3))
						.has_value());
	expectFormatError([&] { static_cast<void>(store.evacuate(rawPage, {}, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.evacuate(rawPage, SegmentId(72), {})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.evacuate(rawPage, SegmentId(72), LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_STATE);
	const VectorPageHeader pqSourceHeader =
			header(VectorStorageKind::VSK_PQ, PageId(301), Generation(1), LogSequenceNumber(4));
	const VectorPage pqSource = page(pqSourceHeader, std::byte{0x44});
	ASSERT_TRUE(store.allocate(allocationRequest(pqSourceHeader, SegmentId(73)), pqSource, PhysicalSlotId(42),
							   PhysicalSlotId(42), LogSequenceNumber(4))
						.has_value());
	const VectorPageHeader pqDestinationHeader =
			header(VectorStorageKind::VSK_PQ, PageId(401), Generation(2), LogSequenceNumber(5));
	ASSERT_TRUE(store.allocate(allocationRequest(pqDestinationHeader, SegmentId(74)),
							   page(pqDestinationHeader, std::byte{0x55}), PhysicalSlotId(43), PhysicalSlotId(43),
							   LogSequenceNumber(5))
						.has_value());
	EXPECT_EQ(store.evacuate(pqSource, SegmentId(74), LogSequenceNumber(6)).location.segment_id, SegmentId(74));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	VectorPageHeader expectedRaw = rawHeader;
	expectedRaw.live_count = 1;
	EXPECT_EQ(store.read(snapshot, rawHeader.page_id).header(), expectedRaw);
}

TEST_F(RelocatableVectorPageStoreTest, RecoversVectorPageEvacuationAtMetadataPublicationBoundaries) {
	const auto initialize = [&] {
		auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
		VectorPageStore store(io, *metadata);
		const VectorPageHeader sourceHeader =
				header(VectorStorageKind::VSK_RAW, PageId(101), Generation(1), LogSequenceNumber(2));
		const VectorPage source = page(sourceHeader, std::byte{0x11});
		EXPECT_TRUE(store.allocate(allocationRequest(sourceHeader, SegmentId(71)), source, PhysicalSlotId(40),
								   PhysicalSlotId(40), LogSequenceNumber(2))
							.has_value());
		const VectorPageHeader destinationHeader =
				header(VectorStorageKind::VSK_RAW, PageId(201), Generation(2), LogSequenceNumber(3));
		EXPECT_TRUE(store.allocate(allocationRequest(destinationHeader, SegmentId(72)),
								   page(destinationHeader, std::byte{0x22}), PhysicalSlotId(41), PhysicalSlotId(41),
								   LogSequenceNumber(3))
							.has_value());
		return std::pair{std::move(metadata), sourceHeader};
	};

	{
		auto [metadata, sourceHeader] = initialize();
		VectorPageStore store(io, *metadata);
		MetadataPublishOptions interrupted;
		interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(store.evacuate(page(sourceHeader, std::byte{0x11}), SegmentId(72),
													  LogSequenceNumber(4), interrupted)),
					 BootstrapPublishInterrupted);
		metadata.reset();
		auto reopened = RelocatableMetadataStore::open(io);
		VectorPageStore reopenedStore(io, *reopened);
		MetadataSnapshotPin beforeRetry = reopened->openSnapshot();
		EXPECT_EQ(reopened->findPageLocation(beforeRetry, sourceHeader.page_id).segment_id, SegmentId(71));
		EXPECT_EQ(reopenedStore.read(beforeRetry, sourceHeader.page_id).header(), [&] {
			VectorPageHeader expected = sourceHeader;
			expected.live_count = 1;
			return expected;
		}());
		beforeRetry.release();
		MetadataSnapshotPin sourceSnapshot = reopened->openSnapshot();
		const VectorPage source = reopenedStore.read(sourceSnapshot, sourceHeader.page_id);
		sourceSnapshot.release();
		EXPECT_EQ(reopenedStore.evacuate(source, SegmentId(72), LogSequenceNumber(4)).location.segment_id,
				  SegmentId(72));
	}

	std::error_code error;
	io.reset();
	file->close();
	std::filesystem::remove(path, error);
	file->open(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(file->is_open());
	io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);

	{
		auto [metadata, sourceHeader] = initialize();
		VectorPageStore store(io, *metadata);
		MetadataPublishOptions interrupted;
		interrupted.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(store.evacuate(page(sourceHeader, std::byte{0x11}), SegmentId(72),
													  LogSequenceNumber(4), interrupted)),
					 BootstrapPublishInterrupted);
		metadata.reset();
		auto reopened = RelocatableMetadataStore::open(io);
		VectorPageStore reopenedStore(io, *reopened);
		const MetadataSnapshotPin recovered = reopened->openSnapshot();
		EXPECT_EQ(reopened->findPageLocation(recovered, sourceHeader.page_id).segment_id, SegmentId(72));
		VectorPageHeader expected = sourceHeader;
		expected.generation = Generation(2);
		expected.committed_lsn = LogSequenceNumber(4);
		expected.live_count = 1;
		EXPECT_EQ(reopenedStore.read(recovered, sourceHeader.page_id).header(), expected);
		EXPECT_EQ(reopenedStore.read(recovered, sourceHeader.page_id).read(0),
				  std::vector<std::byte>(16U, std::byte{0x11}));
	}
}
