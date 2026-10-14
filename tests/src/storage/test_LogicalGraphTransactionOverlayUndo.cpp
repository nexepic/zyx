/**
 * @file test_LogicalGraphTransactionOverlayUndo.cpp
 * @brief Statement-local owner undo and failure recovery for logical overlays.
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <gtest/gtest.h>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
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

} // namespace
} // namespace graph::storage
