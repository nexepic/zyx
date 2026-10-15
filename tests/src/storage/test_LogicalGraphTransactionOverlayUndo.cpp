/**
 * @file test_LogicalGraphTransactionOverlayUndo.cpp
 * @brief Statement-local owner undo and failure recovery for logical overlays.
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <gtest/gtest.h>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "graph/storage/LogicalGraphTransactionMemoryProvider.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include "storage/LogicalGraphTransactionOverlayTestFixture.hpp"

namespace graph::storage {
namespace {
using namespace transaction_overlay_test;

LogicalGraphMutationEntityReference nodeReference(uint64_t id) {
	return {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
}

LogicalGraphMutationEntityReference edgeReference(uint64_t id) {
	return {LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id};
}

TEST(LogicalGraphTransactionOverlayUndoTest, PrivateReadsOfRetainedWideFieldsAdmitCopiesBeforeAllocating) {
    InMemoryLogicalTransaction transaction;
    LogicalGraphTransactionOverlay overlay(transaction);
    const PropertyValue expected(std::string(65536U, 's'));
    overlay.replaceLogicalNodeProperties({{1U, {{"payload", expected}}}});
    overlay.replaceLogicalEdgeProperties({{10U, {{"payload", expected}}}});
    auto snapshot = overlay.openLogicalGraphReadSnapshot();
    const auto fence = snapshot->mutationFence();
    auto small = std::make_shared<query::execution::MaterializedByteBudget>(4096U);
    {
        detail::PropertyDecodeWorkspace workspace(small);
        EXPECT_THROW(static_cast<void>(snapshot->readNodeShaped(1U, {.include_labels = true, .all_properties = true})),
            query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(small->used(), 0U);
    {
        detail::PropertyDecodeWorkspace workspace(small);
        EXPECT_THROW(static_cast<void>(snapshot->readEdgeShaped(10U, {.include_type = true, .all_properties = true})),
            query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(small->used(), 0U);
    auto enough = std::make_shared<query::execution::MaterializedByteBudget>(1048576U);
    {
        detail::PropertyDecodeWorkspace workspace(enough);
        const auto node = snapshot->readNodeShaped(1U, {.include_labels = true, .all_properties = true});
        const auto edge = snapshot->readEdgeShaped(10U, {.include_type = true, .all_properties = true});
        ASSERT_EQ(node.properties.size(), 1U); ASSERT_EQ(edge.properties.size(), 1U);
        EXPECT_EQ(node.properties[0].value, expected); EXPECT_EQ(edge.properties[0].value, expected);
        const std::array<uint64_t, 1U> nodes{1U}, edges{10U};
        const auto nodeBatch = snapshot->readNodesShapedSorted(nodes, {.all_properties = true});
        const auto edgeBatch = snapshot->readEdgesShapedSorted(edges, {.all_properties = true});
        EXPECT_EQ(nodeBatch[0].properties[0].value, expected);
        EXPECT_EQ(edgeBatch[0].properties[0].value, expected);
        EXPECT_GE(enough->used(), 4U * 65537U);
    }
    EXPECT_EQ(enough->used(), 0U);
    EXPECT_EQ(snapshot->mutationFence(), fence);
    EXPECT_EQ(snapshot->readNodeShaped(1U, {.all_properties = true}).properties[0].value, expected);
}

class BudgetedUndoTransaction final : public LogicalGraphTransaction,
                                      public LogicalGraphTransactionMemoryProvider {
public:
    std::shared_ptr<query::execution::MaterializedByteBudget> budget =
        std::make_shared<query::execution::MaterializedByteBudget>(262144U);
    InMemoryLogicalTransaction inner;
    [[nodiscard]] std::shared_ptr<std::pmr::memory_resource> retainedWriteMemory() const override { return budget; }
    [[nodiscard]] LogicalGraphTransactionState state() const override { return inner.state(); }
    [[nodiscard]] bool isReadOnly() const override { return false; }
    [[nodiscard]] LogicalGraphReadSnapshot &readSnapshot() override { return inner.readSnapshot(); }
    [[nodiscard]] LogicalGraphMutationBatchResult previewMutationBatch(const LogicalGraphMutationBatch &batch) const override {
        return inner.previewMutationBatch(batch);
    }
    [[nodiscard]] LogicalGraphMutationBatchResult stageMutationBatch(LogicalGraphMutationBatch batch) override {
        return inner.stageMutationBatch(std::move(batch));
    }
    [[nodiscard]] LogicalGraphMutationBatchResult commit() override { return inner.commit(); }
    void rollback() override { inner.rollback(); }
};

TEST(LogicalGraphTransactionOverlayUndoTest, NestedUndoAdmissionFailurePreservesTheVisibleOwnerAndRefundsItsDraft) {
    BudgetedUndoTransaction transaction;
    {
        LogicalGraphTransactionOverlay overlay(transaction);
        const PropertyValue expected(std::string(65536U, 'a'));
        overlay.replaceLogicalNodeProperties({{1U, {{"payload", expected}}}});
        const auto retained = transaction.budget->used();
        EXPECT_GE(retained, 65536U);
        {
            query::execution::MaterializedByteReservation pressure(transaction.budget.get());
            ASSERT_GT(transaction.budget->available(), 1024U);
            pressure.set(transaction.budget->available() - 1024U);
            EXPECT_THROW(overlay.replaceLogicalNodeProperties({{1U, {{"payload", PropertyValue(9)}}}}),
                query::execution::MaterializedByteLimitExceeded);
            EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->readNodeProperty(1U, "payload"), expected);
        }
        EXPECT_EQ(transaction.budget->used(), retained);
        overlay.replaceLogicalNodeProperties({{1U, {{"payload", PropertyValue(9)}}}});
        EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->readNodeProperty(1U, "payload"), PropertyValue(9));
        EXPECT_LT(transaction.budget->used(), retained);
    }
    EXPECT_EQ(transaction.budget->used(), 0U);
}

TEST(LogicalGraphTransactionOverlayUndoTest, RejectedStageRestoresNestedNodeAndEdgeChargesWithoutCopyingDuringUndo) {
    BudgetedUndoTransaction transaction;
    {
        LogicalGraphTransactionOverlay overlay(transaction);
        const PropertyValue expected(std::string(32768U, 'v'));
        overlay.replaceLogicalNodeProperties({{1U, {{"payload", expected}}}});
        overlay.replaceLogicalEdgeProperties({{10U, {{"payload", expected}}}});
        const auto retained = transaction.budget->used();
        transaction.inner.setRejectStage(true);
        LogicalGraphMutationBatch candidate;
        candidate.property_replacements = {{nodeReference(1U), {{"payload", PropertyValue(std::string(49152U, 'n'))}}},
            {edgeReference(10U), {{"payload", PropertyValue(std::string(49152U, 'e'))}}}};
        EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(candidate)), std::runtime_error);
        EXPECT_EQ(transaction.budget->used(), retained);
        auto snapshot = overlay.openLogicalGraphReadSnapshot();
        EXPECT_EQ(snapshot->readNodeProperty(1U, "payload"), expected);
        EXPECT_EQ(snapshot->readEdgeProperty(10U, "payload"), expected);
        transaction.inner.setRejectStage(false);
        EXPECT_NO_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(candidate)));
    }
    EXPECT_EQ(transaction.budget->used(), 0U);
}

TEST(LogicalGraphTransactionOverlayUndoTest, FailedStageRestoresFirstStateForRepeatedTargetsAndSeparateEntityKinds) {
	InMemoryLogicalTransaction transaction;
	auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
	base.edges.emplace(1U, LogicalGraphEdge{1U, 1U, 2U, "SAME_ID"});
	LogicalGraphTransactionOverlay overlay(transaction);
	overlay.replaceLogicalNodeProperties({{1U, {{"before", PropertyValue(7)}}}});
	overlay.replaceLogicalEdgeProperties({{1U, {{"before", PropertyValue(8)}}}});
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto fence = snapshot->mutationFence();
	LogicalGraphMutationBatch failed;
	failed.node_label_replacements = {{nodeReference(1U), {"Changed"}}};
	failed.property_replacements = {
		{nodeReference(1U), {{"after", PropertyValue(9)}}},
		{edgeReference(1U), {{"after", PropertyValue(10)}}},
		{nodeReference(1U), {{"after", PropertyValue(11)}}}};
	failed.edge_deletes = {{1U}};
	transaction.setRejectStage(true);
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(failed)), std::runtime_error);
	EXPECT_EQ(snapshot->mutationFence(), fence);
	const auto node = snapshot->readNodeShaped(1U, {.include_labels = true, .all_properties = true});
	ASSERT_EQ(node.properties.size(), 1U);
	EXPECT_EQ(node.properties.front().key, "before");
	EXPECT_EQ(node.properties.front().value, PropertyValue(7));
	EXPECT_EQ(node.labels, (std::vector<std::string>{"Base"}));
	const auto edge = snapshot->readEdgeShaped(1U, {.all_properties = true});
	ASSERT_EQ(edge.properties.size(), 1U);
	EXPECT_EQ(edge.properties.front().value, PropertyValue(8));
	transaction.setRejectStage(false);
	EXPECT_NO_THROW(overlay.replaceLogicalNodeProperties({{1U, {{"retry", PropertyValue(12)}}}}));
	EXPECT_NE(snapshot->mutationFence(), fence);
}

TEST(LogicalGraphTransactionOverlayUndoTest, FailedStageRemovesCreatedAndNewlyMaterializedOwnersAndAllowsIdentityRetry) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto fence = snapshot->mutationFence();
	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"New"}, {{"value", PropertyValue(1)}}}};
	batch.edge_creates = {{nodeReference(1U),
		{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U}, "NEW", {}}};
	batch.property_replacements = {{nodeReference(2U), {{"staged", PropertyValue(2)}}},
		{edgeReference(10U), {{"staged", PropertyValue(3)}}}};
	transaction.setRejectStage(true);
	EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(batch)), std::runtime_error);
	EXPECT_EQ(snapshot->mutationFence(), fence);
	EXPECT_FALSE(snapshot->containsNode(100U));
	EXPECT_TRUE(snapshot->readNodeShaped(2U, {.all_properties = true}).properties.empty());
	EXPECT_TRUE(snapshot->readEdgeShaped(10U, {.all_properties = true}).properties.empty());
	// Absence delegates to this fixture's map-backed snapshot, which reports
	// missing IDs with out_of_range; no overlay-created Edge may survive.
	EXPECT_THROW(static_cast<void>(snapshot->readEdgeShaped(200U, {})), std::out_of_range);
	transaction.setRejectStage(false);
	const auto retry = overlay.executeLogicalGraphMutationBatch(batch);
	EXPECT_EQ(retry.created_node_ids, (std::vector<uint64_t>{100U}));
	EXPECT_EQ(retry.created_edge_ids, (std::vector<uint64_t>{200U}));
	EXPECT_TRUE(snapshot->containsNode(100U));
}

TEST(LogicalGraphTransactionOverlayUndoTest, DetachBoundFailureRestoresPersistedAndCreatedIncidentEdges) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	overlay.replaceLogicalEdgeProperties({{10U, {{"old", PropertyValue(4)}}}});
	const auto created = overlay.createLogicalEdges({{1U, 2U, "CREATED", {}}}).front();
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto fence = snapshot->mutationFence();
	EXPECT_THROW(overlay.deleteLogicalNodes({{1U, true, 1U}}), std::runtime_error);
	EXPECT_EQ(snapshot->mutationFence(), fence);
	EXPECT_TRUE(snapshot->containsNode(1U));
	EXPECT_EQ(snapshot->readEdgeShaped(10U, {.all_properties = true}).properties.front().value, PropertyValue(4));
	EXPECT_EQ(snapshot->readEdgeShaped(created, {}).edge_id, created);
	EXPECT_NO_THROW(overlay.deleteLogicalNodes({{1U, true, 2U}}));
	EXPECT_FALSE(snapshot->containsNode(1U));
}

TEST(LogicalGraphTransactionOverlayUndoTest, StageFailureAfterDetachRestoresBothIncidentSourcesAndCreatedSelfLoop) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	const auto created = overlay.createLogicalEdges({{1U, 1U, "SELF", {}}}).front();
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	const auto fence = snapshot->mutationFence();
	transaction.setRejectStage(true);
	EXPECT_THROW(overlay.deleteLogicalNodes({{1U, true, 2U}}), std::runtime_error);
	EXPECT_TRUE(snapshot->containsNode(1U));
	EXPECT_EQ(snapshot->mutationFence(), fence);
	EXPECT_EQ(snapshot->readEdgeShaped(10U, {}).edge_id, 10U);
	EXPECT_EQ(snapshot->readEdgeShaped(created, {}).edge_id, created);
	transaction.setRejectStage(false);
	EXPECT_NO_THROW(overlay.deleteLogicalNodes({{1U, true, 2U}}));
}

TEST(LogicalGraphTransactionOverlayUndoTest, SingleTargetUndoDoesNotRetainUnrelatedWideOwners) {
	InMemoryLogicalTransaction transaction;
	LogicalGraphTransactionOverlay overlay(transaction);
	const PropertyValue wide(std::string(16U * 1024U, 'w'));
	std::vector<LogicalGraphNodeCreateRequest> nodes(32U, {{"Wide"}, {{"payload", wide}}});
	const auto ids = overlay.createLogicalNodes(nodes);
	const auto edge = overlay.createLogicalEdges({{ids.front(), ids.back(), "WIDE", {{"payload", wide}}}}).front();
	const bool valuesWereEnabled = debug::PerfTrace::areValuesEnabled();
	debug::PerfTrace::setValueCollectionEnabled(true);
	debug::PerfTrace::reset();
	for (int value = 0; value < 8; ++value)
		overlay.replaceLogicalNodeProperties({{ids.front(), {{"rank", PropertyValue(value)}}}});
	const auto trace = debug::PerfTrace::snapshotAndReset();
	debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	ASSERT_TRUE(trace.contains("logical_graph.overlay.statement_undo_nodes"));
	EXPECT_EQ(trace.at("logical_graph.overlay.statement_undo_nodes").valueCalls, 8U);
	EXPECT_EQ(trace.at("logical_graph.overlay.statement_undo_nodes").maximumValue, 1);
	EXPECT_EQ(trace.at("logical_graph.overlay.statement_undo_edges").maximumValue, 0);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readNodeShaped(ids.back(), {.all_properties = true}).properties.front().value, wide);
	EXPECT_EQ(snapshot->readEdgeShaped(edge, {.all_properties = true}).properties.front().value, wide);
}

TEST(LogicalGraphTransactionOverlayUndoTest, LegacySnapshotsExplicitlyDeclineSchemaIdentity) {
	InMemoryLogicalTransaction transaction;
	EXPECT_FALSE(transaction.readSnapshot().bindingSchemaIdentity());
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	auto vectorSnapshot = overlay.openLogicalGraphVectorSearchSnapshot();
	EXPECT_FALSE(snapshot->bindingSchemaIdentity());
	EXPECT_FALSE(vectorSnapshot->bindingSchemaIdentity());
	overlay.replaceLogicalNodeProperties({{1U, {{"rank", PropertyValue(1)}}}});
	EXPECT_FALSE(snapshot->bindingSchemaIdentity());
	EXPECT_FALSE(vectorSnapshot->bindingSchemaIdentity());
	transaction.rollback();
	EXPECT_THROW(static_cast<void>(snapshot->bindingSchemaIdentity()), std::logic_error);
}

TEST(LogicalGraphTransactionOverlayUndoTest, InvalidSuppliedSchemaIdentityEscapesInsteadOfBecomingUnsupportedFallback) {
	InMemoryLogicalTransaction transaction;
	auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
	LogicalGraphTransactionOverlay overlay(transaction);
	auto snapshot = overlay.openLogicalGraphReadSnapshot();
	auto vectorSnapshot = overlay.openLogicalGraphVectorSearchSnapshot();
	base.schema_identity = LogicalGraphSchemaIdentity{};
	EXPECT_THROW(static_cast<void>(snapshot->bindingSchemaIdentity()), std::logic_error);
	EXPECT_THROW(static_cast<void>(vectorSnapshot->bindingSchemaIdentity()), std::logic_error);
	base.schema_identity = LogicalGraphSchemaIdentity{1U, 2U, {}, 0U, 1U};
	EXPECT_THROW(static_cast<void>(snapshot->bindingSchemaIdentity()), std::logic_error);
	base.schema_identity = LogicalGraphSchemaIdentity{1U, 2U, {}, 7U, 1U};
	// A well-formed mutable source is explicitly unsupported as a pinned base.
	EXPECT_FALSE(snapshot->bindingSchemaIdentity());
	EXPECT_FALSE(vectorSnapshot->bindingSchemaIdentity());
}

} // namespace
} // namespace graph::storage
