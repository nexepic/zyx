/**
 * @file test_LogicalGraphReadContract.cpp
 * @brief Default-contract tests for the format-independent graph read API.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <gtest/gtest.h>

#include <stdexcept>

#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"

namespace graph::storage {

	namespace {

		class MinimalLogicalGraphSnapshot : public LogicalGraphReadSnapshot {
		public:
			bool return_malformed_edges = false;

			[[nodiscard]] LogicalGraphNode readNode(uint64_t) const override { return {}; }
			[[nodiscard]] LogicalGraphEdge readEdge(uint64_t) const override { return {}; }
			void visitNodes(size_t, const std::function<bool(const LogicalGraphNode &)> &) const override {}
			[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override { return {}; }
			[[nodiscard]] LogicalGraphAdjacencyPage
			scanAdjacentEdges(uint64_t, LogicalGraphReadDirection,
							  std::optional<LogicalGraphAdjacencyContinuation>, size_t) const override {
				return {};
			}
			void visitEdges(size_t, const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
				static_cast<void>(visitor(return_malformed_edges
						? LogicalGraphEdge{0U, 3U, 5U, "KNOWS"}
						: LogicalGraphEdge{11U, 3U, 5U, "KNOWS"}));
			}
			void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
								const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
				static_cast<void>(visitor(return_malformed_edges
						? LogicalGraphEdge{12U, 1U, 3U, "FOLLOWS"}
						: LogicalGraphEdge{12U, 5U, 3U, "FOLLOWS"}));
			}
		};

		class FallbackCandidateLogicalGraphSnapshot final : public MinimalLogicalGraphSnapshot {
		public:
			std::vector<LogicalGraphNode> candidate_nodes;

			[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNodesByExactProperties(
					const std::vector<std::string> &, const std::vector<std::pair<std::string, PropertyValue>> &,
					size_t, size_t) const override {
				return candidate_nodes;
			}
			[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNodesByPropertyRange(
					const std::vector<std::string> &, const LogicalGraphPropertyRange &, size_t, size_t) const override {
				return candidate_nodes;
			}
			[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNodesByCompositePropertyRange(
					const std::vector<std::string> &, const LogicalGraphCompositePropertyRange &, size_t,
					size_t) const override {
				return candidate_nodes;
			}
		};

		class ShapeAwareLogicalGraphSnapshot final : public LogicalGraphReadSnapshot {
		public:
			mutable std::vector<EntityReadShape> node_shapes;
			mutable std::vector<EntityReadShape> edge_shapes;
			bool return_mismatched_identity = false;

			[[nodiscard]] LogicalGraphNode readNode(uint64_t) const override {
				ADD_FAILURE() << "default shaped helpers must not call the complete Node read";
				return {};
			}
			[[nodiscard]] LogicalGraphNode readNodeShaped(uint64_t nodeId, const EntityReadShape &shape) const override {
				node_shapes.push_back(shape);
				return LogicalGraphNode{return_mismatched_identity ? nodeId + 1U : nodeId,
						shape.include_labels ? std::vector<std::string>{"Person"} :
						std::vector<std::string>{}, shape.all_properties ?
						std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Ada")}} :
						std::vector<LogicalGraphPropertyEntry>{}};
			}
			[[nodiscard]] LogicalGraphEdge readEdge(uint64_t) const override {
				ADD_FAILURE() << "default shaped helpers must not call the complete Edge read";
				return {};
			}
			[[nodiscard]] LogicalGraphEdge readEdgeShaped(uint64_t edgeId, const EntityReadShape &shape) const override {
				edge_shapes.push_back(shape);
				return LogicalGraphEdge{return_mismatched_identity ? edgeId + 1U : edgeId, 1U, 2U,
						shape.include_type ? "KNOWS" : ""};
			}
			void visitNodes(size_t, const std::function<bool(const LogicalGraphNode &)> &) const override {}
			[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override { return {}; }
			[[nodiscard]] LogicalGraphAdjacencyPage
			scanAdjacentEdges(uint64_t, LogicalGraphReadDirection,
							  std::optional<LogicalGraphAdjacencyContinuation>, size_t) const override {
				return {};
			}
			void visitEdges(size_t, const std::function<bool(const LogicalGraphEdge &)>&) const override {}
			void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
								const std::function<bool(const LogicalGraphEdge &)>&) const override {}
		};

		class MinimalLogicalGraphTransactionalBackend final : public LogicalGraphTransactionalBackend {
		public:
			[[nodiscard]] std::unique_ptr<LogicalGraphTransaction> beginLogicalGraphTransaction(bool) override {
				return nullptr;
			}
		};

		class MinimalLogicalGraphReadBackend final : public LogicalGraphReadBackend {
		public:
			[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
				return nullptr;
			}
		};

		class MinimalLogicalGraphStatisticsBackend final : public LogicalGraphStatisticsBackend {
		public:
			[[nodiscard]] std::optional<LogicalGraphCardinalityStatistics>
			readLogicalGraphCardinalityStatistics() const override {
				return std::nullopt;
			}
		};

	} // namespace

	TEST(LogicalGraphReadContractTest, DefaultsRejectUnsupportedProbesAndFallBackToNoOptimizations) {
		MinimalLogicalGraphSnapshot snapshot;
		EXPECT_THROW(static_cast<void>(snapshot.readNodeRef(7U, EntityReadShape{.property_tokens = {2U, 1U}})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(snapshot.readEdgeRef(7U, EntityReadShape{.property_tokens = {3U, 3U}})),
					 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(snapshot.containsNode(7U)), std::logic_error);
		EXPECT_EQ(snapshot.nativeSnapshotGeneration(), std::nullopt);
		EXPECT_EQ(snapshot.findNodesByExactProperties({"Person"}, {{"name", PropertyValue("Ada")}}, 1U, 1U),
				  std::nullopt);
		EXPECT_EQ(snapshot.findNodesByPropertyRange(
					  {"Person"}, {"age", PropertyValue(int64_t{18}), PropertyValue(), true, true}, 1U, 1U),
				  std::nullopt);
		EXPECT_EQ(snapshot.findNodesByCompositePropertyRange(
					  {"Person"}, {{{"city", PropertyValue("Beijing")}},
								 {"age", PropertyValue(int64_t{18}), PropertyValue(), true, true}},
					  1U, 1U),
				  std::nullopt);
		EXPECT_FALSE(snapshot.supportsNodeExistenceProbes());
		EXPECT_EQ(snapshot.mutationFence(), std::nullopt);
	}

	TEST(LogicalGraphReadContractTest, DefaultTopologyVisitorsProjectCompleteEdgesAndValidateCallbacks) {
		MinimalLogicalGraphSnapshot snapshot;
		std::vector<LogicalGraphEdgeTopology> allEdges;
		snapshot.visitEdgeTopologies(1U, [&allEdges](const LogicalGraphEdgeTopology &edge) {
			allEdges.push_back(edge);
			return true;
		});
		ASSERT_EQ(allEdges.size(), 1U);
		EXPECT_EQ(allEdges.front(), (LogicalGraphEdgeTopology{11U, 3U, 5U, "KNOWS"}));

		std::vector<LogicalGraphEdgeTopology> adjacentEdges;
		snapshot.visitAdjacentEdgeTopologies(5U, LogicalGraphReadDirection::LGRD_OUTGOING, 1U,
				[&adjacentEdges](const LogicalGraphEdgeTopology &edge) {
					adjacentEdges.push_back(edge);
					return true;
				});
		ASSERT_EQ(adjacentEdges.size(), 1U);
		EXPECT_EQ(adjacentEdges.front(), (LogicalGraphEdgeTopology{12U, 5U, 3U, "FOLLOWS"}));

		EXPECT_THROW(snapshot.visitEdgeTopologies(1U, {}), std::invalid_argument);
		EXPECT_THROW(snapshot.visitAdjacentEdgeTopologies(5U, LogicalGraphReadDirection::LGRD_INCOMING, 1U, {}),
				 std::invalid_argument);
		const std::vector<uint64_t> unorderedNodes{5U, 3U};
		EXPECT_THROW(snapshot.visitAdjacentEdgeRefsForNodes(unorderedNodes, LogicalGraphReadDirection::LGRD_OUTGOING,
				2U, [](const EntityRef &) { return true; }), std::invalid_argument);
	}

	TEST(LogicalGraphReadContractTest, DefaultTopologyAndReferenceVisitorsFailClosedOnMalformedEdges) {
		MinimalLogicalGraphSnapshot snapshot;
		snapshot.return_malformed_edges = true;
		EXPECT_THROW(snapshot.visitEdgeTopologies(1U, [](const LogicalGraphEdgeTopology &) { return true; }),
				 std::runtime_error);
		EXPECT_THROW(snapshot.visitEdgeRefs(1U, [](const EntityRef &) { return true; }), std::runtime_error);
		EXPECT_THROW(snapshot.visitAdjacentEdgeTopologies(5U, LogicalGraphReadDirection::LGRD_INCOMING, 1U,
				[](const LogicalGraphEdgeTopology &) { return true; }), std::runtime_error);
	}

	TEST(LogicalGraphReadContractTest, DefaultHelpersUseShapedReadsForNarrowRequests) {
		ShapeAwareLogicalGraphSnapshot snapshot;

		const EntityRef nodeRef = snapshot.readNodeRef(7U, EntityReadShape{.all_properties = true});
		EXPECT_EQ(nodeRef, (EntityRef{7U, false, 0U, 0U, 0U}));
		ASSERT_EQ(snapshot.node_shapes.size(), 1U);
		EXPECT_EQ(snapshot.node_shapes.front(), EntityReadShape{});

		EXPECT_TRUE(snapshot.nodeHasLabels(7U, {"Person"}));
		ASSERT_EQ(snapshot.node_shapes.size(), 2U);
		EXPECT_EQ(snapshot.node_shapes.back(), (EntityReadShape{.include_labels = true}));

		EXPECT_EQ(snapshot.readNodeProperty(7U, "name"), PropertyValue("Ada"));
		ASSERT_EQ(snapshot.node_shapes.size(), 3U);
		EXPECT_EQ(snapshot.node_shapes.back(), (EntityReadShape{.all_properties = true}));

		const EntityRef edgeRef = snapshot.readEdgeRef(9U, EntityReadShape{.all_properties = true});
		EXPECT_EQ(edgeRef, (EntityRef{9U, true, 1U, 2U, 0U}));
		ASSERT_EQ(snapshot.edge_shapes.size(), 1U);
		EXPECT_EQ(snapshot.edge_shapes.front(), EntityReadShape{});

		EXPECT_EQ(snapshot.readEdgeProperty(9U, "missing"), PropertyValue());
		ASSERT_EQ(snapshot.edge_shapes.size(), 2U);
		EXPECT_EQ(snapshot.edge_shapes.back(), (EntityReadShape{.all_properties = true}));
	}

	TEST(LogicalGraphReadContractTest, DefaultCandidateStreamsEnforceByteBounds) {
		FallbackCandidateLogicalGraphSnapshot snapshot;
		snapshot.candidate_nodes = {
				LogicalGraphNode{1U, {"Person"}, {{"payload", PropertyValue(std::string(256U, 'x'))}}}};
		const auto accept = [](const IndexCandidateBatch &) { return true; };
		const LogicalGraphPropertyRange range{
				"payload", PropertyValue("a"), PropertyValue("z"), true, true};
		const LogicalGraphCompositePropertyRange composite{
				{{"group", PropertyValue("g")}}, {"payload", PropertyValue("a"), PropertyValue("z"), true, true}};

		EXPECT_THROW(static_cast<void>(snapshot.visitNodeCandidatesByExactProperties(
				{"Person"}, {{"payload", PropertyValue("x")}}, 1U, 8U, 1U, accept)), std::length_error);
		EXPECT_THROW(static_cast<void>(snapshot.visitNodeCandidatesByPropertyRange(
				{"Person"}, range, 1U, 8U, 1U, accept)), std::length_error);
		EXPECT_THROW(static_cast<void>(snapshot.visitNodeCandidatesByCompositePropertyRange(
				{"Person"}, composite, 1U, 8U, 1U, accept)), std::length_error);
	}

	TEST(LogicalGraphReadContractTest, DefaultCandidateStreamsValidateFallbackIdentityOrder) {
		FallbackCandidateLogicalGraphSnapshot snapshot;
		snapshot.candidate_nodes = {LogicalGraphNode{2U, {}}, LogicalGraphNode{1U, {}}};
		const auto accept = [](const IndexCandidateBatch &) { return true; };

		EXPECT_THROW(static_cast<void>(snapshot.visitNodeCandidatesByExactProperties(
				{"Person"}, {{"id", PropertyValue(int64_t{1})}}, 2U, 8U, 1U << 20U, accept)), std::runtime_error);
	}

	TEST(LogicalGraphReadContractTest, DefaultShapedBatchesFailClosedOnMismatchedIdentity) {
		ShapeAwareLogicalGraphSnapshot snapshot;
		snapshot.return_mismatched_identity = true;
		const std::vector<uint64_t> nodeIds{1U, 2U};
		const std::vector<uint64_t> edgeIds{3U, 4U};
		EXPECT_THROW(static_cast<void>(snapshot.readNodeRef(1U, EntityReadShape{})), std::runtime_error);
		EXPECT_THROW(static_cast<void>(snapshot.readEdgeRef(3U, EntityReadShape{})), std::runtime_error);
		EXPECT_THROW(static_cast<void>(snapshot.readNodesShapedSorted(nodeIds, EntityReadShape{})), std::runtime_error);
		EXPECT_THROW(static_cast<void>(snapshot.readEdgesShapedSorted(edgeIds, EntityReadShape{})), std::runtime_error);
	}

	TEST(LogicalGraphReadContractTest, MutationFenceRequiresBothOpaqueIdentityTokens) {
		LogicalGraphMutationFence fence;
		EXPECT_FALSE(fence.isValid());
		fence.backend_token = 1U;
		EXPECT_FALSE(fence.isValid());
		fence.format_token = 2U;
		EXPECT_TRUE(fence.isValid());
		EXPECT_EQ(fence, fence);

		const LogicalGraphNode node{7U, {"Person"}, {{"name", PropertyValue("Ada")}}};
		EXPECT_EQ(node, node);
		const LogicalGraphAdjacencyContinuation continuation{10U, 11U};
		EXPECT_EQ(continuation, continuation);
	}

	TEST(LogicalGraphReadContractTest, MutationConflictsExposeTheRetryReason) {
		const LogicalGraphMutationConflict conflict("snapshot fence changed");
		EXPECT_STREQ(conflict.what(), "snapshot fence changed");
		const LogicalGraphMutationEntityReference reference{
				LogicalGraphMutationEntityKind::LGMK_EDGE,
				LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE,
				42U};
		EXPECT_EQ(reference, reference);

		std::unique_ptr<LogicalGraphTransactionalBackend> backend =
				std::make_unique<MinimalLogicalGraphTransactionalBackend>();
		EXPECT_EQ(backend->beginLogicalGraphTransaction(false), nullptr);
	}

	TEST(LogicalGraphReadContractTest, ValueObjectsAndOptionalBackendsHaveStableDefaultContracts) {
		EXPECT_EQ(LogicalGraphNode{}, LogicalGraphNode{});
		EXPECT_EQ(LogicalGraphPropertyRange{}, LogicalGraphPropertyRange{});
		EXPECT_EQ(LogicalGraphCompositePropertyRange{}, LogicalGraphCompositePropertyRange{});
		EXPECT_EQ(LogicalGraphEdge{}, LogicalGraphEdge{});
		EXPECT_EQ(LogicalGraphNodePage{}, LogicalGraphNodePage{});
		EXPECT_EQ(LogicalGraphAdjacencyPage{}, LogicalGraphAdjacencyPage{});
		EXPECT_EQ(LogicalGraphCardinalityStatistics{}, LogicalGraphCardinalityStatistics{});

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = std::make_unique<MinimalLogicalGraphSnapshot>();
		EXPECT_EQ(snapshot->readNode(1U), LogicalGraphNode{});
		std::unique_ptr<LogicalGraphReadBackend> readBackend = std::make_unique<MinimalLogicalGraphReadBackend>();
		EXPECT_EQ(readBackend->openLogicalGraphReadSnapshot(), nullptr);
		std::unique_ptr<LogicalGraphStatisticsBackend> statisticsBackend =
				std::make_unique<MinimalLogicalGraphStatisticsBackend>();
		EXPECT_EQ(statisticsBackend->readLogicalGraphCardinalityStatistics(), std::nullopt);
	}

} // namespace graph::storage
