/**
 * @file test_NativeGraphMutationExecutor_Validation.cpp
 * @brief Public logical mutation validation tests for the native graph executor.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include "graph/debug/PerfTrace.hpp"
#include "storage/relocatable/property/PropertyCollectionRootTestUtils.hpp"
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/PropertyTypes.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
using graph::PropertyValue;

namespace {

	NativeGraphWritePlannerOptions plannerOptions() {
		NativeGraphWritePlannerOptions options;
		options.first_slot = PhysicalSlotId(40U);
		options.last_slot = PhysicalSlotId(80U);
		options.entity_pool_owner_id = 901U;
		options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
		return options;
	}

	NativeGraphMutationExecutorOptions executorOptions() {
		NativeGraphMutationExecutorOptions options;
		options.maximum_index_definitions = 8U;
		options.maximum_constraint_definitions = 8U;
		options.maximum_constraint_entity_scan = 64U;
		options.maximum_constraint_support_collisions = 8U;
		options.maximum_vector_records = 64U;
		options.maximum_vector_pages = 8U;
		options.vector_delta_rebuild_threshold = 8U;
		options.maximum_vector_delta_overlay_entries = 16U;
		options.maximum_vector_delta_pages = 4U;
		return options;
	}

	NativeGraphMutationTokenResolvers tokens(bool duplicateLabels = false, bool resolveProperties = true) {
		NativeGraphMutationTokenResolvers result;
		result.schema_token = [duplicateLabels](const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind,
												std::string_view name) -> std::optional<uint64_t> {
			if (name == "Person")
				return 7U;
			if (name == "Employee")
				return duplicateLabels ? std::optional<uint64_t>(7U) : std::optional<uint64_t>(9U);
			if (name == "KNOWS")
				return 11U;
			return std::nullopt;
		};
		result.property_key_token = [resolveProperties](const MetadataSnapshotPin &,
														std::string_view name) -> std::optional<uint32_t> {
			return resolveProperties && name == "age" ? std::optional<uint32_t>(17U) : std::nullopt;
		};
		result.property_key_name = [](const MetadataSnapshotPin &, uint32_t token) -> std::optional<std::string> {
			return token == 17U ? std::optional<std::string>("age") : std::nullopt;
		};
		return result;
	}

	IndexDefinition vectorDefinition() {
		return {77U,
				"native-vector",
				IndexDefinitionEntityKind::IDEK_NODE,
				IndexDefinitionKind::IDK_VECTOR,
				7U,
				{"embedding"},
				2U,
				"l2"};
	}

	NativeGraphMutationTokenResolvers vectorTokens() {
		NativeGraphMutationTokenResolvers result = tokens();
		result.property_key_token = [](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
			return name == "embedding" ? std::optional<uint32_t>(17U) : std::nullopt;
		};
		result.property_key_name = [](const MetadataSnapshotPin &, uint32_t token) -> std::optional<std::string> {
			return token == 17U ? std::optional<std::string>("embedding") : std::nullopt;
		};
		return result;
	}

	LogicalGraphMutationBatch vectorCreateBatch() {
		LogicalGraphMutationBatch batch;
		batch.node_creates = {{{"Person"},
							   {{"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1)),
																					   PropertyValue(int64_t(2))})}}}};
		return batch;
	}

	LogicalGraphMutationEntityReference createdNodeReference(uint64_t ordinal) {
		return {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE,
				ordinal};
	}

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	void allocateUniqueConstraintSupportRoot(BTreePageStore &indexes, uint64_t constraintId) {
		SlottedPagePoolAllocationRequest allocation;
		allocation.segment_id = SegmentId(71U);
		allocation.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
		allocation.pool_owner_id = constraintId;
		allocation.generation = Generation(1U);
		allocation.page_id = PageId(101U);
		allocation.page_owner_id = constraintId;
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
		const bool allocated = indexes
							  .allocate(allocation, BTreePage::create(owner, header), PhysicalSlotId(40U), PhysicalSlotId(40U),
										LogSequenceNumber(2U))
							  .has_value();
		if (!allocated) {
			throw std::runtime_error("constraint support-tree root allocation unexpectedly ran out of space");
		}
		const IndexRootLocation root{constraintId, allocation.page_id, allocation.generation};
		static_cast<void>(indexes.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));
	}

} // namespace

class NativeGraphMutationExecutorValidationTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {};

TEST_F(NativeGraphMutationExecutorValidationTest,
	   RejectsInvalidBatchReferencesAndConflictingFinalStatesBeforePublishingGraphData) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation beforeGeneration = before.superblock().generation;
	before.release();

	LogicalGraphMutationBatch invalidCreatedReference;
	invalidCreatedReference.edge_creates = {{createdNodeReference(0U), createdNodeReference(1U), "KNOWS", {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(invalidCreatedReference)), std::invalid_argument);

	LogicalGraphMutationBatch duplicateProperties;
	const LogicalGraphMutationEntityReference persistedNode{LogicalGraphMutationEntityKind::LGMK_NODE,
															LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U};
	duplicateProperties.property_replacements.push_back({persistedNode, {{"age", PropertyValue(int64_t(1))}}});
	duplicateProperties.property_replacements.push_back({persistedNode, {{"age", PropertyValue(int64_t(2))}}});
	EXPECT_THROW(static_cast<void>(executor.execute(duplicateProperties)), std::invalid_argument);

	LogicalGraphMutationBatch wrongLabelTarget;
	wrongLabelTarget.node_label_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
			 {"Person"}},
	};
	EXPECT_THROW(static_cast<void>(executor.execute(wrongLabelTarget)), std::invalid_argument);

	LogicalGraphMutationBatch duplicateDeletes;
	duplicateDeletes.node_deletes = {{1U, false, 0U}, {1U, false, 0U}};
	EXPECT_THROW(static_cast<void>(executor.execute(duplicateDeletes)), std::invalid_argument);

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock().generation, beforeGeneration);
	after.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   CachesStablePropertyTokenResolutionAcrossBulkEdgeValidationAndMaterialization) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationTokenResolvers resolver = tokens();
	size_t propertyTokenLookups = 0U;
	resolver.property_key_token = [&propertyTokenLookups](const MetadataSnapshotPin &,
																 std::string_view name) -> std::optional<uint32_t> {
		++propertyTokenLookups;
		return name == "age" ? std::optional<uint32_t>(17U) : std::nullopt;
	};
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, std::move(resolver),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Person"}, {}}, {{"Person"}, {}}};
	batch.edge_creates = {
			{createdNodeReference(0U), createdNodeReference(1U), "KNOWS", {{"age", PropertyValue(int64_t(1))}}},
			{createdNodeReference(1U), createdNodeReference(0U), "KNOWS", {{"age", PropertyValue(int64_t(2))}}}};

	const NativeGraphMutationExecutionResult result = executor.execute(batch);
	ASSERT_TRUE(result.committed());
	EXPECT_EQ(result.result.created_edge_ids.size(), 2U);
	EXPECT_EQ(propertyTokenLookups, 1U);
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   EnforcesTheOptionalReadSnapshotFenceBeforePublishingLogicalGraphChanges) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch rejected;
	rejected.node_creates = {{{"Person"}, {}}};
	bool rejectedFenceWasChecked = false;
	EXPECT_THROW(
			static_cast<void>(executor.execute(rejected, [&](const Superblock &) {
				rejectedFenceWasChecked = true;
				return false;
			})),
			LogicalGraphMutationConflict);
	EXPECT_TRUE(rejectedFenceWasChecked);
	MetadataSnapshotPin afterRejectedFence = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(afterRejectedFence, 1U).has_value());
	afterRejectedFence.release();

	LogicalGraphMutationBatch accepted;
	accepted.node_creates = {{{"Person"}, {}}};
	bool acceptedFenceWasChecked = false;
	const NativeGraphMutationExecutionResult executed = executor.execute(accepted, [&](const Superblock &base) {
		acceptedFenceWasChecked = true;
		return base.generation.isValid() && base.checkpoint_lsn.isValid();
	});
	ASSERT_TRUE(executed.committed());
	ASSERT_TRUE(acceptedFenceWasChecked);
	ASSERT_EQ(executed.result.created_node_ids.size(), 1U);
	MetadataSnapshotPin afterAcceptedFence = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findNodePageLocationOptional(afterAcceptedFence, executed.result.created_node_ids.front())
						.has_value());
	afterAcceptedFence.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   RejectsUnresolvableAndAliasedTokensWithoutPublishingPartialLogicalMutations) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor missingPropertyExecutor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(false, false),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch unknownProperty;
	unknownProperty.node_creates = {{{"Person"}, {{"age", PropertyValue(int64_t(1))}}}};
	EXPECT_THROW(static_cast<void>(missingPropertyExecutor.execute(unknownProperty)), FormatException);

	NativeGraphMutationExecutor duplicateLabelExecutor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(true),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());
	LogicalGraphMutationBatch aliasesOneLabelToken;
	aliasesOneLabelToken.node_creates = {{{"Person", "Employee"}, {}}};
	EXPECT_THROW(static_cast<void>(duplicateLabelExecutor.execute(aliasesOneLabelToken)), FormatException);

	MetadataSnapshotPin current = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(current, 1U).has_value());
	current.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   RejectsInvalidCreationPlansAndFinalStateCombinationsWithoutAdvancingTheGeneration) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation generation = before.superblock().generation;
	before.release();
	const LogicalGraphMutationEntityReference persistedNode{LogicalGraphMutationEntityKind::LGMK_NODE,
															LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U};

	LogicalGraphMutationBatch unleasedCreate;
	unleasedCreate.node_creates = {{{"Person"}, {}}};
	unleasedCreate.creation_identity_plan = LogicalGraphCreationIdentityPlan{{}, {1U}, {}, {}};
	EXPECT_THROW(static_cast<void>(executor.execute(unleasedCreate)), std::invalid_argument);

	LogicalGraphMutationBatch edgeTargetsDeletedNode;
	edgeTargetsDeletedNode.node_deletes = {{1U, false, 0U}};
	edgeTargetsDeletedNode.edge_creates = {{persistedNode, persistedNode, "KNOWS", {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(edgeTargetsDeletedNode)), std::invalid_argument);

	LogicalGraphMutationBatch replacementDuplicatesCreatedProperties;
	replacementDuplicatesCreatedProperties.node_creates = {{{"Person"}, {{"age", PropertyValue(int64_t(1))}}}};
	replacementDuplicatesCreatedProperties.property_replacements.push_back(
			{createdNodeReference(0U), {{"age", PropertyValue(int64_t(2))}}});
	EXPECT_THROW(static_cast<void>(executor.execute(replacementDuplicatesCreatedProperties)), std::invalid_argument);

	LogicalGraphMutationBatch replacementLabelsDeletedNode;
	replacementLabelsDeletedNode.node_deletes = {{1U, false, 0U}};
	replacementLabelsDeletedNode.node_label_replacements = {{persistedNode, {"Person"}}};
	EXPECT_THROW(static_cast<void>(executor.execute(replacementLabelsDeletedNode)), std::invalid_argument);

	LogicalGraphMutationBatch duplicatePropertyNames;
	duplicatePropertyNames.node_creates = {
			{{"Person"}, {{"age", PropertyValue(int64_t(1))}, {"age", PropertyValue(int64_t(2))}}}};
	EXPECT_THROW(static_cast<void>(executor.execute(duplicatePropertyNames)), std::invalid_argument);

	LogicalGraphMutationBatch emptyLabel;
	emptyLabel.node_creates = {{{""}, {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(emptyLabel)), std::invalid_argument);

	LogicalGraphMutationBatch unresolvedRelationshipType;
	unresolvedRelationshipType.node_creates = {{{"Person"}, {}}, {{"Person"}, {}}};
	unresolvedRelationshipType.edge_creates = {{createdNodeReference(0U), createdNodeReference(1U), "UNKNOWN", {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(unresolvedRelationshipType)), FormatException);

	LogicalGraphMutationBatch staleFence;
	staleFence.node_creates = {{{"Person"}, {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(staleFence, [](const Superblock &) { return false; })),
				 LogicalGraphMutationConflict);

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock().generation, generation);
	after.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   RejectsMalformedLogicalReferencesAndCreationLeasePlansWithoutPublishingAWrite) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation generation = before.superblock().generation;
	before.release();

	LogicalGraphMutationBatch createdNodeWrongKind;
	createdNodeWrongKind.node_creates = {{{"Person"}, {}}};
	createdNodeWrongKind.edge_creates = {
			{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
			 createdNodeReference(0U),
			 "KNOWS",
			 {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(createdNodeWrongKind)), std::invalid_argument);

	LogicalGraphMutationBatch invalidCreatedEdgeOrdinal;
	invalidCreatedEdgeOrdinal.node_creates = {{{"Person"}, {}}};
	invalidCreatedEdgeOrdinal.edge_creates = {
			{createdNodeReference(0U),
			 {LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 1U},
			 "KNOWS",
			 {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(invalidCreatedEdgeOrdinal)), std::invalid_argument);

	LogicalGraphMutationBatch createdEdgeUsedAsNode;
	createdEdgeUsedAsNode.node_creates = {{{"Person"}, {}}};
	createdEdgeUsedAsNode.edge_creates = {
			{createdNodeReference(0U),
			 {LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U},
			 "KNOWS",
			 {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(createdEdgeUsedAsNode)), std::invalid_argument);

	LogicalGraphMutationBatch unknownReferenceKind;
	unknownReferenceKind.property_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, static_cast<LogicalGraphMutationReferenceKind>(255U), 1U},
			 {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(unknownReferenceKind)), std::invalid_argument);

	LogicalGraphMutationBatch unknownEntityKind;
	unknownEntityKind.property_replacements = {
			{{static_cast<LogicalGraphMutationEntityKind>(255U), LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
			 {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(unknownEntityKind)), std::invalid_argument);

	LogicalGraphMutationBatch missingLeasedId;
	missingLeasedId.node_creates = {{{"Person"}, {}}};
	missingLeasedId.creation_identity_plan = LogicalGraphCreationIdentityPlan{{{1U, 1U}}, {}, {}, {}};
	EXPECT_THROW(static_cast<void>(executor.execute(missingLeasedId)), std::invalid_argument);

	LogicalGraphMutationBatch zeroLeaseRange;
	zeroLeaseRange.node_creates = {{{"Person"}, {}}};
	zeroLeaseRange.creation_identity_plan = LogicalGraphCreationIdentityPlan{{{0U, 1U}}, {1U}, {}, {}};
	EXPECT_THROW(static_cast<void>(executor.execute(zeroLeaseRange)), std::invalid_argument);

	LogicalGraphMutationBatch overflowingLeaseRange;
	overflowingLeaseRange.node_creates = {{{"Person"}, {}}};
	overflowingLeaseRange.creation_identity_plan = LogicalGraphCreationIdentityPlan{
			{{std::numeric_limits<uint64_t>::max(), 2U}}, {std::numeric_limits<uint64_t>::max()}, {}, {}};
	EXPECT_THROW(static_cast<void>(executor.execute(overflowingLeaseRange)), std::invalid_argument);

	LogicalGraphMutationBatch unorderedLeaseRanges;
	unorderedLeaseRanges.node_creates = {{{"Person"}, {}}, {{"Person"}, {}}};
	unorderedLeaseRanges.creation_identity_plan =
			LogicalGraphCreationIdentityPlan{{{3U, 1U}, {2U, 1U}}, {2U, 3U}, {}, {}};
	EXPECT_THROW(static_cast<void>(executor.execute(unorderedLeaseRanges)), std::invalid_argument);

	LogicalGraphMutationBatch outsideLeaseRange;
	outsideLeaseRange.node_creates = {{{"Person"}, {}}};
	outsideLeaseRange.creation_identity_plan = LogicalGraphCreationIdentityPlan{{{10U, 1U}}, {9U}, {}, {}};
	EXPECT_THROW(static_cast<void>(executor.execute(outsideLeaseRange)), std::invalid_argument);

	LogicalGraphMutationBatch unorderedLeasedIds;
	unorderedLeasedIds.node_creates = {{{"Person"}, {}}, {{"Person"}, {}}};
	unorderedLeasedIds.creation_identity_plan = LogicalGraphCreationIdentityPlan{{{10U, 2U}}, {11U, 10U}, {}, {}};
	EXPECT_THROW(static_cast<void>(executor.execute(unorderedLeasedIds)), std::invalid_argument);

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock().generation, generation);
	after.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   PublishesReservedCreatedNodesAndEdgeThroughTheLogicalBatchExecutor) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	const NativeGraphLogicalIdReservation reservation = planner.reserveLogicalIds(2U, 1U);
	ASSERT_EQ(reservation.node_ranges.size(), 1U);
	ASSERT_EQ(reservation.edge_ranges.size(), 1U);
	const uint64_t firstNodeId = reservation.node_ranges.front().first_id;
	const uint64_t secondNodeId = firstNodeId + 1U;
	const uint64_t edgeId = reservation.edge_ranges.front().first_id;

	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Person"}, {{"age", PropertyValue(int64_t(42))}}}, {{"Employee"}, {}}};
	batch.edge_creates = {{createdNodeReference(0U), createdNodeReference(1U), "KNOWS",
						   {{"age", PropertyValue(int64_t(7))}}}};
	batch.creation_identity_plan = LogicalGraphCreationIdentityPlan{
			{{firstNodeId, 2U}}, {firstNodeId, secondNodeId}, {{edgeId, 1U}}, {edgeId}};

	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());
	EXPECT_EQ(executed.result.created_node_ids, (std::vector<uint64_t>{firstNodeId, secondNodeId}));
	EXPECT_EQ(executed.result.created_edge_ids, (std::vector<uint64_t>{edgeId}));

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(graph.readNode(snapshot, firstNodeId).label_token_ids, (std::vector<uint64_t>{7U}));
	EXPECT_EQ(graph.readNode(snapshot, firstNodeId).properties,
			  (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(42))}}));
	EXPECT_EQ(graph.readNode(snapshot, secondNodeId).label_token_ids, (std::vector<uint64_t>{9U}));
	const PageBackedGraphEdge edge = graph.readEdge(snapshot, edgeId);
	EXPECT_EQ(edge.source_node_id, firstNodeId);
	EXPECT_EQ(edge.target_node_id, secondNodeId);
	EXPECT_EQ(edge.type_id, 11U);
	EXPECT_EQ(edge.properties, (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(7))}}));
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   AppliesPostCreatePropertyReplacementsThroughCreatedEntityReferences) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Person"}, {}}, {{"Employee"}, {}}};
	batch.edge_creates = {{createdNodeReference(0U), createdNodeReference(1U), "KNOWS", {}}};
	batch.property_replacements = {
			{createdNodeReference(0U), {{"age", PropertyValue(int64_t(42))}}},
			{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U},
			 {{"age", PropertyValue(int64_t(7))}}},
	};

	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());
	ASSERT_EQ(executed.result.created_node_ids.size(), 2U);
	ASSERT_EQ(executed.result.created_edge_ids.size(), 1U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(graph.readNode(snapshot, executed.result.created_node_ids.front()).properties,
			  (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(42))}}));
	EXPECT_EQ(graph.readEdge(snapshot, executed.result.created_edge_ids.front()).properties,
			  (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(7))}}));
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   AppliesPostCreateNodeLabelsWithoutDiscardingCreatedProperties) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	// CREATE/SET chains may assign the durable final label after a Node has been
	// allocated within the same logical mutation batch, without discarding the
	// properties supplied by CREATE.
	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Person"}, {{"age", PropertyValue(int64_t(1))}}}};
	batch.node_label_replacements = {{createdNodeReference(0U), {"Employee"}}};

	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());
	ASSERT_EQ(executed.result.created_node_ids.size(), 1U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const PageBackedGraphNode node = graph.readNode(snapshot, executed.result.created_node_ids.front());
	EXPECT_EQ(node.label_token_ids, (std::vector<uint64_t>{9U}));
	EXPECT_EQ(node.properties, (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(1))}}));
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   ReplacesPersistedNodeLabelsAndPropertiesAndPersistedEdgeProperties) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t otherNodeId = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edgeId = create.createEdge(nodeId, otherNodeId, 11U);
	ASSERT_TRUE(create.commit().committed());

	const LogicalGraphMutationEntityReference node{LogicalGraphMutationEntityKind::LGMK_NODE,
														   LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId};
	const LogicalGraphMutationEntityReference edge{LogicalGraphMutationEntityKind::LGMK_EDGE,
														   LogicalGraphMutationReferenceKind::LGRK_PERSISTED, edgeId};
	LogicalGraphMutationBatch batch;
	batch.node_label_replacements = {{node, {"Employee"}}};
	batch.property_replacements = {{node, {{"age", PropertyValue(int64_t(43))}}},
								   {edge, {{"age", PropertyValue(int64_t(8))}}}};

	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());
	EXPECT_TRUE(executed.result.created_node_ids.empty());
	EXPECT_TRUE(executed.result.created_edge_ids.empty());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(graph.readNode(snapshot, nodeId).label_token_ids, (std::vector<uint64_t>{9U}));
	EXPECT_EQ(graph.readNode(snapshot, nodeId).properties,
			  (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(43))}}));
	EXPECT_EQ(graph.readEdge(snapshot, edgeId).properties,
			  (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(8))}}));
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   ReplacesPersistedNodeLabelsWithoutRewritingItsProperties) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"}, {{"age", PropertyValue(int64_t(42))}}}};
	const NativeGraphMutationExecutionResult created = executor.execute(create);
	ASSERT_TRUE(created.committed());
	ASSERT_EQ(created.result.created_node_ids.size(), 1U);
	const uint64_t nodeId = created.result.created_node_ids.front();

	LogicalGraphMutationBatch relabel;
	relabel.node_label_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
			 {"Employee"}},
	};
	ASSERT_TRUE(executor.execute(relabel).committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const PageBackedGraphNode node = graph.readNode(snapshot, nodeId);
	EXPECT_EQ(node.label_token_ids, (std::vector<uint64_t>{9U}));
	EXPECT_EQ(node.properties, (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(42))}}));
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   CreatesAnEdgeFromAPersistedNodeToANewNodeThroughTheLogicalExecutor) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	NativeGraphWriteTransaction seed = planner.beginTransaction();
	const uint64_t persistedNodeId = seed.createNode(std::array<uint64_t, 1U>{7U});
	ASSERT_TRUE(seed.commit().committed());

	const LogicalGraphMutationEntityReference persistedNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, persistedNodeId};
	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Employee"}, {}}};
	batch.edge_creates = {{persistedNode, createdNodeReference(0U), "KNOWS", {}}};

	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());
	ASSERT_EQ(executed.result.created_node_ids.size(), 1U);
	ASSERT_EQ(executed.result.created_edge_ids.size(), 1U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const PageBackedGraphEdge edge = graph.readEdge(snapshot, executed.result.created_edge_ids.front());
	EXPECT_EQ(edge.source_node_id, persistedNodeId);
	EXPECT_EQ(edge.target_node_id, executed.result.created_node_ids.front());
	EXPECT_EQ(edge.type_id, 11U);
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   DeletesAnExplicitEdgeBeforeDeletingItsNowIsolatedNode) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t sourceNodeId = create.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t targetNodeId = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edgeId = create.createEdge(sourceNodeId, targetNodeId, 11U);
	ASSERT_TRUE(create.commit().committed());

	LogicalGraphMutationBatch batch;
	batch.edge_deletes = {{edgeId}};
	batch.node_deletes = {{sourceNodeId, false, 0U}};
	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(graph.readNode(snapshot, sourceNodeId)), FormatException);
	EXPECT_NO_THROW(static_cast<void>(graph.readNode(snapshot, targetNodeId)));
	EXPECT_THROW(static_cast<void>(graph.readEdge(snapshot, edgeId)), FormatException);
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   AllocatesCatalogTokensAndGraphCardinalityInTheSameLogicalGraphPublication) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	SystemCatalog catalog(indexes, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, {},
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions(), &catalog);

	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Person"}, {{"age", PropertyValue(int64_t(42))}}},
					  {{}, {{"age", PropertyValue(int64_t(7))}}}};
	batch.edge_creates = {{createdNodeReference(0U), createdNodeReference(1U), "KNOWS", {}}};
	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());
	ASSERT_EQ(executed.result.created_node_ids.size(), 2U);
	ASSERT_EQ(executed.result.created_edge_ids.size(), 1U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const std::optional<uint64_t> person =
			catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, "Person");
	const std::optional<uint64_t> knows =
			catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "KNOWS");
	const std::optional<uint32_t> age = catalog.findPropertyToken(snapshot, "age");
	ASSERT_TRUE(person.has_value());
	ASSERT_TRUE(knows.has_value());
	ASSERT_TRUE(age.has_value());
	EXPECT_EQ(graph.readNode(snapshot, executed.result.created_node_ids.front()).label_token_ids,
			  (std::vector<uint64_t>{*person}));
	EXPECT_EQ(graph.readNode(snapshot, executed.result.created_node_ids.front()).properties,
			  (std::vector<PropertyCollectionRootEntry>{{*age, PropertyValue(int64_t(42))}}));
	EXPECT_EQ(graph.readNode(snapshot, executed.result.created_node_ids.back()).properties,
			  (std::vector<PropertyCollectionRootEntry>{{*age, PropertyValue(int64_t(7))}}));
	EXPECT_EQ(graph.readEdge(snapshot, executed.result.created_edge_ids.front()).type_id, *knows);
	EXPECT_EQ(catalog.readGraphCardinalityStatistics(snapshot),
			  std::optional<NativeGraphCardinalityStatistics>(NativeGraphCardinalityStatistics{2U, 1U}));
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   CreatesUntypedRelationshipWithoutResolvingASchemaToken) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{}, {}}, {{}, {}}};
	batch.edge_creates = {{createdNodeReference(0U), createdNodeReference(1U), {}, {}}};
	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());
	ASSERT_EQ(executed.result.created_edge_ids.size(), 1U);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	const PageBackedGraphEdge edge = graph.readEdge(snapshot, executed.result.created_edge_ids.front());
	EXPECT_EQ(edge.type_id, RSS_UNTYPED_RELATIONSHIP_TYPE_TOKEN);
	EXPECT_EQ(edge.source_node_id, executed.result.created_node_ids.front());
	EXPECT_EQ(edge.target_node_id, executed.result.created_node_ids.back());
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   MaintainsUniqueConstraintSupportEntriesForCreatedAndPersistedNodes) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const uint64_t constraintId = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
	allocateUniqueConstraintSupportRoot(indexes, constraintId);

	NativeGraphMutationTokenResolvers constraintTokens = tokens();
	constraintTokens.property_key_token = [](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
		if (name == "email") return 19U;
		if (name == "note") return 20U;
		return std::nullopt;
	};
	constraintTokens.property_key_name = [](const MetadataSnapshotPin &, uint32_t token) -> std::optional<std::string> {
		if (token == 19U) return "email";
		if (token == 20U) return "note";
		return std::nullopt;
	};
	ConstraintDefinition unique;
	unique.constraint_id = constraintId;
	unique.name = "person-email-unique";
	unique.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
	unique.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	unique.schema_token_id = 7U;
	unique.property_names = {"email"};
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, std::move(constraintTokens),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[unique](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{unique}; },
			executorOptions());

	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"}, {{"email", PropertyValue("ada@example.test")}}}};
	const NativeGraphMutationExecutionResult created = executor.execute(create);
	ASSERT_TRUE(created.committed());
	ASSERT_EQ(created.result.created_node_ids.size(), 1U);
	const uint64_t nodeId = created.result.created_node_ids.front();
	const std::vector<std::byte> initialKey = ConstraintSupportTree::canonicalKey(
			std::array<PropertyValue, 1U>{PropertyValue("ada@example.test")});

	BTreeIndex support(constraintId, indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(support.find(snapshot, ConstraintSupportTree::entryKey(initialKey, nodeId)), std::optional<uint64_t>(nodeId));
	snapshot.release();

	const auto beforeUnrelated = metadata_->openSnapshot();
	const auto originalSupportRoot = metadata_->findIndexRootOptional(beforeUnrelated, constraintId);
	LogicalGraphMutationBatch unrelated;
	unrelated.property_replacements = {
		{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
		 {{"email", PropertyValue("ada@example.test")}, {"note", PropertyValue("unrelated field")}}}};
	ASSERT_TRUE(executor.execute(unrelated).committed());
	{
		auto afterUnrelated = metadata_->openSnapshot();
		EXPECT_EQ(metadata_->findIndexRootOptional(afterUnrelated, constraintId), originalSupportRoot);
		EXPECT_EQ(graph.readNode(beforeUnrelated, nodeId).properties.size(), 1U);
		EXPECT_EQ(graph.readNode(afterUnrelated, nodeId).properties.size(), 2U);
	}

	LogicalGraphMutationBatch update;
	update.property_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
			 {{"email", PropertyValue("grace@example.test")}}}};
	ASSERT_TRUE(executor.execute(update).committed());
	const std::vector<std::byte> updatedKey = ConstraintSupportTree::canonicalKey(
			std::array<PropertyValue, 1U>{PropertyValue("grace@example.test")});
	snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(support.find(snapshot, ConstraintSupportTree::entryKey(initialKey, nodeId)).has_value());
	EXPECT_EQ(support.find(snapshot, ConstraintSupportTree::entryKey(updatedKey, nodeId)), std::optional<uint64_t>(nodeId));
	snapshot.release();

	LogicalGraphMutationBatch erase;
	erase.node_deletes = {{nodeId, false, 0U}};
	ASSERT_TRUE(executor.execute(erase).committed());
	snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(support.find(snapshot, ConstraintSupportTree::entryKey(updatedKey, nodeId)).has_value());
	EXPECT_THROW(static_cast<void>(graph.readNode(snapshot, nodeId)), FormatException);
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   MaintainsRelationshipUniqueConstraintSupportEntriesForCreatedEdges) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const uint64_t constraintId = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST + 1U;
	allocateUniqueConstraintSupportRoot(indexes, constraintId);

	ConstraintDefinition unique;
	unique.constraint_id = constraintId;
	unique.name = "knows-age-unique";
	unique.entity_kind = ConstraintDefinitionEntityKind::CDEK_EDGE;
	unique.kind = ConstraintDefinitionKind::CDK_UNIQUE;
	unique.schema_token_id = 11U;
	unique.property_names = {"age"};
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[unique](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{unique}; },
			executorOptions());

	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"}, {}}, {{"Employee"}, {}}};
	create.edge_creates = {{createdNodeReference(0U), createdNodeReference(1U), "KNOWS",
						 {{"age", PropertyValue(int64_t(42))}}}};
	const NativeGraphMutationExecutionResult created = executor.execute(create);
	ASSERT_TRUE(created.committed());
	ASSERT_EQ(created.result.created_edge_ids.size(), 1U);
	const uint64_t edgeId = created.result.created_edge_ids.front();

	const std::vector<std::byte> canonical =
			ConstraintSupportTree::canonicalKey(std::array<PropertyValue, 1U>{PropertyValue(int64_t(42))});
	BTreeIndex support(constraintId, indexes, *metadata_);
	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(graph.readEdge(snapshot, edgeId).properties,
			  (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(42))}}));
	EXPECT_EQ(support.find(snapshot, ConstraintSupportTree::entryKey(canonical, edgeId)), std::optional<uint64_t>(edgeId));
	snapshot.release();

	LogicalGraphMutationBatch replace;
	replace.property_replacements = {
				{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, edgeId},
				 {{"age", PropertyValue(int64_t(43))}}}};
	ASSERT_TRUE(executor.execute(replace).committed());
	const std::vector<std::byte> replacedCanonical =
			ConstraintSupportTree::canonicalKey(std::array<PropertyValue, 1U>{PropertyValue(int64_t(43))});
	snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(support.find(snapshot, ConstraintSupportTree::entryKey(canonical, edgeId)).has_value());
	EXPECT_EQ(graph.readEdge(snapshot, edgeId).properties,
			  (std::vector<PropertyCollectionRootEntry>{{17U, PropertyValue(int64_t(43))}}));
	EXPECT_EQ(support.find(snapshot, ConstraintSupportTree::entryKey(replacedCanonical, edgeId)),
			  std::optional<uint64_t>(edgeId));
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest, FinalizesAnUnusedReservedCreationRangeWithoutPublishingAnEntity) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	const NativeGraphLogicalIdReservation reservation = planner.reserveLogicalIds(0U, 1U);
	ASSERT_EQ(reservation.node_ranges.size(), 0U);
	ASSERT_EQ(reservation.edge_ranges.size(), 1U);
	const uint64_t reservedEdgeId = reservation.edge_ranges.front().first_id;
	LogicalGraphMutationBatch batch;
	batch.creation_identity_plan = LogicalGraphCreationIdentityPlan{{}, {}, {{reservedEdgeId, 1U}}, {}};

	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());
	EXPECT_TRUE(executed.result.created_node_ids.empty());
	EXPECT_TRUE(executed.result.created_edge_ids.empty());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findEdgePageLocationOptional(snapshot, reservedEdgeId).has_value());
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest, RejectsNonDetachDeletionWhenAnIncidentEdgeIsMissingFromTheSameBatch) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t source = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = create.createNode(std::array<uint64_t, 0U>{});
	static_cast<void>(create.createEdge(source, target, 11U));
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation generation = before.superblock().generation;
	before.release();
	LogicalGraphMutationBatch batch;
	batch.node_deletes = {{source, false, 0U}};
	try {
		static_cast<void>(executor.execute(batch));
		FAIL() << "non-detach deletion must reject its undeclared incident Edge";
	} catch (const FormatException &error) {
		EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
	}

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock().generation, generation);
	EXPECT_NO_THROW(static_cast<void>(graph.readNode(after, source)));
	after.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   RejectsAnUnboundedDetachIncidentEdgeBudgetWithoutPublishingGraphChanges) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t source = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edge = create.createEdge(source, target, 11U);
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation generation = before.superblock().generation;
	before.release();

	LogicalGraphMutationBatch batch;
	batch.node_deletes = {{source, true, std::numeric_limits<size_t>::max()}};
	EXPECT_THROW(static_cast<void>(executor.execute(batch)), std::invalid_argument);

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock().generation, generation);
	EXPECT_NO_THROW(static_cast<void>(graph.readNode(after, source)));
	EXPECT_NO_THROW(static_cast<void>(graph.readNode(after, target)));
	EXPECT_NO_THROW(static_cast<void>(graph.readEdge(after, edge)));
	after.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   DetachDeletionRemovesBoundedIncidentEdgesAndPreservesOtherEndpoints) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t source = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edge = create.createEdge(source, target, 11U);
	ASSERT_TRUE(create.commit().committed());

	const NativeGraphMutationExecutionResult executed = executor.execute({.node_deletes = {{source, true, 1U}}});
	ASSERT_TRUE(executed.committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(snapshot, source).has_value());
	EXPECT_FALSE(metadata_->findEdgePageLocationOptional(snapshot, edge).has_value());
	EXPECT_TRUE(metadata_->findNodePageLocationOptional(snapshot, target).has_value());
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest, TreatsAnIdempotentPersistedFinalStateAsACommittedNoOp) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t source = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t target = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edge = create.createEdge(source, target, 11U);
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin before = metadata_->openSnapshot();
	const Generation generation = before.superblock().generation;
	before.release();
	const LogicalGraphMutationEntityReference persistedNode{LogicalGraphMutationEntityKind::LGMK_NODE,
															LogicalGraphMutationReferenceKind::LGRK_PERSISTED, source};
	const LogicalGraphMutationEntityReference persistedEdge{LogicalGraphMutationEntityKind::LGMK_EDGE,
															LogicalGraphMutationReferenceKind::LGRK_PERSISTED, edge};
	LogicalGraphMutationBatch batch;
	batch.property_replacements = {{persistedNode, {}}, {persistedEdge, {}}};
	batch.node_label_replacements = {{persistedNode, {}}};

	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	EXPECT_TRUE(executed.committed());
	EXPECT_TRUE(executed.result.created_node_ids.empty());
	EXPECT_TRUE(executed.result.created_edge_ids.empty());

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock().generation, generation);
	EXPECT_EQ(graph.readEdge(after, edge).source_node_id, source);
	after.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest, RejectsIncompleteOrUnboundedPublicExecutorConfiguration) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	const auto emptyIndexes = [](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; };
	const auto emptyConstraints = [](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; };
	const auto expectInvalid = [&](NativeGraphMutationTokenResolvers tokenResolvers,
								   NativeGraphMutationIndexDefinitionReader indexDefinitions,
								   NativeGraphMutationConstraintDefinitionReader constraintDefinitions,
								   NativeGraphMutationExecutorOptions options) {
		EXPECT_THROW(static_cast<void>(NativeGraphMutationExecutor(
							 planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_,
							 std::move(tokenResolvers), std::move(indexDefinitions), std::move(constraintDefinitions),
							 options)),
					 std::invalid_argument);
	};

	NativeGraphMutationTokenResolvers missingSchemaToken = tokens();
	missingSchemaToken.schema_token = {};
	expectInvalid(missingSchemaToken, emptyIndexes, emptyConstraints, executorOptions());
	NativeGraphMutationTokenResolvers missingPropertyToken = tokens();
	missingPropertyToken.property_key_token = {};
	expectInvalid(missingPropertyToken, emptyIndexes, emptyConstraints, executorOptions());
	NativeGraphMutationTokenResolvers missingPropertyName = tokens();
	missingPropertyName.property_key_name = {};
	expectInvalid(missingPropertyName, emptyIndexes, emptyConstraints, executorOptions());
	expectInvalid(tokens(), {}, emptyConstraints, executorOptions());
	expectInvalid(tokens(), emptyIndexes, {}, executorOptions());

	NativeGraphMutationExecutorOptions options = executorOptions();
	options.maximum_index_definitions = 0U;
	expectInvalid(tokens(), emptyIndexes, emptyConstraints, options);
	options = executorOptions();
	options.maximum_constraint_definitions = 0U;
	expectInvalid(tokens(), emptyIndexes, emptyConstraints, options);
	options = executorOptions();
	options.maximum_constraint_entity_scan = 0U;
	expectInvalid(tokens(), emptyIndexes, emptyConstraints, options);
	options = executorOptions();
	options.maximum_constraint_support_collisions = 0U;
	expectInvalid(tokens(), emptyIndexes, emptyConstraints, options);
	options = executorOptions();
	options.maximum_vector_records = 0U;
	expectInvalid(tokens(), emptyIndexes, emptyConstraints, options);
	options = executorOptions();
	options.maximum_vector_pages = 0U;
	expectInvalid(tokens(), emptyIndexes, emptyConstraints, options);
	options = executorOptions();
	options.vector_delta_rebuild_threshold = 0U;
	expectInvalid(tokens(), emptyIndexes, emptyConstraints, options);
	options = executorOptions();
	options.maximum_vector_delta_overlay_entries = 0U;
	expectInvalid(tokens(), emptyIndexes, emptyConstraints, options);
	options = executorOptions();
	options.vector_delta_rebuild_threshold = options.maximum_vector_delta_overlay_entries + 1U;
	expectInvalid(tokens(), emptyIndexes, emptyConstraints, options);
	options = executorOptions();
	options.maximum_vector_delta_pages = 0U;
	expectInvalid(tokens(), emptyIndexes, emptyConstraints, options);
	options = executorOptions();
	options.maximum_vector_delta_pages = 1U;
	options.maximum_vector_delta_overlay_entries = VectorDeltaMapping::maximumEntriesForPageBudget(1U) + 1U;
	expectInvalid(tokens(), emptyIndexes, emptyConstraints, options);
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   RejectsBoundViolationsAndZeroValuedResolverResultsBeforePublishingGraphData) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutorOptions limitedOptions = executorOptions();
	limitedOptions.maximum_index_definitions = 1U;
	limitedOptions.maximum_constraint_definitions = 1U;
	const auto noIndexes = [](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; };
	const auto noConstraints = [](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; };

	NativeGraphMutationExecutor tooManyIndexes(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>(2U); }, noConstraints,
			limitedOptions);
	LogicalGraphMutationBatch empty;
	expectFormatError([&] { static_cast<void>(tooManyIndexes.execute(empty)); }, FormatError::FERR_INVALID_STATE);

	NativeGraphMutationExecutor tooManyConstraints(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(), noIndexes,
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>(2U); }, limitedOptions);
	expectFormatError([&] { static_cast<void>(tooManyConstraints.execute(empty)); }, FormatError::FERR_INVALID_STATE);

	NativeGraphMutationTokenResolvers zeroSchemaToken = tokens();
	zeroSchemaToken.schema_token = [](const MetadataSnapshotPin &, SystemCatalogSchemaTokenKind,
									  std::string_view) -> std::optional<uint64_t> { return 0U; };
	NativeGraphMutationExecutor schemaExecutor(planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_,
											   zeroSchemaToken, noIndexes, noConstraints, executorOptions());
	LogicalGraphMutationBatch labels;
	labels.node_creates = {{{"Person"}, {}}};
	expectFormatError([&] { static_cast<void>(schemaExecutor.execute(labels)); }, FormatError::FERR_INVALID_STATE);
	LogicalGraphMutationBatch relationship;
	relationship.edge_creates = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
			 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
			 "KNOWS",
			 {}}};
	expectFormatError([&] { static_cast<void>(schemaExecutor.execute(relationship)); },
					  FormatError::FERR_INVALID_STATE);

	NativeGraphMutationTokenResolvers zeroPropertyToken = tokens();
	zeroPropertyToken.property_key_token = [](const MetadataSnapshotPin &,
											  std::string_view) -> std::optional<uint32_t> { return 0U; };
	NativeGraphMutationExecutor propertyExecutor(planner, graph, indexes, *vector_pages_, *vector_manifests_,
												 *metadata_, zeroPropertyToken, noIndexes, noConstraints,
												 executorOptions());
	LogicalGraphMutationBatch properties;
	properties.node_creates = {{{"Person"}, {{"age", PropertyValue(int64_t(1))}}}};
	expectFormatError([&] { static_cast<void>(propertyExecutor.execute(properties)); },
					  FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(snapshot, 1U).has_value());
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest, RebasesADisjointLogicalCreateWhenAnotherWriterPublishesFirst) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	bool injectedPublication = false;
	uint64_t concurrentNodeId = 0U;
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[&](const MetadataSnapshotPin &) {
				if (!injectedPublication) {
					injectedPublication = true;
					NativeGraphWriteTransaction competing = planner.beginTransaction();
					concurrentNodeId = competing.createNode(std::array<uint64_t, 0U>{});
					if (!competing.commit().committed()) {
						throw std::runtime_error("competing native graph publication unexpectedly conflicted");
					}
				}
				return std::vector<IndexDefinition>{};
			},
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Person"}, {}}};
	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	ASSERT_TRUE(executed.committed());
	ASSERT_EQ(executed.result.created_node_ids.size(), 1U);
	EXPECT_TRUE(executed.result.created_edge_ids.empty());
	EXPECT_NE(executed.result.created_node_ids.front(), concurrentNodeId);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findNodePageLocationOptional(snapshot, concurrentNodeId).has_value());
	EXPECT_EQ(graph.readNode(snapshot, concurrentNodeId).label_token_ids, std::vector<uint64_t>{});
	EXPECT_EQ(graph.readNode(snapshot, executed.result.created_node_ids.front()).label_token_ids,
			  std::vector<uint64_t>({7U}));
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   RejectsRemainingInvalidFinalStateReferencesBeforeStartingTheNativeTransaction) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());
	const LogicalGraphMutationEntityReference persistedNode{LogicalGraphMutationEntityKind::LGMK_NODE,
																   LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U};
	const LogicalGraphMutationEntityReference persistedEdge{LogicalGraphMutationEntityKind::LGMK_EDGE,
																   LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U};
	const LogicalGraphMutationEntityReference zeroPersistedNode{
			LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 0U};
	LogicalGraphMutationBatch zeroPersistedReference;
	zeroPersistedReference.property_replacements = {{zeroPersistedNode, {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(zeroPersistedReference)), std::invalid_argument);

	LogicalGraphMutationBatch wrongCreatedEdgeKind;
	wrongCreatedEdgeKind.node_creates = {{{"Person"}, {}}};
	wrongCreatedEdgeKind.edge_creates = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U},
			 createdNodeReference(0U),
			 "KNOWS",
			 {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(wrongCreatedEdgeKind)), std::invalid_argument);

	LogicalGraphMutationBatch zeroEdgeDelete;
	zeroEdgeDelete.edge_deletes = {{0U}};
	EXPECT_THROW(static_cast<void>(executor.execute(zeroEdgeDelete)), std::invalid_argument);
	LogicalGraphMutationBatch duplicateEdgeDelete;
	duplicateEdgeDelete.edge_deletes = {{1U}, {1U}};
	EXPECT_THROW(static_cast<void>(executor.execute(duplicateEdgeDelete)), std::invalid_argument);

	LogicalGraphMutationBatch propertyOnDeletedNode;
	propertyOnDeletedNode.node_deletes = {{1U, false, 0U}};
	propertyOnDeletedNode.property_replacements = {{persistedNode, {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(propertyOnDeletedNode)), std::invalid_argument);
	LogicalGraphMutationBatch propertyOnDeletedEdge;
	propertyOnDeletedEdge.edge_deletes = {{1U}};
	propertyOnDeletedEdge.property_replacements = {{persistedEdge, {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(propertyOnDeletedEdge)), std::invalid_argument);

	LogicalGraphMutationBatch duplicateLabelTarget;
	duplicateLabelTarget.node_label_replacements = {{persistedNode, {}}, {persistedNode, {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(duplicateLabelTarget)), std::invalid_argument);

	LogicalGraphMutationBatch duplicateCreatedEdgeProperties;
	duplicateCreatedEdgeProperties.node_creates = {{{"Person"}, {}}};
	duplicateCreatedEdgeProperties.edge_creates = {
			{createdNodeReference(0U), createdNodeReference(0U), "KNOWS", {{"age", PropertyValue(int64_t(1))}}}};
	duplicateCreatedEdgeProperties.property_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U},
			 {}}};
	EXPECT_THROW(static_cast<void>(executor.execute(duplicateCreatedEdgeProperties)), std::invalid_argument);

	NativeGraphMutationTokenResolvers aliasedPropertyTokens = tokens();
	aliasedPropertyTokens.property_key_token = [](const MetadataSnapshotPin &,
												  std::string_view) -> std::optional<uint32_t> { return 17U; };
	NativeGraphMutationExecutor aliasExecutor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, aliasedPropertyTokens,
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());
	LogicalGraphMutationBatch aliasedProperties;
	aliasedProperties.node_creates = {
			{{"Person"}, {{"age", PropertyValue(int64_t(1))}, {"other", PropertyValue(int64_t(2))}}}};
	expectFormatError([&] { static_cast<void>(aliasExecutor.execute(aliasedProperties)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(NativeGraphMutationExecutorValidationTest,
	   ReturnsConflictWithoutPublishingAPropertyChangeAfterAnIncompatibleConcurrentWrite) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t nodeId = create.createNode(std::array<uint64_t, 0U>{});
	ASSERT_TRUE(create.commit().committed());

	bool injectedPublication = false;
	uint64_t concurrentNodeId = 0U;
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
			[&](const MetadataSnapshotPin &) {
				if (!injectedPublication) {
					injectedPublication = true;
					NativeGraphWriteTransaction competing = planner.beginTransaction();
					concurrentNodeId = competing.createNode(std::array<uint64_t, 0U>{});
					if (!competing.commit().committed()) {
						throw std::runtime_error("competing native graph publication unexpectedly conflicted");
					}
				}
				return std::vector<IndexDefinition>{};
			},
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	LogicalGraphMutationBatch batch;
	batch.property_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
			 {{"age", PropertyValue(int64_t(42))}}}};
	const NativeGraphMutationExecutionResult executed = executor.execute(batch);
	EXPECT_FALSE(executed.committed());
	EXPECT_EQ(executed.commit.outcome, NativeGraphWriteCommitOutcome::NGWCO_CONFLICTED);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_TRUE(metadata_->findNodePageLocationOptional(snapshot, concurrentNodeId).has_value());
	EXPECT_TRUE(graph.readNode(snapshot, nodeId).properties.empty());
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest, FailsClosedForEveryWriteWhenAPersistedVectorManifestIsMissing) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, vectorTokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{vectorDefinition()}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	const NativeGraphMutationExecutionResult created = executor.execute(vectorCreateBatch());
	ASSERT_TRUE(created.committed());
	ASSERT_EQ(created.result.created_node_ids.size(), 1U);
	const uint64_t unexpectedNodeId = created.result.created_node_ids.front() + 1U;
	const std::array<uint64_t, 1U> erasedIndexIds = {vectorDefinition().index_id};
	MetadataSnapshotPin published = metadata_->openSnapshot();
	ASSERT_TRUE(metadata_->findIndexRootOptional(published, vectorDefinition().index_id).has_value());
	ASSERT_TRUE(metadata_->findVectorManifestOptional(published, vectorDefinition().index_id).has_value());
	const LogSequenceNumber nextLsn(published.superblock().checkpoint_lsn.value() + 1U);
	published.release();
	static_cast<void>(metadata_->upsertVectorManifests({}, erasedIndexIds, nextLsn, {}));

	MetadataSnapshotPin corrupt = metadata_->openSnapshot();
	ASSERT_TRUE(metadata_->findIndexRootOptional(corrupt, vectorDefinition().index_id).has_value());
	ASSERT_FALSE(metadata_->findVectorManifestOptional(corrupt, vectorDefinition().index_id).has_value());
	const Generation corruptGeneration = corrupt.superblock().generation;
	corrupt.release();

	LogicalGraphMutationBatch unrelated;
	unrelated.node_creates = {{{"Person"}, {}}};
	expectFormatError([&] { static_cast<void>(executor.execute(unrelated)); }, FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock().generation, corruptGeneration);
	EXPECT_FALSE(metadata_->findNodePageLocationOptional(after, unexpectedNodeId).has_value());
	after.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest, UpdatesAnExistingVectorIndexThroughTheLogicalPropertyReplacement) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
			planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, vectorTokens(),
			[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{vectorDefinition()}; },
			[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());

	const NativeGraphMutationExecutionResult created = executor.execute(vectorCreateBatch());
	ASSERT_TRUE(created.committed());
	ASSERT_EQ(created.result.created_node_ids.size(), 1U);
	const uint64_t nodeId = created.result.created_node_ids.front();

	LogicalGraphMutationBatch update;
	update.property_replacements = {
			{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, nodeId},
			 {{"embedding", PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(2)),
																	  PropertyValue(int64_t(3))})}}}};
	const NativeGraphMutationExecutionResult updated = executor.execute(update);
	ASSERT_TRUE(updated.committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_EQ(graph.readNode(snapshot, nodeId).properties,
			  (std::vector<PropertyCollectionRootEntry>{{17U,
																	 PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(2)),
																												PropertyValue(int64_t(3))})}}));
	EXPECT_TRUE(metadata_->findIndexRootOptional(snapshot, vectorDefinition().index_id).has_value());
	EXPECT_TRUE(metadata_->findVectorManifestOptional(snapshot, vectorDefinition().index_id).has_value());
	const VectorManifest manifest = vector_manifests_->read(snapshot, vectorDefinition().index_id).manifest();
	EXPECT_NE(manifest.delta_mapping_index_id, 0U);
	EXPECT_EQ(manifest.delta_entry_count, 2U);
	EXPECT_TRUE(metadata_->findIndexRootOptional(snapshot, manifest.delta_mapping_index_id).has_value());
	snapshot.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest, MutationOldStateReadsSharePropertyPageAndPreserveOldSnapshot) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
		[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
		[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());
	LogicalGraphMutationBatch create;
	create.node_creates = {{{"Person"}, {}}, {{"Person"}, {}}};
	const auto created = executor.execute(create);
	ASSERT_EQ(created.result.created_node_ids.size(), 2U);
	const auto &ids = created.result.created_node_ids;
	const std::array roots{PropertyCollectionRoot{ids[0], graph::Node::typeId, {{17U, PropertyValue(1)}}},
		PropertyCollectionRoot{ids[1], graph::Node::typeId, {{17U, PropertyValue(2)}}}};
	ASSERT_EQ(graph::storage::test::publishPropertyRoots(*entities_, *properties_, *metadata_, roots,
		plannerOptions().property_roots).properties.size(), 2U);
	auto before = metadata_->openSnapshot();
	const auto first = metadata_->findEntityPropertyLocationOptional(before, {EntityPropertyOwnerKind::EPOK_NODE, ids[0]});
	const auto second = metadata_->findEntityPropertyLocationOptional(before, {EntityPropertyOwnerKind::EPOK_NODE, ids[1]});
	ASSERT_TRUE(first); ASSERT_TRUE(second); ASSERT_EQ(first->page_id, second->page_id);
	NativeReadSession candidates;
	const bool valuesEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
	for (size_t index = 0U; index < ids.size(); ++index) {
		graph.validateNodeExactPropertiesSorted(before, std::span<const uint64_t>(&ids[index], 1U), 7U,
			std::array<uint32_t, 1U>{17U}, std::array<PropertyValue, 1U>{PropertyValue(static_cast<int>(index + 1U))}, 8U, &candidates);
	}
	const auto candidateTrace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesEnabled);
	ASSERT_TRUE(candidateTrace.contains("native_graph.read.property_directory_refill"));
	EXPECT_EQ(candidateTrace.at("native_graph.read.property_directory_refill").totalValue, 1);
	LogicalGraphMutationBatch replace;
	for (uint64_t id: ids) replace.property_replacements.push_back({
		{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, id},
		{{"age", PropertyValue(3)}}});
	const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
	static_cast<void>(executor.execute(replace));
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
	ASSERT_TRUE(trace.contains("native_graph.read.property_full_cursor_pages"));
	EXPECT_EQ(trace.at("native_graph.read.property_full_cursor_pages").totalValue, 1);
	auto current = metadata_->openSnapshot();
	for (size_t index = 0U; index < ids.size(); ++index) {
		EXPECT_EQ(graph.readNode(before, ids[index]).properties[0].value, PropertyValue(static_cast<int>(index + 1U)));
		EXPECT_EQ(graph.readNode(current, ids[index]).properties[0].value, PropertyValue(3));
	}
	current.release(); before.release();
}

TEST_F(NativeGraphMutationExecutorValidationTest,
       UnorderedNodeDeletesPreserveOldSnapshotAndSurvivingTopology) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	NativeGraphMutationExecutor executor(
	  planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, tokens(),
	  [](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
	  [](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());
	auto create = planner.beginTransaction();
	const uint64_t first = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t survivor = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t last = create.createNode(std::array<uint64_t, 0U>{});
	const uint64_t edge = create.createEdge(first, last, 11U);
	ASSERT_TRUE(create.commit().committed());
	auto old = metadata_->openSnapshot();
	const auto oldFirst = graph.readNode(old, first);
	const auto oldLast = graph.readNode(old, last);
	const auto oldSurvivor = graph.readNode(old, survivor);
	LogicalGraphMutationBatch batch;
	batch.node_deletes = {{last, true, 1U}, {first, false, 0U}};
	ASSERT_TRUE(executor.execute(batch).committed());
	auto now = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(graph.readNode(now, first)), FormatException);
	EXPECT_THROW(static_cast<void>(graph.readNode(now, last)), FormatException);
	EXPECT_THROW(static_cast<void>(graph.readEdge(now, edge)), FormatException);
	EXPECT_EQ(graph.readNode(now, survivor), oldSurvivor);
	EXPECT_EQ(graph.readNode(old, first), oldFirst);
	EXPECT_EQ(graph.readNode(old, last), oldLast);
	EXPECT_NO_THROW(static_cast<void>(graph.readEdge(old, edge)));
}

TEST_F(NativeGraphMutationExecutorValidationTest, CancelsPropertyResolutionBeforeCopyingOrResolvingLaterFields) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	BTreePageStore indexes(*slotted_pages_, *metadata_);
	bool cancelled = false;
	size_t resolutions = 0U;
	auto resolvers = tokens();
	resolvers.property_key_token = [&](const MetadataSnapshotPin &, std::string_view name) -> std::optional<uint32_t> {
		++resolutions;
		if (name != "age") throw std::logic_error("Unexpected resolution after cancellation");
		cancelled = true;
		return 17U;
	};
	NativeGraphMutationExecutor executor(
		planner, graph, indexes, *vector_pages_, *vector_manifests_, *metadata_, std::move(resolvers),
		[](const MetadataSnapshotPin &) { return std::vector<IndexDefinition>{}; },
		[](const MetadataSnapshotPin &) { return std::vector<ConstraintDefinition>{}; }, executorOptions());
	LogicalGraphMutationBatch batch;
	batch.node_creates = {{{"Person"}, {{"zz", PropertyValue(int64_t(2))}, {"age", PropertyValue(int64_t(1))}}}};
	auto before = metadata_->openSnapshot();
	const auto generation = before.superblock().generation;
	LogicalGraphMutationReadControl control;
	control.checkpoint = [&] { if (cancelled) throw std::runtime_error("Property resolution cancelled"); };
	EXPECT_THROW(static_cast<void>(executor.execute(batch, {}, control)), std::runtime_error);
	EXPECT_EQ(resolutions, 1U);
	auto after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock().generation, generation);
	EXPECT_EQ(batch.node_creates.front().properties.front().key, "zz");
	EXPECT_EQ(batch.node_creates.front().properties.back().value, PropertyValue(int64_t(1)));
}
