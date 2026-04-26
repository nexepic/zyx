/**
 * @file test_PhysicalPlanConverter_LambdaPredicates.cpp
 * @brief Branch-coverage tests for PhysicalPlanConverter lambda predicates.
 *
 * These tests construct logical operator trees, convert them to physical
 * operators via PhysicalPlanConverter, then execute the resulting operators
 * with real data so that internal lambda predicates (multi-label filters,
 * property filters, range filters, traversal target/edge filters) are exercised
 * on both matching and non-matching records.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/execution/operators/FilterOperator.hpp"
#include "graph/query/execution/operators/NodeScanOperator.hpp"
#include "graph/query/execution/operators/TraversalOperator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalCreateConstraint.hpp"
#include "graph/query/logical/operators/LogicalDropConstraint.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalShowConstraints.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/logical/operators/LogicalVarLengthTraversal.hpp"
#include "graph/query/planner/PhysicalPlanConverter.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/constraints/ConstraintManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace fs = std::filesystem;
using namespace graph::query;
using namespace graph::query::logical;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

class PhysicalPlanConverterLambdaPredicatesTest : public ::testing::Test {
protected:
	fs::path testFilePath;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	std::shared_ptr<graph::storage::constraints::ConstraintManager> constraintManager;
	std::unique_ptr<PhysicalPlanConverter> converter;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_ppc_br_" + to_string(uuid) + ".zyx");

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

	static int drainCount(PhysicalOperator *op) {
		op->open();
		int count = 0;
		while (auto batch = op->next()) {
			count += static_cast<int>(batch->size());
		}
		op->close();
		return count;
	}
};

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, MultiLabelScan_NodeMissingOneLabel) {
	const auto labelA = dataManager->getOrCreateTokenId("Alpha");
	const auto labelB = dataManager->getOrCreateTokenId("Beta");

	graph::Node n1(0, labelA);
	dataManager->addNode(n1);

	graph::Node n2(0, labelA);
	n2.addLabelId(labelB);
	dataManager->addNode(n2);

	auto scan = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Alpha", "Beta"});
	auto phys = converter->convert(scan.get());
	ASSERT_NE(phys, nullptr);

	int count = drainCount(phys.get());
	EXPECT_EQ(count, 1) << "Only the node with both labels should pass";
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, ResidualPropertyFilter_KeyNotFound) {
	const auto labelId = dataManager->getOrCreateTokenId("Thing");
	graph::Node n1(0, labelId);
	dataManager->addNode(n1);
	dataManager->addNodeProperties(n1.getId(), {{"name", graph::PropertyValue(std::string("X"))}});

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"Thing"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{
			{"color", graph::PropertyValue(std::string("red"))}
		});
	auto phys = converter->convert(scan.get());
	ASSERT_NE(phys, nullptr);

	int count = drainCount(phys.get());
	EXPECT_EQ(count, 0) << "Missing property key should filter out the node";
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, TraversalTargetLabel_MismatchFiltersOut) {
	const auto labelPerson = dataManager->getOrCreateTokenId("Person");
	const auto labelCity = dataManager->getOrCreateTokenId("City");
	const auto labelVillage = dataManager->getOrCreateTokenId("Village");

	graph::Node person(0, labelPerson);
	dataManager->addNode(person);

	graph::Node city(0, labelCity);
	dataManager->addNode(city);
	graph::Node village(0, labelVillage);
	dataManager->addNode(village);

	const auto edgeType = dataManager->getOrCreateTokenId("LIVES_IN");
	graph::Edge e1(0, person.getId(), city.getId(), edgeType);
	dataManager->addEdge(e1);
	graph::Edge e2(0, person.getId(), village.getId(), edgeType);
	dataManager->addEdge(e2);

	auto scanOp = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto trav = std::make_unique<LogicalTraversal>(
		std::move(scanOp), "n", "r", "m", "LIVES_IN", "out",
		std::vector<std::string>{"City"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{},
		std::unordered_map<std::string, graph::PropertyValue>{});

	auto phys = converter->convert(trav.get());
	ASSERT_NE(phys, nullptr);

	int count = drainCount(phys.get());
	EXPECT_EQ(count, 1) << "Only target with City label should pass";
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, TraversalTargetProperty_MismatchFiltersOut) {
	const auto labelPerson = dataManager->getOrCreateTokenId("Person");
	const auto labelPlace = dataManager->getOrCreateTokenId("Place");
	const auto edgeType = dataManager->getOrCreateTokenId("GOES_TO");

	graph::Node person(0, labelPerson);
	dataManager->addNode(person);

	graph::Node place1(0, labelPlace);
	dataManager->addNode(place1);
	dataManager->addNodeProperties(place1.getId(),
		{{"name", graph::PropertyValue(std::string("Paris"))}});

	graph::Node place2(0, labelPlace);
	dataManager->addNode(place2);
	dataManager->addNodeProperties(place2.getId(),
		{{"name", graph::PropertyValue(std::string("London"))}});

	graph::Edge e1(0, person.getId(), place1.getId(), edgeType);
	dataManager->addEdge(e1);
	graph::Edge e2(0, person.getId(), place2.getId(), edgeType);
	dataManager->addEdge(e2);

	std::vector<std::pair<std::string, graph::PropertyValue>> targetProps;
	targetProps.emplace_back("name", graph::PropertyValue(std::string("Paris")));

	auto scanOp = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto trav = std::make_unique<LogicalTraversal>(
		std::move(scanOp), "n", "r", "m", "GOES_TO", "out",
		std::vector<std::string>{},
		targetProps,
		std::unordered_map<std::string, graph::PropertyValue>{});

	auto phys = converter->convert(trav.get());
	ASSERT_NE(phys, nullptr);

	int count = drainCount(phys.get());
	EXPECT_EQ(count, 1) << "Only target with name=Paris should pass";
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, TraversalEdgeProperty_MismatchFiltersOut) {
	const auto labelPerson = dataManager->getOrCreateTokenId("Person");
	const auto labelCity = dataManager->getOrCreateTokenId("City");
	const auto edgeType = dataManager->getOrCreateTokenId("VISITED");

	graph::Node person(0, labelPerson);
	dataManager->addNode(person);

	graph::Node city(0, labelCity);
	dataManager->addNode(city);

	graph::Edge e1(0, person.getId(), city.getId(), edgeType);
	dataManager->addEdge(e1);
	dataManager->addEdgeProperties(e1.getId(),
		{{"year", graph::PropertyValue(static_cast<int64_t>(2020))}});

	graph::Edge e2(0, person.getId(), city.getId(), edgeType);
	dataManager->addEdge(e2);
	dataManager->addEdgeProperties(e2.getId(),
		{{"year", graph::PropertyValue(static_cast<int64_t>(2021))}});

	std::unordered_map<std::string, graph::PropertyValue> edgeProps;
	edgeProps["year"] = graph::PropertyValue(static_cast<int64_t>(2020));

	auto scanOp = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto trav = std::make_unique<LogicalTraversal>(
		std::move(scanOp), "n", "r", "m", "VISITED", "out",
		std::vector<std::string>{},
		std::vector<std::pair<std::string, graph::PropertyValue>>{},
		edgeProps);

	auto phys = converter->convert(trav.get());
	ASSERT_NE(phys, nullptr);

	int count = drainCount(phys.get());
	EXPECT_EQ(count, 1) << "Only edge with year=2020 should pass";
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, VarLengthTraversal_TargetLabelMismatch) {
	const auto labelPerson = dataManager->getOrCreateTokenId("Person");
	const auto labelCity = dataManager->getOrCreateTokenId("City");
	const auto labelTown = dataManager->getOrCreateTokenId("Town");
	const auto edgeType = dataManager->getOrCreateTokenId("KNOWS");

	graph::Node a(0, labelPerson);
	dataManager->addNode(a);

	graph::Node b(0, labelCity);
	dataManager->addNode(b);

	graph::Node c(0, labelTown);
	dataManager->addNode(c);

	graph::Edge e1(0, a.getId(), b.getId(), edgeType);
	dataManager->addEdge(e1);
	graph::Edge e2(0, a.getId(), c.getId(), edgeType);
	dataManager->addEdge(e2);

	auto scanOp = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto vlt = std::make_unique<LogicalVarLengthTraversal>(
		std::move(scanOp), "n", "r", "m", "KNOWS", "out", 1, 1,
		std::vector<std::string>{"City"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{});

	auto phys = converter->convert(vlt.get());
	ASSERT_NE(phys, nullptr);

	int count = drainCount(phys.get());
	EXPECT_EQ(count, 1) << "Only City target should pass the label filter";
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, VarLengthTraversal_TargetPropertyMismatch) {
	const auto labelPerson = dataManager->getOrCreateTokenId("Person");
	const auto edgeType = dataManager->getOrCreateTokenId("FOLLOWS");

	graph::Node a(0, labelPerson);
	dataManager->addNode(a);

	graph::Node b(0, labelPerson);
	dataManager->addNode(b);
	dataManager->addNodeProperties(b.getId(),
		{{"status", graph::PropertyValue(std::string("active"))}});

	graph::Node c(0, labelPerson);
	dataManager->addNode(c);
	dataManager->addNodeProperties(c.getId(),
		{{"status", graph::PropertyValue(std::string("inactive"))}});

	graph::Edge e1(0, a.getId(), b.getId(), edgeType);
	dataManager->addEdge(e1);
	graph::Edge e2(0, a.getId(), c.getId(), edgeType);
	dataManager->addEdge(e2);

	std::vector<std::pair<std::string, graph::PropertyValue>> targetProps;
	targetProps.emplace_back("status", graph::PropertyValue(std::string("active")));

	auto scanOp = std::make_unique<LogicalNodeScan>("n", std::vector<std::string>{"Person"});
	auto vlt = std::make_unique<LogicalVarLengthTraversal>(
		std::move(scanOp), "n", "r", "m", "FOLLOWS", "out", 1, 1,
		std::vector<std::string>{},
		targetProps);

	auto phys = converter->convert(vlt.get());
	ASSERT_NE(phys, nullptr);

	int count = drainCount(phys.get());
	EXPECT_EQ(count, 1) << "Only target with status=active should pass";
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, CompositeScanResidualFilter) {
	const auto labelId = dataManager->getOrCreateTokenId("Widget");
	indexManager->createCompositeIndex("idx_widget_name_size", "NODE",
		"Widget", std::vector<std::string>{"name", "size"});

	graph::Node n1(0, labelId);
	dataManager->addNode(n1);
	dataManager->addNodeProperties(n1.getId(), {
		{"name", graph::PropertyValue(std::string("Gear"))},
		{"size", graph::PropertyValue(static_cast<int64_t>(10))},
		{"color", graph::PropertyValue(std::string("red"))}
	});

	graph::Node n2(0, labelId);
	dataManager->addNode(n2);
	dataManager->addNodeProperties(n2.getId(), {
		{"name", graph::PropertyValue(std::string("Gear"))},
		{"size", graph::PropertyValue(static_cast<int64_t>(10))},
		{"color", graph::PropertyValue(std::string("blue"))}
	});

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"Widget"},
		std::vector<std::pair<std::string, graph::PropertyValue>>{
			{"name", graph::PropertyValue(std::string("Gear"))},
			{"size", graph::PropertyValue(static_cast<int64_t>(10))},
			{"color", graph::PropertyValue(std::string("red"))}
		});

	CompositeEqualityPredicate comp;
	comp.keys = {"name", "size"};
	comp.values = {graph::PropertyValue(std::string("Gear")), graph::PropertyValue(static_cast<int64_t>(10))};
	scan->setCompositeEquality(std::move(comp));

	auto phys = converter->convert(scan.get());
	ASSERT_NE(phys, nullptr);

	int count = drainCount(phys.get());
	EXPECT_EQ(count, 1) << "Only node with color=red should pass residual filter";
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, ShowConstraints) {
	LogicalShowConstraints sc;
	auto phys = converter->convert(&sc);
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, CreateConstraint) {
	LogicalCreateConstraint cc(
		"unique_person_name", "NODE", "UNIQUE", "Person",
		std::vector<std::string>{"name"}, "");
	auto phys = converter->convert(&cc);
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, DropConstraint) {
	LogicalDropConstraint dc("unique_person_name", true);
	auto phys = converter->convert(&dc);
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, CallProcedureUnknownThrows) {
	LogicalCallProcedure cp("nonexistent.proc", {});
	EXPECT_THROW((void)converter->convert(&cp), std::runtime_error);
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, CallProcedureKnown) {
	LogicalCallProcedure cp("dbms.showStats", {});
	auto phys = converter->convert(&cp);
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, AggregateGroupByEmptyAliasFallback) {
	auto child = std::make_unique<LogicalSingleRow>();
	auto groupExpr = std::make_shared<expressions::VariableReferenceExpression>("city");
	std::vector<std::shared_ptr<expressions::Expression>> groupByExprs{groupExpr};
	std::vector<std::string> groupByAliases{""};
	std::vector<LogicalAggItem> aggs;
	aggs.emplace_back("count",
		std::make_shared<expressions::VariableReferenceExpression>("n"), "cnt");
	LogicalAggregate agg(std::move(child), std::move(groupByExprs), std::move(aggs),
		groupByAliases);
	auto phys = converter->convert(&agg);
	ASSERT_NE(phys, nullptr);
}

TEST_F(PhysicalPlanConverterLambdaPredicatesTest, RangeResidualFilter_NotHandledByIndex) {
	const auto labelId = dataManager->getOrCreateTokenId("Item");

	graph::Node n1(0, labelId);
	dataManager->addNode(n1);
	dataManager->addNodeProperties(n1.getId(), {
		{"price", graph::PropertyValue(50.0)},
		{"weight", graph::PropertyValue(static_cast<int64_t>(5))}
	});

	graph::Node n2(0, labelId);
	dataManager->addNode(n2);
	dataManager->addNodeProperties(n2.getId(), {
		{"price", graph::PropertyValue(150.0)},
		{"weight", graph::PropertyValue(static_cast<int64_t>(15))}
	});

	auto scan = std::make_unique<LogicalNodeScan>(
		"n", std::vector<std::string>{"Item"});

	std::vector<RangePredicate> ranges;
	ranges.push_back(RangePredicate{
		"price",
		graph::PropertyValue(10.0),
		graph::PropertyValue(100.0),
		true, true
	});
	ranges.push_back(RangePredicate{
		"weight",
		graph::PropertyValue(static_cast<int64_t>(1)),
		graph::PropertyValue(static_cast<int64_t>(10)),
		true, true
	});
	scan->setRangePredicates(std::move(ranges));

	auto phys = converter->convert(scan.get());
	ASSERT_NE(phys, nullptr);

	int count = drainCount(phys.get());
	EXPECT_EQ(count, 1) << "Only n1 with price=50, weight=5 should pass both ranges";
}
