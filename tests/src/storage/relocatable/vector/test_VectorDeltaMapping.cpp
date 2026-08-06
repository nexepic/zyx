/**
 * @file test_VectorDeltaMapping.cpp
 * @brief Tests for the durable Vector base/delta overlay mapping.
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
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"

using namespace graph::storage;

namespace {

	constexpr uint64_t VECTOR_INDEX_ID = 97U;

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
		result.database_id = DatabaseId(0xB3A450672819CDEFULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	VectorDeltaMappingBuildOptions options(uint64_t mappingIndexId, size_t maximumEntries = 16U) {
		VectorDeltaMappingBuildOptions result;
		result.tree = {PhysicalSlotId(40U), PhysicalSlotId(42U), mappingIndexId, maximumEntries, 4U};
		result.maximum_entries = maximumEntries;
		return result;
	}

	std::vector<VectorDeltaEntry> entries() {
		return {{1U, 101U, VectorDeltaOperation::VDO_ERASE},
				{3U, 103U, VectorDeltaOperation::VDO_INSERT},
				{8U, 108U, VectorDeltaOperation::VDO_ERASE}};
	}

} // namespace

class RelocatableVectorDeltaMappingTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<StorageIO> io_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path_ = std::filesystem::temp_directory_path() / ("relocatable_vector_delta_mapping_" + to_string(uuid));
		file_ = std::make_shared<std::fstream>(path_,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io_.reset();
		if (file_ != nullptr && file_->is_open()) {
			file_->close();
		}
		std::error_code error;
		std::filesystem::remove(path_, error);
	}
};

TEST_F(RelocatableVectorDeltaMappingTest, BuildsScansAndPinsTypedOverlayEntries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	MetadataSnapshotPin allocationSnapshot = metadata->openSnapshot();
	const uint64_t mappingIndexId =
			VectorDeltaMapping::selectMappingIndexId(allocationSnapshot, *metadata, VECTOR_INDEX_ID);
	allocationSnapshot.release();

	const std::vector<VectorDeltaEntry> expected = entries();
	const BulkBTreeBuildResult built = VectorDeltaMapping::build(trees, *metadata, mappingIndexId, expected,
																 options(mappingIndexId), LogSequenceNumber(2U));
	EXPECT_EQ(built.entry_count, expected.size());

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	EXPECT_EQ(VectorDeltaMapping::findOperation(oldSnapshot, trees, *metadata, mappingIndexId, 1U),
			  std::optional(VectorDeltaOperation::VDO_ERASE));
	EXPECT_EQ(VectorDeltaMapping::findOperation(oldSnapshot, trees, *metadata, mappingIndexId, 3U),
			  std::optional(VectorDeltaOperation::VDO_INSERT));
	EXPECT_FALSE(VectorDeltaMapping::findOperation(oldSnapshot, trees, *metadata, mappingIndexId, 2U).has_value());
	EXPECT_EQ(VectorDeltaMapping::scan(oldSnapshot, trees, *metadata, mappingIndexId, expected.size()), expected);

	static_cast<void>(trees.erase(mappingIndexId, VectorDeltaMapping::key(1U, VectorDeltaOperation::VDO_ERASE),
								  LogSequenceNumber(3U)));
	static_cast<void>(trees.upsertRootLeaf(mappingIndexId,
										   VectorDeltaMapping::key(1U, VectorDeltaOperation::VDO_INSERT), 101U,
										   LogSequenceNumber(4U)));
	MetadataSnapshotPin currentSnapshot = metadata->openSnapshot();
	EXPECT_EQ(VectorDeltaMapping::findOperation(oldSnapshot, trees, *metadata, mappingIndexId, 1U),
			  std::optional(VectorDeltaOperation::VDO_ERASE));
	EXPECT_EQ(VectorDeltaMapping::findOperation(currentSnapshot, trees, *metadata, mappingIndexId, 1U),
			  std::optional(VectorDeltaOperation::VDO_INSERT));
	expectFormatError(
			[&] { static_cast<void>(VectorDeltaMapping::scan(currentSnapshot, trees, *metadata, mappingIndexId, 2U)); },
			FormatError::FERR_INVALID_STATE);
	oldSnapshot.release();
	currentSnapshot.release();

	static_cast<void>(trees.upsertRootLeaf(mappingIndexId,
										   VectorDeltaMapping::key(1U, VectorDeltaOperation::VDO_ERASE), 101U,
										   LogSequenceNumber(5U)));
	MetadataSnapshotPin contradictory = metadata->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(
						VectorDeltaMapping::findOperation(contradictory, trees, *metadata, mappingIndexId, 1U));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { static_cast<void>(VectorDeltaMapping::scan(contradictory, trees, *metadata, mappingIndexId, 4U)); },
			FormatError::FERR_INVALID_STATE);
	contradictory.release();
}

TEST_F(RelocatableVectorDeltaMappingTest, SelectsStableTypedIdsAndRejectsMalformedRequests) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const uint64_t mappingIndexId = VectorDeltaMapping::selectMappingIndexId(snapshot, *metadata, VECTOR_INDEX_ID);
	EXPECT_TRUE(VectorDeltaMapping::isMappingIndexId(mappingIndexId));
	EXPECT_EQ(mappingIndexId, VectorDeltaMapping::selectMappingIndexId(snapshot, *metadata, VECTOR_INDEX_ID));
	snapshot.release();

	expectFormatError([&] { VectorDeltaMapping::validateMappingIndexId(0U); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::selectMappingIndexId(MetadataSnapshotPin{}, *metadata,
																 VECTOR_INDEX_ID));
			},
			FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin invalidIndexSnapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(VectorDeltaMapping::selectMappingIndexId(invalidIndexSnapshot, *metadata, 0U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::selectMappingIndexId(
						invalidIndexSnapshot, *metadata, UINT64_C(1) << 63U));
			},
			FormatError::FERR_INVALID_HEADER);
	invalidIndexSnapshot.release();
	expectFormatError([&] { static_cast<void>(VectorDeltaMapping::key(0U, VectorDeltaOperation::VDO_INSERT)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						VectorDeltaMapping::key(1U, static_cast<VectorDeltaOperation>(0xFFU)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(VectorDeltaMapping::decodeOperation(0U)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { static_cast<void>(VectorDeltaMapping::decodeEntry({{std::byte{0x01}}, 101U})); },
			FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> zeroVectorKey(sizeof(uint64_t) + sizeof(uint8_t));
	zeroVectorKey.back() = static_cast<std::byte>(VectorDeltaOperation::VDO_INSERT);
	expectFormatError(
			[&] { static_cast<void>(VectorDeltaMapping::decodeEntry({zeroVectorKey, 101U})); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::decodeEntry(
						{VectorDeltaMapping::key(1U, VectorDeltaOperation::VDO_INSERT), 0U}));
			},
			FormatError::FERR_INVALID_STATE);
	std::vector<std::byte> invalidOperationKey =
			VectorDeltaMapping::key(1U, VectorDeltaOperation::VDO_INSERT);
	invalidOperationKey.back() = std::byte{0xFFU};
	expectFormatError(
			[&] { static_cast<void>(VectorDeltaMapping::decodeEntry({invalidOperationKey, 101U})); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::build(trees, *metadata, mappingIndexId, {},
															options(mappingIndexId), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	std::vector<VectorDeltaEntry> unordered{{3U, 103U, VectorDeltaOperation::VDO_INSERT},
											{1U, 101U, VectorDeltaOperation::VDO_ERASE}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::build(trees, *metadata, mappingIndexId, unordered,
															options(mappingIndexId), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array invalidEntries = {
			VectorDeltaEntry{0U, 101U, VectorDeltaOperation::VDO_INSERT},
			VectorDeltaEntry{1U, 0U, VectorDeltaOperation::VDO_INSERT},
			VectorDeltaEntry{1U, 101U, static_cast<VectorDeltaOperation>(0xFFU)},
	};
	for (const VectorDeltaEntry &invalidEntry: invalidEntries) {
		const std::array candidate{invalidEntry};
		expectFormatError(
				[&] {
					static_cast<void>(VectorDeltaMapping::build(trees, *metadata, mappingIndexId, candidate,
															 options(mappingIndexId), LogSequenceNumber(2U)));
				},
				FormatError::FERR_INVALID_HEADER);
	}
	const std::array validEntries = {VectorDeltaEntry{1U, 101U, VectorDeltaOperation::VDO_INSERT}};
	VectorDeltaMappingBuildOptions zeroBound = options(mappingIndexId);
	zeroBound.maximum_entries = 0U;
	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::build(trees, *metadata, mappingIndexId, validEntries,
														 zeroBound, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	VectorDeltaMappingBuildOptions mismatchedBound = options(mappingIndexId);
	++mismatchedBound.tree.maximum_entries;
	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::build(trees, *metadata, mappingIndexId, validEntries,
														 mismatchedBound, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::build(trees, *metadata, mappingIndexId, validEntries,
														 options(mappingIndexId), LogSequenceNumber{}));
			},
			FormatError::FERR_INVALID_HEADER);
	static_cast<void>(VectorDeltaMapping::build(trees, *metadata, mappingIndexId, validEntries,
													options(mappingIndexId), LogSequenceNumber(2U)));
	MetadataSnapshotPin occupied = metadata->openSnapshot();
	const uint64_t alternateMappingIndexId =
			VectorDeltaMapping::selectMappingIndexId(occupied, *metadata, VECTOR_INDEX_ID);
	EXPECT_NE(alternateMappingIndexId, mappingIndexId);
	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::build(trees, *metadata, mappingIndexId, validEntries,
														 options(mappingIndexId), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::findOperation(MetadataSnapshotPin{}, trees, *metadata,
														 mappingIndexId, 1U));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::findOperation(occupied, trees, *metadata, mappingIndexId, 0U));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::scan(MetadataSnapshotPin{}, trees, *metadata, mappingIndexId, 1U));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array<size_t, 2U> invalidScanBounds{0U, std::numeric_limits<size_t>::max()};
	for (const size_t invalidBound: invalidScanBounds) {
		expectFormatError(
				[&] {
					static_cast<void>(
							VectorDeltaMapping::scan(occupied, trees, *metadata, mappingIndexId, invalidBound));
				},
				FormatError::FERR_INVALID_HEADER);
	}
	occupied.release();
}

TEST(RelocatableVectorDeltaMappingBoundsTest, AccountsForInternalPagesWhenDerivingOverlayCapacity) {
	const size_t onePage = VectorDeltaMapping::maximumEntriesForPageBudget(1U);
	ASSERT_GT(onePage, 0U);
	EXPECT_EQ(VectorDeltaMapping::maximumEntriesForPageBudget(0U), 0U);
	EXPECT_EQ(VectorDeltaMapping::maximumEntriesForPageBudget(1U), onePage);
	// Two leaves require a separate root page; a two-page budget can therefore
	// still expose only one leaf worth of semantic entries.
	EXPECT_EQ(VectorDeltaMapping::maximumEntriesForPageBudget(2U), onePage);
	EXPECT_EQ(VectorDeltaMapping::maximumEntriesForPageBudget(3U), 2U * onePage);
	EXPECT_EQ(VectorDeltaMapping::maximumEntriesForPageBudget(4U), 3U * onePage);

	size_t previous = 0U;
	for (size_t pageBudget = 0U; pageBudget <= 128U; ++pageBudget) {
		const size_t capacity = VectorDeltaMapping::maximumEntriesForPageBudget(pageBudget);
		EXPECT_GE(capacity, previous);
		EXPECT_LE(capacity, pageBudget * onePage);
		previous = capacity;
	}
}

TEST(RelocatableVectorDeltaMappingBoundsTest, ValidatesPostMutationRawMembershipByOperation) {
	const VectorDeltaEntry inserted{3U, 103U, VectorDeltaOperation::VDO_INSERT};
	const VectorDeltaEntry erased{1U, 101U, VectorDeltaOperation::VDO_ERASE};
	EXPECT_NO_THROW(VectorDeltaMapping::validateCurrentRawMembership(inserted, true));
	EXPECT_NO_THROW(VectorDeltaMapping::validateCurrentRawMembership(erased, false));
	expectFormatError([&] { VectorDeltaMapping::validateCurrentRawMembership(inserted, false); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { VectorDeltaMapping::validateCurrentRawMembership(erased, true); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				VectorDeltaMapping::validateCurrentRawMembership(
						{0U, 101U, VectorDeltaOperation::VDO_INSERT}, true);
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				VectorDeltaMapping::validateCurrentRawMembership(
						{3U, 0U, VectorDeltaOperation::VDO_INSERT}, true);
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				VectorDeltaMapping::validateCurrentRawMembership(
						{3U, 103U, static_cast<VectorDeltaOperation>(0xFFU)}, true);
			},
			FormatError::FERR_INVALID_STATE);
}
