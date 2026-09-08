/**
 * @file test_ConstraintMutationValidator.cpp
 * @brief Unit tests for storage-format-independent constraint mutation validation.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/schema/ConstraintMutationValidator.hpp"

namespace graph::storage {

	namespace {

		ConstraintValidationEntity node(uint64_t id, std::vector<uint64_t> labels,
										std::initializer_list<std::pair<const std::string, PropertyValue>> properties) {
			ConstraintValidationEntity result;
			result.kind = LogicalGraphMutationEntityKind::LGMK_NODE;
			result.stable_id = id;
			result.schema_tokens = std::move(labels);
			result.properties = properties;
			return result;
		}

		ConstraintDefinition nodeConstraint(uint64_t id, ConstraintDefinitionKind kind, std::string name,
											std::vector<std::string> properties) {
			ConstraintDefinition result;
			result.constraint_id = id;
			result.name = std::move(name);
			result.entity_kind = ConstraintDefinitionEntityKind::CDEK_NODE;
			result.kind = kind;
			result.schema_token_id = 7U;
			result.property_names = std::move(properties);
			return result;
		}

		struct FakeGraph final {
			std::map<uint64_t, ConstraintValidationEntity> nodes;
			std::map<uint64_t, ConstraintValidationEntity> edges;
			std::map<std::string, uint64_t, std::less<>> schema_tokens = {{"Person", 7U}, {"KNOWS", 9U}};
			std::set<uint64_t> support_roots;
			std::map<uint64_t, std::vector<BTreePageEntry>> support_entries;

			[[nodiscard]] ConstraintMutationValidationAdapter adapter() {
				ConstraintMutationValidationAdapter result;
				result.read_persisted_entity = [this](LogicalGraphMutationEntityKind kind, uint64_t id) {
					const auto &entities = kind == LogicalGraphMutationEntityKind::LGMK_NODE ? nodes : edges;
					const auto found = entities.find(id);
					if (found == entities.end())
						throw std::runtime_error("test entity does not exist");
					return found->second;
				};
				result.visit_persisted_entities = [this](LogicalGraphMutationEntityKind kind, size_t maximum,
														 const ConstraintValidationEntityVisitor &visitor) {
					const auto &entities = kind == LogicalGraphMutationEntityKind::LGMK_NODE ? nodes : edges;
					if (entities.size() > maximum)
						throw FormatException(FormatError::FERR_INVALID_STATE, "test scan bound");
					for (const auto &[id, entity]: entities) {
						static_cast<void>(id);
						visitor(entity);
					}
				};
				const auto resolveSchemaToken = [this](std::string_view name) {
					const auto found = schema_tokens.find(name);
					if (found == schema_tokens.end()) {
						throw FormatException(FormatError::FERR_INVALID_STATE, "test schema token does not exist");
					}
					return found->second;
				};
				result.resolve_node_label_token = resolveSchemaToken;
				result.resolve_relationship_type_token = resolveSchemaToken;
				result.has_support_tree = [this](uint64_t id) { return support_roots.contains(id); };
				result.scan_support_tree = [this](uint64_t id, std::span<const std::byte> lowerBound, size_t maximum) {
					std::vector<BTreePageEntry> result;
					const auto found = support_entries.find(id);
					if (found == support_entries.end())
						return result;
					for (const BTreePageEntry &entry: found->second) {
						if (std::lexicographical_compare(entry.key.begin(), entry.key.end(), lowerBound.begin(),
														 lowerBound.end())) {
							continue;
						}
						result.push_back(entry);
						if (result.size() == maximum)
							break;
					}
					return result;
				};
				return result;
			}
		};

		[[nodiscard]] ConstraintMutationValidator validator(FakeGraph &graph) {
			return ConstraintMutationValidator(graph.adapter(), {32U, 8U});
		}

		[[nodiscard]] std::vector<std::byte> canonical(std::string value) {
			return ConstraintSupportTree::canonicalKey(std::vector<PropertyValue>{PropertyValue(std::move(value))});
		}

		void addSupportEntry(FakeGraph &graph, uint64_t constraintId, std::string value, uint64_t entityId) {
			std::vector<BTreePageEntry> &entries = graph.support_entries[constraintId];
			entries.push_back(
					{ConstraintSupportTree::entryKey(canonical(std::move(value)), entityId), entityId});
			std::ranges::sort(entries, {}, &BTreePageEntry::key);
		}

	} // namespace

	TEST(ConstraintMutationValidatorTest, PlansNativeUsableSupportTransitionsForReplaceAndCreate) {
		FakeGraph graph;
		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("old@example.test")}}));
		graph.support_roots.insert(81U);
		addSupportEntry(graph, 81U, "old@example.test", 1U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});

		LogicalGraphMutationBatch batch;
		batch.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {{"email", PropertyValue("new@example.test")}}});
		batch.node_creates.push_back({{"Person"}, {{"email", PropertyValue("created@example.test")}}});

		const std::vector<ConstraintSupportTreeMutationPlan> plans =
				validator(graph).validate(std::vector<ConstraintDefinition>{unique}, batch);
		ASSERT_EQ(plans.size(), 2U);
		EXPECT_EQ(plans[0].constraint_id, unique.constraint_id);
		EXPECT_EQ(plans[0].entity_reference_kind, ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_NODE);
		EXPECT_EQ(plans[0].entity_reference_value, 1U);
		EXPECT_EQ(plans[0].erased_canonical_key, std::optional{canonical("old@example.test")});
		EXPECT_EQ(plans[0].upserted_canonical_key, std::optional{canonical("new@example.test")});
		EXPECT_EQ(plans[1].entity_reference_kind, ConstraintSupportTreeEntityReferenceKind::CSTREK_CREATED_NODE);
		EXPECT_EQ(plans[1].entity_reference_value, 0U);
		EXPECT_FALSE(plans[1].erased_canonical_key.has_value());
		EXPECT_EQ(plans[1].upserted_canonical_key, std::optional{canonical("created@example.test")});
	}

	TEST(ConstraintMutationValidatorTest, ReturnsNoPlanForEmptyDefinitionsOrAnUnchangedUniqueKey) {
		FakeGraph graph;
		EXPECT_TRUE(validator(graph).validate(std::span<const ConstraintDefinition>{}, {}).empty());

		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("same@example.test")}}));
		graph.support_roots.insert(81U);
		addSupportEntry(graph, 81U, "same@example.test", 1U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});
		LogicalGraphMutationBatch batch;
		batch.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {{"email", PropertyValue("same@example.test")}}});

		EXPECT_TRUE(validator(graph).validate(std::array{unique}, batch).empty());
	}

	TEST(ConstraintMutationValidatorTest, RejectsDuplicateAgainstAValidatedSupportTreeCandidate) {
		FakeGraph graph;
		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("duplicate@example.test")}}));
		graph.support_roots.insert(81U);
		addSupportEntry(graph, 81U, "duplicate@example.test", 1U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});

		LogicalGraphMutationBatch batch;
		batch.node_creates.push_back({{"Person"}, {{"email", PropertyValue("duplicate@example.test")}}});

		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::vector<ConstraintDefinition>{unique}, batch)),
					 std::runtime_error);
	}

	TEST(ConstraintMutationValidatorTest, AppliesNodeKeyAndTypeChecksToTheBatchFinalState) {
		FakeGraph graph;
		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("old@example.test")}}));
		graph.support_roots.insert(81U);
		const ConstraintDefinition nodeKey =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_NODE_KEY, "person_email_key", {"email"});
		ConstraintDefinition type =
				nodeConstraint(82U, ConstraintDefinitionKind::CDK_PROPERTY_TYPE, "person_email_type", {"email"});
		type.property_type = ConstraintDefinitionPropertyType::CDPT_STRING;

		LogicalGraphMutationBatch missing;
		missing.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {}});
		EXPECT_THROW(
				static_cast<void>(validator(graph).validate(std::vector<ConstraintDefinition>{nodeKey, type}, missing)),
				std::runtime_error);

		LogicalGraphMutationBatch wrongType;
		wrongType.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {{"email", PropertyValue(int64_t{42})}}});
		EXPECT_THROW(static_cast<void>(
							 validator(graph).validate(std::vector<ConstraintDefinition>{nodeKey, type}, wrongType)),
					 std::runtime_error);
	}

	TEST(ConstraintMutationValidatorTest, ValidatesAdditionalDdlDefinitionsAgainstTheCompleteGraph) {
		FakeGraph graph;
		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("same@example.test")}}));
		graph.nodes.emplace(2U, node(2U, {7U}, {{"email", PropertyValue("same@example.test")}}));
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});

		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::span<const ConstraintDefinition>{}, {},
																 std::vector<ConstraintDefinition>{unique})),
					 std::runtime_error);
	}

	TEST(ConstraintMutationValidatorTest, RejectsAnActiveSupportConstraintWithoutAPublishedRoot) {
		FakeGraph graph;
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});
		LogicalGraphMutationBatch batch;
		batch.node_creates.push_back({{"Person"}, {{"email", PropertyValue("one@example.test")}}});

		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::vector<ConstraintDefinition>{unique}, batch)),
					 FormatException);
	}

	TEST(ConstraintMutationValidatorTest, ResolvesEqualLabelAndRelationshipTypeNamesThroughSeparateDomains) {
		FakeGraph graph;
		ConstraintMutationValidationAdapter adapter = graph.adapter();
		bool resolvedLabel = false;
		bool resolvedRelationshipType = false;
		adapter.resolve_node_label_token = [&resolvedLabel](std::string_view name) {
			if (name != "Shared") {
				throw FormatException(FormatError::FERR_INVALID_STATE, "unexpected label name");
			}
			resolvedLabel = true;
			return uint64_t{7U};
		};
		adapter.resolve_relationship_type_token = [&resolvedRelationshipType](std::string_view name) {
			if (name != "Shared") {
				throw FormatException(FormatError::FERR_INVALID_STATE, "unexpected relationship-type name");
			}
			resolvedRelationshipType = true;
			return uint64_t{9U};
		};
		ConstraintMutationValidator typedValidator(std::move(adapter), {32U, 8U});
		const ConstraintDefinition requiredEmail =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_NOT_NULL, "shared_email_required", {"email"});

		LogicalGraphMutationBatch batch;
		batch.node_creates.push_back({{"Shared"}, {}});
		batch.edge_creates.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 "Shared",
				 {}});

		EXPECT_THROW(
				static_cast<void>(typedValidator.validate(std::vector<ConstraintDefinition>{requiredEmail}, batch)),
				std::runtime_error);
		EXPECT_TRUE(resolvedLabel);
		EXPECT_TRUE(resolvedRelationshipType);
	}

	TEST(ConstraintMutationValidatorTest, RejectsIncompleteAdaptersAndInvalidAffectedEntityRequests) {
		ConstraintMutationValidationAdapter incomplete;
		EXPECT_THROW(static_cast<void>(ConstraintMutationValidator(std::move(incomplete), {32U, 8U})),
					 std::invalid_argument);

		FakeGraph graph;
		ConstraintMutationValidationAdapter missingRead = graph.adapter();
		missingRead.read_persisted_entity = {};
		EXPECT_THROW(static_cast<void>(ConstraintMutationValidator(std::move(missingRead), {32U, 8U})),
					 std::invalid_argument);
		ConstraintMutationValidationAdapter missingVisit = graph.adapter();
		missingVisit.visit_persisted_entities = {};
		EXPECT_THROW(static_cast<void>(ConstraintMutationValidator(std::move(missingVisit), {32U, 8U})),
					 std::invalid_argument);
		ConstraintMutationValidationAdapter missingLabel = graph.adapter();
		missingLabel.resolve_node_label_token = {};
		EXPECT_THROW(static_cast<void>(ConstraintMutationValidator(std::move(missingLabel), {32U, 8U})),
					 std::invalid_argument);
		ConstraintMutationValidationAdapter missingRelationshipType = graph.adapter();
		missingRelationshipType.resolve_relationship_type_token = {};
		EXPECT_THROW(static_cast<void>(ConstraintMutationValidator(std::move(missingRelationshipType), {32U, 8U})),
					 std::invalid_argument);
		ConstraintMutationValidationAdapter missingSupportRoot = graph.adapter();
		missingSupportRoot.has_support_tree = {};
		EXPECT_THROW(static_cast<void>(ConstraintMutationValidator(std::move(missingSupportRoot), {32U, 8U})),
					 std::invalid_argument);
		ConstraintMutationValidationAdapter missingSupportScan = graph.adapter();
		missingSupportScan.scan_support_tree = {};
		EXPECT_THROW(static_cast<void>(ConstraintMutationValidator(std::move(missingSupportScan), {32U, 8U})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(ConstraintMutationValidator(graph.adapter(), {0U, 8U})), std::invalid_argument);

		const ConstraintDefinition requiredEmail =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_required", {"email"});
		LogicalGraphMutationBatch unknownCreated;
		unknownCreated.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 {{"email", PropertyValue("one@example.test")}}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::array{requiredEmail}, unknownCreated)),
					 std::invalid_argument);

		LogicalGraphMutationBatch duplicateProperties;
		duplicateProperties.node_creates.push_back(
				{{"Person"}, {{"email", PropertyValue("one@example.test")},
							 {"email", PropertyValue("two@example.test")}}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::array{requiredEmail}, duplicateProperties)),
					 std::invalid_argument);

		ConstraintValidationEntity persistedEdge;
		persistedEdge.kind = LogicalGraphMutationEntityKind::LGMK_EDGE;
		persistedEdge.stable_id = 1U;
		graph.edges.emplace(1U, std::move(persistedEdge));
		LogicalGraphMutationBatch edgeAsNodeLabel;
		edgeAsNodeLabel.node_label_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {"Person"}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::array{requiredEmail}, edgeAsNodeLabel)),
					 std::invalid_argument);
	}

	TEST(ConstraintMutationValidatorTest, AllowsAnAtomicUniqueKeySwapAgainstChangedSupportCandidates) {
		FakeGraph graph;
		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("one@example.test")}}));
		graph.nodes.emplace(2U, node(2U, {7U}, {{"email", PropertyValue("two@example.test")}}));
		graph.support_roots.insert(81U);
		addSupportEntry(graph, 81U, "one@example.test", 1U);
		addSupportEntry(graph, 81U, "two@example.test", 2U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});

		LogicalGraphMutationBatch batch;
		batch.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {{"email", PropertyValue("two@example.test")}}});
		batch.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				 {{"email", PropertyValue("one@example.test")}}});

		const std::vector<ConstraintSupportTreeMutationPlan> plans =
				validator(graph).validate(std::array{unique}, batch);
		ASSERT_EQ(plans.size(), 2U);
		EXPECT_EQ(plans[0].entity_reference_value, 1U);
		EXPECT_EQ(plans[0].erased_canonical_key, std::optional{canonical("one@example.test")});
		EXPECT_EQ(plans[0].upserted_canonical_key, std::optional{canonical("two@example.test")});
		EXPECT_EQ(plans[1].entity_reference_value, 2U);
		EXPECT_EQ(plans[1].erased_canonical_key, std::optional{canonical("two@example.test")});
		EXPECT_EQ(plans[1].upserted_canonical_key, std::optional{canonical("one@example.test")});
	}

	TEST(ConstraintMutationValidatorTest, RejectsMalformedAndOverBudgetSupportTreeCandidates) {
		FakeGraph graph;
		graph.nodes.emplace(2U, node(2U, {7U}, {{"email", PropertyValue("taken@example.test")}}));
		graph.support_roots.insert(81U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});
		LogicalGraphMutationBatch batch;
		batch.node_creates.push_back({{"Person"}, {{"email", PropertyValue("taken@example.test")}}});

		const std::vector<std::byte> key = ConstraintSupportTree::entryKey(canonical("taken@example.test"), 2U);
		graph.support_entries[81U] = {{key, 3U}};
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::array{unique}, batch)), FormatException);

		graph.support_entries[81U] = {{key, 2U}};
		ConstraintMutationValidator noCollisionBudget(graph.adapter(), {32U, 0U});
		EXPECT_THROW(static_cast<void>(noCollisionBudget.validate(std::array{unique}, batch)), FormatException);
	}

	TEST(ConstraintMutationValidatorTest, UsesFullSnapshotValidationWhenDdlAndExistingConstraintsShareABatch) {
		FakeGraph graph;
		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("old@example.test")}}));
		graph.support_roots.insert(81U);
		addSupportEntry(graph, 81U, "old@example.test", 1U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});
		const ConstraintDefinition required =
				nodeConstraint(82U, ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_required", {"email"});

		LogicalGraphMutationBatch batch;
		batch.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {{"email", PropertyValue("new@example.test")}}});
		const std::vector<ConstraintSupportTreeMutationPlan> plans =
				validator(graph).validate(std::array{unique}, batch, std::array{required});
		ASSERT_EQ(plans.size(), 1U);
		EXPECT_EQ(plans.front().constraint_id, unique.constraint_id);
		EXPECT_EQ(plans.front().erased_canonical_key, std::optional{canonical("old@example.test")});
		EXPECT_EQ(plans.front().upserted_canonical_key, std::optional{canonical("new@example.test")});
	}

	TEST(ConstraintMutationValidatorTest, ValidatesUntypedEdgeConstraintsWithoutAllocatingAToken) {
		FakeGraph graph;
		ConstraintDefinition required;
		required.constraint_id = 91U;
		required.name = "untyped_edge_weight_required";
		required.entity_kind = ConstraintDefinitionEntityKind::CDEK_EDGE;
		required.kind = ConstraintDefinitionKind::CDK_NOT_NULL;
		required.schema_token_id = 0U;
		required.property_names = {"weight"};

		LogicalGraphMutationBatch missingProperty;
		missingProperty.edge_creates.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 "", {}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::array{required}, missingProperty)),
					 std::runtime_error);
	}

	TEST(ConstraintMutationValidatorTest, ValidatesEveryDeclaredPropertyTypeInTheFullSnapshotPath) {
		const std::array cases = {
				std::pair{ConstraintDefinitionPropertyType::CDPT_BOOLEAN, PropertyValue(true)},
				std::pair{ConstraintDefinitionPropertyType::CDPT_INTEGER, PropertyValue(int64_t{7})},
				std::pair{ConstraintDefinitionPropertyType::CDPT_FLOAT, PropertyValue(7.5)},
				std::pair{ConstraintDefinitionPropertyType::CDPT_STRING, PropertyValue("value")},
				std::pair{ConstraintDefinitionPropertyType::CDPT_LIST,
						  PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t{7})})},
				std::pair{ConstraintDefinitionPropertyType::CDPT_MAP,
						  PropertyValue(PropertyValue::MapType{{"key", PropertyValue("value")}})},
		};

		for (const auto &[type, value]: cases) {
			FakeGraph graph;
			ConstraintDefinition definition =
					nodeConstraint(90U, ConstraintDefinitionKind::CDK_PROPERTY_TYPE, "typed_property", {"value"});
			definition.property_type = type;
			LogicalGraphMutationBatch batch;
			batch.node_creates.push_back({{"Person"}, {{"value", value}}});
			EXPECT_NO_THROW(static_cast<void>(
					validator(graph).validate(std::span<const ConstraintDefinition>{}, batch, std::array{definition})));
		}

		FakeGraph graph;
		ConstraintDefinition noDeclaredType =
				nodeConstraint(91U, ConstraintDefinitionKind::CDK_PROPERTY_TYPE, "missing_declared_type", {"value"});
		noDeclaredType.property_type = ConstraintDefinitionPropertyType::CDPT_NONE;
		LogicalGraphMutationBatch batch;
		batch.node_creates.push_back({{"Person"}, {{"value", PropertyValue("value")}}});
		EXPECT_THROW(static_cast<void>(
					validator(graph).validate(std::span<const ConstraintDefinition>{}, batch, std::array{noDeclaredType})),
				 std::runtime_error);
	}

	TEST(ConstraintMutationValidatorTest, RejectsEmptyAndUnresolvedSchemaNamesBeforePlanningMutations) {
		const ConstraintDefinition requiredEmail =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_required", {"email"});

		FakeGraph graph;
		LogicalGraphMutationBatch emptyLabel;
		emptyLabel.node_creates.push_back({{""}, {{"email", PropertyValue("one@example.test")}}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::array{requiredEmail}, emptyLabel)),
					 std::invalid_argument);

		ConstraintMutationValidationAdapter zeroLabelAdapter = graph.adapter();
		zeroLabelAdapter.resolve_node_label_token = [](std::string_view) { return uint64_t{0U}; };
		ConstraintMutationValidator zeroLabelValidator(std::move(zeroLabelAdapter), {32U, 8U});
		LogicalGraphMutationBatch unresolvedLabel;
		unresolvedLabel.node_creates.push_back({{"Person"}, {{"email", PropertyValue("one@example.test")}}});
		EXPECT_THROW(static_cast<void>(zeroLabelValidator.validate(std::array{requiredEmail}, unresolvedLabel)),
					 FormatException);

		ConstraintMutationValidationAdapter zeroTypeAdapter = graph.adapter();
		zeroTypeAdapter.resolve_relationship_type_token = [](std::string_view) { return uint64_t{0U}; };
		ConstraintMutationValidator zeroTypeValidator(std::move(zeroTypeAdapter), {32U, 8U});
		LogicalGraphMutationBatch unresolvedType;
		unresolvedType.edge_creates.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 "KNOWS", {}});
		EXPECT_THROW(static_cast<void>(zeroTypeValidator.validate(std::array{requiredEmail}, unresolvedType)),
					 FormatException);
	}

	TEST(ConstraintMutationValidatorTest, AppliesCreatedEntitiesAndDeletesDuringFullSnapshotValidation) {
		FakeGraph graph;
		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("retired@example.test")}}));
		ConstraintValidationEntity edge;
		edge.kind = LogicalGraphMutationEntityKind::LGMK_EDGE;
		edge.stable_id = 10U;
		edge.schema_tokens = {9U};
		edge.properties = {{"weight", PropertyValue(int64_t{1})}};
		graph.edges.emplace(10U, std::move(edge));
		graph.support_roots.insert(81U);
		addSupportEntry(graph, 81U, "retired@example.test", 1U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});
		const ConstraintDefinition required =
				nodeConstraint(82U, ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_required", {"email"});

		LogicalGraphMutationBatch batch;
		batch.node_creates.push_back({{"Person"}, {{"email", PropertyValue("created@example.test")}}});
		batch.edge_creates.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 "KNOWS", {{"weight", PropertyValue(int64_t{2})}}});
		batch.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 {{"email", PropertyValue("final@example.test")}}});
		batch.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U},
				 {{"weight", PropertyValue(int64_t{3})}}});
		batch.node_label_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 {"Person", "Person"}});
		batch.node_deletes.push_back({1U, false, 0U});
		batch.edge_deletes.push_back({10U});

		const std::vector<ConstraintSupportTreeMutationPlan> plans =
				validator(graph).validate(std::array{unique}, batch, std::array{required});
		ASSERT_EQ(plans.size(), 2U);
		EXPECT_EQ(plans[0].entity_reference_kind, ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_NODE);
		EXPECT_EQ(plans[0].entity_reference_value, 1U);
		EXPECT_EQ(plans[0].erased_canonical_key, std::optional{canonical("retired@example.test")});
		EXPECT_FALSE(plans[0].upserted_canonical_key.has_value());
		EXPECT_EQ(plans[1].entity_reference_kind, ConstraintSupportTreeEntityReferenceKind::CSTREK_CREATED_NODE);
		EXPECT_EQ(plans[1].entity_reference_value, 0U);
		EXPECT_EQ(plans[1].upserted_canonical_key, std::optional{canonical("final@example.test")});
	}

	TEST(ConstraintMutationValidatorTest, PlansCreatedEdgeUniqueTransitionsThroughTheAffectedEntityPath) {
		FakeGraph graph;
		graph.support_roots.insert(92U);
		ConstraintDefinition unique;
		unique.constraint_id = 92U;
		unique.name = "knows_code_unique";
		unique.entity_kind = ConstraintDefinitionEntityKind::CDEK_EDGE;
		unique.kind = ConstraintDefinitionKind::CDK_UNIQUE;
		unique.schema_token_id = 9U;
		unique.property_names = {"code"};

		LogicalGraphMutationBatch batch;
		batch.edge_creates.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 "KNOWS", {{"code", PropertyValue("initial")}}});
		batch.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U},
				 {{"code", PropertyValue("final")}}});

		const std::vector<ConstraintSupportTreeMutationPlan> plans =
				validator(graph).validate(std::array{unique}, batch);
		ASSERT_EQ(plans.size(), 1U);
		EXPECT_EQ(plans.front().entity_reference_kind, ConstraintSupportTreeEntityReferenceKind::CSTREK_CREATED_EDGE);
		EXPECT_EQ(plans.front().entity_reference_value, 0U);
		EXPECT_EQ(plans.front().upserted_canonical_key, std::optional{canonical("final")});
	}

	TEST(ConstraintMutationValidatorTest, RejectsDuplicateBatchKeysAndUnknownReferenceKinds) {
		FakeGraph graph;
		graph.support_roots.insert(81U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});
		LogicalGraphMutationBatch duplicate;
		duplicate.node_creates.push_back({{"Person"}, {{"email", PropertyValue("same@example.test")}}});
		duplicate.node_creates.push_back({{"Person"}, {{"email", PropertyValue("same@example.test")}}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::array{unique}, duplicate)), std::runtime_error);

		const ConstraintDefinition required =
				nodeConstraint(82U, ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_required", {"email"});
		LogicalGraphMutationBatch unknownReference;
		unknownReference.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE,
				  static_cast<LogicalGraphMutationReferenceKind>(0xffU),
				  1U},
				 {{"email", PropertyValue("one@example.test")}}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::array{required}, unknownReference)),
					 std::invalid_argument);
	}

	TEST(ConstraintMutationValidatorTest, RejectsMismatchedPersistedEntitiesAndStaleSupportEntries) {
		FakeGraph graph;
		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("old@example.test")}}));
		const ConstraintDefinition required =
				nodeConstraint(82U, ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_required", {"email"});
		ConstraintMutationValidationAdapter mismatchedAdapter = graph.adapter();
		mismatchedAdapter.read_persisted_entity = [](LogicalGraphMutationEntityKind kind, uint64_t) {
			return node(2U, kind == LogicalGraphMutationEntityKind::LGMK_NODE ? std::vector<uint64_t>{7U}
																					 : std::vector<uint64_t>{},
						{{"email", PropertyValue("old@example.test")}});
		};
		ConstraintMutationValidator mismatchedValidator(std::move(mismatchedAdapter), {32U, 8U});
		LogicalGraphMutationBatch persistedUpdate;
		persistedUpdate.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {{"email", PropertyValue("new@example.test")}}});
		EXPECT_THROW(static_cast<void>(mismatchedValidator.validate(std::array{required}, persistedUpdate)),
					 FormatException);

		graph.nodes.emplace(2U, node(2U, {7U}, {}));
		graph.support_roots.insert(81U);
		addSupportEntry(graph, 81U, "wanted@example.test", 2U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});
		LogicalGraphMutationBatch createWanted;
		createWanted.node_creates.push_back({{"Person"}, {{"email", PropertyValue("wanted@example.test")}}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::array{unique}, createWanted)), FormatException);
	}

	TEST(ConstraintMutationValidatorTest, StopsAtTheFirstDifferentSupportDigestAndRejectsInvalidFullScans) {
		FakeGraph graph;
		graph.support_roots.insert(81U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});
		ConstraintMutationValidationAdapter differentDigestAdapter = graph.adapter();
		differentDigestAdapter.scan_support_tree = [](uint64_t, std::span<const std::byte>, size_t) {
			return std::vector<BTreePageEntry>{{ConstraintSupportTree::entryKey(canonical("other@example.test"), 1U), 1U}};
		};
		ConstraintMutationValidator differentDigestValidator(std::move(differentDigestAdapter), {32U, 8U});
		LogicalGraphMutationBatch create;
		create.node_creates.push_back({{"Person"}, {{"email", PropertyValue("new@example.test")}}});
		EXPECT_EQ(differentDigestValidator.validate(std::array{unique}, create).size(), 1U);

		ConstraintMutationValidationAdapter invalidScanAdapter = graph.adapter();
		invalidScanAdapter.visit_persisted_entities = [](LogicalGraphMutationEntityKind, size_t,
																		 const ConstraintValidationEntityVisitor &visitor) {
			ConstraintValidationEntity invalid;
			invalid.kind = LogicalGraphMutationEntityKind::LGMK_EDGE;
			invalid.stable_id = 1U;
			visitor(invalid);
		};
		ConstraintMutationValidator invalidScanValidator(std::move(invalidScanAdapter), {32U, 8U});
		const ConstraintDefinition required =
				nodeConstraint(82U, ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_required", {"email"});
		EXPECT_THROW(static_cast<void>(invalidScanValidator.validate(std::span<const ConstraintDefinition>{}, {},
																			 std::array{required})),
					 FormatException);
	}

	TEST(ConstraintMutationValidatorTest, PlansPersistedEdgeUniqueTransitionsDuringFullSnapshotValidation) {
		FakeGraph graph;
		ConstraintValidationEntity edge;
		edge.kind = LogicalGraphMutationEntityKind::LGMK_EDGE;
		edge.stable_id = 10U;
		edge.schema_tokens = {9U};
		edge.properties = {{"code", PropertyValue("old")}};
		graph.edges.emplace(edge.stable_id, std::move(edge));
		graph.support_roots.insert(92U);
		addSupportEntry(graph, 92U, "old", 10U);

		ConstraintDefinition edgeUnique;
		edgeUnique.constraint_id = 92U;
		edgeUnique.name = "knows_code_unique";
		edgeUnique.entity_kind = ConstraintDefinitionEntityKind::CDEK_EDGE;
		edgeUnique.kind = ConstraintDefinitionKind::CDK_UNIQUE;
		edgeUnique.schema_token_id = 9U;
		edgeUnique.property_names = {"code"};
		const ConstraintDefinition required =
				nodeConstraint(82U, ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_required", {"email"});

		LogicalGraphMutationBatch batch;
		batch.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 10U},
				 {{"code", PropertyValue("new")}}});

		const std::vector<ConstraintSupportTreeMutationPlan> plans =
				validator(graph).validate(std::array{edgeUnique}, batch, std::array{required});
		ASSERT_EQ(plans.size(), 1U);
		EXPECT_EQ(plans.front().entity_reference_kind, ConstraintSupportTreeEntityReferenceKind::CSTREK_PERSISTED_EDGE);
		EXPECT_EQ(plans.front().entity_reference_value, 10U);
		EXPECT_EQ(plans.front().erased_canonical_key, std::optional{canonical("old")});
		EXPECT_EQ(plans.front().upserted_canonical_key, std::optional{canonical("new")});
	}

	TEST(ConstraintMutationValidatorTest, RejectsInvalidTargetsDuringFullSnapshotValidation) {
		FakeGraph graph;
		const ConstraintDefinition required =
				nodeConstraint(82U, ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_required", {"email"});

		LogicalGraphMutationBatch missingPersisted;
		missingPersisted.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {{"email", PropertyValue("one@example.test")}}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::span<const ConstraintDefinition>{}, missingPersisted,
																							 std::array{required})),
					 std::runtime_error);

		LogicalGraphMutationBatch invalidCreatedNode;
		invalidCreatedNode.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				 {{"email", PropertyValue("one@example.test")}}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::span<const ConstraintDefinition>{}, invalidCreatedNode,
																							 std::array{required})),
					 std::invalid_argument);

		LogicalGraphMutationBatch invalidCreatedEdge;
		invalidCreatedEdge.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U},
				 {{"email", PropertyValue("one@example.test")}}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::span<const ConstraintDefinition>{}, invalidCreatedEdge,
																							 std::array{required})),
					 std::invalid_argument);

		LogicalGraphMutationBatch unknownReference;
		unknownReference.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE,
				  static_cast<LogicalGraphMutationReferenceKind>(0xffU),
				  0U},
				 {{"email", PropertyValue("one@example.test")}}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::span<const ConstraintDefinition>{}, unknownReference,
																							 std::array{required})),
					 std::invalid_argument);

		LogicalGraphMutationBatch edgeLabelMutation;
		edgeLabelMutation.node_label_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {"Person"}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::span<const ConstraintDefinition>{}, edgeLabelMutation,
																							 std::array{required})),
					 std::invalid_argument);
	}

	TEST(ConstraintMutationValidatorTest, AppliesRepeatedUpdatesAndRejectsExplicitNullProperties) {
		FakeGraph graph;
		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("old@example.test")}}));
		graph.support_roots.insert(81U);
		addSupportEntry(graph, 81U, "old@example.test", 1U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});

		LogicalGraphMutationBatch repeated;
		repeated.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {{"email", PropertyValue("intermediate@example.test")}}});
		repeated.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {{"email", PropertyValue("final@example.test")}}});
		const std::vector<ConstraintSupportTreeMutationPlan> plans = validator(graph).validate(std::array{unique}, repeated);
		ASSERT_EQ(plans.size(), 1U);
		EXPECT_EQ(plans.front().upserted_canonical_key, std::optional{canonical("final@example.test")});

		const ConstraintDefinition required =
				nodeConstraint(82U, ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_required", {"email"});
		LogicalGraphMutationBatch explicitNull;
		explicitNull.node_creates.push_back({{"Person"}, {{"email", PropertyValue{}}}});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::array{required}, explicitNull)), std::runtime_error);

		LogicalGraphMutationBatch deleted;
		deleted.node_deletes.push_back({1U, false, 0U});
		EXPECT_NO_THROW(static_cast<void>(validator(graph).validate(std::array{required}, deleted)));
	}

	TEST(ConstraintMutationValidatorTest, RemovesAUniqueSupportKeyWhenAnExistingPropertyBecomesNull) {
		FakeGraph graph;
		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("old@example.test")}}));
		graph.support_roots.insert(81U);
		addSupportEntry(graph, 81U, "old@example.test", 1U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});

		LogicalGraphMutationBatch batch;
		batch.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {{"email", PropertyValue{}}}});

		const std::vector<ConstraintSupportTreeMutationPlan> plans = validator(graph).validate(std::array{unique}, batch);
		ASSERT_EQ(plans.size(), 1U);
		EXPECT_EQ(plans.front().erased_canonical_key, std::optional{canonical("old@example.test")});
		EXPECT_FALSE(plans.front().upserted_canonical_key.has_value());
	}

	TEST(ConstraintMutationValidatorTest, AllowsAKeyReleasedByADeletedSupportTreeCandidate) {
		FakeGraph graph;
		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("old@example.test")}}));
		graph.nodes.emplace(2U, node(2U, {7U}, {{"email", PropertyValue("released@example.test")}}));
		graph.support_roots.insert(81U);
		addSupportEntry(graph, 81U, "old@example.test", 1U);
		addSupportEntry(graph, 81U, "released@example.test", 2U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});

		LogicalGraphMutationBatch batch;
		batch.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {{"email", PropertyValue("released@example.test")}}});
		batch.node_deletes.push_back({2U, false, 0U});

		const std::vector<ConstraintSupportTreeMutationPlan> plans = validator(graph).validate(std::array{unique}, batch);
		ASSERT_EQ(plans.size(), 2U);
		EXPECT_EQ(plans[0].entity_reference_value, 1U);
		EXPECT_EQ(plans[0].upserted_canonical_key, std::optional{canonical("released@example.test")});
		EXPECT_EQ(plans[1].entity_reference_value, 2U);
		EXPECT_EQ(plans[1].erased_canonical_key, std::optional{canonical("released@example.test")});
		EXPECT_FALSE(plans[1].upserted_canonical_key.has_value());
	}

	TEST(ConstraintMutationValidatorTest, RejectsAStaleTargetKeyEntryForTheMutatedEntity) {
		FakeGraph graph;
		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("old@example.test")}}));
		graph.support_roots.insert(81U);
		addSupportEntry(graph, 81U, "old@example.test", 1U);
		// A changed Entity may not already have an entry for its unpublished target key.
		addSupportEntry(graph, 81U, "new@example.test", 1U);
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});

		LogicalGraphMutationBatch batch;
		batch.property_replacements.push_back(
				{{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				 {{"email", PropertyValue("new@example.test")}}});

		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::array{unique}, batch)), FormatException);
	}

	TEST(ConstraintMutationValidatorTest, RejectsDuplicateEntitiesAndMissingSupportRootsDuringFullSnapshotValidation) {
		FakeGraph graph;
		const ConstraintDefinition required =
				nodeConstraint(82U, ConstraintDefinitionKind::CDK_NOT_NULL, "person_email_required", {"email"});
		ConstraintMutationValidationAdapter duplicateScanAdapter = graph.adapter();
		duplicateScanAdapter.visit_persisted_entities = [](LogicalGraphMutationEntityKind kind, size_t,
																						 const ConstraintValidationEntityVisitor &visitor) {
			ConstraintValidationEntity entity;
			entity.kind = kind;
			entity.stable_id = 1U;
			entity.schema_tokens = {kind == LogicalGraphMutationEntityKind::LGMK_NODE ? 7U : 9U};
			entity.properties = {{"email", PropertyValue("one@example.test")}};
			visitor(entity);
			visitor(entity);
		};
		ConstraintMutationValidator duplicateScanValidator(std::move(duplicateScanAdapter), {32U, 8U});
		EXPECT_THROW(static_cast<void>(duplicateScanValidator.validate(std::span<const ConstraintDefinition>{}, {},
																													  std::array{required})),
					 FormatException);

		graph.nodes.emplace(1U, node(1U, {7U}, {{"email", PropertyValue("old@example.test")}}));
		const ConstraintDefinition unique =
				nodeConstraint(81U, ConstraintDefinitionKind::CDK_UNIQUE, "person_email_unique", {"email"});
		EXPECT_THROW(static_cast<void>(validator(graph).validate(std::array{unique}, {}, std::array{required})),
					 FormatException);
	}

} // namespace graph::storage
