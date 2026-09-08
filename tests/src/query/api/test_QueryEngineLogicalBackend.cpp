/**
 * @file test_QueryEngineLogicalBackend.cpp
 * @brief Boundary tests for the isolated logical read QueryEngine mode.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Types.hpp"
#include "graph/query/NativeProcedureManifest.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/gds/LogicalProjectionCatalog.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalExplain.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProfile.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/LogicalGraphVectorSearch.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"

namespace graph::query {

	namespace {

		struct NativeProcedureQuery {
			std::string_view name;
			std::string_view arguments;

			[[nodiscard]] std::string text() const { return "CALL " + std::string(name) + std::string(arguments); }
		};

		[[nodiscard]] const std::vector<NativeProcedureQuery> &nativeProcedureQueries() {
			// Keep one valid call for every manifest entry. Building the text from
			// the manifest name prevents a test row from exercising a different
			// procedure than the one its C0 acceptance record claims to cover.
			static const std::vector<NativeProcedureQuery> queries = {
					{"algo.shortestPath", "(1, 2, 1)"},
					{"db.index.vector.queryNodes", "('embedding', 1, [1.0])"},
					{"db.index.vector.train", "('embedding')"},
					{"gds.graph.project", "('projection', {})"},
					{"gds.graph.drop", "('projection')"},
					{"gds.graph.dropAll", "()"},
					{"gds.graph.exists", "('projection')"},
					{"gds.graph.list", "()"},
					{"gds.graph.schema", "('projection')"},
					{"gds.pageRank.stream", "('projection')"},
					{"gds.wcc.stream", "('projection')"},
					{"gds.betweenness.stream", "('projection')"},
					{"gds.closeness.stream", "('projection')"},
					{"gds.leiden.stream", "('projection')"},
					{"gds.shortestPath.dijkstra.stream", "('projection', 1, 2)"},
					{"dbms.listConfig", "()"},
					{"dbms.getConfig", "('query.timeout_ms')"},
					{"dbms.setConfig", "('query.timeout_ms', 45)"},
					{"dbms.showStats", "()"},
					{"dbms.resetStats", "()"},
					{"dbms.listSlowQueries", "()"},
					{"dbms.clearSlowQueries", "()"},
			};
			return queries;
		}

		[[nodiscard]] std::vector<std::string> nativeProcedureManifestNames() {
			std::vector<std::string> names;
			names.reserve(nativeProcedureManifest().size());
			for (const NativeProcedureManifestEntry &entry: nativeProcedureManifest()) {
				names.emplace_back(entry.name);
			}
			std::ranges::sort(names);
			return names;
		}

		[[nodiscard]] std::vector<std::string> nativeProcedureQueryNames() {
			std::vector<std::string> names;
			names.reserve(nativeProcedureQueries().size());
			for (const NativeProcedureQuery &query: nativeProcedureQueries()) {
				names.emplace_back(query.name);
			}
			std::ranges::sort(names);
			return names;
		}

		class UnusedLogicalGraphReadBackend final : public storage::LogicalGraphReadBackend {
		public:
			[[nodiscard]] std::unique_ptr<storage::LogicalGraphReadSnapshot>
			openLogicalGraphReadSnapshot() const override {
				throw std::logic_error("test backend must not open a snapshot");
			}
		};

		class EmptyLogicalGraphSnapshot final : public storage::LogicalGraphReadSnapshot {
		public:
			[[nodiscard]] storage::LogicalGraphNode readNode(uint64_t) const override {
				throw std::logic_error("test snapshot must not read Nodes");
			}
			[[nodiscard]] storage::LogicalGraphEdge readEdge(uint64_t) const override {
				throw std::logic_error("test snapshot must not read relationships");
			}
			void visitNodes(size_t, const std::function<bool(const storage::LogicalGraphNode &)> &) const override {
				throw std::logic_error("test snapshot must not scan Nodes");
			}
			[[nodiscard]] storage::LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override {
				throw std::logic_error("test snapshot must not page through Nodes");
			}
			[[nodiscard]] storage::LogicalGraphAdjacencyPage
			scanAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection,
							  std::optional<storage::LogicalGraphAdjacencyContinuation>, size_t) const override {
				throw std::logic_error("test snapshot must not scan adjacency");
			}
			void visitEdges(size_t, const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {
				throw std::logic_error("test snapshot must not scan relationships");
			}
			void visitAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection, size_t,
								const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {
				throw std::logic_error("test snapshot must not visit adjacency");
			}
			[[nodiscard]] std::optional<storage::LogicalGraphMutationFence> mutationFence() const override {
				return storage::LogicalGraphMutationFence{0x1234U, 0x5678U, {}};
			}
		};

		class CountingLogicalGraphReadBackend final : public storage::LogicalGraphReadBackend {
		public:
			[[nodiscard]] std::unique_ptr<storage::LogicalGraphReadSnapshot>
			openLogicalGraphReadSnapshot() const override {
				++snapshot_opens;
				return std::make_unique<EmptyLogicalGraphSnapshot>();
			}

			mutable size_t snapshot_opens = 0U;
		};

		class StatisticsLogicalGraphReadBackend final : public storage::LogicalGraphReadBackend,
														 public storage::LogicalGraphStatisticsBackend {
		public:
			[[nodiscard]] std::unique_ptr<storage::LogicalGraphReadSnapshot>
			openLogicalGraphReadSnapshot() const override {
				++snapshot_opens;
				throw std::logic_error("planning must not open a logical graph snapshot");
			}

			[[nodiscard]] std::optional<storage::LogicalGraphCardinalityStatistics>
			readLogicalGraphCardinalityStatistics() const override {
				++statistics_reads;
				if (fail_statistics_read) {
					throw std::runtime_error("test statistics service is unavailable");
				}
				return statistics;
			}

			storage::LogicalGraphCardinalityStatistics statistics{7U, 11U};
			bool fail_statistics_read = false;
			mutable size_t snapshot_opens = 0U;
			mutable size_t statistics_reads = 0U;
		};

		class RecordingAdministrationLogicalGraphBackend final : public storage::LogicalGraphReadBackend,
																	 public storage::LogicalGraphAdministrationBackend {
		public:
			[[nodiscard]] std::unique_ptr<storage::LogicalGraphReadSnapshot>
			openLogicalGraphReadSnapshot() const override {
				++snapshot_opens;
				if (snapshot_open_delay.count() > 0) {
					std::this_thread::sleep_for(snapshot_open_delay);
				}
				return std::make_unique<EmptyLogicalGraphSnapshot>();
			}

			[[nodiscard]] std::optional<PropertyValue> findConfiguration(const std::string_view key) const override {
				const auto found = configurations.find(std::string(key));
				return found == configurations.end() ? std::nullopt : std::optional<PropertyValue>{found->second};
			}

			[[nodiscard]] std::vector<storage::LogicalGraphConfiguration>
			readConfigurations(const size_t maximumRecords) const override {
				if (configurations.size() > maximumRecords) {
					throw std::runtime_error("test administration backend exceeded its configuration read bound");
				}
				std::vector<storage::LogicalGraphConfiguration> result;
				result.reserve(configurations.size());
				for (const auto &[key, value]: configurations) {
					result.push_back({key, value});
				}
				return result;
			}

			void setConfiguration(const std::string_view key, const PropertyValue &value) override {
				++configuration_writes;
				configurations.insert_or_assign(std::string(key), value);
			}

			[[nodiscard]] std::vector<storage::LogicalGraphRuntimeStatistic> runtimeStatistics() const override {
				return runtime_statistics;
			}

			void resetRuntimeStatistics() override { ++statistics_resets; }

			std::unordered_map<std::string, PropertyValue> configurations;
			std::vector<storage::LogicalGraphRuntimeStatistic> runtime_statistics;
			std::chrono::milliseconds snapshot_open_delay{0};
			mutable size_t snapshot_opens = 0U;
			size_t configuration_writes = 0U;
			size_t statistics_resets = 0U;
		};

		struct ProcedureBackendState {
			storage::LogicalGraphVectorSearchResult search_result;
			std::unordered_map<uint64_t, storage::LogicalGraphNode> nodes;
			std::vector<storage::LogicalGraphEdge> edges;
			bool return_nonmonotonic_adjacency_page = false;
			bool return_null_search_snapshot = false;
			size_t cancellation_checks_before_throw = 0U;
			mutable size_t search_cancellation_checks = 0U;
			std::chrono::milliseconds search_cancellation_initial_delay{0};
			size_t training_cancellation_checks_before_throw = 0U;
			mutable size_t training_cancellation_checks = 0U;
			std::chrono::milliseconds training_prepare_delay{0};
			std::chrono::milliseconds training_preview_delay{0};
			std::chrono::milliseconds training_commit_delay{0};
			std::chrono::milliseconds projection_scan_delay{0};
		};

		class ProcedureLogicalGraphSnapshot final : public storage::LogicalGraphVectorSearchSnapshot {
		public:
			explicit ProcedureLogicalGraphSnapshot(std::shared_ptr<const ProcedureBackendState> state) : state_(std::move(state)) {}

			[[nodiscard]] storage::LogicalGraphNode readNode(uint64_t nodeId) const override {
				const auto node = state_->nodes.find(nodeId);
				return node == state_->nodes.end() ? storage::LogicalGraphNode{} : node->second;
			}
			[[nodiscard]] bool containsNode(uint64_t nodeId) const override {
				return state_->nodes.contains(nodeId);
			}
			[[nodiscard]] storage::LogicalGraphEdge readEdge(uint64_t edgeId) const override {
				const auto edge = std::ranges::find(state_->edges, edgeId, &storage::LogicalGraphEdge::edge_id);
				return edge == state_->edges.end() ? storage::LogicalGraphEdge{} : *edge;
			}
			void visitNodes(size_t, const std::function<bool(const storage::LogicalGraphNode &)> &) const override {
				throw std::logic_error("scalar procedure test must not scan Nodes");
			}
			[[nodiscard]] storage::LogicalGraphNodePage scanNodesAfter(const uint64_t afterNodeId,
					const size_t maximumRecords, const size_t maximumMetadataPages) const override {
				if (maximumRecords == 0U || maximumMetadataPages == 0U) {
					throw std::invalid_argument("logical projection test received invalid Node scan bounds");
				}
				if (state_->projection_scan_delay.count() > 0) {
					std::this_thread::sleep_for(state_->projection_scan_delay);
				}
				std::vector<storage::LogicalGraphNode> nodes;
				nodes.reserve(state_->nodes.size());
				for (const auto &[_, node]: state_->nodes) {
					nodes.push_back(node);
				}
				std::ranges::sort(nodes, {}, &storage::LogicalGraphNode::node_id);
				storage::LogicalGraphNodePage page;
				for (const storage::LogicalGraphNode &node: nodes) {
					if (node.node_id <= afterNodeId) {
						continue;
					}
					if (page.nodes.size() == maximumRecords) {
						page.may_have_more = true;
						break;
					}
					page.nodes.push_back(node);
				}
				page.metadata_pages_visited = page.nodes.empty() ? 0U : 1U;
				return page;
			}
			[[nodiscard]] storage::LogicalGraphAdjacencyPage
			scanAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection,
							  std::optional<storage::LogicalGraphAdjacencyContinuation>, size_t) const override;
			void visitEdges(size_t, const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {
				throw std::logic_error("scalar procedure test must not scan relationships");
			}
			void visitAdjacentEdges(const uint64_t nodeId, const storage::LogicalGraphReadDirection direction,
							const size_t maximumRecords,
							const std::function<bool(const storage::LogicalGraphEdge &)> &visitor) const override {
				size_t visited = 0U;
				for (const storage::LogicalGraphEdge &edge: state_->edges) {
					const bool matches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
								 ? edge.source_node_id == nodeId
								 : edge.target_node_id == nodeId;
					if (!matches) {
						continue;
					}
					if (++visited > maximumRecords || !visitor(edge)) {
						return;
					}
				}
			}
			[[nodiscard]] std::optional<storage::LogicalGraphMutationFence> mutationFence() const override {
				return storage::LogicalGraphMutationFence{0x3456U, 0x789AU, {}};
			}
			[[nodiscard]] storage::LogicalGraphVectorSearchResult
			searchLogicalVector(const storage::LogicalGraphVectorSearchRequest &request) const override {
				if (state_->cancellation_checks_before_throw != 0U) {
					if (request.cancellation == nullptr) {
						throw std::logic_error("logical Vector procedure did not receive a cancellation checkpoint");
					}
					if (state_->search_cancellation_initial_delay.count() > 0) {
						std::this_thread::sleep_for(state_->search_cancellation_initial_delay);
					}
					for (size_t check = 0U; check < state_->cancellation_checks_before_throw; ++check) {
						++state_->search_cancellation_checks;
						request.cancellation->check();
					}
				}
				return state_->search_result;
			}

		private:
			std::shared_ptr<const ProcedureBackendState> state_;
		};

		storage::LogicalGraphAdjacencyPage ProcedureLogicalGraphSnapshot::scanAdjacentEdges(
				uint64_t nodeId, storage::LogicalGraphReadDirection direction,
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const {
			std::vector<storage::LogicalGraphEdge> edges;
			for (const storage::LogicalGraphEdge &edge: state_->edges) {
				const bool matches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
									 ? edge.source_node_id == nodeId
									 : edge.target_node_id == nodeId;
				if (matches) {
					edges.push_back(edge);
				}
			}
			std::ranges::sort(edges, {}, &storage::LogicalGraphEdge::edge_id);
			if (state_->return_nonmonotonic_adjacency_page && edges.size() > 1U) {
				std::swap(edges[0], edges[1]);
			}
			size_t begin = 0U;
			if (continuation.has_value()) {
				const auto next = std::ranges::find(edges, continuation->next_edge_id,
																		 &storage::LogicalGraphEdge::edge_id);
				if (next == edges.end()) {
					return {};
				}
				begin = static_cast<size_t>(std::distance(edges.begin(), next));
			}
			const size_t count = std::min(maximumRecords, edges.size() - begin);
			storage::LogicalGraphAdjacencyPage page;
			page.edges.insert(page.edges.end(), edges.begin() + static_cast<std::ptrdiff_t>(begin),
							  edges.begin() + static_cast<std::ptrdiff_t>(begin + count));
			if (begin + count < edges.size() && count != 0U) {
				page.continuation = {page.edges.back().edge_id, edges[begin + count].edge_id};
			}
			return page;
		}

		class RecordingVectorProcedureBackend final : public storage::LogicalGraphReadBackend,
																		 public storage::LogicalGraphVectorSearchBackend,
																		 public storage::LogicalGraphVectorMaintenanceBackend {
		public:
			class PreparedTraining final : public storage::PreparedLogicalGraphVectorTraining {
			public:
				PreparedTraining(RecordingVectorProcedureBackend &backend,
									const storage::LogicalGraphVectorTrainingRequest &request,
									storage::LogicalGraphVectorTrainingResult result) :
					backend_(backend), request_(request), result_(result), preview_{result.trained} {}

				[[nodiscard]] storage::LogicalGraphVectorTrainingPreview preview() const noexcept override {
					if (backend_.state->training_preview_delay.count() > 0) {
						std::this_thread::sleep_for(backend_.state->training_preview_delay);
					}
					return preview_;
				}

				[[nodiscard]] storage::LogicalGraphVectorTrainingResult commit() override {
					if (committed_) {
						throw std::logic_error("recording Vector training committed more than once");
					}
					committed_ = true;
					return backend_.publishTraining(request_, result_);
				}

			private:
				RecordingVectorProcedureBackend &backend_;
				storage::LogicalGraphVectorTrainingRequest request_;
				storage::LogicalGraphVectorTrainingResult result_;
				storage::LogicalGraphVectorTrainingPreview preview_;
				bool committed_ = false;
			};

			[[nodiscard]] std::unique_ptr<storage::LogicalGraphReadSnapshot>
			openLogicalGraphReadSnapshot() const override {
				return std::make_unique<ProcedureLogicalGraphSnapshot>(state);
			}
			[[nodiscard]] std::unique_ptr<storage::LogicalGraphVectorSearchSnapshot>
			openLogicalGraphVectorSearchSnapshot() const override {
				++search_snapshot_opens;
				if (state->return_null_search_snapshot) {
					return nullptr;
				}
				return std::make_unique<ProcedureLogicalGraphSnapshot>(state);
			}
			[[nodiscard]] std::unique_ptr<storage::PreparedLogicalGraphVectorTraining>
			prepareLogicalVectorTraining(const storage::LogicalGraphVectorTrainingRequest &request) override {
				return std::make_unique<PreparedTraining>(*this, request, prepareTraining(request));
			}

			[[nodiscard]] storage::LogicalGraphVectorTrainingResult
			prepareTraining(const storage::LogicalGraphVectorTrainingRequest &request) {
				++training_preparations;
				last_training_request = request;
				if (state->training_prepare_delay.count() > 0) {
					std::this_thread::sleep_for(state->training_prepare_delay);
				}
				if (state->training_cancellation_checks_before_throw != 0U) {
					if (request.cancellation == nullptr) {
						throw std::logic_error("logical Vector training did not receive a cancellation checkpoint");
					}
					for (size_t check = 0U; check < state->training_cancellation_checks_before_throw; ++check) {
						++state->training_cancellation_checks;
						request.cancellation->check();
					}
				}
				return training_result;
			}

			[[nodiscard]] storage::LogicalGraphVectorTrainingResult
			publishTraining(const storage::LogicalGraphVectorTrainingRequest &request,
								storage::LogicalGraphVectorTrainingResult result) {
				if (state->training_commit_delay.count() > 0) {
					std::this_thread::sleep_for(state->training_commit_delay);
				}
				++training_calls;
				last_training_request = request;
				return result;
			}

			mutable size_t search_snapshot_opens = 0U;
			size_t training_preparations = 0U;
			size_t training_calls = 0U;
			storage::LogicalGraphVectorTrainingRequest last_training_request;
			storage::LogicalGraphVectorTrainingResult training_result{1U, true, false};
			std::shared_ptr<ProcedureBackendState> state = std::make_shared<ProcedureBackendState>();
		};

		class RecordingPreparedLogicalVectorTraining final : public storage::PreparedLogicalGraphVectorTraining {
		public:
			explicit RecordingPreparedLogicalVectorTraining(size_t &commitCount) : commit_count_(commitCount) {}

			[[nodiscard]] storage::LogicalGraphVectorTrainingPreview preview() const noexcept override { return {true}; }

			[[nodiscard]] storage::LogicalGraphVectorTrainingResult commit() override {
				++commit_count_;
				return {7U, true, false};
			}

		private:
			size_t &commit_count_;
		};

		class DefaultLogicalVectorMaintenanceBackend final : public storage::LogicalGraphVectorMaintenanceBackend {
		public:
			[[nodiscard]] std::unique_ptr<storage::PreparedLogicalGraphVectorTraining>
			prepareLogicalVectorTraining(const storage::LogicalGraphVectorTrainingRequest &) override {
				if (return_null_preparation) {
					return nullptr;
				}
				auto prepared = std::make_unique<RecordingPreparedLogicalVectorTraining>(commit_count);
				return prepared;
			}

			bool return_null_preparation = false;
			size_t commit_count = 0U;
		};

		class SyntheticLogicalOperator final : public logical::LogicalOperator {
		public:
			explicit SyntheticLogicalOperator(logical::LogicalOpType type) : type_(type) {}

			[[nodiscard]] logical::LogicalOpType getType() const override { return type_; }
			[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override { return {}; }
			[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> clone() const override {
				return std::make_unique<SyntheticLogicalOperator>(type_);
			}
			[[nodiscard]] std::string toString() const override { return "SyntheticLogicalOperator"; }
			void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {
				throw std::logic_error("synthetic operator has no children");
			}
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override {
				throw std::logic_error("synthetic operator has no children");
			}

		private:
			logical::LogicalOpType type_;
		};

		class MissingChildLogicalProject final : public logical::LogicalProject {
		public:
			using LogicalProject::LogicalProject;

			[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override { return {}; }
		};

		class RecordingLogicalGraphWriteBackend final : public storage::LogicalGraphWriteBackend {
		public:
			[[nodiscard]] std::vector<uint64_t>
			createLogicalNodes(const std::vector<storage::LogicalGraphNodeCreateRequest> &) override {
				throw std::logic_error("test expects atomic mutation batches");
			}
			[[nodiscard]] std::vector<uint64_t>
			createLogicalEdges(const std::vector<storage::LogicalGraphEdgeCreateRequest> &) override {
				throw std::logic_error("test expects atomic mutation batches");
			}
			void replaceLogicalNodeProperties(const std::vector<storage::LogicalGraphPropertyReplaceRequest> &) override {
				throw std::logic_error("test expects atomic mutation batches");
			}
			void replaceLogicalEdgeProperties(const std::vector<storage::LogicalGraphPropertyReplaceRequest> &) override {
				throw std::logic_error("test expects atomic mutation batches");
			}
			void deleteLogicalNodes(const std::vector<storage::LogicalGraphNodeDeleteRequest> &) override {
				throw std::logic_error("test expects atomic mutation batches");
			}
			void deleteLogicalEdges(const std::vector<storage::LogicalGraphEdgeDeleteRequest> &) override {
				throw std::logic_error("test expects atomic mutation batches");
			}
			[[nodiscard]] storage::LogicalGraphMutationBatchResult executeLogicalGraphMutationBatch(
					const storage::LogicalGraphMutationBatch &batch,
					std::optional<storage::LogicalGraphMutationFence> expectedFence) override {
				if (!expectedFence.has_value() || !expectedFence->isValid()) {
					throw std::logic_error("test mutation requires a valid optimistic fence");
				}
				++batch_count;
				last_batch = batch;
				storage::LogicalGraphMutationBatchResult result;
				for (size_t index = 0U; index < batch.node_creates.size(); ++index) {
					result.created_node_ids.push_back(100U + index);
				}
				for (size_t index = 0U; index < batch.edge_creates.size(); ++index) {
					result.created_edge_ids.push_back(200U + index);
				}
				return result;
			}

			size_t batch_count = 0U;
			storage::LogicalGraphMutationBatch last_batch;
		};

		// QueryEngine accepts pre-built logical plans as a public API boundary. This
		// minimal operator lets the tests verify that EXPLAIN fails closed for plan
		// graphs that a parser would never produce.
		class MalformedExplainLogicalOperator final : public logical::LogicalOperator {
		public:
			enum class ExplainChildShape {
				ECS_NONE,
				ECS_NULL,
				ECS_SELF,
				ECS_EXPLICIT,
			};

			explicit MalformedExplainLogicalOperator(
					const ExplainChildShape shape, std::vector<logical::LogicalOperator *> children = {}) :
				shape_(shape), children_(std::move(children)) {}

			[[nodiscard]] logical::LogicalOpType getType() const override {
				return logical::LogicalOpType::LOP_SINGLE_ROW;
			}

			[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override {
				switch (shape_) {
				case ExplainChildShape::ECS_NONE:
					return {};
				case ExplainChildShape::ECS_NULL:
					return {nullptr};
				case ExplainChildShape::ECS_SELF:
					return {const_cast<MalformedExplainLogicalOperator *>(this)};
				case ExplainChildShape::ECS_EXPLICIT:
					return children_;
				}
				throw std::logic_error("unknown malformed EXPLAIN child shape");
			}

			[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> clone() const override {
				return std::make_unique<MalformedExplainLogicalOperator>(ExplainChildShape::ECS_NONE);
			}
			[[nodiscard]] std::string toString() const override { return "MalformedExplainLogicalOperator"; }
			void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {
				throw std::logic_error("malformed EXPLAIN operator does not own children");
			}
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override {
				throw std::logic_error("malformed EXPLAIN operator does not own children");
			}
			void setTestChildren(std::vector<logical::LogicalOperator *> children) {
				shape_ = ExplainChildShape::ECS_EXPLICIT;
				children_ = std::move(children);
			}

		private:
			ExplainChildShape shape_;
			std::vector<logical::LogicalOperator *> children_;
		};

	} // namespace

	TEST(QueryEngineLogicalBackendBoundaryTest, RejectsANullLogicalBackendAtConstruction) {
		std::shared_ptr<storage::LogicalGraphReadBackend> backend;
		EXPECT_THROW(static_cast<void>(QueryEngine(std::move(backend))), std::invalid_argument);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ReadsBoundedNativeCardinalityDuringLogicalPlanOptimization) {
		auto backend = std::make_shared<StatisticsLogicalGraphReadBackend>();
		QueryEngine engine(backend);
		const QueryPlan plan = engine.buildPlan("MATCH (node) RETURN node");
		ASSERT_NE(plan.root, nullptr);
		EXPECT_EQ(backend->statistics_reads, 1U);
		EXPECT_EQ(backend->snapshot_opens, 0U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, UsesCatalogAdjacencyCardinalityToOrderCartesianTraversalInput) {
		auto backend = std::make_shared<StatisticsLogicalGraphReadBackend>();
		backend->statistics = {7U, 0U};
		QueryEngine engine(backend);

		const QueryPlan plan = engine.buildPlan("MATCH (a)-[r]->(b), (c) RETURN a, b, c");
		const auto *project = dynamic_cast<const logical::LogicalProject *>(plan.root.get());
		ASSERT_NE(project, nullptr);
		ASSERT_EQ(project->getChildren().size(), 1U);
		const auto *join = dynamic_cast<const logical::LogicalJoin *>(project->getChildren().front());
		ASSERT_NE(join, nullptr);
		// The test Catalog has seven Nodes and zero Edges. A single-hop traversal
		// therefore estimates to zero rows and is placed on the materialized right.
		EXPECT_NE(dynamic_cast<const logical::LogicalNodeScan *>(join->getLeft()), nullptr);
		EXPECT_NE(dynamic_cast<const logical::LogicalTraversal *>(join->getRight()), nullptr);
		EXPECT_EQ(backend->statistics_reads, 1U);
		EXPECT_EQ(backend->snapshot_opens, 0U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, FallsBackSafelyWhenOptionalCardinalityStatisticsAreUnavailableOrMalformed) {
		auto unavailableBackend = std::make_shared<StatisticsLogicalGraphReadBackend>();
		unavailableBackend->fail_statistics_read = true;
		QueryEngine unavailableEngine(unavailableBackend);
		EXPECT_NE(unavailableEngine.buildPlan("MATCH (node) RETURN node").root, nullptr);
		EXPECT_EQ(unavailableBackend->statistics_reads, 1U);
		EXPECT_EQ(unavailableBackend->snapshot_opens, 0U);

		// An Edge without a Node is a corrupt catalog summary. It must disable the
		// estimate instead of giving the optimizer a fabricated traversal cost.
		auto malformedBackend = std::make_shared<StatisticsLogicalGraphReadBackend>();
		malformedBackend->statistics = {0U, 1U};
		QueryEngine malformedEngine(malformedBackend);
		EXPECT_NE(malformedEngine.buildPlan("MATCH (source)-[edge]->(target) RETURN target").root, nullptr);
		EXPECT_EQ(malformedBackend->statistics_reads, 1U);
		EXPECT_EQ(malformedBackend->snapshot_opens, 0U);

		// A genuinely empty graph remains a valid zero-cardinality traversal
		// estimate, including when the optimized path contains a traversal node.
		auto emptyBackend = std::make_shared<StatisticsLogicalGraphReadBackend>();
		emptyBackend->statistics = {0U, 0U};
		QueryEngine emptyEngine(emptyBackend);
		EXPECT_NE(emptyEngine.buildPlan("MATCH (source)-[edge]->(target) RETURN target").root, nullptr);
		EXPECT_EQ(emptyBackend->statistics_reads, 1U);
		EXPECT_EQ(emptyBackend->snapshot_opens, 0U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, IdentifiesItsReadBackendAndAcceptsAnEmptyTransactionOverlayPlan) {
		auto readBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		auto writeBackend = std::make_shared<RecordingLogicalGraphWriteBackend>();
		auto otherReadBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		QueryEngine engine(readBackend, writeBackend);

		EXPECT_TRUE(engine.usesLogicalReadBackend(*readBackend));
		EXPECT_FALSE(engine.usesLogicalReadBackend(*otherReadBackend));
		QueryPlan emptyPlan;
		EXPECT_TRUE(engine.executePlanWithLogicalTransactionOverlay(
						std::move(emptyPlan), QueryContext{}, *readBackend, *writeBackend)
						.isEmpty());
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ClearsCachedReadPlansWhenBuildingSchemaMutationPlans) {
		auto backend = std::make_shared<UnusedLogicalGraphReadBackend>();
		QueryEngine engine(backend);
		const std::string cachedQuery = "MATCH (node) RETURN node";

		ASSERT_NE(engine.buildPlan(cachedQuery).root, nullptr);
		EXPECT_EQ(engine.getPlanCache().size(), 1U);
		ASSERT_NE(engine.buildPlan("CREATE INDEX ON :CacheInvalidation(name)").root, nullptr);
		EXPECT_EQ(engine.getPlanCache().size(), 0U);

		ASSERT_NE(engine.buildPlan(cachedQuery).root, nullptr);
		EXPECT_EQ(engine.getPlanCache().hits(), 0U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, RejectsIncompleteExecutionBackendBoundariesBeforeOpeningASnapshot) {
		auto readBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		std::shared_ptr<storage::LogicalGraphWriteBackend> missingWriteBackend;
		EXPECT_THROW(static_cast<void>(QueryEngine(readBackend, missingWriteBackend)), std::invalid_argument);

		QueryEngine readOnlyEngine(readBackend);
		QueryPlan emptyPlan;
		EXPECT_TRUE(readOnlyEngine.executePlan(std::move(emptyPlan), QueryContext{}).isEmpty());

		QueryPlan mutationPlan;
		mutationPlan.root = std::make_unique<logical::LogicalSingleRow>();
		mutationPlan.mutatesData = true;
		EXPECT_THROW(static_cast<void>(readOnlyEngine.executePlan(std::move(mutationPlan), QueryContext{})),
				 std::runtime_error);

		QueryPlan schemaPlan;
		schemaPlan.root = std::make_unique<logical::LogicalSingleRow>();
		schemaPlan.mutatesSchema = true;
		EXPECT_THROW(static_cast<void>(readOnlyEngine.executePlan(std::move(schemaPlan), QueryContext{})),
				 std::runtime_error);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, EnforcesExecutionModesForPrebuiltLogicalPlans) {
		auto readBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		auto writeBackend = std::make_shared<RecordingLogicalGraphWriteBackend>();
		QueryEngine engine(readBackend, writeBackend);

		QueryPlan readOnlyMutation;
		readOnlyMutation.root = std::make_unique<logical::LogicalSingleRow>();
		readOnlyMutation.mutatesData = true;
		QueryContext readOnlyContext;
		readOnlyContext.execMode = ExecMode::EM_READ_ONLY;
		EXPECT_THROW(static_cast<void>(engine.executePlan(std::move(readOnlyMutation), readOnlyContext)),
				 std::runtime_error);

		QueryPlan transactionalSchema;
		transactionalSchema.root = std::make_unique<logical::LogicalSingleRow>();
		transactionalSchema.mutatesSchema = true;
		QueryContext readWriteContext;
		readWriteContext.execMode = ExecMode::EM_READ_WRITE;
		EXPECT_THROW(static_cast<void>(engine.executePlan(std::move(transactionalSchema), readWriteContext)),
				 std::runtime_error);

		QueryPlan overlaySchema;
		overlaySchema.root = std::make_unique<logical::LogicalSingleRow>();
		overlaySchema.mutatesSchema = true;
		EXPECT_THROW(static_cast<void>(engine.executePlanWithLogicalTransactionOverlay(
						std::move(overlaySchema), QueryContext{}, *readBackend, *writeBackend)),
				 std::runtime_error);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ValidatesAndExecutesObservabilityPlansWithoutStorageSideEffects) {
		auto readBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		auto writeBackend = std::make_shared<RecordingLogicalGraphWriteBackend>();
		QueryEngine engine(readBackend, writeBackend);

		QueryPlan missingExplainChild;
		missingExplainChild.root = std::make_unique<logical::LogicalExplain>(nullptr);
		EXPECT_THROW(static_cast<void>(engine.executePlan(std::move(missingExplainChild), QueryContext{})),
				 std::logic_error);

		QueryPlan missingProfileChild;
		missingProfileChild.root = std::make_unique<logical::LogicalProfile>(nullptr);
		EXPECT_THROW(static_cast<void>(engine.executePlan(std::move(missingProfileChild), QueryContext{})),
				 std::logic_error);

		LogicalReadExecutionOptions noExplainRows;
		noExplainRows.bounds.maximum_rows = 0U;
		QueryEngine zeroBoundEngine(readBackend, writeBackend, noExplainRows);
		QueryPlan zeroBoundExplain;
		zeroBoundExplain.root = std::make_unique<logical::LogicalExplain>(
				std::make_unique<logical::LogicalSingleRow>());
		EXPECT_THROW(static_cast<void>(zeroBoundEngine.executePlan(std::move(zeroBoundExplain), QueryContext{})),
				 std::invalid_argument);

		QueryPlan overlayExplain;
		overlayExplain.root = std::make_unique<logical::LogicalExplain>(
				std::make_unique<logical::LogicalSingleRow>());
		const QueryResult result = engine.executePlanWithLogicalTransactionOverlay(
				std::move(overlayExplain), QueryContext{}, *readBackend, *writeBackend);
		EXPECT_EQ(result.getColumns(), (std::vector<std::string>{"operator", "details"}));
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(std::get<std::string>(result.getRows().front().at("operator").asPrimitive().getVariant()),
				  "SingleRow");
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ExplainRejectsMalformedAndOverBoundPrebuiltPlanTrees) {
		auto backend = std::make_shared<UnusedLogicalGraphReadBackend>();
		QueryEngine engine(backend);
		const auto executeExplain = [&engine](std::unique_ptr<logical::LogicalOperator> inner) {
			QueryPlan plan;
			plan.root = std::make_unique<logical::LogicalExplain>(std::move(inner));
			return engine.executePlan(std::move(plan), QueryContext{});
		};

		EXPECT_THROW(static_cast<void>(executeExplain(std::make_unique<MalformedExplainLogicalOperator>(
					 MalformedExplainLogicalOperator::ExplainChildShape::ECS_NULL))),
				 std::logic_error);
		EXPECT_THROW(static_cast<void>(executeExplain(std::make_unique<MalformedExplainLogicalOperator>(
					 MalformedExplainLogicalOperator::ExplainChildShape::ECS_SELF))),
				 std::logic_error);

		MalformedExplainLogicalOperator firstLeaf(MalformedExplainLogicalOperator::ExplainChildShape::ECS_NONE);
		MalformedExplainLogicalOperator secondLeaf(MalformedExplainLogicalOperator::ExplainChildShape::ECS_NONE);

		LogicalReadExecutionOptions oneRowOptions;
		oneRowOptions.bounds.maximum_rows = 1U;
		QueryEngine oneRowEngine(backend, oneRowOptions);
		QueryPlan tooManyPending;
		tooManyPending.root = std::make_unique<logical::LogicalExplain>(
				std::make_unique<MalformedExplainLogicalOperator>(
						MalformedExplainLogicalOperator::ExplainChildShape::ECS_EXPLICIT,
						std::vector<logical::LogicalOperator *>{&firstLeaf, &secondLeaf}));
		EXPECT_THROW(static_cast<void>(oneRowEngine.executePlan(std::move(tooManyPending), QueryContext{})),
				 std::runtime_error);

		QueryPlan tooManyRows;
		tooManyRows.root = std::make_unique<logical::LogicalExplain>(
				std::make_unique<MalformedExplainLogicalOperator>(
						MalformedExplainLogicalOperator::ExplainChildShape::ECS_EXPLICIT,
						std::vector<logical::LogicalOperator *>{&firstLeaf}));
		EXPECT_THROW(static_cast<void>(oneRowEngine.executePlan(std::move(tooManyRows), QueryContext{})),
				 std::runtime_error);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ExplainCapsRenderedIndentForDeepPrebuiltPlanTrees) {
		auto backend = std::make_shared<UnusedLogicalGraphReadBackend>();
		QueryEngine engine(backend);
		std::vector<std::unique_ptr<MalformedExplainLogicalOperator>> chain;
		chain.reserve(130U);
		for (size_t index = 0U; index < 130U; ++index) {
			chain.push_back(std::make_unique<MalformedExplainLogicalOperator>(
					MalformedExplainLogicalOperator::ExplainChildShape::ECS_NONE));
		}
		for (size_t index = 0U; index + 1U < chain.size(); ++index) {
			chain[index]->setTestChildren({chain[index + 1U].get()});
		}

		QueryPlan plan;
		plan.root = std::make_unique<logical::LogicalExplain>(std::move(chain.front()));
		const QueryResult result = engine.executePlan(std::move(plan), QueryContext{});
		ASSERT_EQ(result.rowCount(), 130U);
		const std::string &deepest =
				std::get<std::string>(result.getRows().back().at("operator").asPrimitive().getVariant());
		EXPECT_TRUE(deepest.starts_with(std::string(256U, ' ') + "... "));
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, RejectsUnknownQueryLanguageBeforeParsing) {
		auto backend = std::make_shared<UnusedLogicalGraphReadBackend>();
		QueryEngine engine(backend);
		EXPECT_THROW(static_cast<void>(engine.execute("RETURN 1", static_cast<Language>(77))), std::runtime_error);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ExplainIsSnapshotFreeAndMayInspectAWriteInReadOnlyMode) {
		auto backend = std::make_shared<UnusedLogicalGraphReadBackend>();
		QueryEngine engine(backend);
		QueryContext context;
		context.execMode = ExecMode::EM_READ_ONLY;

		const QueryResult result = engine.execute("EXPLAIN CREATE (n:Person {name: 'Ada'})", context);

		EXPECT_EQ(result.getColumns(), (std::vector<std::string>{"operator", "details"}));
		ASSERT_FALSE(result.isEmpty());
		EXPECT_TRUE(result.getRows().front().contains("operator"));
		EXPECT_TRUE(result.getRows().front().contains("details"));
	}

	TEST(QueryEngineLogicalBackendBoundaryTest,
		 AdministrationMutationWaitsForTheCompleteCypherResultPipelineBeforePublishing) {
		auto backend = std::make_shared<RecordingAdministrationLogicalGraphBackend>();
		QueryEngine engine(backend);

		// `value` is an Integer, so this legal YIELD/WHERE plan fails while its
		// result pipeline is materialized. The deferred configuration write must
		// remain unreachable after the public CALL reports that type error.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CALL dbms.setConfig('query.timeout_ms', 41) YIELD value WHERE value RETURN value")),
				 std::runtime_error);
		EXPECT_EQ(backend->configuration_writes, 0U);
		EXPECT_FALSE(backend->configurations.contains("query.timeout_ms"));

		const QueryResult committed = engine.execute("CALL dbms.setConfig('query.timeout_ms', 41)");
		ASSERT_EQ(committed.rowCount(), 1U);
		EXPECT_EQ(backend->configuration_writes, 1U);
		ASSERT_TRUE(backend->configurations.contains("query.timeout_ms"));
		EXPECT_EQ(backend->configurations.at("query.timeout_ms"), PropertyValue(int64_t{41}));
	}

	TEST(QueryEngineLogicalBackendBoundaryTest,
		 AdministrationMutationChecksAnExpiredGuardImmediatelyBeforePublishing) {
		auto backend = std::make_shared<RecordingAdministrationLogicalGraphBackend>();
		backend->snapshot_open_delay = std::chrono::milliseconds(75);
		QueryEngine engine(backend);
		QueryGuard guard(50, 0);
		// Keep the dispatch and pre-snapshot checks on the hot path. The delayed
		// snapshot then crosses the deadline before the final commit boundary.
		for (size_t index = 0U; index < 1022U; ++index) {
			guard.check();
		}
		QueryContext context;
		context.guard = &guard;

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CALL dbms.setConfig('query.timeout_ms', 41) YIELD value RETURN value", context)),
				 QueryTimeoutException);
		EXPECT_EQ(backend->configuration_writes, 0U);
		EXPECT_FALSE(backend->configurations.contains("query.timeout_ms"));
	}

	TEST(QueryEngineLogicalBackendBoundaryTest,
		 AdministrationObservabilityProceduresExcludeThemselvesAndCommitOnlyAfterTheirResult) {
		auto backend = std::make_shared<RecordingAdministrationLogicalGraphBackend>();
		backend->configurations.emplace("query.slow_log.enabled", PropertyValue(true));
		backend->configurations.emplace("query.slow_log.threshold_ms", PropertyValue(int64_t{0}));
		backend->runtime_statistics = {{"storage", "pages", PropertyValue(int64_t{7})}};
		QueryEngine engine(backend);

		static_cast<void>(engine.execute("RETURN 1 AS value"));
		const QueryResult slowQueries = engine.execute("CALL dbms.listSlowQueries()");
		ASSERT_EQ(slowQueries.rowCount(), 1U);
		EXPECT_EQ(std::get<std::string>(slowQueries.getRows().front().at("logicalPlan").asPrimitive().getVariant()),
				  "Project");
		EXPECT_TRUE(std::get<bool>(slowQueries.getRows().front().at("succeeded").asPrimitive().getVariant()));

		const QueryResult statistics = engine.execute("CALL dbms.showStats()");
		EXPECT_EQ(statistics.rowCount(), 5U);
		EXPECT_EQ(backend->statistics_resets, 0U);

		const QueryResult reset = engine.execute("CALL dbms.resetStats()");
		ASSERT_EQ(reset.rowCount(), 1U);
		EXPECT_EQ(backend->statistics_resets, 1U);
		const QueryResult afterReset = engine.execute("CALL dbms.showStats()");
		ASSERT_EQ(afterReset.rowCount(), 5U);
		for (const QueryResult::Row &row: afterReset.getRows()) {
			if (std::get<std::string>(row.at("category").asPrimitive().getVariant()) == "query") {
				EXPECT_EQ(std::get<int64_t>(row.at("value").asPrimitive().getVariant()), 0);
			}
		}

		const QueryResult cleared = engine.execute("CALL dbms.clearSlowQueries()");
		ASSERT_EQ(cleared.rowCount(), 1U);
		EXPECT_TRUE(engine.execute("CALL dbms.listSlowQueries()").isEmpty());
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, BytesFunctionAcceptsHexListsAndBinaryParameters) {
		auto backend = std::make_shared<CountingLogicalGraphReadBackend>();
		QueryEngine engine(backend);
		QueryContext context;
		context.parameters.emplace("payload", PropertyValue(PropertyValue::Bytes{
				std::byte{0x10}, std::byte{0x80}, std::byte{0xFF}}));

		const QueryResult result = engine.execute(
				"RETURN bytes('0x00ff') AS hex, bytes([1, 128, 255]) AS list, bytes($payload) AS parameter",
				context);

		ASSERT_EQ(result.rowCount(), 1U);
		const auto &row = result.getRows().front();
		ASSERT_TRUE(row.at("hex").isPrimitive());
		ASSERT_TRUE(row.at("list").isPrimitive());
		ASSERT_TRUE(row.at("parameter").isPrimitive());
		EXPECT_EQ(row.at("hex").asPrimitive().getBytes(),
				(PropertyValue::Bytes{std::byte{0x00}, std::byte{0xFF}}));
		EXPECT_EQ(row.at("list").asPrimitive().getBytes(),
				(PropertyValue::Bytes{std::byte{0x01}, std::byte{0x80}, std::byte{0xFF}}));
		EXPECT_EQ(row.at("parameter").asPrimitive().getBytes(), context.parameters.at("payload").getBytes());
		EXPECT_EQ(backend->snapshot_opens, 1U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, BytesFunctionRejectsMalformedInputs) {
		auto backend = std::make_shared<CountingLogicalGraphReadBackend>();
		QueryEngine engine(backend);
		for (const std::string &query: {"RETURN bytes('0')", "RETURN bytes('zz')", "RETURN bytes([256])"}) {
			EXPECT_THROW(static_cast<void>(engine.execute(query)), std::invalid_argument);
		}
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, EntityIntrospectionListsRespectTheNativeResultBound) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		storage::LogicalGraphNode node;
		node.node_id = 1U;
		node.labels = {"First", "Second"};
		node.properties = {{"first", PropertyValue(1)}, {"second", PropertyValue(2)}};
		backend->state->nodes.emplace(node.node_id, std::move(node));
		LogicalReadExecutionOptions options;
		options.bounds.maximum_rows = 1U;
		QueryEngine engine(backend, options);

		// MATCH itself emits one row. The failure must come from the direct native
		// entity-function materialization rather than the ordinary row bound.
		EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n) RETURN labels(n) AS labels")), std::runtime_error);
		EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n) RETURN keys(n) AS keys")), std::runtime_error);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ScalarFunctionsRespectTheNativeListAndArgumentBounds) {
		auto backend = std::make_shared<CountingLogicalGraphReadBackend>();
		LogicalReadExecutionOptions options;
		options.bounds.maximum_rows = 2U;
		QueryEngine engine(backend, options);

		// A parameter bypasses List-literal validation, so this reaches the scalar
		// function boundary and proves copied List inputs are bounded there too.
		QueryContext context;
		context.parameters.emplace(
				"items", PropertyValue(std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)}));
		EXPECT_THROW(static_cast<void>(engine.execute("RETURN tail($items) AS tail", context)), std::runtime_error);

		// split() creates its List incrementally and must fail before materializing
		// more entries than the native query policy permits.
		EXPECT_THROW(static_cast<void>(engine.execute("RETURN split('a,b,c', ',') AS pieces")), std::runtime_error);
		EXPECT_THROW(static_cast<void>(engine.execute("RETURN coalesce(null, null, 'fallback') AS value")),
				 std::runtime_error);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ProfileDispatchesItsInnerReadPlanAndAddsOneSummaryRow) {
		auto backend = std::make_shared<CountingLogicalGraphReadBackend>();
		QueryEngine engine(backend);

		const QueryResult result = engine.execute("PROFILE RETURN 42 AS answer");

		ASSERT_EQ(backend->snapshot_opens, 1U);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(std::get<int64_t>(result.getRows().front().at("answer").asPrimitive().getVariant()), 42);
		EXPECT_GT(result.getDuration(), -1.0);
		EXPECT_EQ(std::get<std::string>(result.getRows().back().at("phase").asPrimitive().getVariant()),
				  "logical_plan");
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ProfileDispatchesItsInnerCreatePlanThroughOneAtomicBatch) {
		auto readBackend = std::make_shared<CountingLogicalGraphReadBackend>();
		auto writeBackend = std::make_shared<RecordingLogicalGraphWriteBackend>();
		QueryEngine engine(readBackend, writeBackend);

		const QueryResult result = engine.execute("PROFILE CREATE (n:Person {name: 'Ada'}) RETURN n");

		ASSERT_EQ(readBackend->snapshot_opens, 1U);
		ASSERT_EQ(writeBackend->batch_count, 1U);
		ASSERT_EQ(writeBackend->last_batch.node_creates.size(), 1U);
		EXPECT_EQ(writeBackend->last_batch.node_creates.front().labels, (std::vector<std::string>{"Person"}));
		EXPECT_EQ(result.rowCount(), 2U);
		EXPECT_TRUE(result.getRows().front().at("n").isLogicalNode());
		EXPECT_EQ(std::get<std::string>(result.getRows().back().at("phase").asPrimitive().getVariant()),
				  "logical_plan");
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ProfileUsesTheLogicalTransactionOverlayInsteadOfTheBaseBackend) {
		auto baseBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		auto overlayReadBackend = std::make_shared<CountingLogicalGraphReadBackend>();
		auto overlayWriteBackend = std::make_shared<RecordingLogicalGraphWriteBackend>();
		QueryEngine engine(baseBackend, overlayWriteBackend);
		QueryPlan plan = engine.buildPlan("PROFILE RETURN 7 AS answer");

		const QueryResult result = engine.executePlanWithLogicalTransactionOverlay(
				std::move(plan), QueryContext{}, *overlayReadBackend, *overlayWriteBackend);

		EXPECT_EQ(overlayReadBackend->snapshot_opens, 1U);
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(std::get<int64_t>(result.getRows().front().at("answer").asPrimitive().getVariant()), 7);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, LogicalVectorBoundaryDefaultsRemainExplicitAndFailClosed) {
		storage::LogicalGraphVectorSearchRequest request{"embedding", {1.0F, 2.0F}, 3U,
				storage::LogicalGraphVectorSearchMode::LGVSM_EXACT, 4U, 5U, 6U, 7U};
		storage::LogicalGraphVectorSearchRequest sameRequest = request;
		class NoopCancellationCheckpoint final : public storage::CancellationCheckpoint {
		public:
			void check() const override {}
		} cancellation;
		sameRequest.cancellation = &cancellation;
		EXPECT_EQ(request, sameRequest);
		const auto expectDifferentSearchRequest = [&request](const auto change) {
			auto different = request;
			change(different);
			EXPECT_NE(request, different);
		};
		expectDifferentSearchRequest([](auto &value) { value.index_name = "other_embedding"; });
		expectDifferentSearchRequest([](auto &value) { value.query_vector.back() = 3.0F; });
		expectDifferentSearchRequest([](auto &value) { value.top_k = 4U; });
		expectDifferentSearchRequest([](auto &value) { value.mode = storage::LogicalGraphVectorSearchMode::LGVSM_AUTO; });
		expectDifferentSearchRequest([](auto &value) { value.maximum_mapping_entries = 8U; });
		expectDifferentSearchRequest([](auto &value) { value.maximum_raw_vector_reads = 9U; });
		expectDifferentSearchRequest([](auto &value) { value.candidate_search_budget = 10U; });
		expectDifferentSearchRequest([](auto &value) { value.candidate_refinement_budget = 11U; });

		storage::LogicalGraphVectorTrainingRequest training{"embedding", 9U, 10U};
		storage::LogicalGraphVectorTrainingRequest sameTraining = training;
		sameTraining.cancellation = &cancellation;
		EXPECT_EQ(training, sameTraining);
		const auto expectDifferentTrainingRequest = [&training](const auto change) {
			auto different = training;
			change(different);
			EXPECT_NE(training, different);
		};
		expectDifferentTrainingRequest([](auto &value) { value.index_name = "other_embedding"; });
		expectDifferentTrainingRequest([](auto &value) { value.maximum_records = 11U; });
		expectDifferentTrainingRequest([](auto &value) { value.maximum_pages = 12U; });

		class DefaultVectorSnapshot final : public storage::LogicalGraphVectorSearchSnapshot {
		public:
			[[nodiscard]] storage::LogicalGraphNode readNode(uint64_t) const override { return {}; }
			[[nodiscard]] storage::LogicalGraphEdge readEdge(uint64_t) const override { return {}; }
			void visitNodes(size_t, const std::function<bool(const storage::LogicalGraphNode &)> &) const override {}
			[[nodiscard]] storage::LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override { return {}; }
			[[nodiscard]] storage::LogicalGraphAdjacencyPage scanAdjacentEdges(
					uint64_t, storage::LogicalGraphReadDirection,
					std::optional<storage::LogicalGraphAdjacencyContinuation>, size_t) const override {
				return {};
			}
			void visitEdges(size_t, const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {}
			void visitAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection, size_t,
					const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {}
			[[nodiscard]] storage::LogicalGraphVectorSearchResult
			searchLogicalVector(const storage::LogicalGraphVectorSearchRequest &) const override {
				return {};
			}
		} snapshot;
		EXPECT_FALSE(snapshot.describeLogicalVectorIndex(request).has_value());
		EXPECT_THROW(static_cast<void>(snapshot.scoreLogicalVectorExactOverlayCandidate(
					 request, storage::LogicalGraphVectorIndexDescriptor{}, storage::LogicalGraphNode{})),
				 std::logic_error);

		DefaultLogicalVectorMaintenanceBackend backend;
		const storage::LogicalGraphVectorTrainingResult result = backend.trainLogicalVector(training);
		EXPECT_EQ(result, (storage::LogicalGraphVectorTrainingResult{7U, true, false}));
		EXPECT_EQ(backend.commit_count, 1U);
		backend.return_null_preparation = true;
		EXPECT_THROW(static_cast<void>(backend.trainLogicalVector(training)), std::logic_error);
		EXPECT_EQ(backend.commit_count, 1U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorTrainingSupportsTheBoundedScalarOutputPipeline) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		QueryEngine engine(backend);

		// A result-pipeline failure occurs before the prepared maintenance operation
		// commits, so it cannot publish a derived generation behind an error.
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CALL db.index.vector.train('memory_embedding') YIELD status RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);
		EXPECT_EQ(backend->training_calls, 0U);

		const QueryResult result = engine.execute(
				"CALL db.index.vector.train('memory_embedding') YIELD status "
				"WHERE status = 'Success' RETURN DISTINCT status AS training_status "
				"ORDER BY training_status DESC SKIP 0 LIMIT 1");

		ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"training_status"}));
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("training_status").asPrimitive(), PropertyValue("Success"));
		EXPECT_EQ(backend->training_calls, 1U);
		EXPECT_EQ(backend->last_training_request.index_name, "memory_embedding");
		EXPECT_GT(backend->last_training_request.maximum_records, 0U);
		EXPECT_GT(backend->last_training_request.maximum_pages, 0U);
		// The rejected RETURN has already opened a read snapshot to evaluate its
		// output pipeline; it still never commits maintenance.
		EXPECT_EQ(backend->search_snapshot_opens, 2U);

		backend->training_result = {0U, false, false};
		const QueryResult skipped = engine.execute("CALL db.index.vector.train('empty_embedding')");
		ASSERT_EQ(skipped.getColumns(), (std::vector<std::string>{"status"}));
		ASSERT_EQ(skipped.rowCount(), 1U);
		EXPECT_EQ(skipped.getRows().front().at("status").asPrimitive(), PropertyValue("Skipped (Empty Data)"));
		EXPECT_EQ(backend->training_calls, 2U);
		EXPECT_EQ(backend->search_snapshot_opens, 2U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorTrainingRejectsReadOnlyAndUnboundedRequestsBeforeMaintenance) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		QueryEngine readOnlyEngine(backend);
		QueryContext readOnly;
		readOnly.execMode = ExecMode::EM_READ_ONLY;
		EXPECT_THROW(static_cast<void>(readOnlyEngine.execute("CALL db.index.vector.train('memory_embedding')", readOnly)),
					 std::runtime_error);
		EXPECT_EQ(backend->training_calls, 0U);
		EXPECT_EQ(backend->search_snapshot_opens, 0U);

		LogicalReadExecutionOptions zeroBoundOptions;
		zeroBoundOptions.maximum_vector_training_records = 0U;
		QueryEngine zeroBoundEngine(backend, zeroBoundOptions);
		EXPECT_THROW(static_cast<void>(zeroBoundEngine.execute("CALL db.index.vector.train('memory_embedding')")),
					 std::invalid_argument);
		EXPECT_EQ(backend->training_calls, 0U);
		EXPECT_EQ(backend->search_snapshot_opens, 0U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, TimedOutVectorTrainingDoesNotStartMaintenance) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		QueryEngine engine(backend);
		QueryGuard guard(1, 0);
		for (size_t index = 0U; index < 1023U; ++index) {
			guard.check();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		QueryContext context;
		context.guard = &guard;

		EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.train('memory_embedding')", context)),
				 QueryTimeoutException);
		EXPECT_EQ(backend->training_calls, 0U);
		EXPECT_EQ(backend->search_snapshot_opens, 0U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorTrainingPropagatesCancellationIntoMaintenanceWork) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		backend->state->training_prepare_delay = std::chrono::milliseconds(2);
		backend->state->training_cancellation_checks_before_throw = 1024U;
		QueryEngine engine(backend);

		QueryGuard guard(1, 0);
		QueryContext context;
		context.guard = &guard;
		EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.train('memory_embedding')", context)),
					 QueryTimeoutException);
		EXPECT_EQ(backend->training_calls, 0U);
		// The deadline crosses during preparation, and the maintenance checkpoint
		// itself observes it on the first periodic sampling boundary.
		EXPECT_EQ(backend->state->training_cancellation_checks, 1023U);
		EXPECT_EQ(backend->training_preparations, 1U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorTrainingDoesNotReportTimeoutAfterSuccessfulMaintenance) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		// Let the deadline expire only after the immediate pre-publication check.
		// A late check after commit would incorrectly report a durable result as a
		// timeout.
		backend->state->training_commit_delay = std::chrono::milliseconds(20);
		QueryEngine engine(backend);

		QueryGuard guard(10, 0);
		QueryContext context;
		context.guard = &guard;
		const QueryResult result = engine.execute("CALL db.index.vector.train('memory_embedding')", context);

		ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"status"}));
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("status").asPrimitive(), PropertyValue("Success"));
		EXPECT_EQ(backend->training_calls, 1U);
		EXPECT_EQ(backend->state->training_cancellation_checks, 0U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorTrainingChecksTheGuardImmediatelyBeforePublication) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		backend->state->training_preview_delay = std::chrono::milliseconds(2);
		QueryEngine engine(backend);

		// The dispatcher and training branch consume checks 1022 and 1023. The
		// deadline then expires while previewing the prepared operation, before
		// its only publication point. A periodic-only check would commit here.
		QueryGuard guard(1, 0);
		for (size_t index = 0U; index < 1021U; ++index) {
			guard.check();
		}
		QueryContext context;
		context.guard = &guard;

		EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.train('memory_embedding')", context)),
					 QueryTimeoutException);
		EXPECT_EQ(backend->training_calls, 0U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ShortestPathProcedureUsesTheLogicalReadSnapshot) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		backend->state->nodes.emplace(1U, storage::LogicalGraphNode{1U, {"Person"}});
		backend->state->nodes.emplace(2U, storage::LogicalGraphNode{2U, {"Person"}});
		backend->state->nodes.emplace(3U, storage::LogicalGraphNode{3U, {"Person"}});
		backend->state->edges = {{11U, 1U, 2U, "KNOWS"}, {12U, 2U, 3U, "KNOWS"}};
		QueryEngine engine(backend);

		const QueryResult result = engine.execute("CALL algo.shortestPath(1, 3, 2)");
		ASSERT_EQ(result.getColumns(), (std::vector<std::string>{"step", "node", "path_length"}));
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("step").asPrimitive(), PropertyValue(int64_t{0}));
		EXPECT_EQ(result.getRows()[2].at("path_length").asPrimitive(), PropertyValue(int64_t{3}));
		EXPECT_EQ(result.getRows()[0].at("node").asLogicalNode().node_id, 1U);
		EXPECT_EQ(result.getRows()[2].at("node").asLogicalNode().node_id, 3U);
		EXPECT_EQ(backend->search_snapshot_opens, 0U);

		const QueryResult tooShallow = engine.execute("CALL algo.shortestPath(1, 3, 1)");
		EXPECT_TRUE(tooShallow.isEmpty());
		EXPECT_THROW(static_cast<void>(engine.execute("CALL algo.shortestPath(1, 3, 0)")), std::invalid_argument);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ShortestPathProcedureRejectsNonMonotonicAdjacencyPages) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		backend->state->nodes.emplace(1U, storage::LogicalGraphNode{1U, {"Person"}});
		backend->state->nodes.emplace(2U, storage::LogicalGraphNode{2U, {"Person"}});
		backend->state->nodes.emplace(3U, storage::LogicalGraphNode{3U, {"Person"}});
		backend->state->nodes.emplace(4U, storage::LogicalGraphNode{4U, {"Person"}});
		backend->state->edges = {{11U, 1U, 2U, "KNOWS"}, {12U, 1U, 3U, "KNOWS"}, {13U, 2U, 4U, "KNOWS"}};
		backend->state->return_nonmonotonic_adjacency_page = true;
		QueryEngine engine(backend);

		EXPECT_THROW(static_cast<void>(engine.execute("CALL algo.shortestPath(1, 4, 2)")), std::runtime_error);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ShortestPathRejectsInvalidReadBudgetsBeforeTraversal) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		backend->state->nodes.emplace(1U, storage::LogicalGraphNode{1U, {"Person"}});
		backend->state->nodes.emplace(2U, storage::LogicalGraphNode{2U, {"Person"}});
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		const logical::LogicalCallProcedure call("algo.shortestPath",
				{PropertyValue(int64_t{1}), PropertyValue(int64_t{2})});
		const LogicalReadExecutionOptions defaults;
		const auto execute = [&runtime, &call](const LogicalReadExecutionOptions &options) {
			return runtime.execute(call, options.bounds, options.maximum_vector_mapping_entries,
					options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
					options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
					options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};

		auto invalid = defaults;
		invalid.bounds.maximum_node_records = 0U;
		EXPECT_THROW(static_cast<void>(execute(invalid)), std::invalid_argument);
		invalid = defaults;
		invalid.bounds.maximum_edges_per_node = 0U;
		EXPECT_THROW(static_cast<void>(execute(invalid)), std::invalid_argument);
		invalid = defaults;
		invalid.bounds.maximum_adjacency_page_records = 0U;
		EXPECT_THROW(static_cast<void>(execute(invalid)), std::invalid_argument);
		invalid = defaults;
		invalid.bounds.maximum_property_entries = 0U;
		EXPECT_THROW(static_cast<void>(execute(invalid)), std::invalid_argument);
		invalid = defaults;
		invalid.bounds.maximum_rows = 0U;
		EXPECT_THROW(static_cast<void>(execute(invalid)), std::invalid_argument);

		auto overflowing = defaults;
		overflowing.bounds.maximum_node_records = std::numeric_limits<size_t>::max();
		overflowing.bounds.maximum_edges_per_node = 2U;
		EXPECT_THROW(static_cast<void>(execute(overflowing)), std::overflow_error);

		backend->state->nodes.erase(2U);
		EXPECT_TRUE(execute(defaults).isEmpty());
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, TransactionOverlayUsesOnlyManifestApprovedPinnedProcedures) {
		auto baseBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		auto overlayBackend = std::make_shared<RecordingVectorProcedureBackend>();
		auto writeBackend = std::make_shared<RecordingLogicalGraphWriteBackend>();
		overlayBackend->state->nodes.emplace(1U, storage::LogicalGraphNode{1U, {"Person"}});
		overlayBackend->state->nodes.emplace(2U, storage::LogicalGraphNode{2U, {"Person"}});
		overlayBackend->state->nodes.emplace(3U, storage::LogicalGraphNode{3U, {"Person"}});
		overlayBackend->state->edges = {{11U, 1U, 2U, "KNOWS"}, {12U, 2U, 3U, "KNOWS"}};
		QueryEngine engine(baseBackend, writeBackend);

		const QueryResult path = engine.executePlanWithLogicalTransactionOverlay(
				engine.buildPlan("CALL algo.shortestPath(1, 3, 2)"), QueryContext{}, *overlayBackend, *writeBackend);
		ASSERT_EQ(path.rowCount(), 3U);
		EXPECT_EQ(path.getRows().front().at("node").asLogicalNode().node_id, 1U);
		EXPECT_EQ(path.getRows().back().at("node").asLogicalNode().node_id, 3U);

		EXPECT_THROW(static_cast<void>(engine.executePlanWithLogicalTransactionOverlay(
							 engine.buildPlan("CALL gds.graph.project('transaction-local', {})"), QueryContext{},
							 *overlayBackend, *writeBackend)),
					 std::runtime_error);
		EXPECT_THROW(static_cast<void>(engine.executePlanWithLogicalTransactionOverlay(
						 engine.buildPlan("CALL db.index.vector.train('memory_embedding')"), QueryContext{},
						 *overlayBackend, *writeBackend)),
					 std::runtime_error);
		overlayBackend->state->search_result.hits = {{1U, 0.0F}};
		const QueryResult vector = engine.executePlanWithLogicalTransactionOverlay(
				engine.buildPlan("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])"), QueryContext{},
				*overlayBackend, *writeBackend);
		ASSERT_EQ(vector.rowCount(), 1U);
		EXPECT_EQ(vector.getRows().front().at("node").asLogicalNode().node_id, 1U);
		EXPECT_EQ(overlayBackend->search_snapshot_opens, 1U);
		// Training remains autocommit-only: an explicit graph transaction cannot
		// publish a separate derived Vector generation.
		EXPECT_EQ(overlayBackend->training_calls, 0U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, TransactionOverlayRejectsEveryAutocommitOnlyProcedureBeforeBackendAccess) {
		auto baseBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		auto overlayBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		auto writeBackend = std::make_shared<RecordingLogicalGraphWriteBackend>();
		QueryEngine engine(baseBackend, writeBackend);

		// The manifest boundary is evaluated before a procedure can open the
		// overlay snapshot or reach administration/maintenance services. These are
		// all currently declared AUTOCOMMIT_ONLY native procedures.
		ASSERT_EQ(nativeProcedureQueryNames(), nativeProcedureManifestNames());
		size_t checkedProcedures = 0U;
		for (const NativeProcedureQuery &query: nativeProcedureQueries()) {
			const NativeProcedureManifestEntry *manifest = findNativeProcedureManifestEntry(query.name);
			ASSERT_NE(manifest, nullptr);
			if (nativeProcedureTransactionMode(manifest->kind) != NativeProcedureTransactionMode::NPTM_AUTOCOMMIT_ONLY) {
				continue;
			}
			EXPECT_THROW(static_cast<void>(engine.executePlanWithLogicalTransactionOverlay(
						 engine.buildPlan(query.text()), QueryContext{}, *overlayBackend, *writeBackend)),
					 std::runtime_error) << query.name;
			++checkedProcedures;
		}
		EXPECT_EQ(checkedProcedures, 8U);
		EXPECT_EQ(writeBackend->batch_count, 0U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, EveryNativeProcedureTimesOutBeforeBackendAccess) {
		auto backend = std::make_shared<UnusedLogicalGraphReadBackend>();
		QueryEngine engine(backend);

		// Keep this one valid query per manifest entry. An expired guard must win
		// before the procedure can require a specific backend capability, open a
		// snapshot, or publish an administrative/maintenance side effect.
		ASSERT_EQ(nativeProcedureQueryNames(), nativeProcedureManifestNames());

		for (const NativeProcedureQuery &query: nativeProcedureQueries()) {
			QueryGuard guard(1, 0);
			for (size_t index = 0U; index < 1023U; ++index) {
				guard.check();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			QueryContext context;
			context.guard = &guard;
			EXPECT_THROW(static_cast<void>(engine.execute(query.text(), context)), QueryTimeoutException) << query.name;
		}
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, NativeProcedureEntryChecksAnExpiredGuardWithoutPeriodicPriming) {
		auto backend = std::make_shared<UnusedLogicalGraphReadBackend>();
		QueryEngine engine(backend);

		// Do not prime QueryGuard's periodic counter. The common procedure entry
		// must still reject this naturally expired deadline before dispatching to
		// any native backend, regardless of which manifest entry is selected.
		ASSERT_EQ(nativeProcedureQueryNames(), nativeProcedureManifestNames());

		for (const NativeProcedureQuery &query: nativeProcedureQueries()) {
			QueryGuard guard(1, 0);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			QueryContext context;
			context.guard = &guard;
			EXPECT_THROW(static_cast<void>(engine.execute(query.text(), context)), QueryTimeoutException) << query.name;
		}
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, TransactionOverlayProcedureTimesOutBeforeOpeningItsSnapshot) {
		auto baseBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		auto overlayBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		auto writeBackend = std::make_shared<RecordingLogicalGraphWriteBackend>();
		QueryEngine engine(baseBackend, writeBackend);

		// Prime the guard just below its periodic check threshold, then make its
		// deadline expire. The procedure must check it before opening the overlay
		// snapshot, which prevents a timed-out read from touching transaction state.
		QueryGuard guard(1, 0);
		for (size_t index = 0U; index < 1023U; ++index) {
			guard.check();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		QueryContext context;
		context.guard = &guard;
		EXPECT_THROW(static_cast<void>(engine.executePlanWithLogicalTransactionOverlay(
					 engine.buildPlan("CALL algo.shortestPath(1, 2, 1)"), context, *overlayBackend, *writeBackend)),
					 QueryTimeoutException);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, TransactionOverlayVectorProcedureTimesOutBeforeOpeningItsSnapshot) {
		auto baseBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		auto overlayBackend = std::make_shared<RecordingVectorProcedureBackend>();
		auto writeBackend = std::make_shared<RecordingLogicalGraphWriteBackend>();
		QueryEngine engine(baseBackend, writeBackend);

		QueryGuard guard(1, 0);
		for (size_t index = 0U; index < 1023U; ++index) {
			guard.check();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		QueryContext context;
		context.guard = &guard;
		EXPECT_THROW(static_cast<void>(engine.executePlanWithLogicalTransactionOverlay(
					 engine.buildPlan("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])"), context,
					 *overlayBackend, *writeBackend)),
					 QueryTimeoutException);
		EXPECT_EQ(overlayBackend->search_snapshot_opens, 0U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorProcedurePropagatesCancellationIntoTheSearchWork) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		backend->state->cancellation_checks_before_throw = 1023U;
		backend->state->search_cancellation_initial_delay = std::chrono::milliseconds(2);
		QueryEngine engine(backend);

		QueryGuard guard(1, 0);
		QueryContext context;
		context.guard = &guard;
		EXPECT_THROW(static_cast<void>(engine.executePlan(engine.buildPlan(
					 "CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])"),
				 context)),
				QueryTimeoutException);
		EXPECT_EQ(backend->search_snapshot_opens, 1U);
		// The immediate entry guard admits this initially live query. The backend
		// then crosses the deadline and receives cancellation on its 1023rd callback:
		// one periodic check protected Vector-snapshot acquisition beforehand.
		EXPECT_EQ(backend->state->search_cancellation_checks, 1023U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, TransactionLocalGdsProcedureTimesOutBeforeOpeningItsSnapshot) {
		auto baseBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		auto overlayBackend = std::make_shared<UnusedLogicalGraphReadBackend>();
		auto writeBackend = std::make_shared<RecordingLogicalGraphWriteBackend>();
		gds::LogicalProjectionCatalog transactionCatalog;
		QueryEngine engine(baseBackend, writeBackend);

		QueryGuard guard(1, 0);
		for (size_t index = 0U; index < 1023U; ++index) {
			guard.check();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		QueryContext context;
		context.guard = &guard;
		EXPECT_THROW(static_cast<void>(engine.executePlanWithLogicalTransactionOverlay(
					 engine.buildPlan("CALL gds.graph.project('timed_out', {})"), context, *overlayBackend, *writeBackend,
					 &transactionCatalog)),
				 QueryTimeoutException);
		EXPECT_FALSE(transactionCatalog.exists("timed_out"));
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, GdsProjectionCancellationDoesNotPublishAPartialCatalogEntry) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		for (uint64_t nodeId = 1U; nodeId <= 32U; ++nodeId) {
			backend->state->nodes.emplace(nodeId, storage::LogicalGraphNode{nodeId, {"ProjectionNode"}});
		}
		// Make the snapshot scan cross the deadline after procedure dispatch, so
		// cancellation is observed inside projection construction rather than at
		// the initial procedure checkpoint.
		backend->state->projection_scan_delay = std::chrono::milliseconds(2);
		QueryEngine engine(backend);

		QueryGuard guard(1, 0);
		for (size_t index = 0U; index < 1'000U; ++index) {
			guard.check();
		}
		QueryContext context;
		context.guard = &guard;
		EXPECT_THROW(static_cast<void>(engine.execute("CALL gds.graph.project('cancelled_projection', {})", context)),
				 QueryTimeoutException);

		backend->state->projection_scan_delay = std::chrono::milliseconds(0);
		const QueryResult retried = engine.execute("CALL gds.graph.project('cancelled_projection', {})");
		ASSERT_EQ(retried.rowCount(), 1U);
		EXPECT_EQ(retried.getRows().front().at("name").asPrimitive(), PropertyValue("cancelled_projection"));
	}

	TEST(QueryEngineLogicalBackendBoundaryTest,
			 GdsProjectionChecksAnExpiredGuardImmediatelyBeforePublishingItsCatalogMutation) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		backend->state->nodes.emplace(1U, storage::LogicalGraphNode{1U, {"ProjectionNode"}});
		// This crosses the deadline inside scanNodesAfter(). Fewer than 1,024
		// ordinary checkpoints execute in this small projection, so only the
		// publication-boundary checkNow() can observe the timeout.
		backend->state->projection_scan_delay = std::chrono::milliseconds(75);
		QueryEngine engine(backend);
		QueryGuard guard(50, 0);
		QueryContext context;
		context.guard = &guard;

		EXPECT_THROW(static_cast<void>(engine.execute("CALL gds.graph.project('expired_projection', {})", context)),
					 QueryTimeoutException);
		EXPECT_EQ(engine.execute("CALL gds.graph.exists('expired_projection')").getRows().front().at("exists").asPrimitive(),
				  PropertyValue(false));

		backend->state->projection_scan_delay = std::chrono::milliseconds(0);
		EXPECT_EQ(engine.execute("CALL gds.graph.project('expired_projection', {})").rowCount(), 1U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, TimedOutGdsCatalogDeletionLeavesTheProjectionVisible) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		backend->state->nodes.emplace(1U, storage::LogicalGraphNode{1U, {"ProjectionNode"}});
		QueryEngine engine(backend);
		static_cast<void>(engine.execute("CALL gds.graph.project('guarded_projection', {})"));

		const auto expireGuard = [](QueryGuard &guard) {
			for (size_t index = 0U; index < 1023U; ++index) {
				guard.check();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		};
		QueryGuard dropGuard(1, 0);
		expireGuard(dropGuard);
		QueryContext dropContext;
		dropContext.guard = &dropGuard;
		EXPECT_THROW(static_cast<void>(engine.execute("CALL gds.graph.drop('guarded_projection')", dropContext)),
				 QueryTimeoutException);
		EXPECT_EQ(engine.execute("CALL gds.graph.exists('guarded_projection')").getRows().front().at("exists").asPrimitive(),
				  PropertyValue(true));

		QueryGuard dropAllGuard(1, 0);
		expireGuard(dropAllGuard);
		QueryContext dropAllContext;
		dropAllContext.guard = &dropAllGuard;
		EXPECT_THROW(static_cast<void>(engine.execute("CALL gds.graph.dropAll()", dropAllContext)), QueryTimeoutException);
		EXPECT_EQ(engine.execute("CALL gds.graph.exists('guarded_projection')").getRows().front().at("exists").asPrimitive(),
				  PropertyValue(true));
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, FailedGdsCatalogResultPipelinesDoNotPublishMutations) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		backend->state->nodes.emplace(1U, storage::LogicalGraphNode{1U, {"ProjectionNode"}});
		QueryEngine engine(backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CALL gds.graph.project('atomic_projection', {}) YIELD name RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);
		EXPECT_EQ(engine.execute("CALL gds.graph.exists('atomic_projection')").getRows().front().at("exists").asPrimitive(),
				  PropertyValue(false));

		static_cast<void>(engine.execute("CALL gds.graph.project('atomic_projection', {})"));
		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CALL gds.graph.drop('atomic_projection') YIELD name RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);
		EXPECT_EQ(engine.execute("CALL gds.graph.exists('atomic_projection')").getRows().front().at("exists").asPrimitive(),
				  PropertyValue(true));

		EXPECT_THROW(static_cast<void>(engine.execute(
					 "CALL gds.graph.dropAll() YIELD droppedCount RETURN 1 / 0 AS invalidResult")),
				 std::runtime_error);
		EXPECT_EQ(engine.execute("CALL gds.graph.exists('atomic_projection')").getRows().front().at("exists").asPrimitive(),
				  PropertyValue(true));
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, TimedOutGdsAlgorithmLeavesItsProjectionReusable) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		for (uint64_t nodeId = 1U; nodeId <= 32U; ++nodeId) {
			backend->state->nodes.emplace(nodeId, storage::LogicalGraphNode{nodeId, {"ProjectionNode"}});
		}
		QueryEngine engine(backend);
		static_cast<void>(engine.execute("CALL gds.graph.project('algorithm_timeout_projection', {})"));

		QueryGuard guard(1, 0);
		for (size_t index = 0U; index < 1023U; ++index) {
			guard.check();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		QueryContext context;
		context.guard = &guard;
		EXPECT_THROW(static_cast<void>(engine.execute("CALL gds.pageRank.stream('algorithm_timeout_projection')", context)),
				 QueryTimeoutException);

		EXPECT_EQ(engine.execute("CALL gds.graph.exists('algorithm_timeout_projection')").getRows().front().at("exists").asPrimitive(),
				  PropertyValue(true));
		EXPECT_EQ(engine.execute("CALL gds.pageRank.stream('algorithm_timeout_projection')").rowCount(), 32U);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, GdsProcedureRowsRespectTheConfiguredResultBound) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		backend->state->nodes.emplace(1U, storage::LogicalGraphNode{1U, {"ProjectionNode"}});
		backend->state->nodes.emplace(2U, storage::LogicalGraphNode{2U, {"ProjectionNode"}});
		LogicalReadExecutionOptions options;
		options.bounds.maximum_rows = 1U;
		QueryEngine engine(backend, options);

		const QueryResult projected = engine.execute("CALL gds.graph.project('row_bound_projection', {})");
		ASSERT_EQ(projected.rowCount(), 1U);
		EXPECT_THROW(static_cast<void>(engine.execute("CALL gds.pageRank.stream('row_bound_projection')")),
					 std::runtime_error);

		// Result materialization must fail before publishing a partial procedure
		// result; the immutable catalog entry remains usable for later bounded calls.
		const QueryResult exists = engine.execute("CALL gds.graph.exists('row_bound_projection')");
		ASSERT_EQ(exists.rowCount(), 1U);
		EXPECT_EQ(exists.getRows().front().at("exists").asPrimitive(), PropertyValue(true));
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorSearchRejectsInvalidArgumentsAndBrokenBackendResults) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		QueryEngine engine(backend);
		const auto expectArgumentFailure = [&engine](const std::string &query) {
			EXPECT_THROW(static_cast<void>(engine.execute(query)), std::invalid_argument);
		};

		expectArgumentFailure("CALL db.index.vector.queryNodes(1, 1, [1.0])");
		expectArgumentFailure("CALL db.index.vector.queryNodes('', 1, [1.0])");
		expectArgumentFailure("CALL db.index.vector.queryNodes('memory_embedding', 'one', [1.0])");
		expectArgumentFailure("CALL db.index.vector.queryNodes('memory_embedding', 1, [])");
		expectArgumentFailure("CALL db.index.vector.queryNodes('memory_embedding', 1, ['not-a-number'])");
		const QueryResult zeroTopK = engine.execute("CALL db.index.vector.queryNodes('memory_embedding', 0, [1.0])");
		EXPECT_TRUE(zeroTopK.isEmpty());
		EXPECT_EQ(zeroTopK.getColumns(), (std::vector<std::string>{"node", "score"}));
		EXPECT_EQ(backend->search_snapshot_opens, 0U);

		const QueryResult integerCoordinates =
				engine.execute("CALL db.index.vector.queryNodes('memory_embedding', 1, [1, 2])");
		EXPECT_TRUE(integerCoordinates.isEmpty());
		EXPECT_EQ(backend->search_snapshot_opens, 1U);

		backend->state->return_null_search_snapshot = true;
		EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])")),
					 std::runtime_error);
		backend->state->return_null_search_snapshot = false;

		backend->state->search_result.hits = {{1U, 0.0F}, {2U, 1.0F}};
		EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])")),
					 std::runtime_error);
		backend->state->search_result.hits = {{0U, 0.0F}};
		EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])")),
					 std::runtime_error);
		backend->state->search_result.hits = {{1U, std::numeric_limits<float>::quiet_NaN()}};
		EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])")),
					 std::runtime_error);

		backend->state->search_result.hits = {{1U, 0.0F}};
		backend->state->nodes.clear();
		EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])")),
					 std::runtime_error);
		storage::LogicalGraphNode node;
		node.node_id = 1U;
		node.labels = {""};
		backend->state->nodes.emplace(node.node_id, node);
		EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])")),
					 std::runtime_error);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorSearchValidatesOwnerKeyedPropertiesAtResultMaterialization) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		QueryEngine engine(backend);
		backend->state->search_result.hits = {{1U, 0.0F}};
		storage::LogicalGraphNode node;
		node.node_id = 1U;
		node.labels = {"Memory"};
		backend->state->nodes.emplace(node.node_id, node);

		const QueryResult withoutProperties =
				engine.execute("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])");
		ASSERT_EQ(withoutProperties.rowCount(), 1U);
		EXPECT_TRUE(withoutProperties.getRows().front().at("node").asLogicalNode().properties.empty());

		backend->state->nodes.at(1U).properties = {{"", PropertyValue("invalid")}};
		EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])")),
					 std::runtime_error);

		backend->state->nodes.at(1U).properties = {{"kind", PropertyValue("memory")},
																		{"kind", PropertyValue("duplicate")}};
		EXPECT_THROW(static_cast<void>(engine.execute("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])")),
					 std::runtime_error);
		backend->state->nodes.at(1U).properties = {{"kind", PropertyValue("memory")},
																		{"source", PropertyValue("test")}};
		LogicalReadExecutionOptions onePropertyEntry;
		onePropertyEntry.bounds.maximum_property_entries = 1U;
		QueryEngine boundedEngine(backend, onePropertyEntry);
		EXPECT_THROW(static_cast<void>(boundedEngine.execute("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])")),
					 std::runtime_error);

		backend->state->nodes.at(1U).properties = {{"kind", PropertyValue("memory")}};
		const QueryResult withProperties =
				engine.execute("CALL db.index.vector.queryNodes('memory_embedding', 1, [1.0])");
		ASSERT_EQ(withProperties.rowCount(), 1U);
		EXPECT_EQ(withProperties.getRows().front().at("node").asLogicalNode().properties.at("kind"),
			  PropertyValue("memory"));
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorProcedureRuntimeRejectsMalformedAndUnavailableBoundaries) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		const LogicalReadExecutionOptions options;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		const auto execute = [&runtime, &options](const logical::LogicalOperator &root) {
			return runtime.execute(root, options.bounds, options.maximum_vector_mapping_entries,
					options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
					options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
					options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};

		SyntheticLogicalOperator nonProcedure(logical::LogicalOpType::LOP_SINGLE_ROW);
		EXPECT_THROW(static_cast<void>(execute(nonProcedure)), std::logic_error);
		SyntheticLogicalOperator malformedProject(logical::LogicalOpType::LOP_PROJECT);
		EXPECT_THROW(static_cast<void>(execute(malformedProject)), std::logic_error);
		logical::LogicalProject missingChild(nullptr, {});
		EXPECT_THROW(static_cast<void>(execute(missingChild)), std::logic_error);
		MissingChildLogicalProject missingChildList(nullptr, {});
		EXPECT_THROW(static_cast<void>(execute(missingChildList)), std::logic_error);
		SyntheticLogicalOperator syntheticCall(logical::LogicalOpType::LOP_CALL_PROCEDURE);
		EXPECT_THROW(static_cast<void>(execute(syntheticCall)), std::logic_error);
		SyntheticLogicalOperator malformedFilter(logical::LogicalOpType::LOP_FILTER);
		EXPECT_THROW(static_cast<void>(execute(malformedFilter)), std::logic_error);
		SyntheticLogicalOperator malformedSkip(logical::LogicalOpType::LOP_SKIP);
		EXPECT_THROW(static_cast<void>(execute(malformedSkip)), std::logic_error);
		SyntheticLogicalOperator malformedLimit(logical::LogicalOpType::LOP_LIMIT);
		EXPECT_THROW(static_cast<void>(execute(malformedLimit)), std::logic_error);
		SyntheticLogicalOperator malformedSort(logical::LogicalOpType::LOP_SORT);
		EXPECT_THROW(static_cast<void>(execute(malformedSort)), std::logic_error);

		logical::LogicalCallProcedure missingTrainArgument("db.index.vector.train", {});
		EXPECT_THROW(static_cast<void>(execute(missingTrainArgument)), std::invalid_argument);
		logical::LogicalCallProcedure invalidTrainArgument("db.index.vector.train", {PropertyValue(int64_t{1})});
		EXPECT_THROW(static_cast<void>(execute(invalidTrainArgument)), std::invalid_argument);
		logical::LogicalCallProcedure trainWithValidArgument("db.index.vector.train", {PropertyValue("memory_embedding")});
		EXPECT_THROW(static_cast<void>(runtime.execute(
					 trainWithValidArgument, options.bounds, options.maximum_vector_mapping_entries,
					 options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					 options.vector_candidate_refinement_budget, options.maximum_vector_training_records, 0U,
					 options.maximum_gds_algorithm_edges, options.maximum_gds_algorithm_iterations,
					 options.maximum_gds_algorithm_work_units, QueryContext{})),
					 std::invalid_argument);

		execution::LogicalPlanProcedureRuntime noMaintenanceRuntime(*backend, backend.get(), nullptr);
		logical::LogicalCallProcedure train("db.index.vector.train", {PropertyValue("memory_embedding")});
		EXPECT_THROW(static_cast<void>(noMaintenanceRuntime.execute(
					 train, options.bounds, options.maximum_vector_mapping_entries, options.maximum_raw_vector_reads,
					 options.vector_candidate_search_budget, options.vector_candidate_refinement_budget,
					 options.maximum_vector_training_records, options.maximum_vector_training_pages,
					 options.maximum_gds_algorithm_edges, options.maximum_gds_algorithm_iterations,
					 options.maximum_gds_algorithm_work_units, QueryContext{})),
					 std::runtime_error);

		backend->state->return_null_search_snapshot = true;
		auto call = std::make_unique<logical::LogicalCallProcedure>(
				"db.index.vector.train", std::vector<PropertyValue>{PropertyValue("memory_embedding")});
		logical::LogicalProject projectedTrain(std::move(call), {{nullptr, "status"}});
		EXPECT_THROW(static_cast<void>(execute(projectedTrain)), std::runtime_error);
		backend->state->return_null_search_snapshot = false;

		logical::LogicalCallProcedure unsupported("db.index.vector.unknown",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{1}),
						PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})});
		EXPECT_THROW(static_cast<void>(execute(unsupported)), std::runtime_error);
		logical::LogicalCallProcedure wrongSearchArity(
				"db.index.vector.queryNodes", {PropertyValue("memory_embedding"), PropertyValue(int64_t{1})});
		EXPECT_THROW(static_cast<void>(execute(wrongSearchArity)), std::invalid_argument);
		logical::LogicalCallProcedure nonFiniteQuery(
				"db.index.vector.queryNodes",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{1}),
					 PropertyValue(std::vector<PropertyValue>{PropertyValue(std::numeric_limits<double>::infinity())})});
		EXPECT_THROW(static_cast<void>(execute(nonFiniteQuery)), std::invalid_argument);
		logical::LogicalCallProcedure tooLargePositiveQuery(
				"db.index.vector.queryNodes",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{1}),
					 PropertyValue(std::vector<PropertyValue>{PropertyValue(std::numeric_limits<double>::max())})});
		EXPECT_THROW(static_cast<void>(execute(tooLargePositiveQuery)), std::invalid_argument);
		logical::LogicalCallProcedure tooLargeNegativeQuery(
				"db.index.vector.queryNodes",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{1}),
					 PropertyValue(std::vector<PropertyValue>{PropertyValue(-std::numeric_limits<double>::max())})});
		EXPECT_THROW(static_cast<void>(execute(tooLargeNegativeQuery)), std::invalid_argument);
		logical::LogicalCallProcedure nonListQueryVector(
				"db.index.vector.queryNodes",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{1}), PropertyValue("not-a-list")});
		EXPECT_THROW(static_cast<void>(execute(nonListQueryVector)), std::invalid_argument);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, ValidatesScalarGdsArgumentsBeforeCatalogLookup) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		gds::LogicalProjectionCatalog catalog;
		const LogicalReadExecutionOptions options;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get(), nullptr, &catalog);
		const auto execute = [&runtime, &options](const logical::LogicalOperator &root) {
			return runtime.execute(root, options.bounds, options.maximum_vector_mapping_entries,
					options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
					options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
					options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};

		// The valid integer damping form reaches the catalog lookup, proving that
		// numeric conversion is distinct from algorithm execution or catalog state.
		logical::LogicalCallProcedure integerDamping(
				"gds.pageRank.stream",
				{PropertyValue("missing"), PropertyValue(int64_t{1}), PropertyValue(int64_t{0})});
		EXPECT_THROW(static_cast<void>(execute(integerDamping)), std::out_of_range);

		logical::LogicalCallProcedure nonNumericDamping(
				"gds.pageRank.stream",
				{PropertyValue("missing"), PropertyValue(int64_t{1}), PropertyValue("not-a-number")});
		EXPECT_THROW(static_cast<void>(execute(nonNumericDamping)), std::invalid_argument);
		logical::LogicalCallProcedure nonFiniteDamping(
				"gds.pageRank.stream",
				{PropertyValue("missing"), PropertyValue(int64_t{1}),
				 PropertyValue(std::numeric_limits<double>::infinity())});
		EXPECT_THROW(static_cast<void>(execute(nonFiniteDamping)), std::invalid_argument);

		logical::LogicalCallProcedure nonNumericSampling(
				"gds.betweenness.stream", {PropertyValue("missing"), PropertyValue("one")});
		EXPECT_THROW(static_cast<void>(execute(nonNumericSampling)), std::invalid_argument);
		logical::LogicalCallProcedure emptyProjectionName("gds.wcc.stream", {PropertyValue("")});
		EXPECT_THROW(static_cast<void>(execute(emptyProjectionName)), std::invalid_argument);
		logical::LogicalCallProcedure nonStringProjectionName("gds.closeness.stream", {PropertyValue(int64_t{1})});
		EXPECT_THROW(static_cast<void>(execute(nonStringProjectionName)), std::invalid_argument);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorProcedureRuntimeRejectsMalformedOutputOperators) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		const LogicalReadExecutionOptions options;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		const auto execute = [&runtime](const logical::LogicalOperator &root,
															 const LogicalReadExecutionOptions &executionOptions) {
			return runtime.execute(root, executionOptions.bounds, executionOptions.maximum_vector_mapping_entries,
					executionOptions.maximum_raw_vector_reads, executionOptions.vector_candidate_search_budget,
					executionOptions.vector_candidate_refinement_budget, executionOptions.maximum_vector_training_records,
					executionOptions.maximum_vector_training_pages, executionOptions.maximum_gds_algorithm_edges,
					executionOptions.maximum_gds_algorithm_iterations,
					executionOptions.maximum_gds_algorithm_work_units, QueryContext{});
		};
		const auto trainCall = [] {
			return std::make_unique<logical::LogicalCallProcedure>(
					"db.index.vector.train", std::vector<PropertyValue>{PropertyValue("memory_embedding")});
		};
		const auto searchCall = [] {
			return std::make_unique<logical::LogicalCallProcedure>(
					"db.index.vector.queryNodes",
					std::vector<PropertyValue>{PropertyValue("memory_embedding"), PropertyValue(int64_t{2}),
							PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})});
		};

		logical::LogicalProject emptyProject(trainCall(), {});
		EXPECT_THROW(static_cast<void>(execute(emptyProject, options)), std::invalid_argument);
		logical::LogicalProject invalidProject(trainCall(), {{nullptr, "status"}});
		EXPECT_THROW(static_cast<void>(execute(invalidProject, options)), std::logic_error);
		const auto status = std::make_shared<expressions::VariableReferenceExpression>("status");
		logical::LogicalProject emptyProjectionAlias(trainCall(), {{status, ""}});
		EXPECT_THROW(static_cast<void>(execute(emptyProjectionAlias, options)), std::logic_error);
		logical::LogicalProject duplicateAlias(trainCall(), {{status, "status"}, {status, "status"}});
		EXPECT_THROW(static_cast<void>(execute(duplicateAlias, options)), std::logic_error);

		logical::LogicalFilter missingPredicate(trainCall(), nullptr);
		EXPECT_THROW(static_cast<void>(execute(missingPredicate, options)), std::invalid_argument);
		logical::LogicalSkip negativeBufferedSkip(trainCall(), -1);
		EXPECT_THROW(static_cast<void>(execute(negativeBufferedSkip, options)), std::invalid_argument);
		logical::LogicalLimit negativeBufferedLimit(trainCall(), -1);
		EXPECT_THROW(static_cast<void>(execute(negativeBufferedLimit, options)), std::invalid_argument);
		logical::LogicalSort emptySort(trainCall(), {});
		EXPECT_THROW(static_cast<void>(execute(emptySort, options)), std::invalid_argument);
		logical::LogicalSort nullSortExpression(trainCall(), {{nullptr, true}});
		EXPECT_THROW(static_cast<void>(execute(nullSortExpression, options)), std::invalid_argument);

		logical::LogicalSkip negativeStreamingSkip(searchCall(), -1);
		EXPECT_THROW(static_cast<void>(execute(negativeStreamingSkip, options)), std::invalid_argument);
		logical::LogicalLimit negativeStreamingLimit(searchCall(), -1);
		EXPECT_THROW(static_cast<void>(execute(negativeStreamingLimit, options)), std::invalid_argument);

		LogicalReadExecutionOptions noDistinctCapacity = options;
		noDistinctCapacity.bounds.maximum_distinct_rows = 0U;
		logical::LogicalProject exhaustedDistinct(trainCall(), {{status, "status"}}, true);
		EXPECT_THROW(static_cast<void>(execute(exhaustedDistinct, noDistinctCapacity)), std::invalid_argument);

		storage::LogicalGraphNode firstNode;
		firstNode.node_id = 1U;
		firstNode.labels = {"Memory"};
		storage::LogicalGraphNode secondNode;
		secondNode.node_id = 2U;
		secondNode.labels = {"Memory"};
		backend->state->nodes = {{firstNode.node_id, firstNode}, {secondNode.node_id, secondNode}};
		backend->state->search_result.hits = {{firstNode.node_id, 0.0F}, {secondNode.node_id, 1.0F}};
		LogicalReadExecutionOptions oneDistinctCapacity = options;
		oneDistinctCapacity.bounds.maximum_distinct_rows = 1U;
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalProject exhaustedSearchDistinct(searchCall(), {{score, "score"}}, true);
		EXPECT_THROW(static_cast<void>(execute(exhaustedSearchDistinct, oneDistinctCapacity)), std::runtime_error);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorProcedureRuntimeRejectsInvalidSearchBoundsAndHits) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		const LogicalReadExecutionOptions options;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		logical::LogicalCallProcedure search(
				"db.index.vector.queryNodes",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{1}),
					 PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})});
		const auto execute = [&runtime, &search](const LogicalReadExecutionOptions &executionOptions) {
			return runtime.execute(search, executionOptions.bounds, executionOptions.maximum_vector_mapping_entries,
					executionOptions.maximum_raw_vector_reads, executionOptions.vector_candidate_search_budget,
					executionOptions.vector_candidate_refinement_budget, executionOptions.maximum_vector_training_records,
					executionOptions.maximum_vector_training_pages, executionOptions.maximum_gds_algorithm_edges,
					executionOptions.maximum_gds_algorithm_iterations,
					executionOptions.maximum_gds_algorithm_work_units, QueryContext{});
		};
		const auto expectInvalidBounds = [&execute](LogicalReadExecutionOptions executionOptions) {
			EXPECT_THROW(static_cast<void>(execute(executionOptions)), std::invalid_argument);
		};

		auto invalid = options;
		invalid.bounds.maximum_property_entries = 0U;
		expectInvalidBounds(invalid);
		invalid = options;
		invalid.maximum_vector_mapping_entries = 0U;
		expectInvalidBounds(invalid);
		invalid = options;
		invalid.maximum_raw_vector_reads = 0U;
		expectInvalidBounds(invalid);
		invalid = options;
		invalid.vector_candidate_search_budget = 0U;
		expectInvalidBounds(invalid);
		invalid = options;
		invalid.vector_candidate_refinement_budget = 0U;
		expectInvalidBounds(invalid);
		invalid = options;
		invalid.vector_candidate_search_budget = invalid.maximum_vector_mapping_entries + 1U;
		expectInvalidBounds(invalid);
		invalid = options;
		invalid.vector_candidate_refinement_budget = invalid.maximum_raw_vector_reads + 1U;
		expectInvalidBounds(invalid);
		invalid = options;
		invalid.bounds.maximum_rows = 0U;
		expectInvalidBounds(invalid);
		EXPECT_EQ(backend->search_snapshot_opens, 0U);

		backend->state->search_result.hits = {{1U, 0.0F}, {2U, 1.0F}};
		EXPECT_THROW(static_cast<void>(execute(options)), std::runtime_error);
		logical::LogicalCallProcedure multipleSearch(
				"db.index.vector.queryNodes",
				{PropertyValue("memory_embedding"), PropertyValue(int64_t{2}),
					 PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})});
		const auto executeMultiple = [&runtime, &multipleSearch, &options] {
			return runtime.execute(multipleSearch, options.bounds, options.maximum_vector_mapping_entries,
					options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
					options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
					options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};
		storage::LogicalGraphNode firstNode;
		firstNode.node_id = 1U;
		firstNode.labels = {"Memory"};
		storage::LogicalGraphNode secondNode;
		secondNode.node_id = 2U;
		secondNode.labels = {"Memory"};
		backend->state->nodes = {{firstNode.node_id, firstNode}, {secondNode.node_id, secondNode}};
		backend->state->search_result.hits = {{1U, 0.0F}, {1U, 0.0F}};
		EXPECT_THROW(static_cast<void>(executeMultiple()), std::runtime_error);
		backend->state->search_result.hits = {{2U, 1.0F}, {1U, 0.0F}};
		EXPECT_THROW(static_cast<void>(executeMultiple()), std::runtime_error);

		backend->state->search_result.hits = {{1U, 0.0F}};
		firstNode.properties = {{"", PropertyValue(1)}};
		backend->state->nodes = {{firstNode.node_id, firstNode}};
		EXPECT_THROW(static_cast<void>(execute(options)), std::runtime_error);
		backend->state->nodes.at(1U).properties.clear();
		backend->state->nodes.at(1U).labels = {"Memory", "Memory"};
		EXPECT_THROW(static_cast<void>(execute(options)), std::runtime_error);
	}

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorProcedureRuntimeValidatesBufferedSearchRowsAndSortKeys) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		const LogicalReadExecutionOptions options;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		const auto searchCall = [] {
			return std::make_unique<logical::LogicalCallProcedure>(
					"db.index.vector.queryNodes",
					std::vector<PropertyValue>{PropertyValue("memory_embedding"), PropertyValue(int64_t{2}),
							PropertyValue(std::vector<PropertyValue>{PropertyValue(1.0)})});
		};
		const auto execute = [&runtime, &options](const logical::LogicalOperator &root) {
			return runtime.execute(root, options.bounds, options.maximum_vector_mapping_entries,
					options.maximum_raw_vector_reads, options.vector_candidate_search_budget,
					options.vector_candidate_refinement_budget, options.maximum_vector_training_records,
					options.maximum_vector_training_pages, options.maximum_gds_algorithm_edges,
					options.maximum_gds_algorithm_iterations, options.maximum_gds_algorithm_work_units, QueryContext{});
		};

		storage::LogicalGraphNode firstNode;
		firstNode.node_id = 1U;
		firstNode.labels = {"Memory"};
		storage::LogicalGraphNode secondNode;
		secondNode.node_id = 2U;
		secondNode.labels = {"Memory"};
		backend->state->nodes = {{firstNode.node_id, firstNode}, {secondNode.node_id, secondNode}};
		const auto literalSortKey = std::make_shared<expressions::LiteralExpression>(int64_t{1});

		backend->state->search_result.hits = {{0U, 0.0F}};
		logical::LogicalSort zeroIdHitSort(searchCall(), {{literalSortKey, true}});
		EXPECT_THROW(static_cast<void>(execute(zeroIdHitSort)), std::runtime_error);
		backend->state->search_result.hits = {{firstNode.node_id, std::numeric_limits<float>::quiet_NaN()}};
		logical::LogicalSort nonFiniteHitSort(searchCall(), {{literalSortKey, true}});
		EXPECT_THROW(static_cast<void>(execute(nonFiniteHitSort)), std::runtime_error);
		backend->state->search_result.hits = {{firstNode.node_id, 0.0F}, {firstNode.node_id, 0.0F}};
		logical::LogicalSort duplicateHitSort(searchCall(), {{literalSortKey, true}});
		EXPECT_THROW(static_cast<void>(execute(duplicateHitSort)), std::runtime_error);
		backend->state->search_result.hits = {{firstNode.node_id, 1.0F}, {secondNode.node_id, 0.0F}};
		logical::LogicalSort unorderedScoreSort(searchCall(), {{literalSortKey, true}});
		EXPECT_THROW(static_cast<void>(execute(unorderedScoreSort)), std::runtime_error);
		backend->state->search_result.hits = {{firstNode.node_id, 0.0F}, {firstNode.node_id, 1.0F}};
		logical::LogicalSort repeatedIdSort(searchCall(), {{literalSortKey, true}});
		EXPECT_THROW(static_cast<void>(execute(repeatedIdSort)), std::runtime_error);

		backend->state->nodes.at(firstNode.node_id).node_id = secondNode.node_id;
		backend->state->search_result.hits = {{firstNode.node_id, 0.0F}};
		logical::LogicalSort mismatchedNodeSort(searchCall(), {{literalSortKey, true}});
		EXPECT_THROW(static_cast<void>(execute(mismatchedNodeSort)), std::runtime_error);
		backend->state->nodes.at(firstNode.node_id) = firstNode;

		backend->state->search_result.hits = {{firstNode.node_id, 0.0F}};
		const auto nodeValue = std::make_shared<expressions::VariableReferenceExpression>("node");
		logical::LogicalSort nodeSortKey(searchCall(), {{nodeValue, true}});
		EXPECT_THROW(static_cast<void>(execute(nodeSortKey)), std::runtime_error);
		logical::LogicalFilter nodePredicate(searchCall(), nodeValue);
		EXPECT_THROW(static_cast<void>(execute(nodePredicate)), std::runtime_error);

		backend->state->search_result.hits = {{firstNode.node_id, 0.0F}, {secondNode.node_id, 1.0F}};
		logical::LogicalSort equalSortKeys(searchCall(), {{literalSortKey, true}});
		EXPECT_NO_THROW(static_cast<void>(execute(equalSortKeys)));
		const auto score = std::make_shared<expressions::VariableReferenceExpression>("score");
		logical::LogicalSort ascendingScoreSort(searchCall(), {{score, true}});
		EXPECT_NO_THROW(static_cast<void>(execute(ascendingScoreSort)));
		logical::LogicalSort descendingScoreSort(searchCall(), {{score, false}});
		EXPECT_NO_THROW(static_cast<void>(execute(descendingScoreSort)));
	}

} // namespace graph::query
