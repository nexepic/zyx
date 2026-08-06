/**
 * @file test_NativeGraphMutationExecutor_Validation.cpp
 * @brief Public logical mutation validation tests for the native graph executor.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/relocatable/entity/NativeGraphMutationExecutor.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
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
