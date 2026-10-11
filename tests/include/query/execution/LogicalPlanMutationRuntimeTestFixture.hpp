/**
 * @file test_LogicalPlanMutationRuntime.cpp
 * @brief End-to-end tests for bounded logical-plan mutations.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Types.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/query/execution/LogicalPlanMutationRuntime.hpp"
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalCreateEdge.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalForeach.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalRemove.hpp"
#include "graph/query/logical/operators/LogicalSet.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"

namespace graph::query {

	namespace {

		using PropertyMap = std::unordered_map<std::string, PropertyValue>;

		std::atomic<int64_t> mergeResultExpressionCalls{0};

		[[nodiscard]] PropertyValue countingMergeResultExpression(
				[[maybe_unused]] const std::vector<PropertyValue> &arguments,
				[[maybe_unused]] const expressions::EvaluationContext &context) {
			return PropertyValue(mergeResultExpressionCalls.fetch_add(1, std::memory_order_relaxed));
		}

		void registerCountingMergeResultExpression() {
			expressions::FunctionRegistry::getInstance().registerFunction(
					std::make_unique<expressions::LambdaScalarFunction>(
							expressions::FunctionSignature{"testMergeResultCounter", 0U, 0U},
							&countingMergeResultExpression));
		}

		[[nodiscard]] execution::LogicalPlanReadRuntimeBounds nativeMutationBounds() {
			return LogicalReadExecutionOptions{}.bounds;
		}

		struct TestNode {
			std::vector<std::string> labels;
			PropertyMap properties;
		};

		struct TestEdge {
			uint64_t source = 0U;
			uint64_t target = 0U;
			std::string type;
			PropertyMap properties;
		};

		struct TestGraph {
			uint64_t generation = 1U;
			uint64_t next_node_id = 1U;
			uint64_t next_edge_id = 1U;
			std::map<uint64_t, TestNode> nodes;
			std::map<uint64_t, TestEdge> edges;
			size_t mutation_batches = 0U;
			std::chrono::milliseconds delay_after_mutation_batch{0};
		};

		enum class TestSnapshotMode {
			TSSM_VALID,
			TSSM_NULL,
			TSSM_MISSING_FENCE,
			TSSM_INVALID_FENCE,
		};

		[[nodiscard]] PropertyMap toPropertyMap(const std::vector<storage::LogicalGraphWriteProperty> &properties) {
			PropertyMap result;
			result.reserve(properties.size());
			for (const storage::LogicalGraphWriteProperty &property: properties) {
				if (property.key.empty() || !result.emplace(property.key, property.value).second) {
					throw std::logic_error("test graph received malformed Property input");
				}
			}
			return result;
		}

		class TestLogicalGraphSnapshot final : public storage::LogicalGraphReadSnapshot {
		public:
			explicit TestLogicalGraphSnapshot(TestGraph graph, TestSnapshotMode mode = TestSnapshotMode::TSSM_VALID) :
				graph_(std::move(graph)), mode_(mode) {}

			[[nodiscard]] storage::LogicalGraphNode readNode(uint64_t nodeId) const override {
				const auto found = graph_.nodes.find(nodeId);
				if (found == graph_.nodes.end()) {
					throw std::runtime_error("test graph Node was not found");
				}
				std::vector<storage::LogicalGraphPropertyEntry> properties;
				properties.reserve(found->second.properties.size());
				for (const auto &[key, value]: found->second.properties) {
					properties.push_back({key, value});
				}
				std::ranges::sort(properties, {}, &storage::LogicalGraphPropertyEntry::key);
				return {nodeId, found->second.labels, std::move(properties)};
			}

			[[nodiscard]] storage::LogicalGraphEdge readEdge(uint64_t edgeId) const override {
				const auto found = graph_.edges.find(edgeId);
				if (found == graph_.edges.end()) {
					throw std::runtime_error("test graph relationship was not found");
				}
				std::vector<storage::LogicalGraphPropertyEntry> properties;
				properties.reserve(found->second.properties.size());
				for (const auto &[key, value]: found->second.properties) {
					properties.push_back({key, value});
				}
				std::ranges::sort(properties, {}, &storage::LogicalGraphPropertyEntry::key);
				return {edgeId, found->second.source, found->second.target, found->second.type, std::move(properties)};
			}

			void visitNodes(size_t maximumRecords,
							const std::function<bool(const storage::LogicalGraphNode &)> &visitor) const override {
				if (!visitor || graph_.nodes.size() > maximumRecords) {
					throw std::runtime_error("test graph Node visitor bound was exceeded");
				}
				for (const auto &[id, _]: graph_.nodes) {
					if (!visitor(readNode(id))) {
						return;
					}
				}
			}

			[[nodiscard]] storage::LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords,
																	   size_t maximumMetadataPages) const override {
				if (maximumRecords == 0U || maximumMetadataPages == 0U) {
					throw std::runtime_error("test graph Node scan needs positive bounds");
				}
				storage::LogicalGraphNodePage page;
				page.metadata_pages_visited = 1U;
				auto current = graph_.nodes.upper_bound(afterNodeId);
				while (current != graph_.nodes.end() && page.nodes.size() < maximumRecords) {
					page.nodes.push_back(readNode(current->first));
					++current;
				}
				page.may_have_more = current != graph_.nodes.end();
				return page;
			}

			[[nodiscard]] storage::LogicalGraphAdjacencyPage
			scanAdjacentEdges(uint64_t nodeId, storage::LogicalGraphReadDirection direction,
							  std::optional<storage::LogicalGraphAdjacencyContinuation> continuation,
							  size_t maximumRecords) const override {
				if (maximumRecords == 0U || !graph_.nodes.contains(nodeId)) {
					throw std::runtime_error("test graph relationship scan needs a valid bound and Node");
				}
				std::vector<storage::LogicalGraphEdge> candidates;
				for (const auto &[id, edge]: graph_.edges) {
					const bool matches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
												 ? edge.source == nodeId
												 : edge.target == nodeId;
					if (matches) {
						candidates.push_back(readEdge(id));
					}
				}
				size_t start = 0U;
				if (continuation.has_value()) {
					while (start < candidates.size() && candidates[start].edge_id != continuation->next_edge_id) {
						++start;
					}
					if (start == candidates.size()) {
						throw std::runtime_error("test graph relationship continuation is invalid");
					}
				}
				storage::LogicalGraphAdjacencyPage page;
				const size_t end = std::min(candidates.size(), start + maximumRecords);
				page.edges.insert(page.edges.end(), candidates.begin() + static_cast<std::ptrdiff_t>(start),
								  candidates.begin() + static_cast<std::ptrdiff_t>(end));
				if (end < candidates.size()) {
					page.continuation = storage::LogicalGraphAdjacencyContinuation{page.edges.back().edge_id,
																				   candidates[end].edge_id};
				}
				return page;
			}

			void visitEdges(size_t maximumRecords,
							const std::function<bool(const storage::LogicalGraphEdge &)> &visitor) const override {
				if (!visitor || graph_.edges.size() > maximumRecords) {
					throw std::runtime_error("test graph relationship visitor bound was exceeded");
				}
				for (const auto &[id, _]: graph_.edges) {
					if (!visitor(readEdge(id))) {
						return;
					}
				}
			}

			void
			visitAdjacentEdges(uint64_t nodeId, storage::LogicalGraphReadDirection direction, size_t maximumEdges,
							   const std::function<bool(const storage::LogicalGraphEdge &)> &visitor) const override {
				if (!visitor) {
					throw std::runtime_error("test graph relationship visitor is empty");
				}
				auto page = scanAdjacentEdges(nodeId, direction, std::nullopt, maximumEdges);
				if (page.continuation.has_value()) {
					throw std::runtime_error("test graph relationship visitor bound was exceeded");
				}
				for (const auto &edge: page.edges) {
					if (!visitor(edge)) {
						return;
					}
				}
			}

			[[nodiscard]] std::optional<storage::LogicalGraphMutationFence> mutationFence() const override {
				if (mode_ == TestSnapshotMode::TSSM_MISSING_FENCE) {
					return std::nullopt;
				}
				if (mode_ == TestSnapshotMode::TSSM_INVALID_FENCE) {
					return storage::LogicalGraphMutationFence{};
				}
				storage::LogicalGraphMutationFence fence;
				fence.backend_token = 0x1234U;
				fence.format_token = 0x5678U;
				fence.graph_state_tokens[0] = graph_.generation;
				return fence;
			}

		private:
			TestGraph graph_;
			TestSnapshotMode mode_;
		};

		class TestLogicalGraph final : public storage::LogicalGraphReadBackend,
									   public storage::LogicalGraphWriteBackend {
		public:
			[[nodiscard]] std::unique_ptr<storage::LogicalGraphReadSnapshot>
			openLogicalGraphReadSnapshot() const override {
				if (snapshot_mode_ == TestSnapshotMode::TSSM_NULL) {
					return nullptr;
				}
				return std::make_unique<TestLogicalGraphSnapshot>(graph_, snapshot_mode_);
			}

			[[nodiscard]] std::vector<uint64_t>
			createLogicalNodes(const std::vector<storage::LogicalGraphNodeCreateRequest> &) override {
				throw std::logic_error("test graph expects atomic mutation batches");
			}

			[[nodiscard]] std::vector<uint64_t>
			createLogicalEdges(const std::vector<storage::LogicalGraphEdgeCreateRequest> &) override {
				throw std::logic_error("test graph expects atomic mutation batches");
			}

			void
			replaceLogicalNodeProperties(const std::vector<storage::LogicalGraphPropertyReplaceRequest> &) override {
				throw std::logic_error("test graph expects atomic mutation batches");
			}

			void
			replaceLogicalEdgeProperties(const std::vector<storage::LogicalGraphPropertyReplaceRequest> &) override {
				throw std::logic_error("test graph expects atomic mutation batches");
			}

			void deleteLogicalNodes(const std::vector<storage::LogicalGraphNodeDeleteRequest> &) override {
				throw std::logic_error("test graph expects atomic mutation batches");
			}

			void deleteLogicalEdges(const std::vector<storage::LogicalGraphEdgeDeleteRequest> &) override {
				throw std::logic_error("test graph expects atomic mutation batches");
			}

			[[nodiscard]] storage::LogicalGraphMutationBatchResult
			executeLogicalGraphMutationBatch(const storage::LogicalGraphMutationBatch &batch,
											 std::optional<storage::LogicalGraphMutationFence> expectedFence) override {
				if (!expectedFence.has_value() || expectedFence->backend_token != 0x1234U ||
					expectedFence->format_token != 0x5678U ||
					expectedFence->graph_state_tokens[0] != graph_.generation) {
					throw storage::LogicalGraphMutationConflict("test graph observed a stale mutation fence");
				}
				if (remaining_mutation_conflicts_ != 0U) {
					--remaining_mutation_conflicts_;
					throw storage::LogicalGraphMutationConflict("test graph injected an optimistic conflict");
				}
				storage::LogicalGraphMutationBatchResult result;
				result.created_node_ids.reserve(batch.node_creates.size());
				for (const auto &request: batch.node_creates) {
					const uint64_t id = graph_.next_node_id++;
					graph_.nodes.emplace(id, TestNode{request.labels, toPropertyMap(request.properties)});
					result.created_node_ids.push_back(id);
				}
				const auto resolveNode = [&](const storage::LogicalGraphMutationEntityReference &reference) {
					if (reference.entity_kind != storage::LogicalGraphMutationEntityKind::LGMK_NODE) {
						throw std::logic_error("test graph relationship endpoint is not a Node");
					}
					if (reference.reference_kind == storage::LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
						if (!graph_.nodes.contains(reference.value)) {
							throw std::logic_error("test graph relationship endpoint does not exist");
						}
						return reference.value;
					}
					if (reference.reference_kind == storage::LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
						return result.created_node_ids.at(static_cast<size_t>(reference.value));
					}
					throw std::logic_error("test graph relationship endpoint reference is invalid");
				};
				result.created_edge_ids.reserve(batch.edge_creates.size());
				for (const auto &request: batch.edge_creates) {
					const uint64_t id = graph_.next_edge_id++;
					graph_.edges.emplace(id, TestEdge{resolveNode(request.source), resolveNode(request.target),
													  request.type, toPropertyMap(request.properties)});
					result.created_edge_ids.push_back(id);
				}
				for (const auto &replacement: batch.property_replacements) {
					const PropertyMap properties = toPropertyMap(replacement.properties);
					if (replacement.entity.reference_kind !=
						storage::LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
						throw std::logic_error("test graph only accepts persisted Property replacement targets");
					}
					if (replacement.entity.entity_kind == storage::LogicalGraphMutationEntityKind::LGMK_NODE) {
						graph_.nodes.at(replacement.entity.value).properties = properties;
					} else {
						graph_.edges.at(replacement.entity.value).properties = properties;
					}
				}
				for (const auto &replacement: batch.node_label_replacements) {
					if (replacement.node.entity_kind != storage::LogicalGraphMutationEntityKind::LGMK_NODE ||
						replacement.node.reference_kind != storage::LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
						throw std::logic_error("test graph only accepts persisted Node label replacement targets");
					}
					graph_.nodes.at(replacement.node.value).labels = replacement.labels;
				}
				for (const auto &request: batch.edge_deletes) {
					if (graph_.edges.erase(request.edge_id) != 1U) {
						throw std::logic_error("test graph relationship deletion target does not exist");
					}
				}
				for (const auto &request: batch.node_deletes) {
					if (!graph_.nodes.contains(request.node_id)) {
						throw std::logic_error("test graph Node deletion target does not exist");
					}
					std::vector<uint64_t> incidentEdges;
					for (const auto &[edgeId, edge]: graph_.edges) {
						if (edge.source == request.node_id || edge.target == request.node_id) {
							incidentEdges.push_back(edgeId);
						}
					}
					if (!request.detach && !incidentEdges.empty()) {
						throw std::logic_error("test graph cannot delete a Node with relationships without DETACH");
					}
					if (request.detach && incidentEdges.size() > request.maximum_detach_edges) {
						throw std::logic_error("test graph detach deletion exceeded its relationship bound");
					}
					for (const uint64_t edgeId: incidentEdges) {
						graph_.edges.erase(edgeId);
					}
					graph_.nodes.erase(request.node_id);
				}
				if (!batch.node_creates.empty() || !batch.edge_creates.empty() ||
					!batch.property_replacements.empty() || !batch.node_label_replacements.empty() ||
					!batch.edge_deletes.empty() || !batch.node_deletes.empty()) {
					++graph_.generation;
					++graph_.mutation_batches;
					if (graph_.delay_after_mutation_batch.count() > 0) {
						std::this_thread::sleep_for(graph_.delay_after_mutation_batch);
					}
				}
				return result;
			}

			[[nodiscard]] uint64_t addNode(std::vector<std::string> labels, PropertyMap properties) {
				const uint64_t id = graph_.next_node_id++;
				graph_.nodes.emplace(id, TestNode{std::move(labels), std::move(properties)});
				return id;
			}

			[[nodiscard]] size_t countNodesWithLabel(const std::string &label) const {
				return static_cast<size_t>(std::ranges::count_if(graph_.nodes, [&label](const auto &entry) {
					return std::ranges::find(entry.second.labels, label) != entry.second.labels.end();
				}));
			}

			[[nodiscard]] size_t edgeCount() const { return graph_.edges.size(); }
			[[nodiscard]] const TestGraph &graph() const { return graph_; }
			void rejectNextMutation() { remaining_mutation_conflicts_ = 1U; }
			void rejectMutationAttempts(size_t count) { remaining_mutation_conflicts_ = count; }
			void setSnapshotMode(TestSnapshotMode mode) { snapshot_mode_ = mode; }
			void setDelayAfterMutationBatch(std::chrono::milliseconds delay) {
				graph_.delay_after_mutation_batch = delay;
			}

		private:
			TestGraph graph_;
			TestSnapshotMode snapshot_mode_ = TestSnapshotMode::TSSM_VALID;
			 size_t remaining_mutation_conflicts_ = 0U;
		};

		// Logical plans can be built by public native callers, so the mutation
		// boundary must reject a type/shape mismatch before it stages a write.
		class MalformedLogicalOperator final : public logical::LogicalOperator {
		public:
			MalformedLogicalOperator(logical::LogicalOpType type, std::vector<logical::LogicalOperator *> children) :
				type_(type), children_(std::move(children)) {}

			[[nodiscard]] logical::LogicalOpType getType() const override { return type_; }
			[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override { return children_; }
			[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> clone() const override {
				return std::make_unique<MalformedLogicalOperator>(type_, children_);
			}
			[[nodiscard]] std::string toString() const override { return "MalformedLogicalOperator"; }
			void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {
				throw std::logic_error("malformed test operator cannot replace children");
			}
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override {
				throw std::logic_error("malformed test operator cannot detach children");
			}

		private:
			logical::LogicalOpType type_;
			std::vector<logical::LogicalOperator *> children_;
		};

		class MalformedExpression final : public expressions::Expression {
		public:
			explicit MalformedExpression(expressions::ExpressionType type) : type_(type) {}

			[[nodiscard]] expressions::ExpressionType getExpressionType() const override { return type_; }
			void accept(expressions::ExpressionVisitor &) override {}
			void accept(expressions::ConstExpressionVisitor &) const override {}
			[[nodiscard]] std::string toString() const override { return "MalformedExpression"; }
			[[nodiscard]] std::unique_ptr<expressions::Expression> clone() const override {
				return std::make_unique<MalformedExpression>(type_);
			}

		private:
			expressions::ExpressionType type_;
		};

	} // namespace

} // namespace graph::query
