#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
using namespace graph::storage;

class LocationDirectoryRootGrowthTest : public ::testing::Test {
protected:
 std::filesystem::path path;
 std::shared_ptr<std::fstream> file;
 std::shared_ptr<StorageIO> io;
 void SetUp() override {
  path = std::filesystem::temp_directory_path() / ("zyx_directory_growth_" + to_string(boost::uuids::random_generator()()));
  file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(file->is_open());
  io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
 }
 void TearDown() override {
  io.reset(); file->close(); file.reset();
  std::error_code error; std::filesystem::remove(path, error);
 }
};

TEST_F(LocationDirectoryRootGrowthTest, ErasesPublishedRoutesWhileGrowingOneOrTwoRadixLevels) {
 for (uint64_t factor : {uint64_t{RSS_CATALOG_RADIX_FANOUT}, uint64_t{RSS_CATALOG_RADIX_FANOUT} * RSS_CATALOG_RADIX_FANOUT}) {
  Superblock initial;
  initial.database_id = DatabaseId(17);
  initial.generation = Generation(1);
  initial.checkpoint_lsn = LogSequenceNumber(1);
  SuperblockFile bootstrap(io); bootstrap.initializeNew(initial);
  MetadataPageFile pages(io, initial);
  MetadataPagePoolAllocator allocator(io, initial);
  LocationDirectory directory(pages, allocator);
  const std::array old = {PageLocation{PageId(1), SegmentId(10), 1, Generation(1), 0},
                          PageLocation{PageId(2), SegmentId(10), 2, Generation(1), 0}};
  MetadataWriteBatch first(allocator, initial, LogSequenceNumber(2));
  directory.prepareReplacePages(first, old, {}); first.publish(bootstrap);
  const auto before = first.candidate();
  const PageLocation high{PageId(factor * RSS_LOCATION_LEAF_MAX_ENTRIES + 1), SegmentId(11), 1, Generation(2), 0};
  const std::array erased = {uint64_t{1}};
  MetadataWriteBatch next(allocator, before, LogSequenceNumber(3));
  ASSERT_NO_THROW(directory.prepareReplacePages(next, std::span(&high, 1), erased));
  next.publish(bootstrap);
  EXPECT_EQ(directory.findPage(next.candidate(), high.page_id), high);
  EXPECT_EQ(directory.findPage(next.candidate(), old[1].page_id), old[1]);
  EXPECT_THROW(static_cast<void>(directory.findPage(next.candidate(), old[0].page_id)), FormatException);
  EXPECT_EQ(directory.findPage(before, old[0].page_id), old[0]);
  const std::array absent = {uint64_t{3}};
  MetadataWriteBatch invalid(allocator, next.candidate(), LogSequenceNumber(4));
  EXPECT_THROW(directory.prepareReplacePages(invalid, {}, absent), FormatException);
 }
}
