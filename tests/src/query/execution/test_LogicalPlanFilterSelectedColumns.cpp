#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"
#include "graph/query/optimization/LogicalPlanOptimizer.hpp"
#include <tuple>

namespace graph::query::execution {
using namespace direct_edge_test;

TEST(LogicalPlanDirectEdgeRuntimeTest, FilterSelectedColumnsPreserveEscapedCachesAndBagOrder) {
	for (const bool earlyStop : {false, true}) {
		DirectEdgeGraph graph = directGraph();
		graph.edges.emplace_back(13, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
		graph.edges.emplace_back(14, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("b", "rank"),
			expressions::BinaryOperatorType::BOP_GREATER,
			std::make_unique<expressions::LiteralExpression>(int64_t{1}));
		auto filter = std::make_unique<logical::LogicalFilter>(std::move(traversal), predicate);
		std::vector<logical::LogicalSortItem> order;
		order.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b", "rank"), true);
		auto sort = std::make_unique<logical::LogicalSort>(std::move(filter), std::move(order));
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b", "rank"), "rank");
		logical::LogicalProject project(std::move(sort), std::move(items));
		const bool previous = debug::PerfTrace::areValuesEnabled();
		debug::PerfTrace::reset();
		debug::PerfTrace::setValueCollectionEnabled(true);
		std::vector<LogicalPlanReadRow> escaped;
		try {
			static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
				escaped.push_back(row);
				return !earlyStop;
			}));
		} catch (...) { debug::PerfTrace::setValueCollectionEnabled(previous); throw; }
		const auto trace = debug::PerfTrace::snapshotAndReset();
		debug::PerfTrace::setValueCollectionEnabled(previous);
		graph.nodes.clear();
		graph.edges.clear();
		std::vector<PropertyValue> ranks;
		for (const auto &row : escaped) ranks.push_back(std::get<PropertyValue>(row.values.at("rank")));
		EXPECT_EQ(ranks, (earlyStop ? std::vector<PropertyValue>{PropertyValue(2)}
			: std::vector<PropertyValue>{PropertyValue(2), PropertyValue(2), PropertyValue(2), PropertyValue(3)}));
		ASSERT_TRUE(trace.contains("logical_plan.prefetch.borrowed_values"));
		EXPECT_EQ(trace.at("logical_plan.prefetch.borrowed_values").totalValue, 5);
		ASSERT_TRUE(trace.contains("logical_plan.prefetch.moved_values"));
		EXPECT_EQ(trace.at("logical_plan.prefetch.moved_values").totalValue, 2);
	}
}

TEST(LogicalPlanDirectEdgeRuntimeTest, FilterSelectedColumnsKeepNullAndMissingPredicateSemantics) {
	for (const bool missing : {false, true}) {
		DirectEdgeGraph graph = directGraph();
		graph.nodes.at(2).properties.clear();
		if (!missing) graph.nodes.at(2).properties.emplace_back("rank", PropertyValue{});
		graph.edges.emplace_back(13, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
		graph.edges.emplace_back(14, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(
			std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
		auto predicate = std::make_shared<expressions::BinaryOpExpression>(
			std::make_unique<expressions::VariableReferenceExpression>("b", "rank"),
			expressions::BinaryOperatorType::BOP_GREATER,
			std::make_unique<expressions::LiteralExpression>(int64_t{1}));
		auto filter = std::make_unique<logical::LogicalFilter>(std::move(traversal), predicate);
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b", "rank"), "rank");
		logical::LogicalProject project(std::move(filter), std::move(items));
		std::vector<PropertyValue> ranks;
		static_cast<void>(runtime.execute(project, directBounds(), [&](const LogicalPlanReadRow &row) {
			ranks.push_back(std::get<PropertyValue>(row.values.at("rank")));
			return true;
		}));
		EXPECT_EQ(ranks, std::vector<PropertyValue>{PropertyValue(3)});
	}
}

TEST(LogicalPlanDirectEdgeRuntimeTest, FilterSelectedColumnsReleaseWideRejectedBatchesAfterFailure) {
	DirectEdgeGraph graph = directGraph();
	for (auto &[id, node] : graph.nodes) node.properties.emplace_back("payload", PropertyValue(std::string(4096, 'x')));
	for (uint64_t id = 20; id < 84; ++id)
		graph.edges.emplace_back(id, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{});
	DirectEdgeBackend backend(graph);
	LogicalPlanReadRuntime runtime(backend);
	auto traversal = std::make_unique<logical::LogicalTraversal>(
		std::make_unique<logical::LogicalNodeScan>("a"), "a", "r", "b", "KNOWS", "out");
	auto predicate = std::make_shared<expressions::BinaryOpExpression>(
		std::make_unique<expressions::VariableReferenceExpression>("b", "payload"),
		expressions::BinaryOperatorType::BOP_EQUAL,
		std::make_unique<expressions::LiteralExpression>(std::string("unmatched")));
	auto filter = std::make_unique<logical::LogicalFilter>(std::move(traversal), predicate);
	std::vector<logical::LogicalProjectItem> items;
	items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b", "payload"), "payload");
	logical::LogicalProject project(std::move(filter), std::move(items));
	auto bounds = directBounds();
	bounds.maximum_materialized_bytes = 16 * 1024;
	bounds.maximum_rows = 128;
	bounds.maximum_edges_per_node = 128;
	bounds.maximum_edge_records = 128;
	// Frozen prior runtime rejects both 16KiB and 32KiB for this wide fixture:
	// entity caches and columns share the allowance. Preserve those rejections
	// before checking multi-batch cleanup with a bounded 128KiB working set.
	EXPECT_THROW(static_cast<void>(runtime.execute(project, bounds, [](const auto &) { return true; })), std::runtime_error);
	graph.enable_adjacency_reference_pages = true;
	bounds.maximum_materialized_bytes = 32 * 1024;
	EXPECT_THROW(static_cast<void>(runtime.execute(project, bounds, [](const auto &) { return true; })), std::runtime_error);
	bounds.maximum_materialized_bytes = 128 * 1024;
	graph.truncate_node_property_batch = true;
	EXPECT_THROW(static_cast<void>(runtime.execute(project, bounds, [](const auto &) { return true; })), std::runtime_error);
	graph.truncate_node_property_batch = false;
	size_t emitted = 0;
	static_cast<void>(runtime.execute(project, bounds, [&](const auto &) { ++emitted; return true; }));
	EXPECT_EQ(emitted, 0U);
	EXPECT_GT(graph.node_property_column_batch_reads, 1U);
}
TEST(LogicalPlanDirectEdgeRuntimeTest, JoinFilterPushdownPreservesScalarBagNullsAndEarlyStop) {
 for (const bool floating : {false, true}) {
 DirectEdgeGraph graph = directGraph();
 graph.nodes.emplace(4U, LogicalGraphNode{4U, {}, {{"rank", PropertyValue(2)}}});
 graph.nodes.emplace(5U, LogicalGraphNode{5U, {}, {{"rank", PropertyValue(2)}}});
 graph.nodes.emplace(6U, LogicalGraphNode{6U, {}, {}});
 graph.nodes.emplace(7U, LogicalGraphNode{7U, {}, {{"rank", PropertyValue{}}}});
 if (floating) for (uint64_t id : {2U, 4U, 5U})
  graph.nodes.at(id).properties = {{"rank", PropertyValue(2.0)}};
 // Equality is type-sensitive; both fixture and literal use the same type.
 DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
 auto predicate = std::make_shared<expressions::BinaryOpExpression>(
  std::make_unique<expressions::VariableReferenceExpression>("b", "rank"), expressions::BinaryOperatorType::BOP_EQUAL,
  floating ? std::make_unique<expressions::LiteralExpression>(2.0) : std::make_unique<expressions::LiteralExpression>(int64_t{2}));
 auto filter = std::make_unique<logical::LogicalFilter>(std::make_unique<logical::LogicalJoin>(
  std::make_unique<logical::LogicalNodeScan>("a"), std::make_unique<logical::LogicalNodeScan>("b")), predicate);
 std::vector<logical::LogicalProjectItem> items;
 items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("a", "rank"), "a");
 items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b", "rank"), "b");
 auto original = std::make_unique<logical::LogicalProject>(std::move(filter), std::move(items));
 auto optimized = optimization::LogicalPlanOptimizer{}.optimize(original->clone()).root;
 auto bounds = directBounds(); bounds.maximum_rows = 64U;
 const auto run = [&](const logical::LogicalOperator &plan, bool stop) {
  std::map<std::pair<int64_t, int64_t>, size_t> bag;
  static_cast<void>(runtime.execute(plan, bounds, [&](const auto &row) {
   const auto key = [&](const char *name) {
    const auto &value = std::get<PropertyValue>(row.values.at(name));
    if (value.getType() == PropertyType::NULL_TYPE) return int64_t{-1};
    if (std::holds_alternative<double>(value.getVariant())) {
     EXPECT_TRUE(floating); EXPECT_DOUBLE_EQ(std::get<double>(value.getVariant()), 2.0); return int64_t{2};
    }
    const auto integer = std::get<int64_t>(value.getVariant());
    EXPECT_FALSE(floating && integer == 2); return integer;
   };
   ++bag[{key("a"), key("b")}]; return !stop;
  }));
  return bag;
 };
 const std::map<std::pair<int64_t, int64_t>, size_t> expected{{{1,2},3},{{2,2},9},{{3,2},3},{{-1,2},6}};
 EXPECT_EQ(run(*original, false), expected); EXPECT_EQ(run(*optimized, false), expected);
 EXPECT_EQ(run(*optimized, true), run(*original, true));
 }
}

}

namespace graph::query::execution {
using namespace direct_edge_test;

TEST(LogicalPlanDirectEdgeRuntimeTest, ReversedSingleHopAnchorPreservesDirectionalBagsAndEarlyStop) {
	class Statistics final : public optimization::LogicalStatisticsProvider {
	public:
		std::optional<optimization::LogicalCardinalityEstimate> estimate(const logical::LogicalOperator &) const noexcept override { return std::nullopt; }
		std::optional<uint64_t> estimateAnchorScan(const logical::LogicalOperator &root) const noexcept override {
			const auto *scan = dynamic_cast<const logical::LogicalNodeScan *>(&root);
			return scan ? std::optional<uint64_t>(scan->getVariable() == "a" ? 3U : 1U) : std::nullopt;
		}
	} statistics;
	for (const bool topology : {false, true}) for (const std::string direction : {"out", "in", "both"}) {
		DirectEdgeGraph graph = directGraph(); graph.nodes.at(2).labels.push_back("Rare");
		for (auto &[id, node] : graph.nodes) { static_cast<void>(id); node.labels.push_back("Universe"); }
		graph.enable_native_topology_pages = topology;
		graph.enable_native_node_ref_scans = topology;
		graph.edges.emplace_back(13, 1, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{{"weight", PropertyValue(7)}});
		graph.edges.emplace_back(14, 2, 2, "KNOWS", std::vector<storage::LogicalGraphPropertyEntry>{{"weight", PropertyValue(7)}});
		DirectEdgeBackend backend(graph); LogicalPlanReadRuntime runtime(backend);
		auto traversal = std::make_unique<logical::LogicalTraversal>(std::make_unique<logical::LogicalNodeScan>("a", std::vector<std::string>{"Universe"}),
			"a", "e", "b", "KNOWS", direction, std::vector<std::string>{"Rare"},
			std::vector<std::pair<std::string, PropertyValue>>{}, std::unordered_map<std::string, PropertyValue>{{"weight", PropertyValue(7)}});
		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("a", "rank"), "source");
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("b", "rank"), "target");
		items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("e", "weight"), "weight");
		logical::LogicalProject original(std::move(traversal), std::move(items));
		optimization::LogicalOptimizerOptions options; options.allow_read_traversal_anchor = true;
		auto result = optimization::LogicalPlanOptimizer(options).optimize(original.clone(), &statistics);
		ASSERT_EQ(result.rewrite_count, 1U);
		using Bag = std::map<std::tuple<int64_t, int64_t, int64_t>, size_t>;
		const auto execute = [&](const logical::LogicalOperator &plan, bool earlyStop) {
			Bag bag; static_cast<void>(runtime.execute(plan, directBounds(), [&](const LogicalPlanReadRow &row) {
				++bag[{std::get<int64_t>(std::get<PropertyValue>(row.values.at("source")).getVariant()),
					std::get<int64_t>(std::get<PropertyValue>(row.values.at("target")).getVariant()),
					std::get<int64_t>(std::get<PropertyValue>(row.values.at("weight")).getVariant())}]; return !earlyStop;
			})); return bag;
		};
		const auto expected = execute(original, false); EXPECT_EQ(execute(*result.root, false), expected);
		EXPECT_EQ(expected.at({2, 2, 7}), 1U);
		if (direction != "in") EXPECT_EQ(expected.at({1, 2, 7}), 2U);
		EXPECT_EQ(execute(*result.root, true).size(), 1U);
		EXPECT_EQ(result.root->getOutputVariables(), original.getOutputVariables());
		 auto limited = directBounds(); limited.maximum_materialized_bytes = 1U;
		 const auto visitor = [](const LogicalPlanReadRow &) { return true; };
		 EXPECT_THROW(static_cast<void>(runtime.execute(original, limited, visitor)), std::runtime_error);
		 EXPECT_THROW(static_cast<void>(runtime.execute(*result.root, limited, visitor)), std::runtime_error);

	}
}

} // namespace graph::query::execution
