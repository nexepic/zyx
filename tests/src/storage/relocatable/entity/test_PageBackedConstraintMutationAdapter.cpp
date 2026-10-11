/**
 * @file test_PageBackedConstraintMutationAdapter.cpp
 * @brief Public contract tests for native constraint-validation adaptation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedConstraintMutationAdapter.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
using graph::Edge;
using graph::Node;
using graph::PropertyValue;

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

	PageBackedConstraintPropertyKeyResolver propertyNames() {
		return [](const MetadataSnapshotPin &, uint32_t token) {
			if (token == 17U) {
				return std::string("name");
			}
			if (token == 19U) {
				return std::string("weight");
			}
			return std::string{};
		};
	}

	PageBackedConstraintNodeLabelTokenResolver nodeLabels() {
		return [](const MetadataSnapshotPin &, std::string_view name) { return name == "Person" ? 7U : 0U; };
	}

	PageBackedConstraintRelationshipTypeTokenResolver relationshipTypes() {
		return [](const MetadataSnapshotPin &, std::string_view name) { return name == "KNOWS" ? 11U : 0U; };
	}

} // namespace

class PageBackedConstraintMutationAdapterTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {};

TEST_F(PageBackedConstraintMutationAdapterTest, ValidatesConstructorDependenciesBeforeExposingCallbacks) {
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	MetadataSnapshotPin invalid;
	EXPECT_THROW(static_cast<void>(PageBackedConstraintMutationAdapter(
				 invalid, graph, *btree_pages_, *metadata_, propertyNames(), nodeLabels(), relationshipTypes())),
			 std::invalid_argument);

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(PageBackedConstraintMutationAdapter(
				 snapshot, graph, *btree_pages_, *metadata_, {}, nodeLabels(), relationshipTypes())),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(PageBackedConstraintMutationAdapter(
				 snapshot, graph, *btree_pages_, *metadata_, propertyNames(), {}, relationshipTypes())),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(PageBackedConstraintMutationAdapter(
				 snapshot, graph, *btree_pages_, *metadata_, propertyNames(), nodeLabels(), {})),
			 std::invalid_argument);
}

TEST_F(PageBackedConstraintMutationAdapterTest, MaterializesNodesEdgesAndBoundedScansFromOneSnapshot) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t firstNode = create.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t secondNode = create.createNode(std::array<uint64_t, 1U>{7U});
	const uint64_t edgeId = create.createEdge(firstNode, secondNode, 11U);
	create.replaceNodeProperty({firstNode, Node::typeId, {{17U, PropertyValue(std::string("Ada"))}}});
	create.replaceEdgeProperty({edgeId, Edge::typeId, {{19U, PropertyValue(int64_t(3))}}});
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	PageBackedConstraintMutationAdapter native(snapshot, graph, *btree_pages_, *metadata_, propertyNames(), nodeLabels(),
											 relationshipTypes());
	const ConstraintMutationValidationAdapter adapter = native.makeAdapter();

	const ConstraintValidationEntity node = adapter.read_persisted_entity(LogicalGraphMutationEntityKind::LGMK_NODE, firstNode);
	EXPECT_EQ(node.kind, LogicalGraphMutationEntityKind::LGMK_NODE);
	EXPECT_EQ(node.stable_id, firstNode);
	EXPECT_EQ(node.schema_tokens, (std::vector<uint64_t>{7U}));
	ASSERT_EQ(node.properties.size(), 1U);
	EXPECT_EQ(node.properties.at("name"), PropertyValue(std::string("Ada")));

	const ConstraintValidationEntity edge = adapter.read_persisted_entity(LogicalGraphMutationEntityKind::LGMK_EDGE, edgeId);
	EXPECT_EQ(edge.kind, LogicalGraphMutationEntityKind::LGMK_EDGE);
	EXPECT_EQ(edge.stable_id, edgeId);
	EXPECT_EQ(edge.schema_tokens, (std::vector<uint64_t>{11U}));
	ASSERT_EQ(edge.properties.size(), 1U);
	EXPECT_EQ(edge.properties.at("weight"), PropertyValue(int64_t(3)));

	const bool traceWasEnabled = graph::debug::PerfTrace::isEnabled();
	const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	graph::debug::PerfTrace::reset();
	std::vector<uint64_t> nodeIds;
	adapter.visit_persisted_entities(LogicalGraphMutationEntityKind::LGMK_NODE, 2U,
			[&](const ConstraintValidationEntity &entity) {
				nodeIds.push_back(entity.stable_id);
				EXPECT_EQ(entity.kind, LogicalGraphMutationEntityKind::LGMK_NODE);
				EXPECT_EQ(entity.schema_tokens, (std::vector<uint64_t>{7U}));
				if (entity.stable_id == firstNode) {
					EXPECT_EQ(entity.properties, node.properties);
				} else {
					EXPECT_TRUE(entity.properties.empty());
				}
			});
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{firstNode, secondNode}));
	std::vector<uint64_t> edgeIds;
	adapter.visit_persisted_entities(LogicalGraphMutationEntityKind::LGMK_EDGE, 1U,
			[&](const ConstraintValidationEntity &entity) {
				edgeIds.push_back(entity.stable_id);
				EXPECT_EQ(entity.kind, LogicalGraphMutationEntityKind::LGMK_EDGE);
				EXPECT_EQ(entity.schema_tokens, edge.schema_tokens);
				EXPECT_EQ(entity.properties, edge.properties);
			});
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
	graph::debug::PerfTrace::setEnabled(traceWasEnabled);
	EXPECT_FALSE(trace.contains("native_graph.read.entity_node_record_cache_miss"));
	EXPECT_FALSE(trace.contains("native_graph.read.entity_edge_record_cache_miss"));
	ASSERT_TRUE(trace.contains("native_graph.read.node_page_scan.records"));
	EXPECT_EQ(trace.at("native_graph.read.node_page_scan.records").totalValue, 2);
	ASSERT_TRUE(trace.contains("native_graph.read.edge_page_scan.records"));
	EXPECT_EQ(trace.at("native_graph.read.edge_page_scan.records").totalValue, 1);
	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{edgeId}));
	EXPECT_FALSE(adapter.has_support_tree(71U));
	EXPECT_TRUE(adapter.scan_support_tree(71U, {}, 1U).empty());
	EXPECT_EQ(adapter.resolve_node_label_token("Person"), 7U);
	EXPECT_EQ(adapter.resolve_relationship_type_token("KNOWS"), 11U);
}

TEST_F(PageBackedConstraintMutationAdapterTest, TransferredPropertiesRemainIndependentOfPersistedReadsAndScans) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	auto create = planner.beginTransaction();
	const auto nodeId = create.createNode(std::array<uint64_t, 1U>{7U});
	const auto edgeId = create.createEdge(nodeId, nodeId, 11U);
	const PropertyValue payload(std::vector<PropertyValue>(16U, PropertyValue(std::string(80U, 'p'))));
	create.replaceNodeProperty({nodeId, Node::typeId, {{17U, payload}}});
	create.replaceEdgeProperty({edgeId, Edge::typeId, {{19U, payload}}});
	ASSERT_TRUE(create.commit().committed());
	auto snapshot = metadata_->openSnapshot();
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	PageBackedConstraintMutationAdapter native(snapshot, graph, *btree_pages_, *metadata_, propertyNames(), nodeLabels(), relationshipTypes());
	const auto adapter = native.makeAdapter();
	for (auto kind : {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationEntityKind::LGMK_EDGE}) {
		const bool node = kind == LogicalGraphMutationEntityKind::LGMK_NODE;
		const auto id = node ? nodeId : edgeId;
		const auto key = node ? "name" : "weight";
		for (int retry = 0; retry < 2; ++retry) {
			auto result = adapter.read_persisted_entity(kind, id);
			EXPECT_EQ(result.properties.at(key), payload);
			result.properties.at(key) = PropertyValue("changed");
			adapter.visit_persisted_entities(kind, 1U, [&](const ConstraintValidationEntity &entity) {
				EXPECT_EQ(entity.properties.at(key), payload);
				EXPECT_EQ(entity.schema_tokens, (std::vector<uint64_t>{node ? 7U : 11U}));
			});
		}
	}
}

TEST_F(PageBackedConstraintMutationAdapterTest, RejectsInvalidKindsBoundsAndResolverResults) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	NativeGraphWriteTransaction create = planner.beginTransaction();
	const uint64_t firstNode = create.createNode(std::array<uint64_t, 1U>{7U});
	static_cast<void>(create.createNode(std::array<uint64_t, 1U>{7U}));
	create.replaceNodeProperty(
			{firstNode, Node::typeId, {{23U, PropertyValue(int64_t(1))}, {24U, PropertyValue(int64_t(2))}}});
	ASSERT_TRUE(create.commit().committed());

	MetadataSnapshotPin snapshot = metadata_->openSnapshot();
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	PageBackedConstraintMutationAdapter native(snapshot, graph, *btree_pages_, *metadata_, propertyNames(), nodeLabels(),
											 relationshipTypes());
	const ConstraintMutationValidationAdapter adapter = native.makeAdapter();
	const auto unknownKind = static_cast<LogicalGraphMutationEntityKind>(255U);
	EXPECT_THROW(static_cast<void>(adapter.read_persisted_entity(LogicalGraphMutationEntityKind::LGMK_NODE, 0U)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.read_persisted_entity(LogicalGraphMutationEntityKind::LGMK_NODE, firstNode)),
			 FormatException);
	PageBackedConstraintMutationAdapter duplicateNames(
			snapshot, graph, *btree_pages_, *metadata_,
			[](const MetadataSnapshotPin &, uint32_t) { return std::string("duplicate"); }, nodeLabels(),
			relationshipTypes());
	EXPECT_THROW(static_cast<void>(duplicateNames.makeAdapter().read_persisted_entity(
						 LogicalGraphMutationEntityKind::LGMK_NODE, firstNode)),
			 FormatException);
	EXPECT_THROW(static_cast<void>(adapter.read_persisted_entity(unknownKind, firstNode)), std::invalid_argument);
	EXPECT_THROW(adapter.visit_persisted_entities(LogicalGraphMutationEntityKind::LGMK_NODE, 0U,
															  [](const ConstraintValidationEntity &) {}),
			 FormatException);
	EXPECT_THROW(adapter.visit_persisted_entities(LogicalGraphMutationEntityKind::LGMK_NODE,
															  (std::numeric_limits<size_t>::max)(),
															  [](const ConstraintValidationEntity &) {}),
			 FormatException);
	EXPECT_THROW(adapter.visit_persisted_entities(LogicalGraphMutationEntityKind::LGMK_NODE, 1U, {}), FormatException);
	EXPECT_THROW(adapter.visit_persisted_entities(LogicalGraphMutationEntityKind::LGMK_NODE, 1U,
															  [](const ConstraintValidationEntity &) {}),
			 FormatException);
	EXPECT_THROW(adapter.visit_persisted_entities(unknownKind, 1U, [](const ConstraintValidationEntity &) {}),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(adapter.resolve_node_label_token("Missing")), FormatException);
	EXPECT_THROW(static_cast<void>(adapter.resolve_relationship_type_token("Missing")), FormatException);
}

TEST_F(PageBackedConstraintMutationAdapterTest, SelectedConstraintFieldsPreserveMetadataBatchBoundsAndRetry) {
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, plannerOptions());
	auto create = planner.beginTransaction();
	std::vector<uint64_t> ids;
	for (size_t i = 0U; i < 130U; ++i) ids.push_back(create.createNode(std::array<uint64_t, 1U>{7U}));
	const auto edge = create.createEdge(ids[0], ids[1], 11U);
	create.replaceNodeProperty({ids[0], Node::typeId, {{17U, PropertyValue("Ada")}, {25U, PropertyValue(std::string(1000U, 'p'))}}});
	create.replaceEdgeProperty({edge, Edge::typeId, {{19U, PropertyValue(3)}, {25U, PropertyValue(std::string(1000U, 'p'))}}});
	ASSERT_TRUE(create.commit().committed());
	auto snapshot = metadata_->openSnapshot();
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	PageBackedConstraintMutationAdapter native(snapshot, graph, *btree_pages_, *metadata_, propertyNames(), nodeLabels(),
		relationshipTypes(), PageBackedConstraintPropertySelection{{17U, 37U}, {19U}});
	const auto adapter = native.makeAdapter();
	for (size_t retry = 0U; retry < 2U; ++retry) {
		const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
		graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
		const auto node = adapter.read_persisted_entity(LogicalGraphMutationEntityKind::LGMK_NODE, ids[0]);
		EXPECT_EQ(node.schema_tokens, (std::vector<uint64_t>{7U}));
		ASSERT_EQ(node.properties.size(), 1U); EXPECT_EQ(node.properties.at("name"), PropertyValue("Ada"));
		const auto relationship = adapter.read_persisted_entity(LogicalGraphMutationEntityKind::LGMK_EDGE, edge);
		EXPECT_EQ(relationship.schema_tokens, (std::vector<uint64_t>{11U}));
		ASSERT_EQ(relationship.properties.size(), 1U); EXPECT_EQ(relationship.properties.at("weight"), PropertyValue(3));
		std::vector<uint64_t> scanned;
		adapter.visit_persisted_entities(LogicalGraphMutationEntityKind::LGMK_NODE, 130U, [&](const auto &entity) {
			scanned.push_back(entity.stable_id);
			EXPECT_EQ(entity.schema_tokens, (std::vector<uint64_t>{7U}));
			if (entity.stable_id != ids[0]) EXPECT_TRUE(entity.properties.empty());
		});
		EXPECT_EQ(scanned, ids);
		scanned.clear();
		EXPECT_THROW(adapter.visit_persisted_entities(LogicalGraphMutationEntityKind::LGMK_NODE, 129U,
			[&](const auto &entity) { scanned.push_back(entity.stable_id); }), FormatException);
		EXPECT_EQ(scanned.size(), 129U);
		adapter.visit_persisted_entities(LogicalGraphMutationEntityKind::LGMK_EDGE, 1U, [&](const auto &entity) {
			EXPECT_EQ(entity.stable_id, edge); EXPECT_EQ(entity.properties, relationship.properties);
		});
		const auto trace = graph::debug::PerfTrace::snapshotAndReset();
		graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
		EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
	}
	PageBackedConstraintMutationAdapter metadataOnly(snapshot, graph, *btree_pages_, *metadata_, propertyNames(), nodeLabels(),
		relationshipTypes(), PageBackedConstraintPropertySelection{});
	EXPECT_TRUE(metadataOnly.makeAdapter().read_persisted_entity(LogicalGraphMutationEntityKind::LGMK_NODE, ids[0]).properties.empty());
}

TEST_F(PageBackedConstraintMutationAdapterTest, ConstraintSelectionSeparatesNodeEdgeFieldsAndDeduplicatesTokens) {
	ConstraintDefinition node; node.property_names = {"weight", "name", "weight", "missing"};
	ConstraintDefinition edge; edge.entity_kind = ConstraintDefinitionEntityKind::CDEK_EDGE; edge.property_names = {"weight"};
	const auto selected = selectConstraintProperties(std::array{node, edge}, [](std::string_view name) -> std::optional<uint32_t> {
		if (name == "name") return 17U;
		if (name == "weight") return 19U;
		return std::nullopt;
	});
	EXPECT_EQ(selected.node_tokens, (std::vector<uint32_t>{17U, 19U}));
	EXPECT_EQ(selected.edge_tokens, (std::vector<uint32_t>{19U}));
}
