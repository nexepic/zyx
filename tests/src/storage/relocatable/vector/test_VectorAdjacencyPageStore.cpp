/**
 * @file test_VectorAdjacencyPageStore.cpp
 * @brief Derived adjacency-page staging and lookup tests.
 */

#include <array>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"

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

	VectorAdjacencyMaterializationOptions options() { return {PhysicalSlotId(40), PhysicalSlotId(45), 128, 16}; }

	std::vector<uint64_t> neighbors(uint64_t first, size_t count) {
		std::vector<uint64_t> result;
		result.reserve(count);
		for (size_t ordinal = 0; ordinal < count; ++ordinal) {
			result.push_back(first + ordinal);
		}
		return result;
	}

	std::vector<VectorAdjacencyMaterializationRecord> records(size_t count, size_t neighborCount = 0U) {
		std::vector<VectorAdjacencyMaterializationRecord> result;
		result.reserve(count);
		for (size_t ordinal = 0; ordinal < count; ++ordinal) {
			result.push_back({static_cast<uint64_t>(ordinal + 1U), neighbors(10000U, neighborCount)});
		}
		return result;
	}

	void overwritePagePayload(const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata, PageId pageId,
							  std::span<const std::byte> bytes) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		const PageLocation location = metadata.findPageLocation(snapshot, pageId);
		const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
		const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(snapshot.superblock().data_region_offset,
																			pool.physical_slot) +
								static_cast<uint64_t>(location.page_index) * RSS_PAGE_SIZE +
								RSS_PAGE_OWNER_HEADER_BYTES;
		snapshot.release();
		io->writeAt(offset, bytes.data(), bytes.size());
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
		const std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> bytes = PageOwnerHeaderCodec::encode(owner);
		io->writeAt(offset, bytes.data(), bytes.size());
	}

} // namespace

class RelocatableVectorAdjacencyPageStoreTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_adjacency_page_store_" + to_string(uuid));
		walPath = path.string() + ".foreign";
		file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		walFile = std::make_shared<std::fstream>(walPath,
												 std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file->is_open());
		ASSERT_TRUE(walFile->is_open());
		io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		walIo = std::make_shared<StorageIO>(walFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io.reset();
		walIo.reset();
		if (file && file->is_open()) {
			file->close();
		}
		if (walFile && walFile->is_open()) {
			walFile->close();
		}
		std::error_code error;
		std::filesystem::remove(path, error);
		std::filesystem::remove(walPath, error);
	}
};

TEST_F(RelocatableVectorAdjacencyPageStoreTest, StagesPackedPagesForManifestPublishedMapping) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorAdjacencyPageStore store(io, *metadata);
	const auto source = records(11, 400);
	const VectorAdjacencyMaterializationResult result = store.materialize(17, source, options(), LogSequenceNumber(2));
	EXPECT_EQ(result.record_count, source.size());
	ASSERT_GT(result.pages.size(), 1U);
	ASSERT_EQ(result.mappings.size(), source.size());
	EXPECT_LT(result.pages.front().page_id, result.pages.back().page_id);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	for (size_t ordinal = 0; ordinal < source.size(); ++ordinal) {
		EXPECT_EQ(store.readRecord(snapshot, result.mappings[ordinal].page_id, source[ordinal].vector_id),
				  (VectorAdjacencyRecord{source[ordinal].vector_id, source[ordinal].neighbors}));
	}
	const std::optional<DerivedVectorAdjacencyPage> inspected =
			store.inspectDerivedPage(snapshot, result.pages.front().page_id);
	ASSERT_TRUE(inspected.has_value());
	EXPECT_EQ(inspected->index_id, 17U);
	EXPECT_EQ(inspected->owner_lsn, LogSequenceNumber(2U));
	EXPECT_EQ(store.readPage(snapshot, result.pages.front().page_id).ownerId(),
			  VectorAdjacencyPageStore::derivedOwnerId(17));
	snapshot.release();
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	VectorAdjacencyPageStore reopenedStore(io, *reopened);
	const MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedStore.readRecord(reopenedSnapshot, result.mappings.back().page_id, source.back().vector_id),
			  (VectorAdjacencyRecord{source.back().vector_id, source.back().neighbors}));
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest,
       NativeDiskFullDuringMaterializationRetainsNoPublishedAdjacencyPageAndCanRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
		VectorAdjacencyPageStore store(nativeIo, *metadata);
		const std::vector<VectorAdjacencyMaterializationRecord> source = records(2U);
		bool diskFullObserved = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting failFirstCandidate(
					[&diskFullObserved](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (!diskFullObserved && byteCount == RSS_PAGE_SIZE) {
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
			EXPECT_THROW(static_cast<void>(store.materialize(17U, source, options(), LogSequenceNumber(2U))),
					 std::system_error);
		}
		EXPECT_TRUE(diskFullObserved);
		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_TRUE(metadata->findCurrentVectorAdjacencyPagePools(current).empty());
		EXPECT_FALSE(metadata->findPageLocationOptional(current, PageId(1U)).has_value());
		current.release();
		metadata.reset();

		auto recovered = RelocatableMetadataStore::open(nativeIo);
		VectorAdjacencyPageStore recoveredStore(nativeIo, *recovered);
		MetadataSnapshotPin recoveredSnapshot = recovered->openSnapshot();
		EXPECT_TRUE(recovered->findCurrentVectorAdjacencyPagePools(recoveredSnapshot).empty());
		EXPECT_FALSE(recovered->findPageLocationOptional(recoveredSnapshot, PageId(1U)).has_value());
		recoveredSnapshot.release();

		const VectorAdjacencyMaterializationResult retry =
				recoveredStore.materialize(17U, source, options(), LogSequenceNumber(2U));
		ASSERT_EQ(retry.pages.size(), 1U);
		MetadataSnapshotPin published = recovered->openSnapshot();
		EXPECT_EQ(recoveredStore.readRecord(published, retry.pages.front().page_id, source.front().vector_id),
				  (VectorAdjacencyRecord{source.front().vector_id, source.front().neighbors}));
		published.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest,
       NativePrePublicationSyncFailureRetainsNoPublishedAdjacencyPageAndCanRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
		VectorAdjacencyPageStore store(nativeIo, *metadata);
		const std::vector<VectorAdjacencyMaterializationRecord> source = records(2U);
		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failFirstDataFence([&syncCalls](file_handle_t fd) {
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
			EXPECT_THROW(static_cast<void>(store.materialize(17U, source, options(), LogSequenceNumber(2U))),
					 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 1U);
		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_TRUE(metadata->findCurrentVectorAdjacencyPagePools(current).empty());
		EXPECT_FALSE(metadata->findPageLocationOptional(current, PageId(1U)).has_value());
		current.release();
		metadata.reset();

		auto recovered = RelocatableMetadataStore::open(nativeIo);
		VectorAdjacencyPageStore recoveredStore(nativeIo, *recovered);
		MetadataSnapshotPin recoveredSnapshot = recovered->openSnapshot();
		EXPECT_TRUE(recovered->findCurrentVectorAdjacencyPagePools(recoveredSnapshot).empty());
		EXPECT_FALSE(recovered->findPageLocationOptional(recoveredSnapshot, PageId(1U)).has_value());
		recoveredSnapshot.release();

		const VectorAdjacencyMaterializationResult retry =
				recoveredStore.materialize(17U, source, options(), LogSequenceNumber(2U));
		ASSERT_EQ(retry.pages.size(), 1U);
		MetadataSnapshotPin published = recovered->openSnapshot();
		EXPECT_EQ(recoveredStore.readRecord(published, retry.pages.front().page_id, source.back().vector_id),
				  (VectorAdjacencyRecord{source.back().vector_id, source.back().neighbors}));
		published.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest,
       NativeFinalPublicationSyncFailureReportsUncertainDurabilityAndReopensTheCompleteAdjacencyPage) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
		VectorAdjacencyPageStore store(nativeIo, *metadata);
		const std::vector<VectorAdjacencyMaterializationRecord> source = records(2U);
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
			EXPECT_THROW(static_cast<void>(store.materialize(17U, source, options(), LogSequenceNumber(2U))),
					 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);

		// The inactive Superblock is complete before its final durability fence.
		// The operation is uncertain to its caller, while every reader must select
		// the complete new Page Directory route and its immutable page.
		MetadataSnapshotPin current = metadata->openSnapshot();
		ASSERT_EQ(metadata->findCurrentVectorAdjacencyPagePools(current).size(), 1U);
		ASSERT_TRUE(metadata->findPageLocationOptional(current, PageId(1U)).has_value());
		EXPECT_EQ(store.readRecord(current, PageId(1U), source.front().vector_id),
				  (VectorAdjacencyRecord{source.front().vector_id, source.front().neighbors}));
		current.release();
		metadata.reset();

		auto recovered = RelocatableMetadataStore::open(nativeIo);
		VectorAdjacencyPageStore recoveredStore(nativeIo, *recovered);
		MetadataSnapshotPin recoveredSnapshot = recovered->openSnapshot();
		ASSERT_EQ(recovered->findCurrentVectorAdjacencyPagePools(recoveredSnapshot).size(), 1U);
		ASSERT_TRUE(recovered->findPageLocationOptional(recoveredSnapshot, PageId(1U)).has_value());
		EXPECT_EQ(recoveredStore.readRecord(recoveredSnapshot, PageId(1U), source.back().vector_id),
				  (VectorAdjacencyRecord{source.back().vector_id, source.back().neighbors}));
		recoveredSnapshot.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest, RejectsUnboundedUnorderedAndInvalidRecordsBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorAdjacencyPageStore store(io, *metadata);
	VectorAdjacencyMaterializationOptions invalid = options();
	invalid.maximum_records = 0;
	expectFormatError([&] { static_cast<void>(store.materialize(17, records(1), invalid, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	const std::vector<VectorAdjacencyMaterializationRecord> unordered{{2, {}}, {1, {}}};
	expectFormatError([&] { static_cast<void>(store.materialize(17, unordered, options(), LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	const std::vector<VectorAdjacencyMaterializationRecord> selfNeighbor{{1, {1}}};
	expectFormatError([&] { static_cast<void>(store.materialize(17, selfNeighbor, options(), LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findPageLocationOptional(snapshot, PageId(1)).has_value());
	expectFormatError([&] { static_cast<void>(VectorAdjacencyPageStore::derivedOwnerId(0)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						VectorAdjacencyPageStore::derivedOwnerId(RSS_DERIVED_ADJACENCY_OWNER_MASK | UINT64_C(17)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::vector<VectorAdjacencyMaterializationRecord> reservedIndex{{1, {}}};
	expectFormatError(
			[&] {
				static_cast<void>(store.materialize(RSS_DERIVED_ADJACENCY_OWNER_MASK | UINT64_C(17), reservedIndex,
													options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest, EnforcesPageCapacityAndRecordLookupIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorAdjacencyPageStore store(io, *metadata);
	VectorAdjacencyMaterializationOptions onePage = options();
	onePage.maximum_pages = 1;
	expectFormatError(
			[&] { static_cast<void>(store.materialize(17, records(11, 400), onePage, LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_STATE);
	const auto source = records(2);
	const VectorAdjacencyMaterializationResult result = store.materialize(17, source, options(), LogSequenceNumber(2));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.readRecord(snapshot, result.mappings.front().page_id, 9)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store.readRecord(snapshot, result.mappings.front().page_id, 0)); },
					  FormatError::FERR_INVALID_HEADER);
	snapshot.release();
	const std::vector<VectorAdjacencyMaterializationRecord> startsAtTwo{{2, {}}, {3, {}}};
	const VectorAdjacencyMaterializationResult later =
			store.materialize(17, startsAtTwo, options(), LogSequenceNumber(3));
	const MetadataSnapshotPin laterSnapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.readRecord(laterSnapshot, later.mappings.front().page_id, 1)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest, RejectsARecordThatFitsTheAdjacencyCodecButNotTheSlottedPage) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorAdjacencyPageStore store(io, *metadata);
	const std::vector<VectorAdjacencyMaterializationRecord> oversized{
			{1, neighbors(10000U, RSS_VECTOR_ADJACENCY_MAX_NEIGHBORS)}};
	expectFormatError([&] { static_cast<void>(store.materialize(17, oversized, options(), LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest, DetectsUnorderedRecordsAfterTheRequestedRecord) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorAdjacencyPageStore store(io, *metadata);
	const std::vector<VectorAdjacencyMaterializationRecord> source{{1, {}}, {2, {}}};
	const VectorAdjacencyMaterializationResult result = store.materialize(17, source, options(), LogSequenceNumber(2));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	SlottedPage corrupt = store.readPage(snapshot, result.mappings.front().page_id);
	const std::vector<SlottedSlotReference> slots = corrupt.liveSlots();
	ASSERT_EQ(slots.size(), 2U);
	corrupt.overwrite(slots.back(), VectorAdjacencyCodec::encode({1, {}}));
	snapshot.release();
	overwritePagePayload(io, *metadata, result.mappings.front().page_id, corrupt.serialize());
	const MetadataSnapshotPin corruptedSnapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(store.readRecord(corruptedSnapshot, result.mappings.front().page_id, 1)); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest, RejectsMalformedPreparedPoolAndRetirementBatchesBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorAdjacencyPageStore store(io, *metadata);
	const std::array<VectorAdjacencyMaterializationRecord, 1U> source{{{1U, {}}}};
	std::array<VectorAdjacencyMaterializationPoolBatch, 0U> noPools{};
	expectFormatError(
			[&] {
				static_cast<void>(store.prepareNewPoolPages(17U, source, options(), noPools, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);
	std::array<VectorAdjacencyMaterializationPoolBatch, 1U> malformedPool{};
	expectFormatError(
			[&] {
				static_cast<void>(
						store.prepareNewPoolPages(17U, source, options(), malformedPool, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);

	const std::array<ExistingPoolPageRetirementBatch, 0U> noRetirements{};
	expectFormatError([&] { store.prepareDerivedRetirements(noRetirements, LogSequenceNumber(2U)); },
					  FormatError::FERR_INVALID_HEADER);
	const std::array<ExistingPoolPageRetirementBatch, 1U> malformedRetirementPool{};
	expectFormatError(
			[&] { store.prepareDerivedRetirements(malformedRetirementPool, LogSequenceNumber(2U)); },
			FormatError::FERR_INVALID_STATE);

	const VectorAdjacencyMaterializationResult materialized =
			store.materialize(17U, source, options(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PageLocation location = metadata->findPageLocation(snapshot, materialized.pages.front().page_id);
	const CatalogEntry pool = metadata->findCatalog(snapshot, location.segment_id);
	const PageRetirement retirement{location, VectorAdjacencyPageStore::derivedOwnerId(17U),
									 PageOwnerKind::POK_VECTOR_ADJACENCY, LogSequenceNumber(2U)};
	snapshot.release();
	const ExistingPoolPageRetirementBatch validPool{pool, {retirement}};
	expectFormatError([&] { store.prepareDerivedRetirements(std::span(&validPool, 1U), {}); },
					  FormatError::FERR_INVALID_HEADER);
	const ExistingPoolPageRetirementBatch duplicatePage{pool, {retirement, retirement}};
	expectFormatError(
			[&] { store.prepareDerivedRetirements(std::span(&duplicatePage, 1U), LogSequenceNumber(3U)); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest, CancellationPreventsPreparedMaterializationAndRetirement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorAdjacencyPageStore store(io, *metadata);
	const std::array<VectorAdjacencyMaterializationRecord, 1U> source{{{1U, {}}}};
	std::array<VectorAdjacencyMaterializationPoolBatch, 0U> noPools{};
	ThrowOnCancellationCheck cancellation;
	EXPECT_THROW(static_cast<void>(
				 store.prepareNewPoolPages(17U, source, options(), noPools, LogSequenceNumber(2U), &cancellation)),
			 std::runtime_error);

	const VectorAdjacencyMaterializationResult materialized =
			store.materialize(17U, source, options(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PageLocation location = metadata->findPageLocation(snapshot, materialized.pages.front().page_id);
	const CatalogEntry pool = metadata->findCatalog(snapshot, location.segment_id);
	EXPECT_EQ(store.readRecord(snapshot, materialized.pages.front().page_id, 1U).vector_id, 1U);
	snapshot.release();
	const ExistingPoolPageRetirementBatch retirementPool{
			pool,
			{{location, VectorAdjacencyPageStore::derivedOwnerId(17U), PageOwnerKind::POK_VECTOR_ADJACENCY,
			  LogSequenceNumber(2U)}}};
	EXPECT_THROW(store.prepareDerivedRetirements(std::span(&retirementPool, 1U), LogSequenceNumber(3U), &cancellation),
			 std::runtime_error);

	const MetadataSnapshotPin afterCancellation = metadata->openSnapshot();
	EXPECT_EQ(store.readRecord(afterCancellation, materialized.pages.front().page_id, 1U).vector_id, 1U);
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest, RetiresAnUnreferencedDerivedAdjacencyPageAtOneMetadataBoundary) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorAdjacencyPageStore store(io, *metadata);
	const VectorAdjacencyMaterializationResult materialized =
			store.materialize(17U, records(1U), options(), LogSequenceNumber(2U));

	MetadataSnapshotPin plannedSnapshot = metadata->openSnapshot();
	const std::optional<DerivedVectorAdjacencyPage> observed =
			store.inspectDerivedPage(plannedSnapshot, materialized.pages.front().page_id);
	ASSERT_TRUE(observed.has_value());
	const Generation plannedGeneration = plannedSnapshot.superblock().generation;
	plannedSnapshot.release();
	const std::array<DerivedVectorAdjacencyPage, 1U> plan{*observed};

	const Superblock published = store.retireDerivedPages(plannedGeneration, plan, LogSequenceNumber(3U));
	EXPECT_EQ(published.generation, Generation(plannedGeneration.value() + 1U));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findPageLocationOptional(current, materialized.pages.front().page_id).has_value());
	current.release();

	store.reclaimRetiredFrames(observed->location.segment_id);
	EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()));
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest,
       NativeRetirementDiskFullRetainsThePublishedAdjacencyPageAndCanRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
		VectorAdjacencyPageStore store(nativeIo, *metadata);
		const VectorAdjacencyMaterializationResult materialized =
				store.materialize(17U, records(1U), options(), LogSequenceNumber(2U));
		MetadataSnapshotPin plannedSnapshot = metadata->openSnapshot();
		const std::optional<DerivedVectorAdjacencyPage> observed =
				store.inspectDerivedPage(plannedSnapshot, materialized.pages.front().page_id);
		ASSERT_TRUE(observed.has_value());
		const Generation plannedGeneration = plannedSnapshot.superblock().generation;
		plannedSnapshot.release();
		const std::array<DerivedVectorAdjacencyPage, 1U> plan{*observed};

		bool diskFullObserved = false;
		{
			StorageIO::ScopedNativeWriteHookForTesting failFirstCandidate(
					[&diskFullObserved](file_handle_t fd, const void *bytes, size_t byteCount, pwrite_off_t offset) {
						if (!diskFullObserved && byteCount == RSS_PAGE_SIZE) {
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
			EXPECT_THROW(static_cast<void>(store.retireDerivedPages(plannedGeneration, plan, LogSequenceNumber(3U))),
					 std::system_error);
		}
		EXPECT_TRUE(diskFullObserved);
		MetadataSnapshotPin current = metadata->openSnapshot();
		ASSERT_TRUE(metadata->findPageLocationOptional(current, materialized.pages.front().page_id).has_value());
		EXPECT_EQ(store.readRecord(current, materialized.pages.front().page_id, 1U).vector_id, 1U);
		current.release();
		metadata.reset();

		auto recovered = RelocatableMetadataStore::open(nativeIo);
		VectorAdjacencyPageStore recoveredStore(nativeIo, *recovered);
		MetadataSnapshotPin recoveredSnapshot = recovered->openSnapshot();
		ASSERT_TRUE(recovered->findPageLocationOptional(recoveredSnapshot, materialized.pages.front().page_id).has_value());
		EXPECT_EQ(recoveredStore.readRecord(recoveredSnapshot, materialized.pages.front().page_id, 1U).vector_id, 1U);
		recoveredSnapshot.release();

		const Superblock retried = recoveredStore.retireDerivedPages(plannedGeneration, plan, LogSequenceNumber(3U));
		EXPECT_EQ(retried.generation, Generation(plannedGeneration.value() + 1U));
		MetadataSnapshotPin retired = recovered->openSnapshot();
		EXPECT_FALSE(recovered->findPageLocationOptional(retired, materialized.pages.front().page_id).has_value());
		retired.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest,
       NativeRetirementPrePublicationSyncFailureRetainsThePublishedAdjacencyPageAndCanRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
		VectorAdjacencyPageStore store(nativeIo, *metadata);
		const VectorAdjacencyMaterializationResult materialized =
				store.materialize(17U, records(1U), options(), LogSequenceNumber(2U));
		MetadataSnapshotPin plannedSnapshot = metadata->openSnapshot();
		const std::optional<DerivedVectorAdjacencyPage> observed =
				store.inspectDerivedPage(plannedSnapshot, materialized.pages.front().page_id);
		ASSERT_TRUE(observed.has_value());
		const Generation plannedGeneration = plannedSnapshot.superblock().generation;
		plannedSnapshot.release();
		const std::array<DerivedVectorAdjacencyPage, 1U> plan{*observed};

		std::atomic<uint32_t> syncCalls{0U};
		{
			StorageIO::ScopedNativeSyncHookForTesting failFirstDataFence([&syncCalls](file_handle_t fd) {
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
			EXPECT_THROW(static_cast<void>(store.retireDerivedPages(plannedGeneration, plan, LogSequenceNumber(3U))),
					 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 1U);
		MetadataSnapshotPin current = metadata->openSnapshot();
		ASSERT_TRUE(metadata->findPageLocationOptional(current, materialized.pages.front().page_id).has_value());
		EXPECT_EQ(store.readRecord(current, materialized.pages.front().page_id, 1U).vector_id, 1U);
		current.release();
		metadata.reset();

		auto recovered = RelocatableMetadataStore::open(nativeIo);
		VectorAdjacencyPageStore recoveredStore(nativeIo, *recovered);
		MetadataSnapshotPin recoveredSnapshot = recovered->openSnapshot();
		ASSERT_TRUE(recovered->findPageLocationOptional(recoveredSnapshot, materialized.pages.front().page_id).has_value());
		EXPECT_EQ(recoveredStore.readRecord(recoveredSnapshot, materialized.pages.front().page_id, 1U).vector_id, 1U);
		recoveredSnapshot.release();

		const Superblock retried = recoveredStore.retireDerivedPages(plannedGeneration, plan, LogSequenceNumber(3U));
		EXPECT_EQ(retried.generation, Generation(plannedGeneration.value() + 1U));
		MetadataSnapshotPin retired = recovered->openSnapshot();
		EXPECT_FALSE(recovered->findPageLocationOptional(retired, materialized.pages.front().page_id).has_value());
		retired.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest,
       NativeRetirementFinalPublicationSyncFailureReportsUncertainDurabilityAndReopensTheCompleteRetirement) {
	const file_handle_t nativeHandle = portable_open_rw(path.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	{
		auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
		auto metadata = RelocatableMetadataStore::initializeNew(nativeIo, initialSuperblock());
		VectorAdjacencyPageStore store(nativeIo, *metadata);
		const VectorAdjacencyMaterializationResult materialized =
				store.materialize(17U, records(1U), options(), LogSequenceNumber(2U));
		MetadataSnapshotPin plannedSnapshot = metadata->openSnapshot();
		const std::optional<DerivedVectorAdjacencyPage> observed =
				store.inspectDerivedPage(plannedSnapshot, materialized.pages.front().page_id);
		ASSERT_TRUE(observed.has_value());
		const Generation plannedGeneration = plannedSnapshot.superblock().generation;
		plannedSnapshot.release();
		const std::array<DerivedVectorAdjacencyPage, 1U> plan{*observed};

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
			EXPECT_THROW(static_cast<void>(store.retireDerivedPages(plannedGeneration, plan, LogSequenceNumber(3U))),
					 std::runtime_error);
		}
		EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);
		MetadataSnapshotPin current = metadata->openSnapshot();
		EXPECT_FALSE(metadata->findPageLocationOptional(current, materialized.pages.front().page_id).has_value());
		current.release();
		metadata.reset();

		auto recovered = RelocatableMetadataStore::open(nativeIo);
		MetadataSnapshotPin recoveredSnapshot = recovered->openSnapshot();
		EXPECT_FALSE(recovered->findPageLocationOptional(recoveredSnapshot, materialized.pages.front().page_id).has_value());
		recoveredSnapshot.release();
	}
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableVectorAdjacencyPageStoreTest, RejectsCrossNamespaceReadsAndRetainsUnfamiliarValidOwners) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	VectorAdjacencyPageStore store(io, *metadata);
	const VectorAdjacencyMaterializationResult materialized =
			store.materialize(17U, records(1U), options(), LogSequenceNumber(2U));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const PageLocation location = metadata->findPageLocation(snapshot, materialized.pages.front().page_id);
	snapshot.release();

	const PageOwnerHeader poolOwner{location.page_id, 17U, PageOwnerKind::POK_VECTOR_ADJACENCY, 0U,
									  RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, location.generation, LogSequenceNumber(2U)};
	overwritePageOwner(io, *metadata, location.page_id, poolOwner);
	MetadataSnapshotPin poolOwnerSnapshot = metadata->openSnapshot();
	EXPECT_FALSE(store.inspectDerivedPage(poolOwnerSnapshot, location.page_id).has_value());
	expectFormatError([&] { static_cast<void>(store.readPage(poolOwnerSnapshot, location.page_id)); },
					  FormatError::FERR_INVALID_STATE);
	poolOwnerSnapshot.release();

	const PageOwnerHeader foreignOwner{location.page_id, 99U, PageOwnerKind::POK_VECTOR_ADJACENCY, 0U,
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, location.generation, LogSequenceNumber(2U)};
	overwritePageOwner(io, *metadata, location.page_id, foreignOwner);
	MetadataSnapshotPin foreignOwnerSnapshot = metadata->openSnapshot();
	EXPECT_FALSE(store.inspectDerivedPage(foreignOwnerSnapshot, location.page_id).has_value());
	expectFormatError([&] { static_cast<void>(store.readPage(foreignOwnerSnapshot, location.page_id)); },
					  FormatError::FERR_INVALID_STATE);
	foreignOwnerSnapshot.release();

	PageOwnerHeader malformedOwner = foreignOwner;
	malformedOwner.kind = PageOwnerKind::POK_VECTOR_RAW;
	overwritePageOwner(io, *metadata, location.page_id, malformedOwner);
	MetadataSnapshotPin malformedSnapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.inspectDerivedPage(malformedSnapshot, location.page_id)); },
					  FormatError::FERR_INVALID_STATE);
	malformedSnapshot.release();

	const PageOwnerHeader futureOwner{location.page_id, VectorAdjacencyPageStore::derivedOwnerId(17U),
									PageOwnerKind::POK_VECTOR_ADJACENCY, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									location.generation, LogSequenceNumber(3U)};
	overwritePageOwner(io, *metadata, location.page_id, futureOwner);
	MetadataSnapshotPin futureSnapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(store.readPage(futureSnapshot, location.page_id)); },
					  FormatError::FERR_INVALID_STATE);
}
