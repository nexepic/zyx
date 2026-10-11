/**
 * @file test_LogicalPlanVarLengthRuntime.cpp
 * @brief Bounded logical variable-length traversal regression tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNamedPath.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalUnion.hpp"
#include "graph/query/logical/operators/LogicalVarLengthTraversal.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::execution;
using namespace graph::storage;
namespace logical = graph::query::logical;

namespace {

	class PathSnapshot final : public LogicalGraphReadSnapshot {
	public:
		explicit PathSnapshot(bool nativeRefs = false, std::shared_ptr<size_t> referenceCalls = nullptr,
				std::shared_ptr<size_t> edgeReads = nullptr, std::shared_ptr<size_t> nodeReads = nullptr,
				bool danglingEndpoint = false, bool existenceProbes = false,
				std::shared_ptr<size_t> existenceBatchCalls = nullptr,
				std::shared_ptr<size_t> edgeBatchReads = nullptr,
				std::shared_ptr<size_t> nodeBatchReads = nullptr,
				std::shared_ptr<size_t> batchReferenceCalls = nullptr,
				std::shared_ptr<size_t> exactLookupCalls = nullptr, bool branched = false, int batchFault = 0,
				size_t recordPayloadBytes = 0U, size_t additionalSourceNodes = 0U)
				: native_refs_(nativeRefs), reference_calls_(std::move(referenceCalls)), edge_reads_(std::move(edgeReads)),
				  node_reads_(std::move(nodeReads)), existence_probes_(existenceProbes),
				  existence_batch_calls_(std::move(existenceBatchCalls)), edge_batch_reads_(std::move(edgeBatchReads)),
				  node_batch_reads_(std::move(nodeBatchReads)), batch_reference_calls_(std::move(batchReferenceCalls)),
				  exact_lookup_calls_(std::move(exactLookupCalls)), batch_fault_(batchFault) {
			nodes_.emplace(1U, LogicalGraphNode{1U, {"Source"}, {{"id", PropertyValue("source")}}});
			nodes_.emplace(2U, LogicalGraphNode{2U, {"Middle"}, {{"id", PropertyValue("middle")}}});
			nodes_.emplace(3U, LogicalGraphNode{3U, {"Target"}, {{"id", PropertyValue("target")}}});
			for (size_t index = 0U; index < additionalSourceNodes; ++index) {
				const uint64_t id = 100U + static_cast<uint64_t>(index);
				nodes_.emplace(id, LogicalGraphNode{id, {"Source"}, {{"id", PropertyValue(static_cast<int64_t>(id))}}});
			}
			edges_.emplace(10U, LogicalGraphEdge{10U, 1U, 2U, "KNOWS"});
			edges_.emplace(11U, LogicalGraphEdge{11U, 2U, 3U, "KNOWS"});
			if (recordPayloadBytes != 0U) {
				nodes_.at(2U).properties.push_back({"payload", PropertyValue(std::string(recordPayloadBytes, 'n'))});
				edges_.at(10U).properties.push_back({"payload", PropertyValue(std::string(recordPayloadBytes, 'e'))});
			}
			if (branched) {
				edges_.at(10U).target_node_id = 3U;
				edges_.emplace(12U, LogicalGraphEdge{12U, 1U, 2U, "KNOWS"});
				edges_.emplace(13U, LogicalGraphEdge{13U, 1U, 2U, "KNOWS"});
			}
			if (danglingEndpoint) {
				edges_.at(11U).target_node_id = 99U;
			}
		}

		[[nodiscard]] LogicalGraphNode readNode(uint64_t nodeId) const override {
			if (node_reads_) {
				++*node_reads_;
			}
			return nodes_.at(nodeId);
		}
		[[nodiscard]] std::vector<LogicalGraphNode> readNodesShapedSorted(
				std::span<const uint64_t> nodeIds, const EntityReadShape &shape) const override {
			validateEntityReadShape(shape);
			if (!node_batch_reads_) {
				return LogicalGraphReadSnapshot::readNodesShapedSorted(nodeIds, shape);
			}
			++*node_batch_reads_;
			std::vector<LogicalGraphNode> result;
			result.reserve(nodeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t nodeId: nodeIds) {
				if (nodeId == 0U || nodeId <= previous) {
					throw std::invalid_argument("test Node batch IDs are not strictly increasing");
				}
				previous = nodeId;
				result.push_back(nodes_.at(nodeId));
			}
			if (!result.empty()) {
				if (batch_fault_ == 1) result.pop_back();
				if (batch_fault_ == 2) std::ranges::reverse(result);
				if (batch_fault_ == 3) result.front().node_id = 99U;
				if (batch_fault_ == 4) result.front().labels = {std::string(8192U, 'x')};
			}
			return result;
		}
		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t edgeId) const override {
			if (edge_reads_) {
				++*edge_reads_;
			}
			return edges_.at(edgeId);
		}
		[[nodiscard]] std::vector<LogicalGraphEdge> readEdgesShapedSorted(
				std::span<const uint64_t> edgeIds, const EntityReadShape &shape) const override {
			validateEntityReadShape(shape);
			if (!edge_batch_reads_) {
				return LogicalGraphReadSnapshot::readEdgesShapedSorted(edgeIds, shape);
			}
			++*edge_batch_reads_;
			std::vector<LogicalGraphEdge> result;
			result.reserve(edgeIds.size());
			uint64_t previous = 0U;
			for (const uint64_t edgeId: edgeIds) {
				if (edgeId == 0U || edgeId <= previous) {
					throw std::invalid_argument("test Edge batch IDs are not strictly increasing");
				}
				previous = edgeId;
				result.push_back(edges_.at(edgeId));
			}
			return result;
		}
		[[nodiscard]] bool containsNode(uint64_t nodeId) const override { return nodes_.contains(nodeId); }
		[[nodiscard]] std::vector<bool> containsNodesSorted(std::span<const uint64_t> nodeIds) const override {
			if (existence_batch_calls_) {
				++*existence_batch_calls_;
			}
			return LogicalGraphReadSnapshot::containsNodesSorted(nodeIds);
		}
		[[nodiscard]] bool supportsNodeExistenceProbes() const override { return native_refs_ || existence_probes_; }
		void visitNodes(size_t, const std::function<bool(const LogicalGraphNode &)> &) const override {
			throw std::logic_error("unexpected legacy Node traversal");
		}
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t afterNodeId, size_t maximumRecords, size_t) const override {
			LogicalGraphNodePage page;
			page.metadata_pages_visited = 1U;
			for (const auto &[id, node]: nodes_) {
				if (id <= afterNodeId) {
					continue;
				}
				if (page.nodes.size() == maximumRecords) {
					page.may_have_more = true;
					break;
				}
				page.nodes.push_back(node);
			}
			return page;
		}
		[[nodiscard]] LogicalGraphAdjacencyReferencePage scanAdjacentEdgeRefs(
			uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const override {
			if (!native_refs_) {
				throw std::logic_error("unexpected native adjacency reference traversal");
			}
			if (reference_calls_) {
				++*reference_calls_;
			}
			const LogicalGraphAdjacencyPage full = scanAdjacentEdges(nodeId, direction, std::move(continuation), maximumRecords);
			LogicalGraphAdjacencyReferencePage result;
			result.edges.reserve(full.edges.size());
			for (const LogicalGraphEdge &edge: full.edges) {
				result.edges.push_back({edge.edge_id, true, edge.source_node_id, edge.target_node_id, 1U});
			}
			result.continuation = full.continuation;
			return result;
		}
		[[nodiscard]] bool supportsNativeAdjacentEdgeRefScans() const override { return native_refs_; }
		void visitAdjacentEdgeRefsForNodes(std::span<const uint64_t> nodeIds,
				LogicalGraphReadDirection direction, size_t maximumEdges,
				const std::function<bool(const EntityRef &)> &visitor) const override {
			if (!native_refs_ || nodeIds.empty() || maximumEdges == 0U || !visitor ||
				!std::ranges::is_sorted(nodeIds) || std::ranges::adjacent_find(nodeIds) != nodeIds.end()) {
				throw std::invalid_argument("test batch adjacency request is invalid");
			}
			if (batch_reference_calls_) {
				++*batch_reference_calls_;
			}
			size_t visited = 0U;
			for (const uint64_t nodeId: nodeIds) {
				for (const auto &[edgeId, edge]: edges_) {
					static_cast<void>(edgeId);
					const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
							? edge.source_node_id == nodeId : edge.target_node_id == nodeId;
					if (!matches) {
						continue;
					}
					if (visited == maximumEdges) {
						return;
					}
					++visited;
					if (!visitor({edge.edge_id, true, edge.source_node_id, edge.target_node_id, 1U})) {
						return;
					}
				}
			}
		}
		[[nodiscard]] bool supportsNativeBatchAdjacentEdgeRefs() const override { return native_refs_; }
		[[nodiscard]] bool supportsNativeRelationshipTypeTokens() const override { return native_refs_; }
		[[nodiscard]] std::optional<uint64_t> findRelationshipTypeToken(std::string_view name) const override {
			return native_refs_ && name == "KNOWS" ? std::optional<uint64_t>{1U} : std::nullopt;
		}
		[[nodiscard]] std::optional<uint32_t> findPropertyToken(std::string_view name) const override {
			return native_refs_ && name == "id" ? std::optional<uint32_t>{1U} : std::nullopt;
		}
		[[nodiscard]] std::optional<std::vector<LogicalGraphNode>> findNodesByExactProperties(
				const std::vector<std::string> &labels,
				const std::vector<std::pair<std::string, PropertyValue>> &predicates,
				size_t maximumNodes, size_t maximumProperties) const override {
			if (!native_refs_ || predicates.empty() || predicates.size() > maximumProperties) {
				return std::nullopt;
			}
			if (exact_lookup_calls_) {
				++*exact_lookup_calls_;
			}
			std::vector<LogicalGraphNode> matches;
			for (const auto &[id, node]: nodes_) {
				static_cast<void>(id);
				const bool labelsMatch = std::ranges::all_of(labels, [&node](const std::string &label) {
					return std::ranges::find(node.labels, label) != node.labels.end();
				});
				const bool propertiesMatch = std::ranges::all_of(predicates, [&node](const auto &predicate) {
					const auto found = std::ranges::find(node.properties, predicate.first,
							&LogicalGraphPropertyEntry::key);
					return found != node.properties.end() && found->value == predicate.second;
				});
				if (labelsMatch && propertiesMatch) {
					if (matches.size() == maximumNodes) {
						throw std::runtime_error("test exact lookup exceeded its Node bound");
					}
					matches.push_back(node);
				}
			}
			return matches;
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdges(
				uint64_t nodeId, LogicalGraphReadDirection direction,
				std::optional<LogicalGraphAdjacencyContinuation>, size_t maximumRecords) const override {
			LogicalGraphAdjacencyPage page;
			for (const auto &[_, edge]: edges_) {
				const bool matches = direction == LogicalGraphReadDirection::LGRD_OUTGOING
									 ? edge.source_node_id == nodeId
									 : edge.target_node_id == nodeId;
				if (!matches) {
					continue;
				}
				if (page.edges.size() == maximumRecords) {
					throw std::logic_error("test graph unexpectedly needs an adjacency continuation");
				}
				page.edges.push_back(edge);
			}
			return page;
		}
		void visitEdges(size_t, const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("unexpected legacy Edge traversal");
		}
		void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
								const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("unexpected legacy adjacency traversal");
		}

	private:
		std::map<uint64_t, LogicalGraphNode> nodes_;
		std::map<uint64_t, LogicalGraphEdge> edges_;
		bool native_refs_ = false;
		std::shared_ptr<size_t> reference_calls_;
		std::shared_ptr<size_t> edge_reads_;
		std::shared_ptr<size_t> node_reads_;
		bool existence_probes_ = false;
		std::shared_ptr<size_t> existence_batch_calls_;
		std::shared_ptr<size_t> edge_batch_reads_;
		std::shared_ptr<size_t> node_batch_reads_;
		std::shared_ptr<size_t> batch_reference_calls_;
		std::shared_ptr<size_t> exact_lookup_calls_;
		int batch_fault_ = 0;
	};

	class PathBackend final : public LogicalGraphReadBackend {
	public:
		PathBackend(bool nativeRefs, size_t recordPayloadBytes) :
			native_refs_(nativeRefs), record_payload_bytes_(recordPayloadBytes) {}
		PathBackend(bool nativeRefs, size_t recordPayloadBytes, size_t additionalSourceNodes) :
			native_refs_(nativeRefs), record_payload_bytes_(recordPayloadBytes), additional_source_nodes_(additionalSourceNodes) {}
		explicit PathBackend(bool nativeRefs = false, std::shared_ptr<size_t> referenceCalls = nullptr,
														std::shared_ptr<size_t> edgeReads = nullptr, std::shared_ptr<size_t> nodeReads = nullptr,
														bool danglingEndpoint = false, bool existenceProbes = false,
														std::shared_ptr<size_t> existenceBatchCalls = nullptr,
										std::shared_ptr<size_t> edgeBatchReads = nullptr,
										std::shared_ptr<size_t> nodeBatchReads = nullptr,
										std::shared_ptr<size_t> batchReferenceCalls = nullptr,
										std::shared_ptr<size_t> exactLookupCalls = nullptr, bool branched = false, int batchFault = 0,
						size_t recordPayloadBytes = 0U)
				: native_refs_(nativeRefs), reference_calls_(std::move(referenceCalls)), edge_reads_(std::move(edgeReads)),
				  node_reads_(std::move(nodeReads)), dangling_endpoint_(danglingEndpoint), existence_probes_(existenceProbes),
				  existence_batch_calls_(std::move(existenceBatchCalls)), edge_batch_reads_(std::move(edgeBatchReads)),
				  node_batch_reads_(std::move(nodeBatchReads)), batch_reference_calls_(std::move(batchReferenceCalls)),
				  exact_lookup_calls_(std::move(exactLookupCalls)), branched_(branched), batch_fault_(batchFault),
				  record_payload_bytes_(recordPayloadBytes) {}
		[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
			return std::make_unique<PathSnapshot>(native_refs_, reference_calls_, edge_reads_, node_reads_,
					dangling_endpoint_, existence_probes_, existence_batch_calls_, edge_batch_reads_, node_batch_reads_,
					batch_reference_calls_, exact_lookup_calls_, branched_, batch_fault_, record_payload_bytes_, additional_source_nodes_);
		}
	private:
		bool native_refs_ = false;
		std::shared_ptr<size_t> reference_calls_;
		std::shared_ptr<size_t> edge_reads_;
		std::shared_ptr<size_t> node_reads_;
		bool dangling_endpoint_ = false;
		bool existence_probes_ = false;
		std::shared_ptr<size_t> existence_batch_calls_;
		std::shared_ptr<size_t> edge_batch_reads_;
		std::shared_ptr<size_t> node_batch_reads_;
		std::shared_ptr<size_t> batch_reference_calls_;
		std::shared_ptr<size_t> exact_lookup_calls_;
		bool branched_ = false;
		int batch_fault_ = 0;
		size_t record_payload_bytes_ = 0U;
		size_t additional_source_nodes_ = 0U;
	};

	[[nodiscard]] LogicalPlanReadRuntimeBounds bounds() {
		return {
				.maximum_node_records = 8U,
				.maximum_node_page_records = 2U,
				.maximum_metadata_pages_per_node_page = 2U,
				.maximum_edges_per_node = 4U,
				.maximum_edge_records = 16U,
				.maximum_adjacency_page_records = 2U,
				.maximum_property_entries = 4U,
				.maximum_rows = 8U,
				.maximum_distinct_rows = 8U,
		};
	}

} // namespace

TEST(LogicalPlanVarLengthRuntimeTest, ExecutesBoundedTargetTraversalAndRejectsUnrepresentableRelationshipLists) {
	PathBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalVarLengthTraversal targetOnly(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 1, 2, std::vector<std::string>{"Target"});
	const graph::query::QueryResult result = runtime.executeToQueryResult(targetOnly, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows()[0].at("source").asLogicalNode().node_id, 1U);
	EXPECT_EQ(result.getRows()[0].at("target").asLogicalNode().node_id, 3U);

	logical::LogicalVarLengthTraversal namedRelationships(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "edges",
			"target", "KNOWS", "out", 1, 2);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(namedRelationships, bounds())), std::invalid_argument);
}

TEST(LogicalPlanVarLengthRuntimeTest, BuildsAValidatedNamedPathUsingTheExistingPathListContract) {
	PathBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source",
			"relationship", "target", "KNOWS", "out");
	logical::LogicalNamedPath path(std::move(traversal), "p", std::vector<std::string>{"source", "target"},
								  std::vector<std::string>{"relationship"});

	const graph::query::QueryResult result = runtime.executeToQueryResult(path, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const std::vector<PropertyValue> &values = result.getRows()[0].at("p").asPrimitive().getList();
	ASSERT_EQ(values.size(), 3U);
	EXPECT_EQ(values[0].getMap().at("_type"), PropertyValue("node"));
	EXPECT_EQ(values[0].getMap().at("_id"), PropertyValue(1));
	EXPECT_EQ(values[1].getMap().at("_type"), PropertyValue("relationship"));
	EXPECT_EQ(values[1].getMap().at("_id"), PropertyValue(10));
	EXPECT_EQ(values[2].getMap().at("_type"), PropertyValue("node"));
	EXPECT_EQ(values[2].getMap().at("_id"), PropertyValue(2));
}

TEST(LogicalPlanVarLengthRuntimeTest, AdmitsNamedPathResultMapsBeforeCopyingProperties) {
	PathBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const auto makePath = [] {
		auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 2, 2, std::vector<std::string>{"Target"});
		return logical::LogicalNamedPath(std::move(traversal), "p",
			std::vector<std::string>{"source", "target"}, std::vector<std::string>{});
	};
	auto limited = bounds();
	limited.maximum_property_entries = 2U;
	try {
		static_cast<void>(runtime.executeToQueryResult(makePath(), limited));
		FAIL() << "NamedPath accepted more Properties than the configured bound";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(), "logical-plan NamedPath exceeded the configured Property bound");
	}
	limited.maximum_property_entries = 4U;
	// The upstream row and PropertyValue sizes vary by platform. Find caps
	// that reject the source cache and the additional NamedPath result copy.
	bool sourceBudgetRejected = false;
	bool pathBudgetRejected = false;
	for (size_t budget = 1000U; budget <= 16384U; budget += 16U) {
		limited.maximum_materialized_bytes = budget;
		try {
			static_cast<void>(runtime.executeToQueryResult(makePath(), limited));
		} catch (const std::runtime_error &error) {
			if (std::string_view(error.what()) == "logical-plan NamedPath Property cache exceeded its shared byte bound")
				sourceBudgetRejected = true;
			if (std::string_view(error.what()) == "logical-plan NamedPath result exceeded its shared byte bound")
				pathBudgetRejected = true;
		}
		if (sourceBudgetRejected && pathBudgetRejected) break;
	}
	EXPECT_TRUE(sourceBudgetRejected);
	EXPECT_TRUE(pathBudgetRejected);
	limited.maximum_materialized_bytes = 65536U;
	const QueryResult result = runtime.executeToQueryResult(makePath(), limited);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("p").asPrimitive().getList().size(), 5U);
}

TEST(LogicalPlanVarLengthRuntimeTest, MaterializesNamedPathsFromVariableLengthTraversalRoutes) {
	PathBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 2, 2, std::vector<std::string>{"Target"});
	logical::LogicalNamedPath path(std::move(traversal), "p", std::vector<std::string>{"source", "target"},
								  std::vector<std::string>{});

	const QueryResult result = runtime.executeToQueryResult(path, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const std::vector<PropertyValue> &values = result.getRows().front().at("p").asPrimitive().getList();
	ASSERT_EQ(values.size(), 5U);
	EXPECT_EQ(values[0].getMap().at("_id"), PropertyValue(1));
	EXPECT_EQ(values[1].getMap().at("_id"), PropertyValue(10));
	EXPECT_EQ(values[2].getMap().at("_id"), PropertyValue(2));
	EXPECT_EQ(values[3].getMap().at("_id"), PropertyValue(11));
	EXPECT_EQ(values[4].getMap().at("_id"), PropertyValue(3));
}

TEST(LogicalPlanVarLengthRuntimeTest, JoinAndOptionalMatchPreserveDeferredVariableLengthPathCaches) {
	const auto makeTraversal = [] {
		return std::make_unique<logical::LogicalVarLengthTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
				"target", "KNOWS", "out", 2, 2, std::vector<std::string>{"Target"});
	};
	const auto executeNamedPath = [](std::unique_ptr<logical::LogicalOperator> child) {
		PathBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		logical::LogicalNamedPath path(std::move(child), "p", std::vector<std::string>{"source", "target"}, {});
		return runtime.executeToQueryResult(path, bounds());
	};
	const auto verifyPath = [](const QueryResult &result) {
		ASSERT_EQ(result.rowCount(), 1U);
		const auto &values = result.getRows().front().at("p").asPrimitive().getList();
		ASSERT_EQ(values.size(), 5U);
		EXPECT_EQ(values[0].getMap().at("_id"), PropertyValue(1));
		EXPECT_EQ(values[1].getMap().at("_id"), PropertyValue(10));
		EXPECT_EQ(values[2].getMap().at("_id"), PropertyValue(2));
		EXPECT_EQ(values[3].getMap().at("_id"), PropertyValue(11));
		EXPECT_EQ(values[4].getMap().at("_id"), PropertyValue(3));
	};

	const QueryResult joined = executeNamedPath(std::make_unique<logical::LogicalJoin>(
			std::make_unique<logical::LogicalSingleRow>(), makeTraversal()));
	verifyPath(joined);

	const QueryResult optional = executeNamedPath(std::make_unique<logical::LogicalOptionalMatch>(
			std::make_unique<logical::LogicalSingleRow>(), makeTraversal()));
	verifyPath(optional);
}

TEST(LogicalPlanVarLengthRuntimeTest, CallSubqueryImportsDeferredVariableLengthPathCache) {
	PathBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 2, 2, std::vector<std::string>{"Target"});
	auto body = std::make_unique<logical::LogicalNamedPath>(
			std::make_unique<logical::LogicalSingleRow>(), "p", std::vector<std::string>{"source", "target"},
			std::vector<std::string>{});
	logical::LogicalCallSubquery call(std::move(traversal), std::move(body),
			std::vector<std::string>{"source", "target"}, std::vector<std::string>{"p"});

	const QueryResult result = runtime.executeToQueryResult(call, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &values = result.getRows().front().at("p").asPrimitive().getList();
	ASSERT_EQ(values.size(), 5U);
	EXPECT_EQ(values[0].getMap().at("_id"), PropertyValue(1));
	EXPECT_EQ(values[1].getMap().at("_id"), PropertyValue(10));
	EXPECT_EQ(values[2].getMap().at("_id"), PropertyValue(2));
	EXPECT_EQ(values[3].getMap().at("_id"), PropertyValue(11));
	EXPECT_EQ(values[4].getMap().at("_id"), PropertyValue(3));
}

TEST(LogicalPlanVarLengthRuntimeTest, UsesNativeAdjacencyReferencesForVariableLengthTraversal) {
	auto referenceCalls = std::make_shared<size_t>(0U);
	auto edgeReads = std::make_shared<size_t>(0U);
	PathBackend backend(true, referenceCalls, edgeReads);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalVarLengthTraversal traversal(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 1, 2, std::vector<std::string>{"Target"});

	const QueryResult result = runtime.executeToQueryResult(traversal, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("target").asLogicalNode().node_id, 3U);
	EXPECT_GT(*referenceCalls, 0U);
	EXPECT_EQ(*edgeReads, 0U);
}

TEST(LogicalPlanVarLengthRuntimeTest, BatchesVariableLengthCountWithoutChangingSimplePathSemantics) {
	auto batchCalls = std::make_shared<size_t>(0U);
	auto referenceCalls = std::make_shared<size_t>(0U);
	PathBackend backend(true, referenceCalls, nullptr, nullptr, false, false, nullptr, nullptr, nullptr, batchCalls);
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 1, 2);
	logical::LogicalAggregate aggregate(
			std::move(traversal), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("target"), "total"}});

	const QueryResult result = runtime.executeToQueryResult(aggregate, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(2));
	EXPECT_EQ(*batchCalls, 2U);
	EXPECT_EQ(*referenceCalls, 0U);
}

TEST(LogicalPlanVarLengthRuntimeTest, ChargesInitialCountFrontierCapacityToMaterializedByteBound) {
	PathBackend backend(true, 0U, 64U);
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{}), "source", "",
			"target", "KNOWS", "out", 1, 1);
	logical::LogicalAggregate aggregate(
			std::move(traversal), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("target"), "total"}});
	auto limited = bounds();
	limited.maximum_node_records = 128U;
	limited.maximum_materialized_bytes = sizeof(LogicalPlanReadRow) * 2U;
	try {
		static_cast<void>(runtime.executeToQueryResult(aggregate, limited));
		FAIL() << "Variable-length count accepted an unaccounted initial frontier";
	} catch (const std::runtime_error &error) {
		EXPECT_STREQ(error.what(),
				"logical-plan variable-length count exceeded its materialized byte bound");
	}

	limited.maximum_materialized_bytes = 65536U;
	const QueryResult result = runtime.executeToQueryResult(aggregate, limited);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(2));
}

TEST(LogicalPlanVarLengthRuntimeTest, BatchesFilteredVariableLengthCountAndEnforcesResultBounds) {
	auto batchCalls = std::make_shared<size_t>(0U);
	PathBackend backend(true, nullptr, nullptr, nullptr, false, false, nullptr, nullptr, nullptr, batchCalls);
	LogicalPlanReadRuntime runtime(backend);
	const auto makeAggregate = [] {
		auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}),
				"source", "", "target", "KNOWS", "out", 1, 2, std::vector<std::string>{"Target"});
		return std::make_unique<logical::LogicalAggregate>(
				std::move(traversal), std::vector<std::shared_ptr<expressions::Expression>>{},
				std::vector<logical::LogicalAggItem>{{
						"count", std::make_shared<expressions::VariableReferenceExpression>("target"), "total"}});
	};

	const QueryResult result = runtime.executeToQueryResult(*makeAggregate(), bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(1));

	auto bounded = bounds();
	bounded.maximum_rows = 1U;
	auto unfiltered = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 1, 2);
	logical::LogicalAggregate exceedsRows(
			std::move(unfiltered), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("target"), "total"}});
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(exceedsRows, bounded)), std::runtime_error);
	EXPECT_GE(*batchCalls, 4U);
}

TEST(LogicalPlanVarLengthRuntimeTest, BatchesIncomingAndBidirectionalVariableLengthCounts) {
	auto batchCalls = std::make_shared<size_t>(0U);
	PathBackend backend(true, nullptr, nullptr, nullptr, false, false, nullptr, nullptr, nullptr, batchCalls);
	LogicalPlanReadRuntime runtime(backend);
	const auto executeCount = [&](std::unique_ptr<logical::LogicalVarLengthTraversal> traversal) {
		logical::LogicalAggregate aggregate(
				std::move(traversal), {},
				{{"count", std::make_shared<expressions::VariableReferenceExpression>("target"), "total"}});
		return runtime.executeToQueryResult(aggregate, bounds()).getRows().front().at("total").asPrimitive();
	};

	EXPECT_EQ(executeCount(std::make_unique<logical::LogicalVarLengthTraversal>(
				  std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Target"}),
				  "source", "", "target", "KNOWS", "in", 1, 2)),
			PropertyValue(2));
	EXPECT_EQ(executeCount(std::make_unique<logical::LogicalVarLengthTraversal>(
				  std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Middle"}),
				  "source", "", "target", "KNOWS", "both", 1, 2)),
			PropertyValue(2));
	EXPECT_GT(*batchCalls, 0U);
}

TEST(LogicalPlanVarLengthRuntimeTest, MissingRelationshipTypeProducesZeroWithoutScanningAdjacency) {
	auto batchCalls = std::make_shared<size_t>(0U);
	PathBackend backend(true, nullptr, nullptr, nullptr, false, false, nullptr, nullptr, nullptr, batchCalls);
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "MISSING", "out", 1, 2);
	logical::LogicalAggregate aggregate(
			std::move(traversal), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("target"), "total"}});

	const QueryResult result = runtime.executeToQueryResult(aggregate, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(0));
	EXPECT_EQ(*batchCalls, 0U);
}

TEST(LogicalPlanVarLengthRuntimeTest, FallsBackWhenBatchAdjacencyIsUnavailable) {
	PathBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 1, 2);
	logical::LogicalAggregate aggregate(
			std::move(traversal), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("target"), "total"}});

	const QueryResult result = runtime.executeToQueryResult(aggregate, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(2));
}

TEST(LogicalPlanVarLengthRuntimeTest, ReusesOneBoundedReverseEnvelopeForAnExactTarget) {
	auto batchCalls = std::make_shared<size_t>(0U);
	auto referenceCalls = std::make_shared<size_t>(0U);
	auto exactLookups = std::make_shared<size_t>(0U);
	PathBackend backend(true, referenceCalls, nullptr, nullptr, false, false, nullptr, nullptr, nullptr,
			batchCalls, exactLookups);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalVarLengthTraversal traversal(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 1, 2, std::vector<std::string>{"Target"},
			{{"id", PropertyValue("target")}});

	const QueryResult result = runtime.executeToQueryResult(traversal, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("target").asLogicalNode().node_id, 3U);
	EXPECT_EQ(*exactLookups, 1U);
	EXPECT_EQ(*batchCalls, 2U);
	EXPECT_EQ(*referenceCalls, 2U);
}

TEST(LogicalPlanVarLengthRuntimeTest, AccountsReverseReachabilityEnvelopeAgainstSharedByteBound) {
	PathBackend backend(true);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalVarLengthTraversal traversal(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 1, 2, std::vector<std::string>{"Target"},
			{{"id", PropertyValue("target")}});
	auto limited = bounds();
	limited.maximum_materialized_bytes = 128U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(traversal, limited)), std::runtime_error);

	limited.maximum_materialized_bytes = 65536U;
	const QueryResult result = runtime.executeToQueryResult(traversal, limited);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("target").asLogicalNode().node_id, 3U);
}

TEST(LogicalPlanVarLengthRuntimeTest, ExactTargetMissStopsBeforeAdjacencyTraversal) {
	auto batchCalls = std::make_shared<size_t>(0U);
	auto referenceCalls = std::make_shared<size_t>(0U);
	auto exactLookups = std::make_shared<size_t>(0U);
	PathBackend backend(true, referenceCalls, nullptr, nullptr, false, false, nullptr, nullptr, nullptr,
			batchCalls, exactLookups);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalVarLengthTraversal traversal(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 1, 2, std::vector<std::string>{"Target"},
			{{"id", PropertyValue("missing")}});

	EXPECT_EQ(runtime.executeToQueryResult(traversal, bounds()).rowCount(), 0U);
	EXPECT_EQ(*exactLookups, 1U);
	EXPECT_EQ(*batchCalls, 0U);
	EXPECT_EQ(*referenceCalls, 0U);
}

TEST(LogicalPlanVarLengthRuntimeTest, ReverseEnvelopeHonorsTheQueryWideEdgeBound) {
	PathBackend backend(true);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalVarLengthTraversal traversal(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 1, 2, std::vector<std::string>{"Target"},
			{{"id", PropertyValue("target")}});
	auto bounded = bounds();
	bounded.maximum_edge_records = 1U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(traversal, bounded)), std::runtime_error);
}

TEST(LogicalPlanVarLengthRuntimeTest, DefersNativeTopologyOnlyTargetNodeMaterialization) {
	auto nodeReads = std::make_shared<size_t>(0U);
	auto referenceCalls = std::make_shared<size_t>(0U);
	PathBackend backend(true, referenceCalls, nullptr, nodeReads);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalVarLengthTraversal traversal(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 1, 2);

	const QueryResult result = runtime.executeToQueryResult(traversal, bounds());
	ASSERT_EQ(result.rowCount(), 2U);
	EXPECT_EQ(result.getRows()[0].at("target").asLogicalNode().node_id, 2U);
	EXPECT_EQ(result.getRows()[1].at("target").asLogicalNode().node_id, 3U);
	EXPECT_GT(*referenceCalls, 0U);
	// The public result boundary materializes the two target references once;
	// the traversal itself never performs a full endpoint Node read.
	EXPECT_EQ(*nodeReads, 2U);
}

TEST(LogicalPlanVarLengthRuntimeTest, BatchesGenericTopologyEndpointValidation) {
	auto batchCalls = std::make_shared<size_t>(0U);
	PathBackend backend(false, nullptr, nullptr, nullptr, false, true, batchCalls);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalVarLengthTraversal traversal(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "", "out", 1, 1);

	const QueryResult result = runtime.executeToQueryResult(traversal, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_GT(*batchCalls, 0U);
}

TEST(LogicalPlanVarLengthRuntimeTest, RejectsDanglingNativeTopologyEndpointBeforeEmittingRows) {
	PathBackend backend(true, nullptr, nullptr, nullptr, true);
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalVarLengthTraversal traversal(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"", "KNOWS", "out", 1, 2);

	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(traversal, bounds())), std::runtime_error);
}

TEST(LogicalPlanVarLengthRuntimeTest, MaterializesNativePathEdgeReferencesOnlyAtNamedPathBoundary) {
	auto edgeReads = std::make_shared<size_t>(0U);
	auto edgeBatchReads = std::make_shared<size_t>(0U);
	auto nodeBatchReads = std::make_shared<size_t>(0U);
	PathBackend backend(true, nullptr, edgeReads, nullptr, false, false, nullptr, edgeBatchReads, nodeBatchReads);
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 2, 2);
	logical::LogicalNamedPath path(std::move(traversal), "p", std::vector<std::string>{"source", "target"},
								  std::vector<std::string>{});

		const QueryResult result = runtime.executeToQueryResult(path, bounds());
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("p").asPrimitive().getList().size(), 5U);
	EXPECT_EQ(*edgeReads, 0U);
	EXPECT_EQ(*edgeBatchReads, 1U);
	EXPECT_GE(*nodeBatchReads, 1U);
}

TEST(LogicalPlanVarLengthRuntimeTest, AdmitsDeferredNamedPathRecordResultsBeforeCopyingPayloads) {
	const auto makePath = [] {
		auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}), "source", "",
			"target", "KNOWS", "out", 2, 2);
		return logical::LogicalNamedPath(std::move(traversal), "p",
			std::vector<std::string>{"source", "target"}, {});
	};
	bool nodeResultRejected = false;
	bool edgeResultRejected = false;
	for (size_t budget = 1024U; budget <= 131072U; budget += 64U) {
		auto limited = bounds();
		limited.maximum_materialized_bytes = budget;
		PathBackend backend(true, 4096U);
		LogicalPlanReadRuntime runtime(backend);
		try {
			static_cast<void>(runtime.executeToQueryResult(makePath(), limited));
		} catch (const std::runtime_error &error) {
			nodeResultRejected |= std::string_view(error.what()) ==
				"logical-plan Node reference result exceeded its shared byte bound";
			edgeResultRejected |= std::string_view(error.what()) ==
				"logical-plan Edge reference result exceeded its shared byte bound";
		}
		if (nodeResultRejected && edgeResultRejected) break;
	}
	EXPECT_TRUE(nodeResultRejected);
	EXPECT_TRUE(edgeResultRejected);
	PathBackend backend(true, 4096U);
	LogicalPlanReadRuntime runtime(backend);
	auto ample = bounds();
	ample.maximum_materialized_bytes = 262144U;
	const QueryResult result = runtime.executeToQueryResult(makePath(), ample);
	ASSERT_EQ(result.rowCount(), 1U);
	const auto &path = result.getRows().front().at("p").asPrimitive().getList();
	ASSERT_EQ(path.size(), 5U);
	EXPECT_EQ(path[1].getMap().at("payload"), PropertyValue(std::string(4096U, 'e')));
	EXPECT_EQ(path[2].getMap().at("payload"), PropertyValue(std::string(4096U, 'n')));
}

TEST(LogicalPlanVarLengthRuntimeTest, AdmitsInlineNamedPathRecordPayloadsBeforeCopying) {
	const auto makePath = [] {
		return logical::LogicalNamedPath(std::make_unique<logical::LogicalSingleRow>(), "p",
			std::vector<std::string>{"source", "target"}, std::vector<std::string>{"relationship"});
	};
	PathBackend backend(false, 4096U);
	LogicalPlanReadRuntime runtime(backend);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	LogicalPlanReadRow input;
	input.values.emplace("source", snapshot->readNode(1U));
	input.values.emplace("target", snapshot->readNode(2U));
	input.values.emplace("relationship", snapshot->readEdge(10U));
	bool nodePayloadRejected = false;
	bool edgePayloadRejected = false;
	bool inputRowRejected = false;
	for (size_t budget = 1024U; budget <= 131072U; budget += 64U) {
		auto limited = bounds();
		limited.maximum_materialized_bytes = budget;
		try {
			static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, makePath(), limited, input,
				[](const LogicalPlanReadRow &) { return true; }, QueryContext{}));
		} catch (const std::runtime_error &error) {
			inputRowRejected |= std::string_view(error.what()) ==
				"logical-plan NamedPath input row exceeded its shared byte bound";
			nodePayloadRejected |= std::string_view(error.what()) ==
				"logical-plan NamedPath inline Node payload exceeded its shared byte bound";
			edgePayloadRejected |= std::string_view(error.what()) ==
				"logical-plan NamedPath inline Edge payload exceeded its shared byte bound";
		}
		if (inputRowRejected && nodePayloadRejected && edgePayloadRejected) break;
	}
	EXPECT_TRUE(inputRowRejected);
	EXPECT_TRUE(nodePayloadRejected);
	EXPECT_TRUE(edgePayloadRejected);
	auto ample = bounds();
	ample.maximum_materialized_bytes = 262144U;
	size_t delivered = 0U;
	static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, makePath(), ample, input,
		[&](const LogicalPlanReadRow &row) {
			++delivered;
			const auto &path = std::get<PropertyValue>(row.values.at("p")).getList();
			EXPECT_EQ(path.size(), 3U);
			EXPECT_EQ(path[1].getMap().at("payload"), PropertyValue(std::string(4096U, 'e')));
			EXPECT_EQ(path[2].getMap().at("payload"), PropertyValue(std::string(4096U, 'n')));
			return true;
		}, QueryContext{}));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanVarLengthRuntimeTest, BatchesDeferredPathReferencesAcrossUnionRows) {
	auto edgeBatchReads = std::make_shared<size_t>(0U);
	auto nodeBatchReads = std::make_shared<size_t>(0U);
	PathBackend backend(true, nullptr, nullptr, nullptr, false, false, nullptr, edgeBatchReads, nodeBatchReads);
	LogicalPlanReadRuntime runtime(backend);
	const auto makeTraversal = [] {
		return std::make_unique<logical::LogicalVarLengthTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}),
				"source", "", "target", "KNOWS", "out", 1, 2);
	};
	auto unionAll = std::make_unique<logical::LogicalUnion>(makeTraversal(), makeTraversal(), true);
	logical::LogicalNamedPath path(std::move(unionAll), "p", std::vector<std::string>{"source", "target"}, {});

	const QueryResult result = runtime.executeToQueryResult(path, bounds());
	ASSERT_EQ(result.rowCount(), 4U);
	// The QueryResult boundary allows a bounded read-ahead. All path references
	// from both UNION branches are prefetched in one shaped batch before each
	// NamedPath row is expanded, while row order and duplicates remain intact.
	EXPECT_EQ(*edgeBatchReads, 1U);
	EXPECT_EQ(*nodeBatchReads, 1U);
	for (const auto &row: result.getRows()) {
		const size_t pathSize = row.at("p").asPrimitive().getList().size();
		EXPECT_TRUE(pathSize == 3U || pathSize == 5U);
	}
}

TEST(LogicalPlanVarLengthRuntimeTest, SharesNamedPathInputBatchBytesWithReferencePrefetch) {
	const auto makeTraversal = [] {
		return std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}),
			"source", "", "target", "KNOWS", "out", 2, 2);
	};
	const auto makeSinglePath = [&] {
		return logical::LogicalNamedPath(makeTraversal(), "p", std::vector<std::string>{"source", "target"}, {});
	};
	const auto makeDoublePath = [&] {
		return logical::LogicalLimit(
			std::make_unique<logical::LogicalNamedPath>(
				std::make_unique<logical::LogicalUnion>(makeTraversal(), makeTraversal(), true),
				"p", std::vector<std::string>{"source", "target"}, std::vector<std::string>{}), 1);
	};
	// LIMIT keeps the final result to one row; the two input rows still share a
	// NamedPath batch. Find the pressure interval without assuming host sizes.
	bool rejectedPrefetchScratch = false;
	for (size_t budget = 512U; budget <= 16384U; budget += 16U) {
		auto limited = bounds();
		limited.maximum_materialized_bytes = budget;
		PathBackend backend(true);
		LogicalPlanReadRuntime runtime(backend);
		try {
			static_cast<void>(runtime.executeToQueryResult(makeDoublePath(), limited));
		} catch (const std::runtime_error &error) {
			if (std::string_view(error.what()) ==
					"logical-plan reference prefetch scratch exceeded its shared byte bound") {
				rejectedPrefetchScratch = true;
				break;
			}
		}
	}
	EXPECT_TRUE(rejectedPrefetchScratch);
	bool rejectedRetainedRows = false;
	for (size_t budget = 1024U; budget <= 16384U; budget += 32U) {
		auto limited = bounds();
		limited.maximum_materialized_bytes = budget;
		try {
			PathBackend singleBackend(true);
			LogicalPlanReadRuntime singleRuntime(singleBackend);
			if (singleRuntime.executeToQueryResult(makeSinglePath(), limited).rowCount() != 1U) continue;
		} catch (const std::runtime_error &) {
			continue;
		}
		PathBackend doubleBackend(true);
		LogicalPlanReadRuntime doubleRuntime(doubleBackend);
		try {
			static_cast<void>(doubleRuntime.executeToQueryResult(makeDoublePath(), limited));
		} catch (const std::runtime_error &error) {
			if (std::string_view(error.what()) == "shared materialization exceeded its byte bound") {
				rejectedRetainedRows = true;
				break;
			}
		}
	}
	EXPECT_TRUE(rejectedRetainedRows);
	PathBackend highBackend(true);
	LogicalPlanReadRuntime highRuntime(highBackend);
	auto high = bounds();
	high.maximum_materialized_bytes = 65536U;
	const QueryResult result = highRuntime.executeToQueryResult(makeDoublePath(), high);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("p").asPrimitive().getList().size(), 5U);
}

TEST(LogicalPlanVarLengthRuntimeTest, PublicVisitorKeepsNamedPathReadAheadDisabled) {
	auto edgeBatchReads = std::make_shared<size_t>(0U);
	PathBackend backend(true, nullptr, nullptr, nullptr, false, false, nullptr, edgeBatchReads);
	LogicalPlanReadRuntime runtime(backend);
	const auto makeTraversal = [] {
		return std::make_unique<logical::LogicalVarLengthTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}),
				"source", "", "target", "KNOWS", "out", 1, 2);
	};
	auto unionAll = std::make_unique<logical::LogicalUnion>(makeTraversal(), makeTraversal(), true);
	logical::LogicalNamedPath path(std::move(unionAll), "p", std::vector<std::string>{"source", "target"}, {});
	uint64_t delivered = 0U;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(path, bounds(), [&](const LogicalPlanReadRow &) {
		++delivered;
		return false;
	});

	EXPECT_EQ(delivered, 1U);
	EXPECT_EQ(stats.rows_emitted, 1U);
	EXPECT_EQ(*edgeBatchReads, 1U);
}

TEST(LogicalPlanVarLengthRuntimeTest, TraversesIncomingAndBothDirectionsWithTargetBounds) {
	PathBackend backend;
	LogicalPlanReadRuntime runtime(backend);

	logical::LogicalVarLengthTraversal incoming(
			std::make_unique<logical::LogicalNodeScan>("target", std::vector<std::string>{"Target"}), "target", "",
			"source", "KNOWS", "in", 1, 1, std::vector<std::string>{"Middle"});
	const QueryResult incomingResult = runtime.executeToQueryResult(incoming, bounds());
	ASSERT_EQ(incomingResult.rowCount(), 1U);
	EXPECT_EQ(incomingResult.getRows().front().at("source").asLogicalNode().node_id, 2U);

	logical::LogicalVarLengthTraversal both(
			std::make_unique<logical::LogicalNodeScan>("middle", std::vector<std::string>{"Middle"}), "middle", "",
			"neighbor", "KNOWS", "both", 1, 1);
	const QueryResult bothResult = runtime.executeToQueryResult(both, bounds());
	ASSERT_EQ(bothResult.rowCount(), 2U);
	EXPECT_EQ(bothResult.getRows()[0].at("neighbor").asLogicalNode().node_id, 3U);
	EXPECT_EQ(bothResult.getRows()[1].at("neighbor").asLogicalNode().node_id, 1U);
}

TEST(LogicalPlanVarLengthRuntimeTest, RejectsInvalidDirectionDepthAndSourceShapes) {
	PathBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const auto scan = [] { return std::make_unique<logical::LogicalNodeScan>("source"); };

	logical::LogicalVarLengthTraversal unknownDirection(scan(), "source", "", "target", "KNOWS", "sideways", 1, 1);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(unknownDirection, bounds())), std::invalid_argument);
	logical::LogicalVarLengthTraversal negativeMinimum(scan(), "source", "", "target", "KNOWS", "out", -1, 1);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(negativeMinimum, bounds())), std::invalid_argument);
	logical::LogicalVarLengthTraversal reversedRange(scan(), "source", "", "target", "KNOWS", "out", 2, 1);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(reversedRange, bounds())), std::invalid_argument);
	logical::LogicalVarLengthTraversal namedEdges(scan(), "source", "edges", "target", "KNOWS", "out", 1, 1);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(namedEdges, bounds())), std::invalid_argument);

	QueryContext negativeDepth;
	negativeDepth.maxVarLengthDepth = -1;
	logical::LogicalVarLengthTraversal bounded(scan(), "source", "", "target", "KNOWS", "out", 1, 1);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(bounded, bounds(), negativeDepth)), std::invalid_argument);
	QueryContext tooShallow;
	tooShallow.maxVarLengthDepth = 0;
	EXPECT_EQ(runtime.executeToQueryResult(bounded, bounds(), tooShallow).rowCount(), 0U);
}

TEST(LogicalPlanVarLengthRuntimeTest, ScalarTopologyOutputDefersSourceIncludingZeroHopAndEarlyStop) {
	for (const bool nativeRefs: {false, true}) {
		for (const int minimumHops: {0, 1}) {
			auto nodeReads = std::make_shared<size_t>(0U);
			auto edgeReads = std::make_shared<size_t>(0U);
			PathBackend backend(nativeRefs, nullptr, edgeReads, nodeReads, false, true);
			LogicalPlanReadRuntime runtime(backend);
			auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
					std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}),
					"source", "", "target", "KNOWS", "out", minimumHops, 2);
			std::vector<std::unique_ptr<expressions::Expression>> arguments;
			arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("target"));
			std::vector<logical::LogicalProjectItem> items;
			items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("id", std::move(arguments)),
					"targetId");
			logical::LogicalProject project(std::move(traversal), std::move(items));
			size_t delivered = 0U;
			const auto stats = runtime.execute(project, bounds(), [&](const LogicalPlanReadRow &row) {
				++delivered;
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("targetId")),
						PropertyValue(int64_t{minimumHops == 0 ? 1 : 2}));
				return false;
			});
			EXPECT_EQ(delivered, 1U);
			EXPECT_EQ(stats.rows_emitted, 1U);
			EXPECT_EQ(*nodeReads, 0U);
			EXPECT_EQ(*edgeReads, 0U);
		}
	}
}

TEST(LogicalPlanVarLengthRuntimeTest, ZeroHopNativeCountDoesNotReadCompleteSource) {
	auto nodeReads = std::make_shared<size_t>(0U);
	auto batchCalls = std::make_shared<size_t>(0U);
	PathBackend backend(true, nullptr, nullptr, nodeReads, false, false, nullptr, nullptr, nullptr, batchCalls);
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}),
			"source", "", "target", "KNOWS", "out", 0, 0);
	logical::LogicalAggregate aggregate(std::move(traversal), {},
			{{"count", std::make_shared<expressions::VariableReferenceExpression>("target"), "total"}});
	const auto result = runtime.executeToQueryResult(aggregate, bounds());
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("total").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(*nodeReads, 0U);
	EXPECT_EQ(*batchCalls, 0U);
}

TEST(LogicalPlanVarLengthRuntimeTest, BatchesPredicateEndpointsInSortedUniqueOrderAndRestoresDuplicates) {
	for (const bool nativeRefs: {false, true}) {
		auto nodeReads = std::make_shared<size_t>(0U);
		auto batchReads = std::make_shared<size_t>(0U);
		PathBackend backend(nativeRefs, nullptr, nullptr, nodeReads, false, true, nullptr,
				nullptr, batchReads, nullptr, nullptr, true);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
				std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}),
				"source", "", "target", "KNOWS", "out", 1, 1,
				std::vector<std::string>{"Middle"});
		std::vector<std::unique_ptr<expressions::Expression>> arguments;
		arguments.push_back(std::make_unique<expressions::VariableReferenceExpression>("target"));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::FunctionCallExpression>("id", std::move(arguments)), "value");
		logical::LogicalProject project(std::move(traversal), std::move(items));
		auto bounded = bounds();
		bounded.maximum_adjacency_page_records = 4U;
		const auto result = runtime.executeToQueryResult(project, bounded);
		ASSERT_EQ(result.rowCount(), 2U);
		for (const auto &row: result.getRows()) EXPECT_EQ(row.at("value").asPrimitive(), PropertyValue(2));
		EXPECT_EQ(*batchReads, 1U);
		EXPECT_EQ(*nodeReads, 0U);
		*batchReads = 0U;
		*nodeReads = 0U;
		size_t delivered = 0U;
		static_cast<void>(runtime.execute(project, bounded, [&](const LogicalPlanReadRow &row) {
			++delivered;
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("value")), PropertyValue(2));
			return false;
		}));
		EXPECT_EQ(delivered, 1U);
		// A public callback keeps predicate reads lazy: reject target 3, emit target
		// 2, and stop before reading the third edge's repeated target 2.
		EXPECT_EQ(*batchReads, 0U);
		EXPECT_EQ(*nodeReads, 2U);
	}
}

TEST(LogicalPlanVarLengthRuntimeTest, RejectsMalformedOrOversizedPredicateEndpointBatches) {
	for (const bool nativeRefs: {false, true}) {
		for (const int fault: {1, 2, 3, 4}) {
			auto batchReads = std::make_shared<size_t>(0U);
			PathBackend backend(nativeRefs, nullptr, nullptr, nullptr, false, true, nullptr,
					nullptr, batchReads, nullptr, nullptr, true, fault);
			LogicalPlanReadRuntime runtime(backend);
			logical::LogicalVarLengthTraversal traversal(
					std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}),
					"source", "", "target", "KNOWS", "out", 1, 1,
					std::vector<std::string>{"Middle"});
			auto bounded = bounds();
			bounded.maximum_adjacency_page_records = 4U;
			bounded.maximum_materialized_bytes = 4096U;
			EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(traversal, bounded)), std::runtime_error);
		}
	}
}

TEST(LogicalPlanVarLengthRuntimeTest, SinglePredicateTargetRejectsBeforeReadingNextNodeAtSharedBudgetLimit) {
	for (const bool nativeRefs: {false, true}) {
		auto nodeReads = std::make_shared<size_t>(0U);
		PathBackend backend(nativeRefs, nullptr, nullptr, nodeReads, false, false, nullptr,
				nullptr, nullptr, nullptr, nullptr, false, 0, 2048U);
		LogicalPlanReadRuntime runtime(backend);
		const auto makeTraversal = [] {
			return logical::LogicalVarLengthTraversal(
					std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}),
					"source", "", "target", "KNOWS", "out", 2, 2,
					std::vector<std::string>{"Target"});
		};
		bool rejectedSharedPeak = false;
		for (size_t budget = 2048U; budget <= 8192U; budget += 8U) {
			*nodeReads = 0U;
			auto limited = bounds();
			limited.maximum_materialized_bytes = budget;
			try {
				static_cast<void>(runtime.execute(makeTraversal(), limited,
						[](const LogicalPlanReadRow &) { return false; }));
			} catch (const std::runtime_error &error) {
				if (std::string_view(error.what()) ==
						"logical-plan traversal predicate target exceeded its shared byte bound") {
					EXPECT_EQ(*nodeReads, 1U);
					rejectedSharedPeak = true;
					break;
				}
			}
		}
		EXPECT_TRUE(rejectedSharedPeak);
		auto ample = bounds();
		ample.maximum_materialized_bytes = 16384U;
		size_t delivered = 0U;
		static_cast<void>(runtime.execute(makeTraversal(), ample, [&](const LogicalPlanReadRow &) {
			++delivered;
			return false;
		}));
		EXPECT_EQ(delivered, 1U);
	}
}

TEST(LogicalPlanVarLengthRuntimeTest, SortedOutputPreservesPathAfterChargedHandoffAndEarlyStop) {
	PathBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const auto makeSorted = [] {
		auto traversal = std::make_unique<logical::LogicalVarLengthTraversal>(
			std::make_unique<logical::LogicalNodeScan>("source", std::vector<std::string>{"Source"}),
			"source", "", "target", "KNOWS", "out", 2, 2, std::vector<std::string>{"Target"});
		std::vector<logical::LogicalSortItem> keys;
		keys.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("target"), true);
		return logical::LogicalSort(std::move(traversal), std::move(keys));
	};
	for (const bool stop : {false, true}) {
		size_t delivered = 0U;
		static_cast<void>(runtime.execute(makeSorted(), bounds(), [&](const LogicalPlanReadRow &row) {
			++delivered;
			const auto &path = row.path_cache.at("target");
			EXPECT_EQ(path.nodes.size(), 3U);
			if (path.nodes.size() != 3U) return false;
			EXPECT_EQ(path.nodes.front().node_id, 1U);
			EXPECT_EQ(path.nodes.back().node_id, 3U);
			EXPECT_EQ(path.edges.size(), 2U);
			return !stop;
		}));
		EXPECT_EQ(delivered, 1U);
	}
}
