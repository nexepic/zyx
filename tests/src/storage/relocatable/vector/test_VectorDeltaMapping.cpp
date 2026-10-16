/**
 * @file test_VectorDeltaMapping.cpp
 * @brief Tests for the durable Vector base/delta overlay mapping.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
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

	void expectKeyMutationsToRejectOrExactlyRoundTrip(const std::vector<std::byte> &encodedKey) {
		for (size_t byteOffset = 0U; byteOffset < encodedKey.size(); ++byteOffset) {
			for (uint8_t bit = 0U; bit < 8U; ++bit) {
				auto mutation = encodedKey;
				mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
				try {
					const VectorDeltaEntry decoded = VectorDeltaMapping::decodeEntry({mutation, 101U});
					EXPECT_EQ(VectorDeltaMapping::key(decoded.vector_id, decoded.operation), mutation)
							<< "Vector delta key mutation was normalized at byte " << byteOffset << ", bit "
							<< static_cast<unsigned>(bit);
				} catch (const FormatException &) {
					// Invalid operation or zero identity must fail at the typed boundary.
				}
			}
		}
	}

	std::vector<VectorDeltaEntry> entries() {
		return {{1U, 101U, VectorDeltaOperation::VDO_ERASE},
				{3U, 103U, VectorDeltaOperation::VDO_INSERT},
				{8U, 108U, VectorDeltaOperation::VDO_ERASE}};
	}

	class ThrowingCancellationCheckpoint final : public CancellationCheckpoint {
	public:
		explicit ThrowingCancellationCheckpoint(size_t throwAfter) : throw_after_(throwAfter) {}

		void check() const override {
			++checks;
			if (checks == throw_after_) {
				throw std::runtime_error("test Vector delta scan cancellation");
			}
		}

		mutable size_t checks = 0U;

	private:
		size_t throw_after_ = 0U;
	};

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
    {
        using graph::query::execution::MaterializedByteBudget;
        auto memory = std::make_shared<MaterializedByteBudget>(1024U * 1024U);
        graph::utils::OwnedAllocationScope scope(memory);
        {
            const auto owned = VectorDeltaMapping::scanOwned(oldSnapshot, trees, *metadata, mappingIndexId, expected.size());
            EXPECT_EQ(owned, expected);
            EXPECT_EQ(memory->used(), owned.retainedBackingRequestBytes());
            EXPECT_GE(memory->used(), expected.size() * sizeof(VectorDeltaEntry));
        }
        EXPECT_EQ(memory->used(), 0U);
    }


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

TEST_F(RelocatableVectorDeltaMappingTest, RejectsOrExactlyRoundTripsEverySingleBitKeyMutation) {
	expectKeyMutationsToRejectOrExactlyRoundTrip(VectorDeltaMapping::key(1U, VectorDeltaOperation::VDO_INSERT));
	expectKeyMutationsToRejectOrExactlyRoundTrip(VectorDeltaMapping::key(1U, VectorDeltaOperation::VDO_ERASE));
}

TEST_F(RelocatableVectorDeltaMappingTest, RejectsForeignTreeStoreBeforeCreatingAnOverlay) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const uint64_t mappingIndexId = VectorDeltaMapping::selectMappingIndexId(snapshot, *metadata, VECTOR_INDEX_ID);
	snapshot.release();

	const std::filesystem::path foreignPath = path_.string() + ".foreign";
	auto foreignFile = std::make_shared<std::fstream>(foreignPath,
												  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(foreignFile->is_open());
	auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
	SlottedPagePool foreignSlotted(foreignIo, *foreignMetadata);
	BTreePageStore foreignTrees(foreignSlotted, *foreignMetadata);
	const std::array validEntries = {VectorDeltaEntry{1U, 101U, VectorDeltaOperation::VDO_INSERT}};

	expectFormatError(
			[&] {
				static_cast<void>(VectorDeltaMapping::build(foreignTrees, *metadata, mappingIndexId, validEntries,
																	options(mappingIndexId), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);

	foreignMetadata.reset();
	foreignIo.reset();
	foreignFile->close();
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
}

TEST_F(RelocatableVectorDeltaMappingTest, RejectsZeroNodeIdsInDurableOverlayEntries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	MetadataSnapshotPin allocationSnapshot = metadata->openSnapshot();
	const uint64_t mappingIndexId =
			VectorDeltaMapping::selectMappingIndexId(allocationSnapshot, *metadata, VECTOR_INDEX_ID);
	allocationSnapshot.release();
	static_cast<void>(VectorDeltaMapping::build(trees, *metadata, mappingIndexId, entries(), options(mappingIndexId),
																	 LogSequenceNumber(2U)));

	static_cast<void>(trees.upsertRootLeaf(mappingIndexId, VectorDeltaMapping::key(3U, VectorDeltaOperation::VDO_INSERT),
														  0U, LogSequenceNumber(3U)));
	MetadataSnapshotPin zeroInsert = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(VectorDeltaMapping::findOperation(zeroInsert, trees, *metadata, mappingIndexId, 3U)); },
			FormatError::FERR_INVALID_STATE);
	zeroInsert.release();

	static_cast<void>(trees.upsertRootLeaf(mappingIndexId, VectorDeltaMapping::key(1U, VectorDeltaOperation::VDO_ERASE),
														  0U, LogSequenceNumber(4U)));
	MetadataSnapshotPin zeroErase = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(VectorDeltaMapping::findOperation(zeroErase, trees, *metadata, mappingIndexId, 1U)); },
			FormatError::FERR_INVALID_STATE);
	zeroErase.release();
}

TEST_F(RelocatableVectorDeltaMappingTest, PropagatesCancellationAfterBTreeMaterialization) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	MetadataSnapshotPin allocationSnapshot = metadata->openSnapshot();
	const uint64_t mappingIndexId =
			VectorDeltaMapping::selectMappingIndexId(allocationSnapshot, *metadata, VECTOR_INDEX_ID);
	allocationSnapshot.release();
	const std::array entry = {VectorDeltaEntry{1U, 101U, VectorDeltaOperation::VDO_INSERT}};
	static_cast<void>(VectorDeltaMapping::build(trees, *metadata, mappingIndexId, entry, options(mappingIndexId),
																	 LogSequenceNumber(2U)));

	MetadataSnapshotPin cancellationSnapshot = metadata->openSnapshot();
	// A single-entry leaf has three bounded B+Tree checkpoints before the
	// Vector mapping decodes its first materialized entry.
	ThrowingCancellationCheckpoint cancellation(4U);
	EXPECT_THROW(static_cast<void>(VectorDeltaMapping::scan(cancellationSnapshot, trees, *metadata, mappingIndexId,
																								  entry.size(), &cancellation)),
				 std::runtime_error);
	EXPECT_EQ(cancellation.checks, 4U);
	cancellationSnapshot.release();
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
