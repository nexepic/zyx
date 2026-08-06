/**
 * @file test_LogicalPlanMutationRuntime.cpp
 * @brief End-to-end tests for bounded logical-plan mutations.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Types.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/LogicalPlanMutationRuntime.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"

namespace graph::query {

	namespace {

		using PropertyMap = std::unordered_map<std::string, PropertyValue>;

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
			explicit TestLogicalGraphSnapshot(TestGraph graph) : graph_(std::move(graph)) {}

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
				storage::LogicalGraphMutationFence fence;
				fence.backend_token = 0x1234U;
				fence.format_token = 0x5678U;
				fence.graph_state_tokens[0] = graph_.generation;
				return fence;
			}

		private:
			TestGraph graph_;
		};

		class TestLogicalGraph final : public storage::LogicalGraphReadBackend,
									   public storage::LogicalGraphWriteBackend {
		public:
			[[nodiscard]] std::unique_ptr<storage::LogicalGraphReadSnapshot>
			openLogicalGraphReadSnapshot() const override {
				return std::make_unique<TestLogicalGraphSnapshot>(graph_);
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
				if (reject_next_mutation_) {
					reject_next_mutation_ = false;
					throw storage::LogicalGraphMutationConflict("test graph injected one optimistic conflict");
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
			void rejectNextMutation() { reject_next_mutation_ = true; }

		private:
			TestGraph graph_;
			bool reject_next_mutation_ = false;
		};

	} // namespace

	TEST(LogicalPlanMutationRuntimeTest, MergeUsesOneAtomicBatchAndSelectsOnCreateOrOnMatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult created =
				engine.execute("MERGE (n:Person {name: 'Ada'}) ON CREATE SET n.created = 1, n.copied = n.name "
							   "RETURN n.name AS name, n.created AS created, n.copied AS copied");
		ASSERT_EQ(created.rowCount(), 1U);
		EXPECT_EQ(std::get<std::string>(created.getRows().front().at("name").asPrimitive().getVariant()), "Ada");
		EXPECT_EQ(std::get<int64_t>(created.getRows().front().at("created").asPrimitive().getVariant()), 1);
		EXPECT_EQ(std::get<std::string>(created.getRows().front().at("copied").asPrimitive().getVariant()), "Ada");
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);

		QueryResult matched = engine.execute("MERGE (n:Person {name: 'Ada'}) ON MATCH SET n.seen = true RETURN "
											 "n.created + 1 AS created, n.seen AS seen");
		ASSERT_EQ(matched.rowCount(), 1U);
		EXPECT_EQ(std::get<int64_t>(matched.getRows().front().at("created").asPrimitive().getVariant()), 2);
		EXPECT_TRUE(std::get<bool>(matched.getRows().front().at("seen").asPrimitive().getVariant()));
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 2U);

		QueryResult combined =
				engine.execute("MATCH (n:Person {name: 'Ada'}) SET n.rank = 7, n:Visible RETURN n.rank AS rank");
		ASSERT_EQ(combined.rowCount(), 1U);
		EXPECT_EQ(std::get<int64_t>(combined.getRows().front().at("rank").asPrimitive().getVariant()), 7);
		EXPECT_EQ(backend->countNodesWithLabel("Visible"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 3U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeReevaluatesTheEntireDecisionAfterAnOptimisticConflict) {
		auto backend = std::make_shared<TestLogicalGraph>();
		backend->rejectNextMutation();
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute("MERGE (n:Person {name: 'Ada'}) RETURN n");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_TRUE(result.getRows().front().at("n").isLogicalNode());
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeDeduplicatesCreatedPatternsAcrossInputRowsAndMergesRelationships) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {}));
		static_cast<void>(backend->addNode({"Seed"}, {}));
		QueryEngine engine(backend, backend);

		QueryResult shared = engine.execute("MATCH (s:Seed) MERGE (n:Person {name: 'Ada'}) RETURN n.name AS name");
		ASSERT_EQ(shared.rowCount(), 2U);
		EXPECT_EQ(std::get<std::string>(shared.getRows()[0].at("name").asPrimitive().getVariant()), "Ada");
		EXPECT_EQ(std::get<std::string>(shared.getRows()[1].at("name").asPrimitive().getVariant()), "Ada");
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);

		QueryResult created =
				engine.execute("MERGE (a:Person {name: 'Ada'})-[r:KNOWS {since: 2026}]->(b:Person {name: 'Bob'}) "
							   "ON CREATE SET r.created = true, r.copied = r.since RETURN a, r, b");
		ASSERT_EQ(created.rowCount(), 1U);
		const auto &edge = created.getRows().front().at("r").asLogicalEdge();
		EXPECT_EQ(edge.type, "KNOWS");
		EXPECT_EQ(edge.properties.at("since"), PropertyValue(int64_t(2026)));
		EXPECT_EQ(edge.properties.at("created"), PropertyValue(true));
		EXPECT_EQ(edge.properties.at("copied"), PropertyValue(int64_t(2026)));
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 2U);
		EXPECT_EQ(backend->edgeCount(), 1U);

		QueryResult matched =
				engine.execute("MERGE (a:Person {name: 'Ada'})-[r:KNOWS {since: 2026}]->(b:Person {name: 'Bob'}) "
							   "ON MATCH SET r.seen = true RETURN r.seen AS seen");
		ASSERT_EQ(matched.rowCount(), 1U);
		EXPECT_TRUE(std::get<bool>(matched.getRows().front().at("seen").asPrimitive().getVariant()));
		EXPECT_EQ(backend->edgeCount(), 1U);

		QueryResult incoming =
				engine.execute("MERGE (b:Person {name: 'Bob'})<-[r:KNOWS {since: 2026}]-(a:Person {name: 'Ada'}) "
							   "ON MATCH SET r.incoming = true RETURN r.incoming AS incoming");
		ASSERT_EQ(incoming.rowCount(), 1U);
		EXPECT_TRUE(std::get<bool>(incoming.getRows().front().at("incoming").asPrimitive().getVariant()));
		EXPECT_EQ(backend->edgeCount(), 1U);

		QueryResult either =
				engine.execute("MERGE (a:Person {name: 'Ada'})-[r:KNOWS {since: 2026}]-(b:Person {name: 'Bob'}) "
							   "ON MATCH SET r.either = true RETURN r.either AS either");
		ASSERT_EQ(either.rowCount(), 1U);
		EXPECT_TRUE(std::get<bool>(either.getRows().front().at("either").asPrimitive().getVariant()));
		EXPECT_EQ(backend->edgeCount(), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MergeUndirectedRelationshipFallsBackToIncomingAdjacency) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Person"}, {{"name", PropertyValue("Ada")}}));
		static_cast<void>(backend->addNode({"Person"}, {{"name", PropertyValue("Bob")}}));
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"MATCH (a:Person {name: 'Ada'}), (b:Person {name: 'Bob'}) CREATE (a)-[:KNOWS {since: 2026}]->(b)"));
		QueryResult result = engine.execute(
				"MERGE (a:Person {name: 'Bob'})-[r:KNOWS {since: 2026}]-(b:Person {name: 'Ada'}) RETURN r");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalEdgeResult &edge = result.getRows().front().at("r").asLogicalEdge();
		EXPECT_EQ(edge.type, "KNOWS");
		EXPECT_EQ(edge.properties.at("since"), PropertyValue(int64_t(2026)));
		EXPECT_EQ(backend->edgeCount(), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, UnwindCreatesOneLogicalNodePerInputValueInOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult result =
				engine.execute("UNWIND [4, 5, 6] AS value CREATE (n:Item {value: value}) RETURN n.value AS value");
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("value").asPrimitive(), PropertyValue(4));
		EXPECT_EQ(result.getRows()[1].at("value").asPrimitive(), PropertyValue(5));
		EXPECT_EQ(result.getRows()[2].at("value").asPrimitive(), PropertyValue(6));
		EXPECT_EQ(backend->countNodesWithLabel("Item"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, WriteOnlyCallSubqueryRebindsEveryOuterRowIntoOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t firstSeed = backend->addNode({"Seed"}, {});
		const uint64_t secondSeed = backend->addNode({"Seed"}, {});
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute("MATCH (seed:Seed) CALL { CREATE (:Child {created: true}) } RETURN seed");

		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("seed").asLogicalNode().node_id, firstSeed);
		EXPECT_EQ(result.getRows()[1].at("seed").asLogicalNode().node_id, secondSeed);
		EXPECT_EQ(backend->countNodesWithLabel("Child"), 2U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, WriteOnlyCallSubqueryFoldsCreateSetAndRemoveIntoOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"value", PropertyValue(9)}});
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"MATCH (seed:Seed) CALL { CREATE (child:Child {copied: seed.value, temporary: true}) "
				"SET child.doubled = child.copied * 2 REMOVE child.temporary } RETURN seed");

		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("seed").asLogicalNode().node_id, seedId);
		ASSERT_EQ(backend->countNodesWithLabel("Child"), 1U);
		const TestNode &child = std::find_if(backend->graph().nodes.begin(), backend->graph().nodes.end(),
										  [](const auto &entry) {
											  return std::ranges::find(entry.second.labels, "Child") != entry.second.labels.end();
										  })
										  ->second;
		EXPECT_EQ(child.properties.at("copied"), PropertyValue(9));
		EXPECT_EQ(child.properties.at("doubled"), PropertyValue(18));
		EXPECT_FALSE(child.properties.contains("temporary"));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, WriteOnlyCallSubqueryRejectsExportsAndTransactionBatches) {
		auto backend = std::make_shared<TestLogicalGraph>();
		execution::LogicalPlanMutationRuntime runtime(*backend, *backend);
		const auto makeBody = [] {
			return std::make_unique<logical::LogicalCreateNode>(
					"child", std::vector<std::string>{"Child"}, PropertyMap{},
					std::unordered_map<std::string, std::shared_ptr<expressions::Expression>>{},
					std::make_unique<logical::LogicalSingleRow>());
		};

		logical::LogicalCallSubquery exportingCall(std::make_unique<logical::LogicalSingleRow>(), makeBody(),
												   std::vector<std::string>{}, std::vector<std::string>{"child"});
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(exportingCall, execution::LogicalPlanReadRuntimeBounds{}, QueryContext{})),
					 std::logic_error);

		logical::LogicalCallSubquery transactionBatch(std::make_unique<logical::LogicalSingleRow>(), makeBody(),
													  std::vector<std::string>{}, std::vector<std::string>{}, true, 32);
		EXPECT_THROW(static_cast<void>(runtime.execute(transactionBatch, execution::LogicalPlanReadRuntimeBounds{},
													   QueryContext{})),
					 std::logic_error);
		EXPECT_EQ(backend->countNodesWithLabel("Child"), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachCreatesOneLogicalNodePerListElementInOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute("FOREACH (value IN [4, 5, 6] | CREATE (:Item {value: value}))"));
		EXPECT_EQ(backend->countNodesWithLabel("Item"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachCreatesAllBodyEntitiesInOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute("FOREACH (value IN [4, 5, 6] | CREATE (item:Item {value: value}) CREATE "
										 "(audit:Audit {value: value}))"));
		EXPECT_EQ(backend->countNodesWithLabel("Item"), 3U);
		EXPECT_EQ(backend->countNodesWithLabel("Audit"), 3U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateThenSetUsesTheCreatedEntityWithinOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"CREATE (n:Item {value: 4}) SET n.doubled = n.value RETURN n.value + n.doubled AS total, "
				"n.doubled AS doubled");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(8));
		EXPECT_EQ(result.getRows().front().at("doubled").asPrimitive(), PropertyValue(4));
		ASSERT_EQ(backend->countNodesWithLabel("Item"), 1U);
		ASSERT_EQ(backend->graph().nodes.size(), 1U);
		EXPECT_EQ(backend->graph().nodes.begin()->second.properties.at("value"), PropertyValue(4));
		EXPECT_EQ(backend->graph().nodes.begin()->second.properties.at("doubled"), PropertyValue(4));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ReusesLogicalReadExpressionSemanticsForCreateAndSet) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult created = engine.execute("CREATE (n:Counter {value: 1 + 2}) RETURN n.value + 1 AS value");
		ASSERT_EQ(created.rowCount(), 1U);
		EXPECT_EQ(created.getRows().front().at("value").asPrimitive(), PropertyValue(4));

		QueryResult updated =
				engine.execute("MATCH (n:Counter) SET n.value = n.value * 2 + 1 RETURN n.value + 1 AS value");
		ASSERT_EQ(updated.rowCount(), 1U);
		EXPECT_EQ(updated.getRows().front().at("value").asPrimitive(), PropertyValue(8));

		QueryResult copied = engine.execute(
				"MATCH (n:Counter) CREATE (copy:Copy {value: n.value + 1}) RETURN copy.value + 1 AS value");
		ASSERT_EQ(copied.rowCount(), 1U);
		EXPECT_EQ(copied.getRows().front().at("value").asPrimitive(), PropertyValue(9));
		EXPECT_EQ(backend->graph().nodes.at(1U).properties.at("value"), PropertyValue(7));
		EXPECT_EQ(backend->countNodesWithLabel("Copy"), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ProjectsScalarExpressionsFromCreatedRelationshipFinalState) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"CREATE (a:Start {value: 2})-[r:LINK {weight: 3}]->(b:End {value: 4}) RETURN r.weight * 2 AS weight");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("weight").asPrimitive(), PropertyValue(6));
		EXPECT_EQ(backend->edgeCount(), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateSetMaterializesCreatedRelationshipAfterFinalStateFold) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult result =
				engine.execute("CREATE (a:Start)-[r:LINK {weight: 1}]->(b:End) SET r.weight = r.weight + 1 RETURN r");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalEdgeResult &edge = result.getRows().front().at("r").asLogicalEdge();
		EXPECT_EQ(edge.type, "LINK");
		EXPECT_EQ(edge.properties.at("weight"), PropertyValue(2));
		EXPECT_EQ(backend->edgeCount(), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachCreateThenSetUsesOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"FOREACH (value IN [4, 5, 6] | CREATE (item:Item {value: value}) SET item.doubled = item.value)"));
		ASSERT_EQ(backend->countNodesWithLabel("Item"), 3U);
		for (const auto &[_, node]: backend->graph().nodes) {
			EXPECT_EQ(node.properties.at("doubled"), node.properties.at("value"));
		}
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachCreateThenRemoveFoldsEachCreatedPropertyState) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"FOREACH (value IN [4, 5, 6] | CREATE (item:Item {value: value, temporary: true}) "
				"SET item.doubled = item.value * 2 REMOVE item.temporary)"));

		ASSERT_EQ(backend->countNodesWithLabel("Item"), 3U);
		for (const auto &[_, node]: backend->graph().nodes) {
			EXPECT_EQ(node.properties.at("doubled"), PropertyValue(std::get<int64_t>(node.properties.at("value").getVariant()) * 2));
			EXPECT_FALSE(node.properties.contains("temporary"));
		}
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachFoldsCreatedRelationshipPropertiesAgainstCreatedNodeState) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute(
				"FOREACH (value IN [4, 5, 6] | CREATE (source:Start {value: value})-"
				"[relationship:LINK {temporary: true}]->(:End) "
				"SET relationship.weight = source.value * 2 REMOVE relationship.temporary)"));

		ASSERT_EQ(backend->edgeCount(), 3U);
		for (const auto &[_, edge]: backend->graph().edges) {
			EXPECT_EQ(edge.type, "LINK");
			const TestNode &source = backend->graph().nodes.at(edge.source);
			const int64_t value = std::get<int64_t>(source.properties.at("value").getVariant());
			EXPECT_EQ(edge.properties.at("weight"), PropertyValue(value * 2));
			EXPECT_FALSE(edge.properties.contains("temporary"));
		}
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachPreservesMatchedRowsForFollowingProjection) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"MATCH (n:Seed) FOREACH (value IN [4, 5, 6] | SET n.value = value) RETURN n.value AS value");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(6));
		EXPECT_EQ(backend->graph().nodes.at(seedId).properties.at("value"), PropertyValue(6));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachScalarInputSkipsItsBodyAndRetainsTheInputRow) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"value", PropertyValue(7)}});
		QueryEngine engine(backend, backend);

		QueryResult result =
				engine.execute("MATCH (n:Seed) FOREACH (value IN n.value | SET n.changed = true) RETURN n");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalNodeResult &node = result.getRows().front().at("n").asLogicalNode();
		EXPECT_EQ(node.node_id, seedId);
		EXPECT_FALSE(node.properties.contains("changed"));
		EXPECT_FALSE(backend->graph().nodes.at(seedId).properties.contains("changed"));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachReturnsCapturedRowsAfterItsMutationChangesTheInputMatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute("MATCH (n:Seed) FOREACH (value IN [1] | REMOVE n:Seed) RETURN n");
		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalNodeResult &returned = result.getRows().front().at("n").asLogicalNode();
		EXPECT_EQ(returned.node_id, seedId);
		EXPECT_TRUE(returned.labels.empty());
		EXPECT_TRUE(backend->graph().nodes.at(seedId).labels.empty());
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateThenRemoveFoldsTheFinalPropertyStateIntoOneAtomicBatch) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"CREATE (n:Item {value: 4, temporary: true}) REMOVE n.temporary RETURN n.value + 1 AS value");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(5));
		ASSERT_EQ(backend->graph().nodes.size(), 1U);
		const PropertyMap &properties = backend->graph().nodes.begin()->second.properties;
		EXPECT_EQ(properties.at("value"), PropertyValue(4));
		EXPECT_FALSE(properties.contains("temporary"));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MatchCreateThenSetPublishesPersistedAndCreatedFinalStatesTogether) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"value", PropertyValue(4)}});
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute(
				"MATCH (seed:Seed) CREATE (child:Child {copied: seed.value}) SET seed.updated = child.copied "
				"RETURN seed.updated AS updated, child.copied AS copied");
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("updated").asPrimitive(), PropertyValue(4));
		EXPECT_EQ(result.getRows().front().at("copied").asPrimitive(), PropertyValue(4));
		EXPECT_EQ(backend->graph().nodes.at(seedId).properties.at("updated"), PropertyValue(4));
		EXPECT_EQ(backend->countNodesWithLabel("Child"), 1U);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, SetAndRemoveUseCompleteNodeAndRelationshipPropertyReplacements) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t first = backend->addNode({"Person", "Stale"},
												{{"name", PropertyValue("Ada")}, {"remove", PropertyValue(true)}});
		const uint64_t second = backend->addNode({"Person"}, {{"name", PropertyValue("Bob")}});
		QueryEngine engine(backend, backend);

		QueryResult nodeResult =
				engine.execute("MATCH (n:Person {name: 'Ada'}) SET n += {age: 42, remove: null}, n:Current RETURN n");
		ASSERT_EQ(nodeResult.rowCount(), 1U);
		const LogicalNodeResult &updatedNode = nodeResult.getRows().front().at("n").asLogicalNode();
		EXPECT_EQ(updatedNode.node_id, first);
		EXPECT_EQ(updatedNode.properties.at("name"), PropertyValue("Ada"));
		EXPECT_EQ(updatedNode.properties.at("age"), PropertyValue(42));
		EXPECT_FALSE(updatedNode.properties.contains("remove"));
		EXPECT_TRUE(std::ranges::find(updatedNode.labels, "Current") != updatedNode.labels.end());
		EXPECT_EQ(backend->graph().mutation_batches, 1U);

		QueryResult edgeCreate = engine.execute("MATCH (a:Person {name: 'Ada'}), (b:Person {name: 'Bob'}) CREATE "
												"(a)-[r:KNOWS {weight: 1, stale: true}]->(b) RETURN r");
		ASSERT_EQ(edgeCreate.rowCount(), 1U);
		const uint64_t edgeId = edgeCreate.getRows().front().at("r").asLogicalEdge().edge_id;
		QueryResult edgeResult = engine.execute("MATCH (:Person {name: 'Ada'})-[r:KNOWS]->(:Person {name: 'Bob'}) SET "
												"r.weight = r.weight + 1 REMOVE r.stale RETURN r");
		ASSERT_EQ(edgeResult.rowCount(), 1U);
		const LogicalEdgeResult &updatedEdge = edgeResult.getRows().front().at("r").asLogicalEdge();
		EXPECT_EQ(updatedEdge.edge_id, edgeId);
		EXPECT_EQ(updatedEdge.properties.at("weight"), PropertyValue(2));
		EXPECT_FALSE(updatedEdge.properties.contains("stale"));
		EXPECT_EQ(backend->graph().edges.at(edgeId).properties, updatedEdge.properties);
		EXPECT_EQ(backend->graph().nodes.at(second).properties.at("name"), PropertyValue("Bob"));
	}

	TEST(LogicalPlanMutationRuntimeTest, DeleteMaterializesPreDeleteBindingsAndHonorsDetachSemantics) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Person"}, {{"name", PropertyValue("Ada")}}));
		static_cast<void>(backend->addNode({"Person"}, {{"name", PropertyValue("Bob")}}));
		QueryEngine engine(backend, backend);

		QueryResult created = engine.execute(
				"MATCH (a:Person {name: 'Ada'}), (b:Person {name: 'Bob'}) CREATE (a)-[r:KNOWS]->(b) RETURN r");
		ASSERT_EQ(created.rowCount(), 1U);
		const LogicalEdgeResult originalEdge = created.getRows().front().at("r").asLogicalEdge();

		QueryResult deletedEdge = engine.execute("MATCH ()-[r:KNOWS]->() DELETE r RETURN r");
		ASSERT_EQ(deletedEdge.rowCount(), 1U);
		EXPECT_EQ(deletedEdge.getRows().front().at("r").asLogicalEdge(), originalEdge);
		EXPECT_EQ(backend->edgeCount(), 0U);

		static_cast<void>(
				engine.execute("MATCH (a:Person {name: 'Ada'}), (b:Person {name: 'Bob'}) CREATE (a)-[:KNOWS]->(b)"));
		QueryResult detached = engine.execute("MATCH (n:Person {name: 'Ada'}) DETACH DELETE n RETURN n");
		ASSERT_EQ(detached.rowCount(), 1U);
		EXPECT_EQ(detached.getRows().front().at("n").asLogicalNode().properties.at("name"), PropertyValue("Ada"));
		EXPECT_EQ(backend->edgeCount(), 0U);
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CountStarObservesMutationPipelineRowsAndMergePostMutations) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		QueryResult count = engine.execute("CREATE (:Person {name: 'Ada'}) RETURN count(*) AS count");
		ASSERT_EQ(count.rowCount(), 1U);
		EXPECT_EQ(count.getRows().front().at("count").asPrimitive(), PropertyValue(1));

		QueryResult merge = engine.execute(
				"MERGE (n:Person {name: 'Ada'}) SET n += {seen: true, name: 'Ada'} REMOVE n.name RETURN n");
		ASSERT_EQ(merge.rowCount(), 1U);
		const LogicalNodeResult &node = merge.getRows().front().at("n").asLogicalNode();
		EXPECT_TRUE(std::get<bool>(node.properties.at("seen").getVariant()));
		EXPECT_FALSE(node.properties.contains("name"));
		EXPECT_EQ(backend->countNodesWithLabel("Person"), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, MutationAggregatesPreserveInputRowsAcrossSetAndMerge) {
		auto backend = std::make_shared<TestLogicalGraph>();
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(1)}}));
		static_cast<void>(backend->addNode({"Seed"}, {{"value", PropertyValue(2)}}));
		QueryEngine engine(backend, backend);

		QueryResult setCount = engine.execute("MATCH (n:Seed) SET n.value = n.value + 1 RETURN count(*) AS count");
		ASSERT_EQ(setCount.rowCount(), 1U);
		EXPECT_EQ(setCount.getRows().front().at("count").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(backend->graph().nodes.at(1U).properties.at("value"), PropertyValue(2));
		EXPECT_EQ(backend->graph().nodes.at(2U).properties.at("value"), PropertyValue(3));

		QueryResult mergeCount = engine.execute("MERGE (:Created {name: 'Ada'}) RETURN count(*) AS count");
		ASSERT_EQ(mergeCount.rowCount(), 1U);
		EXPECT_EQ(mergeCount.getRows().front().at("count").asPrimitive(), PropertyValue(1));
		EXPECT_EQ(backend->countNodesWithLabel("Created"), 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, RetriesStandaloneCreateSetAndDeleteAfterOptimisticConflicts) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		backend->rejectNextMutation();
		QueryResult created = engine.execute("CREATE (n:Retry {value: 1}) RETURN n");
		ASSERT_EQ(created.rowCount(), 1U);
		const uint64_t retryId = created.getRows().front().at("n").asLogicalNode().node_id;
		EXPECT_EQ(backend->graph().nodes.at(retryId).properties.at("value"), PropertyValue(1));

		backend->rejectNextMutation();
		QueryResult updated = engine.execute("MATCH (n:Retry) SET n.value = n.value + 1 RETURN n.value AS value");
		ASSERT_EQ(updated.rowCount(), 1U);
		EXPECT_EQ(updated.getRows().front().at("value").asPrimitive(), PropertyValue(2));

		backend->rejectNextMutation();
		QueryResult deleted = engine.execute("MATCH (n:Retry) DELETE n RETURN n");
		ASSERT_EQ(deleted.rowCount(), 1U);
		EXPECT_EQ(deleted.getRows().front().at("n").asLogicalNode().node_id, retryId);
		EXPECT_FALSE(backend->graph().nodes.contains(retryId));
		EXPECT_EQ(backend->graph().mutation_batches, 3U);
	}

	TEST(LogicalPlanMutationRuntimeTest, CreateSetChainCancelsTransientCreatesAndPublishesPersistedDeleteAtomically) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t seedId = backend->addNode({"Seed"}, {{"name", PropertyValue("Ada")}});
		QueryEngine engine(backend, backend);

		static_cast<void>(engine.execute("MATCH (:Seed) CREATE (transient:Transient {value: 1}) DELETE transient"));
		EXPECT_EQ(backend->countNodesWithLabel("Transient"), 0U);
		EXPECT_TRUE(backend->graph().nodes.contains(seedId));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);

		static_cast<void>(engine.execute("MATCH (seed:Seed) CREATE (child:Child {copied: seed.name}) SET child += "
										 "{created: true, copied: 'Ada'} "
										 "REMOVE child.copied DELETE seed"));
		EXPECT_FALSE(backend->graph().nodes.contains(seedId));
		ASSERT_EQ(backend->countNodesWithLabel("Child"), 1U);
		const auto child = std::ranges::find_if(backend->graph().nodes, [](const auto &entry) {
			return std::ranges::find(entry.second.labels, "Child") != entry.second.labels.end();
		});
		ASSERT_NE(child, backend->graph().nodes.end());
		EXPECT_EQ(child->second.properties.at("created"), PropertyValue(true));
		EXPECT_FALSE(child->second.properties.contains("copied"));
		EXPECT_EQ(backend->graph().mutation_batches, 1U);

		static_cast<void>(engine.execute("CREATE (a:Ephemeral)-[r:LINK]->(b:Ephemeral) DELETE r, a, b"));
		EXPECT_EQ(backend->countNodesWithLabel("Ephemeral"), 0U);
		EXPECT_EQ(backend->edgeCount(), 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, ForeachRejectsUnsupportedDeleteWithoutPublishingItsCreates) {
		auto backend = std::make_shared<TestLogicalGraph>();
		QueryEngine engine(backend, backend);

		EXPECT_THROW(static_cast<void>(engine.execute(
							 "FOREACH (value IN [1] | CREATE (a:Ephemeral)-[r:LINK]->(b:Ephemeral) DELETE r, a, b)")),
					 std::logic_error);

		EXPECT_EQ(backend->countNodesWithLabel("Ephemeral"), 0U);
		EXPECT_EQ(backend->edgeCount(), 0U);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, UnifiedSetRemovePathFoldsLabelsNullsAndMapUpdatesInOrder) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId = backend->addNode(
				{"Seed", "Old"},
				{{"keep", PropertyValue(1)}, {"remove", PropertyValue(true)}, {"old", PropertyValue("stale")}});
		QueryEngine engine(backend, backend);

		QueryResult result = engine.execute("MATCH (n:Seed) SET n.remove = null, n += {added: 4, old: null}, n:New "
											"REMOVE n.old, n:Old RETURN n");

		ASSERT_EQ(result.rowCount(), 1U);
		const LogicalNodeResult &node = result.getRows().front().at("n").asLogicalNode();
		EXPECT_EQ(node.node_id, nodeId);
		EXPECT_EQ(node.properties.at("keep"), PropertyValue(1));
		EXPECT_EQ(node.properties.at("added"), PropertyValue(4));
		EXPECT_FALSE(node.properties.contains("remove"));
		EXPECT_FALSE(node.properties.contains("old"));
		EXPECT_TRUE(std::ranges::find(node.labels, "Seed") != node.labels.end());
		EXPECT_TRUE(std::ranges::find(node.labels, "New") != node.labels.end());
		EXPECT_TRUE(std::ranges::find(node.labels, "Old") == node.labels.end());
		EXPECT_EQ(backend->graph().nodes.at(nodeId).properties, node.properties);
		EXPECT_EQ(backend->graph().nodes.at(nodeId).labels, node.labels);
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, UnifiedSetRemovePathAvoidsNoOpPublicationAndRetriesConflicts) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t nodeId = backend->addNode({"Seed", "Stable"}, {{"value", PropertyValue(1)}});
		QueryEngine engine(backend, backend);

		QueryResult noOp =
				engine.execute("MATCH (n:Seed) SET n.value = 1, n:Stable REMOVE n.missing, n:Missing RETURN n");
		ASSERT_EQ(noOp.rowCount(), 1U);
		EXPECT_EQ(noOp.getRows().front().at("n").asLogicalNode().node_id, nodeId);
		EXPECT_EQ(backend->graph().mutation_batches, 0U);

		backend->rejectNextMutation();
		QueryResult retried = engine.execute(
				"MATCH (n:Seed) SET n.value = n.value + 1, n:Updated REMOVE n:Stable RETURN n.value AS value");
		ASSERT_EQ(retried.rowCount(), 1U);
		EXPECT_EQ(retried.getRows().front().at("value").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(backend->graph().nodes.at(nodeId).properties.at("value"), PropertyValue(2));
		EXPECT_TRUE(std::ranges::find(backend->graph().nodes.at(nodeId).labels, "Updated") !=
					backend->graph().nodes.at(nodeId).labels.end());
		EXPECT_TRUE(std::ranges::find(backend->graph().nodes.at(nodeId).labels, "Stable") ==
					backend->graph().nodes.at(nodeId).labels.end());
		EXPECT_EQ(backend->graph().mutation_batches, 1U);
	}

	TEST(LogicalPlanMutationRuntimeTest, UnifiedSetRemovePathRejectsBoundedPartialMutations) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t first = backend->addNode({"Seed"}, {{"value", PropertyValue(1)}});
		const uint64_t second = backend->addNode({"Seed"}, {{"value", PropertyValue(2)}});
		LogicalReadExecutionOptions options;
		options.bounds.maximum_node_records = 16U;
		options.bounds.maximum_node_page_records = 4U;
		options.bounds.maximum_metadata_pages_per_node_page = 4U;
		options.bounds.maximum_edges_per_node = 16U;
		options.bounds.maximum_adjacency_page_records = 4U;
		options.bounds.maximum_property_entries = 16U;
		options.bounds.maximum_rows = 1U;
		options.bounds.maximum_distinct_rows = 16U;
		QueryEngine engine(backend, backend, options);

		EXPECT_THROW(static_cast<void>(engine.execute("MATCH (n:Seed) SET n.value = n.value + 1 RETURN n")),
					 std::runtime_error);
		EXPECT_EQ(backend->graph().nodes.at(first).properties.at("value"), PropertyValue(1));
		EXPECT_EQ(backend->graph().nodes.at(second).properties.at("value"), PropertyValue(2));
		EXPECT_EQ(backend->graph().mutation_batches, 0U);
	}

	TEST(LogicalPlanMutationRuntimeTest, AppliesRuntimeMapParametersToPersistedAndCreatedEntities) {
		auto backend = std::make_shared<TestLogicalGraph>();
		const uint64_t persisted =
				backend->addNode({"Seed"}, {{"keep", PropertyValue(1)}, {"remove", PropertyValue("stale")}});
		QueryEngine engine(backend, backend);

		QueryContext persistedContext;
		persistedContext.parameters.emplace(
				"patch",
				PropertyValue(PropertyValue::MapType{{"added", PropertyValue(2)}, {"remove", PropertyValue()}}));
		QueryResult persistedResult = engine.execute("MATCH (n:Seed) SET n += $patch RETURN n", persistedContext);
		ASSERT_EQ(persistedResult.rowCount(), 1U);
		const LogicalNodeResult &persistedNode = persistedResult.getRows().front().at("n").asLogicalNode();
		EXPECT_EQ(persistedNode.node_id, persisted);
		EXPECT_EQ(persistedNode.properties.at("keep"), PropertyValue(1));
		EXPECT_EQ(persistedNode.properties.at("added"), PropertyValue(2));
		EXPECT_FALSE(persistedNode.properties.contains("remove"));

		QueryContext createdContext;
		createdContext.parameters.emplace("patch", PropertyValue(PropertyValue::MapType{{"added", PropertyValue(4)},
																						{"remove", PropertyValue()}}));
		QueryResult createdResult = engine.execute(
				"CREATE (n:Created {keep: 3, remove: 'temporary'}) SET n += $patch RETURN n", createdContext);
		ASSERT_EQ(createdResult.rowCount(), 1U);
		const LogicalNodeResult &createdNode = createdResult.getRows().front().at("n").asLogicalNode();
		EXPECT_EQ(createdNode.properties.at("keep"), PropertyValue(3));
		EXPECT_EQ(createdNode.properties.at("added"), PropertyValue(4));
		EXPECT_FALSE(createdNode.properties.contains("remove"));
		EXPECT_EQ(backend->graph().nodes.at(createdNode.node_id).properties, createdNode.properties);
	}

} // namespace graph::query
