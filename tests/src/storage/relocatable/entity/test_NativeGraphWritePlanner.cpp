/**
 * @file test_NativeGraphWritePlanner.cpp
 * @brief Atomic native Entity/Page graph write planner tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/adjacency/GlobalAdjacencyIndex.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/NativeScalarIndexMutationPlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedConstraintMutationAdapter.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
using graph::Edge;
using graph::Node;
using graph::PropertyValue;

namespace {

	PropertyCollectionRoot propertyRoot(uint64_t ownerId, uint32_t ownerType, int64_t value) {
		return {ownerId, ownerType, {{17U, PropertyValue(value)}}};
	}

	NativeGraphWritePlannerOptions plannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 901U;
		// Most planner tests assert exact ID boundaries; leasing one ID at a time
		// isolates those semantic assertions from the production batching policy.
		options.logical_id_lease_block_size = 1U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
		return options;
	}

	MetadataPublishOptions withBootstrapFailpoint(BootstrapPublishFailpoint failpoint) {
		MetadataPublishOptions options;
		options.bootstrap_failpoint = failpoint;
		return options;
	}

	IndexRootLocation allocateEmptyIndexRoot(BTreePageStore &indexes, uint64_t indexId = 17U,
											 SegmentId segmentId = SegmentId(71U), PageId pageId = PageId(101U),
											 PhysicalSlotId slot = PhysicalSlotId(40U),
											 LogSequenceNumber checkpointLsn = LogSequenceNumber(2U)) {
		SlottedPagePoolAllocationRequest allocation;
		allocation.segment_id = segmentId;
		allocation.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		allocation.pool_owner_id = indexId;
		allocation.generation = Generation(1U);
		allocation.page_id = pageId;
		allocation.page_owner_id = indexId;
		allocation.page_owner_kind = PageOwnerKind::POK_INDEX;
		PageOwnerHeader owner;
		owner.page_id = allocation.page_id;
		owner.owner_id = allocation.page_owner_id;
		owner.kind = allocation.page_owner_kind;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = allocation.generation;
		owner.committed_lsn = checkpointLsn;
		BTreePageHeader header;
		header.index_id = allocation.page_owner_id;
		header.page_id = allocation.page_id;
		header.generation = allocation.generation;
		if (!indexes.allocate(allocation, BTreePage::create(owner, header), slot, slot, checkpointLsn).has_value()) {
			throw std::runtime_error("test index root allocation unexpectedly ran out of space");
		}
		const IndexRootLocation root{indexId, allocation.page_id, allocation.generation};
		static_cast<void>(
				indexes.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(checkpointLsn.value() + 1U)));
		return root;
	}

	NativeGraphMutationTokenResolvers mutationTokens() {
		NativeGraphMutationTokenResolvers result;
		result.schema_token = [](const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind,
								 std::string_view name) -> std::optional<uint64_t> {
			if (name == "Person")
				return 7U;
			if (name == "Employee")
				return 9U;
			if (name == "KNOWS")
				return 11U;
			return std::nullopt;
		};
		result.property_key_token = [](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
			if (name == "age")
				return 17U;
			if (name == "email")
				return 19U;
			if (name == "name")
				return 23U;
			return std::nullopt;
		};
		result.property_key_name = [](const MetadataSnapshotPin &, uint32_t token) -> std::optional<std::string> {
			if (token == 17U)
				return std::string("age");
			if (token == 19U)
				return std::string("email");
			if (token == 23U)
				return std::string("name");
			return std::nullopt;
		};
		return result;
	}

	NativeGraphMutationExecutorOptions mutationExecutorOptions() {
		NativeGraphMutationExecutorOptions result;
		result.maximum_index_definitions = 128U;
		result.maximum_constraint_definitions = 128U;
		result.maximum_constraint_entity_scan = 1024U;
		result.maximum_constraint_support_collisions = 16U;
		result.maximum_vector_records = 1024U;
		result.maximum_vector_pages = 16U;
		result.vector_delta_rebuild_threshold = 128U;
		result.maximum_vector_delta_overlay_entries = 512U;
		result.maximum_vector_delta_pages = 16U;
		return result;
	}

} // namespace

class NativeGraphWritePlannerTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {};

TEST_F(NativeGraphWritePlannerTest, PublishesTopologyLabelsAndPropertiesInOneNativeGeneration) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const std::array<uint64_t, 4U> firstLabels = {7U, 9U, 11U, 13U};
	const std::array<uint64_t, 1U> secondLabels = {17U};
	const uint64_t firstNode = transaction.createNode(firstLabels);
	const uint64_t secondNode = transaction.createNode(secondLabels);
	const uint64_t edge = transaction.createEdge(firstNode, secondNode, 23U);
	transaction.replaceNodeProperty(propertyRoot(firstNode, Node::typeId, 42));
	transaction.replaceEdgeProperty(propertyRoot(edge, Edge::typeId, 84));
	const NativeGraphWriteCommitResult committed = transaction.commit();
	ASSERT_TRUE(committed.committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const NodeEntityRecord first = entities_->readNode(snapshot, firstNode);
	const NodeEntityRecord second = entities_->readNode(snapshot, secondNode);
	const EdgeEntityRecord created = entities_->readEdge(snapshot, edge);
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, firstNode, 1U).edge_ids,
			  std::vector<uint64_t>({edge}));
	EXPECT_EQ(adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_IN, secondNode, 1U).edge_ids,
			  std::vector<uint64_t>({edge}));
	EXPECT_EQ(labels_->readNodeLabels(snapshot, first), std::vector<uint64_t>(firstLabels.begin(), firstLabels.end()));
	EXPECT_EQ(labels_->readNodeLabels(snapshot, second),
			  std::vector<uint64_t>(secondLabels.begin(), secondLabels.end()));
	EXPECT_EQ(created.source_node_id, firstNode);
	EXPECT_EQ(created.target_node_id, secondNode);
	EXPECT_EQ(created.type_id, 23U);
	EXPECT_EQ(properties_->readEntity(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, firstNode}),
			  propertyRoot(firstNode, Node::typeId, 42));
	EXPECT_EQ(properties_->readEntity(snapshot, {EntityPropertyOwnerKind::EPOK_EDGE, edge}),
			  propertyRoot(edge, Edge::typeId, 84));
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerTest, ReopensTheGlobalAdjacencyTreeWithoutNodeRootFields) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction first = planner.beginTransaction();
	const uint64_t firstSource = first.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstTarget = first.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstEdge = first.createEdge(firstSource, firstTarget, 7U);
	ASSERT_TRUE(first.commit().committed());

	MetadataSnapshotPin initial = metadata_->openSnapshot();
	const std::optional<IndexRootLocation> firstRoot =
			metadata_->findIndexRootOptional(initial, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	ASSERT_TRUE(firstRoot.has_value());
	initial.release();

	reopenNativeStores();
	NativeGraphWritePlanner reopened(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction second = reopened.beginTransaction();
	const uint64_t secondSource = second.createNode(std::array<uint64_t, 0U>{});
	const uint64_t secondTarget = second.createNode(std::array<uint64_t, 0U>{});
	const uint64_t secondEdge = second.createEdge(secondSource, secondTarget, 9U);
	ASSERT_TRUE(second.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const std::optional<IndexRootLocation> currentRoot =
			metadata_->findIndexRootOptional(current, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	ASSERT_TRUE(currentRoot.has_value());
	// The global root has a stable logical PageId; its current physical route is
	// resolved through the pinned Page Directory, never through either Node.
	EXPECT_EQ(currentRoot->root_page_id, firstRoot->root_page_id);
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_EQ(adjacency.scan(current, GlobalAdjacencyDirection::GAD_OUT, firstSource, 4U).edge_ids,
			  (std::vector<uint64_t>{firstEdge}));
	EXPECT_EQ(adjacency.scan(current, GlobalAdjacencyDirection::GAD_OUT, secondSource, 4U).edge_ids,
			  (std::vector<uint64_t>{secondEdge}));
	current.release();
}

TEST_F(NativeGraphWritePlannerTest, ExecutesLogicalBatchThroughNativePagesWithoutLegacyPropertyIds) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, mutationTokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, mutationExecutorOptions());

	LogicalGraphMutationBatch batch;
	batch.node_creates = {
			{{"Employee", "Person"}, {{"name", PropertyValue("Ada")}, {"age", PropertyValue(int64_t(42))}}},
			{{"Person"}, {}}};
	batch.edge_creates = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			 "KNOWS",
			 {{"age", PropertyValue(int64_t(7))}}}};
	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());
	ASSERT_EQ(executed.result.created_node_ids.size(), 2U);
	ASSERT_EQ(executed.result.created_edge_ids.size(), 1U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const PageBackedGraphNode first = graph.readNode(snapshot, executed.result.created_node_ids.front());
	const PageBackedGraphEdge edge = graph.readEdge(snapshot, executed.result.created_edge_ids.front());
	EXPECT_EQ(first.label_token_ids, (std::vector<uint64_t>{7U, 9U}));
	EXPECT_EQ(first.properties, (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(42))},
																		  {23U, PropertyValue("Ada")}}));
	EXPECT_EQ(edge.source_node_id, executed.result.created_node_ids[0]);
	EXPECT_EQ(edge.target_node_id, executed.result.created_node_ids[1]);
	EXPECT_EQ(edge.type_id, 11U);
	EXPECT_EQ(edge.properties, (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(7))}}));
	EXPECT_EQ(properties_
					  ->readEntity(snapshot,
								   {EntityPropertyOwnerKind::EPOK_NODE, executed.result.created_node_ids.front()})
					  .owner_entity_id,
			  executed.result.created_node_ids.front());
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerTest, AppliesSparseCreatedEdgePropertyReplacementWithoutAResolvedEdgeBatch) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, mutationTokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, mutationExecutorOptions());

	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Person"}, {}}, {{"Person"}, {}}};
	batch.edge_creates = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			 "KNOWS",
			 {}}};
	batch.property_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U},
			 {{"age", PropertyValue(int64_t(7))}}}};

	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());
	ASSERT_EQ(executed.result.created_edge_ids.size(), 1U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(graph.readEdge(snapshot, executed.result.created_edge_ids.front()).properties,
			  (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(7))}}));
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerTest, AllocatesSystemCatalogTokensWithTheNativeGraphPublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, {},
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, mutationExecutorOptions(),
			&catalog);
	MetadataSnapshotPin before = metadata_->openSnapshot();

	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Shared"}, {{"name", PropertyValue("Ada")}}}, {{"Shared"}, {}}};
	batch.edge_creates = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			 "Shared",
			 {{"weight", PropertyValue(int64_t(1))}}}};
	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());
	ASSERT_EQ(executed.result.created_node_ids.size(), 2U);
	ASSERT_EQ(executed.result.created_edge_ids.size(), 1U);

	EXPECT_EQ(catalog.findPropertyToken(before, "name"), std::nullopt);
	EXPECT_EQ(catalog.findSchemaToken(before, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Shared"), std::nullopt);
	before.release();

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const std::optional<uint32_t> nameToken = catalog.findPropertyToken(current, "name");
	const std::optional<uint32_t> weightToken = catalog.findPropertyToken(current, "weight");
	const std::optional<uint64_t> labelToken =
			catalog.findSchemaToken(current, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Shared");
	const std::optional<uint64_t> typeToken =
			catalog.findSchemaToken(current, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "Shared");
	ASSERT_TRUE(nameToken.has_value());
	ASSERT_TRUE(weightToken.has_value());
	ASSERT_TRUE(labelToken.has_value());
	ASSERT_TRUE(typeToken.has_value());
	EXPECT_NE(labelToken, typeToken);
	EXPECT_EQ(catalog.findPropertyName(current, *nameToken), std::optional<std::string>("name"));
	EXPECT_EQ(catalog.findPropertyName(current, *weightToken), std::optional<std::string>("weight"));
	EXPECT_EQ(graph.readNode(current, executed.result.created_node_ids.front()).label_token_ids,
			  (std::vector<uint64_t>{*labelToken}));
	const PageBackedGraphEdge edge = graph.readEdge(current, executed.result.created_edge_ids.front());
	EXPECT_EQ(edge.type_id, *typeToken);
	EXPECT_EQ(edge.properties, (std::vector<PropertyCollectionRootEntry>{{*weightToken, PropertyValue(int64_t(1))}}));
	current.release();
}

TEST_F(NativeGraphWritePlannerTest, ExecutesNativeFinalStateReplacementAndBoundedDetachDeletion) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, mutationTokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, mutationExecutorOptions());

	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"}, {{"age", PropertyValue(int64_t(1))}}}, {{"Person"}, {}}};
	create.edge_creates = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U},
			 "KNOWS",
			 {}}};
	const NativeGraphMutationExecutionResult created = executor.execute(create);
	ASSERT_TRUE(created.committed());

	LogicalGraphMutationBatch update;
	update.property_replacements.push_back(
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			  created.result.created_node_ids.front()},
			 {{"email", PropertyValue("ada@example.test")}}});
	update.node_label_replacements.push_back(
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED,
			  created.result.created_node_ids.front()},
			 {"Employee"}});
	ASSERT_TRUE(executor.execute(update).committed());

	MetadataSnapshotPin updated = metadata_->openSnapshot();
	const PageBackedGraphNode changed = graph.readNode(updated, created.result.created_node_ids.front());
	EXPECT_EQ(changed.label_token_ids, (std::vector<uint64_t>{9U}));
	EXPECT_EQ(changed.properties, (std::vector<PropertyCollectionRootEntry>{{19U, PropertyValue("ada@example.test")}}));
	updated.release();

	LogicalGraphMutationBatch detach;
	detach.node_deletes.push_back({created.result.created_node_ids.front(), true, 1U});
	ASSERT_TRUE(executor.execute(detach).committed());
	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(graph.readNode(after, created.result.created_node_ids.front())), FormatException);
	EXPECT_THROW(static_cast<void>(graph.readEdge(after, created.result.created_edge_ids.front())), FormatException);
	after.release();
}

TEST_F(NativeGraphWritePlannerTest, MaintainsNativeScalarAndUniqueConstraintSupportTrees) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const uint64_t scalarIndexId = 177U;
	const uint64_t constraintId = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
	static_cast<void>(
			allocateEmptyIndexRoot(indexes, scalarIndexId, SegmentId(71U), PageId(101U), PhysicalSlotId(40U)));
	static_cast<void>(allocateEmptyIndexRoot(indexes, constraintId, SegmentId(72U), PageId(102U), PhysicalSlotId(41U),
											 LogSequenceNumber(4U)));
	IndexDefinition scalarDefinition;
	scalarDefinition.index_id = scalarIndexId;
	scalarDefinition.name = "person-email";
	scalarDefinition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	scalarDefinition.kind = IndexDefinitionKind::IDK_PROPERTY;
	scalarDefinition.label_token_id = 7U;
	scalarDefinition.property_names = {"email"};
	ConstraintDefinition unique;
	unique.constraint_id = constraintId;
	unique.name = "person-email-unique";
	unique.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	unique.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	unique.schema_token_id = 7U;
	unique.property_names = {"email"};

	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, mutationTokens(),
			[scalarDefinition](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{scalarDefinition}; },
			[unique](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{unique}; },
			mutationExecutorOptions());

	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"}, {{"email", PropertyValue("ada@example.test")}}}};
	NativeGraphMutationExecutionResult created;
	ASSERT_NO_THROW(created = executor.execute(create));
	ASSERT_TRUE(created.committed());
	const uint64_t nodeId = created.result.created_node_ids.front();
	const std::vector<std::byte> scalarKey = IndexKeyCodec::encodeEntryKey(PropertyValue("ada@example.test"), nodeId);
	const std::vector<std::byte> canonical =
			ConstraintSupportTree::canonicalKey(std::vector<PropertyValue>{PropertyValue("ada@example.test")});
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	BTreeIndex scalar(scalarIndexId, indexes, *metadata_);
	BTreeIndex support(constraintId, indexes, *metadata_);
	EXPECT_EQ(scalar.find(snapshot, scalarKey), std::optional<uint64_t>(nodeId));
	EXPECT_EQ(support.find(snapshot, ConstraintSupportTree::entryKey(canonical, nodeId)),
			  std::optional<uint64_t>(nodeId));
	snapshot.release();

	LogicalGraphMutationBatch update;
	update.property_replacements.push_back(
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
			 {{"email", PropertyValue("grace@example.test")}}});
	ASSERT_TRUE(executor.execute(update).committed());
	const std::vector<std::byte> updatedScalarKey =
			IndexKeyCodec::encodeEntryKey(PropertyValue("grace@example.test"), nodeId);
	const std::vector<std::byte> updatedCanonical =
			ConstraintSupportTree::canonicalKey(std::vector<PropertyValue>{PropertyValue("grace@example.test")});
	snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(scalar.find(snapshot, scalarKey).has_value());
	EXPECT_FALSE(support.find(snapshot, ConstraintSupportTree::entryKey(canonical, nodeId)).has_value());
	EXPECT_EQ(scalar.find(snapshot, updatedScalarKey), std::optional<uint64_t>(nodeId));
	EXPECT_EQ(support.find(snapshot, ConstraintSupportTree::entryKey(updatedCanonical, nodeId)),
			  std::optional<uint64_t>(nodeId));
	snapshot.release();

	LogicalGraphMutationBatch duplicate;
	duplicate.node_creates = {{{"Person"}, {{"email", PropertyValue("grace@example.test")}}}};
	EXPECT_THROW(static_cast<void>(executor.execute(duplicate)), std::runtime_error);
}

TEST_F(NativeGraphWritePlannerTest, PreservesCancelledCreationReservationsAndRejectsStaleNativeFences) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, mutationTokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, mutationExecutorOptions());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const uint64_t firstId = before.superblock().next_logical_node_id;
	const Superblock fence = before.superblock();
	before.release();
	const NativeGraphMutationExecutionResult noOp = executor.execute({});
	EXPECT_TRUE(noOp.committed());
	EXPECT_TRUE(noOp.result.created_node_ids.empty());
	EXPECT_EQ(noOp.commit.superblock, fence);
	LogicalGraphMutationBatch rejected;
	rejected.node_creates = {{{"Person"}, {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(rejected, [](const Superblock &) { return false; })),
				 LogicalGraphMutationConflict);
	EXPECT_EQ(metadata_->openSnapshot().superblock(), fence);
	const NativeGraphLogicalIdLease lease = metadata_->reserveNativeGraphLogicalIds(2U, 0U);

	LogicalGraphMutationBatch reserved;
	reserved.node_creates = {{{"Person"}, {}}};
	reserved.creation_identity_plan = LogicalGraphCreationIdentityPlan{{{firstId, 2U}}, {firstId + 1U}, {}, {}};
	const NativeGraphMutationExecutionResult created =
			executor.execute(reserved, [lease](const Superblock &candidate) { return candidate == lease.superblock; });
	ASSERT_TRUE(created.committed());
	ASSERT_EQ(created.result.created_node_ids, (std::vector<uint64_t>{firstId + 1U}));
	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(after, firstId).has_value());
	EXPECT_TRUE(metadata_->findNodePageLocationOptional(after, firstId + 1U).has_value());
	EXPECT_EQ(after.superblock().next_logical_node_id, firstId + 2U);
	after.release();
}

TEST_F(NativeGraphWritePlannerTest, ErasesGlobalAdjacencyAndOwnerKeyedPropertiesWithDeletion) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t firstNode = create.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNode = create.createNode(std::array<uint64_t, 1U>{9U});
	const uint64_t edge = create.createEdge(firstNode, secondNode, 23U);
	create.replaceNodeProperty(propertyRoot(secondNode, Node::typeId, 42));
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	erase.eraseEdge(edge);
	erase.eraseNode(secondNode);
	ASSERT_TRUE(erase.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	EXPECT_TRUE(adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, firstNode, 16U).edge_ids.empty());
	EXPECT_THROW(static_cast<void>(entities_->readEdge(snapshot, edge)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readNode(snapshot, secondNode)), FormatException);
	EXPECT_THROW(static_cast<void>(properties_->readEntity(snapshot, {EntityPropertyOwnerKind::EPOK_NODE, secondNode})),
				 FormatException);
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerTest, ReusesFreeSlotsInCurrentEntityPagesBeforeAppendingAnotherPage) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction first = planner.beginTransaction();
	const uint64_t firstNode = first.createNode(std::array<uint64_t, 1U>{7U});
	ASSERT_TRUE(first.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const EntityPageLocation firstLocation = metadata_->findNodePageLocation(before, firstNode);
	ASSERT_EQ(entities_->currentPools(before).size(), 1U);
	before.release();

	NativeGraphWriteTransaction second = planner.beginTransaction();
	const uint64_t secondNode = second.createNode(std::array<uint64_t, 2U>{9U, 11U});
	ASSERT_TRUE(second.commit().committed());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	const EntityPageLocation secondLocation = metadata_->findNodePageLocation(after, secondNode);
	EXPECT_EQ(secondLocation.page_id, firstLocation.page_id);
	EXPECT_EQ(entities_->currentPools(after).size(), 1U);
	EXPECT_EQ(labels_->readNodeLabels(after, entities_->readNode(after, secondNode)), (std::vector<uint64_t>{9U, 11U}));
	after.release();
}

TEST_F(NativeGraphWritePlannerTest, PublishesPropertyOnlyChangesWithoutRewritingTheNodePage) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 1U>{7U});
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const PageId nodePageId = metadata_->findNodePageLocation(before, nodeId).page_id;
	const PageLocation beforePage = metadata_->findPageLocation(before, nodePageId);
	before.release();

	NativeGraphWriteTransaction propertyOnly = planner.beginTransaction();
	propertyOnly.replaceNodeProperty(propertyRoot(nodeId, Node::typeId, 42));
	ASSERT_TRUE(propertyOnly.commit().committed());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findPageLocation(after, nodePageId), beforePage);
	EXPECT_EQ(properties_->readEntity(after, {EntityPropertyOwnerKind::EPOK_NODE, nodeId}),
			  propertyRoot(nodeId, Node::typeId, 42));
	after.release();
}

TEST_F(NativeGraphWritePlannerTest, RebasesDisjointLeasedNodeCreationsWithoutDuplicatingIds) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction winner = planner.beginTransaction();
	NativeGraphWriteTransaction stale = planner.beginTransaction();
	EXPECT_EQ(winner.createNode(std::array<uint64_t, 1U>{7U}), 1U);
	EXPECT_EQ(stale.createNode(std::array<uint64_t, 1U>{9U}), 2U);
	ASSERT_TRUE(winner.commit().committed());
	EXPECT_TRUE(stale.commit().committed());
	EXPECT_EQ(stale.state(), NativeGraphWriteTransactionState::NGWTS_COMMITTED);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(entities_->readNode(snapshot, 1U).inline_label_ids.front(), 7U);
	EXPECT_EQ(entities_->readNode(snapshot, 2U).inline_label_ids.front(), 9U);
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerTest, PersistsCancelledLogicalIdReservationsWithoutPlaceholderEntities) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation firstReservationGeneration = before.superblock().generation;
	const uint64_t firstNodeId = before.superblock().next_logical_node_id;
	const uint64_t firstEdgeId = before.superblock().next_logical_edge_id;
	before.release();

	NativeGraphWriteTransaction cancelled = planner.beginTransaction();
	cancelled.reserveNodeIds(2U);
	cancelled.reserveEdgeIds(3U);
	MetadataSnapshotPin leased = metadata_->openSnapshot();
	const Generation reservationGeneration = leased.superblock().generation;
	EXPECT_GT(reservationGeneration.value(), firstReservationGeneration.value());
	EXPECT_EQ(leased.superblock().next_logical_node_id, firstNodeId + 2U);
	EXPECT_EQ(leased.superblock().next_logical_edge_id, firstEdgeId + 3U);
	leased.release();
	ASSERT_TRUE(cancelled.commit().committed());

	MetadataSnapshotPin reserved = metadata_->openSnapshot();
	// Leases advance durable high-water marks; the reservation-only commit must
	// not publish an otherwise empty graph generation after those leases.
	EXPECT_EQ(reserved.superblock().generation, reservationGeneration);
	EXPECT_EQ(reserved.superblock().next_logical_node_id, firstNodeId + 2U);
	EXPECT_EQ(reserved.superblock().next_logical_edge_id, firstEdgeId + 3U);
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(reserved, firstNodeId).has_value());
	EXPECT_FALSE(metadata_->findEdgePageLocationOptional(reserved, firstEdgeId).has_value());
	reserved.release();
	reopenNativeStores();

	MetadataSnapshotPin reopened = metadata_->openSnapshot();
	EXPECT_EQ(reopened.superblock().generation, reservationGeneration);
	EXPECT_EQ(reopened.superblock().next_logical_node_id, firstNodeId + 2U);
	EXPECT_EQ(reopened.superblock().next_logical_edge_id, firstEdgeId + 3U);
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(reopened, firstNodeId).has_value());
	EXPECT_FALSE(metadata_->findEdgePageLocationOptional(reopened, firstEdgeId).has_value());
	reopened.release();

	NativeGraphWritePlanner reopenedPlanner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
											plannerOptions());
	NativeGraphWriteTransaction next = reopenedPlanner.beginTransaction();
	const uint64_t nodeId = next.createNode(std::array<uint64_t, 0U>{});
	EXPECT_EQ(nodeId, firstNodeId + 2U);
	ASSERT_TRUE(next.commit().committed());
}

TEST_F(NativeGraphWritePlannerTest, RejectsAnOversizedLogicalIdReservationBeforeLeasingAnyIds) {
	NativeGraphWritePlannerOptions options = plannerOptions();
	options.maximum_logical_id_reservation = 2U;
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Superblock original = before.superblock();
	before.release();

	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	EXPECT_THROW(transaction.reserveNodeIds(3U), FormatException);
	EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_ACTIVE);
	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock(), original);
	after.release();
	transaction.rollback();
}

TEST_F(NativeGraphWritePlannerTest, KeepsConcurrentCancelledLeasesDurableAndNonReusable) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const uint64_t firstNodeId = before.superblock().next_logical_node_id;
	before.release();

	NativeGraphWriteTransaction winner = planner.beginTransaction();
	NativeGraphWriteTransaction stale = planner.beginTransaction();
	winner.reserveNodeIds(2U);
	ASSERT_TRUE(winner.commit().committed());

	stale.reserveNodeIds(3U);
	EXPECT_TRUE(stale.commit().committed());
	EXPECT_EQ(stale.state(), NativeGraphWriteTransactionState::NGWTS_COMMITTED);

	MetadataSnapshotPin published = metadata_->openSnapshot();
	EXPECT_EQ(published.superblock().next_logical_node_id, firstNodeId + 5U);
	published.release();

	NativeGraphWriteTransaction next = planner.beginTransaction();
	EXPECT_EQ(next.createNode(std::array<uint64_t, 0U>{}), firstNodeId + 5U);
	ASSERT_TRUE(next.commit().committed());
}

TEST_F(NativeGraphWritePlannerTest, PublishesLeadingAndTrailingReservedIdsAroundCreatedEntity) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const uint64_t firstNodeId = before.superblock().next_logical_node_id;
	before.release();

	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	transaction.reserveNodeIds(2U);
	const uint64_t createdNodeId = transaction.createNode(std::array<uint64_t, 0U>{});
	transaction.reserveNodeIds(3U);
	EXPECT_EQ(createdNodeId, firstNodeId + 2U);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin published = metadata_->openSnapshot();
	EXPECT_EQ(published.superblock().next_logical_node_id, firstNodeId + 6U);
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(published, firstNodeId).has_value());
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(published, firstNodeId + 1U).has_value());
	EXPECT_TRUE(metadata_->findNodePageLocationOptional(published, createdNodeId).has_value());
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(published, createdNodeId + 1U).has_value());
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(published, createdNodeId + 3U).has_value());
	published.release();
}

TEST_F(NativeGraphWritePlannerTest, RecoversOldReservationGenerationWhenPublicationStopsBeforeWrite) {
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Superblock original = before.superblock();
	before.release();

	EXPECT_THROW(static_cast<void>(metadata_->reserveNativeGraphLogicalIds(
						 2U, 3U, withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);

	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock(), original);
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(recovered, original.next_logical_node_id).has_value());
	EXPECT_FALSE(metadata_->findEdgePageLocationOptional(recovered, original.next_logical_edge_id).has_value());
	recovered.release();
}

TEST_F(NativeGraphWritePlannerTest, RecoversNewReservationGenerationWhenPublicationStopsAfterWrite) {
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Superblock original = before.superblock();
	before.release();

	EXPECT_THROW(static_cast<void>(metadata_->reserveNativeGraphLogicalIds(
						 2U, 3U, withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);

	reopenNativeStores();
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(original.generation.value() + 1U));
	EXPECT_EQ(recovered.superblock().next_logical_node_id, original.next_logical_node_id + 2U);
	EXPECT_EQ(recovered.superblock().next_logical_edge_id, original.next_logical_edge_id + 3U);
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(recovered, original.next_logical_node_id).has_value());
	EXPECT_FALSE(metadata_->findEdgePageLocationOptional(recovered, original.next_logical_edge_id).has_value());
	recovered.release();
}

TEST_F(NativeGraphWritePlannerTest, EnumeratesTransactionLocalIncidentEdgesAfterAdjacencyRepair) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t source = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t tail = create.createEdge(source, target, 7U);
	const uint64_t head = create.createEdge(source, target, 7U);
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction mutate = planner.beginTransaction();
	EXPECT_TRUE(mutate.nodeHasIncidentEdges(source));
	mutate.eraseEdge(head);
	EXPECT_EQ(mutate.collectIncidentEdgeIds(source, 2U), std::vector<uint64_t>({tail}));
	EXPECT_EQ(mutate.collectIncidentEdgeIds(target, 2U), std::vector<uint64_t>({tail}));
	ASSERT_TRUE(mutate.commit().committed());
}

TEST_F(NativeGraphWritePlannerTest, BoundsTheCombinedIncomingAndOutgoingIncidentEdges) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t center = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t outgoingTarget = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t incomingSource = create.createNode(std::array<uint64_t, 0U>{});
	static_cast<void>(create.createEdge(center, outgoingTarget, 7U));
	static_cast<void>(create.createEdge(incomingSource, center, 7U));
	ASSERT_TRUE(create.commit().committed());

	NativeGraphWriteTransaction inspect = planner.beginTransaction();
	EXPECT_THROW(static_cast<void>(inspect.collectIncidentEdgeIds(center, 1U)), FormatException);
}

TEST_F(NativeGraphWritePlannerTest, ReturnsLogicalContinuationWhenGlobalAdjacencyRangeIsBounded) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t source = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstTarget = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t secondTarget = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t firstEdge = create.createEdge(source, firstTarget, 7U);
	const uint64_t secondEdge = create.createEdge(source, secondTarget, 7U);
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	const GlobalAdjacencyRange firstPage = adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, source, 1U);
	EXPECT_EQ(firstPage.edge_ids, (std::vector<uint64_t>{firstEdge}));
	ASSERT_TRUE(firstPage.continuation.has_value());
	EXPECT_EQ(adjacency.scan(snapshot, GlobalAdjacencyDirection::GAD_OUT, source, 1U, firstPage.continuation).edge_ids,
			  (std::vector<uint64_t>{secondEdge}));
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerTest, PlacesInitialOverflowAndNodePageInOneFreshEntityPool) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t nodeId = transaction.createNode(std::array<uint64_t, 4U>{7U, 9U, 11U, 13U});
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const NodeEntityRecord node = entities_->readNode(snapshot, nodeId);
	ASSERT_FALSE(node.label_overflow.isEmpty());
	const EntityPageLocation nodeLocation = metadata_->findNodePageLocation(snapshot, nodeId);
	const PageLocation nodePage = metadata_->findPageLocation(snapshot, nodeLocation.page_id);
	const PageLocation labelPage = metadata_->findPageLocation(snapshot, node.label_overflow.page_id);
	EXPECT_EQ(labelPage.segment_id, nodePage.segment_id);
	EXPECT_EQ(entities_->currentPools(snapshot).size(), 1U);
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerTest, GrowsExistingInlineLabelsWithoutConsumingSpeculativeIdentifiers) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const std::array<uint64_t, 3U> inlineLabels = {7U, 9U, 11U};
	const uint64_t nodeId = create.createNode(inlineLabels);
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	EXPECT_TRUE(entities_->readNode(before, nodeId).label_overflow.isEmpty());

	NativeGraphWriteTransaction grow = planner.beginTransaction();
	const std::array<uint64_t, 4U> overflowLabels = {7U, 9U, 11U, 13U};
	grow.replaceNodeLabels(nodeId, overflowLabels);
	ASSERT_TRUE(grow.commit().committed());

	// The previously pinned generation remains self-consistent even after the
	// current Node becomes reachable through a LabelSet overflow reference.
	EXPECT_EQ(labels_->readNodeLabels(before, entities_->readNode(before, nodeId)),
			  std::vector<uint64_t>(inlineLabels.begin(), inlineLabels.end()));
	before.release();

	MetadataSnapshotPin after = metadata_->openSnapshot();
	const NodeEntityRecord node = entities_->readNode(after, nodeId);
	ASSERT_FALSE(node.label_overflow.isEmpty());
	EXPECT_EQ(labels_->readNodeLabels(after, node),
			  std::vector<uint64_t>(overflowLabels.begin(), overflowLabels.end()));
	EXPECT_EQ(after.superblock().next_logical_page_id, beforeSuperblock.next_logical_page_id + 1U);
	EXPECT_EQ(after.superblock().next_logical_segment_id, beforeSuperblock.next_logical_segment_id);
	after.release();
}

TEST_F(NativeGraphWritePlannerTest, MovesAnExpandedOverflowIntoAnotherLabelPageWithoutLosingPeers) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction first = planner.beginTransaction();
	const std::array<uint64_t, 4U> firstLabels = {7U, 9U, 11U, 13U};
	const uint64_t firstNode = first.createNode(firstLabels);
	ASSERT_TRUE(first.commit().committed());

	NativeGraphWriteTransaction second = planner.beginTransaction();
	const std::array<uint64_t, 4U> secondLabels = {17U, 19U, 23U, 29U};
	const uint64_t secondNode = second.createNode(secondLabels);
	ASSERT_TRUE(second.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const NodeEntityRecord firstBefore = entities_->readNode(before, firstNode);
	const NodeEntityRecord secondBefore = entities_->readNode(before, secondNode);
	ASSERT_FALSE(firstBefore.label_overflow.isEmpty());
	EXPECT_EQ(firstBefore.label_overflow.page_id, secondBefore.label_overflow.page_id);

	std::vector<uint64_t> expandedLabels(RSS_ENTITY_MAX_LABELS);
	std::iota(expandedLabels.begin(), expandedLabels.end(), 1U);
	NativeGraphWriteTransaction grow = planner.beginTransaction();
	grow.replaceNodeLabels(firstNode, expandedLabels);
	ASSERT_TRUE(grow.commit().committed());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	const NodeEntityRecord firstAfter = entities_->readNode(after, firstNode);
	const NodeEntityRecord secondAfter = entities_->readNode(after, secondNode);
	EXPECT_NE(firstAfter.label_overflow.page_id, firstBefore.label_overflow.page_id);
	EXPECT_EQ(secondAfter.label_overflow, secondBefore.label_overflow);
	EXPECT_EQ(labels_->readNodeLabels(after, firstAfter), expandedLabels);
	EXPECT_EQ(labels_->readNodeLabels(after, secondAfter),
			  std::vector<uint64_t>(secondLabels.begin(), secondLabels.end()));
	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerTest, BatchesInitialOverflowLabelsWithoutConflictingPageReplacements) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const std::array<uint64_t, 4U> seedLabels = {7U, 9U, 11U, 13U};
	const uint64_t seedNode = seed.createNode(seedLabels);
	ASSERT_TRUE(seed.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const PageId existingLabelPage = entities_->readNode(before, seedNode).label_overflow.page_id;
	before.release();

	NativeGraphWriteTransaction batch = planner.beginTransaction();
	const std::array<uint64_t, 4U> firstLabels = {17U, 19U, 23U, 29U};
	const std::array<uint64_t, 4U> secondLabels = {31U, 37U, 41U, 43U};
	const uint64_t firstNode = batch.createNode(firstLabels);
	const uint64_t secondNode = batch.createNode(secondLabels);
	ASSERT_TRUE(batch.commit().committed());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	const NodeEntityRecord first = entities_->readNode(after, firstNode);
	const NodeEntityRecord second = entities_->readNode(after, secondNode);
	EXPECT_EQ(first.label_overflow.page_id, existingLabelPage);
	EXPECT_NE(second.label_overflow.page_id, existingLabelPage);
	EXPECT_EQ(labels_->readNodeLabels(after, first), std::vector<uint64_t>(firstLabels.begin(), firstLabels.end()));
	EXPECT_EQ(labels_->readNodeLabels(after, second), std::vector<uint64_t>(secondLabels.begin(), secondLabels.end()));
	after.release();
}

TEST_F(NativeGraphWritePlannerTest, ErasesExactGlobalKeysWithoutRewritingPeerEdgesAcrossGenerations) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t source = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t first = create.createEdge(source, target, 7U);
	const uint64_t middle = create.createEdge(source, target, 7U);
	const uint64_t last = create.createEdge(source, target, 7U);
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	GlobalAdjacencyIndex adjacency(*btree_pages_, *metadata_);
	const std::vector<uint64_t> allEdges = {first, middle, last};
	EXPECT_EQ(adjacency.scan(before, GlobalAdjacencyDirection::GAD_OUT, source, 16U).edge_ids, allEdges);
	EXPECT_EQ(adjacency.scan(before, GlobalAdjacencyDirection::GAD_IN, target, 16U).edge_ids, allEdges);
	const EdgeEntityRecord firstBefore = entities_->readEdge(before, first);
	const EdgeEntityRecord lastBefore = entities_->readEdge(before, last);

	NativeGraphWriteTransaction eraseMiddle = planner.beginTransaction();
	eraseMiddle.eraseEdge(middle);
	ASSERT_TRUE(eraseMiddle.commit().committed());
	MetadataSnapshotPin afterMiddle = metadata_->openSnapshot();
	EXPECT_EQ(adjacency.scan(afterMiddle, GlobalAdjacencyDirection::GAD_OUT, source, 16U).edge_ids,
			  (std::vector<uint64_t>{first, last}));
	EXPECT_EQ(adjacency.scan(afterMiddle, GlobalAdjacencyDirection::GAD_IN, target, 16U).edge_ids,
			  (std::vector<uint64_t>{first, last}));
	// Exact B+Tree erase changes only derived index pages. Unrelated Edge
	// records retain their authoritative semantic payload.
	EXPECT_EQ(entities_->readEdge(afterMiddle, first), firstBefore);
	EXPECT_EQ(entities_->readEdge(afterMiddle, last), lastBefore);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(afterMiddle, middle)), FormatException);
	afterMiddle.release();

	NativeGraphWriteTransaction eraseLast = planner.beginTransaction();
	eraseLast.eraseEdge(last);
	ASSERT_TRUE(eraseLast.commit().committed());
	NativeGraphWriteTransaction eraseFirst = planner.beginTransaction();
	eraseFirst.eraseEdge(first);
	ASSERT_TRUE(eraseFirst.commit().committed());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_TRUE(adjacency.scan(after, GlobalAdjacencyDirection::GAD_OUT, source, 16U).edge_ids.empty());
	EXPECT_TRUE(adjacency.scan(after, GlobalAdjacencyDirection::GAD_IN, target, 16U).edge_ids.empty());
	EXPECT_THROW(static_cast<void>(entities_->readEdge(after, first)), FormatException);
	EXPECT_THROW(static_cast<void>(entities_->readEdge(after, last)), FormatException);
	after.release();

	EXPECT_EQ(adjacency.scan(before, GlobalAdjacencyDirection::GAD_OUT, source, 16U).edge_ids, allEdges);
	EXPECT_EQ(adjacency.scan(before, GlobalAdjacencyDirection::GAD_IN, target, 16U).edge_ids, allEdges);
	EXPECT_EQ(entities_->readEdge(before, middle).source_node_id, source);
	EXPECT_EQ(entities_->readEdge(before, middle).target_node_id, target);
	before.release();
}

TEST_F(NativeGraphWritePlannerTest, PublishesNativeDerivedIndexFramesWithOneSnapshotSafeGeneration) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SlottedPagePoolAllocationRequest allocation;
	allocation.segment_id = SegmentId(71U);
	allocation.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	allocation.pool_owner_id = 17U;
	allocation.generation = Generation(1U);
	allocation.page_id = PageId(101U);
	allocation.page_owner_id = 17U;
	allocation.page_owner_kind = PageOwnerKind::POK_INDEX;
	PageOwnerHeader owner;
	owner.page_id = allocation.page_id;
	owner.owner_id = allocation.page_owner_id;
	owner.kind = allocation.page_owner_kind;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = allocation.generation;
	owner.committed_lsn = LogSequenceNumber(2U);
	BTreePageHeader header;
	header.index_id = allocation.page_owner_id;
	header.page_id = allocation.page_id;
	header.generation = allocation.generation;
	ASSERT_TRUE(indexes.allocate(allocation, BTreePage::create(owner, header), PhysicalSlotId(40U), PhysicalSlotId(40U),
								 LogSequenceNumber(2U))
						.has_value());
	const IndexRootLocation root{17U, allocation.page_id, allocation.generation};
	static_cast<void>(indexes.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const std::array<std::byte, 1U> key = {std::byte{'k'}};
	const BTreeRootLeafMutationPlan plan = indexes.planRootLeafUpsert(before, root.index_id, key, 42U);
	NativeGraphIndexTreeMutation treeMutation;
	treeMutation.index_id = plan.expected_root.index_id;
	treeMutation.expected_root = plan.expected_root;
	treeMutation.pool_requests.push_back({plan.expected_pool.segment_id, {plan.expected_location}, 0U, {}});
	treeMutation.stable_replacement_root = root.root_page_id;
	NativeGraphDerivedStateMutation derived;
	derived.index_tree_mutations.push_back(std::move(treeMutation));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const Superblock published = metadata_->mutateNativeGraphPages(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools, derived,
			LogSequenceNumber(4U),
			[](std::span<EntityPageReplacementMaterialization> replacements,
			   std::span<EntityPageAppendBatchMaterialization> appends,
			   std::span<EntityPageNewPoolBatchMaterialization> newPools,
			   std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
			   std::span<NativeGraphPropertyPageBatchMaterialization> newPropertyPools) {
				EXPECT_TRUE(replacements.empty());
				EXPECT_TRUE(appends.empty());
				EXPECT_TRUE(newPools.empty());
				EXPECT_TRUE(properties.empty());
				EXPECT_TRUE(newPropertyPools.empty());
			},
			[&indexes, &plan](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				ASSERT_EQ(batches.size(), 1U);
				ASSERT_EQ(batches.front().size(), 1U);
				IndexTreeMutationPoolBatch &batch = batches.front().front();
				ASSERT_EQ(batch.expected_replacements.size(), 1U);
				indexes.prepareRootLeafMutation(plan, batch.pool, batch.expected_replacements.front(),
												batch.replacement_locations.front(), LogSequenceNumber(4U));
			});
	EXPECT_EQ(published.generation, Generation(4U));
	EXPECT_TRUE(indexes.read(before, root.root_page_id).entries().empty());
	BTreeIndex index(root.index_id, indexes, *metadata_);
	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(index.find(after, key), std::optional<uint64_t>(42U));
	EXPECT_EQ(metadata_->findIndexRoot(after, root.index_id), root);
	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerTest, PublishesFreshDerivedIndexPoolAndRootPromotionAtomically) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SlottedPagePoolAllocationRequest allocation;
	allocation.segment_id = SegmentId(71U);
	allocation.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	allocation.pool_owner_id = 17U;
	allocation.generation = Generation(1U);
	allocation.page_id = PageId(101U);
	allocation.page_owner_id = 17U;
	allocation.page_owner_kind = PageOwnerKind::POK_INDEX;
	PageOwnerHeader owner;
	owner.page_id = allocation.page_id;
	owner.owner_id = allocation.page_owner_id;
	owner.kind = allocation.page_owner_kind;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = allocation.generation;
	owner.committed_lsn = LogSequenceNumber(2U);
	BTreePageHeader header;
	header.index_id = allocation.page_owner_id;
	header.page_id = allocation.page_id;
	header.generation = allocation.generation;
	ASSERT_TRUE(indexes.allocate(allocation, BTreePage::create(owner, header), PhysicalSlotId(40U), PhysicalSlotId(40U),
								 LogSequenceNumber(2U))
						.has_value());
	const IndexRootLocation root{17U, allocation.page_id, allocation.generation};
	static_cast<void>(indexes.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));

	MetadataSnapshotPin before = metadata_->openSnapshot();
	NativeGraphIndexTreeMutation treeMutation;
	treeMutation.index_id = root.index_id;
	treeMutation.expected_root = root;
	treeMutation.new_pool_requests.push_back({PhysicalSlotId(41U), PhysicalSlotId(41U), 17U, 1U});
	treeMutation.replacement_root = {0U, 0U, true};
	NativeGraphDerivedStateMutation derived;
	derived.index_tree_mutations.push_back(std::move(treeMutation));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<std::byte, 1U> key = {std::byte{'n'}};
	static_cast<void>(metadata_->mutateNativeGraphPages(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools, derived,
			LogSequenceNumber(4U),
			[](std::span<EntityPageReplacementMaterialization> replacements,
			   std::span<EntityPageAppendBatchMaterialization> appends,
			   std::span<EntityPageNewPoolBatchMaterialization> newPools,
			   std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
			   std::span<NativeGraphPropertyPageBatchMaterialization> newPropertyPools) {
				EXPECT_TRUE(replacements.empty());
				EXPECT_TRUE(appends.empty());
				EXPECT_TRUE(newPools.empty());
				EXPECT_TRUE(properties.empty());
				EXPECT_TRUE(newPropertyPools.empty());
			},
			[&indexes, &key](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				ASSERT_EQ(batches.size(), 1U);
				ASSERT_EQ(batches.front().size(), 1U);
				IndexTreeMutationPoolBatch &batch = batches.front().front();
				ASSERT_TRUE(batch.newly_reserved);
				ASSERT_EQ(batch.appended_page_ids.size(), 1U);
				BTreePageHeader newRootHeader;
				newRootHeader.index_id = 17U;
				newRootHeader.page_id = batch.appended_page_ids.front();
				newRootHeader.generation = batch.pool.generation;
				PageOwnerHeader newRootOwner;
				newRootOwner.page_id = newRootHeader.page_id;
				newRootOwner.owner_id = newRootHeader.index_id;
				newRootOwner.kind = PageOwnerKind::POK_INDEX;
				newRootOwner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
				newRootOwner.generation = newRootHeader.generation;
				newRootOwner.committed_lsn = LogSequenceNumber(4U);
				std::vector<BTreePage> pages;
				pages.push_back(BTreePage::create(newRootOwner, newRootHeader));
				pages.front().insert({std::vector<std::byte>(key.begin(), key.end()), 84U});
				indexes.prepareNewPoolInitialPages(batch.pool, pages, batch.appended_locations, LogSequenceNumber(4U));
			}));

	EXPECT_TRUE(indexes.read(before, root.root_page_id).entries().empty());
	MetadataSnapshotPin after = metadata_->openSnapshot();
	const IndexRootLocation promoted = metadata_->findIndexRoot(after, root.index_id);
	EXPECT_NE(promoted.root_page_id, root.root_page_id);
	EXPECT_EQ(metadata_->findCatalog(after, metadata_->findPageLocation(after, promoted.root_page_id).segment_id).kind,
			  SegmentKind::SGK_INDEX_PAGE_POOL);
	BTreeIndex index(root.index_id, indexes, *metadata_);
	EXPECT_EQ(index.find(after, key), std::optional<uint64_t>(84U));
	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerTest, CreatesAbsentDerivedIndexRootInTheNativeGraphGeneration) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const uint64_t indexId = 17U;
	const std::array<std::byte, 1U> key = {std::byte{'c'}};
	MetadataSnapshotPin before = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(before, indexId).has_value());
	const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), 84U}}};
	const BTreeInitialTreeMutationPlan initialPlan = indexes.planInitialTreeMutation(before, indexId, entries);

	NativeGraphIndexTreeMutation treeMutation;
	treeMutation.index_id = initialPlan.index_id;
	for (const BTreeNewIndexTreePoolMutation &pool: initialPlan.new_pool_mutations) {
		treeMutation.new_pool_requests.push_back(
				{PhysicalSlotId(40U), PhysicalSlotId(40U), pool.owner_id, pool.appended_page_count});
	}
	treeMutation.replacement_root = {initialPlan.root_page.pool_ordinal, initialPlan.root_page.appended_page_ordinal,
									 true};
	NativeGraphDerivedStateMutation derived;
	derived.index_tree_mutations.push_back(std::move(treeMutation));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const Superblock published = metadata_->mutateNativeGraphPages(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools, derived,
			LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization> replacements,
			   std::span<EntityPageAppendBatchMaterialization> appends,
			   std::span<EntityPageNewPoolBatchMaterialization> newPools,
			   std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
			   std::span<NativeGraphPropertyPageBatchMaterialization> newPropertyPools) {
				EXPECT_TRUE(replacements.empty());
				EXPECT_TRUE(appends.empty());
				EXPECT_TRUE(newPools.empty());
				EXPECT_TRUE(properties.empty());
				EXPECT_TRUE(newPropertyPools.empty());
			},
			[&initialPlan](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				ASSERT_EQ(batches.size(), 1U);
				ASSERT_EQ(batches.front().size(), 1U);
				initialPlan.preparer(batches.front(), LogSequenceNumber(2U));
			});

	EXPECT_EQ(published.generation, Generation(2U));
	EXPECT_FALSE(metadata_->findIndexRootOptional(before, indexId).has_value());
	MetadataSnapshotPin after = metadata_->openSnapshot();
	const IndexRootLocation root = metadata_->findIndexRoot(after, indexId);
	EXPECT_EQ(root.index_id, indexId);
	EXPECT_TRUE(root.root_page_id.isValid());
	EXPECT_EQ(root.root_generation, Generation(1U));
	BTreeIndex index(indexId, indexes, *metadata_);
	EXPECT_EQ(index.find(after, key), std::optional<uint64_t>(84U));
	after.release();

	NativeGraphIndexTreeMutation duplicateMutation;
	duplicateMutation.index_id = indexId;
	duplicateMutation.new_pool_requests.push_back({PhysicalSlotId(41U), PhysicalSlotId(41U), indexId, 1U});
	duplicateMutation.replacement_root = {0U, 0U, true};
	NativeGraphDerivedStateMutation duplicate;
	duplicate.index_tree_mutations.push_back(std::move(duplicateMutation));
	EXPECT_THROW(
			static_cast<void>(metadata_->mutateNativeGraphPages(
					noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
					duplicate, LogSequenceNumber(3U),
					[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
					   std::span<EntityPageNewPoolBatchMaterialization>,
					   std::span<NativeGraphPropertyPageReplacementMaterialization>,
					   std::span<NativeGraphPropertyPageBatchMaterialization>) {},
					[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {})),
			FormatException);
	before.release();
}

TEST_F(NativeGraphWritePlannerTest, PublishesEntityAndDerivedIndexInOneSnapshotGeneration) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SlottedPagePoolAllocationRequest allocation;
	allocation.segment_id = SegmentId(71U);
	allocation.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	allocation.pool_owner_id = 17U;
	allocation.generation = Generation(1U);
	allocation.page_id = PageId(101U);
	allocation.page_owner_id = 17U;
	allocation.page_owner_kind = PageOwnerKind::POK_INDEX;
	PageOwnerHeader owner;
	owner.page_id = allocation.page_id;
	owner.owner_id = allocation.page_owner_id;
	owner.kind = allocation.page_owner_kind;
	owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	owner.generation = allocation.generation;
	owner.committed_lsn = LogSequenceNumber(2U);
	BTreePageHeader header;
	header.index_id = allocation.page_owner_id;
	header.page_id = allocation.page_id;
	header.generation = allocation.generation;
	ASSERT_TRUE(indexes.allocate(allocation, BTreePage::create(owner, header), PhysicalSlotId(40U), PhysicalSlotId(40U),
								 LogSequenceNumber(2U))
						.has_value());
	const IndexRootLocation root{17U, allocation.page_id, allocation.generation};
	static_cast<void>(indexes.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const std::array<std::byte, 1U> key = {std::byte{'n'}};
	const BTreeRootLeafMutationPlan indexPlan = indexes.planRootLeafUpsert(before, root.index_id, key, 1001U);
	NativeGraphIndexTreeMutation treeMutation;
	treeMutation.index_id = indexPlan.expected_root.index_id;
	treeMutation.expected_root = indexPlan.expected_root;
	treeMutation.pool_requests.push_back({indexPlan.expected_pool.segment_id, {indexPlan.expected_location}, 0U, {}});
	treeMutation.stable_replacement_root = root.root_page_id;
	NativeGraphDerivedStateMutation derived;
	derived.index_tree_mutations.push_back(std::move(treeMutation));
	const std::array<EntityPageBatchNewPool, 1U> newNodePool{{
			{.request = {SegmentId(81U), 901U, Generation(1U), PageId(201U), PhysicalSlotId(41U), PhysicalSlotId(41U)},
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_insertions = {{1001U, {}}}},
	}};
	const std::array<EntityPageBatchMutation, 0U> noMutations{};
	const std::array<EntityPageBatchAppend, 0U> noAppends{};
	const std::array<EntityPropertyRootMutationPlan, 0U> noPropertyPlans{};
	const EntityPageAuxiliaryMutationBatch noAuxiliaryPages;
	const EntityPageBatchResult published = entities_->applyNativeGraphBatchTransaction(
			before, noMutations, noAppends, newNodePool, noPropertyPlans, *properties_, plannerOptions().property_roots,
			noAuxiliaryPages, derived,
			[&indexes, &indexPlan](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				ASSERT_EQ(batches.size(), 1U);
				ASSERT_EQ(batches.front().size(), 1U);
				IndexTreeMutationPoolBatch &batch = batches.front().front();
				ASSERT_EQ(batch.expected_replacements.size(), 1U);
				indexes.prepareRootLeafMutation(indexPlan, batch.pool, batch.expected_replacements.front(),
												batch.replacement_locations.front(), LogSequenceNumber(4U));
			},
			LogSequenceNumber(4U));
	EXPECT_EQ(published.superblock.generation, Generation(4U));

	BTreeIndex index(root.index_id, indexes, *metadata_);
	EXPECT_THROW(static_cast<void>(entities_->readNode(before, 1001U)), FormatException);
	EXPECT_FALSE(index.find(before, key).has_value());
	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(entities_->readNode(after, 1001U), newNodePool.front().node_insertions.front().record);
	EXPECT_EQ(index.find(after, key), std::optional<uint64_t>(1001U));
	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerTest, CreatesScalarIndexRootWithNativeGraphStateInOneGeneration) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const std::array<uint64_t, 0U> noLabels{};
	const std::array<std::byte, 1U> key = {std::byte{'i'}};
	MetadataSnapshotPin before = metadata_->openSnapshot();
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t nodeId = transaction.createNode(noLabels);
	const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), nodeId}}};
	transaction.stageScalarIndexCreate(indexes, 17U, entries);
	const NativeGraphWriteCommitResult committed = transaction.commit();
	ASSERT_TRUE(committed.committed());
	// The durable logical-ID lease is published before the graph/index generation.
	EXPECT_EQ(committed.superblock.generation, Generation(3U));
	EXPECT_THROW(static_cast<void>(entities_->readNode(before, nodeId)), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(before, 17U).has_value());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(entities_->readNode(after, nodeId), NodeEntityRecord{});
	BTreeIndex index(17U, indexes, *metadata_);
	EXPECT_EQ(index.find(after, key), std::optional<uint64_t>(nodeId));
	EXPECT_TRUE(metadata_->findIndexRoot(after, 17U).root_page_id.isValid());
	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerTest, PublishesStreamingBulkScalarIndexPlanThroughNativeTransaction) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	std::vector<BTreePageEntry> sourceEntries;
	sourceEntries.reserve(96U);
	for (uint16_t ordinal = 0U; ordinal < 96U; ++ordinal) {
		std::vector<std::byte> key(48U, std::byte{'b'});
		key[0] = static_cast<std::byte>(ordinal >> 8U);
		key[1] = static_cast<std::byte>(ordinal & 0xFFU);
		sourceEntries.push_back({std::move(key), ordinal + 1U});
	}
	const auto source = [entries = std::make_shared<const std::vector<BTreePageEntry>>(sourceEntries)] {
		size_t ordinal = 0U;
		return [entries, ordinal]() mutable -> std::optional<BTreePageEntry> {
			if (ordinal == entries->size())
				return std::nullopt;
			return (*entries)[ordinal++];
		};
	};
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const BulkBTreeBuildOptions options{PhysicalSlotId(40U), PhysicalSlotId(40U), 17U, sourceEntries.size(), 64U};
	const BTreeInitialTreeMutationPlan plan =
			BulkBTreeBuilder::planNativeTree(indexes, *metadata_, before, 17U, source, options);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	transaction.stageScalarIndexCreate(indexes, plan);
	const NativeGraphWriteCommitResult committed = transaction.commit();
	ASSERT_TRUE(committed.committed());

	BTreeIndex index(17U, indexes, *metadata_);
	MetadataSnapshotPin after = metadata_->openSnapshot();
	const std::vector<BTreePageEntry> actual = index.scan(after, std::nullopt, sourceEntries.size() + 1U);
	EXPECT_EQ(actual, sourceEntries);
	EXPECT_NE(metadata_->findIndexRoot(after, 17U).root_page_id, PageId{});
	after.release();
	before.release();
}

TEST_F(NativeGraphWritePlannerTest, RejectsAChangedStreamingBulkSourceBeforePublishingItsIndexRoot) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const auto pass = std::make_shared<size_t>(0U);
	const OrderedBTreeEntrySource source = [pass] {
		auto values = std::make_shared<std::vector<BTreePageEntry>>();
		values->push_back({std::vector<std::byte>{std::byte{'a'}}, 1U});
		values->push_back({std::vector<std::byte>{std::byte{'b'}}, 2U});
		if ((*pass)++ != 0U) {
			// The second pass remains ordered, but changes the first planned leaf boundary.
			(*values)[0].key = {std::byte{'A'}};
		}
		auto ordinal = std::make_shared<size_t>(0U);
		return [values, ordinal]() -> std::optional<BTreePageEntry> {
			if (*ordinal == values->size())
				return std::nullopt;
			return (*values)[(*ordinal)++];
		};
	};
	MetadataSnapshotPin before = metadata_->openSnapshot();
	const BulkBTreeBuildOptions options{PhysicalSlotId(40U), PhysicalSlotId(40U), 17U, 2U, 8U};
	const BTreeInitialTreeMutationPlan plan =
			BulkBTreeBuilder::planNativeTree(indexes, *metadata_, before, 17U, source, options);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	transaction.stageScalarIndexCreate(indexes, plan);
	EXPECT_THROW(static_cast<void>(transaction.commit()), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(before, 17U).has_value());
	before.release();

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(after, 17U).has_value());
	after.release();
}

TEST_F(NativeGraphWritePlannerTest, RejectsOtherStreamingBulkSecondPassMismatchesBeforePublication) {
	const auto sourceWithSecondPass = [](std::vector<BTreePageEntry> secondPass) {
		const auto pass = std::make_shared<size_t>(0U);
		return OrderedBTreeEntrySource{[pass, secondPass = std::move(secondPass)] {
			const std::vector<BTreePageEntry> firstPass = {
					{std::vector<std::byte>{std::byte{'a'}}, 1U},
					{std::vector<std::byte>{std::byte{'b'}}, 2U},
			};
			auto values = std::make_shared<std::vector<BTreePageEntry>>((*pass)++ == 0U ? firstPass : secondPass);
			auto ordinal = std::make_shared<size_t>(0U);
			return OrderedBTreeEntryCursor{[values, ordinal]() -> std::optional<BTreePageEntry> {
				if (*ordinal == values->size())
					return std::nullopt;
				return (*values)[(*ordinal)++];
			}};
		}};
	};
	const auto expectRejected = [&](const OrderedBTreeEntrySource &source) {
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		MetadataSnapshotPin before = metadata_->openSnapshot();
		const BulkBTreeBuildOptions options{PhysicalSlotId(40U), PhysicalSlotId(40U), 17U, 2U, 8U};
		const BTreeInitialTreeMutationPlan plan =
				BulkBTreeBuilder::planNativeTree(indexes, *metadata_, before, 17U, source, options);
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
										plannerOptions());
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		transaction.stageScalarIndexCreate(indexes, plan);
		EXPECT_THROW(static_cast<void>(transaction.commit()), FormatException);
		EXPECT_FALSE(metadata_->findIndexRootOptional(before, 17U).has_value());
		before.release();
	};

	expectRejected(sourceWithSecondPass({{std::vector<std::byte>{std::byte{'a'}}, 1U}}));
	expectRejected(sourceWithSecondPass(
			{{std::vector<std::byte>{std::byte{'a'}}, 1U}, {std::vector<std::byte>{std::byte{'b'}}, 0U}}));
	expectRejected(sourceWithSecondPass(
			{{std::vector<std::byte>{std::byte{'b'}}, 2U}, {std::vector<std::byte>{std::byte{'a'}}, 1U}}));
	expectRejected(sourceWithSecondPass(
			{{std::vector<std::byte>{std::byte{'a'}}, 9U}, {std::vector<std::byte>{std::byte{'b'}}, 2U}}));
	expectRejected(sourceWithSecondPass({{std::vector<std::byte>{std::byte{'a'}}, 1U},
										 {std::vector<std::byte>{std::byte{'b'}}, 2U},
										 {std::vector<std::byte>{std::byte{'c'}}, 3U}}));

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(after, 17U).has_value());
	after.release();
}

TEST_F(NativeGraphWritePlannerTest, RejectsANonMonotonicNativeBulkSecondPassAfterTheFirstLeafBoundary) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const auto pass = std::make_shared<size_t>(0U);
	const OrderedBTreeEntrySource source = [pass] {
		const std::vector<BTreePageEntry> firstPass = {
				{{std::byte{'a'}}, 1U},
				{{std::byte{'b'}}, 2U},
				{{std::byte{'c'}}, 3U},
		};
		const std::vector<BTreePageEntry> secondPass = {
				{{std::byte{'a'}}, 1U},
				{{std::byte{'c'}}, 3U},
				{{std::byte{'b'}}, 2U},
		};
		auto values = std::make_shared<std::vector<BTreePageEntry>>((*pass)++ == 0U ? firstPass : secondPass);
		auto ordinal = std::make_shared<size_t>(0U);
		return OrderedBTreeEntryCursor{[values, ordinal]() -> std::optional<BTreePageEntry> {
			if (*ordinal == values->size()) {
				return std::nullopt;
			}
			return (*values)[(*ordinal)++];
		}};
	};

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const BulkBTreeBuildOptions options{PhysicalSlotId(40U), PhysicalSlotId(40U), 17U, 3U, 8U};
	const BTreeInitialTreeMutationPlan plan =
			BulkBTreeBuilder::planNativeTree(indexes, *metadata_, before, 17U, source, options);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	transaction.stageScalarIndexCreate(indexes, plan);
	EXPECT_THROW(static_cast<void>(transaction.commit()), FormatException);
	EXPECT_FALSE(metadata_->findIndexRootOptional(before, 17U).has_value());
	before.release();

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findIndexRootOptional(after, 17U).has_value());
	after.release();
}

TEST_F(NativeGraphWritePlannerTest, RecoversOldGenerationWhenInitialScalarIndexPublicationStopsBeforeSuperblock) {
	const std::array<uint64_t, 0U> noLabels{};
	const std::array<std::byte, 1U> key = {std::byte{'o'}};
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
										plannerOptions());
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		const uint64_t nodeId = transaction.createNode(noLabels);
		const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), nodeId}}};
		transaction.stageScalarIndexCreate(indexes, 17U, entries);
		EXPECT_THROW(static_cast<void>(transaction.commit(
							 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
					 BootstrapPublishInterrupted);
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
		MetadataSnapshotPin current = metadata_->openSnapshot();
		EXPECT_EQ(current.superblock().generation, Generation(2U));
		EXPECT_THROW(static_cast<void>(entities_->readNode(current, nodeId)), FormatException);
		EXPECT_FALSE(metadata_->findIndexRootOptional(current, 17U).has_value());
		current.release();
	}

	properties_.reset();
	labels_.reset();
	slotted_pages_.reset();
	entities_.reset();
	metadata_.reset();
	metadata_ = RelocatableMetadataStore::open(io_);
	entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
	slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
	labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_);
	properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);

	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(2U));
	EXPECT_FALSE(metadata_->findIndexRootOptional(recovered, 17U).has_value());
	EXPECT_THROW(static_cast<void>(entities_->readNode(recovered, 1U)), FormatException);
	recovered.release();
}

TEST_F(NativeGraphWritePlannerTest, RecoversNewGenerationWhenInitialScalarIndexPublicationStopsAfterSuperblock) {
	const std::array<uint64_t, 0U> noLabels{};
	const std::array<std::byte, 1U> key = {std::byte{'n'}};
	{
		BTreePageStore indexes(*slotted_pages_, *metadata_);
		NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_,
										plannerOptions());
		NativeGraphWriteTransaction transaction = planner.beginTransaction();
		const uint64_t nodeId = transaction.createNode(noLabels);
		const std::array<BTreePageEntry, 1U> entries{{{std::vector<std::byte>(key.begin(), key.end()), nodeId}}};
		transaction.stageScalarIndexCreate(indexes, 17U, entries);
		EXPECT_THROW(static_cast<void>(transaction.commit(
							 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
					 BootstrapPublishInterrupted);
		EXPECT_EQ(transaction.state(), NativeGraphWriteTransactionState::NGWTS_INDETERMINATE);
		MetadataSnapshotPin current = metadata_->openSnapshot();
		EXPECT_EQ(current.superblock().generation, Generation(3U));
		EXPECT_EQ(entities_->readNode(current, nodeId), NodeEntityRecord{});
		BTreeIndex index(17U, indexes, *metadata_);
		EXPECT_EQ(index.find(current, key), std::optional<uint64_t>(nodeId));
		current.release();
	}

	properties_.reset();
	labels_.reset();
	slotted_pages_.reset();
	entities_.reset();
	metadata_.reset();
	metadata_ = RelocatableMetadataStore::open(io_);
	entities_ = std::make_unique<EntityPageStore>(io_, *metadata_);
	slotted_pages_ = std::make_unique<SlottedPagePool>(io_, *metadata_);
	labels_ = std::make_unique<EntityLabelSetStore>(io_, *metadata_, *slotted_pages_);
	properties_ = std::make_unique<PropertyCollectionRootStore>(*slotted_pages_, *metadata_);

	BTreePageStore reopenedIndexes(*slotted_pages_, *metadata_);
	BTreeIndex reopenedIndex(17U, reopenedIndexes, *metadata_);
	MetadataSnapshotPin recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(3U));
	EXPECT_EQ(entities_->readNode(recovered, 1U), NodeEntityRecord{});
	EXPECT_EQ(reopenedIndex.find(recovered, key), std::optional<uint64_t>(1U));
	recovered.release();
}

TEST_F(NativeGraphWritePlannerTest, StagesScalarIndexMutationsWithNativeGraphWrites) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const IndexRootLocation root = allocateEmptyIndexRoot(indexes);
	BTreeIndex index(root.index_id, indexes, *metadata_);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const std::array<uint64_t, 0U> noLabels{};
	const std::array<std::byte, 1U> key = {std::byte{'s'}};
	MetadataSnapshotPin before = metadata_->openSnapshot();
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(noLabels);
	create.stageScalarIndexUpsert(indexes, root.index_id, key, nodeId);
	const NativeGraphWriteCommitResult created = create.commit();
	ASSERT_TRUE(created.committed());
	EXPECT_EQ(created.superblock.generation, Generation(5U));
	EXPECT_THROW(static_cast<void>(entities_->readNode(before, nodeId)), FormatException);
	EXPECT_FALSE(index.find(before, key).has_value());
	MetadataSnapshotPin afterCreate = metadata_->openSnapshot();
	EXPECT_EQ(entities_->readNode(afterCreate, nodeId), NodeEntityRecord{});
	EXPECT_EQ(index.find(afterCreate, key), std::optional<uint64_t>(nodeId));
	afterCreate.release();
	before.release();

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	erase.stageScalarIndexErase(indexes, root.index_id, key);
	const NativeGraphWriteCommitResult erased = erase.commit();
	ASSERT_TRUE(erased.committed());
	EXPECT_EQ(erased.superblock.generation, Generation(6U));
	MetadataSnapshotPin afterErase = metadata_->openSnapshot();
	EXPECT_EQ(entities_->readNode(afterErase, nodeId), NodeEntityRecord{});
	EXPECT_FALSE(index.find(afterErase, key).has_value());
	afterErase.release();
}

TEST_F(NativeGraphWritePlannerTest, MaintainsNativeLabelTypeAndPropertyIndexesAcrossGraphLifecycle) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const IndexRootLocation nodeLabels = allocateEmptyIndexRoot(indexes, 17U, SegmentId(71U), PageId(101U),
																PhysicalSlotId(40U), LogSequenceNumber(2U));
	const IndexRootLocation nodeProperty = allocateEmptyIndexRoot(indexes, 18U, SegmentId(72U), PageId(102U),
																  PhysicalSlotId(41U), LogSequenceNumber(4U));
	const IndexRootLocation nodeComposite = allocateEmptyIndexRoot(indexes, 19U, SegmentId(73U), PageId(103U),
																   PhysicalSlotId(42U), LogSequenceNumber(6U));
	const IndexRootLocation edgeTypes = allocateEmptyIndexRoot(indexes, 20U, SegmentId(74U), PageId(104U),
															   PhysicalSlotId(43U), LogSequenceNumber(8U));
	const IndexRootLocation edgeProperty = allocateEmptyIndexRoot(indexes, 21U, SegmentId(75U), PageId(105U),
																  PhysicalSlotId(44U), LogSequenceNumber(10U));

	IndexDefinition nodeLabelDefinition;
	nodeLabelDefinition.index_id = nodeLabels.index_id;
	nodeLabelDefinition.name = "node-labels";
	nodeLabelDefinition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	nodeLabelDefinition.kind = IndexDefinitionKind::IDK_LABEL;
	IndexDefinition nodePropertyDefinition;
	nodePropertyDefinition.index_id = nodeProperty.index_id;
	nodePropertyDefinition.name = "node-score";
	nodePropertyDefinition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	nodePropertyDefinition.kind = IndexDefinitionKind::IDK_PROPERTY;
	nodePropertyDefinition.label_token_id = 7U;
	nodePropertyDefinition.property_names = {"score"};
	IndexDefinition nodeCompositeDefinition;
	nodeCompositeDefinition.index_id = nodeComposite.index_id;
	nodeCompositeDefinition.name = "node-score-name";
	nodeCompositeDefinition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	nodeCompositeDefinition.kind = IndexDefinitionKind::IDK_COMPOSITE;
	nodeCompositeDefinition.label_token_id = 9U;
	nodeCompositeDefinition.property_names = {"score", "name"};
	IndexDefinition edgeTypeDefinition;
	edgeTypeDefinition.index_id = edgeTypes.index_id;
	edgeTypeDefinition.name = "edge-types";
	edgeTypeDefinition.entity_kind = IndexDefinitionEntityKind::IDEK_EDGE;
	edgeTypeDefinition.kind = IndexDefinitionKind::IDK_LABEL;
	IndexDefinition edgePropertyDefinition;
	edgePropertyDefinition.index_id = edgeProperty.index_id;
	edgePropertyDefinition.name = "edge-weight";
	edgePropertyDefinition.entity_kind = IndexDefinitionEntityKind::IDEK_EDGE;
	edgePropertyDefinition.kind = IndexDefinitionKind::IDK_PROPERTY;
	edgePropertyDefinition.property_names = {"weight"};

	NativeScalarIndexMutationPlanner scalar(
			*metadata_, indexes,
			{nodeLabelDefinition, nodePropertyDefinition, nodeCompositeDefinition, edgeTypeDefinition,
			 edgePropertyDefinition},
			[](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
				if (name == "score") {
					return 17U;
				}
				if (name == "name") {
					return 18U;
				}
				if (name == "weight") {
					return 19U;
				}
				return std::nullopt;
			});
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const std::array<uint64_t, 2U> firstLabels = {7U, 9U};
	const std::array<uint64_t, 1U> secondLabels = {11U};
	const PropertyCollectionRoot initialNode = {
			1U, Node::typeId, {{17U, PropertyValue(int64_t(42))}, {18U, PropertyValue(std::string("alpha"))}}};
	const PropertyCollectionRoot initialEdge = {1U, Edge::typeId, {{19U, PropertyValue(int64_t(8))}}};

	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t firstNode = create.createNode(firstLabels);
	const uint64_t secondNode = create.createNode(secondLabels);
	const uint64_t edge = create.createEdge(firstNode, secondNode, 23U);
	ASSERT_EQ(firstNode, initialNode.owner_entity_id);
	ASSERT_EQ(edge, initialEdge.owner_entity_id);
	scalar.stageNodeLabels(create, firstNode, firstLabels, false);
	scalar.stageNodeLabels(create, secondNode, secondLabels, false);
	scalar.stageProperties(create, IndexDefinitionEntityKind::IDEK_NODE, firstNode, firstLabels, initialNode, false);
	scalar.stageEdgeType(create, edge, 23U, false);
	scalar.stageProperties(create, IndexDefinitionEntityKind::IDEK_EDGE, edge, {}, initialEdge, false);
	create.replaceNodeProperty(initialNode);
	create.replaceEdgeProperty(initialEdge);
	ASSERT_TRUE(create.commit().committed());

	const std::vector<std::byte> labelSeven = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(7)), firstNode);
	const std::vector<std::byte> labelNine = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(9)), firstNode);
	const std::vector<std::byte> scoreFortyTwo = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(42)), firstNode);
	const std::array<PropertyValue, 2U> compositeValues = {PropertyValue(int64_t(42)),
														   PropertyValue(std::string("alpha"))};
	const std::vector<std::byte> scoreAndName = IndexKeyCodec::encodeCompositeEntryKey(compositeValues, firstNode);
	const std::vector<std::byte> typeTwentyThree = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(23)), edge);
	const std::vector<std::byte> weightEight = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(8)), edge);
	BTreeIndex labelIndex(nodeLabels.index_id, indexes, *metadata_);
	BTreeIndex nodePropertyIndex(nodeProperty.index_id, indexes, *metadata_);
	BTreeIndex nodeCompositeIndex(nodeComposite.index_id, indexes, *metadata_);
	BTreeIndex edgeTypeIndex(edgeTypes.index_id, indexes, *metadata_);
	BTreeIndex edgePropertyIndex(edgeProperty.index_id, indexes, *metadata_);
	MetadataSnapshotPin afterCreate = metadata_->openSnapshot();
	EXPECT_EQ(labelIndex.find(afterCreate, labelSeven), std::optional<uint64_t>(firstNode));
	EXPECT_EQ(labelIndex.find(afterCreate, labelNine), std::optional<uint64_t>(firstNode));
	EXPECT_EQ(nodePropertyIndex.find(afterCreate, scoreFortyTwo), std::optional<uint64_t>(firstNode));
	EXPECT_EQ(nodeCompositeIndex.find(afterCreate, scoreAndName), std::optional<uint64_t>(firstNode));
	EXPECT_EQ(edgeTypeIndex.find(afterCreate, typeTwentyThree), std::optional<uint64_t>(edge));
	EXPECT_EQ(edgePropertyIndex.find(afterCreate, weightEight), std::optional<uint64_t>(edge));
	afterCreate.release();

	const PropertyCollectionRoot replacementNode = {
			firstNode, Node::typeId, {{17U, PropertyValue(int64_t(84))}, {18U, PropertyValue(std::string("alpha"))}}};
	NativeGraphWriteTransaction replace = planner.beginTransaction();
	scalar.stageProperties(replace, IndexDefinitionEntityKind::IDEK_NODE, firstNode, firstLabels, initialNode, true);
	scalar.stageProperties(replace, IndexDefinitionEntityKind::IDEK_NODE, firstNode, firstLabels, replacementNode,
						   false);
	replace.replaceNodeProperty(replacementNode);
	ASSERT_TRUE(replace.commit().committed());
	const std::vector<std::byte> scoreEightyFour = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(84)), firstNode);
	const std::array<PropertyValue, 2U> replacementComposite = {PropertyValue(int64_t(84)),
																PropertyValue(std::string("alpha"))};
	const std::vector<std::byte> replacementScoreAndName =
			IndexKeyCodec::encodeCompositeEntryKey(replacementComposite, firstNode);
	MetadataSnapshotPin afterReplace = metadata_->openSnapshot();
	EXPECT_FALSE(nodePropertyIndex.find(afterReplace, scoreFortyTwo).has_value());
	EXPECT_FALSE(nodeCompositeIndex.find(afterReplace, scoreAndName).has_value());
	EXPECT_EQ(nodePropertyIndex.find(afterReplace, scoreEightyFour), std::optional<uint64_t>(firstNode));
	EXPECT_EQ(nodeCompositeIndex.find(afterReplace, replacementScoreAndName), std::optional<uint64_t>(firstNode));
	afterReplace.release();

	const std::array<uint64_t, 1U> retainedLabel = {9U};
	NativeGraphWriteTransaction replaceLabels = planner.beginTransaction();
	scalar.stageNodeLabels(replaceLabels, firstNode, firstLabels, true);
	scalar.stageProperties(replaceLabels, IndexDefinitionEntityKind::IDEK_NODE, firstNode, firstLabels, replacementNode,
						   true);
	replaceLabels.replaceNodeLabels(firstNode, retainedLabel);
	scalar.stageNodeLabels(replaceLabels, firstNode, retainedLabel, false);
	scalar.stageProperties(replaceLabels, IndexDefinitionEntityKind::IDEK_NODE, firstNode, retainedLabel,
						   replacementNode, false);
	ASSERT_TRUE(replaceLabels.commit().committed());
	MetadataSnapshotPin afterLabels = metadata_->openSnapshot();
	EXPECT_FALSE(labelIndex.find(afterLabels, labelSeven).has_value());
	EXPECT_EQ(labelIndex.find(afterLabels, labelNine), std::optional<uint64_t>(firstNode));
	EXPECT_FALSE(nodePropertyIndex.find(afterLabels, scoreEightyFour).has_value());
	EXPECT_EQ(nodeCompositeIndex.find(afterLabels, replacementScoreAndName), std::optional<uint64_t>(firstNode));
	afterLabels.release();

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	scalar.stageProperties(erase, IndexDefinitionEntityKind::IDEK_EDGE, edge, {}, initialEdge, true);
	scalar.stageEdgeType(erase, edge, 23U, true);
	erase.eraseEdge(edge);
	scalar.stageProperties(erase, IndexDefinitionEntityKind::IDEK_NODE, firstNode, retainedLabel, replacementNode,
						   true);
	scalar.stageNodeLabels(erase, firstNode, retainedLabel, true);
	erase.eraseNode(firstNode);
	ASSERT_TRUE(erase.commit().committed());
	MetadataSnapshotPin afterErase = metadata_->openSnapshot();
	EXPECT_FALSE(labelIndex.find(afterErase, labelNine).has_value());
	EXPECT_FALSE(nodeCompositeIndex.find(afterErase, replacementScoreAndName).has_value());
	EXPECT_FALSE(edgeTypeIndex.find(afterErase, typeTwentyThree).has_value());
	EXPECT_FALSE(edgePropertyIndex.find(afterErase, weightEight).has_value());
	afterErase.release();
}

TEST_F(NativeGraphWritePlannerTest, NativeScalarIndexPlannerRejectsMalformedInputAndDefersBuildPendingIndexes) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	IndexDefinition pending;
	pending.index_id = 17U;
	pending.name = "pending-node-labels";
	pending.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
	pending.kind = IndexDefinitionKind::IDK_LABEL;
	EXPECT_THROW(static_cast<void>(NativeScalarIndexMutationPlanner(*metadata_, indexes, {pending}, {})),
				 std::invalid_argument);
	NativeScalarIndexMutationPlanner scalar(
			*metadata_, indexes, {pending},
			[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return std::nullopt; });
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	const uint64_t nodeId = transaction.createNode(std::array<uint64_t, 1U>{7U});
	// An index definition may be visible while its online/bulk build has not yet
	// published a root. The mutation is deliberately deferred; the builder scans
	// this committed Node before publishing the initial tree.
	scalar.stageNodeLabels(transaction, nodeId, std::array<uint64_t, 1U>{7U}, false);
	ASSERT_TRUE(transaction.commit().committed());
	NativeGraphWriteTransaction invalidLabels = planner.beginTransaction();
	EXPECT_THROW(scalar.stageNodeLabels(invalidLabels, nodeId, std::array<uint64_t, 2U>{9U, 7U}, false),
				 FormatException);
	PropertyCollectionRoot wrongOwner = {nodeId, Edge::typeId, {{17U, PropertyValue(int64_t(1))}}};
	NativeGraphWriteTransaction invalid = planner.beginTransaction();
	EXPECT_THROW(scalar.stageProperties(invalid, IndexDefinitionEntityKind::IDEK_NODE, nodeId,
										std::array<uint64_t, 1U>{7U}, wrongOwner, false),
				 FormatException);
}

TEST_F(NativeGraphWritePlannerTest, StagesNativeConstraintSupportTreeMutationsWithTheGraphGeneration) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const uint64_t constraintId = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
	const IndexRootLocation root = allocateEmptyIndexRoot(indexes, constraintId);
	NativeScalarIndexMutationPlanner scalar(
			*metadata_, indexes, {},
			[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return std::nullopt; });
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const std::array<PropertyValue, 1U> values = {PropertyValue(int64_t(42))};
	const std::vector<std::byte> canonical = ConstraintSupportTree::canonicalKey(values);
	const std::vector<std::byte> key = ConstraintSupportTree::entryKey(canonical, 77U);

	NativeGraphWriteTransaction insert = planner.beginTransaction();
	scalar.stageConstraintSupportUpsert(insert, constraintId, canonical, 77U);
	ASSERT_TRUE(insert.commit().committed());
	BTreeIndex support(constraintId, indexes, *metadata_);
	MetadataSnapshotPin afterInsert = metadata_->openSnapshot();
	EXPECT_EQ(metadata_->findIndexRoot(afterInsert, constraintId), root);
	EXPECT_EQ(support.find(afterInsert, key), std::optional<uint64_t>(77U));
	afterInsert.release();

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	scalar.stageConstraintSupportErase(erase, constraintId, canonical, 77U);
	ASSERT_TRUE(erase.commit().committed());
	MetadataSnapshotPin afterErase = metadata_->openSnapshot();
	EXPECT_FALSE(support.find(afterErase, key).has_value());
	afterErase.release();

	NativeGraphWriteTransaction missingRoot = planner.beginTransaction();
	EXPECT_THROW(scalar.stageConstraintSupportUpsert(missingRoot, constraintId + 1U, canonical, 77U), FormatException);
}

TEST_F(NativeGraphWritePlannerTest, AdaptsOwnerKeyedNativePagesForSharedConstraintValidation) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const uint64_t constraintId = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
	static_cast<void>(allocateEmptyIndexRoot(indexes, constraintId));
	NativeScalarIndexMutationPlanner scalar(
			*metadata_, indexes, {},
			[](const MetadataSnapshotPin &, std::string_view) -> std::optional<uint32_t> { return std::nullopt; });
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 1U>{7U});
	const PropertyCollectionRoot initial = {
			nodeId, Node::typeId, {{17U, PropertyValue(std::string("old@example.test"))}}};
	create.replaceNodeProperty(initial);
	const std::vector<std::byte> oldKey = ConstraintSupportTree::canonicalKey(
			std::array<PropertyValue, 1U>{PropertyValue(std::string("old@example.test"))});
	scalar.stageConstraintSupportUpsert(create, constraintId, oldKey, nodeId);
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	PageBackedConstraintMutationAdapter native(
			snapshot, graph, indexes, *metadata_,
			[](const MetadataSnapshotPin &, uint32_t token) {
				if (token != 17U)
					throw FormatException(FormatError::FERR_INVALID_STATE, "unexpected Property Token");
				return std::string("email");
			},
			[](const MetadataSnapshotPin &, std::string_view name) {
				if (name != "Person")
					throw FormatException(FormatError::FERR_INVALID_STATE, "unexpected schema name");
				return uint64_t{7U};
			},
			[](const MetadataSnapshotPin &, std::string_view name) {
				if (name != "Person")
					throw FormatException(FormatError::FERR_INVALID_STATE, "unexpected schema name");
				return uint64_t{7U};
			});
	ConstraintMutationValidator validator(native.makeAdapter(), {16U, 4U});
	ConstraintDefinition unique;
	unique.constraint_id = constraintId;
	unique.name = "person_email_unique";
	unique.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	unique.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	unique.schema_token_id = 7U;
	unique.property_names = {"email"};
	LogicalGraphMutationBatch batch;
	batch.property_replacements.push_back(
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
			 {{"email", PropertyValue(std::string("new@example.test"))}}});
	const std::vector<ConstraintSupportTreeMutationPlan> plans =
			validator.validate(std::array<ConstraintDefinition, 1U>{unique}, batch);
	ASSERT_EQ(plans.size(), 1U);
	EXPECT_EQ(plans.front().entity_reference_kind, ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_NODE);
	EXPECT_EQ(plans.front().entity_reference_value, nodeId);
	EXPECT_EQ(plans.front().erased_canonical_key, std::optional{oldKey});
	EXPECT_EQ(plans.front().upserted_canonical_key,
			  std::optional{ConstraintSupportTree::canonicalKey(
					  std::array<PropertyValue, 1U>{PropertyValue(std::string("new@example.test"))})});
	snapshot.release();
}

TEST_F(NativeGraphWritePlannerTest, PublishesScalarIndexesIntoDistinctFramesOfOneSharedCurrentPool) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const IndexRootLocation firstRoot = allocateEmptyIndexRoot(indexes);
	SlottedPagePoolAppendRequest secondRequest;
	secondRequest.segment_id = SegmentId(71U);
	secondRequest.generation = Generation(1U);
	secondRequest.page_id = PageId(102U);
	secondRequest.page_owner_id = 18U;
	secondRequest.page_owner_kind = PageOwnerKind::POK_INDEX;
	PageOwnerHeader secondOwner;
	secondOwner.page_id = secondRequest.page_id;
	secondOwner.owner_id = secondRequest.page_owner_id;
	secondOwner.kind = secondRequest.page_owner_kind;
	secondOwner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	secondOwner.generation = secondRequest.generation;
	secondOwner.committed_lsn = LogSequenceNumber(4U);
	BTreePageHeader secondHeader;
	secondHeader.index_id = secondRequest.page_owner_id;
	secondHeader.page_id = secondRequest.page_id;
	secondHeader.generation = secondRequest.generation;
	static_cast<void>(
			indexes.append(secondRequest, BTreePage::create(secondOwner, secondHeader), LogSequenceNumber(4U)));
	const IndexRootLocation secondRoot{18U, secondRequest.page_id, secondRequest.generation};
	static_cast<void>(indexes.publishRoots(std::span(&secondRoot, 1U), {}, LogSequenceNumber(5U)));

	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	const std::array<std::byte, 1U> firstKey = {std::byte{'f'}};
	const std::array<std::byte, 1U> secondKey = {std::byte{'s'}};
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	// Stage in reverse IndexId order: the native mutation must canonicalize the
	// metadata publication while retaining each Index's operation order.
	transaction.stageScalarIndexUpsert(indexes, secondRoot.index_id, secondKey, 2002U);
	transaction.stageScalarIndexUpsert(indexes, firstRoot.index_id, firstKey, 1001U);
	const NativeGraphWriteCommitResult committed = transaction.commit();
	ASSERT_TRUE(committed.committed());
	EXPECT_EQ(committed.superblock.generation, Generation(6U));

	BTreeIndex first(firstRoot.index_id, indexes, *metadata_);
	BTreeIndex second(secondRoot.index_id, indexes, *metadata_);
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_EQ(first.find(current, firstKey), std::optional<uint64_t>(1001U));
	EXPECT_EQ(second.find(current, secondKey), std::optional<uint64_t>(2002U));
	// Root PageIds stay stable for ordinary COW replacements. The distinct
	// physical locations prove the two callbacks received separate frames from
	// their shared Index pool instead of overwriting one another.
	const PageLocation firstLocation =
			metadata_->findPageLocation(current, metadata_->findIndexRoot(current, firstRoot.index_id).root_page_id);
	const PageLocation secondLocation = metadata_->findPageLocation(
			current, metadata_->findIndexRoot(current, secondRoot.index_id).root_page_id);
	EXPECT_EQ(firstLocation.segment_id, SegmentId(71U));
	EXPECT_EQ(secondLocation.segment_id, SegmentId(71U));
	EXPECT_NE(firstLocation.page_index, secondLocation.page_index);
	current.release();
}

TEST_F(NativeGraphWritePlannerTest, ReclaimsFramesBeforeSharingAConstrainedCurrentPool) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const IndexRootLocation firstRoot = allocateEmptyIndexRoot(indexes);
	SlottedPagePoolAppendRequest secondRequest;
	secondRequest.segment_id = SegmentId(71U);
	secondRequest.generation = Generation(1U);
	secondRequest.page_id = PageId(102U);
	secondRequest.page_owner_id = 18U;
	secondRequest.page_owner_kind = PageOwnerKind::POK_INDEX;
	PageOwnerHeader secondOwner;
	secondOwner.page_id = secondRequest.page_id;
	secondOwner.owner_id = secondRequest.page_owner_id;
	secondOwner.kind = secondRequest.page_owner_kind;
	secondOwner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	secondOwner.generation = secondRequest.generation;
	secondOwner.committed_lsn = LogSequenceNumber(4U);
	BTreePageHeader secondHeader;
	secondHeader.index_id = secondRequest.page_owner_id;
	secondHeader.page_id = secondRequest.page_id;
	secondHeader.generation = secondRequest.generation;
	static_cast<void>(
			indexes.append(secondRequest, BTreePage::create(secondOwner, secondHeader), LogSequenceNumber(4U)));
	const IndexRootLocation secondRoot{18U, secondRequest.page_id, secondRequest.generation};
	static_cast<void>(indexes.publishRoots(std::span(&secondRoot, 1U), {}, LogSequenceNumber(5U)));

	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	MetadataSnapshotPin pinned = metadata_->openSnapshot();
	const std::array<std::byte, 1U> firstKey = {std::byte{'f'}};
	for (size_t ordinal = 0U; ordinal < RSS_PAGE_POOL_DATA_FRAME_COUNT - 3U; ++ordinal) {
		NativeGraphWriteTransaction update = planner.beginTransaction();
		update.stageScalarIndexUpsert(indexes, firstRoot.index_id, firstKey, ordinal + 1U);
		ASSERT_TRUE(update.commit().committed());
	}

	MetadataSnapshotPin beforeSharedMutation = metadata_->openSnapshot();
	const CatalogEntry originalPool = metadata_->findCatalog(beforeSharedMutation, SegmentId(71U));
	ASSERT_TRUE(indexes.hasReplacementFrameCapacity(originalPool, 1U));
	ASSERT_FALSE(indexes.hasReplacementFrameCapacity(originalPool, 2U));
	beforeSharedMutation.release();

	const std::array<std::byte, 1U> secondKey = {std::byte{'s'}};
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	// The old snapshot retains the original root frame but not intermediate COW
	// versions. The graph planner must reclaim those now-unreachable frames
	// before budgeting both scalar callbacks against this shared current pool.
	transaction.stageScalarIndexUpsert(indexes, secondRoot.index_id, secondKey, 2002U);
	transaction.stageScalarIndexUpsert(indexes, firstRoot.index_id, firstKey, 1001U);
	ASSERT_TRUE(transaction.commit().committed());

	MetadataSnapshotPin current = metadata_->openSnapshot();
	const IndexRootLocation currentFirst = metadata_->findIndexRoot(current, firstRoot.index_id);
	const IndexRootLocation currentSecond = metadata_->findIndexRoot(current, secondRoot.index_id);
	const bool firstUsesOriginalPool =
			metadata_->findPageLocation(current, currentFirst.root_page_id).segment_id == originalPool.segment_id;
	const bool secondUsesOriginalPool =
			metadata_->findPageLocation(current, currentSecond.root_page_id).segment_id == originalPool.segment_id;
	EXPECT_TRUE(firstUsesOriginalPool);
	EXPECT_TRUE(secondUsesOriginalPool);
	BTreeIndex first(firstRoot.index_id, indexes, *metadata_);
	BTreeIndex second(secondRoot.index_id, indexes, *metadata_);
	EXPECT_EQ(first.find(current, firstKey), std::optional<uint64_t>(1001U));
	EXPECT_EQ(second.find(current, secondKey), std::optional<uint64_t>(2002U));
	current.release();
	pinned.release();
}

TEST_F(NativeGraphWritePlannerTest, PublishesStructuralScalarIndexSplitThroughNativeTransaction) {
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const IndexRootLocation root = allocateEmptyIndexRoot(indexes);
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction transaction = planner.beginTransaction();
	std::vector<std::vector<std::byte>> keys;
	keys.reserve(96U);
	for (uint16_t ordinal = 0U; ordinal < 96U; ++ordinal) {
		std::vector<std::byte> key(48U, std::byte{'k'});
		key[0] = static_cast<std::byte>(ordinal >> 8U);
		key[1] = static_cast<std::byte>(ordinal & 0xFFU);
		keys.push_back(std::move(key));
		transaction.stageScalarIndexUpsert(indexes, root.index_id, keys.back(), ordinal + 1U);
	}
	const NativeGraphWriteCommitResult committed = transaction.commit();
	ASSERT_TRUE(committed.committed());
	EXPECT_EQ(committed.superblock.generation, Generation(4U));

	BTreeIndex index(root.index_id, indexes, *metadata_);
	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_NE(metadata_->findIndexRoot(current, root.index_id).root_page_id, root.root_page_id);
	for (uint16_t ordinal = 0U; ordinal < keys.size(); ++ordinal) {
		EXPECT_EQ(index.find(current, keys[ordinal]), std::optional<uint64_t>(ordinal + 1U));
	}
	current.release();

	NativeGraphWriteTransaction erase = planner.beginTransaction();
	for (size_t ordinal = 0U; ordinal + 1U < keys.size(); ++ordinal) {
		erase.stageScalarIndexErase(indexes, root.index_id, keys[ordinal]);
	}
	const NativeGraphWriteCommitResult erased = erase.commit();
	ASSERT_TRUE(erased.committed());
	EXPECT_EQ(erased.superblock.generation, Generation(5U));
	MetadataSnapshotPin collapsed = metadata_->openSnapshot();
	const IndexRootLocation collapsedRoot = metadata_->findIndexRoot(collapsed, root.index_id);
	EXPECT_EQ(indexes.read(collapsed, collapsedRoot.root_page_id).header().kind, BTreeNodeKind::BPN_LEAF);
	EXPECT_EQ(index.find(collapsed, keys.back()), std::optional<uint64_t>(96U));
	EXPECT_FALSE(index.find(collapsed, keys.front()).has_value());
	collapsed.release();
}
