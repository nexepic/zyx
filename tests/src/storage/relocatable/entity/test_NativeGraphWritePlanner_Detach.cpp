/**
 * @file test_NativeGraphWritePlanner_Detach.cpp
 * @brief Bounded DETACH DELETE contracts for the native graph writer.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <array>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;

namespace {

	NativeGraphWritePlannerOptions plannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 901U;
		options.logical_id_lease_block_size = 1U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
		return options;
	}

	NativeGraphMutationTokenResolvers tokens() {
		NativeGraphMutationTokenResolvers result;
		result.schema_token = [](const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind kind,
								 std::string_view name) -> std::optional<uint64_t> {
			if (kind == SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE && name == "KNOWS") {
				return 11U;
			}
			return std::nullopt;
		};
		result.property_key_token = [](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> {
			return std::nullopt;
		};
		result.property_key_name = [](const MetadataSnapshotPin &, uint32_t) -> std::optional<std::string> {
			return std::nullopt;
		};
		return result;
	}

	NativeGraphMutationExecutorOptions executorOptions() {
		NativeGraphMutationExecutorOptions options;
		options.maximum_index_definitions = 8U;
		options.maximum_constraint_definitions = 8U;
		options.maximum_constraint_entity_scan = 32U;
		options.maximum_constraint_support_collisions = 8U;
		options.maximum_vector_records = 32U;
		options.maximum_vector_pages = 8U;
		options.vector_delta_rebuild_threshold = 8U;
		options.maximum_vector_delta_overlay_entries = 16U;
		options.maximum_vector_delta_pages = 8U;
		return options;
	}

} // namespace

class NativeGraphWritePlannerDetachTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {};

TEST_F(NativeGraphWritePlannerDetachTest, RejectsAnOverLimitDetachWithoutPublishingAPartialDeletion) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphMutationExecutor executor(
			planner, graph, *btree_pages_, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch create;
	create.node_creates.resize(3U);
	create.edge_creates = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			 "KNOWS", {}},
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 2U},
			 "KNOWS", {}},
	};
	const NativeGraphMutationExecutionResult created = executor.execute(create);
	ASSERT_TRUE(created.committed());
	ASSERT_EQ(created.result.created_node_ids.size(), 3U);
	ASSERT_EQ(created.result.created_edge_ids.size(), 2U);

	LogicalGraphMutationBatch detach;
	detach.node_deletes.push_back({created.result.created_node_ids.front(), true, 1U});
	EXPECT_THROW(static_cast<void>(executor.execute(detach)), FormatException);

	const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_NO_THROW(static_cast<void>(graph.readNode(snapshot, created.result.created_node_ids.front())));
	for (const uint64_t edgeId: created.result.created_edge_ids) {
		EXPECT_NO_THROW(static_cast<void>(graph.readEdge(snapshot, edgeId)));
	}
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, created.result.created_node_ids.front(), 8U)
			  .edge_ids,
			  created.result.created_edge_ids);
}

TEST_F(NativeGraphWritePlannerDetachTest,
		   DetachDeleteDeduplicatesASelfLoopAndRemovesEveryAuthoritativeAdjacencyEntry) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphMutationExecutor executor(
			planner, graph, *btree_pages_, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch create;
	create.node_creates.resize(2U);
	create.edge_creates = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			 "KNOWS", {}},
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			 "KNOWS", {}},
	};
	const NativeGraphMutationExecutionResult created = executor.execute(create);
	ASSERT_TRUE(created.committed());
	ASSERT_EQ(created.result.created_node_ids.size(), 2U);
	ASSERT_EQ(created.result.created_edge_ids.size(), 2U);

	// A self-loop contributes one logical incident Edge, even though it has both
	// outgoing and incoming index keys. A bound of two therefore accepts this
	// self-loop plus the ordinary outgoing Edge rather than double-counting it.
	LogicalGraphMutationBatch detach;
	detach.node_deletes.push_back({created.result.created_node_ids.front(), true, 2U});
	const NativeGraphMutationExecutionResult erased = executor.execute(detach);
	ASSERT_TRUE(erased.committed());

	{
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
		EXPECT_THROW(static_cast<void>(graph.readNode(snapshot, created.result.created_node_ids.front())),
					 FormatException);
		EXPECT_NO_THROW(static_cast<void>(graph.readNode(snapshot, created.result.created_node_ids.back())));
		for (const uint64_t edgeId: created.result.created_edge_ids) {
			EXPECT_THROW(static_cast<void>(graph.readEdge(snapshot, edgeId)), FormatException);
		}
		EXPECT_TRUE(adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT,
								  created.result.created_node_ids.front(), 4U)
						 .edge_ids.empty());
		EXPECT_TRUE(adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_IN,
								  created.result.created_node_ids.front(), 4U)
						 .edge_ids.empty());
		EXPECT_TRUE(adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_IN,
								  created.result.created_node_ids.back(), 4U)
						 .edge_ids.empty());
		snapshot.release();
	}

	// Reopen verifies that the complete post-DETACH generation is the durable
	// state; no half-deleted Edge or directional key can reappear from a prior
	// snapshot route.
	reopenNativeStores();
	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	PageBackedGraphReadAdapter reopenedGraph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_THROW(static_cast<void>(reopenedGraph.readNode(reopened, created.result.created_node_ids.front())),
				 FormatException);
	EXPECT_NO_THROW(static_cast<void>(reopenedGraph.readNode(reopened, created.result.created_node_ids.back())));
	for (const uint64_t edgeId: created.result.created_edge_ids) {
		EXPECT_THROW(static_cast<void>(reopenedGraph.readEdge(reopened, edgeId)), FormatException);
	}
	EXPECT_TRUE(adjacency.scan(reopened, GlobalAdjacencyDirection::GAD_OUT,
							 created.result.created_node_ids.front(), 4U)
					 .edge_ids.empty());
	EXPECT_TRUE(adjacency.scan(reopened, GlobalAdjacencyDirection::GAD_IN,
							 created.result.created_node_ids.back(), 4U)
					 .edge_ids.empty());
	reopened.release();
}
