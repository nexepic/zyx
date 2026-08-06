/**
 * @file test_VectorPqMapping.cpp
 * @brief Durable VectorId-to-PQ-page mapping contract tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

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
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"

using namespace graph::storage;

namespace {

	constexpr uint64_t MAPPING_INDEX_ID = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x74);

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
		result.database_id = DatabaseId(0x3141592653589793ULL);
		result.generation = Generation(1U);
		result.checkpoint_lsn = LogSequenceNumber(1U);
		return result;
	}

	VectorPqMappingBuildOptions options() {
		VectorPqMappingBuildOptions result;
		result.tree = {PhysicalSlotId(40U), PhysicalSlotId(42U), MAPPING_INDEX_ID, 16U, 4U};
		result.maximum_entries = 16U;
		return result;
	}

	std::vector<VectorPqPageMapping> mappings() { return {{1U, PageId(101U)}, {3U, PageId(103U)}, {8U, PageId(108U)}}; }

} // namespace

class RelocatableVectorPqMappingTest : public ::testing::Test {
protected:
	std::filesystem::path path_;
	std::filesystem::path wal_path_;
	std::shared_ptr<std::fstream> file_;
	std::shared_ptr<std::fstream> wal_file_;
	std::shared_ptr<StorageIO> io_;
	std::shared_ptr<StorageIO> wal_io_;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path_ = std::filesystem::temp_directory_path() / ("relocatable_pq_mapping_" + to_string(uuid));
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

TEST_F(RelocatableVectorPqMappingTest, BuildsFindsAndReopensSortedMappings) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const BulkBTreeBuildResult built =
			VectorPqMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(), options(), LogSequenceNumber(2U));
	EXPECT_EQ(built.entry_count, 3U);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(VectorPqMapping::findPage(snapshot, trees, *metadata, MAPPING_INDEX_ID, 1U), PageId(101U));
	EXPECT_EQ(VectorPqMapping::findPage(snapshot, trees, *metadata, MAPPING_INDEX_ID, 3U), PageId(103U));
	EXPECT_FALSE(VectorPqMapping::findPage(snapshot, trees, *metadata, MAPPING_INDEX_ID, 2U).has_value());
	snapshot.release();
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io_);
	SlottedPagePool reopenedSlotted(io_, *reopened);
	BTreePageStore reopenedTrees(reopenedSlotted, *reopened);
	const MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(VectorPqMapping::findPage(reopenedSnapshot, reopenedTrees, *reopened, MAPPING_INDEX_ID, 8U),
			  PageId(108U));
}

TEST_F(RelocatableVectorPqMappingTest, RejectsInvalidBuildIdentitiesBoundsAndEntries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	expectFormatError([&] { VectorPqMapping::validateMappingIndexId(0U); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { VectorPqMapping::validateMappingIndexId(17U); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						VectorPqMapping::build(trees, *metadata, 17U, mappings(), options(), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(VectorPqMapping::build(trees, *metadata, MAPPING_INDEX_ID, {}, options(),
														 LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	VectorPqMappingBuildOptions invalidOptions = options();
	invalidOptions.maximum_entries = 0U;
	expectFormatError(
			[&] {
				static_cast<void>(VectorPqMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(), invalidOptions,
														 LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	invalidOptions = options();
	invalidOptions.tree.maximum_entries = invalidOptions.maximum_entries - 1U;
	expectFormatError(
			[&] {
				static_cast<void>(VectorPqMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(), invalidOptions,
														 LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						VectorPqMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(), options(), {}));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array zeroVector = {VectorPqPageMapping{0U, PageId(101U)}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorPqMapping::build(trees, *metadata, MAPPING_INDEX_ID, zeroVector, options(),
														 LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array unordered = {VectorPqPageMapping{3U, PageId(103U)}, VectorPqPageMapping{1U, PageId(101U)}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorPqMapping::build(trees, *metadata, MAPPING_INDEX_ID, unordered, options(),
														 LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::array invalidPage = {VectorPqPageMapping{1U, PageId{}}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorPqMapping::build(trees, *metadata, MAPPING_INDEX_ID, invalidPage, options(),
														 LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorPqMappingTest, RejectsForeignTreeAndInvalidLookupRequests) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
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
	SlottedPagePool foreignSlotted(foreignIo, *foreignMetadata);
	BTreePageStore foreignTrees(foreignSlotted, *foreignMetadata);
	expectFormatError(
			[&] {
				static_cast<void>(VectorPqMapping::build(foreignTrees, *metadata, MAPPING_INDEX_ID, mappings(),
														 options(), LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_STATE);
	static_cast<void>(
			VectorPqMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(), options(), LogSequenceNumber(2U)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError([&] { static_cast<void>(VectorPqMapping::findPage(snapshot, trees, *metadata, 17U, 1U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(VectorPqMapping::findPage(snapshot, trees, *metadata, MAPPING_INDEX_ID, 0U)); },
			FormatError::FERR_INVALID_HEADER);
	snapshot.release();
	foreignMetadata.reset();
	foreignIo.reset();
	foreignWalIo.reset();
	foreignFile->close();
	foreignWalFile->close();
	std::error_code error;
	std::filesystem::remove(foreignPath, error);
	std::filesystem::remove(foreignWalPath, error);
}

TEST_F(RelocatableVectorPqMappingTest, RejectsDuplicateBuildAndPersistedZeroPageMapping) {
	auto metadata = RelocatableMetadataStore::initializeNew(io_, initialSuperblock());
	SlottedPagePool slotted(io_, *metadata);
	BTreePageStore trees(slotted, *metadata);
	static_cast<void>(
			VectorPqMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(), options(), LogSequenceNumber(2U)));
	expectFormatError(
			[&] {
				static_cast<void>(VectorPqMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(), options(),
														 LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
	static_cast<void>(trees.upsert(MAPPING_INDEX_ID, VectorPqMapping::key(9U), 0U, LogSequenceNumber(3U)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(VectorPqMapping::findPage(snapshot, trees, *metadata, MAPPING_INDEX_ID, 9U)); },
			FormatError::FERR_INVALID_STATE);
}
