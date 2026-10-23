#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>
#include "graph/storage/StorageIO.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
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

class LocationDirectoryLogicalCowTest : public LocationDirectoryRootGrowthTest, public ::testing::WithParamInterface<size_t> {};
TEST_P(LocationDirectoryLogicalCowTest, KeepsUntouchedChildIdentitiesWithoutDecodingSiblingLeaves) {
 const size_t branchCount = GetParam();
  Superblock initial; initial.database_id = DatabaseId(17); initial.generation = Generation(1);
  initial.checkpoint_lsn = LogSequenceNumber(1);
  auto store = RelocatableMetadataStore::initializeNewComplete(io, initial);
  std::vector<PageLocation> locations;
  for (size_t i = 0; i < branchCount; ++i)
   locations.push_back({PageId(i * RSS_LOCATION_LEAF_MAX_ENTRIES + 1U), SegmentId(10), 1, Generation(1), 0});
  static_cast<void>(store->upsertLocations(locations, {}, LogSequenceNumber(3)));
  auto old = store->openSnapshot();
  auto replacement = locations.front(); replacement.free_bytes = 64U;
  const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
  struct Restore { bool enabled; ~Restore() { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); } } restore{enabled};
  graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
  static_cast<void>(store->upsertLocations(std::span(&replacement, 1U), {}, LogSequenceNumber(4)));
  const auto counters = graph::debug::PerfTrace::snapshotAndReset();
  ASSERT_TRUE(counters.contains("native_graph.directory.retained_logical_radix_pages"));
  EXPECT_GT(counters.at("native_graph.directory.retained_logical_radix_pages").totalValue, 0);
  ASSERT_TRUE(counters.contains("native_graph.read.metadata_decodes.location"));
  EXPECT_LE(counters.at("native_graph.read.metadata_decodes.location").totalValue, 4) << branchCount;
  ASSERT_TRUE(counters.contains("native_graph.directory.mutation_child_route_resolutions"));
  EXPECT_EQ(counters.at("native_graph.directory.mutation_child_route_resolutions").totalValue, 1);
  RecordProperty("selected_child_routes_" + std::to_string(branchCount),
    std::to_string(counters.at("native_graph.directory.mutation_child_route_resolutions").totalValue));
  RecordProperty("sibling_branches_" + std::to_string(branchCount),
    std::to_string(counters.at("native_graph.read.metadata_decodes.location").totalValue));
  auto current = store->openSnapshot();
  EXPECT_EQ(store->findPageLocation(old, locations.front().page_id), locations.front());
  EXPECT_EQ(store->findPageLocation(current, replacement.page_id), replacement);
  for (size_t i = 1; i < locations.size(); ++i)
   EXPECT_EQ(store->findPageLocation(current, locations[i].page_id), locations[i]);
  old.release(); current.release(); store.reset();
  store = RelocatableMetadataStore::open(io);
  auto reopened = store->openSnapshot();
  EXPECT_EQ(store->findPageLocation(reopened, replacement.page_id), replacement);
  for (size_t i = 1; i < locations.size(); ++i)
   EXPECT_EQ(store->findPageLocation(reopened, locations[i].page_id), locations[i]);
  reopened.release(); store.reset();
}
TEST_P(LocationDirectoryLogicalCowTest, CatalogCowPreservesSiblingIdsAndOldSnapshots) {
 const size_t branchCount = GetParam();
 Superblock initial; initial.database_id = DatabaseId(17); initial.generation = Generation(1);
 initial.checkpoint_lsn = LogSequenceNumber(1);
 auto store = RelocatableMetadataStore::initializeNewComplete(io, initial);
 std::vector<CatalogEntry> entries;
 for (size_t i = 0; i < branchCount; ++i)
  entries.push_back({SegmentId(i * RSS_CATALOG_PAGE_MAX_ENTRIES + 10U), PhysicalSlotId(1000U + i),
    SegmentKind::SGK_ENTITY_PAGE_POOL, 7U, Generation(1), SegmentState::SGS_SEALED, LogSequenceNumber(3)});
 static_cast<void>(store->upsertCatalog(entries, LogSequenceNumber(3)));
 auto old = store->openSnapshot();
 auto replacement = entries.front(); replacement.generation = Generation(2); replacement.committed_lsn = LogSequenceNumber(4);
 const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
 struct Restore { bool enabled; ~Restore() { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); } } restore{enabled};
 graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
 static_cast<void>(store->upsertCatalog(std::span(&replacement, 1U), LogSequenceNumber(4)));
 const auto counters = graph::debug::PerfTrace::snapshotAndReset();
 ASSERT_TRUE(counters.contains("native_graph.catalog.retained_logical_radix_pages"));
 EXPECT_GT(counters.at("native_graph.catalog.retained_logical_radix_pages").totalValue, 0);
 ASSERT_TRUE(counters.contains("native_graph.read.metadata_decodes.catalog"));
 EXPECT_LE(counters.at("native_graph.read.metadata_decodes.catalog").totalValue, 8) << branchCount;
 ASSERT_TRUE(counters.contains("native_graph.catalog.mutation_child_route_resolutions"));
 EXPECT_LE(counters.at("native_graph.catalog.mutation_child_route_resolutions").totalValue, 4);
 RecordProperty("catalog_selected_child_routes_" + std::to_string(branchCount),
   std::to_string(counters.at("native_graph.catalog.mutation_child_route_resolutions").totalValue));
 RecordProperty("catalog_sibling_branches_" + std::to_string(branchCount),
   std::to_string(counters.at("native_graph.read.metadata_decodes.catalog").totalValue));
 auto current = store->openSnapshot();
 EXPECT_EQ(store->findCatalog(old, entries.front().segment_id).generation, entries.front().generation);
 EXPECT_EQ(store->findCatalog(current, replacement.segment_id).generation, replacement.generation);
 for (size_t i = 1; i < entries.size(); ++i)
  EXPECT_EQ(store->findCatalog(current, entries[i].segment_id).physical_slot, entries[i].physical_slot);
 old.release(); current.release(); store.reset(); store = RelocatableMetadataStore::open(io);
 auto reopened = store->openSnapshot();
 EXPECT_EQ(store->findCatalog(reopened, replacement.segment_id).generation, replacement.generation);
 for (size_t i = 1; i < entries.size(); ++i)
  EXPECT_EQ(store->findCatalog(reopened, entries[i].segment_id).physical_slot, entries[i].physical_slot);
}
INSTANTIATE_TEST_SUITE_P(BranchCounts, LocationDirectoryLogicalCowTest, ::testing::Values(3U, 31U, 90U));

TEST_F(LocationDirectoryRootGrowthTest, BootstrapRootGrowthWritesLogicalInternalChildrenAndRetainsTheirIds) {
 Superblock initial; initial.database_id = DatabaseId(17); initial.generation = Generation(1);
 initial.checkpoint_lsn = LogSequenceNumber(1);
 auto store = RelocatableMetadataStore::initializeNewComplete(io, initial);
 const PageLocation low{PageId(1), SegmentId(10), 1, Generation(1), 0};
 static_cast<void>(store->upsertLocations(std::span(&low, 1U), {}, LogSequenceNumber(3)));
 auto old = store->openSnapshot();
 PageLocation high{PageId(uint64_t{RSS_CATALOG_RADIX_FANOUT} * RSS_LOCATION_LEAF_MAX_ENTRIES + 1U),
   SegmentId(11), 1, Generation(1), 0};
 static_cast<void>(store->upsertLocations(std::span(&high, 1U), {}, LogSequenceNumber(4)));
 auto current = store->openSnapshot();
 MetadataPageFile pages(io, current.superblock());
 const auto boot = pages.readPageDirectoryBootstrap(current.superblock().page_directory_root);
 const auto root = pages.readMetadataRadixUnresolved(boot.data_page_directory_root);
 ASSERT_EQ(root.level, 1U); ASSERT_TRUE(root.logical_children_enabled);
 MetadataPageRouteDirectory routes(pages);
 for (const auto id : root.logical_children) if (id.isValid()) {
  const auto target = routes.find(current.superblock(), boot.metadata_page_directory_root, id);
  const auto child = pages.readMetadataRadixUnresolved(target);
  EXPECT_TRUE(child.logical_children_enabled);
  EXPECT_EQ(child.page_id, id);
 }
 const auto highChild = root.logical_children[1U]; ASSERT_TRUE(highChild.isValid());
 high.free_bytes = 128U;
 static_cast<void>(store->upsertLocations(std::span(&high, 1U), {}, LogSequenceNumber(5)));
 auto next = store->openSnapshot();
 const auto nextBoot = pages.readPageDirectoryBootstrap(next.superblock().page_directory_root);
 const auto nextRoot = pages.readMetadataRadixUnresolved(nextBoot.data_page_directory_root);
 EXPECT_EQ(nextRoot.logical_children[1U], highChild);
 EXPECT_EQ(store->findPageLocation(old, low.page_id), low);
 EXPECT_EQ(store->findPageLocation(next, high.page_id), high);
 old.release(); current.release(); next.release(); store.reset();
 store = RelocatableMetadataStore::open(io); auto reopened = store->openSnapshot();
 EXPECT_EQ(store->findPageLocation(reopened, low.page_id), low);
 EXPECT_EQ(store->findPageLocation(reopened, high.page_id), high);
}

TEST_F(LocationDirectoryRootGrowthTest, SparseEraseKeepsUnresolvedSiblingIdentitiesInBothDirectories) {
 Superblock initial; initial.database_id = DatabaseId(17); initial.generation = Generation(1);
 initial.checkpoint_lsn = LogSequenceNumber(1);
 auto store = RelocatableMetadataStore::initializeNewComplete(io, initial);
 const std::array locations{
  PageLocation{PageId(1), SegmentId(10), 1, Generation(1), 0},
  PageLocation{PageId(RSS_LOCATION_LEAF_MAX_ENTRIES + 1U), SegmentId(11), 1, Generation(1), 0},
  PageLocation{PageId(2U * RSS_LOCATION_LEAF_MAX_ENTRIES + 1U), SegmentId(12), 1, Generation(1), 0}};
 static_cast<void>(store->upsertLocations(locations, {}, LogSequenceNumber(3)));
 const std::array entries{
  CatalogEntry{SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES + 17U), PhysicalSlotId(1000U),
   SegmentKind::SGK_ENTITY_PAGE_POOL, 7U, Generation(1), SegmentState::SGS_SEALED, LogSequenceNumber(4)},
  CatalogEntry{SegmentId(2U * RSS_CATALOG_PAGE_MAX_ENTRIES + 17U), PhysicalSlotId(1001U),
   SegmentKind::SGK_ENTITY_PAGE_POOL, 7U, Generation(1), SegmentState::SGS_SEALED, LogSequenceNumber(4)}};
 static_cast<void>(store->upsertCatalog(entries, LogSequenceNumber(4)));
 auto old = store->openSnapshot(); const auto current = old.superblock();
 MetadataPageFile pages(io, current);
 const auto boot = pages.readPageDirectoryBootstrap(current.page_directory_root);
 const auto beforeData = pages.readMetadataRadixUnresolved(boot.data_page_directory_root);
 const auto beforeCatalog = pages.readMetadataRadixUnresolved(current.catalog_root);
 MetadataPagePoolAllocator allocator(io, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER);
 CatalogTree catalog(pages, allocator); LocationDirectory directory(pages, allocator);
 MetadataWriteBatch batch(allocator, current, LogSequenceNumber(5));
 const std::array erasedCatalog{entries.front().segment_id};
 catalog.prepareErase(batch, erasedCatalog);
 const std::array<uint64_t, 1U> erasedPages{locations.front().page_id.value()};
 directory.prepareReplacePages(batch, {}, erasedPages);
 const auto afterCatalog = pages.readMetadataRadixUnresolved(batch.candidate().catalog_root);
 ASSERT_TRUE(batch.pendingBootstrapDataRoot());
 const auto afterData = pages.readMetadataRadixUnresolved(*batch.pendingBootstrapDataRoot());
 EXPECT_TRUE(afterCatalog.logical_children_enabled); EXPECT_TRUE(afterData.logical_children_enabled);
 EXPECT_FALSE(afterCatalog.logical_children[1U].isValid());
 EXPECT_EQ(afterCatalog.logical_children[2U], beforeCatalog.logical_children[2U]);
 EXPECT_FALSE(afterData.logical_children[0U].isValid());
 EXPECT_EQ(afterData.logical_children[2U], beforeData.logical_children[2U]);
 EXPECT_EQ(store->findCatalog(old, entries.front().segment_id).physical_slot, entries.front().physical_slot);
 EXPECT_EQ(store->findPageLocation(old, locations.front().page_id), locations.front());
}
