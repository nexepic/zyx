/**
 * @file test_VectorNodeMapping.cpp
 * @brief Tests for the durable VectorId-to-NodeId semantic mapping.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"

using namespace graph::storage;

namespace {

	constexpr uint64_t VECTOR_INDEX_ID = 77U;

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
		result.database_id = DatabaseId(0x7A6B5C4D3E2F1021ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	[[nodiscard]] std::vector<BTreePageEntry> entries() {
		return {{VectorNodeMapping::key(1U), 101U},
				{VectorNodeMapping::key(3U), 103U},
				{VectorNodeMapping::key(8U), 108U}};
	}

	[[nodiscard]] OrderedBTreeEntrySource sourceFor(const std::vector<BTreePageEntry> &entries,
													const std::shared_ptr<size_t> &cursorCreations = {}) {
		const auto shared = std::make_shared<const std::vector<BTreePageEntry>>(entries);
		return [shared, cursorCreations] {
			if (cursorCreations != nullptr) {
				++*cursorCreations;
			}
			auto position = std::make_shared<size_t>(0U);
			return [shared, position]() -> std::optional<BTreePageEntry> {
				if (*position == shared->size()) {
					return std::nullopt;
				}
				return (*shared)[(*position)++];
			};
		};
	}

	[[nodiscard]] VectorNodeMappingBuildOptions options(uint64_t mappingIndexId, size_t maximumEntries = 16U) {
		VectorNodeMappingBuildOptions result;
		result.tree = {PhysicalSlotId(40U), PhysicalSlotId(42U), mappingIndexId, maximumEntries, 4U};
		result.maximum_entries = maximumEntries;
		return result;
	}

} // namespace

class RelocatableVectorNodeMappingTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::filesystem::path wal_path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<std::fstream> wal_file_;
	std::shared_ptr<StorageIO> io_;
	std::shared_ptr<StorageIO> wal_io_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path_ = std::filesystem::temp_directory_path() / ("relocatable_vector_node_mapping_" + to_string(uuid));
		wal_path_ = path_.string() + ".foreign";
		file_ = std::make_shared<std::fstream>(path_,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		wal_file_ = std::make_shared<std::fstream>(wal_path_,
												   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(file_->is_open());
		ASSERT_TRUE(wal_file_->is_open());
		io_ = std::make_shared<StorageIO>(file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		wal_io_ = std::make_shared<StorageIO>(wal_file_, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	}

	void TearDown() override {
		io_.reset();
		wal_io_.reset();
		if (file_ != nullptr && file_->is_open()) {
			file_->close();
		}
		if (wal_file_ != nullptr && wal_file_->is_open()) {
			wal_file_->close();
		}
		std::error_code error;
		std::filesystem::remove(path_, error);
		std::filesystem::remove(wal_path_, error);
	}
};

TEST_F(RelocatableVectorNodeMappingTest, BuildsStreamedMappingsAndPinsLookupsToTheirMetadataSnapshot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	MetadataSnapshotPin allocationSnapshot = metadata->openSnapshot();
	const uint64_t mappingIndexId =
			VectorNodeMapping::selectMappingIndexId(allocationSnapshot, *metadata, VECTOR_INDEX_ID);
	allocationSnapshot.release();
	const auto cursorCreations = std::make_shared<size_t>(0U);
	const std::vector<BTreePageEntry> expected = entries();
	const BulkBTreeBuildResult built =
			VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor(expected, cursorCreations),
									 options(mappingIndexId), LogSequenceNumber(2U));
	EXPECT_EQ(built.entry_count, expected.size());
	EXPECT_GE(*cursorCreations, 2U);

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	EXPECT_EQ(VectorNodeMapping::findNode(oldSnapshot, trees, *metadata, mappingIndexId, 1U),
			  std::optional<uint64_t>(101U));
	EXPECT_EQ(VectorNodeMapping::findNode(oldSnapshot, trees, *metadata, mappingIndexId, 3U),
			  std::optional<uint64_t>(103U));
	EXPECT_FALSE(VectorNodeMapping::findNode(oldSnapshot, trees, *metadata, mappingIndexId, 2U).has_value());

	static_cast<void>(trees.upsertRootLeaf(mappingIndexId, VectorNodeMapping::key(1U), 201U, LogSequenceNumber(3U)));
	MetadataSnapshotPin currentSnapshot = metadata->openSnapshot();
	EXPECT_EQ(VectorNodeMapping::findNode(oldSnapshot, trees, *metadata, mappingIndexId, 1U),
			  std::optional<uint64_t>(101U));
	EXPECT_EQ(VectorNodeMapping::findNode(currentSnapshot, trees, *metadata, mappingIndexId, 1U),
			  std::optional<uint64_t>(201U));
	oldSnapshot.release();
	currentSnapshot.release();
}

TEST_F(RelocatableVectorNodeMappingTest, SelectsStableTypedIdsAndProbesPastAnOccupiedCandidate) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	MetadataSnapshotPin firstSnapshot = metadata->openSnapshot();
	const uint64_t first = VectorNodeMapping::selectMappingIndexId(firstSnapshot, *metadata, VECTOR_INDEX_ID);
	EXPECT_TRUE(VectorNodeMapping::isMappingIndexId(first));
	EXPECT_EQ(first, VectorNodeMapping::selectMappingIndexId(firstSnapshot, *metadata, VECTOR_INDEX_ID));
	firstSnapshot.release();
	static_cast<void>(VectorNodeMapping::build(trees, *metadata, first, sourceFor(entries()), options(first),
											   LogSequenceNumber(2U)));
	MetadataSnapshotPin occupiedSnapshot = metadata->openSnapshot();
	const uint64_t next = VectorNodeMapping::selectMappingIndexId(occupiedSnapshot, *metadata, VECTOR_INDEX_ID);
	EXPECT_TRUE(VectorNodeMapping::isMappingIndexId(next));
	EXPECT_NE(next, first);
	EXPECT_EQ(next, VectorNodeMapping::selectMappingIndexId(occupiedSnapshot, *metadata, VECTOR_INDEX_ID));
	occupiedSnapshot.release();
}

TEST_F(RelocatableVectorNodeMappingTest, RejectsUntypedIdsAndMalformedCanonicalEntries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	expectFormatError([&] { VectorNodeMapping::validateMappingIndexId(0U); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { VectorNodeMapping::validateMappingIndexId(VECTOR_INDEX_ID); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(VectorNodeMapping::key(0U)); }, FormatError::FERR_INVALID_HEADER);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const uint64_t mappingIndexId = VectorNodeMapping::selectMappingIndexId(snapshot, *metadata, VECTOR_INDEX_ID);
	snapshot.release();
	const std::vector<BTreePageEntry> malformedKey = {{{std::byte{0x01}}, 1U}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor(malformedKey),
														   options(mappingIndexId), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::vector<BTreePageEntry> zeroNode = {{VectorNodeMapping::key(1U), 0U}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor(zeroNode),
														   options(mappingIndexId), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::vector<BTreePageEntry> unordered = {{VectorNodeMapping::key(3U), 3U}, {VectorNodeMapping::key(1U), 1U}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor(unordered),
														   options(mappingIndexId), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorNodeMappingTest, RejectsEmptyBoundedAndAlreadyPublishedBuildInputs) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const uint64_t mappingIndexId = VectorNodeMapping::selectMappingIndexId(snapshot, *metadata, VECTOR_INDEX_ID);
	snapshot.release();

	const OrderedBTreeEntrySource noSource;
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, noSource,
														   options(mappingIndexId), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	VectorNodeMappingBuildOptions zeroMaximum = options(mappingIndexId);
	zeroMaximum.maximum_entries = 0U;
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor(entries()),
														   zeroMaximum, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	VectorNodeMappingBuildOptions inconsistent = options(mappingIndexId);
	inconsistent.tree.maximum_entries = inconsistent.maximum_entries - 1U;
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor(entries()),
														   inconsistent, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor({}),
														   options(mappingIndexId), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor(entries()),
														   options(mappingIndexId), LogSequenceNumber{}));
			},
			FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> zeroVectorKey(sizeof(uint64_t));
	const std::vector<BTreePageEntry> zeroVector = {{std::move(zeroVectorKey), 1U}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor(zeroVector),
														   options(mappingIndexId), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::vector<BTreePageEntry> tooMany = {{VectorNodeMapping::key(1U), 1U}, {VectorNodeMapping::key(2U), 2U}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor(tooMany),
														   options(mappingIndexId, 1U), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);

	static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor(entries()),
											   options(mappingIndexId), LogSequenceNumber(2U)));
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor(entries()),
														   options(mappingIndexId), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(VectorNodeMapping::findNode(current, trees, *metadata, mappingIndexId, 0U)); },
			FormatError::FERR_INVALID_HEADER);
	current.release();
	expectFormatError(
			[&] { static_cast<void>(VectorNodeMapping::findNode(current, trees, *metadata, mappingIndexId, 1U)); },
			FormatError::FERR_INVALID_HEADER);
	static_cast<void>(trees.upsertRootLeaf(mappingIndexId, VectorNodeMapping::key(1U), 0U, LogSequenceNumber(3U)));
	MetadataSnapshotPin zeroValueSnapshot = metadata->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::findNode(zeroValueSnapshot, trees, *metadata, mappingIndexId, 1U));
			},
			FormatError::FERR_INVALID_STATE);
	zeroValueSnapshot.release();
}

TEST_F(RelocatableVectorNodeMappingTest, RejectsInvalidAllocationInputsAndForeignTreeStores) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const uint64_t mappingIndexId = VectorNodeMapping::selectMappingIndexId(snapshot, *metadata, VECTOR_INDEX_ID);
	snapshot.release();
	MetadataSnapshotPin invalidSnapshot = metadata->openSnapshot();
	invalidSnapshot.release();
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::selectMappingIndexId(invalidSnapshot, *metadata, VECTOR_INDEX_ID));
			},
			FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin validSnapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(VectorNodeMapping::selectMappingIndexId(validSnapshot, *metadata, 0U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						VectorNodeMapping::selectMappingIndexId(validSnapshot, *metadata, UINT64_C(1) << 63U));
			},
			FormatError::FERR_INVALID_HEADER);
	validSnapshot.release();

	const std::filesystem::path foreignPath = path_.string() + ".foreign";
	const std::filesystem::path foreignWalPath = foreignPath.string() + ".foreign";
	auto foreignFile = std::make_shared<std::fstream>(foreignPath, std::ios::in | std::ios::out | std::ios::binary |
																		   std::ios::trunc);
	auto foreignWalFile = std::make_shared<std::fstream>(foreignWalPath, std::ios::in | std::ios::out |
																				 std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(foreignFile->is_open());
	ASSERT_TRUE(foreignWalFile->is_open());
	auto foreignIo = std::make_shared<StorageIO>(foreignFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto foreignWalIo = std::make_shared<StorageIO>(foreignWalFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto foreignMetadata = RelocatableMetadataStore::initializeNew(foreignIo, initialSuperblock());
	expectFormatError(
			[&] {
				static_cast<void>(VectorNodeMapping::build(trees, *foreignMetadata, mappingIndexId,
														   sourceFor(entries()), options(mappingIndexId),
														   LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);
	foreignMetadata.reset();
	foreignIo.reset();
	foreignWalIo.reset();
	foreignFile->close();
	foreignWalFile->close();
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
	std::filesystem::remove(foreignWalPath, error);
}

TEST_F(RelocatableVectorNodeMappingTest, FailsClosedAfterExhaustingTheBoundedTypedIdProbeSequence) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const std::vector<BTreePageEntry> singleton = {{VectorNodeMapping::key(1U), 1U}};
	for (size_t probe = 0U; probe < 256U; ++probe) {
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const uint64_t mappingIndexId = VectorNodeMapping::selectMappingIndexId(snapshot, *metadata, VECTOR_INDEX_ID);
		snapshot.release();
		VectorNodeMappingBuildOptions buildOptions = options(mappingIndexId, 1U);
		buildOptions.tree.first_slot = PhysicalSlotId(40U);
		buildOptions.tree.last_slot = PhysicalSlotId(400U);
		static_cast<void>(VectorNodeMapping::build(trees, *metadata, mappingIndexId, sourceFor(singleton), buildOptions,
												   LogSequenceNumber(probe + 2U)));
	}
	MetadataSnapshotPin exhaustedSnapshot = metadata->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(
						VectorNodeMapping::selectMappingIndexId(exhaustedSnapshot, *metadata, VECTOR_INDEX_ID));
			},
			FormatError::FERR_INVALID_STATE);
	exhaustedSnapshot.release();
}
