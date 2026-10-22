#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <atomic>
#include <cerrno>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace graph::storage;
using namespace relocatable_storage_backend_test;
namespace {
struct DeltaWork {
    int64_t full = 0, incremental = 0, frames = 0, refused = 0, member_full = 0, member_incremental = 0, member_frames = 0, ordinary_plans = 0, ordinary_images = 0, ordinary_member_plans = 0, ordinary_member_images = 0, mixed_skips = 0, fresh_pools = 0;
    static void observe(void *owner, std::string_view key, int64_t value, bool) noexcept {
        auto &w = *static_cast<DeltaWork *>(owner);
        if (key == "native_graph.write.index_images.new_pools_registered") w.fresh_pools += value;
        if (key == "native_graph.write.index_placement.mixed_pool_skipped") w.mixed_skips += value;
        if (key == "native_graph.write.ordinary_membership.incremental_plans") w.ordinary_member_plans += value;
        if (key == "native_graph.write.ordinary_membership.prepared_images") w.ordinary_member_images += value;
        if (key == "native_graph.write.ordinary_adjacency.incremental_plans") w.ordinary_plans += value;
        if (key == "native_graph.write.ordinary_adjacency.prepared_images") w.ordinary_images += value;
        if (key == "native_graph.write.streamed_membership.full_plans") w.member_full += value;
        if (key == "native_graph.write.streamed_membership.incremental_plans") w.member_incremental += value;
        if (key == "native_graph.write.streamed_membership.incremental_frames") w.member_frames += value;
        if (key == "native_graph.write.streamed_adjacency.bounds_refused") w.refused += value;
        if (key == "native_graph.write.streamed_adjacency.full_plans") w.full += value;
        if (key == "native_graph.write.streamed_adjacency.incremental_plans") w.incremental += value;
        if (key == "native_graph.write.streamed_adjacency.incremental_frames") w.frames += value;
    }
};
std::vector<uint64_t> append(RelocatableStorageBackend &backend, std::span<const uint64_t> sources,
    std::span<const uint64_t> targets, std::span<const LogicalGraphBulkEdgePropertyColumn> properties = {}) {
    auto tx = backend.beginLogicalGraphTransaction(false);
    auto *bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(tx.get());
    if (!bulk) throw std::runtime_error("missing bounded bulk capability");
    auto ids = bulk->appendPersistedBulkEdges({"R4_DELTA", sources, targets, properties});
    static_cast<void>(tx->commit()); return ids;
}
}
TEST_F(RelocatableStorageBackendTest, StreamedSmallAdjacencyDeltaPreservesOldPinsAndChangesOnlyBoundedPages) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}}); ASSERT_EQ(nodes.size(), 2U);
    std::vector<uint64_t> sources(8192, nodes[0]), targets(8192, nodes[1]);
    DeltaWork work; graph::debug::PerfTrace::ScopedObserver observer(&work, DeltaWork::observe);
    const auto initial = append(backend, sources, targets); ASSERT_EQ(initial.size(), 8192U);
    EXPECT_EQ(work.full, 1); auto old = backend.openLogicalGraphReadSnapshot();
    work = {};
    const std::array<uint64_t, 3> nextSources{nodes[0], nodes[0], nodes[1]}, nextTargets{nodes[1], nodes[0], nodes[0]};
    const auto added = append(backend, nextSources, nextTargets); ASSERT_EQ(added.size(), 3U);
    EXPECT_EQ(work.incremental, 1);
    EXPECT_EQ(work.full, 0); EXPECT_GT(work.frames, 0); EXPECT_LE(work.frames, 8);
    EXPECT_EQ(work.member_incremental, 1); EXPECT_EQ(work.member_full, 0);
    EXPECT_GT(work.member_frames, 0); EXPECT_LE(work.member_frames, 8);
    EXPECT_EQ(old->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 9000).edges.size(), 8192U);
    EXPECT_TRUE(old->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_INCOMING, {}, 9000).edges.empty());
    auto now = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(now->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 9000).edges.size(), 8194U);
    EXPECT_EQ(now->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_INCOMING, {}, 9000).edges.size(), 2U);
    EXPECT_EQ(now->readEdge(added[1]).source_node_id, now->readEdge(added[1]).target_node_id);
    const auto statistics = backend.readLogicalGraphCardinalityStatistics(); ASSERT_TRUE(statistics); EXPECT_EQ(statistics->edge_count, 8195U);
    old.reset(); now.reset(); backend.close(); backend.open();
    now = backend.openLogicalGraphReadSnapshot(); EXPECT_EQ(now->readEdge(added[0]).target_node_id, nodes[1]);
    now.reset(); EXPECT_NO_THROW((void)backend.verifyIntegrity()); backend.close();
}
TEST_F(RelocatableStorageBackendTest, StreamedAdjacencyShapeRejectsStaleHintsAndResumesAfterConfirmedRebuild) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
    const std::array<uint64_t, 1> sources{nodes[0]}, targets{nodes[1]};
    static_cast<void>(append(backend, sources, targets));
    static_cast<void>(backend.createLogicalNodes({{{"Unrelated"}, {}}}));
    DeltaWork work; graph::debug::PerfTrace::ScopedObserver observer(&work, DeltaWork::observe);
    static_cast<void>(append(backend, sources, targets)); EXPECT_EQ(work.full, 1); EXPECT_EQ(work.incremental, 0);
    EXPECT_EQ(work.member_full, 1); EXPECT_EQ(work.member_incremental, 0);
    work = {}; static_cast<void>(append(backend, sources, targets)); EXPECT_EQ(work.full, 0); EXPECT_EQ(work.incremental, 1);
    EXPECT_EQ(work.member_full, 0); EXPECT_EQ(work.member_incremental, 1);
    backend.close(); backend.open(); work = {};
    static_cast<void>(append(backend, sources, targets)); EXPECT_EQ(work.full, 1); EXPECT_EQ(work.incremental, 0);
    EXPECT_EQ(work.member_full, 1); EXPECT_EQ(work.member_incremental, 0);
    EXPECT_NO_THROW((void)backend.verifyIntegrity()); backend.close();
}
TEST_F(RelocatableStorageBackendTest, StreamedAdjacencyDeltaDiskFailureKeepsTopologyAndAllowsFreshRetry) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
    const std::array<uint64_t, 1> sources{nodes[0]}, targets{nodes[1]};
    const auto initial = append(backend, sources, targets);
    auto old = backend.openLogicalGraphReadSnapshot();
    auto tx = backend.beginLogicalGraphTransaction(false);
    auto *bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(tx.get()); ASSERT_NE(bulk, nullptr);
    const auto rejected = bulk->appendPersistedBulkEdges({"R4_DELTA", sources, targets, {}});
    std::atomic<bool> injected{false}; DeltaWork work;
    graph::debug::PerfTrace::ScopedObserver observer(&work, DeltaWork::observe);
    {
        StorageIO::ScopedNativeWriteHookForTesting failure([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) {
            bool selected = false;
            if (count == RSS_PAGE_SIZE && !injected.load()) try {
                const auto page = std::span(static_cast<const std::byte *>(bytes), count);
                const auto owner = PageOwnerHeaderCodec::decode(page.first(RSS_PAGE_OWNER_HEADER_BYTES));
                selected = owner.kind == PageOwnerKind::POK_INDEX && owner.owner_id == RSS_GLOBAL_ADJACENCY_INDEX_ID;
            } catch (const FormatException &) {}
            if (selected) {
                injected.store(true);
#ifdef _WIN32
                SetLastError(ERROR_DISK_FULL);
#else
                errno = ENOSPC;
#endif
                return static_cast<pwrite_ssize_t>(-1);
            }
            return portable_pwrite(fd, bytes, count, offset);
        });
        EXPECT_THROW((void)tx->commit(), std::system_error);
    }
    EXPECT_TRUE(injected.load()); EXPECT_EQ(work.incremental, 1);
    EXPECT_EQ(tx->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK); tx.reset();
    auto current = backend.openLogicalGraphReadSnapshot(); EXPECT_THROW((void)current->readEdge(rejected[0]), FormatException);
    EXPECT_EQ(current->readEdge(initial[0]), old->readEdge(initial[0])); current.reset();
    const auto retried = append(backend, sources, targets); EXPECT_GT(retried[0], rejected[0]);
    EXPECT_THROW((void)old->readEdge(retried[0]), FormatException); old.reset();
    EXPECT_NO_THROW((void)backend.verifyIntegrity()); backend.close();
}

namespace {
class StreamedAdjacencyBootstrapFaultTest : public RelocatableStorageBackendTest,
    public testing::WithParamInterface<bool> {};
}
TEST_P(StreamedAdjacencyBootstrapFaultTest, IncrementalPublicationRecoversOneCompleteGeneration) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
    const std::array<uint64_t, 1> sources{nodes[0]}, targets{nodes[1]};
    const auto initial = append(backend, sources, targets); auto old = backend.openLogicalGraphReadSnapshot();
    auto tx = backend.beginLogicalGraphTransaction(false);
    auto *bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(tx.get()); ASSERT_NE(bulk, nullptr);
    const auto pending = bulk->appendPersistedBulkEdges({"R4_DELTA", sources, targets, {}});
    struct Armed { DeltaWork work; std::atomic<bool> ready{false}; } armed;
    std::atomic<bool> injected{false};
    graph::debug::PerfTrace::ScopedObserver observer(&armed, [](void *owner, std::string_view key, int64_t value, bool duration) noexcept {
        auto &a = *static_cast<Armed *>(owner); DeltaWork::observe(&a.work, key, value, duration);
        if (key == "native_graph.write.streamed_adjacency.incremental_plans" && value) a.ready.store(true);
    });
    {
        StorageIO::ScopedNativeWriteHookForTesting failure([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) {
            if (armed.ready.load() && offset >= 0 && uint64_t(offset) < 2U * RSS_SUPERBLOCK_BYTES) {
                if (!injected.exchange(true)) {
                    if (GetParam()) static_cast<void>(portable_pwrite(fd, bytes, count, offset));
                    else return portable_pwrite(fd, bytes, RSS_SUPERBLOCK_HEADER_CRC_OFFSET, offset);
                }
#ifdef _WIN32
                SetLastError(ERROR_DISK_FULL);
#else
                errno = ENOSPC;
#endif
                return static_cast<pwrite_ssize_t>(-1);
            }
            return portable_pwrite(fd, bytes, count, offset);
        });
        EXPECT_THROW((void)tx->commit(), std::system_error);
    }
    EXPECT_TRUE(injected.load()); EXPECT_EQ(armed.work.incremental, 1);
    EXPECT_EQ(tx->state(), LogicalGraphTransactionState::LGTTS_INDETERMINATE);
    EXPECT_EQ(old->readEdge(initial[0]).target_node_id, nodes[1]);
    EXPECT_THROW((void)old->readEdge(pending[0]), FormatException);
    old.reset(); tx.reset(); backend.close();
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend recovered(options); recovered.open(); auto snapshot = recovered.openLogicalGraphReadSnapshot();
    EXPECT_EQ(snapshot->readEdge(initial[0]).source_node_id, nodes[0]);
    if (GetParam()) EXPECT_EQ(snapshot->readEdge(pending[0]).target_node_id, nodes[1]);
    else EXPECT_THROW((void)snapshot->readEdge(pending[0]), FormatException);
    const auto counts = recovered.readLogicalGraphCardinalityStatistics(); ASSERT_TRUE(counts);
    EXPECT_EQ(counts->edge_count, GetParam() ? 2U : 1U);
    const std::array<std::string, 1> types{"R4_DELTA"};
    const auto planning = recovered.readLogicalPlanningStatistics({}, types);
    ASSERT_EQ(planning.edge_type_counts.size(), 1U); EXPECT_EQ(planning.edge_type_counts[0], counts->edge_count);
    snapshot.reset(); recovered.close();
    // Physical read-only reopen must not reclaim private scratch left by an
    // indeterminate commit. A writable open performs the existing recovery.
    options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend cleaned(options); cleaned.open();
    EXPECT_NO_THROW((void)cleaned.verifyIntegrity()); cleaned.close();
}
INSTANTIATE_TEST_SUITE_P(PartialAndCompleteInactiveSlot, StreamedAdjacencyBootstrapFaultTest, testing::Values(false, true));

TEST_F(RelocatableStorageBackendTest, StreamedAdjacencyDeltaEnforcesFinalEntryBoundBeforePublication) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    options.maximum_bulk_edge_index_entries = 17000U;
    options.maximum_bulk_edge_transaction_records = 8192U;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
    std::vector<uint64_t> sources(8192, nodes[0]), targets(8192, nodes[1]);
    const auto initial = append(backend, sources, targets); auto old = backend.openLogicalGraphReadSnapshot();
    sources.resize(1000); targets.resize(1000);
    DeltaWork work; graph::debug::PerfTrace::ScopedObserver observer(&work, DeltaWork::observe);
    EXPECT_THROW((void)append(backend, sources, targets), FormatException);
    EXPECT_EQ(work.refused, 1); EXPECT_EQ(work.full, 0); EXPECT_EQ(work.frames, 0);
    const auto statistics = backend.readLogicalGraphCardinalityStatistics(); ASSERT_TRUE(statistics);
    EXPECT_EQ(statistics->edge_count, initial.size());
    auto now = backend.openLogicalGraphReadSnapshot(); EXPECT_EQ(now->readEdge(initial.back()), old->readEdge(initial.back()));
    now.reset(); old.reset(); EXPECT_NO_THROW((void)backend.verifyIntegrity()); backend.close();
}

TEST_F(RelocatableStorageBackendTest, StreamedPropertyBatchesReuseAdjacencyAndPublishExactTypeCounts) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
    std::vector<uint64_t> sources(1000, nodes[0]), targets(1000, nodes[1]);
    std::vector<PropertyValue> weights(1000, PropertyValue(int64_t(7)));
    const std::array<LogicalGraphBulkEdgePropertyColumn, 1> properties{{{"weight", weights}}};
    const auto initial = append(backend, sources, targets, properties); auto old = backend.openLogicalGraphReadSnapshot();
    DeltaWork work; graph::debug::PerfTrace::ScopedObserver observer(&work, DeltaWork::observe);
    const auto added = append(backend, sources, targets, properties);
    EXPECT_EQ(work.incremental, 1); EXPECT_EQ(work.full, 0);
    EXPECT_EQ(work.member_incremental, 1); EXPECT_EQ(work.member_full, 0);
    auto now = backend.openLogicalGraphReadSnapshot();
    const auto edge = now->readEdge(added.front()); ASSERT_EQ(edge.properties.size(), 1U);
    EXPECT_EQ(edge.properties.front().key, "weight"); EXPECT_EQ(edge.properties.front().value, PropertyValue(int64_t(7)));
    EXPECT_THROW((void)old->readEdge(added.front()), FormatException);
    EXPECT_EQ(now->readEdge(initial.front()), old->readEdge(initial.front()));
    const auto statistics = backend.readLogicalGraphCardinalityStatistics(); ASSERT_TRUE(statistics);
    EXPECT_EQ(statistics->edge_count, 2000U);
    const std::array<std::string, 1> types{"R4_DELTA"};
    const auto planning = backend.readLogicalPlanningStatistics({}, types);
    ASSERT_EQ(planning.edge_type_counts.size(), 1U); EXPECT_EQ(planning.edge_type_counts[0], 2000U);
    now.reset(); old.reset(); EXPECT_NO_THROW((void)backend.verifyIntegrity()); backend.close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryAdjacencyUsesBoundedOwnedImagesAndPreservesOldPins) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
    std::vector<LogicalGraphEdgeCreateRequest> warm(512, {nodes[0], nodes[1], "ORDINARY", {}});
    const auto initial = backend.createLogicalEdges(warm); auto old = backend.openLogicalGraphReadSnapshot();
    DeltaWork work; graph::debug::PerfTrace::ScopedObserver observer(&work, DeltaWork::observe);
    const auto pending = backend.createLogicalEdges({{nodes[0], nodes[1], "ORDINARY", {{"weight", PropertyValue(int64_t(7))}}},
        {nodes[0], nodes[0], "ORDINARY", {}}, {nodes[1], nodes[0], "ORDINARY", {}}});
    EXPECT_THROW((void)old->readEdge(pending[0]), FormatException);
    EXPECT_GT(work.ordinary_plans, 0); EXPECT_GT(work.ordinary_images, 0); EXPECT_LE(work.ordinary_images, 8);
    EXPECT_GT(work.ordinary_member_plans, 0); EXPECT_GT(work.ordinary_member_images, 0); EXPECT_LE(work.ordinary_member_images, 8);
    auto now = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(now->readEdge(initial.front()), old->readEdge(initial.front()));
    EXPECT_EQ(now->readEdge(pending[0]).properties.front().value, PropertyValue(int64_t(7)));
    EXPECT_EQ(now->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1000).edges.size(), 514U);
    now.reset(); old.reset(); backend.close(); backend.open();
    EXPECT_NO_THROW((void)backend.verifyIntegrity()); backend.close();
}

TEST_P(StreamedAdjacencyBootstrapFaultTest, OrdinaryImagePublicationRecoversOneCompleteGeneration) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
    const auto initial = backend.createLogicalEdges({{nodes[0], nodes[1], "ORDINARY_FAULT", {}}});
    auto old = backend.openLogicalGraphReadSnapshot();
    struct Armed { DeltaWork work; bool ready = false; } armed;
    bool injected = false;
    graph::debug::PerfTrace::ScopedObserver observer(&armed, [](void *owner, std::string_view key, int64_t value, bool duration) noexcept {
        auto &a = *static_cast<Armed *>(owner); DeltaWork::observe(&a.work, key, value, duration);
        if (key == "native_graph.write.ordinary_adjacency.incremental_plans" && value) a.ready = true;
    });
    {
        StorageIO::ScopedNativeWriteHookForTesting failure([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) {
            if (armed.ready && offset >= 0 && uint64_t(offset) < 2U * RSS_SUPERBLOCK_BYTES) {
                if (!injected) {
                    injected = true;
                    if (GetParam()) static_cast<void>(portable_pwrite(fd, bytes, count, offset));
                    else return portable_pwrite(fd, bytes, RSS_SUPERBLOCK_HEADER_CRC_OFFSET, offset);
                }
#ifdef _WIN32
                SetLastError(ERROR_DISK_FULL);
#else
                errno = ENOSPC;
#endif
                return static_cast<pwrite_ssize_t>(-1);
            }
            return portable_pwrite(fd, bytes, count, offset);
        });
        EXPECT_THROW((void)backend.createLogicalEdges({{nodes[0], nodes[1], "ORDINARY_FAULT", {}}}), std::system_error);
    }
    EXPECT_TRUE(injected); EXPECT_GT(armed.work.ordinary_plans, 0);
    EXPECT_EQ(old->readEdge(initial.front()).target_node_id, nodes[1]); old.reset(); backend.close();
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend recovered(options); recovered.open();
    const auto counts = recovered.readLogicalGraphCardinalityStatistics(); ASSERT_TRUE(counts);
    EXPECT_EQ(counts->edge_count, GetParam() ? 2U : 1U);
    auto now = recovered.openLogicalGraphReadSnapshot();
    EXPECT_EQ(now->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 10).edges.size(), counts->edge_count);
    now.reset(); recovered.close(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend cleaned(options); cleaned.open(); EXPECT_NO_THROW((void)cleaned.verifyIntegrity()); cleaned.close();
}

TEST_F(RelocatableStorageBackendTest, OrdinaryIndexImagesRegisterSharedBatchFreshPoolsOnlyOnce) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
    const std::vector<LogicalGraphEdgeCreateRequest> batch(1000, {nodes[0], nodes[1], "FRESH_BATCH", {}});
    static_cast<void>(backend.createLogicalEdges(batch)); auto old = backend.openLogicalGraphReadSnapshot();
    DeltaWork work; graph::debug::PerfTrace::ScopedObserver observer(&work, DeltaWork::observe);
    for (size_t round = 0; round < 5; ++round) {
        EXPECT_EQ(backend.createLogicalEdges(batch).size(), 1000U);
        const auto counts = backend.readLogicalGraphCardinalityStatistics(); ASSERT_TRUE(counts);
        EXPECT_EQ(counts->edge_count, (round + 2U) * 1000U);
        EXPECT_EQ(old->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1100).edges.size(), 1000U);
    }
    EXPECT_GE(work.ordinary_plans, 5); EXPECT_GE(work.ordinary_member_plans, 5); EXPECT_GT(work.fresh_pools, 0);
    old.reset(); EXPECT_NO_THROW((void)backend.verifyIntegrity()); backend.close(); backend.open();
    const auto counts = backend.readLogicalGraphCardinalityStatistics(); ASSERT_TRUE(counts); EXPECT_EQ(counts->edge_count, 6000U);
    EXPECT_NO_THROW((void)backend.verifyIntegrity()); backend.close();
}

TEST_P(StreamedAdjacencyBootstrapFaultTest, OrdinaryDeletionImagesRecoverOneCompleteGeneration) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
    const auto initial = backend.createLogicalEdges(std::vector<LogicalGraphEdgeCreateRequest>(3, {nodes[0], nodes[1], "ORDINARY_FAULT", {}}));
    auto old = backend.openLogicalGraphReadSnapshot();
    struct Armed { DeltaWork work; bool ready = false; } armed;
    bool injected = false;
    graph::debug::PerfTrace::ScopedObserver observer(&armed, [](void *owner, std::string_view key, int64_t value, bool duration) noexcept {
        auto &a = *static_cast<Armed *>(owner); DeltaWork::observe(&a.work, key, value, duration);
        if (key == "native_graph.write.ordinary_adjacency.incremental_plans" && value) a.ready = true;
    });
    {
        StorageIO::ScopedNativeWriteHookForTesting failure([&](file_handle_t fd, const void *bytes, size_t count, pwrite_off_t offset) {
            if (armed.ready && offset >= 0 && uint64_t(offset) < 2U * RSS_SUPERBLOCK_BYTES) {
                if (!injected) {
                    injected = true;
                    if (GetParam()) static_cast<void>(portable_pwrite(fd, bytes, count, offset));
                    else return portable_pwrite(fd, bytes, RSS_SUPERBLOCK_HEADER_CRC_OFFSET, offset);
                }
#ifdef _WIN32
                SetLastError(ERROR_DISK_FULL);
#else
                errno = ENOSPC;
#endif
                return static_cast<pwrite_ssize_t>(-1);
            }
            return portable_pwrite(fd, bytes, count, offset);
        });
        EXPECT_THROW(backend.deleteLogicalEdges({{initial.front()}}), std::system_error);
    }
    EXPECT_TRUE(injected); EXPECT_GT(armed.work.ordinary_plans, 0);
    EXPECT_EQ(old->readEdge(initial.front()).target_node_id, nodes[1]); old.reset(); backend.close();
    options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
    RelocatableStorageBackend recovered(options); recovered.open();
    const auto counts = recovered.readLogicalGraphCardinalityStatistics(); ASSERT_TRUE(counts);
    EXPECT_EQ(counts->edge_count, GetParam() ? 2U : 3U);
    auto now = recovered.openLogicalGraphReadSnapshot();
    EXPECT_EQ(now->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 10).edges.size(), counts->edge_count);
    if (GetParam()) EXPECT_THROW((void)now->readEdge(initial.front()), FormatException);
    else EXPECT_EQ(now->readEdge(initial.front()).target_node_id, nodes[1]);
    now.reset(); recovered.close(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend cleaned(options); cleaned.open(); EXPECT_NO_THROW((void)cleaned.verifyIntegrity()); cleaned.close();
}


TEST_F(RelocatableStorageBackendTest, OrdinaryDeletionImagesPreserveBothDirectionsAndTerminalRoots) {
    materializeNativeDatabase(); auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
    RelocatableStorageBackend backend(options); backend.open();
    const auto nodes = backend.createLogicalNodes({{{"Source"}, {}}, {{"Target"}, {}}});
    const auto initial = backend.createLogicalEdges(std::vector<LogicalGraphEdgeCreateRequest>(512,
        {nodes[0], nodes[1], "DELETE_DELTA", {{"weight", PropertyValue(7)}}}));
    const auto self = backend.createLogicalEdges({{nodes[0], nodes[0], "LAST_TYPE", {}}});
    auto old = backend.openLogicalGraphReadSnapshot();
    DeltaWork work; graph::debug::PerfTrace::ScopedObserver observer(&work, DeltaWork::observe);
    backend.deleteLogicalEdges({{initial[255]}});
    EXPECT_EQ(work.ordinary_plans, 1); EXPECT_EQ(work.ordinary_member_plans, 1);
    EXPECT_GT(work.ordinary_images, 0); EXPECT_LE(work.ordinary_images, 8);
    EXPECT_GT(work.ordinary_member_images, 0); EXPECT_LE(work.ordinary_member_images, 8);
    backend.deleteLogicalEdges({{self.front()}}); // last type entry, nonempty global root
    auto now = backend.openLogicalGraphReadSnapshot();
    EXPECT_EQ(now->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1000).edges.size(), 511U);
    EXPECT_EQ(now->scanAdjacentEdges(nodes[1], LogicalGraphReadDirection::LGRD_INCOMING, {}, 1000).edges.size(), 511U);
    EXPECT_EQ(old->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1000).edges.size(), 513U);
    EXPECT_EQ(old->readEdge(initial[255]).target_node_id, nodes[1]);
    EXPECT_THROW((void)now->readEdge(initial[255]), FormatException);
    now.reset();
    std::vector<LogicalGraphEdgeDeleteRequest> rest;
    for (auto id : initial) if (id != initial[255]) rest.push_back({id});
    backend.deleteLogicalEdges(rest); // last global entry uses bounded retirement
    const auto counts = backend.readLogicalGraphCardinalityStatistics(); ASSERT_TRUE(counts); EXPECT_EQ(counts->edge_count, 0U);
    EXPECT_EQ(old->readEdge(initial.front()).target_node_id, nodes[1]);
    old.reset(); EXPECT_NO_THROW((void)backend.verifyIntegrity()); backend.close(); backend.open();
    now = backend.openLogicalGraphReadSnapshot();
    EXPECT_TRUE(now->scanAdjacentEdges(nodes[0], LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1000).edges.empty());
    now.reset(); EXPECT_NO_THROW((void)backend.verifyIntegrity()); backend.close();
}
