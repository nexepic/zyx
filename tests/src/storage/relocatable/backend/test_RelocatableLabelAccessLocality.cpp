/**
 * @file test_RelocatableLabelAccessLocality.cpp
 * @brief Snapshot-bound, bounded locality selection for native label access.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/storage/relocatable/entity/NativeMembershipAccessCost.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
using namespace relocatable_storage_backend_test;

TEST(NativeMembershipAccessCostTest, SameCardinalityUsesObservedPageLocalityAndKeepsDenseScans) {
    EXPECT_TRUE(NativeMembershipAccessCost::preferNodeCandidates(10000U, 4000U, 10000U, 64U, 1U));
    EXPECT_FALSE(NativeMembershipAccessCost::preferNodeCandidates(10000U, 4000U, 10000U, 64U, 64U));
    EXPECT_FALSE(NativeMembershipAccessCost::preferNodeCandidates(10000U, 10000U, 10000U, 64U, 1U));
    EXPECT_TRUE(NativeMembershipAccessCost::preferNodeCandidates(10000U, 4000U, 4000U, 64U, 64U));
}

TEST_F(RelocatableStorageBackendTest, LabelLocalityAvoidsUnrelatedNodesAndRetainsOldSnapshotMembership) {
    auto options = backendOptions(); options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
    auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
    std::vector<LogicalGraphNodeCreateRequest> requests(1024U, {{"All"}, {}});
    for (size_t i = 0U; i < 400U; ++i) requests[i].labels.push_back("Selected");
    const auto nodes = backend->createLogicalNodes(requests);
    auto old = backend->openLogicalGraphReadSnapshot();
    const auto collect = [](const LogicalGraphReadSnapshot &snapshot, const std::vector<std::string> &labels) {
        std::vector<uint64_t> result;
        snapshot.visitNodeRefsForLabelPredicate(1024U, labels, [&](const EntityRef &ref, bool match) {
            if (match) result.push_back(ref.entity_id); return true;
        });
        return result;
    };
    graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset();
    const auto selected = collect(*old, {"All", "Selected"});
    const auto trace = graph::debug::PerfTrace::snapshotAndReset();
    graph::debug::PerfTrace::setValueCollectionEnabled(false);
    EXPECT_EQ(selected, (std::vector<uint64_t>(nodes.begin(), nodes.begin() + 400U)));
    ASSERT_TRUE(trace.contains("native_graph.read.label_access.sample_rows"));
    EXPECT_LE(trace.at("native_graph.read.label_access.sample_rows").totalValue, 64);
    EXPECT_FALSE(trace.contains("native_graph.read.node_page_scan.records"));
    ASSERT_TRUE(trace.contains("native_graph.read.label_index_candidates"));
    EXPECT_EQ(trace.at("native_graph.read.label_index_candidates").totalValue, 400);
    ASSERT_TRUE(trace.contains("native_graph.read.records_decoded"));
    EXPECT_EQ(trace.at("native_graph.read.records_decoded").totalValue, 400);
    for (const auto &[name, counter] : trace)
        if (name.find("label_access.") != std::string::npos || name == "native_graph.read.records_decoded")
            RecordProperty(name, std::to_string(counter.totalValue));
    graph::debug::PerfTrace::setValueCollectionEnabled(true); graph::debug::PerfTrace::reset();
    EXPECT_EQ(collect(*old, {"All"}), nodes);
    const auto dense = graph::debug::PerfTrace::snapshotAndReset();
    graph::debug::PerfTrace::setValueCollectionEnabled(false);
    EXPECT_FALSE(dense.contains("native_graph.read.label_access.sample_rows"));
    EXPECT_FALSE(dense.contains("native_graph.read.label_index_candidates"));
    ASSERT_TRUE(dense.contains("native_graph.read.node_page_scan.records"));
    EXPECT_EQ(dense.at("native_graph.read.node_page_scan.records").totalValue, 1024);
    LogicalGraphMutationBatch replace;
    replace.node_label_replacements.push_back({
        {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodes.front()}, {"All"}});
    {
        auto transaction = backend->beginLogicalGraphTransaction(false);
        auto stagedLabels = replace;
        stagedLabels.node_label_replacements.push_back({
            {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodes[500U]},
            {"All", "Selected"}});
        // The backend transaction exposes its pinned base; the production
        // read-your-writes adapter applies staged label changes to that base.
        LogicalGraphTransactionOverlay overlay(*transaction);
        static_cast<void>(overlay.executeLogicalGraphMutationBatch(stagedLabels));
        auto overlaySnapshot = overlay.openLogicalGraphReadSnapshot();
        auto staged = collect(*overlaySnapshot, {"Selected"});
        std::ranges::sort(staged);
        std::vector<uint64_t> expected(nodes.begin() + 1U, nodes.begin() + 400U); expected.push_back(nodes[500U]);
        EXPECT_EQ(staged, expected);
        EXPECT_EQ(collect(*old, {"Selected"}), selected);
        overlaySnapshot.reset();
        transaction->rollback();
    }
    static_cast<void>(backend->executeLogicalGraphMutationBatch(replace));
    EXPECT_EQ(collect(*old, {"Selected"}), selected);
    auto current = backend->openLogicalGraphReadSnapshot();
    EXPECT_EQ(collect(*current, {"Selected"}), (std::vector<uint64_t>(nodes.begin() + 1U, nodes.begin() + 400U)));
    current.reset(); old.reset(); backend->close(); backend->open();
    current = backend->openLogicalGraphReadSnapshot();
    EXPECT_EQ(collect(*current, {"Selected"}).size(), 399U);
    current.reset(); backend->close();
}
