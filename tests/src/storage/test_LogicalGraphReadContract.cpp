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
			void visitEdges(size_t, const std::function<bool(const LogicalGraphEdge &)> &) const override {}
			void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
								const std::function<bool(const LogicalGraphEdge &)> &) const override {}
		};

		class MinimalLogicalGraphTransactionalBackend final : public LogicalGraphTransactionalBackend {
		public:
			[[nodiscard]] std::unique_ptr<LogicalGraphTransaction> beginLogicalGraphTransaction(bool) override {
				return nullptr;
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
		EXPECT_EQ(snapshot.mutationFence(), std::nullopt);
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

} // namespace graph::storage
