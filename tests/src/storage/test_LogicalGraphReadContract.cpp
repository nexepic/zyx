/**
 * @file test_LogicalGraphReadContract.cpp
 * @brief Default-contract tests for the format-independent graph read API.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <gtest/gtest.h>

#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"

namespace graph::storage {

	namespace {

		class MinimalLogicalGraphSnapshot final : public LogicalGraphReadSnapshot {
		public:
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
				static_cast<void>(visitor(LogicalGraphEdge{11U, 3U, 5U, "KNOWS"}));
			}
			void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
								const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
				static_cast<void>(visitor(LogicalGraphEdge{12U, 5U, 3U, "FOLLOWS"}));
			}
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
		EXPECT_THROW(static_cast<void>(snapshot.containsNode(7U)), std::logic_error);
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
		snapshot.visitAdjacentEdgeTopologies(5U, LogicalGraphReadDirection::LGRD_INCOMING, 1U,
				[&adjacentEdges](const LogicalGraphEdgeTopology &edge) {
					adjacentEdges.push_back(edge);
					return true;
				});
		ASSERT_EQ(adjacentEdges.size(), 1U);
		EXPECT_EQ(adjacentEdges.front(), (LogicalGraphEdgeTopology{12U, 5U, 3U, "FOLLOWS"}));

		EXPECT_THROW(snapshot.visitEdgeTopologies(1U, {}), std::invalid_argument);
		EXPECT_THROW(snapshot.visitAdjacentEdgeTopologies(5U, LogicalGraphReadDirection::LGRD_INCOMING, 1U, {}),
				 std::invalid_argument);
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
