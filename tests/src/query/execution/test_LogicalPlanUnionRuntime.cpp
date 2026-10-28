/**
 * @file test_LogicalPlanUnionRuntime.cpp
 * @brief Logical UNION execution tests independent of physical graph storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/query/execution/RetainedPropertyBytes.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"
#include "graph/query/QueryGuard.hpp"
#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/execution/SelectionVector.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalUnion.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"
#include "graph/storage/LogicalGraphRead.hpp"

using namespace graph;
using namespace graph::query;
using namespace graph::query::execution;
using namespace graph::storage;

namespace {

	class NoGraphSnapshot : public LogicalGraphReadSnapshot {
	public:
		[[nodiscard]] LogicalGraphNode readNode(uint64_t) const override { throw std::logic_error("unexpected Node read"); }
		[[nodiscard]] LogicalGraphEdge readEdge(uint64_t) const override { throw std::logic_error("unexpected Edge read"); }
		void visitNodes(size_t, const std::function<bool(const LogicalGraphNode &)> &) const override {
			throw std::logic_error("unexpected Node traversal");
		}
		[[nodiscard]] LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override {
			throw std::logic_error("unexpected Node page scan");
		}
		[[nodiscard]] LogicalGraphAdjacencyPage scanAdjacentEdges(
				uint64_t, LogicalGraphReadDirection, std::optional<LogicalGraphAdjacencyContinuation>, size_t) const override {
			throw std::logic_error("unexpected adjacency scan");
		}
		void visitEdges(size_t, const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("unexpected Edge traversal");
		}
		void visitAdjacentEdges(uint64_t, LogicalGraphReadDirection, size_t,
								const std::function<bool(const LogicalGraphEdge &)> &) const override {
			throw std::logic_error("unexpected adjacency traversal");
		}
	};

	class NoGraphBackend final : public LogicalGraphReadBackend {
	public:
		[[nodiscard]] std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
			return std::make_unique<NoGraphSnapshot>();
		}
	};

	[[nodiscard]] LogicalPlanReadRuntimeBounds bounds() {
		return {
				.maximum_node_records = 16U,
				.maximum_node_page_records = 4U,
				.maximum_metadata_pages_per_node_page = 4U,
				.maximum_edges_per_node = 16U,
				.maximum_edge_records = 32U,
				.maximum_adjacency_page_records = 4U,
				.maximum_property_entries = 16U,
				.maximum_rows = 16U,
				.maximum_distinct_rows = 16U,
		};
	}

} // namespace

TEST(LogicalPlanUnionRuntimeTest, ExecutesParserProducedUnionAndUnionAllWithCypherDistinctSemantics) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;

	QueryPlan distinctPlan = parser.parseToLogical("RETURN 1 AS value UNION RETURN 1 AS value UNION RETURN 2 AS value");
	ASSERT_NE(distinctPlan.root, nullptr);
	const QueryResult distinct = runtime.executeToQueryResult(*distinctPlan.root, bounds());
	ASSERT_EQ(distinct.rowCount(), 2U);
	EXPECT_EQ(distinct.getRows()[0].at("value").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(distinct.getRows()[1].at("value").asPrimitive(), PropertyValue(2));

	QueryPlan allPlan = parser.parseToLogical("RETURN 1 AS value UNION ALL RETURN 1 AS value UNION ALL RETURN 2 AS value");
	ASSERT_NE(allPlan.root, nullptr);
	const QueryResult all = runtime.executeToQueryResult(*allPlan.root, bounds());
	ASSERT_EQ(all.rowCount(), 3U);
	EXPECT_EQ(all.getRows()[0].at("value").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(all.getRows()[1].at("value").asPrimitive(), PropertyValue(1));
	EXPECT_EQ(all.getRows()[2].at("value").asPrimitive(), PropertyValue(2));
}

TEST(LogicalPlanUnionRuntimeTest, RejectsMismatchedOutputColumnsBeforeExecutingEitherBranch) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical("RETURN 1 AS left UNION RETURN 2 AS right");
	ASSERT_NE(plan.root, nullptr);
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds())), std::invalid_argument);
}

TEST(LogicalPlanUnionRuntimeTest, BoundsDistinctMaterializedKeysByBytes) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"RETURN 'a long union value that must be accounted for' AS value "
			"UNION RETURN 'another long union value that must be accounted for' AS value");
	ASSERT_NE(plan.root, nullptr);
	LogicalPlanReadRuntimeBounds constrained = bounds();
	constrained.maximum_materialized_bytes = 8U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, constrained)), std::runtime_error);
}

TEST(LogicalPlanUnionRuntimeTest, StopsBeforeReadingTheSecondBranchAfterDownstreamCancellation) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryPlan plan = parser.parseToLogical(
			"UNWIND [1, 2] AS value RETURN value UNION ALL "
			"UNWIND [3, 4] AS value RETURN value");
	ASSERT_NE(plan.root, nullptr);
	std::vector<PropertyValue> delivered;
	const LogicalPlanReadRuntimeStats stats = runtime.execute(
			*plan.root, bounds(), [&delivered](const LogicalPlanReadRow &row) {
				delivered.push_back(std::get<PropertyValue>(row.values.at("value")));
				return false;
			});
	ASSERT_EQ(delivered.size(), 1U);
	EXPECT_EQ(delivered.front(), PropertyValue(1));
	EXPECT_EQ(stats.rows_emitted, 1U);
}

TEST(LogicalPlanUnionRuntimeTest, SortTransfersRowsAcrossAllAndDistinctUnionBranches) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const auto makeSortedBranch = [](std::vector<PropertyValue> values) {
		auto source = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "value", std::move(values));
		std::vector<logical::LogicalSortItem> keys;
		keys.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("value"), true);
		return std::make_unique<logical::LogicalSort>(std::move(source), std::move(keys));
	};
	for (const bool all: {true, false}) {
		SCOPED_TRACE(all);
		auto branches = std::make_unique<logical::LogicalUnion>(
			makeSortedBranch({PropertyValue(2), PropertyValue(1)}),
			makeSortedBranch({PropertyValue(3), PropertyValue(2)}), all);
		std::vector<logical::LogicalSortItem> outerKeys;
		outerKeys.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("value"), true);
		logical::LogicalSort outer(std::move(branches), std::move(outerKeys));
		auto constrained = bounds();
		constrained.maximum_materialized_bytes = 5000U;
		const QueryResult result = runtime.executeToQueryResult(outer, constrained);
		const std::vector<PropertyValue> expected = all
			? std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(2), PropertyValue(3)}
			: std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2), PropertyValue(3)};
		ASSERT_EQ(result.rowCount(), expected.size());
		for (size_t index = 0U; index < expected.size(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("value").asPrimitive(), expected[index]);
		}
	}
}

TEST(LogicalPlanUnionRuntimeTest, ChargedUnionLimitStopsBeforeEvaluatingTheRightBranch) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	std::vector<logical::LogicalProjectItem> leftItems;
	leftItems.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{1}), "value");
	std::vector<logical::LogicalProjectItem> rightItems;
	rightItems.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("missing"), "value");
	auto branches = std::make_unique<logical::LogicalUnion>(
		std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(), std::move(leftItems)),
		std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(), std::move(rightItems)),
		true);
	logical::LogicalLimit limited(std::move(branches), 1);
	auto constrained = bounds();
	constrained.maximum_materialized_bytes = 2000U;
	const QueryResult result = runtime.executeToQueryResult(limited, constrained);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue(1));
}

TEST(LogicalPlanUnionRuntimeTest, LimitStreamsAnExactBudgetRowWithoutSelectionStorage) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const PropertyValue payload(std::string(1000U, 'x'));
	auto source = std::make_unique<logical::LogicalUnwind>(
		std::make_unique<logical::LogicalSingleRow>(), "value", std::vector<PropertyValue>{payload});
	logical::LogicalLimit limit(std::move(source), 1);
	auto constrained = bounds();
	// Include the one resident output slot reserved while the source is running.
	constrained.maximum_materialized_bytes = sizeof(LogicalPlanReadRow) + sizeof(LogicalPlanReadRow) + std::string("value").size() +
		sizeof(LogicalPlanReadValue) + property_utils::getPropertyValueSize(payload);
	size_t delivered = 0U;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(limit, constrained, [&](const LogicalPlanReadRow &row) {
		++delivered;
		EXPECT_EQ(std::get<PropertyValue>(row.values.at("value")), payload);
		return true;
	})));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanUnionRuntimeTest, ProjectAndUnionDistinctKeysShareTheBudgetWithDownstreamSort) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const PropertyValue a(std::string(1500U, 'a'));
	const PropertyValue b(std::string(1500U, 'b'));
	const auto makeRows = [](std::vector<PropertyValue> values) {
		return std::make_unique<logical::LogicalUnwind>(std::make_unique<logical::LogicalSingleRow>(), "value", std::move(values));
	};
	const auto makeSort = [](std::unique_ptr<logical::LogicalOperator> child) {
		std::vector<logical::LogicalSortItem> keys;
		keys.emplace_back(std::make_shared<expressions::LiteralExpression>(int64_t{0}), true);
		return std::make_unique<logical::LogicalSort>(std::move(child), std::move(keys));
	};
	for (const bool unionDistinct: {false, true}) {
		SCOPED_TRACE(unionDistinct);
		const auto makeDistinct = [&]() -> std::unique_ptr<logical::LogicalOperator> {
			if (unionDistinct) return std::make_unique<logical::LogicalUnion>(makeRows({a, a}), makeRows({b, a}), false);
			std::vector<logical::LogicalProjectItem> items;
			items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("value"), "value");
			return std::make_unique<logical::LogicalProject>(makeRows({a, a, b}), std::move(items), true);
		};
		auto standaloneDistinct = makeDistinct();
		auto standaloneSort = makeSort(makeRows({a, b}));
		auto combined = makeSort(makeDistinct());
		auto constrained = bounds();
		// Measure the standalone admission thresholds rather than assuming a
		// standard-library hash-node size or encoded string growth policy.
		const auto minimumBudget = [&](const logical::LogicalOperator &plan) {
			const auto fits = [&](size_t bytes) {
				auto limits = bounds(); limits.maximum_materialized_bytes = bytes;
				try {
					static_cast<void>(runtime.execute(plan, limits, [](const LogicalPlanReadRow &) { return true; }));
					return true;
				} catch (const std::runtime_error &) { return false; }
			};
			size_t low = 1U, high = 1024U * 1024U;
			EXPECT_TRUE(fits(high));
			while (low < high) {
				const size_t middle = low + (high - low) / 2U;
				if (fits(middle)) high = middle; else low = middle + 1U;
			}
			return low;
		};
		constrained.maximum_materialized_bytes = std::max(minimumBudget(*standaloneDistinct), minimumBudget(*standaloneSort));

		size_t delivered = 0U;
		const auto count = [&](const LogicalPlanReadRow &) { ++delivered; return true; };
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*standaloneDistinct, constrained, count)));
		EXPECT_EQ(delivered, 2U);
		delivered = 0U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*standaloneSort, constrained, count)));
		EXPECT_EQ(delivered, 2U);
		delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(*combined, constrained, count)), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		constrained.maximum_materialized_bytes = 16000U;
		for (const bool stopAfterFirst: {true, false}) {
			std::vector<PropertyValue> output;
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(*combined, constrained, [&](const LogicalPlanReadRow &row) {
				output.push_back(std::get<PropertyValue>(row.values.at("value")));
				return !stopAfterFirst;
			})));
			EXPECT_EQ(output, stopAfterFirst ? std::vector<PropertyValue>{a} : (std::vector<PropertyValue>{a, b}));
		}
	}
}

TEST(LogicalPlanUnionRuntimeTest, SortAdmitsKeySlotsBeforeReservingThem) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	auto input = std::make_unique<logical::LogicalUnwind>(
		std::make_unique<logical::LogicalSingleRow>(), "value",
		std::vector<PropertyValue>{PropertyValue(2), PropertyValue(1)});
	std::vector<logical::LogicalSortItem> keys;
	keys.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("value"), true);
	logical::LogicalSort sort(std::move(input), std::move(keys));
	auto constrained = bounds();
	const size_t rowBytes = sizeof(LogicalPlanReadRow) + std::string("value").size() +
		sizeof(LogicalPlanReadValue) + sizeof(int64_t);
	// The runtime also reserves the output row and the Sort row slots. Walk
	// those unrelated admission boundaries until the key-slot admission itself
	// is the first request that exceeds the shared allowance.
	const size_t minimumBudget = sizeof(LogicalPlanReadRow) +
		2U * rowBytes + 2U * sizeof(PropertyValue) - 1U;
	size_t delivered = 0U;
	bool keySlotsRejected = false;
	debug::PerfTrace::ScopedObserver observer(&keySlotsRejected,
		[](void *owner, std::string_view key, int64_t, bool) noexcept {
			if (key == "query.sort.key_slot_admission_rejected") *static_cast<bool *>(owner) = true;
		});
	for (size_t budget = 1U; budget < minimumBudget + 4096U; ++budget) {
		constrained.maximum_materialized_bytes = budget;
		delivered = 0U;
		try {
			static_cast<void>(runtime.execute(sort, constrained, [&](const LogicalPlanReadRow &) {
				++delivered;
				return true;
			}));
			break;
		} catch (const MaterializedByteLimitExceeded &) {
			if (keySlotsRejected) break;
			// Earlier shared-account admissions are expected at smaller budgets.
		} catch (const std::invalid_argument &error) {
			if (std::string_view(error.what()) != "logical-plan output batch storage exhausts its materialized byte bound") {
				ADD_FAILURE() << "unexpected constrained Sort failure: " << error.what();
				break;
			}
		} catch (const std::runtime_error &error) {
			if (std::string_view(error.what()) != "shared materialization exceeded its byte bound" &&
				std::string_view(error.what()) != "logical-plan output materialization exceeds its row or byte bound") {
				ADD_FAILURE() << "unexpected constrained Sort failure: " << error.what();
				break;
			}
		}
	}
	EXPECT_TRUE(keySlotsRejected) << "could not reach Sort key-slot admission after row-slot reservations";
	EXPECT_EQ(delivered, 0U);
	constrained.maximum_materialized_bytes = bounds().maximum_materialized_bytes;
	std::vector<PropertyValue> output;
	static_cast<void>(runtime.execute(sort, constrained, [&](const LogicalPlanReadRow &row) {
		output.push_back(std::get<PropertyValue>(row.values.at("value")));
		return true;
	}));
	EXPECT_EQ(output, (std::vector<PropertyValue>{PropertyValue(1), PropertyValue(2)}));
}

TEST(LogicalPlanUnionRuntimeTest, SortTransfersLargeRowsToDirectLimitAndSkipBuffers) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	const auto makeSort = [] {
		std::vector<PropertyValue> input;
		for (char value: {'d', 'b', 'a', 'c'}) input.emplace_back(std::string(4096U, value));
		auto source = std::make_unique<logical::LogicalUnwind>(
			std::make_unique<logical::LogicalSingleRow>(), "value", std::move(input));
		std::vector<logical::LogicalSortItem> keys;
		keys.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("value"), true);
		return std::make_unique<logical::LogicalSort>(std::move(source), std::move(keys));
	};
	const size_t rowBytes = sizeof(LogicalPlanReadRow) + std::string("value").size() +
		sizeof(LogicalPlanReadValue) + 4096U + sizeof(size_t);
	auto constrained = bounds();
	constrained.maximum_materialized_bytes = 4U * rowBytes +
		4U * sizeof(PropertyValue) + 4U * (4096U + sizeof(size_t)) +
		// The public visitor retains one output-row slot alongside the Sort.
		SelectionVector::requiredStorageBytes(4U) + sizeof(LogicalPlanReadRow);
	logical::LogicalLimit limit(makeSort(), 1);
	std::vector<PropertyValue> limited;
	static_cast<void>(runtime.execute(limit, constrained, [&](const LogicalPlanReadRow &row) {
		limited.push_back(std::get<PropertyValue>(row.values.at("value")));
		return true;
	}));
	EXPECT_EQ(limited, (std::vector<PropertyValue>{PropertyValue(std::string(4096U, 'a'))}));
	logical::LogicalSkip skip(makeSort(), 3);
	std::vector<PropertyValue> skipped;
	static_cast<void>(runtime.execute(skip, constrained, [&](const LogicalPlanReadRow &row) {
		skipped.push_back(std::get<PropertyValue>(row.values.at("value")));
		return true;
	}));
	EXPECT_EQ(skipped, (std::vector<PropertyValue>{PropertyValue(std::string(4096U, 'd'))}));
}

TEST(LogicalPlanUnionRuntimeTest, DistinctCancellationDoesNotEvaluateTheSecondBranch) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	const auto plan = parser.parseToLogical("RETURN 1 AS value UNION RETURN $missing AS value");
	for (size_t execution = 0U; execution < 2U; ++execution) {
		size_t delivered = 0U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, bounds(), [&](const LogicalPlanReadRow &row) {
			++delivered;
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("value")), PropertyValue(1));
			return false;
		})));
		EXPECT_EQ(delivered, 1U);
	}
}

TEST(LogicalPlanUnionRuntimeTest, ImportedRowCachesChargeNestedPropertySlotsAndRetry) {
	NoGraphBackend backend;
	NoGraphSnapshot snapshot;
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalSingleRow root;
	QueryContext context;
	const PropertyValue payload(std::vector<PropertyValue>(128U));
	for (const bool metadata : {false, true}) {
		SCOPED_TRACE(metadata);
		LogicalPlanReadRow input;
		auto &cache = metadata ? input.entity_metadata_cache : input.property_cache;
		cache["n"].emplace("payload", payload);
		auto limited = bounds();
		limited.maximum_materialized_bytes = 1024U;
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
			snapshot, root, limited, input, [&](const LogicalPlanReadRow &) {
				++delivered; return true;
			}, context)), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		EXPECT_EQ(cache.at("n").at("payload"), payload);
		limited.maximum_materialized_bytes = 65536U;
		EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
			snapshot, root, limited, input, [&](const LogicalPlanReadRow &row) {
				const auto &output = metadata ? row.entity_metadata_cache : row.property_cache;
				EXPECT_EQ(output.at("n").at("payload"), payload);
				++delivered; return true;
			}, context)));
		EXPECT_EQ(delivered, 1U);
	}
}

TEST(LogicalPlanUnionRuntimeTest, CompleteEntitiesChargeNestedPropertySlotsAndRetry) {
	using namespace graph::query::execution::direct_edge_test;
	graph::parser::cypher::CypherParserImpl parser;
	const PropertyValue payload(std::vector<PropertyValue>(128U));
	for (const bool edge : {false, true}) {
		SCOPED_TRACE(edge);
		DirectEdgeGraph graph = directGraph();
		for (auto &[id, node] : graph.nodes) node.properties = {{"payload", payload}};
		for (auto &record : graph.edges) record.properties = {{"payload", payload}};
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		const auto plan = parser.parseToLogical(edge ? "MATCH ()-[r]->() RETURN r" : "MATCH (n) RETURN n");
		auto limited = directBounds();
		limited.maximum_materialized_bytes = 4096U;
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
			[&](const LogicalPlanReadRow &) { ++delivered; return true; })), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		limited.maximum_materialized_bytes = 1024U * 1024U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
			[&](const LogicalPlanReadRow &row) {
				const auto &value = row.values.at(edge ? "r" : "n");
				const auto &properties = edge ? std::get<LogicalGraphEdge>(value).properties :
					std::get<LogicalGraphNode>(value).properties;
				EXPECT_EQ(properties.size(), 1U);
				if (!properties.empty()) EXPECT_EQ(properties.front().value, payload);
				++delivered; return true;
			})));
		EXPECT_EQ(delivered, edge ? graph.edges.size() : graph.nodes.size());
	}
}

TEST(LogicalPlanUnionRuntimeTest, ImportedEntitiesChargeSourceSpareSlotsAndRetry) {
	class SpareSnapshot final : public NoGraphSnapshot {
	public:
		LogicalGraphNode readNode(uint64_t id) const override {
			LogicalGraphNode node;
			node.node_id = id;
			node.labels.reserve(512U);
			node.labels.emplace_back("Person");
			node.properties.reserve(512U);
			node.properties.push_back({"value", PropertyValue(int64_t{7})});
			return node;
		}
		LogicalGraphNode readNodeShaped(uint64_t id, const EntityReadShape &) const override {
			return readNode(id);
		}
		LogicalGraphEdge readEdge(uint64_t id) const override {
			LogicalGraphEdge edge;
			edge.edge_id = id;
			edge.source_node_id = 1U;
			edge.target_node_id = 2U;
			edge.type = "KNOWS";
			edge.properties.reserve(512U);
			edge.properties.push_back({"value", PropertyValue(int64_t{7})});
			return edge;
		}
		LogicalGraphEdge readEdgeShaped(uint64_t id, const EntityReadShape &) const override {
			return readEdge(id);
		}
	} snapshot;
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalSingleRow root;
	QueryContext context;
	for (const bool edge : {false, true}) {
		SCOPED_TRACE(edge);
		LogicalPlanReadRow input;
		if (edge) input.values.emplace("entity", snapshot.readEdge(1U));
		else input.values.emplace("entity", snapshot.readNode(1U));
		auto limited = bounds();
		limited.maximum_materialized_bytes = 4096U;
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
			snapshot, root, limited, input, [&](const LogicalPlanReadRow &) {
				++delivered; return true;
			}, context)), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		limited.maximum_materialized_bytes = 1024U * 1024U;
		EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
			snapshot, root, limited, input, [&](const LogicalPlanReadRow &row) {
				const auto &value = row.values.at("entity");
				const auto &properties = edge ? std::get<LogicalGraphEdge>(value).properties :
					std::get<LogicalGraphNode>(value).properties;
				EXPECT_EQ(properties.size(), 1U);
				if (!properties.empty()) EXPECT_EQ(properties.front().value, PropertyValue(int64_t{7}));
				++delivered; return true;
			}, context)));
		EXPECT_EQ(delivered, 1U);
	}
}

TEST(LogicalPlanUnionRuntimeTest, ComparisonAndInTemporariesChargeNestedListSlots) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryContext context;
	const PropertyValue values(std::vector<PropertyValue>(128U));
	context.parameters.emplace("values", values);
	for (const bool membership : {false, true}) {
		SCOPED_TRACE(membership);
		const auto plan = parser.parseToLogical(membership ?
			"RETURN 7 IN $values AS result" : "RETURN $values = $values AS result");
		auto limited = bounds();
		limited.maximum_rows = 256U;
		limited.maximum_materialized_bytes = 1024U;
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
			[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context)), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		EXPECT_EQ(context.parameters.at("values"), values);
		limited.maximum_materialized_bytes = 65536U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
			[&](const LogicalPlanReadRow &row) {
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("result")),
					membership ? PropertyValue() : PropertyValue(true));
				++delivered; return true;
			}, context)));
		EXPECT_EQ(delivered, 1U);
	}
}

TEST(LogicalPlanUnionRuntimeTest, FunctionAndCaseTemporariesChargeCapacityWithoutAccumulatingDeadBranches) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryContext context;
	const PropertyValue values(std::vector<PropertyValue>(128U));
	context.parameters.emplace("values", values);
	context.parameters.emplace("other", PropertyValue(std::vector<PropertyValue>(128U, PropertyValue(int64_t{7}))));
	for (const bool simpleCase : {false, true}) {
		SCOPED_TRACE(simpleCase);
		const auto plan = parser.parseToLogical(simpleCase ?
			"RETURN CASE $values WHEN $other THEN 1 WHEN $other THEN 2 WHEN $other THEN 3 ELSE 0 END AS result" :
			"RETURN size($values) AS result");
		auto limited = bounds();
		limited.maximum_rows = 256U;
		limited.maximum_materialized_bytes = 1024U;
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
			[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context)), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		// Enough for the comparison and one WHEN, but not three retained WHENs.
		limited.maximum_materialized_bytes = 2U * 128U * sizeof(PropertyValue) + 1024U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
			[&](const LogicalPlanReadRow &row) {
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("result")), PropertyValue(int64_t{simpleCase ? 0 : 128}));
				++delivered; return true;
			}, context)));
		EXPECT_EQ(delivered, 1U);
		EXPECT_EQ(context.parameters.at("values"), values);
	}
}

TEST(LogicalPlanUnionRuntimeTest, EmptySliceStillAdmitsItsNestedSourceCapacity) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryContext context;
	const PropertyValue nested(std::vector<PropertyValue>{PropertyValue(std::vector<PropertyValue>(128U))});
	context.parameters.emplace("nested", nested);
	const auto plan = parser.parseToLogical("RETURN size($nested[0..0]) AS result");
	auto limited = bounds();
	limited.maximum_rows = 256U;
	limited.maximum_materialized_bytes = 1024U;
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context)), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	EXPECT_EQ(context.parameters.at("nested"), nested);
	limited.maximum_materialized_bytes = 65536U;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
		[&](const LogicalPlanReadRow &row) {
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("result")), PropertyValue(int64_t{0}));
			++delivered; return true;
		}, context)));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanUnionRuntimeTest, ListComprehensionAdmitsOldAndNewSlotsDuringGrowth) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryContext context;
	context.parameters.emplace("values", PropertyValue(std::vector<PropertyValue>(128U, PropertyValue(int64_t{7}))));
	const auto plan = parser.parseToLogical("RETURN size([x IN $values | x]) AS result");
	auto limited = bounds();
	limited.maximum_rows = 256U;
	// Source and final result fit. Growing 64 slots to 128 must additionally
	// admit the old 64-slot allocation until relocation finishes.
	const size_t listBytes = sizeof(uint32_t) + 128U * (sizeof(PropertyValue) + sizeof(int64_t));
	limited.maximum_materialized_bytes = 2U * listBytes + 1024U;
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context)), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	limited.maximum_materialized_bytes = 65536U;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
		[&](const LogicalPlanReadRow &row) {
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("result")), PropertyValue(int64_t{128}));
			++delivered; return true;
		}, context)));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanUnionRuntimeTest, RowCacheAccountingChecksGuardDuringNestedValuesAndRetries) {
	NoGraphBackend backend;
	NoGraphSnapshot snapshot;
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalSingleRow root;
	for (const bool metadata : {false, true}) {
		SCOPED_TRACE(metadata);
		LogicalPlanReadRow input;
		auto &cache = metadata ? input.entity_metadata_cache : input.property_cache;
		cache["n"].emplace("nested", PropertyValue(std::vector<PropertyValue>(4096U, PropertyValue(int64_t{7}))));
		size_t samples = 0U;
		QueryGuard guard(0, 1, [&] { return ++samples >= 8U ? 2U * 1024U * 1024U : 0U; });
		QueryContext context;
		context.guard = &guard;
		auto permitted = bounds();
		permitted.maximum_materialized_bytes = 4U * 1024U * 1024U;
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
			snapshot, root, permitted, input, [&](const LogicalPlanReadRow &) {
				++delivered; return true;
			}, context)), QueryMemoryExceededException);
		EXPECT_EQ(delivered, 0U);
		EXPECT_EQ(samples, 8U);
		ASSERT_EQ(cache.at("n").at("nested").getList().size(), 4096U);
		context.guard = nullptr;
		EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
			snapshot, root, permitted, input, [&](const LogicalPlanReadRow &row) {
				const auto &output = metadata ? row.entity_metadata_cache : row.property_cache;
				EXPECT_EQ(output.at("n").at("nested"), cache.at("n").at("nested"));
				++delivered; return true;
			}, context)));
		EXPECT_EQ(delivered, 1U);
	}
}

TEST(LogicalPlanUnionRuntimeTest, SortChargesNestedParameterKeysEvenForSmallOutputRows) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryContext context;
	const PropertyValue key(std::vector<PropertyValue>(128U));
	context.parameters.emplace("key", key);
	const auto plan = parser.parseToLogical("UNWIND [2, 1] AS item RETURN item ORDER BY $key, item");
	auto limited = bounds();
	limited.maximum_rows = 256U;
	limited.maximum_materialized_bytes = 4096U;
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context)), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	EXPECT_EQ(context.parameters.at("key"), key);
	limited.maximum_materialized_bytes = 65536U;
	std::vector<int64_t> output;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
		[&](const LogicalPlanReadRow &row) {
			output.push_back(std::get<int64_t>(std::get<PropertyValue>(row.values.at("item")).getVariant()));
			return true;
		}, context)));
	EXPECT_EQ(output, (std::vector<int64_t>{1, 2}));
}

TEST(LogicalPlanUnionRuntimeTest, RetainedSortKeysShareBudgetWithLaterKeyExpressionTemporaries) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryContext context;
	context.parameters.emplace("key", PropertyValue(std::vector<PropertyValue>(32U)));
	context.parameters.emplace("other", PropertyValue(std::vector<PropertyValue>(64U, PropertyValue(int64_t{7}))));
	const auto plan = parser.parseToLogical("UNWIND [2, 1] AS item RETURN item ORDER BY $key, size($other), item");
	auto limited = bounds();
	limited.maximum_rows = 256U;
	// Final row/key storage fits; a retained first key and the later function's
	// argument container must also fit simultaneously during key evaluation.
	limited.maximum_materialized_bytes = 6000U;
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context)), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	limited.maximum_materialized_bytes = 65536U;
	std::vector<int64_t> output;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, limited,
		[&](const LogicalPlanReadRow &row) {
			output.push_back(std::get<int64_t>(std::get<PropertyValue>(row.values.at("item")).getVariant()));
			return true;
		}, context)));
	EXPECT_EQ(output, (std::vector<int64_t>{1, 2}));
}

TEST(LogicalPlanUnionRuntimeTest, SortCacheEnrichmentSharesBudgetWithLaterKeyTemporaries) {
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	QueryContext context;
	const PropertyValue map(PropertyValue::MapType{{"payload", PropertyValue(std::vector<PropertyValue>(16U))}});
	context.parameters.emplace("map", map);
	context.parameters.emplace("other", PropertyValue(std::vector<PropertyValue>(64U, PropertyValue(int64_t{7}))));
	const auto parsed = parser.parseToLogical("RETURN $map AS m ORDER BY size(keys(m)), size($other)");
	const logical::LogicalOperator *sortNode = parsed.root.get();
	while (sortNode->getType() != logical::LogicalOpType::LOP_SORT) {
		const auto children = sortNode->getChildren();
		ASSERT_EQ(children.size(), 1U);
		sortNode = children.front();
	}
	const auto &parsedSort = dynamic_cast<const logical::LogicalSort &>(*sortNode);
	logical::LogicalSort plan(std::make_unique<logical::LogicalUnwind>(
		std::make_unique<logical::LogicalSingleRow>(), "m", std::vector<PropertyValue>{map, map}),
		parsedSort.getSortItems());
	auto limited = bounds();
	limited.maximum_rows = 256U;
	const size_t rowBytes = sizeof(LogicalPlanReadRow) + 1U + sizeof(LogicalPlanReadValue) + graph::query::execution::detail::retainedPropertyValueBytes(map);
	// Original rows and key slots plus one large function argument fit. The
	// cache populated by keys(m) must also remain charged for the later key.
	limited.maximum_materialized_bytes = 2U * rowBytes + 4U * sizeof(PropertyValue) +
		64U * sizeof(PropertyValue) + sizeof(PropertyValue) + 128U +
		// Allow one cache copy; both enriched rows must coexist later.
		1U + std::string("payload").size() +
		graph::query::execution::detail::retainedPropertyValueBytes(map.getMap().at("payload"));
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.execute(plan, limited,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context)), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	limited.maximum_materialized_bytes = 65536U;
	size_t retried = 0U;
	EXPECT_NO_THROW(static_cast<void>(runtime.execute(plan, limited,
		[&](const LogicalPlanReadRow &row) {
			++retried;
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("m")), map);
			return true;
		}, context)));
	EXPECT_EQ(retried, 2U);
}

TEST(LogicalPlanUnionRuntimeTest, WideNodeLabelAccountingChecksGuardAndRetries) {
	NoGraphBackend backend;
	NoGraphSnapshot snapshot;
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalSingleRow root;
	storage::LogicalGraphNode node;
	node.node_id = 1U;
	for (size_t index = 0U; index < 4096U; ++index)
		node.labels.push_back("label" + std::to_string(index));
	LogicalPlanReadRow input;
	input.path_cache["p"].nodes.push_back(node);
	size_t samples = 0U;
	QueryGuard guard(0, 1, [&] { return ++samples >= 8U ? 2U * 1024U * 1024U : 0U; });
	QueryContext context;
	context.guard = &guard;
	auto permitted = bounds();
	permitted.maximum_materialized_bytes = 4U * 1024U * 1024U;
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
		snapshot, root, permitted, input, [&](const LogicalPlanReadRow &) {
			++delivered; return true;
		}, context)), QueryMemoryExceededException);
	EXPECT_EQ(delivered, 0U);
	EXPECT_EQ(samples, 8U);
	EXPECT_EQ(input.path_cache.at("p").nodes.at(0).labels, node.labels);
	context.guard = nullptr;
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
		snapshot, root, permitted, input, [&](const LogicalPlanReadRow &row) {
			EXPECT_EQ(row.path_cache.at("p").nodes.at(0).labels, node.labels);
			++delivered; return true;
		}, context)));
	EXPECT_EQ(delivered, 1U);
	// Imported entities are validated before refreshing their storage records.
	// Cancellation must occur in that validation loop before NoGraphSnapshot
	// can report an unexpected storage read.
	LogicalPlanReadRow imported;
	imported.values.emplace("n", node);
	samples = 0U;
	QueryGuard validationGuard(0, 1, [&] { return ++samples >= 8U ? 2U * 1024U * 1024U : 0U; });
	context.guard = &validationGuard;
	delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
		snapshot, root, permitted, imported, [&](const LogicalPlanReadRow &) {
			++delivered; return true;
		}, context)), QueryMemoryExceededException);
	EXPECT_EQ(delivered, 0U);
	EXPECT_EQ(samples, 8U);
	EXPECT_EQ(std::get<storage::LogicalGraphNode>(imported.values.at("n")).labels, node.labels);

}

TEST(LogicalPlanUnionRuntimeTest, RetainedNodeResultsChargeLabelSlotsAcrossRows) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	for (auto &[id, node] : graph.nodes) {
		node.labels.clear();
		for (size_t index = 0U; index < 128U; ++index)
			node.labels.push_back("L" + std::to_string(index));
	}
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	std::string query = "UNWIND [";
	for (size_t index = 0U; index < 64U; ++index) {
		if (index != 0U) query += ',';
		query += std::to_string(index);
	}
	query += "] AS i MATCH (n) RETURN n";
	const auto plan = parser.parseToLogical(query);
	auto limited = directBounds();
	limited.maximum_rows = 256U;
	limited.maximum_materialized_bytes = 256U * 1024U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited)), std::runtime_error);
	limited.maximum_materialized_bytes = 4U * 1024U * 1024U;
	const QueryResult result = runtime.executeToQueryResult(*plan.root, limited);
	ASSERT_EQ(result.rowCount(), 64U * graph.nodes.size());
	for (const auto &row : result.getRows())
		EXPECT_EQ(row.at("n").asLogicalNode().labels.size(), 128U);
}

TEST(LogicalPlanUnionRuntimeTest, RepeatedEntityResultPropertiesRespectBudgetAndRetry) {
	using namespace graph::query::execution::direct_edge_test;
	graph::parser::cypher::CypherParserImpl parser;
	const PropertyValue payload(std::vector<PropertyValue>(128U, PropertyValue(int64_t{7})));
	for (const bool edge : {false, true}) {
		SCOPED_TRACE(edge);
		DirectEdgeGraph graph = directGraph();
		for (auto &[id, node] : graph.nodes) node.properties = {{"payload", payload}};
		for (auto &record : graph.edges) record.properties = {{"payload", payload}};
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		const auto plan = parser.parseToLogical(edge ?
			"MATCH ()-[r]->() RETURN r AS a, r AS b" : "MATCH (n) RETURN n AS a, n AS b");
		auto limited = directBounds();
		limited.maximum_materialized_bytes = 8192U;
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited)), std::runtime_error);
		limited.maximum_materialized_bytes = 1024U * 1024U;
		const QueryResult result = runtime.executeToQueryResult(*plan.root, limited);
		ASSERT_EQ(result.rowCount(), edge ? graph.edges.size() : graph.nodes.size());
		for (const auto &row : result.getRows()) {
			for (const auto *alias : {"a", "b"}) {
				const auto &properties = edge ? row.at(alias).asLogicalEdge().properties :
					row.at(alias).asLogicalNode().properties;
				EXPECT_EQ(properties.at("payload"), payload);
			}
		}
	}
}

TEST(LogicalPlanUnionRuntimeTest, NamedPathCacheSeparatesNodeAndEdgeWithTheSameId) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	ASSERT_FALSE(graph.edges.empty());
	graph.edges.front().edge_id = 1U;
	for (auto &[id, node] : graph.nodes) node.properties = {{"kind", PropertyValue("node")}};
	for (auto &edge : graph.edges) edge.properties = {{"kind", PropertyValue("edge")}};
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	const auto plan = parser.parseToLogical("MATCH p=(a)-[r]->(b) RETURN p");
	const QueryResult result = runtime.executeToQueryResult(*plan.root, directBounds());
	ASSERT_EQ(result.rowCount(), graph.edges.size());
	for (const auto &row : result.getRows()) {
		const auto &path = row.at("p").asPrimitive().getList();
		ASSERT_EQ(path.size(), 3U);
		EXPECT_EQ(path[0].getMap().at("kind"), PropertyValue("node"));
		EXPECT_EQ(path[1].getMap().at("kind"), PropertyValue("edge"));
		EXPECT_EQ(path[2].getMap().at("kind"), PropertyValue("node"));
	}
}

TEST(LogicalPlanUnionRuntimeTest, ImportedNodeLabelValidationUsesSharedBudgetAndRetries) {
	class LabelSnapshot final : public NoGraphSnapshot {
	public:
		LogicalGraphNode node;
		mutable size_t reads = 0U;
		LogicalGraphNode readNode(uint64_t) const override { ++reads; return node; }
		LogicalGraphNode readNodeShaped(uint64_t id, const EntityReadShape &) const override { return readNode(id); }
	} snapshot;
	snapshot.node.node_id = 1U;
	for (size_t index = 0U; index < 4096U; ++index)
		snapshot.node.labels.push_back("L" + std::to_string(index));
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalSingleRow root;
	LogicalPlanReadRow input;
	input.values.emplace("n", snapshot.node);
	auto permitted = bounds();
	permitted.maximum_materialized_bytes = 1024U;
	QueryContext context;
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
		snapshot, root, permitted, input, [&](const LogicalPlanReadRow &) {
			++delivered; return true;
		}, context)), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	EXPECT_EQ(snapshot.reads, 0U);
	permitted.maximum_materialized_bytes = 4U * 1024U * 1024U;
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(
		snapshot, root, permitted, input, [&](const LogicalPlanReadRow &row) {
			EXPECT_EQ(std::get<LogicalGraphNode>(row.values.at("n")).labels, snapshot.node.labels);
			++delivered; return true;
		}, context)));
	EXPECT_EQ(delivered, 1U);
	EXPECT_GT(snapshot.reads, 0U);
	EXPECT_EQ(std::get<LogicalGraphNode>(input.values.at("n")).labels, snapshot.node.labels);
}

TEST(LogicalPlanUnionRuntimeTest, SelectedNestedPropertyColumnsRespectBudgetAndRetry) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	graph.enable_property_tokens = true;
	const PropertyValue payload(std::vector<PropertyValue>(128U, PropertyValue{}));
	for (auto &[id, node] : graph.nodes) node.properties = {{"payload", payload}};
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	std::string query = "MATCH (n {payload:[";
	for (size_t index = 0U; index < 128U; ++index) {
		if (index != 0U) query += ",";
		query += "NULL";
	}
	query += "]}) RETURN count(n) AS count";
	const auto plan = parser.parseToLogical(query);
	auto limited = directBounds();
	limited.maximum_materialized_bytes = 4096U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited)), std::runtime_error);
	limited.maximum_materialized_bytes = 1024U * 1024U;
	const QueryResult result = runtime.executeToQueryResult(*plan.root, limited);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("count").asPrimitive(),
		PropertyValue(static_cast<int64_t>(graph.nodes.size())));
}

TEST(LogicalPlanUnionRuntimeTest, SelectedPropertyColumnsChargeUnusedSlotsAndRetry) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	graph.enable_property_tokens = true;
	graph.node_property_column_extra_slots = 4096U;
	for (auto &[id, node] : graph.nodes) node.properties = {{"payload", PropertyValue(int64_t{7})}};
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	const auto plan = parser.parseToLogical("MATCH (n {payload:7}) RETURN count(n) AS count");
	auto limited = directBounds();
	limited.maximum_materialized_bytes = 64U * 1024U;
	EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited)), std::runtime_error);
	EXPECT_GT(graph.node_property_column_batch_reads, 0U);
	limited.maximum_materialized_bytes = 4U * 1024U * 1024U;
	const QueryResult result = runtime.executeToQueryResult(*plan.root, limited);
	ASSERT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows().front().at("count").asPrimitive(),
		PropertyValue(static_cast<int64_t>(graph.nodes.size())));
}

TEST(LogicalPlanUnionRuntimeTest, TraversalNestedSelectedColumnsRespectBudgetAndRetry) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	graph.enable_property_tokens = true;
	const PropertyValue payload(std::vector<PropertyValue>(128U, PropertyValue{}));
	for (auto &edge : graph.edges) edge.properties = {{"payload", payload}};
	for (auto &[id, node] : graph.nodes) node.properties = {{"payload", payload}};
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	std::string query = "MATCH (a)-[r {payload:[";
	for (size_t index = 0U; index < 128U; ++index) {
		if (index != 0U) query += ",";
		query += "NULL";
	}
	query += "]}]->(b) RETURN r.payload AS payload";
	std::string targetQuery = query;
	const std::string literal = query.substr(query.find('[' , query.find("payload:")),
		query.find("]}") - query.find('[', query.find("payload:")) + 1U);
	targetQuery.replace(targetQuery.find("(b)"), 3U, "(b {payload:" + literal + "})");
	for (const auto &selectedQuery : {query, targetQuery}) {
		SCOPED_TRACE(selectedQuery);
		const auto plan = parser.parseToLogical(selectedQuery);
		auto limited = directBounds();
		limited.maximum_materialized_bytes = 4096U;
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, limited)), std::runtime_error);
		limited.maximum_materialized_bytes = 4U * 1024U * 1024U;
		const QueryResult result = runtime.executeToQueryResult(*plan.root, limited);
		ASSERT_EQ(result.rowCount(), graph.edges.size());
		for (const auto &row : result.getRows()) EXPECT_EQ(row.at("payload").asPrimitive(), payload);
	}
}

TEST(LogicalPlanUnionRuntimeTest, NestedPropertyPrefetchPreservesIndependentResults) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	const PropertyValue payload(std::vector<PropertyValue>(4096U, PropertyValue(int64_t{7})));
	for (auto &[id, node] : graph.nodes) node.properties = {{"payload", payload}};
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	graph::parser::cypher::CypherParserImpl parser;
	const auto plan = parser.parseToLogical("MATCH (n) RETURN n.payload AS payload");
	auto permitted = directBounds();
	permitted.maximum_materialized_bytes = 16U * 1024U * 1024U;
	const QueryResult result = runtime.executeToQueryResult(*plan.root, permitted);
	ASSERT_EQ(result.rowCount(), graph.nodes.size());
	EXPECT_EQ(graph.node_property_column_batch_reads, 1U);
	for (const auto &row : result.getRows()) {
		const auto &actual = row.at("payload").asPrimitive();
		EXPECT_EQ(actual, payload);
		EXPECT_NE(actual.getList().data(), payload.getList().data());
	}
}

TEST(LogicalPlanUnionRuntimeTest, ImportedNestedCachesRejectBeforeDeliveryAndRetryUnchanged) {
	NoGraphSnapshot snapshot;
	NoGraphBackend backend;
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalSingleRow root;
	LogicalPlanReadRow input;
	input.property_cache["n"].emplace("payload", PropertyValue(std::vector<PropertyValue>(4096U, PropertyValue{})));
	input.values.emplace("list", input.property_cache.at("n").at("payload"));
	input.entity_metadata_cache["n"].emplace("labels", PropertyValue("Person"));
	input.complete_entity_property_cache.insert("n");
	input.path_cache["p"].node_refs.reserve(4096U);
	const auto original = input;
	const size_t originalCapacity = input.path_cache.at("p").node_refs.capacity();
	auto permitted = bounds();
	permitted.maximum_materialized_bytes = 1024U;
	QueryContext context;
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(snapshot, root, permitted, input,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context)), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	EXPECT_EQ(input, original);
	EXPECT_EQ(input.path_cache.at("p").node_refs.capacity(), originalCapacity);
	permitted.maximum_materialized_bytes = 4U * 1024U * 1024U;
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(snapshot, root, permitted, input,
		[&](const LogicalPlanReadRow &row) {
			EXPECT_EQ(row, input);
			EXPECT_NE(row.property_cache.at("n").at("payload").getList().data(),
				input.property_cache.at("n").at("payload").getList().data());
			EXPECT_NE(std::get<PropertyValue>(row.values.at("list")).getList().data(),
				std::get<PropertyValue>(input.values.at("list")).getList().data());
			++delivered; return true;
		}, context)));
	EXPECT_EQ(delivered, 1U);
	EXPECT_EQ(input, original);
}

TEST(LogicalPlanUnionRuntimeTest, ImportedRepeatedEntitiesKeepIndependentNestedCopiesOnRetry) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	const PropertyValue payload(std::vector<PropertyValue>(4096U, PropertyValue(int64_t{7})));
	graph.nodes.at(1U).properties = {{"payload", payload}};
	graph.edges.front().properties = {{"payload", payload}};
	DirectEdgeBackend backend(graph);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalSingleRow root;
	LogicalPlanReadRow input;
	input.values.emplace("n1", graph.nodes.at(1U));
	input.values.emplace("n2", graph.nodes.at(1U));
	input.values.emplace("e1", graph.edges.front());
	input.values.emplace("e2", graph.edges.front());
	const auto original = input;
	auto permitted = directBounds();
	permitted.maximum_materialized_bytes = 1024U;
	QueryContext context;
	size_t delivered = 0U;
	const auto receive = [&](const LogicalPlanReadRow &row) {
		++delivered;
		const auto &n1 = std::get<LogicalGraphNode>(row.values.at("n1"));
		const auto &n2 = std::get<LogicalGraphNode>(row.values.at("n2"));
		const auto &e1 = std::get<LogicalGraphEdge>(row.values.at("e1"));
		const auto &e2 = std::get<LogicalGraphEdge>(row.values.at("e2"));
		EXPECT_EQ(n1, n2);
		EXPECT_EQ(e1, e2);
		EXPECT_EQ(n1.properties.front().value, payload);
		EXPECT_EQ(e1.properties.front().value, payload);
		EXPECT_NE(n1.properties.front().value.getList().data(), n2.properties.front().value.getList().data());
		EXPECT_NE(e1.properties.front().value.getList().data(), e2.properties.front().value.getList().data());
		return true;
	};
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, permitted,
		input, receive, context)), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	permitted.maximum_materialized_bytes = 4U * 1024U * 1024U;
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, permitted,
		input, receive, context)));
	EXPECT_EQ(delivered, 1U);
	EXPECT_EQ(input, original);
}

TEST(LogicalPlanUnionRuntimeTest, ImportedRepeatedEntityCopiesStayBoundedBeforeCount) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	const PropertyValue payload(std::vector<PropertyValue>(4096U, PropertyValue(int64_t{7})));
	graph.nodes.at(1U).properties = {{"payload", payload}};
	DirectEdgeBackend backend(graph);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalAggregate root(std::make_unique<logical::LogicalSingleRow>(), {}, {{"count", nullptr, "total"}});
	LogicalPlanReadRow input;
	for (size_t index = 0U; index < 8U; ++index)
		input.values.emplace("n" + std::to_string(index), graph.nodes.at(1U));
	auto permitted = directBounds();
	// A source entity fits; the refreshed alias payloads must remain bounded even for count-only output.
	permitted.maximum_materialized_bytes = 2U * 4096U * sizeof(PropertyValue) + 64U * 1024U;
	size_t delivered = 0U;
	QueryContext context;
	const auto receive = [&](const LogicalPlanReadRow &row) {
		++delivered;
		EXPECT_EQ(std::get<PropertyValue>(row.values.at("total")), PropertyValue(int64_t{1}));
		return true;
	};
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, permitted,
		input, receive, context)), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	permitted.maximum_materialized_bytes = 16U * 1024U * 1024U;
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, permitted,
		input, receive, context)));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanUnionRuntimeTest, ImportedEntityRefreshSkipsInvalidatedCachesLargerThanBudget) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	const PropertyValue fresh(std::string(512U * 1024U, 'f'));
	graph.nodes.at(1U).properties = {{"payload", fresh}};
	DirectEdgeBackend backend(graph);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	LogicalPlanReadRuntime runtime(backend);
	logical::LogicalSingleRow root;
	LogicalPlanReadRow input;
	input.values.emplace("n", graph.nodes.at(1U));
	input.property_cache["n"].emplace("payload", PropertyValue(std::string(1024U * 1024U, 's')));
	input.complete_entity_property_cache.insert("n");
	const auto original = input;
	auto permitted = directBounds();
	permitted.maximum_materialized_bytes = 768U * 1024U;
	size_t delivered = 0U;
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, permitted, input,
		[&](const LogicalPlanReadRow &row) {
			++delivered;
			EXPECT_EQ(std::get<LogicalGraphNode>(row.values.at("n")).properties.front().value, fresh);
			EXPECT_FALSE(row.property_cache.contains("n"));
			EXPECT_FALSE(row.complete_entity_property_cache.contains("n"));
			return true;
		}, QueryContext{})));
	EXPECT_EQ(delivered, 1U);
	EXPECT_EQ(input, original);
}

TEST(LogicalPlanUnionRuntimeTest, ImportedCachePruningPreservesReferencesPrimitivesAndPaths) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	DirectEdgeBackend backend(graph);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	LogicalPlanReadRuntime runtime(backend);
	std::vector<logical::LogicalProjectItem> items;
	for (const auto *alias : {"n", "ref", "m"})
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>(alias), alias);
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("ref", "rank"), "cached_rank");
	logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
	LogicalPlanReadRow input;
	input.values.emplace("n", graph.nodes.at(1U));
	input.values.emplace("ref", EntityRef{2U, false, 0U, 0U, 0U});
	input.values.emplace("m", PropertyValue(PropertyValue::MapType{{"name", PropertyValue("kept")}}));
	input.property_cache["n"].emplace("stale", PropertyValue("discard"));
	input.entity_metadata_cache["n"].emplace("stale", PropertyValue("discard"));
	input.complete_entity_property_cache.insert("n");
	input.property_cache["ref"].emplace("rank", PropertyValue(int64_t{999}));
	input.entity_metadata_cache["ref"].emplace("labels", PropertyValue(std::vector<PropertyValue>{PropertyValue("Person")}));
	input.complete_entity_property_cache.insert("ref");
	input.property_cache["m"].emplace("name", PropertyValue("kept"));
	input.complete_entity_property_cache.insert("m");
	input.path_cache["n"].nodes.push_back(graph.nodes.at(1U));
	input.path_cache["n"].node_refs.push_back(EntityRef{2U, false, 0U, 0U, 0U});
	const auto original = input;
	size_t delivered = 0U;
	const auto receive = [&](const LogicalPlanReadRow &row) {
			++delivered;
			EXPECT_FALSE(row.property_cache.contains("n"));
			EXPECT_FALSE(row.entity_metadata_cache.contains("n"));
			EXPECT_FALSE(row.complete_entity_property_cache.contains("n"));
			if (row.values.contains("cached_rank"))
				EXPECT_EQ(std::get<PropertyValue>(row.values.at("cached_rank")), PropertyValue(int64_t{999}));
			EXPECT_FALSE(row.property_cache.contains("ref"));
			EXPECT_EQ(row.entity_metadata_cache.at("ref"), input.entity_metadata_cache.at("ref"));
			EXPECT_FALSE(row.complete_entity_property_cache.contains("ref"));
			if (!row.values.contains("cached_rank")) {
				EXPECT_EQ(row.property_cache.at("m"), input.property_cache.at("m"));
				EXPECT_TRUE(row.complete_entity_property_cache.contains("m"));
				EXPECT_EQ(row.path_cache.at("n"), input.path_cache.at("n"));
			}
			EXPECT_EQ(std::get<LogicalGraphNode>(row.values.at("ref")).node_id, 2U);
			return true;
		};
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, directBounds(), input,
		receive, QueryContext{})));
	logical::LogicalSingleRow passthrough;
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, passthrough, directBounds(), input,
		receive, QueryContext{})));
	EXPECT_EQ(delivered, 2U);
	EXPECT_EQ(input, original);
}

TEST(LogicalPlanUnionRuntimeTest, ImportedMultiAliasProjectionPreservesIndependentMetadataCaches) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	std::vector<PropertyValue> labels;
	graph.nodes.at(1U).labels.clear();
	for (size_t index = 0U; index < 128U; ++index) {
		const auto label = "L" + std::to_string(index);
		graph.nodes.at(1U).labels.push_back(label);
		labels.emplace_back(label);
	}
	const PropertyValue payload(std::vector<PropertyValue>(4096U, PropertyValue(int64_t{7})));
	graph.nodes.at(1U).properties = {{"payload", payload}};
	DirectEdgeBackend backend(graph);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRow input;
	input.values.emplace("n", EntityRef{1U, false, 0U, 0U, 0U});
	input.property_cache["n"].emplace("payload", payload);
	input.entity_metadata_cache["n"].emplace("labels", PropertyValue(labels));
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "a");
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n"), "b");
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "payload"), "payload");
	logical::LogicalProject root(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
	size_t delivered = 0U;
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, directBounds(), input,
		[&](const LogicalPlanReadRow &row) {
			++delivered;
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("payload")), payload);
			const auto &a = row.entity_metadata_cache.at("a").at("labels");
			const auto &b = row.entity_metadata_cache.at("b").at("labels");
			EXPECT_EQ(a, PropertyValue(labels));
			EXPECT_EQ(b, a);
			EXPECT_NE(a.getList().data(), b.getList().data());
			EXPECT_EQ(std::get<LogicalGraphNode>(row.values.at("a")).labels, graph.nodes.at(1U).labels);
			EXPECT_EQ(std::get<LogicalGraphNode>(row.values.at("b")).labels, graph.nodes.at(1U).labels);
			return true;
		}, QueryContext{})));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanUnionRuntimeTest, SubqueryImportedNestedValuesPreserveIndependentResultsAndRetry) {
	NoGraphBackend backend;
	NoGraphSnapshot snapshot;
	LogicalPlanReadRuntime runtime(backend);
	std::vector<PropertyValue> entries(4096U, PropertyValue(int64_t{7}));
	entries.reserve(8192U);
	LogicalPlanReadRow input;
	input.values.emplace("outer", PropertyValue(std::move(entries)));
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("outer"), "copied");
	auto body = std::make_unique<logical::LogicalProject>(
		std::make_unique<logical::LogicalSingleRow>(), std::move(items));
	logical::LogicalCallSubquery root(std::make_unique<logical::LogicalSingleRow>(), std::move(body),
		{"outer"}, {"copied"});
	auto permitted = bounds();
	permitted.maximum_materialized_bytes = 1024U;
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(snapshot, root, permitted, input,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; }, QueryContext{})), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	permitted.maximum_materialized_bytes = 16U * 1024U * 1024U;
	size_t samples = 0U;
	QueryGuard guard(0, 1, [&] { return ++samples >= 32U ? 2U * 1024U * 1024U : 0U; });
	QueryContext cancelled;
	cancelled.guard = &guard;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(snapshot, root, permitted, input,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; }, cancelled)), QueryMemoryExceededException);
	EXPECT_EQ(delivered, 0U);
	EXPECT_EQ(samples, 32U);
	EXPECT_EQ(std::get<PropertyValue>(input.values.at("outer")).getList().front(), PropertyValue(int64_t{7}));
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(snapshot, root, permitted, input,
		[&](const LogicalPlanReadRow &row) {
			++delivered;
			const auto &original = std::get<PropertyValue>(input.values.at("outer"));
			const auto &copied = std::get<PropertyValue>(row.values.at("copied"));
			EXPECT_EQ(copied, original);
			EXPECT_NE(copied.getList().data(), original.getList().data());
			return true;
		}, QueryContext{})));
	EXPECT_EQ(delivered, 1U);
	EXPECT_EQ(std::get<PropertyValue>(input.values.at("outer")).getList().size(), 4096U);
	EXPECT_GE(std::get<PropertyValue>(input.values.at("outer")).getList().capacity(), 8192U);
}

TEST(LogicalPlanUnionRuntimeTest, ImportedSingleHopTraversalPreservesIndependentNestedEntities) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	graph.enable_adjacency_reference_pages = false;
	const PropertyValue payload(std::vector<PropertyValue>(4096U, PropertyValue(int64_t{7})));
	for (auto &[id, node] : graph.nodes) node.properties = {{"payload", payload}};
	for (auto &edge : graph.edges) edge.properties = {{"payload", payload}};
	DirectEdgeBackend backend(graph);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRow input;
	input.values.emplace("source", graph.nodes.at(1U));
	logical::LogicalTraversal root(std::make_unique<logical::LogicalSingleRow>(), "source", "r", "target", "", "out");
	auto permitted = directBounds();
	permitted.maximum_materialized_bytes = 16U * 1024U * 1024U;
	size_t delivered = 0U;
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, permitted, input,
		[&](const LogicalPlanReadRow &row) {
			++delivered;
			const auto &edge = std::get<LogicalGraphEdge>(row.values.at("r"));
			const auto &node = std::get<LogicalGraphNode>(row.values.at("target"));
			EXPECT_FALSE(edge.properties.empty());
			EXPECT_EQ(edge.properties.front().value, payload);
			EXPECT_EQ(node.properties.front().value, payload);
			EXPECT_NE(edge.properties.front().value.getList().data(), payload.getList().data());
			EXPECT_NE(node.properties.front().value.getList().data(), payload.getList().data());
			return true;
		}, QueryContext{})));
	const size_t expected = std::count_if(graph.edges.begin(), graph.edges.end(),
		[](const auto &edge) { return edge.source_node_id == 1U; });
	EXPECT_EQ(delivered, expected);
	EXPECT_GT(delivered, 0U);
	logical::LogicalSort sorted(root.clone(), {
		{std::make_shared<expressions::VariableReferenceExpression>("target"), true}});
	size_t sortedDelivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, sorted, permitted, input,
		[&](const LogicalPlanReadRow &row) {
			++sortedDelivered;
			EXPECT_EQ(std::get<LogicalGraphEdge>(row.values.at("r")).properties.front().value, payload);
			EXPECT_EQ(std::get<LogicalGraphNode>(row.values.at("target")).properties.front().value, payload);
			return false;
		}, QueryContext{})), std::runtime_error);
	EXPECT_EQ(sortedDelivered, 1U);
	EXPECT_EQ(std::get<LogicalGraphNode>(input.values.at("source")).properties.front().value, payload);
}

TEST(LogicalPlanUnionRuntimeTest, AnonymousTraversalDoesNotCopyUnboundEdgePayload) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	graph.enable_native_node_ref_scans = false;
	graph.enable_adjacency_reference_pages = false;
	graph.edges.resize(1U);
	graph.edges.front().properties.emplace_back("payload", PropertyValue(std::string(65536U, 'e')));
	DirectEdgeBackend backend(graph);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRow input;
	input.values.emplace("source", EntityRef{1U, false, 0U, 0U, 0U});
	logical::LogicalTraversal root(std::make_unique<logical::LogicalSingleRow>(), "source", "", "target", "KNOWS", "out",
		{}, {}, OwnedPropertyMap(PropertyValue::MapType{{"weight", PropertyValue(7)}}));
	auto permitted = directBounds();
	permitted.maximum_materialized_bytes = 8192U;
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, permitted, input,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; }, QueryContext{})), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	permitted.maximum_materialized_bytes = 256U * 1024U;
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, permitted, input,
		[&](const LogicalPlanReadRow &row) {
			++delivered;
			EXPECT_FALSE(row.values.contains(""));
			EXPECT_EQ(std::get<LogicalGraphNode>(row.values.at("target")).node_id, graph.edges.front().target_node_id);
			return true;
		}, QueryContext{})));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanUnionRuntimeTest, GenericTraversalRetainsTargetsTooLargeForEntityCache) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	graph.enable_shaped_property_admission = true;
	graph.enable_native_node_ref_scans = false;
	graph.edges.resize(1U);
	const auto targetId = graph.edges.front().target_node_id;
	graph.nodes.at(targetId).properties = {{"payload", PropertyValue(std::string(5U * 1024U * 1024U, 't'))}};
	DirectEdgeBackend backend(graph);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRow input;
	input.values.emplace("source", EntityRef{1U, false, 0U, 0U, 0U});
	logical::LogicalTraversal root(std::make_unique<logical::LogicalSingleRow>(), "source", "", "target", "KNOWS", "out");
	auto permitted = directBounds();
	permitted.maximum_materialized_bytes = 32U * 1024U * 1024U;
	size_t delivered = 0U;
	permitted.maximum_materialized_bytes = 2U * 1024U * 1024U;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, permitted, input,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; }, QueryContext{})), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	EXPECT_GT(graph.node_shaped_admission_reads, 0U);
	EXPECT_EQ(std::ranges::find(graph.node_read_ids, targetId), graph.node_read_ids.end());
	permitted.maximum_materialized_bytes = 8U * 1024U * 1024U;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, permitted, input,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; }, QueryContext{})), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	permitted.maximum_materialized_bytes = 32U * 1024U * 1024U;
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, permitted, input,
		[&](const LogicalPlanReadRow &row) {
			++delivered;
			const auto &target = std::get<LogicalGraphNode>(row.values.at("target"));
			EXPECT_EQ(target.node_id, targetId);
			EXPECT_EQ(target.properties.front().value, graph.nodes.at(targetId).properties.front().value);
			return true;
		}, QueryContext{})));
	EXPECT_EQ(delivered, 1U);
}

TEST(LogicalPlanUnionRuntimeTest, CompleteAdjacencyAdmissionCombinesPayloadAndBufferBudgets) {
	using namespace graph::query::execution::direct_edge_test;
	DirectEdgeGraph graph = directGraph();
	graph.enable_native_node_ref_scans = false;
	graph.enable_adjacency_reference_pages = false;
	graph.enable_adjacency_read_admission = true;
	graph.edges.resize(1U);
	DirectEdgeBackend backend(graph);
	auto snapshot = backend.openLogicalGraphReadSnapshot();
	LogicalPlanReadRuntime runtime(backend);
	LogicalPlanReadRow input;
	input.values.emplace("source", EntityRef{1U, false, 0U, 0U, 0U});
	logical::LogicalTraversal root(std::make_unique<logical::LogicalSingleRow>(), "source", "r", "target", "", "out",
		{}, {}, OwnedPropertyMap(PropertyValue::MapType{{"weight", PropertyValue(7)}}));
	auto bounds = directBounds();
	bounds.maximum_materialized_bytes = 6000U;
	size_t delivered = 0U;
	EXPECT_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, bounds, input,
		[&](const LogicalPlanReadRow &) { ++delivered; return true; }, QueryContext{})), std::runtime_error);
	EXPECT_EQ(delivered, 0U);
	EXPECT_EQ(graph.adjacency_scans, 0U);
	bounds.maximum_materialized_bytes = 65536U;
	EXPECT_NO_THROW(static_cast<void>(runtime.executeWithSnapshotAndInputRow(*snapshot, root, bounds, input,
		[&](const LogicalPlanReadRow &row) { ++delivered; EXPECT_EQ(std::get<LogicalGraphEdge>(row.values.at("r")).edge_id, graph.edges.front().edge_id); return true; }, QueryContext{})));
	EXPECT_EQ(delivered, 1U);
}
