/**
 * @file test_QueryEngineLogicalBackend.cpp
 * @brief Boundary tests for the isolated logical read QueryEngine mode.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Types.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/LogicalGraphVectorSearch.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"

namespace graph::query {

	namespace {

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

		struct ProcedureBackendState {
			storage::LogicalGraphVectorSearchResult search_result;
			std::unordered_map<uint64_t, storage::LogicalGraphNode> nodes;
			std::vector<storage::LogicalGraphEdge> edges;
			bool return_null_search_snapshot = false;
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
			[[nodiscard]] storage::LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override {
				throw std::logic_error("scalar procedure test must not page through Nodes");
			}
			[[nodiscard]] storage::LogicalGraphAdjacencyPage
			scanAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection,
							  std::optional<storage::LogicalGraphAdjacencyContinuation>, size_t) const override;
			void visitEdges(size_t, const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {
				throw std::logic_error("scalar procedure test must not scan relationships");
			}
			void visitAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection, size_t,
							const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {
				throw std::logic_error("scalar procedure test must not visit adjacency");
			}
			[[nodiscard]] std::optional<storage::LogicalGraphMutationFence> mutationFence() const override {
				return storage::LogicalGraphMutationFence{0x3456U, 0x789AU, {}};
			}
			[[nodiscard]] storage::LogicalGraphVectorSearchResult
			searchLogicalVector(const storage::LogicalGraphVectorSearchRequest &) const override {
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
			[[nodiscard]] storage::LogicalGraphVectorTrainingResult
			trainLogicalVector(const storage::LogicalGraphVectorTrainingRequest &request) override {
				++training_calls;
				last_training_request = request;
				return training_result;
			}

			mutable size_t search_snapshot_opens = 0U;
			size_t training_calls = 0U;
			storage::LogicalGraphVectorTrainingRequest last_training_request;
			storage::LogicalGraphVectorTrainingResult training_result{1U, true, false};
			std::shared_ptr<ProcedureBackendState> state = std::make_shared<ProcedureBackendState>();
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

	} // namespace

	TEST(QueryEngineLogicalBackendBoundaryTest, RejectsANullLogicalBackendAtConstruction) {
		std::shared_ptr<storage::LogicalGraphReadBackend> backend;
		EXPECT_THROW(static_cast<void>(QueryEngine(std::move(backend))), std::invalid_argument);
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

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorTrainingSupportsTheBoundedScalarOutputPipeline) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		QueryEngine engine(backend);

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
		EXPECT_EQ(backend->search_snapshot_opens, 1U);

		backend->training_result = {0U, false, false};
		const QueryResult skipped = engine.execute("CALL db.index.vector.train('empty_embedding')");
		ASSERT_EQ(skipped.getColumns(), (std::vector<std::string>{"status"}));
		ASSERT_EQ(skipped.rowCount(), 1U);
		EXPECT_EQ(skipped.getRows().front().at("status").asPrimitive(), PropertyValue("Skipped (Empty Data)"));
		EXPECT_EQ(backend->training_calls, 2U);
		EXPECT_EQ(backend->search_snapshot_opens, 1U);
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
					options.maximum_vector_training_pages, QueryContext{});
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
					 QueryContext{})),
					 std::invalid_argument);

		execution::LogicalPlanProcedureRuntime noMaintenanceRuntime(*backend, backend.get(), nullptr);
		logical::LogicalCallProcedure train("db.index.vector.train", {PropertyValue("memory_embedding")});
		EXPECT_THROW(static_cast<void>(noMaintenanceRuntime.execute(
					 train, options.bounds, options.maximum_vector_mapping_entries, options.maximum_raw_vector_reads,
					 options.vector_candidate_search_budget, options.vector_candidate_refinement_budget,
					 options.maximum_vector_training_records, options.maximum_vector_training_pages, QueryContext{})),
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

	TEST(QueryEngineLogicalBackendBoundaryTest, VectorProcedureRuntimeRejectsMalformedOutputOperators) {
		auto backend = std::make_shared<RecordingVectorProcedureBackend>();
		const LogicalReadExecutionOptions options;
		execution::LogicalPlanProcedureRuntime runtime(*backend, backend.get(), backend.get());
		const auto execute = [&runtime](const logical::LogicalOperator &root,
																						 const LogicalReadExecutionOptions &executionOptions) {
			return runtime.execute(root, executionOptions.bounds, executionOptions.maximum_vector_mapping_entries,
					executionOptions.maximum_raw_vector_reads, executionOptions.vector_candidate_search_budget,
					executionOptions.vector_candidate_refinement_budget, executionOptions.maximum_vector_training_records,
					executionOptions.maximum_vector_training_pages, QueryContext{});
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
					executionOptions.maximum_vector_training_pages, QueryContext{});
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
					options.maximum_vector_training_pages, QueryContext{});
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
					options.maximum_vector_training_pages, QueryContext{});
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
