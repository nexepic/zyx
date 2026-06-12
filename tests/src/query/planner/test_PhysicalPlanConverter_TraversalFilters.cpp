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
#include "graph/query/execution/operators/NodeTopKScanOperator.hpp"
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
#include "graph/query/execution/operators/ProjectOperator.hpp"
#include "graph/query/execution/operators/SortOperator.hpp"
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
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
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

TEST_F(PhysicalPlanConverterTraversalTest, MultiLabelScanRejectsNodesMissingSecondaryLabel) {
	const auto firstLabel = dataManager->getOrCreateTokenId("MultilabelPerson");
	const auto secondLabel = dataManager->getOrCreateTokenId("MultilabelEmployee");
	graph::Node both(0, firstLabel);
	ASSERT_TRUE(both.addLabelId(secondLabel));
	graph::Node firstOnly(0, firstLabel);
	dataManager->addNode(both);
	dataManager->addNode(firstOnly);

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"MultilabelPerson", "MultilabelEmployee"});
	auto phys = converter->convert(scan.get());
	ASSERT_NE(phys, nullptr);

	phys->open();
	auto batch = phys->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	auto node = batch->front().getNode("n");
	ASSERT_TRUE(node.has_value());
	EXPECT_EQ(node->getId(), both.getId());
	EXPECT_FALSE(phys->next().has_value());
	phys->close();
}

TEST_F(PhysicalPlanConverterTraversalTest, ResidualPropertyScanRejectsNodesMissingRequestedProperty) {
	const auto label = dataManager->getOrCreateTokenId("ResidualPropertyPerson");
	graph::Node complete(0, label);
	complete.addProperty("status", graph::PropertyValue("active"));
	complete.addProperty("tenant", graph::PropertyValue("east"));
	graph::Node missingTenant(0, label);
	missingTenant.addProperty("status", graph::PropertyValue("active"));
	dataManager->addNode(complete);
	dataManager->addNode(missingTenant);

	std::vector<std::pair<std::string, graph::PropertyValue>> predicates = {
		{"status", graph::PropertyValue("active")},
		{"tenant", graph::PropertyValue("east")},
	};
	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"ResidualPropertyPerson"}, predicates);
	auto phys = converter->convert(scan.get());
	ASSERT_NE(phys, nullptr);

	phys->open();
	auto batch = phys->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	auto node = batch->front().getNode("n");
	ASSERT_TRUE(node.has_value());
	EXPECT_EQ(node->getId(), complete.getId());
	EXPECT_FALSE(phys->next().has_value());
	phys->close();
}

TEST_F(PhysicalPlanConverterTraversalTest, CompositeNodeScanKeepsOnlyResidualPropertyPredicates) {
	const auto personLabel = dataManager->getOrCreateTokenId("CompositePerson");
	graph::Node active(0, personLabel);
	graph::Node inactiveStatus(0, personLabel);
	dataManager->addNode(active);
	dataManager->addNode(inactiveStatus);
	dataManager->addNodeProperties(active.getId(), {
			{"country", graph::PropertyValue("CN")},
			{"age", graph::PropertyValue(int64_t{30})},
			{"status", graph::PropertyValue("active")}});
	dataManager->addNodeProperties(inactiveStatus.getId(), {
			{"country", graph::PropertyValue("CN")},
			{"age", graph::PropertyValue(int64_t{30})},
			{"status", graph::PropertyValue("inactive")}});
	ASSERT_TRUE(indexManager->createCompositeIndex("idx_composite_person_country_age",
	                                               "node",
	                                               "CompositePerson",
	                                               {"country", "age"}));

	std::vector<std::pair<std::string, graph::PropertyValue>> predicates = {
			{"country", graph::PropertyValue("CN")},
			{"age", graph::PropertyValue(int64_t{30})},
			{"status", graph::PropertyValue("active")},
	};
	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"CompositePerson"}, predicates);
	scan->setCompositeEquality({{"country", "age"}, {graph::PropertyValue("CN"), graph::PropertyValue(int64_t{30})}});

	auto phys = converter->convert(scan.get());
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterTraversalTest, IndexedRangeScanOmitsMatchingResidualRangePredicate) {
	const auto userLabel = dataManager->getOrCreateTokenId("RangePerson");
	graph::Node inside(0, userLabel);
	graph::Node outside(0, userLabel);
	dataManager->addNode(inside);
	dataManager->addNode(outside);
	dataManager->addNodeProperties(inside.getId(), {{"age", graph::PropertyValue(int64_t{30})}});
	dataManager->addNodeProperties(outside.getId(), {{"age", graph::PropertyValue(int64_t{70})}});
	ASSERT_TRUE(indexManager->createIndex("idx_range_person_age", "node", "RangePerson", "age"));

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"RangePerson"});
	scan->setRangePredicates({RangePredicate{"age", graph::PropertyValue(int64_t{18}), graph::PropertyValue(int64_t{65}), true, true}});

	auto phys = converter->convert(scan.get());
	ASSERT_NE(phys, nullptr);
	phys->open();
	auto batch = phys->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1U);
	auto node = batch->front().getNode("n");
	ASSERT_TRUE(node.has_value());
	EXPECT_EQ(node->getId(), inside.getId());
	EXPECT_FALSE(phys->next().has_value());
	phys->close();
}

TEST_F(PhysicalPlanConverterTraversalTest, LimitOverSortUsesBoundedSort) {
	auto input = std::make_unique<LogicalSingleRow>();

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(std::make_shared<VariableReferenceExpression>("x"), false);
	auto sort = std::make_unique<LogicalSort>(std::move(input), std::move(sortItems));
	auto limit = std::make_unique<LogicalLimit>(std::move(sort), 3);

	auto phys = converter->convert(limit.get());
	auto *sortPhys = dynamic_cast<SortOperator *>(phys.get());
	ASSERT_NE(sortPhys, nullptr);
	EXPECT_NE(sortPhys->toString().find("LIMIT 3"), std::string::npos);
}

TEST_F(PhysicalPlanConverterTraversalTest, ProjectLimitSortNodeScanUsesTopKScanPath) {
	const auto userLabel = dataManager->getOrCreateTokenId("User");
	graph::Node low(1, userLabel);
	graph::Node high(2, userLabel);
	graph::Node mid(3, userLabel);
	dataManager->addNode(low);
	dataManager->addNode(high);
	dataManager->addNode(mid);
	dataManager->addNodeProperties(low.getId(), {
		{"id", graph::PropertyValue(std::string("low"))},
		{"score", graph::PropertyValue(1.0)},
		{"extra", graph::PropertyValue(std::string("ignored"))}});
	dataManager->addNodeProperties(high.getId(), {
		{"id", graph::PropertyValue(std::string("high"))},
		{"score", graph::PropertyValue(9.0)},
		{"extra", graph::PropertyValue(std::string("ignored"))}});
	dataManager->addNodeProperties(mid.getId(), {
		{"id", graph::PropertyValue(std::string("mid"))},
		{"score", graph::PropertyValue(5.0)},
		{"extra", graph::PropertyValue(std::string("ignored"))}});
	ASSERT_TRUE(indexManager->createIndex("idx_user_label_topk", "node", "User", ""));

	auto scan = std::make_unique<LogicalNodeScan>("u", std::vector<std::string>{"User"});

	std::vector<LogicalSortItem> sortItems;
	sortItems.emplace_back(std::make_shared<VariableReferenceExpression>("u", "score"), false);
	auto sort = std::make_unique<LogicalSort>(std::move(scan), std::move(sortItems));
	auto limit = std::make_unique<LogicalLimit>(std::move(sort), 2);

	std::vector<LogicalProjectItem> projectItems;
	projectItems.emplace_back(std::make_shared<VariableReferenceExpression>("u", "id"), "id");
	auto project = std::make_unique<LogicalProject>(std::move(limit), std::move(projectItems));

	auto phys = converter->convert(project.get());
	auto *topKPhys = dynamic_cast<NodeTopKScanOperator *>(phys.get());
	ASSERT_NE(topKPhys, nullptr);

	phys->open();
	auto batch = phys->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2UL);
	ASSERT_TRUE((*batch)[0].getValue("id").has_value());
	ASSERT_TRUE((*batch)[1].getValue("id").has_value());
	EXPECT_EQ(std::get<std::string>((*batch)[0].getValue("id")->getVariant()), "high");
	EXPECT_EQ(std::get<std::string>((*batch)[1].getValue("id")->getVariant()), "mid");
	EXPECT_FALSE(phys->next().has_value());
	phys->close();
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
