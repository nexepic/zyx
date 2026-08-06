/**
 * @file test_VectorAdjacencyMapping.cpp
 * @brief Immutable VectorId-to-adjacency-page mapping tests.
 */

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"

using namespace graph::storage;

namespace {

	constexpr uint64_t MAPPING_INDEX_ID = RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | UINT64_C(0x74);

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

	VectorAdjacencyMappingBuildOptions options() {
		VectorAdjacencyMappingBuildOptions result;
		result.tree = {PhysicalSlotId(40), PhysicalSlotId(42), MAPPING_INDEX_ID, 16, 4};
		result.maximum_entries = 16;
		return result;
	}

	std::vector<VectorAdjacencyPageMapping> mappings() {
		return {{1, PageId(101)}, {3, PageId(103)}, {8, PageId(108)}};
	}

} // namespace

class RelocatableVectorAdjacencyMappingTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::filesystem::path walPath;
	std::shared_ptr<std::fstream> file;
	std::shared_ptr<std::fstream> walFile;
	std::shared_ptr<StorageIO> io;
	std::shared_ptr<StorageIO> walIo;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		path = std::filesystem::temp_directory_path() / ("relocatable_adjacency_mapping_" + to_string(uuid));
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

TEST_F(RelocatableVectorAdjacencyMappingTest, BuildsAndFindsSortedMappingsAcrossReopen) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const BulkBTreeBuildResult built = VectorAdjacencyMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(),
																	 options(), LogSequenceNumber(2));
	EXPECT_EQ(built.entry_count, 3U);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(VectorAdjacencyMapping::findPage(snapshot, trees, *metadata, MAPPING_INDEX_ID, 1), PageId(101));
	EXPECT_EQ(VectorAdjacencyMapping::findPage(snapshot, trees, *metadata, MAPPING_INDEX_ID, 3), PageId(103));
	EXPECT_FALSE(VectorAdjacencyMapping::findPage(snapshot, trees, *metadata, MAPPING_INDEX_ID, 2).has_value());
	snapshot.release();
	metadata.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedSlotted(io, *reopened);
	BTreePageStore reopenedTrees(reopenedSlotted, *reopened);
	const MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(VectorAdjacencyMapping::findPage(reopenedSnapshot, reopenedTrees, *reopened, MAPPING_INDEX_ID, 8),
			  PageId(108));
}

TEST_F(RelocatableVectorAdjacencyMappingTest, RejectsPublicOrZeroMappingIdentityAndZeroKey) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	expectFormatError([&] { VectorAdjacencyMapping::validateMappingIndexId(0); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { VectorAdjacencyMapping::validateMappingIndexId(17); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(VectorAdjacencyMapping::key(0)); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(VectorAdjacencyMapping::build(trees, *metadata, 17, mappings(), options(),
																LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorAdjacencyMappingTest, RejectsUnsortedDuplicateAndInvalidMappingValues) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	const std::vector<VectorAdjacencyPageMapping> unsorted{{3, PageId(103)}, {1, PageId(101)}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorAdjacencyMapping::build(trees, *metadata, MAPPING_INDEX_ID, unsorted, options(),
																LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::vector<VectorAdjacencyPageMapping> duplicate{{1, PageId(101)}, {1, PageId(102)}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorAdjacencyMapping::build(trees, *metadata, MAPPING_INDEX_ID, duplicate,
																options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::vector<VectorAdjacencyPageMapping> zeroVector{{0, PageId(101)}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorAdjacencyMapping::build(trees, *metadata, MAPPING_INDEX_ID, zeroVector,
																 options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	const std::vector<VectorAdjacencyPageMapping> invalidPage{{1, PageId{}}};
	expectFormatError(
			[&] {
				static_cast<void>(VectorAdjacencyMapping::build(trees, *metadata, MAPPING_INDEX_ID, invalidPage,
																options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableVectorAdjacencyMappingTest, RejectsForeignPageStoreDuplicateRootAndZeroLookupIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	static_cast<void>(
			VectorAdjacencyMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(), options(), LogSequenceNumber(2U)));
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	expectFormatError(
			[&] { static_cast<void>(VectorAdjacencyMapping::findPage(snapshot, trees, *metadata, MAPPING_INDEX_ID, 0U)); },
			FormatError::FERR_INVALID_HEADER);
	snapshot.release();
	expectFormatError(
			[&] {
				static_cast<void>(
						VectorAdjacencyMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(), options(),
																LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);

	const std::filesystem::path otherPath = path.string() + ".other";
	auto otherFile = std::make_shared<std::fstream>(
			otherPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(otherFile->is_open());
	auto otherIo = std::make_shared<StorageIO>(otherFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto otherMetadata = RelocatableMetadataStore::initializeNew(otherIo, initialSuperblock());
	SlottedPagePool otherSlotted(otherIo, *otherMetadata);
	BTreePageStore foreignTrees(otherSlotted, *otherMetadata);
	expectFormatError(
			[&] {
				static_cast<void>(VectorAdjacencyMapping::build(foreignTrees, *metadata, MAPPING_INDEX_ID,
																								mappings(), options(), LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
	otherMetadata.reset();
	otherIo.reset();
	otherFile->close();
	std::error_code error;
	std::filesystem::remove(otherPath, error);
}

TEST_F(RelocatableVectorAdjacencyMappingTest, RejectsUnboundedOrInconsistentBuildOptions) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	SlottedPagePool slotted(io, *metadata);
	BTreePageStore trees(slotted, *metadata);
	VectorAdjacencyMappingBuildOptions invalid = options();
	invalid.maximum_entries = 0;
	expectFormatError(
			[&] {
				static_cast<void>(VectorAdjacencyMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(), invalid,
																LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	invalid = options();
	invalid.tree.maximum_entries = 15;
	expectFormatError(
			[&] {
				static_cast<void>(VectorAdjacencyMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(), invalid,
																LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	invalid = options();
	invalid.tree.pool_owner_id = MAPPING_INDEX_ID + 1U;
	expectFormatError(
			[&] {
				static_cast<void>(VectorAdjacencyMapping::build(trees, *metadata, MAPPING_INDEX_ID, mappings(), invalid,
															 LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
}
