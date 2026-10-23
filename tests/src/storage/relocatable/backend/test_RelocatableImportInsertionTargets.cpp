#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <cerrno>
#ifdef _WIN32
#include <windows.h>
#endif
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
    EXPECT_FALSE(counters.contains("native_graph.commit.entity_insert_node_directory_pages"));
    ASSERT_TRUE(counters.contains("native_graph.write.entity_free_summary.candidate_pages"));
    EXPECT_EQ(counters.at("native_graph.write.entity_free_summary.candidate_pages").totalValue, 1);
    RecordProperty("cold_refresh_directory_entries", 0);
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

TEST_F(RelocatableStorageBackendTest, ColdInsertionUsesBoundedSummaryAcrossGrowingDirectories) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    const size_t capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_NODE);
    for (const size_t pages : {3U, 30U, 90U}) {
        std::vector<uint64_t> ids;
        {
            RelocatableStorageBackend populate(options); populate.open();
            ids = populate.createLogicalNodes(std::vector<LogicalGraphNodeCreateRequest>(pages * capacity, {{"Import"}, {}}));
            ASSERT_EQ(ids.size(), pages * capacity);
            populate.deleteLogicalNodes({{ids.front()}});
            populate.close();
        }
        RelocatableStorageBackend cold(options); cold.open();
        ImportTrace trace;
        ASSERT_EQ(cold.createLogicalNodes({{{"Import"}, {}}}).size(), 1U);
        const auto counters = graph::debug::PerfTrace::snapshotAndReset();
        EXPECT_FALSE(counters.contains("native_graph.commit.entity_insert_node_directory_pages"));
        ASSERT_TRUE(counters.contains("native_graph.write.entity_free_summary.candidate_pages"));
        EXPECT_EQ(counters.at("native_graph.write.entity_free_summary.candidate_pages").totalValue, 1);
        EXPECT_NO_THROW((void)cold.verifyIntegrity());
        RecordProperty("largest_added_page_count", static_cast<int>(pages));
        cold.close();
    }
}

TEST_F(RelocatableStorageBackendTest, ColdCompleteEmptySummaryAvoidsDirectoryCensus) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    {
        RelocatableStorageBackend populate(options); populate.open();
        const size_t capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_NODE);
        ASSERT_EQ(populate.createLogicalNodes(std::vector<LogicalGraphNodeCreateRequest>(capacity * 5U, {{"Import"}, {}})).size(), capacity * 5U);
        populate.close();
    }
    RelocatableStorageBackend cold(options); cold.open(); ImportTrace trace;
    ASSERT_EQ(cold.createLogicalNodes({{{"Import"}, {}}}).size(), 1U);
    const auto counters = graph::debug::PerfTrace::snapshotAndReset();
    EXPECT_FALSE(counters.contains("native_graph.commit.entity_insert_node_directory_pages"));
    EXPECT_FALSE(counters.contains("native_graph.write.entity_free_summary.candidate_pages"));
    EXPECT_NO_THROW((void)cold.verifyIntegrity());
}

TEST_F(RelocatableStorageBackendTest, FreeSummariesSeparateNodeAndEdgeSlotsAndSurviveExplicitRebuild) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
    const auto capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_EDGE);
    const auto edges = backend.createLogicalEdges(std::vector<LogicalGraphEdgeCreateRequest>(capacity * 2U, {nodes[0], nodes[1], "LINK", {}}));
    ASSERT_EQ(edges.size(), capacity * 2U);
    backend.deleteLogicalEdges({{edges.front()}});
    EXPECT_NO_THROW(backend.rebuildNativeEntityFreeSpaceSummaries());
    EXPECT_NO_THROW((void)backend.verifyIntegrity());
    backend.close(); backend.open(); ImportTrace trace;
    ASSERT_EQ(backend.createLogicalEdges({{nodes[0], nodes[1], "LINK", {}}}).size(), 1U);
    const auto counters = graph::debug::PerfTrace::snapshotAndReset();
    EXPECT_FALSE(counters.contains("native_graph.commit.entity_insert_edge_directory_pages"));
    ASSERT_TRUE(counters.contains("native_graph.write.entity_free_summary.candidate_pages"));
    EXPECT_EQ(counters.at("native_graph.write.entity_free_summary.candidate_pages").totalValue, 1);
    EXPECT_NO_THROW((void)backend.verifyIntegrity());
    size_t checkpoints = 0U;
    EXPECT_THROW(backend.rebuildNativeEntityFreeSpaceSummaries([&] {
        if (++checkpoints == 3U) throw std::runtime_error("cancel partial summary census");
    }), std::runtime_error);
    EXPECT_EQ(checkpoints, 3U);
    EXPECT_NO_THROW((void)backend.verifyIntegrity());
}

TEST_F(RelocatableStorageBackendTest, FreeSummaryRebuildFencesIndeterminatePublicationUntilReopen) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Import"}, {}}});
    bool wroteSuperblock = false, injected = false;
    {
        StorageIO::ScopedNativeWriteHookForTesting observe([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) {
            if (count == RSS_SUPERBLOCK_BYTES && offset >= 0 && uint64_t(offset) < RSS_FILE_BOOTSTRAP_BYTES)
                wroteSuperblock = true;
            return portable_pwrite(fd, bytes, count, offset);
        });
        StorageIO::ScopedNativeSyncHookForTesting fail([&](file_handle_t fd) {
            if (wroteSuperblock && !injected) {
                injected = true;
#ifdef _WIN32
                SetLastError(ERROR_WRITE_FAULT);
#else
                errno = EIO;
#endif
                return -1;
            }
            return portable_fsync(fd);
        });
        EXPECT_THROW(backend.rebuildNativeEntityFreeSpaceSummaries(), std::exception);
    }
    ASSERT_TRUE(injected);
    EXPECT_TRUE(backend.hasIndeterminateWriteOutcome());
    EXPECT_THROW(backend.rebuildNativeEntityFreeSpaceSummaries(), std::exception);
    backend.close(); backend.open();
    EXPECT_NO_THROW((void)backend.verifyIntegrity());
    EXPECT_EQ(backend.openLogicalGraphReadSnapshot()->readNode(nodes.front()).node_id, nodes.front());
}
