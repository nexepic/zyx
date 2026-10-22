#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/debug/PerfTrace.hpp"
using namespace relocatable_storage_backend_test;
namespace {
struct ImportTrace {
    bool enabled = graph::debug::PerfTrace::areValuesEnabled();
    ImportTrace() { graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true); }
    ~ImportTrace() { graph::debug::PerfTrace::setValueCollectionEnabled(enabled); }
};
}
TEST_F(RelocatableStorageBackendTest, FullEntityPagesRemainKnownFullAcrossImportBatches) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const size_t capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_NODE);
    std::vector<LogicalGraphNodeCreateRequest> requests(capacity, {{"Import"}, {}});
    ASSERT_EQ(backend.createLogicalNodes(requests).size(), capacity);
    // Each batch ends on a full page, so the target vector is empty. It is still
    // complete knowledge under the unchanged/locally maintained COW root.
    ImportTrace trace;
    int64_t cacheHits = 0;
    for (size_t batch = 0; batch < 3; ++batch) {
        ASSERT_EQ(backend.createLogicalNodes(requests).size(), capacity);
        const auto counters = graph::debug::PerfTrace::snapshotAndReset();
        EXPECT_FALSE(counters.contains("native_graph.commit.entity_insert_node_directory_pages"));
        EXPECT_FALSE(counters.contains("native_metadata.external_source_refresh"));
        EXPECT_FALSE(counters.contains("native_metadata.reclaim.full_traversals"));
        ASSERT_TRUE(counters.contains("native_graph.commit.entity_insert_target_cache_hits"));
        cacheHits += counters.at("native_graph.commit.entity_insert_target_cache_hits").totalValue;
    }
    RecordProperty("batch_rows", static_cast<int>(capacity));
    RecordProperty("cache_hits_across_three_batches", static_cast<int>(cacheHits));
    RecordProperty("entity_directory_entries_visited", 0);
    RecordProperty("full_metadata_traversals", 0);
    EXPECT_NO_THROW((void)backend.verifyIntegrity());
}
TEST_F(RelocatableStorageBackendTest, AnotherHandleDeletionInvalidatesNegativeInsertionKnowledge) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend reader(options), writer(options); reader.open(); writer.open();
    const size_t capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_NODE);
    const size_t initialCount = capacity * 4U;
    const auto ids = reader.createLogicalNodes(std::vector<LogicalGraphNodeCreateRequest>(initialCount, {{"Import"}, {}}));
    ASSERT_EQ(ids.size(), initialCount);
    auto old = reader.openLogicalGraphReadSnapshot();
    ASSERT_NO_THROW(writer.deleteLogicalNodes({{ids.front()}}));
    ImportTrace trace;
    std::vector<uint64_t> created; ASSERT_NO_THROW(created = reader.createLogicalNodes({{{"Import"}, {}}})); ASSERT_EQ(created.size(), 1U);
    const auto counters = graph::debug::PerfTrace::snapshotAndReset();
    ASSERT_TRUE(counters.contains("native_graph.commit.entity_insert_node_directory_pages"));
    EXPECT_EQ(counters.at("native_graph.commit.entity_insert_node_directory_pages").totalValue, initialCount - 1U);
    RecordProperty("cold_refresh_directory_entries", static_cast<int>(counters.at("native_graph.commit.entity_insert_node_directory_pages").totalValue));
    RecordProperty("protected_old_rows", static_cast<int>(initialCount));
    RecordProperty("alternating_publications", 20);
    for (size_t publication = 0; publication < 20U; ++publication) {
        auto &nextWriter = publication % 2U ? reader : writer;
        ASSERT_NO_THROW((void)nextWriter.createLogicalNodes({{{"Import"}, {}}}));
    }
    // Evict the old view's bounded record cache before checking the removed
    // entity, so this assertion still needs its protected physical frames.
    for (const auto id : ids) ASSERT_NO_THROW(EXPECT_EQ(old->readNode(id).node_id, id));
    ASSERT_NO_THROW(EXPECT_EQ(old->readNode(ids.front()).node_id, ids.front()));
    old.reset(); EXPECT_NO_THROW((void)reader.verifyIntegrity());
    reader.close(); writer.close();
    RelocatableStorageBackend reopened(options); reopened.open();
    EXPECT_EQ(reopened.openLogicalGraphReadSnapshot()->readNode(created.front()).node_id, created.front());
    EXPECT_THROW((void)reopened.openLogicalGraphReadSnapshot()->readNode(ids.front()), FormatException);
}
