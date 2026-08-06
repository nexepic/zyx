/**
 * @file test_VectorAdjacencyPageStore.cpp
 * @brief Derived adjacency-page staging and lookup tests.
 */

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"

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
