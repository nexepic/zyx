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

	std::vector<uint64_t> nodeIds;
	adapter.visit_persisted_entities(LogicalGraphMutationEntityKind::LGMK_NODE, 2U,
									  [&nodeIds](const ConstraintValidationEntity &entity) { nodeIds.push_back(entity.stable_id); });
	EXPECT_EQ(nodeIds, (std::vector<uint64_t>{firstNode, secondNode}));
	std::vector<uint64_t> edgeIds;
	adapter.visit_persisted_entities(LogicalGraphMutationEntityKind::LGMK_EDGE, 1U,
									  [&edgeIds](const ConstraintValidationEntity &entity) { edgeIds.push_back(entity.stable_id); });
	EXPECT_EQ(edgeIds, (std::vector<uint64_t>{edgeId}));
	EXPECT_FALSE(adapter.has_support_tree(71U));
	EXPECT_TRUE(adapter.scan_support_tree(71U, {}, 1U).empty());
	EXPECT_EQ(adapter.resolve_node_label_token("Person"), 7U);
	EXPECT_EQ(adapter.resolve_relationship_type_token("KNOWS"), 11U);
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
