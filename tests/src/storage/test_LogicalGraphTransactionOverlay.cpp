/**
 * @file test_LogicalGraphTransactionOverlay.cpp
 * @brief Unit tests for transaction-local logical graph read overlays.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Types.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"

namespace graph::storage {

	namespace {

	class InMemoryLogicalSnapshot final : public LogicalGraphReadSnapshot {
		public:
			std::map<uint64_t, LogicalGraphNode> nodes;
			std::map<uint64_t, LogicalGraphEdge> edges;
			LogicalGraphMutationFence fence{1U, 2U, {}};
			size_t required_node_metadata_pages = 1U;
			bool return_empty_nonterminal_node_page = false;
			bool return_zero_cost_nonterminal_node_page = false;
			bool return_oversized_node_metadata_cost = false;
			bool return_zero_cost_terminal_node_page = false;
			bool return_empty_nonterminal_adjacency_page = false;
			bool hide_edges_from_adjacency = false;
			bool ignore_edge_visit_bound = false;
			bool ignore_adjacency_visit_bound = false;
			bool return_no_mutation_fence = false;
			std::function<std::optional<std::vector<LogicalGraphNode>>(
					const std::vector<std::string> &, const std::vector<std::pair<std::string, PropertyValue>>, size_t,
					size_t)>
					exact_node_lookup;

			[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override { return nodes.at(nodeId); }
			[[nodiscard]] bool containsNode(uint64_t nodeId) const override { return nodes.contains(nodeId); }
			[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override { return edges.at(edgeId); }
			[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
			findNodesByExactProperties(const std::vector<std::string> &labels,
											 const std::vector<std::pair<std::string, PropertyValue>> &properties,
											 size_t maximumNodes, size_t maximumPropertyEntries) const override {
				if (!exact_node_lookup) {
					return std::nullopt;
				}
				return exact_node_lookup(labels, properties, maximumNodes, maximumPropertyEntries);
			}
			void visitNodes(size_t maximumRecords, const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
				size_t visited = 0U;
				for (const auto &[id, node]: nodes) {
					static_cast<void>(id);
					if (visited == maximumRecords) {
						throw std::runtime_error("test Node visit bound exceeded");
					}
					++visited;
					if (!visitor(node)) {
						return;
					}
				}
			}
			[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																				  size_t maximumMetadataPages) const override {
				if (maximumRecords == 0U || maximumMetadataPages == 0U) {
					throw std::invalid_argument("test Node scan requires positive bounds");
				}
				if (maximumMetadataPages < required_node_metadata_pages) {
					throw std::runtime_error("test Node scan metadata-page budget was too small");
				}
				LogicalGraphNodePage result;
				result.metadata_pages_visited = required_node_metadata_pages;
				if (return_zero_cost_terminal_node_page) {
					result.metadata_pages_visited = 0U;
					return result;
				}
				if (return_empty_nonterminal_node_page) {
					result.may_have_more = true;
					if (return_zero_cost_nonterminal_node_page) {
						result.metadata_pages_visited = 0U;
					}
					if (return_oversized_node_metadata_cost) {
						result.metadata_pages_visited = maximumMetadataPages + 1U;
					}
					return result;
				}
				for (const auto &[id, node]: nodes) {
					if (id <= afterNodeId) {
						continue;
					}
					if (result.nodes.size() == maximumRecords) {
						result.may_have_more = true;
						break;
					}
					result.nodes.push_back(node);
				}
				return result;
			}
			[[nodiscard]] LogicalGraphAdjacencyPage
			scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
							  std::optional<LogicalGraphAdjacencyContinuation> continuation,
							  size_t maximumRecords) const override {
				if (maximumRecords == 0U) {
					throw std::invalid_argument("test adjacency scan requires a positive bound");
				}
				if (return_empty_nonterminal_adjacency_page) {
					return {{}, LogicalGraphAdjacencyContinuation{1U, 2U}};
				}
				const uint64_t after = continuation.has_value() ? continuation->next_edge_id - 1U : 0U;
				LogicalGraphAdjacencyPage result;
				for (const auto &[id, edge]: edges) {
					const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
																 ? edge.source_node_id == nodeId
																 : edge.target_node_id == nodeId;
					if (hide_edges_from_adjacency || id <= after || !matches) {
						continue;
					}
					if (result.edges.size() == maximumRecords) {
						result.continuation = {result.edges.back().edge_id, id};
						break;
					}
					result.edges.push_back(edge);
				}
				return result;
			}
			void visitEdges(size_t maximumRecords, const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
				size_t visited = 0U;
				for (const auto &[id, edge]: edges) {
					static_cast<void>(id);
					if (!ignore_edge_visit_bound && visited == maximumRecords) {
						throw std::runtime_error("test Edge visit bound exceeded");
					}
					++visited;
					if (!visitor(edge)) {
						return;
					}
				}
			}
			void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
													const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
				if (ignore_adjacency_visit_bound) {
					for (const auto &[id, edge]: edges) {
						static_cast<void>(id);
						const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
																 ? edge.source_node_id == nodeId
																 : edge.target_node_id == nodeId;
						if (matches && !visitor(edge)) {
							return;
						}
					}
					return;
				}
				std::optional<LogicalGraphAdjacencyContinuation> continuation;
				size_t visited = 0U;
				while (true) {
					const LogicalGraphAdjacencyPage page = scanAdjacentEdges(nodeId, direction, continuation,
																				 maximumEdges - visited);
					for (const LogicalGraphEdge &edge: page.edges) {
						if (!visitor(edge)) {
							return;
						}
						++visited;
					}
					if (!page.continuation.has_value()) {
						return;
					}
					if (visited == maximumEdges) {
						throw std::runtime_error("test adjacency visit bound exceeded");
					}
					continuation = page.continuation;
				}
			}
		[[nodiscard]] std::optional<LogicalGraphMutationFence> mutationFence() const override {
			return return_no_mutation_fence ? std::nullopt : std::optional<LogicalGraphMutationFence>(fence);
		}
	};

	class InMemoryVectorSnapshot final : public LogicalGraphVectorSearchSnapshot {
	public:
		explicit InMemoryVectorSnapshot(const InMemoryLogicalSnapshot &snapshot) : snapshot_(snapshot) {}

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override { return snapshot_.readNode(nodeId); }
		[[nodiscard]] bool containsNode(uint64_t nodeId) const override { return snapshot_.containsNode(nodeId); }
		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override { return snapshot_.readEdge(edgeId); }
		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>>
		findNodesByExactProperties(const std::vector<std::string> &labels,
									 const std::vector<std::pair<std::string, PropertyValue>> &properties,
									 size_t maximumNodes, size_t maximumPropertyEntries) const override {
			return snapshot_.findNodesByExactProperties(labels, properties, maximumNodes, maximumPropertyEntries);
		}
		void visitNodes(size_t maximumRecords, const std::function<bool(const LogicalGraphNode &)> &visitor) const override {
			snapshot_.visitNodes(maximumRecords, visitor);
		}
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																		 size_t maximumMetadataPages) const override {
			return snapshot_.scanNodesAfter(afterNodeId, maximumRecords, maximumMetadataPages);
		}
		[[nodiscard]] LogicalGraphAdjacencyPage
		scanAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction,
						  std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const override {
			return snapshot_.scanAdjacentEdges(nodeId, direction, continuation, maximumRecords);
		}
		void visitEdges(size_t maximumRecords, const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			snapshot_.visitEdges(maximumRecords, visitor);
		}
		void visitAdjacentEdges(uint64_t nodeId, LogicalGraphReadDirection direction, size_t maximumEdges,
									const std::function<bool(const LogicalGraphEdge &)> &visitor) const override {
			snapshot_.visitAdjacentEdges(nodeId, direction, maximumEdges, visitor);
		}
		[[nodiscard]] std::optional<LogicalGraphMutationFence> mutationFence() const override {
			return snapshot_.mutationFence();
		}
		[[nodiscard]] LogicalGraphVectorSearchResult
		searchLogicalVector(const LogicalGraphVectorSearchRequest &) const override {
			throw std::logic_error("test derived Vector snapshot must use the exact overlay fallback");
		}
		[[nodiscard]] std::optional<LogicalGraphVectorIndexDescriptor>
		describeLogicalVectorIndex(const LogicalGraphVectorSearchRequest &) const override {
			return LogicalGraphVectorIndexDescriptor{1U, "Embedding", "value", 2U, "l2"};
		}
		[[nodiscard]] std::optional<LogicalGraphVectorSearchHit>
		scoreLogicalVectorExactOverlayCandidate(const LogicalGraphVectorSearchRequest &,
															 const LogicalGraphVectorIndexDescriptor &,
															 const LogicalGraphNode &node) const override {
			++score_calls;
			if (!std::ranges::binary_search(node.labels, std::string("Embedding"))) {
				return std::nullopt;
			}
			return LogicalGraphVectorSearchHit{node.node_id, 0.0F};
		}

		mutable size_t score_calls = 0U;

	private:
		const InMemoryLogicalSnapshot &snapshot_;
	};

	class ThrowAfterCancellationCheckpoint final : public CancellationCheckpoint {
	public:
		explicit ThrowAfterCancellationCheckpoint(size_t throwAfter) : throw_after_(throwAfter) {}

		void check() const override {
			++checks;
			if (checks == throw_after_) {
				throw std::runtime_error("test transaction-overlay cancellation checkpoint");
			}
		}

		mutable size_t checks = 0U;

	private:
		size_t throw_after_ = 0U;
	};

	class InMemoryLogicalTransaction final : public LogicalGraphTransaction,
												 public LogicalGraphTransactionVectorSnapshotProvider {
	public:
		explicit InMemoryLogicalTransaction(bool readOnly = false, size_t requiredNodeMetadataPages = 1U) :
			vector_snapshot_(snapshot_), read_only_(readOnly) {
			snapshot_.required_node_metadata_pages = requiredNodeMetadataPages;
			snapshot_.nodes.emplace(1U, LogicalGraphNode{1U, {"Base"}, {{"name", PropertyValue("Ada")}}});
			snapshot_.nodes.emplace(2U, LogicalGraphNode{2U, {"Base"}});
			snapshot_.edges.emplace(10U, LogicalGraphEdge{10U, 1U, 2U, "BASE_LINK"});
		}

		[[nodiscard]] LogicalGraphTransactionState state() const override { return state_; }
		[[nodiscard]] bool isReadOnly() const override { return read_only_; }
		void setIncompletePreview(bool enabled) { incomplete_preview_ = enabled; }
		void setMismatchedStageResult(bool enabled) { mismatched_stage_result_ = enabled; }
		void setDuplicateNodeIds(bool enabled) { duplicate_node_ids_ = enabled; }
		void setDuplicateEdgeIds(bool enabled) { duplicate_edge_ids_ = enabled; }
		[[nodiscard]] LogicalGraphReadSnapshot &readSnapshot() override {
			requireActive();
			return snapshot_;
		}
		[[nodiscard]] LogicalGraphVectorSearchSnapshot &pinnedLogicalGraphVectorSearchSnapshot() override {
			requireActive();
			return vector_snapshot_;
		}
		[[nodiscard]] size_t vectorScoreCalls() const { return vector_snapshot_.score_calls; }
		[[nodiscard]] LogicalGraphMutationBatchResult
		previewMutationBatch(const LogicalGraphMutationBatch &batch) const override {
			requireActive();
			if (read_only_) {
				throw std::logic_error("read-only test transaction cannot preview writes");
			}
			LogicalGraphMutationBatchResult result;
			for (size_t index = 0U; index < batch.node_creates.size(); ++index) {
				result.created_node_ids.push_back(100U + staged_node_count_ + index);
			}
			for (size_t index = 0U; index < batch.edge_creates.size(); ++index) {
				result.created_edge_ids.push_back(200U + staged_edge_count_ + index);
			}
			if (incomplete_preview_) {
				if (!result.created_node_ids.empty()) {
					result.created_node_ids.pop_back();
				} else if (!result.created_edge_ids.empty()) {
					result.created_edge_ids.pop_back();
				}
			}
			if (duplicate_node_ids_ && result.created_node_ids.size() > 1U) {
				result.created_node_ids[1] = result.created_node_ids.front();
			}
			if (duplicate_edge_ids_ && result.created_edge_ids.size() > 1U) {
				result.created_edge_ids[1] = result.created_edge_ids.front();
			}
			return result;
		}
		[[nodiscard]] LogicalGraphMutationBatchResult stageMutationBatch(LogicalGraphMutationBatch batch) override {
			LogicalGraphMutationBatchResult result = previewMutationBatch(batch);
			staged_node_count_ += batch.node_creates.size();
			staged_edge_count_ += batch.edge_creates.size();
			if (mismatched_stage_result_) {
				if (!result.created_node_ids.empty()) {
					++result.created_node_ids.front();
				} else if (!result.created_edge_ids.empty()) {
					++result.created_edge_ids.front();
				}
			}
			return result;
		}
		[[nodiscard]] LogicalGraphMutationBatchResult commit() override {
			requireActive();
			state_ = LogicalGraphTransactionState::LGTTS_COMMITTED;
			return {};
		}
		void rollback() override {
			requireActive();
			state_ = LogicalGraphTransactionState::LGTTS_ROLLED_BACK;
		}

	private:
		void requireActive() const {
			if (state_ != LogicalGraphTransactionState::LGTTS_ACTIVE) {
				throw std::logic_error("test transaction is not active");
			}
		}

		InMemoryLogicalSnapshot snapshot_;
		InMemoryVectorSnapshot vector_snapshot_;
		LogicalGraphTransactionState state_ = LogicalGraphTransactionState::LGTTS_ACTIVE;
		bool read_only_ = false;
		size_t staged_node_count_ = 0U;
		size_t staged_edge_count_ = 0U;
		bool incomplete_preview_ = false;
		bool mismatched_stage_result_ = false;
		bool duplicate_node_ids_ = false;
		bool duplicate_edge_ids_ = false;
	};

	TEST(LogicalGraphTransactionOverlayTest, OverlaysPropertiesCreatedNodesAndResumableEdges) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		overlay.replaceLogicalNodeProperties({{1U, {{"name", PropertyValue("Grace")},
																				  {"rank", PropertyValue(int64_t(7))}}}});
		const std::vector<uint64_t> created = overlay.createLogicalNodes({{{"Added"}, {}}});
		ASSERT_EQ(created, (std::vector<uint64_t>{100U}));
		const std::vector<uint64_t> edges = overlay.createLogicalEdges(
				{{1U, created[0], "ADDED", {}}, {1U, created[0], "ADDED", {}}, {1U, created[0], "ADDED", {}}});
		ASSERT_EQ(edges, (std::vector<uint64_t>{200U, 201U, 202U}));

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNode updated = snapshot->readNode(1U);
		EXPECT_EQ(updated.properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Grace")},
																		 {"rank", PropertyValue(int64_t(7))}}));
		const LogicalGraphNodePage firstNodes = snapshot->scanNodesAfter(0U, 2U, 1U);
		ASSERT_EQ(firstNodes.nodes.size(), 2U);
		ASSERT_TRUE(firstNodes.may_have_more);
		const LogicalGraphNodePage secondNodes = snapshot->scanNodesAfter(2U, 2U, 1U);
		ASSERT_EQ(secondNodes.nodes, (std::vector<LogicalGraphNode>{{100U, {"Added"}}}));
		const LogicalGraphAdjacencyPage firstEdges = snapshot->scanAdjacentEdges(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 2U);
		ASSERT_EQ(firstEdges.edges.size(), 2U);
		ASSERT_TRUE(firstEdges.continuation.has_value());
		const LogicalGraphAdjacencyPage secondEdges = snapshot->scanAdjacentEdges(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, firstEdges.continuation, 2U);
		ASSERT_EQ(secondEdges.edges.size(), 2U);
		EXPECT_FALSE(secondEdges.continuation.has_value());

		overlay.deleteLogicalEdges({{200U}});
		snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(200U)), std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, CreationIdentityRangesAreBoundedAndNeverWrap) {
		const LogicalGraphCreationIdentityRange range{10U, 3U};
		EXPECT_FALSE(range.contains(9U));
		EXPECT_TRUE(range.contains(10U));
		EXPECT_TRUE(range.contains(11U));
		EXPECT_TRUE(range.contains(12U));
		EXPECT_FALSE(range.contains(13U));

		const LogicalGraphCreationIdentityRange zeroFirstId{0U, 3U};
		const LogicalGraphCreationIdentityRange emptyRange{10U, 0U};
		EXPECT_FALSE(zeroFirstId.contains(1U));
		EXPECT_FALSE(emptyRange.contains(10U));
		const LogicalGraphCreationIdentityRange nearLimit{std::numeric_limits<uint64_t>::max() - 1U, 2U};
		EXPECT_TRUE(nearLimit.contains(std::numeric_limits<uint64_t>::max() - 1U));
		EXPECT_TRUE(nearLimit.contains(std::numeric_limits<uint64_t>::max()));
	}

	TEST(LogicalGraphTransactionOverlayTest, CancelledVectorOverlayScanPreservesStagedFinalStateForRetry) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		const std::vector<uint64_t> created = overlay.createLogicalNodes({{{"Embedding"}, {{"value", PropertyValue(1.0)}}}});
		ASSERT_EQ(created, (std::vector<uint64_t>{100U}));

		auto snapshot = overlay.openLogicalGraphVectorSearchSnapshot();
		ASSERT_NE(snapshot, nullptr);
		LogicalGraphVectorSearchRequest request;
		request.index_name = "embedding";
		request.query_vector = {1.0F, 0.0F};
		request.top_k = 1U;
		request.maximum_mapping_entries = 4U;
		request.maximum_raw_vector_reads = 4U;
		ThrowAfterCancellationCheckpoint cancellation(2U);
		request.cancellation = &cancellation;

		EXPECT_THROW(static_cast<void>(snapshot->searchLogicalVector(request)), std::runtime_error);
		EXPECT_EQ(cancellation.checks, 2U);
		EXPECT_EQ(transaction.vectorScoreCalls(), 0U);
		EXPECT_EQ(overlay.openLogicalGraphReadSnapshot()->readNode(created.front()).node_id, created.front());

		request.cancellation = nullptr;
		const LogicalGraphVectorSearchResult retry = snapshot->searchLogicalVector(request);
		ASSERT_EQ(retry.hits.size(), 1U);
		EXPECT_EQ(retry.hits.front().node_id, created.front());
		EXPECT_TRUE(retry.stats.used_exact_overlay);
		EXPECT_GT(retry.stats.exact_overlay_candidate_scans, 0U);
	}

	TEST(LogicalGraphTransactionOverlayTest, VectorSnapshotDelegatesTheReadSurfaceAcrossOverlayRevisions) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		std::unique_ptr<LogicalGraphVectorSearchSnapshot> snapshot = overlay.openLogicalGraphVectorSearchSnapshot();

		EXPECT_EQ(snapshot->readNode(1U).node_id, 1U);
		EXPECT_TRUE(snapshot->containsNode(1U));
		EXPECT_EQ(snapshot->readEdge(10U).edge_id, 10U);
		EXPECT_FALSE(snapshot->findNodesByExactProperties({"Base"}, {}, 2U, 2U).has_value());
		EXPECT_EQ(snapshot->scanNodesAfter(0U, 2U, 1U).nodes.size(), 2U);
		EXPECT_EQ(snapshot->scanAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)
				  .edges.size(),
			  1U);
		size_t visitedNodes = 0U;
		snapshot->visitNodes(2U, [&visitedNodes](const LogicalGraphNode &) {
			++visitedNodes;
			return true;
		});
		EXPECT_EQ(visitedNodes, 2U);
		size_t visitedEdges = 0U;
		snapshot->visitEdges(1U, [&visitedEdges](const LogicalGraphEdge &) {
			++visitedEdges;
			return true;
		});
		EXPECT_EQ(visitedEdges, 1U);
		EXPECT_TRUE(snapshot->mutationFence().has_value());
		EXPECT_EQ(snapshot->mutationFence()->logical_revision, 0U);

		const uint64_t created = overlay.createLogicalNodes({{{"Added"}, {}}}).front();
		EXPECT_EQ(snapshot->readNode(created).node_id, created);
		EXPECT_TRUE(snapshot->containsNode(created));
		EXPECT_EQ(snapshot->scanNodesAfter(2U, 2U, 1U).nodes,
				  (std::vector<LogicalGraphNode>{{created, {"Added"}}}));
		EXPECT_EQ(snapshot->mutationFence()->logical_revision, 1U);

		static_cast<void>(transaction.commit());
		EXPECT_THROW(static_cast<void>(snapshot->containsNode(1U)), std::logic_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, SharesTheBaseMetadataPageBudgetAcrossItsNodeOverlay) {
		// A relocatable base scan needs at least a directory root and leaf. The overlay
		// must forward that budget instead of constraining every probe to one page.
		InMemoryLogicalTransaction transaction(false, 2U);
		LogicalGraphTransactionOverlay overlay(transaction);
		ASSERT_EQ(overlay.createLogicalNodes({{{"Added"}, {}}}), (std::vector<uint64_t>{100U}));

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNodePage page = snapshot->scanNodesAfter(0U, 3U, 2U);
		EXPECT_EQ(page.nodes,
				  (std::vector<LogicalGraphNode>{{1U, {"Base"}, {{"name", PropertyValue("Ada")}}},
																						 {2U, {"Base"}}, {100U, {"Added"}}}));
		EXPECT_FALSE(page.may_have_more);
		EXPECT_EQ(page.metadata_pages_visited, 2U);
	}

	TEST(LogicalGraphTransactionOverlayTest, MergesCompleteExactLookupWithTheTransactionFinalState) {
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.edges.clear();
		base.exact_node_lookup = [&base](const std::vector<std::string> &labels,
											 const std::vector<std::pair<std::string, PropertyValue>> &properties,
											 size_t maximumNodes, size_t maximumPropertyEntries)
				-> std::optional<std::vector<LogicalGraphNode>> {
			if (maximumNodes == 0U || maximumPropertyEntries == 0U) {
				throw std::invalid_argument("test exact lookup requires positive bounds");
			}
			if (labels != std::vector<std::string>{"Base"} || properties.size() != 1U ||
				properties.front().first != "name") {
				return std::nullopt;
			}
			if (properties.front().second == PropertyValue("Ada")) {
				return std::vector<LogicalGraphNode>{base.nodes.at(1U)};
			}
			return std::vector<LogicalGraphNode>{};
		};
	LogicalGraphTransactionOverlay overlay(transaction);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->findNodesByExactProperties(
				 {"Base"}, {{"name", PropertyValue("Ada")}}, 0U, 1U)),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(snapshot->findNodesByExactProperties(
				 {"Base"}, {{"name", PropertyValue("Ada")}}, 1U, 0U)),
			 std::invalid_argument);
	const auto findByName = [&snapshot](std::string_view name) {
		return snapshot->findNodesByExactProperties({"Base"}, {{"name", PropertyValue(std::string(name))}}, 2U, 1U);
	};

		ASSERT_TRUE(findByName("Ada").has_value());
		EXPECT_EQ(findByName("Ada")->front().node_id, 1U);

		// A base support-tree match must be filtered through the final transaction
		// state before it reaches the runtime.
		overlay.replaceLogicalNodeProperties({{1U, {{"name", PropertyValue("Grace")}}}});
		EXPECT_EQ(*findByName("Ada"), (std::vector<LogicalGraphNode>{}));
		ASSERT_TRUE(findByName("Grace").has_value());
		EXPECT_EQ(findByName("Grace")->front().node_id, 1U);

		overlay.deleteLogicalNodes({{1U, false, 0U}});
		EXPECT_EQ(*findByName("Grace"), (std::vector<LogicalGraphNode>{}));
		const uint64_t created = overlay.createLogicalNodes({{{"Base"}, {{"name", PropertyValue("Ada")}}}}).front();
		ASSERT_TRUE(findByName("Ada").has_value());
		EXPECT_EQ(findByName("Ada")->front().node_id, created);
	}

	TEST(LogicalGraphTransactionOverlayTest, PreservesTheOptionalExactLookupFallback) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();

		// A backend without a complete index lookup must leave the runtime on its
		// bounded scan path rather than returning an incomplete overlay result.
		EXPECT_EQ(snapshot->findNodesByExactProperties(
					  {"Base"}, {{"name", PropertyValue("Ada")}}, 1U, 1U),
				  std::nullopt);
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsMismatchedFenceAndReadOnlyStaging) {
		InMemoryLogicalTransaction writable;
		LogicalGraphTransactionOverlay overlay(writable);
		LogicalGraphMutationFence wrongFence{9U, 9U, {}};
		LogicalGraphMutationBatch batch;
		batch.node_creates = {{{"Blocked"}, {}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(batch, wrongFence)),
				 LogicalGraphMutationConflict);

		InMemoryLogicalTransaction readOnly(true);
		LogicalGraphTransactionOverlay readOnlyOverlay(readOnly);
		EXPECT_THROW(static_cast<void>(readOnlyOverlay.executeLogicalGraphMutationBatch(batch)), std::logic_error);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = readOnlyOverlay.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->readNode(1U).node_id, 1U);
	}

	TEST(LogicalGraphTransactionOverlayTest, DetachDeleteDoesNotChargeAlreadyDeletedBaseOrCreatedEdges) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		ASSERT_EQ(overlay.createLogicalEdges({{1U, 2U, "STAGED", {}}}), (std::vector<uint64_t>{200U}));
		overlay.deleteLogicalEdges({{10U}, {200U}});

		// The explicit deletions are already final-state invisible, so a detach
		// delete with one permitted Edge must not count them a second time.
		EXPECT_NO_THROW(overlay.deleteLogicalNodes({{1U, true, 1U}}));
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readNode(1U)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(10U)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(200U)), std::runtime_error);
		EXPECT_EQ(snapshot->readNode(2U).node_id, 2U);
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsNonDetachDeleteWithAVisibleIncidentEdgeAtStaging) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		EXPECT_THROW(overlay.deleteLogicalNodes({{1U, false, 0U}}), std::runtime_error);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->readNode(1U).node_id, 1U);
		EXPECT_EQ(snapshot->readEdge(10U).edge_id, 10U);
	}

	TEST(LogicalGraphTransactionOverlayTest, AppliesFinalStateBatchForCreatedEntitiesAndProperties) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		const LogicalGraphMutationEntityReference firstCreatedNode{
				LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
		const LogicalGraphMutationEntityReference secondCreatedNode{
				LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U};
		LogicalGraphMutationBatch batch;
		batch.node_creates = {{{"Original"}, {{"name", PropertyValue("Ada")}}}, {{"Target"}, {}}};
		batch.node_label_replacements = {{firstCreatedNode, {"Final", "Extra"}}};
		batch.edge_creates = {{firstCreatedNode, secondCreatedNode, "REL", {{"rank", PropertyValue(int64_t(1))}}},
							  {firstCreatedNode, secondCreatedNode, "REL", {}},
							  {firstCreatedNode, secondCreatedNode, "REL", {}}};
		const LogicalGraphMutationBatchResult result = overlay.executeLogicalGraphMutationBatch(batch);
		ASSERT_EQ(result.created_node_ids, (std::vector<uint64_t>{100U, 101U}));
		ASSERT_EQ(result.created_edge_ids, (std::vector<uint64_t>{200U, 201U, 202U}));

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNode created = snapshot->readNode(100U);
		EXPECT_EQ(created.labels, (std::vector<std::string>{"Extra", "Final"}));
		EXPECT_EQ(created.properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Ada")}}));
		const LogicalGraphEdge edge = snapshot->readEdge(200U);
		EXPECT_EQ(edge.properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"rank", PropertyValue(int64_t(1))}}));

		const LogicalGraphAdjacencyPage first = snapshot->scanAdjacentEdges(
				100U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 2U);
		ASSERT_EQ(first.edges.size(), 2U);
		ASSERT_TRUE(first.continuation.has_value());
		const LogicalGraphAdjacencyPage second = snapshot->scanAdjacentEdges(
				100U, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 2U);
		ASSERT_EQ(second.edges.size(), 1U);
		EXPECT_FALSE(second.continuation.has_value());

		std::vector<uint64_t> nodeIds;
		snapshot->visitNodes(4U, [&nodeIds](const LogicalGraphNode &node) {
			nodeIds.push_back(node.node_id);
			return true;
		});
		EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U, 2U, 100U, 101U}));
		std::vector<uint64_t> edgeIds;
		snapshot->visitEdges(4U, [&edgeIds](const LogicalGraphEdge &value) {
			edgeIds.push_back(value.edge_id);
			return true;
		});
		EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U, 200U, 201U, 202U}));

		overlay.replaceLogicalNodeProperties({{100U, {}}});
		overlay.replaceLogicalEdgeProperties({{200U, {}}});
		snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_TRUE(snapshot->readNode(100U).properties.empty());
		EXPECT_TRUE(snapshot->readEdge(200U).properties.empty());
	}

	TEST(LogicalGraphTransactionOverlayTest, ReplacesPersistedStateAndRejectsMalformedReadPages) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		const LogicalGraphMutationEntityReference persistedNode{
				LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U};
		const LogicalGraphMutationEntityReference persistedEdge{
				LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 10U};
		LogicalGraphMutationBatch batch;
		batch.node_label_replacements = {{persistedNode, {"Updated", "Base"}}};
		batch.property_replacements = {{persistedNode, {{"name", PropertyValue("Grace")},
																 {"rank", PropertyValue(int64_t(7))}}},
										 {persistedEdge, {{"weight", PropertyValue(int64_t(3))}}}};
		EXPECT_TRUE(overlay.executeLogicalGraphMutationBatch(batch).created_node_ids.empty());

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNode node = snapshot->readNode(1U);
		EXPECT_EQ(node.labels, (std::vector<std::string>{"Base", "Updated"}));
		EXPECT_EQ(node.properties.size(), 2U);
		const LogicalGraphEdge edge = snapshot->readEdge(10U);
		ASSERT_EQ(edge.properties.size(), 1U);
		EXPECT_EQ(edge.properties.front().key, "weight");
		EXPECT_THROW(snapshot->visitNodes(0U, [](const LogicalGraphNode &) { return true; }), std::invalid_argument);
		EXPECT_THROW(snapshot->visitEdges(0U, [](const LogicalGraphEdge &) { return true; }), std::invalid_argument);
		EXPECT_THROW(snapshot->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, 0U,
															 [](const LogicalGraphEdge &) { return true; }),
				 std::invalid_argument);

		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.return_empty_nonterminal_node_page = true;
		EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 1U)), std::runtime_error);
		base.return_zero_cost_nonterminal_node_page = true;
		EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 1U)), std::runtime_error);
		base.return_zero_cost_nonterminal_node_page = false;
		base.return_oversized_node_metadata_cost = true;
		EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 1U)), std::runtime_error);
		base.return_empty_nonterminal_node_page = false;
		base.return_oversized_node_metadata_cost = false;
		base.return_empty_nonterminal_adjacency_page = true;
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
								 1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)),
				 std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsInvalidMutationsAndInvalidBaseFenceWithoutLeakingState) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		LogicalGraphMutationBatch duplicateProperties;
		duplicateProperties.node_creates = {{{"Created"}, {{"key", PropertyValue(int64_t(1))},
																					 {"key", PropertyValue(int64_t(2))}}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(duplicateProperties)),
				 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(overlay.createLogicalNodes({{{""}, {}}})), std::invalid_argument);
		LogicalGraphMutationBatch untypedEdge;
		untypedEdge.edge_creates = {{
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				"", {}}};
		EXPECT_EQ(overlay.executeLogicalGraphMutationBatch(untypedEdge).created_edge_ids, (std::vector<uint64_t>{200U}));
		std::unique_ptr<LogicalGraphReadSnapshot> untypedSnapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_TRUE(untypedSnapshot->readEdge(200U).type.empty());

		LogicalGraphMutationBatch invalidReference;
		invalidReference.edge_creates = {{
				{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				"REL", {}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(invalidReference)),
				 std::invalid_argument);
		EXPECT_THROW(overlay.deleteLogicalEdges({{0U}}), std::invalid_argument);

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readNode(100U)), std::out_of_range);
		EXPECT_EQ(snapshot->readNode(1U).labels, (std::vector<std::string>{"Base"}));
		static_cast<void>(transaction.commit());
		EXPECT_THROW(static_cast<void>(overlay.openLogicalGraphReadSnapshot()), std::logic_error);
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch({})), std::logic_error);

		InMemoryLogicalTransaction invalidFenceTransaction;
		static_cast<InMemoryLogicalSnapshot &>(invalidFenceTransaction.readSnapshot()).fence = {};
		EXPECT_THROW(static_cast<void>(LogicalGraphTransactionOverlay(invalidFenceTransaction)), std::logic_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, RollsBackOverlayStateWhenTransactionIdentityResultsAreInconsistent) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		transaction.setIncompletePreview(true);
		EXPECT_THROW(static_cast<void>(overlay.createLogicalNodes({{{"Incomplete"}, {}}})), std::logic_error);
		transaction.setIncompletePreview(false);
		transaction.setMismatchedStageResult(true);
		EXPECT_THROW(static_cast<void>(overlay.createLogicalNodes({{{"Mismatch"}, {}}})), std::logic_error);
		transaction.setMismatchedStageResult(false);

		InMemoryLogicalTransaction duplicateNodesTransaction;
		LogicalGraphTransactionOverlay duplicateNodes(duplicateNodesTransaction);
		duplicateNodesTransaction.setDuplicateNodeIds(true);
		EXPECT_THROW(static_cast<void>(duplicateNodes.createLogicalNodes({{{"One"}, {}}, {{"Two"}, {}}})),
				 std::logic_error);
		InMemoryLogicalTransaction duplicateEdgesTransaction;
		LogicalGraphTransactionOverlay duplicateEdges(duplicateEdgesTransaction);
		duplicateEdgesTransaction.setDuplicateEdgeIds(true);
		EXPECT_THROW(static_cast<void>(duplicateEdges.createLogicalEdges(
								  {{1U, 2U, "REL", {}}, {1U, 2U, "REL", {}}})),
				 std::logic_error);

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readNode(100U)), std::out_of_range);

		// A failed stage after a direct-property replacement must restore both the
		// entity image and its old fixed-record bridge visibility.
		InMemoryLogicalTransaction propertyTransaction;
		LogicalGraphTransactionOverlay propertyOverlay(propertyTransaction);
		propertyTransaction.setMismatchedStageResult(true);
		LogicalGraphMutationBatch mismatchedPropertyBatch;
		mismatchedPropertyBatch.node_creates = {{{"Mismatch"}, {}}};
		mismatchedPropertyBatch.property_replacements = {{
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				{{"name", PropertyValue("Grace")}}}};
		EXPECT_THROW(static_cast<void>(propertyOverlay.executeLogicalGraphMutationBatch(mismatchedPropertyBatch)),
				 std::logic_error);
		propertyTransaction.setMismatchedStageResult(false);
		std::unique_ptr<LogicalGraphReadSnapshot> propertySnapshot = propertyOverlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNode unchanged = propertySnapshot->readNode(1U);
		EXPECT_EQ(unchanged.properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Ada")}}));
	}

	TEST(LogicalGraphTransactionOverlayTest, PreservesBaseContinuationsAndEnforcesVisitBounds) {
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "SECOND"});
		LogicalGraphTransactionOverlay overlay(transaction);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();

		const LogicalGraphAdjacencyPage first = snapshot->scanAdjacentEdges(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
		ASSERT_EQ(first.edges, (std::vector<LogicalGraphEdge>{base.edges.at(10U)}));
		ASSERT_TRUE(first.continuation.has_value());
		const LogicalGraphAdjacencyPage second = snapshot->scanAdjacentEdges(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1U);
		EXPECT_EQ(second.edges, (std::vector<LogicalGraphEdge>{base.edges.at(11U)}));
		EXPECT_FALSE(second.continuation.has_value());

		std::vector<uint64_t> visited;
		snapshot->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
													[&visited](const LogicalGraphEdge &edge) {
														visited.push_back(edge.edge_id);
														return false;
													});
		EXPECT_EQ(visited, (std::vector<uint64_t>{10U}));
		EXPECT_THROW(snapshot->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, 1U,
															 [](const LogicalGraphEdge &) { return true; }),
				 std::runtime_error);

		overlay.deleteLogicalEdges({{10U}});
		snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(10U)), std::runtime_error);
		std::vector<uint64_t> visibleEdges;
		snapshot->visitEdges(2U, [&visibleEdges](const LogicalGraphEdge &edge) {
			visibleEdges.push_back(edge.edge_id);
			return true;
		});
		EXPECT_EQ(visibleEdges, (std::vector<uint64_t>{11U}));
	}

	TEST(LogicalGraphTransactionOverlayTest, AppliesDetachDeletionToBaseAndCreatedIncidentEdges) {
		InMemoryLogicalTransaction baseTransaction;
		LogicalGraphTransactionOverlay baseOverlay(baseTransaction);
		EXPECT_NO_THROW(baseOverlay.deleteLogicalNodes({{1U, true, 1U}}));
		std::unique_ptr<LogicalGraphReadSnapshot> baseSnapshot = baseOverlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(baseSnapshot->readNode(1U)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(baseSnapshot->readEdge(10U)), std::runtime_error);

		InMemoryLogicalTransaction createdTransaction;
		LogicalGraphTransactionOverlay createdOverlay(createdTransaction);
		const uint64_t created = createdOverlay.createLogicalNodes({{{"Created"}, {}}}).front();
		const uint64_t edge = createdOverlay.createLogicalEdges({{1U, created, "REL", {}}}).front();
		EXPECT_THROW(createdOverlay.deleteLogicalNodes({{created, false, 0U}}), std::runtime_error);
		EXPECT_NO_THROW(createdOverlay.deleteLogicalNodes({{created, true, 1U}}));
		std::unique_ptr<LogicalGraphReadSnapshot> createdSnapshot = createdOverlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(createdSnapshot->readNode(created)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(createdSnapshot->readEdge(edge)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(createdOverlay.createLogicalEdges({{created, 1U, "REL", {}}})),
				 std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, CoversResumableNodeAndAdjacencyBoundaryFailures) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		ASSERT_EQ(overlay.createLogicalNodes({{{"Created"}, {}}, {{"Created"}, {}}}).size(), 2U);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 0U, 1U)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 0U)), std::invalid_argument);
		EXPECT_THROW(snapshot->visitNodes(2U, [](const LogicalGraphNode &) { return true; }), std::runtime_error);
		size_t visited = 0U;
		snapshot->visitNodes(4U, [&visited](const LogicalGraphNode &) {
			++visited;
			return false;
		});
		EXPECT_EQ(visited, 1U);

		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
								 100U, LogicalGraphReadDirection::LGRD_OUTGOING,
								 LogicalGraphAdjacencyContinuation{1U, 2U}, 1U)),
				 std::runtime_error);
		EXPECT_TRUE(overlay.executeLogicalGraphMutationBatch({}).created_node_ids.empty());
	}

	TEST(LogicalGraphTransactionOverlayTest, FiltersDeletedBasePagesAndRetainsCreatedContinuationValidation) {
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "SECOND"});
		LogicalGraphTransactionOverlay overlay(transaction);
		overlay.deleteLogicalEdges({{10U}});
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const LogicalGraphAdjacencyPage visible = snapshot->scanAdjacentEdges(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
		EXPECT_EQ(visible.edges, (std::vector<LogicalGraphEdge>{base.edges.at(11U)}));

		const uint64_t node = overlay.createLogicalNodes({{{"Created"}, {}}}).front();
		ASSERT_EQ(overlay.createLogicalEdges({{node, node, "REL", {}}, {node, node, "REL", {}}}).size(), 2U);
		const LogicalGraphAdjacencyPage first = snapshot->scanAdjacentEdges(
				node, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
		ASSERT_TRUE(first.continuation.has_value());
		overlay.deleteLogicalEdges({{200U}, {201U}});
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
								 node, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1U)),
				 std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsInvalidReferenceKindsAndDeletedEntityMutations) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		const LogicalGraphMutationEntityReference persistedNode{
				LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U};
		const LogicalGraphMutationEntityReference persistedEdge{
				LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 10U};

		LogicalGraphMutationBatch wrongLabelKind;
		wrongLabelKind.node_label_replacements = {{persistedEdge, {"Wrong"}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(wrongLabelKind)), std::invalid_argument);
		LogicalGraphMutationBatch unknownPropertyKind;
		unknownPropertyKind.property_replacements = {{{static_cast<LogicalGraphMutationEntityKind>(99U),
																						  LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U}, {}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(unknownPropertyKind)),
				 std::invalid_argument);
		LogicalGraphMutationBatch invalidCreatedNode;
		invalidCreatedNode.node_creates = {{{"Created"}, {}}};
		invalidCreatedNode.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_NODE,
																						 LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U}, {}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(invalidCreatedNode)),
				 std::invalid_argument);
		LogicalGraphMutationBatch invalidReferenceKind;
		invalidReferenceKind.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_NODE,
																						  static_cast<LogicalGraphMutationReferenceKind>(99U), 1U}, {}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(invalidReferenceKind)),
				 std::invalid_argument);
		LogicalGraphMutationBatch createdEdgeProperty;
		createdEdgeProperty.edge_creates = {{persistedNode,
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				"REL", {}}};
		createdEdgeProperty.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_EDGE,
																						  LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U},
																						 {{"rank", PropertyValue(int64_t(2))}}}};
		EXPECT_EQ(overlay.executeLogicalGraphMutationBatch(createdEdgeProperty).created_edge_ids,
				  (std::vector<uint64_t>{200U}));
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->readEdge(200U).properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"rank", PropertyValue(int64_t(2))}}));

		overlay.deleteLogicalEdges({{10U}, {200U}});
		EXPECT_THROW(overlay.replaceLogicalEdgeProperties({{10U, {{"rank", PropertyValue(int64_t(1))}}}}),
				 std::runtime_error);
		overlay.deleteLogicalNodes({{1U, false, 0U}});
		EXPECT_THROW(overlay.replaceLogicalNodeProperties({{1U, {{"rank", PropertyValue(int64_t(1))}}}}),
				 std::runtime_error);
		EXPECT_THROW(overlay.replaceLogicalNodeProperties({{0U, {}}}), std::invalid_argument);

	}

	TEST(LogicalGraphTransactionOverlayTest, DefensivelyHidesBaseEdgesWhoseEndpointWasDeleted) {
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		// A lower layer must normally expose matching adjacency and direct Edge
		// reads. Keep the overlay's endpoint visibility check tested even if a
		// malformed lower snapshot violates that invariant.
		base.hide_edges_from_adjacency = true;
		LogicalGraphTransactionOverlay overlay(transaction);
		overlay.deleteLogicalNodes({{1U, false, 0U}});

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(10U)), std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, EnforcesNodeScanBudgetAndTerminalTraversalPaths) {
		InMemoryLogicalTransaction terminalTransaction;
		LogicalGraphTransactionOverlay terminalOverlay(terminalTransaction);
		std::unique_ptr<LogicalGraphReadSnapshot> terminal = terminalOverlay.openLogicalGraphReadSnapshot();
		std::vector<uint64_t> terminalNodes;
		terminal->visitNodes(3U, [&terminalNodes](const LogicalGraphNode &node) {
			terminalNodes.push_back(node.node_id);
			return true;
		});
		EXPECT_EQ(terminalNodes, (std::vector<uint64_t>{1U, 2U}));

		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.edges.clear();
		LogicalGraphTransactionOverlay overlay(transaction);
		overlay.deleteLogicalNodes({{1U, false, 0U}});
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 1U)), std::runtime_error);

		ASSERT_EQ(overlay.createLogicalNodes({{{"Created"}, {}}, {{"Created"}, {}}}).size(), 2U);
		const LogicalGraphNodePage createdPage = snapshot->scanNodesAfter(2U, 1U, 1U);
		ASSERT_EQ(createdPage.nodes.size(), 1U);
		EXPECT_TRUE(createdPage.may_have_more);
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
								 0U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)),
				 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
								 1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 0U)),
				 std::invalid_argument);
	}

	TEST(LogicalGraphTransactionOverlayTest, DefensivelyEnforcesVisitorBoundsAndStopsCreatedVisitors) {
		InMemoryLogicalTransaction malformedVisitorTransaction;
		auto &malformedBase = static_cast<InMemoryLogicalSnapshot &>(malformedVisitorTransaction.readSnapshot());
		malformedBase.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "SECOND"});
		malformedBase.ignore_edge_visit_bound = true;
		LogicalGraphTransactionOverlay malformedVisitorOverlay(malformedVisitorTransaction);
		std::unique_ptr<LogicalGraphReadSnapshot> malformedSnapshot =
				malformedVisitorOverlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(malformedSnapshot->visitEdges(1U, [](const LogicalGraphEdge &) { return true; }), std::runtime_error);

		InMemoryLogicalTransaction createdVisitorTransaction;
		LogicalGraphTransactionOverlay createdVisitorOverlay(createdVisitorTransaction);
		ASSERT_EQ(createdVisitorOverlay.createLogicalEdges({{1U, 2U, "CREATED", {}}}).size(), 1U);
		std::unique_ptr<LogicalGraphReadSnapshot> createdSnapshot = createdVisitorOverlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(createdSnapshot->visitEdges(1U, [](const LogicalGraphEdge &) { return true; }), std::runtime_error);

		createdVisitorOverlay.deleteLogicalEdges({{10U}});
		createdSnapshot = createdVisitorOverlay.openLogicalGraphReadSnapshot();
		size_t createdVisits = 0U;
		createdSnapshot->visitEdges(2U, [&createdVisits](const LogicalGraphEdge &) {
			++createdVisits;
			return false;
		});
		EXPECT_EQ(createdVisits, 1U);

		InMemoryLogicalTransaction adjacencyTransaction;
		LogicalGraphTransactionOverlay adjacencyOverlay(adjacencyTransaction);
		std::unique_ptr<LogicalGraphReadSnapshot> adjacency = adjacencyOverlay.openLogicalGraphReadSnapshot();
		std::vector<uint64_t> adjacentIds;
		adjacency->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
																				[&adjacentIds](const LogicalGraphEdge &edge) {
																					adjacentIds.push_back(edge.edge_id);
																					return true;
																				});
		EXPECT_EQ(adjacentIds, (std::vector<uint64_t>{10U}));
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsMalformedCreatedReferencesAndDetachBoundViolations) {
		InMemoryLogicalTransaction completedTransaction;
		static_cast<void>(completedTransaction.commit());
		EXPECT_THROW(static_cast<void>(LogicalGraphTransactionOverlay(completedTransaction)), std::logic_error);

		InMemoryLogicalTransaction referenceTransaction;
		LogicalGraphTransactionOverlay referenceOverlay(referenceTransaction);
		const LogicalGraphMutationEntityReference persistedNode{
				LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U};
		LogicalGraphMutationBatch invalidCreatedEdgeReference;
		invalidCreatedEdgeReference.edge_creates = {{
				persistedNode,
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				"REL", {}}};
		invalidCreatedEdgeReference.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_EDGE,
																									 LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 1U}, {}}};
		EXPECT_THROW(static_cast<void>(referenceOverlay.executeLogicalGraphMutationBatch(invalidCreatedEdgeReference)),
				 std::invalid_argument);

		InMemoryLogicalTransaction malformedAdjacencyTransaction;
		auto &malformedBase = static_cast<InMemoryLogicalSnapshot &>(malformedAdjacencyTransaction.readSnapshot());
		malformedBase.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "SECOND"});
		malformedBase.ignore_adjacency_visit_bound = true;
		LogicalGraphTransactionOverlay malformedAdjacencyOverlay(malformedAdjacencyTransaction);
		EXPECT_THROW(malformedAdjacencyOverlay.deleteLogicalNodes({{1U, true, 1U}}), std::runtime_error);

		InMemoryLogicalTransaction createdDetachTransaction;
		auto &createdBase = static_cast<InMemoryLogicalSnapshot &>(createdDetachTransaction.readSnapshot());
		createdBase.edges.clear();
		LogicalGraphTransactionOverlay createdDetachOverlay(createdDetachTransaction);
		const uint64_t createdNode = createdDetachOverlay.createLogicalNodes({{{"Created"}, {}}}).front();
		const std::vector<uint64_t> createdEdges = createdDetachOverlay.createLogicalEdges(
				{{createdNode, createdNode, "SELF", {}}, {createdNode, createdNode, "SELF", {}}});
		ASSERT_EQ(createdEdges.size(), 2U);
		EXPECT_THROW(createdDetachOverlay.deleteLogicalNodes({{createdNode, true, 1U}}), std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, ValidatesVisitorsDirectionsAndEmptyDirectoryMetadata) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const std::function<bool(const LogicalGraphNode &)> noNodeVisitor;
		const std::function<bool(const LogicalGraphEdge &)> noEdgeVisitor;
		EXPECT_THROW(snapshot->visitNodes(1U, noNodeVisitor), std::invalid_argument);
		EXPECT_THROW(snapshot->visitEdges(1U, noEdgeVisitor), std::invalid_argument);
		EXPECT_THROW(snapshot->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, 1U, noEdgeVisitor),
				 std::invalid_argument);

		const LogicalGraphAdjacencyPage emptyOutgoing = snapshot->scanAdjacentEdges(
				2U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
		EXPECT_TRUE(emptyOutgoing.edges.empty());
		EXPECT_FALSE(emptyOutgoing.continuation.has_value());
		const LogicalGraphAdjacencyPage incoming = snapshot->scanAdjacentEdges(
				2U, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U);
		EXPECT_EQ(incoming.edges, (std::vector<LogicalGraphEdge>{LogicalGraphEdge{10U, 1U, 2U, "BASE_LINK"}}));

		InMemoryLogicalTransaction emptyDirectoryTransaction;
		auto &emptyBase = static_cast<InMemoryLogicalSnapshot &>(emptyDirectoryTransaction.readSnapshot());
		emptyBase.nodes.clear();
		emptyBase.edges.clear();
		emptyBase.return_zero_cost_terminal_node_page = true;
		LogicalGraphTransactionOverlay emptyDirectoryOverlay(emptyDirectoryTransaction);
		std::unique_ptr<LogicalGraphReadSnapshot> emptyDirectory = emptyDirectoryOverlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNodePage emptyNodes = emptyDirectory->scanNodesAfter(0U, 1U, 1U);
		EXPECT_TRUE(emptyNodes.nodes.empty());
		EXPECT_FALSE(emptyNodes.may_have_more);
		EXPECT_EQ(emptyNodes.metadata_pages_visited, 0U);

		InMemoryLogicalTransaction noFenceTransaction;
		static_cast<InMemoryLogicalSnapshot &>(noFenceTransaction.readSnapshot()).return_no_mutation_fence = true;
		EXPECT_THROW(static_cast<void>(LogicalGraphTransactionOverlay(noFenceTransaction)), std::logic_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, ValidatesShortCircuitingMutationInputsAndReusableProperties) {
		InMemoryLogicalTransaction invalidFenceTransaction;
		LogicalGraphTransactionOverlay invalidFenceOverlay(invalidFenceTransaction);
		LogicalGraphMutationBatch createBatch;
		createBatch.node_creates = {{{"Created"}, {}}};
		EXPECT_THROW(static_cast<void>(invalidFenceOverlay.executeLogicalGraphMutationBatch(
								 createBatch, LogicalGraphMutationFence{})),
				 LogicalGraphMutationConflict);
		EXPECT_THROW(static_cast<void>(invalidFenceOverlay.createLogicalNodes(
								 {{{"Created"}, {{"", PropertyValue(int64_t(1))}}}})),
				 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(invalidFenceOverlay.createLogicalNodes({{{"Duplicate", "Duplicate"}, {}}})),
				 std::invalid_argument);

		LogicalGraphMutationBatch invalidTarget;
		invalidTarget.edge_creates = {{
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				"REL", {}}};
		EXPECT_THROW(static_cast<void>(invalidFenceOverlay.executeLogicalGraphMutationBatch(invalidTarget)),
				 std::invalid_argument);

		InMemoryLogicalTransaction deletedTargetTransaction;
		auto &deletedTargetBase = static_cast<InMemoryLogicalSnapshot &>(deletedTargetTransaction.readSnapshot());
		deletedTargetBase.edges.clear();
		LogicalGraphTransactionOverlay deletedTargetOverlay(deletedTargetTransaction);
		deletedTargetOverlay.deleteLogicalNodes({{2U, false, 0U}});
		EXPECT_THROW(static_cast<void>(deletedTargetOverlay.createLogicalEdges({{1U, 2U, "REL", {}}})),
				 std::runtime_error);

		InMemoryLogicalTransaction propertyTransaction;
		LogicalGraphTransactionOverlay propertyOverlay(propertyTransaction);
		propertyOverlay.replaceLogicalEdgeProperties({{10U, {{"first", PropertyValue(int64_t(1))}}}});
		propertyOverlay.replaceLogicalEdgeProperties({{10U, {{"second", PropertyValue(int64_t(2))}}}});
		std::unique_ptr<LogicalGraphReadSnapshot> propertySnapshot = propertyOverlay.openLogicalGraphReadSnapshot();
		const LogicalGraphEdge updated = propertySnapshot->readEdge(10U);
		ASSERT_EQ(updated.properties.size(), 1U);
		EXPECT_EQ(updated.properties.front().key, "second");

		InMemoryLogicalTransaction incompleteEdgeTransaction;
		LogicalGraphTransactionOverlay incompleteEdgeOverlay(incompleteEdgeTransaction);
		incompleteEdgeTransaction.setIncompletePreview(true);
		EXPECT_THROW(static_cast<void>(incompleteEdgeOverlay.createLogicalEdges({{1U, 2U, "REL", {}}})),
				 std::logic_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, ValidatesTypedCreatedReferencesAndFiltersCreatedDirections) {
		const LogicalGraphMutationEntityReference persistedNode{
				LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U};
		InMemoryLogicalTransaction wrongCreatedNodeTransaction;
		LogicalGraphTransactionOverlay wrongCreatedNodeOverlay(wrongCreatedNodeTransaction);
		LogicalGraphMutationBatch wrongCreatedNode;
		wrongCreatedNode.node_creates = {{{"Created"}, {}}};
		wrongCreatedNode.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_EDGE,
																									 LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U}, {}}};
		EXPECT_THROW(static_cast<void>(wrongCreatedNodeOverlay.executeLogicalGraphMutationBatch(wrongCreatedNode)),
				 std::invalid_argument);

		InMemoryLogicalTransaction wrongCreatedEdgeTransaction;
		LogicalGraphTransactionOverlay wrongCreatedEdgeOverlay(wrongCreatedEdgeTransaction);
		LogicalGraphMutationBatch wrongCreatedEdge;
		wrongCreatedEdge.edge_creates = {{
				persistedNode,
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				"REL", {}}};
		wrongCreatedEdge.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_NODE,
																									 LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U}, {}}};
		EXPECT_THROW(static_cast<void>(wrongCreatedEdgeOverlay.executeLogicalGraphMutationBatch(wrongCreatedEdge)),
				 std::invalid_argument);

		InMemoryLogicalTransaction directionTransaction;
		auto &directionBase = static_cast<InMemoryLogicalSnapshot &>(directionTransaction.readSnapshot());
		directionBase.edges.clear();
		LogicalGraphTransactionOverlay directionOverlay(directionTransaction);
		const uint64_t created = directionOverlay.createLogicalNodes({{{"Created"}, {}}}).front();
		ASSERT_EQ(directionOverlay.createLogicalEdges({{1U, created, "REL", {}}}), (std::vector<uint64_t>{200U}));
		std::unique_ptr<LogicalGraphReadSnapshot> directionSnapshot = directionOverlay.openLogicalGraphReadSnapshot();
		EXPECT_EQ(directionSnapshot->scanAdjacentEdges(
							  created, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U)
					  .edges,
				  (std::vector<LogicalGraphEdge>{{200U, 1U, created, "REL"}}));
		EXPECT_TRUE(directionSnapshot->scanAdjacentEdges(
							  2U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)
					  .edges
					  .empty());
	}

	TEST(LogicalGraphTransactionOverlayTest, FiltersDeletedNodesAcrossScansAndDirectEdgeReads) {
		InMemoryLogicalTransaction nodeScanTransaction;
		auto &nodeScanBase = static_cast<InMemoryLogicalSnapshot &>(nodeScanTransaction.readSnapshot());
		nodeScanBase.edges.clear();
		LogicalGraphTransactionOverlay nodeScanOverlay(nodeScanTransaction);
		nodeScanOverlay.replaceLogicalNodeProperties({{1U, {{"state", PropertyValue("updated")}}}});
		ASSERT_EQ(nodeScanOverlay.createLogicalNodes({{{"Created"}, {}}, {{"Created"}, {}}}).size(), 2U);
		nodeScanOverlay.deleteLogicalNodes({{100U, false, 0U}});
		std::unique_ptr<LogicalGraphReadSnapshot> nodeScan = nodeScanOverlay.openLogicalGraphReadSnapshot();
		EXPECT_EQ(nodeScan->scanNodesAfter(100U, 1U, 1U).nodes,
				  (std::vector<LogicalGraphNode>{{101U, {"Created"}}}));
		EXPECT_TRUE(nodeScan->scanNodesAfter(101U, 1U, 1U).nodes.empty());

		InMemoryLogicalTransaction endpointTransaction;
		auto &endpointBase = static_cast<InMemoryLogicalSnapshot &>(endpointTransaction.readSnapshot());
		endpointBase.hide_edges_from_adjacency = true;
		LogicalGraphTransactionOverlay endpointOverlay(endpointTransaction);
		endpointOverlay.deleteLogicalNodes({{2U, false, 0U}});
		// Create an overlay Edge state without marking it deleted. The direct read
		// must still reject it because its target Node is transaction-invisible.
		endpointOverlay.replaceLogicalEdgeProperties({{10U, {{"state", PropertyValue("updated")}}}});
		std::unique_ptr<LogicalGraphReadSnapshot> endpointSnapshot = endpointOverlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(endpointSnapshot->readEdge(10U)), std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, EnforcesCreatedTargetDetachAndHiddenPropertyLookup) {
		InMemoryLogicalTransaction targetDetachTransaction;
		auto &targetDetachBase = static_cast<InMemoryLogicalSnapshot &>(targetDetachTransaction.readSnapshot());
		targetDetachBase.edges.clear();
		LogicalGraphTransactionOverlay targetDetachOverlay(targetDetachTransaction);
		ASSERT_EQ(targetDetachOverlay.createLogicalEdges({{1U, 2U, "REL", {}}}), (std::vector<uint64_t>{200U}));
		EXPECT_THROW(targetDetachOverlay.deleteLogicalNodes({{2U, false, 0U}}), std::runtime_error);
		EXPECT_NO_THROW(targetDetachOverlay.deleteLogicalNodes({{2U, true, 1U}}));

		InMemoryLogicalTransaction propertyTransaction;
		auto &propertyBase = static_cast<InMemoryLogicalSnapshot &>(propertyTransaction.readSnapshot());
		propertyBase.edges.clear();
		LogicalGraphTransactionOverlay propertyOverlay(propertyTransaction);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = propertyOverlay.openLogicalGraphReadSnapshot();
		propertyOverlay.deleteLogicalNodes({{1U, false, 0U}});
		EXPECT_THROW(static_cast<void>(snapshot->readNode(1U)), std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, RetainsReservationOnlyBatchesAndChecksBothDetachEdgeSources) {
		InMemoryLogicalTransaction reservationTransaction;
		LogicalGraphTransactionOverlay reservationOverlay(reservationTransaction);
		LogicalGraphMutationBatch cancelledReservation;
		cancelledReservation.creation_identity_plan =
				LogicalGraphCreationIdentityPlan{{{100U, 1U}}, {}, {{200U, 1U}}, {}};
		EXPECT_EQ(reservationOverlay.executeLogicalGraphMutationBatch(cancelledReservation),
				  (LogicalGraphMutationBatchResult{}));

		InMemoryLogicalTransaction stagedBaseEdgeTransaction;
		LogicalGraphTransactionOverlay stagedBaseEdgeOverlay(stagedBaseEdgeTransaction);
		stagedBaseEdgeOverlay.replaceLogicalEdgeProperties({{10U, {{"state", PropertyValue("updated")}}}});
		EXPECT_NO_THROW(stagedBaseEdgeOverlay.deleteLogicalNodes({{1U, true, 1U}}));

		InMemoryLogicalTransaction stagedSourceEdgeTransaction;
		auto &stagedSourceBase = static_cast<InMemoryLogicalSnapshot &>(stagedSourceEdgeTransaction.readSnapshot());
		stagedSourceBase.edges.clear();
		LogicalGraphTransactionOverlay stagedSourceEdgeOverlay(stagedSourceEdgeTransaction);
		ASSERT_EQ(stagedSourceEdgeOverlay.createLogicalEdges({{1U, 2U, "REL", {}}}), (std::vector<uint64_t>{200U}));
		EXPECT_THROW(stagedSourceEdgeOverlay.deleteLogicalNodes({{1U, false, 0U}}), std::runtime_error);
	}

	} // namespace

} // namespace graph::storage
