/**
 * @file test_PhysicalPlanConverter_TraversalFilters.cpp
 * @brief Branch coverage tests for PhysicalPlanConverter: traversal filters,
 *        var-length traversal filters, multi-label scans, range predicates,
 *        composite scans, foreach/subquery, and load CSV paths.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "graph/query/planner/PhysicalPlanConverter.hpp"
#include "graph/query/execution/operators/FilterOperator.hpp"
#include "graph/query/execution/operators/TraversalOperator.hpp"
#include "graph/query/execution/operators/VarLengthTraversalOperator.hpp"
#include "graph/query/execution/operators/NodeScanOperator.hpp"
#include "graph/query/execution/operators/ForeachOperator.hpp"
#include "graph/query/execution/operators/CallSubqueryOperator.hpp"
#include "graph/query/execution/operators/LoadCsvOperator.hpp"
#include "graph/query/execution/operators/NamedPathOperator.hpp"
#include "graph/query/execution/operators/UnwindOperator.hpp"
#include "graph/query/execution/operators/UnionOperator.hpp"
#include "graph/query/execution/operators/SingleRowOperator.hpp"
#include "graph/query/execution/operators/MergeNodeOperator.hpp"
#include "graph/query/execution/operators/MergeEdgeOperator.hpp"
#include "graph/query/execution/operators/CreateIndexOperator.hpp"
#include "graph/query/execution/operators/DropIndexOperator.hpp"
#include "graph/query/execution/operators/CreateVectorIndexOperator.hpp"
#include "graph/query/execution/operators/ExplainOperator.hpp"
#include "graph/query/execution/operators/ProfileOperator.hpp"
#include "graph/query/expressions/Expression.hpp"

#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalVarLengthTraversal.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalForeach.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalLoadCsv.hpp"
#include "graph/query/logical/operators/LogicalNamedPath.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"
#include "graph/query/logical/operators/LogicalUnion.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"
#include "graph/query/logical/operators/LogicalMergeEdge.hpp"
#include "graph/query/logical/operators/LogicalCreateIndex.hpp"
#include "graph/query/logical/operators/LogicalDropIndex.hpp"
#include "graph/query/logical/operators/LogicalCreateVectorIndex.hpp"
#include "graph/query/logical/operators/LogicalExplain.hpp"
#include "graph/query/logical/operators/LogicalProfile.hpp"
#include "graph/query/logical/operators/LogicalCreateEdge.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalOptionalMatch.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"

#include "graph/storage/FileStorage.hpp"
#include "graph/storage/constraints/ConstraintManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;
using namespace graph::query;
using namespace graph::query::logical;
using namespace graph::query::expressions;
using namespace graph::query::execution::operators;

class PhysicalPlanConverterTraversalTest : public ::testing::Test {
protected:
	fs::path testFilePath;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	std::shared_ptr<graph::storage::constraints::ConstraintManager> constraintManager;
	std::unique_ptr<PhysicalPlanConverter> converter;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_ppc_tf_" + to_string(uuid) + ".zyx");

		storage = std::make_shared<graph::storage::FileStorage>(
			testFilePath.string(), 4096, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();
		dataManager = storage->getDataManager();

		indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
		indexManager->initialize();

		constraintManager = std::make_shared<graph::storage::constraints::ConstraintManager>(
			storage, indexManager);
		constraintManager->initialize();

		converter = std::make_unique<PhysicalPlanConverter>(
			dataManager, indexManager, constraintManager);
	}

	void TearDown() override {
		if (storage) storage->close();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}
};

// ============================================================================
// Traversal with target labels, target properties, edge properties
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, TraversalWithTargetLabels) {
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto trav = std::make_unique<LogicalTraversal>(
		std::move(scan), "n", "r", "m", "KNOWS", "out",
		std::vector<std::string>{"Engineer"},    // target labels
		std::vector<std::pair<std::string, graph::PropertyValue>>{},
		std::unordered_map<std::string, graph::PropertyValue>{});

	auto phys = converter->convert(trav.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, TraversalWithTargetProperties) {
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<std::pair<std::string, graph::PropertyValue>> targetProps;
	targetProps.emplace_back("status", graph::PropertyValue("active"));

	auto trav = std::make_unique<LogicalTraversal>(
		std::move(scan), "n", "r", "m", "KNOWS", "out",
		std::vector<std::string>{},
		targetProps,
		std::unordered_map<std::string, graph::PropertyValue>{});

	auto phys = converter->convert(trav.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, TraversalWithEdgeProperties) {
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::unordered_map<std::string, graph::PropertyValue> edgeProps;
	edgeProps["since"] = graph::PropertyValue(static_cast<int64_t>(2020));

	auto trav = std::make_unique<LogicalTraversal>(
		std::move(scan), "n", "r", "m", "KNOWS", "out",
		std::vector<std::string>{},
		std::vector<std::pair<std::string, graph::PropertyValue>>{},
		edgeProps);

	auto phys = converter->convert(trav.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, TraversalWithAllFilters) {
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<std::pair<std::string, graph::PropertyValue>> targetProps;
	targetProps.emplace_back("active", graph::PropertyValue(true));

	std::unordered_map<std::string, graph::PropertyValue> edgeProps;
	edgeProps["weight"] = graph::PropertyValue(1.5);

	auto trav = std::make_unique<LogicalTraversal>(
		std::move(scan), "n", "r", "m", "KNOWS", "out",
		std::vector<std::string>{"Engineer", "Manager"},
		targetProps, edgeProps);

	auto phys = converter->convert(trav.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// VarLength traversal with target labels and target properties
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, VarLengthTraversalWithTargetLabels) {
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto vlt = std::make_unique<LogicalVarLengthTraversal>(
		std::move(scan), "n", "r", "m", "KNOWS", "out", 1, 3,
		std::vector<std::string>{"City"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{});

	auto phys = converter->convert(vlt.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, VarLengthTraversalWithTargetProperties) {
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<std::pair<std::string, graph::PropertyValue>> targetProps;
	targetProps.emplace_back("name", graph::PropertyValue("Alice"));

	auto vlt = std::make_unique<LogicalVarLengthTraversal>(
		std::move(scan), "n", "r", "m", "KNOWS", "out", 1, 5,
		std::vector<std::string>{},
		targetProps);

	auto phys = converter->convert(vlt.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, VarLengthTraversalWithBothFilters) {
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	std::vector<std::pair<std::string, graph::PropertyValue>> targetProps;
	targetProps.emplace_back("age", graph::PropertyValue(static_cast<int64_t>(30)));

	auto vlt = std::make_unique<LogicalVarLengthTraversal>(
		std::move(scan), "n", "r", "m", "KNOWS", "out", 2, 4,
		std::vector<std::string>{"Developer"},
		targetProps);

	auto phys = converter->convert(vlt.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// Multi-label node scan
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, NodeScanWithMultipleLabels) {
	std::vector<std::string> labels{"Person", "Employee"};
	auto scan = std::make_unique<LogicalNodeScan>("n", labels);
	auto phys = converter->convert(scan.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// Unwind with literal list vs expression list
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, UnwindWithLiteralList) {
	auto singleRow = std::make_unique<LogicalSingleRow>();
	std::vector<graph::PropertyValue> values;
	values.emplace_back(static_cast<int64_t>(1));
	values.emplace_back(static_cast<int64_t>(2));
	auto unwind = std::make_unique<LogicalUnwind>(
		std::move(singleRow), "x", values);
	auto phys = converter->convert(unwind.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, UnwindWithExpression) {
	auto singleRow = std::make_unique<LogicalSingleRow>();
	auto listExpr = std::make_shared<VariableReferenceExpression>("myList");
	auto unwind = std::make_unique<LogicalUnwind>(
		std::move(singleRow), "x", listExpr);
	auto phys = converter->convert(unwind.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// Union
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, UnionAll) {
	auto left = std::make_unique<LogicalSingleRow>();
	auto right = std::make_unique<LogicalSingleRow>();
	auto un = std::make_unique<LogicalUnion>(std::move(left), std::move(right), true);
	auto phys = converter->convert(un.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// Foreach — covers both input and body branches
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, ForeachWithInputAndBody) {
	auto input = std::make_unique<LogicalSingleRow>();
	auto listExpr = std::make_shared<LiteralExpression>(static_cast<int64_t>(1));

	// Body: a simple create node chain with SingleRow at the leaf
	auto bodySingleRow = std::make_unique<LogicalSingleRow>();
	auto body = std::make_unique<LogicalCreateNode>(
		"x", std::vector<std::string>{"Temp"},
		std::unordered_map<std::string, graph::PropertyValue>{},
		std::unordered_map<std::string, std::shared_ptr<Expression>>{});
	body->setChild(0, std::move(bodySingleRow));

	auto foreach = std::make_unique<LogicalForeach>(
		std::move(input), "i", listExpr, std::move(body));
	auto phys = converter->convert(foreach.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, ForeachWithNullInputAndNullBody) {
	auto listExpr = std::make_shared<LiteralExpression>(static_cast<int64_t>(0));
	auto foreach = std::make_unique<LogicalForeach>(
		nullptr, "i", listExpr, nullptr);
	auto phys = converter->convert(foreach.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// CallSubquery — covers imported vars and no-imported-vars branches
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, CallSubqueryWithImportedVars) {
	auto input = std::make_unique<LogicalSingleRow>();
	auto subquery = std::make_unique<LogicalSingleRow>();
	std::vector<std::string> imported{"n"};
	std::vector<std::string> returned{"result"};

	auto callSub = std::make_unique<LogicalCallSubquery>(
		std::move(input), std::move(subquery), imported, returned);
	auto phys = converter->convert(callSub.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, CallSubqueryNoImportedVars) {
	auto input = std::make_unique<LogicalSingleRow>();
	auto subquery = std::make_unique<LogicalSingleRow>();
	std::vector<std::string> empty;
	std::vector<std::string> returned{"result"};

	auto callSub = std::make_unique<LogicalCallSubquery>(
		std::move(input), std::move(subquery), empty, returned);
	auto phys = converter->convert(callSub.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, CallSubqueryNullInputAndSubquery) {
	std::vector<std::string> imported;
	std::vector<std::string> returned;
	auto callSub = std::make_unique<LogicalCallSubquery>(
		nullptr, nullptr, imported, returned);
	auto phys = converter->convert(callSub.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// LoadCsv — covers child vs no-child branches
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, LoadCsvWithChild) {
	auto child = std::make_unique<LogicalSingleRow>();
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///data.csv"));
	auto loadCsv = std::make_unique<LogicalLoadCsv>(
		std::move(child), urlExpr, "row", true, ",");
	auto phys = converter->convert(loadCsv.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, LoadCsvWithoutChild) {
	auto urlExpr = std::make_shared<LiteralExpression>(std::string("file:///data.csv"));
	auto loadCsv = std::make_unique<LogicalLoadCsv>(
		nullptr, urlExpr, "row", false, ";");
	auto phys = converter->convert(loadCsv.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// NamedPath — covers child vs no-child branches
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, NamedPathWithChild) {
	auto child = std::make_unique<LogicalSingleRow>();
	std::vector<std::string> nodeVars{"a", "b"};
	std::vector<std::string> edgeVars{"r"};
	auto namedPath = std::make_unique<LogicalNamedPath>(
		std::move(child), "p", nodeVars, edgeVars);
	auto phys = converter->convert(namedPath.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, NamedPathWithoutChild) {
	std::vector<std::string> nodeVars{"a"};
	std::vector<std::string> edgeVars;
	auto namedPath = std::make_unique<LogicalNamedPath>(
		nullptr, "p", nodeVars, edgeVars);
	auto phys = converter->convert(namedPath.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// MergeNode/MergeEdge with children
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, MergeNodeWithChild) {
	auto child = std::make_unique<LogicalSingleRow>();
	std::vector<MergeSetAction> emptyActions;
	auto merge = std::make_unique<LogicalMergeNode>(
		"n", std::vector<std::string>{"Person"},
		std::unordered_map<std::string, graph::PropertyValue>{{"name", graph::PropertyValue("Alice")}},
		emptyActions, emptyActions, std::move(child));
	auto phys = converter->convert(merge.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, MergeEdgeWithChild) {
	auto child = std::make_unique<LogicalSingleRow>();
	std::vector<MergeSetAction> emptyActions;
	auto mergeEdge = std::make_unique<LogicalMergeEdge>(
		"a", "r", "b", "KNOWS", "out",
		std::unordered_map<std::string, graph::PropertyValue>{},
		emptyActions, emptyActions, std::move(child));
	auto phys = converter->convert(mergeEdge.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, MergeEdgeWithoutChild) {
	std::vector<MergeSetAction> emptyActions;
	auto mergeEdge = std::make_unique<LogicalMergeEdge>(
		"a", "r", "b", "KNOWS", "out",
		std::unordered_map<std::string, graph::PropertyValue>{},
		emptyActions, emptyActions, nullptr);
	auto phys = converter->convert(mergeEdge.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// CreateIndex — composite vs single
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, CreateIndexComposite) {
	auto ci = std::make_unique<LogicalCreateIndex>(
		"idx_comp", "Person", std::vector<std::string>{"name", "age"});
	auto phys = converter->convert(ci.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, CreateIndexSingle) {
	auto ci = std::make_unique<LogicalCreateIndex>("idx_single", "Person", "name");
	auto phys = converter->convert(ci.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// DropIndex — by name vs by label+property
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, DropIndexByName) {
	auto di = std::make_unique<LogicalDropIndex>("idx_name", "", "");
	auto phys = converter->convert(di.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, DropIndexByLabelProperty) {
	auto di = std::make_unique<LogicalDropIndex>("", "Person", "name");
	auto phys = converter->convert(di.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// CreateVectorIndex
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, CreateVectorIndex) {
	auto cvi = std::make_unique<LogicalCreateVectorIndex>(
		"vec_idx", "Document", "embedding", 128, "L2");
	auto phys = converter->convert(cvi.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// Explain and Profile
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, Explain) {
	auto inner = std::make_unique<LogicalSingleRow>();
	auto explain = std::make_unique<LogicalExplain>(std::move(inner));
	// Keep inner alive: LogicalExplain stores raw pointer
	auto phys = converter->convert(explain.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, Profile) {
	auto inner = std::make_unique<LogicalSingleRow>();
	auto profile = std::make_unique<LogicalProfile>(std::move(inner));
	auto phys = converter->convert(profile.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// Join and OptionalMatch
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, JoinConversion) {
	auto left = std::make_unique<LogicalNodeScan>("a", std::vector<std::string>{"Person"});
	auto right = std::make_unique<LogicalNodeScan>("b", std::vector<std::string>{"City"});
	auto join = std::make_unique<LogicalJoin>(std::move(left), std::move(right));
	auto phys = converter->convert(join.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, OptionalMatchConversion) {
	auto input = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto pattern = std::make_unique<LogicalNodeScan>("m", std::vector<std::string>{"City"});
	std::vector<std::string> vars{"m"};
	auto opt = std::make_unique<LogicalOptionalMatch>(
		std::move(input), std::move(pattern), vars);
	auto phys = converter->convert(opt.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// CreateNode/CreateEdge with and without children
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, CreateNodeWithChild) {
	auto child = std::make_unique<LogicalSingleRow>();
	auto cn = std::make_unique<LogicalCreateNode>(
		"n", std::vector<std::string>{"Person"},
		std::unordered_map<std::string, graph::PropertyValue>{{"name", graph::PropertyValue("Bob")}},
		std::unordered_map<std::string, std::shared_ptr<Expression>>{});
	cn->setChild(0, std::move(child));
	auto phys = converter->convert(cn.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, CreateNodeWithoutChild) {
	auto cn = std::make_unique<LogicalCreateNode>(
		"n", std::vector<std::string>{"Person"},
		std::unordered_map<std::string, graph::PropertyValue>{},
		std::unordered_map<std::string, std::shared_ptr<Expression>>{});
	auto phys = converter->convert(cn.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, CreateEdgeWithChild) {
	// First create nodes, then edge
	auto createA = std::make_unique<LogicalCreateNode>(
		"a", std::vector<std::string>{"Person"},
		std::unordered_map<std::string, graph::PropertyValue>{},
		std::unordered_map<std::string, std::shared_ptr<Expression>>{});
	auto ce = std::make_unique<LogicalCreateEdge>(
		"r", "KNOWS",
		std::unordered_map<std::string, graph::PropertyValue>{},
		"a", "b");
	ce->setChild(0, std::move(createA));
	auto phys = converter->convert(ce.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, CreateEdgeWithoutChild) {
	auto ce = std::make_unique<LogicalCreateEdge>(
		"r", "KNOWS",
		std::unordered_map<std::string, graph::PropertyValue>{},
		"a", "b");
	auto phys = converter->convert(ce.get());
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// Aggregate with stdev/stdevp functions (not yet covered)
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, AggregateStdev) {
	auto child = std::make_unique<LogicalSingleRow>();
	std::vector<std::shared_ptr<Expression>> groupByExprs;
	std::vector<LogicalAggItem> aggs;
	aggs.emplace_back("stdev", std::make_shared<VariableReferenceExpression>("x"), "s");
	LogicalAggregate agg(std::move(child), std::move(groupByExprs), std::move(aggs), {});
	auto phys = converter->convert(&agg);
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, AggregateStdevp) {
	auto child = std::make_unique<LogicalSingleRow>();
	std::vector<std::shared_ptr<Expression>> groupByExprs;
	std::vector<LogicalAggItem> aggs;
	aggs.emplace_back("stdevp", std::make_shared<VariableReferenceExpression>("x"), "sp");
	LogicalAggregate agg(std::move(child), std::move(groupByExprs), std::move(aggs), {});
	auto phys = converter->convert(&agg);
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// Aggregate with groupBy alias fallback (non-empty alias)
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, AggregateGroupByWithAlias) {
	auto child = std::make_unique<LogicalSingleRow>();
	auto groupExpr = std::make_shared<VariableReferenceExpression>("city");
	std::vector<std::shared_ptr<Expression>> groupByExprs{groupExpr};
	std::vector<std::string> groupByAliases{"cityAlias"};
	std::vector<LogicalAggItem> aggs;
	aggs.emplace_back("count", std::make_shared<VariableReferenceExpression>("n"), "cnt");
	LogicalAggregate agg(std::move(child), std::move(groupByExprs), std::move(aggs),
	                     groupByAliases);
	auto phys = converter->convert(&agg);
	ASSERT_NE(phys, nullptr);
}

// ============================================================================
// SingleRow with override (already tested implicitly by Foreach, but explicit)
// ============================================================================

TEST_F(PhysicalPlanConverterTraversalTest, SingleRowDefault) {
	LogicalSingleRow sr;
	auto phys = converter->convert(&sr);
	ASSERT_NE(phys, nullptr);
	auto *singleRow = dynamic_cast<SingleRowOperator *>(phys.get());
	EXPECT_NE(singleRow, nullptr);
}
