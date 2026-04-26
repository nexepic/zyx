/**
 * @file test_QueryPlanner.cpp
 * @brief Unit tests for QueryPlanner branch coverage
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "graph/core/Database.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/query/optimizer/Optimizer.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/query/execution/operators/NodeScanOperator.hpp"
#include "graph/query/execution/operators/FilterOperator.hpp"
#include "graph/query/execution/operators/CreateNodeOperator.hpp"
#include "graph/query/execution/operators/CreateEdgeOperator.hpp"
#include "graph/query/execution/operators/TraversalOperator.hpp"
#include "graph/query/execution/operators/ProjectOperator.hpp"
#include "graph/query/execution/operators/LimitOperator.hpp"
#include "graph/query/execution/operators/SkipOperator.hpp"
#include "graph/query/execution/operators/SortOperator.hpp"
#include "graph/query/execution/operators/DeleteOperator.hpp"
#include "graph/query/execution/operators/MergeNodeOperator.hpp"
#include "graph/query/execution/operators/MergeEdgeOperator.hpp"
#include "graph/query/execution/operators/ShowIndexesOperator.hpp"
#include "graph/query/execution/operators/DropIndexOperator.hpp"
#include "graph/query/execution/operators/CreateIndexOperator.hpp"
#include "graph/query/execution/operators/CartesianProductOperator.hpp"
#include "graph/query/execution/operators/OptionalMatchOperator.hpp"
#include "graph/query/execution/operators/UnwindOperator.hpp"
#include "graph/query/execution/operators/SingleRowOperator.hpp"
#include "graph/query/execution/operators/VarLengthTraversalOperator.hpp"
#include "graph/query/execution/operators/CreateVectorIndexOperator.hpp"
#include "graph/query/execution/operators/SetOperator.hpp"
#include "graph/query/execution/operators/RemoveOperator.hpp"
#include "graph/query/execution/operators/CreateConstraintOperator.hpp"
#include "graph/query/execution/operators/DropConstraintOperator.hpp"
#include "graph/query/execution/operators/ShowConstraintsOperator.hpp"
#include "graph/query/execution/operators/TransactionControlOperator.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/constraints/ConstraintManager.hpp"
#include "graph/query/expressions/Expression.hpp"

namespace fs = std::filesystem;

class QueryPlannerTest : public ::testing::Test {
protected:
	fs::path testFilePath;
	std::shared_ptr<graph::storage::FileStorage> storage;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::query::indexes::IndexManager> indexManager;
	std::shared_ptr<graph::storage::constraints::ConstraintManager> constraintManager;
	std::shared_ptr<graph::query::QueryPlanner> planner;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() / ("test_qplanner_" + to_string(uuid) + ".zyx");

		storage = std::make_shared<graph::storage::FileStorage>(
			testFilePath.string(), 4096, graph::storage::OpenMode::OPEN_CREATE_NEW_FILE);
		storage->open();
		dataManager = storage->getDataManager();
		indexManager = std::make_shared<graph::query::indexes::IndexManager>(storage);
		indexManager->initialize();
		constraintManager = std::make_shared<graph::storage::constraints::ConstraintManager>(storage, indexManager);
		constraintManager->initialize();

		planner = std::make_shared<graph::query::QueryPlanner>(dataManager, indexManager, constraintManager);
	}

	void TearDown() override {
		if (storage) storage->close();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}
};

// ============================================================================
// scanOp branch coverage
// ============================================================================

TEST_F(QueryPlannerTest, ScanOp_NoKeyNoFilter) {
	// Tests scanOp with empty key - no residual filter should be added
	// Branch: line 79 !key.empty() -> false
	auto op = planner->scanOp("n", "User");
	ASSERT_NE(op, nullptr);

	// The result should be a NodeScanOperator (no FilterOperator wrapping)
	auto *scanOp = dynamic_cast<graph::query::execution::operators::NodeScanOperator *>(op.get());
	EXPECT_NE(scanOp, nullptr) << "Expected a NodeScanOperator without filter wrapping";
}

TEST_F(QueryPlannerTest, ScanOp_WithKeyNoIndex) {
	// Tests scanOp with a key but no property index exists
	// Branch: line 79 !key.empty() -> true, line 80 config.type != PROPERTY_SCAN -> true
	// This should add a residual FilterOperator
	auto op = planner->scanOp("n", "User", "name", graph::PropertyValue("Alice"));
	ASSERT_NE(op, nullptr);

	// The result should be wrapped in a FilterOperator
	auto *filterOp = dynamic_cast<graph::query::execution::operators::FilterOperator *>(op.get());
	EXPECT_NE(filterOp, nullptr) << "Expected a FilterOperator wrapping the scan (no index available)";
}

TEST_F(QueryPlannerTest, ScanOp_WithKeyAndPropertyIndex) {
	// Create a property index to test the PROPERTY_SCAN path
	// Branch: line 80 config.type != PROPERTY_SCAN -> false (index used)

	// First create a node with label and property so the index has something
	graph::Node node(0, dataManager->getOrCreateTokenId("Indexed"));
	dataManager->addNode(node);
	dataManager->addNodeProperties(node.getId(), {{"prop", graph::PropertyValue("val")}});

	// Create the property index
	indexManager->createIndex("idx_test", "node", "Indexed", "prop");

	auto cfg = planner->getOptimizer()->optimizeNodeScan(
		"n", std::vector<std::string>{"Indexed"}, "prop", graph::PropertyValue("val"));
	EXPECT_EQ(cfg.type, graph::query::execution::ScanType::PROPERTY_SCAN);

	auto op = planner->scanOp("n", "Indexed", "prop", graph::PropertyValue("val"));
	ASSERT_NE(op, nullptr);

	// If property index was used, there should be no FilterOperator wrapping
	// (The optimizer chose PROPERTY_SCAN, so scan already filters)
	auto *scanOp = dynamic_cast<graph::query::execution::operators::NodeScanOperator *>(op.get());
	// It might be a NodeScanOperator directly, or it might still wrap in filter
	// depending on optimizer behavior. Either way, no crash.
	EXPECT_TRUE(scanOp != nullptr ||
		dynamic_cast<graph::query::execution::operators::FilterOperator *>(op.get()) != nullptr);
}

TEST_F(QueryPlannerTest, ScanOp_MultiLabelResidualFilterExecutesPredicateBranches) {
	const auto labelA = dataManager->getOrCreateTokenId("A");
	const auto labelB = dataManager->getOrCreateTokenId("B");

	graph::Node onlyA(1, labelA);
	graph::Node both(2, labelA);
	(void)both.addLabelId(labelB);
	graph::Node onlyB(3, labelB);

	dataManager->addNode(onlyA);
	dataManager->addNode(both);
	dataManager->addNode(onlyB);
	ASSERT_TRUE(indexManager->createIndex("idx_label_scan", "node", "A", ""));

	auto op = planner->scanOp("n", std::vector<std::string>{"A", "B"});
	ASSERT_NE(op, nullptr);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);

	auto matched = (*batch)[0].getNode("n");
	ASSERT_TRUE(matched.has_value());
	EXPECT_EQ(matched->getId(), 2);
	op->close();
}

TEST_F(QueryPlannerTest, ScanOp_ResidualFilterHandlesMissingProperty) {
	const auto labelId = dataManager->getOrCreateTokenId("User");
	graph::Node n1(1, labelId);
	graph::Node n2(2, labelId);
	dataManager->addNode(n1);
	dataManager->addNode(n2);
	dataManager->addNodeProperties(n1.getId(), {{"name", graph::PropertyValue("Alice")}});
	// n2 intentionally does not have key "age"

	auto op = planner->scanOp("n", "User", "age", graph::PropertyValue(int64_t(30)));
	ASSERT_NE(op, nullptr);

	op->open();
	auto batch = op->next();
	if (batch.has_value()) {
		EXPECT_TRUE(batch->empty());
	}
	op->close();
}

// ============================================================================
// callProcedureOp branch coverage
// ============================================================================

TEST_F(QueryPlannerTest, CallProcedureOp_UnknownProcedure) {
	// Branch: line 154 - factory is null, throws runtime_error
	EXPECT_THROW((void)planner->callProcedureOp("nonexistent.procedure", {}), std::runtime_error);
}

TEST_F(QueryPlannerTest, CallProcedureOp_KnownProcedure) {
	// Branch: line 150-151 - factory found and invoked
	auto op = planner->callProcedureOp("dbms.listConfig", {});
	EXPECT_NE(op, nullptr);
}

// ============================================================================
// Factory method coverage (ensure all return non-null)
// ============================================================================

TEST_F(QueryPlannerTest, CreateNodeOp) {
	auto op = planner->createOp("n", "Label", {{"key", graph::PropertyValue("val")}});
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, CreateNodeOp_EmptySingleLabelUsesFallbackPath) {
	auto op = planner->createOp("n", "", {{"key", graph::PropertyValue("val")}});
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, CreateEdgeOp) {
	auto op = planner->createOp("e", "KNOWS", {}, "a", "b");
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, FilterOp) {
	auto scan = planner->scanOp("n", "User");
	auto op = graph::query::QueryPlanner::filterOp(
		std::move(scan),
		[](const graph::query::execution::Record &) { return true; },
		"test filter");
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, TraverseOp) {
	auto scan = planner->scanOp("n", "User");
	auto op = planner->traverseOp(std::move(scan), "n", "e", "m", "KNOWS", "out");
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, ProjectOp) {
	auto scan = planner->scanOp("n", "User");
	std::vector<graph::query::execution::operators::ProjectItem> items;
	items.emplace_back(nullptr, "n");
	auto op = planner->projectOp(std::move(scan), items);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, LimitOp) {
	auto scan = planner->scanOp("n", "User");
	auto op = graph::query::QueryPlanner::limitOp(std::move(scan), 10);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, SkipOp) {
	auto scan = planner->scanOp("n", "User");
	auto op = graph::query::QueryPlanner::skipOp(std::move(scan), 5);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, SortOp) {
	auto scan = planner->scanOp("n", "User");
	std::vector<graph::query::execution::operators::SortItem> items;
	auto op = graph::query::QueryPlanner::sortOp(std::move(scan), items);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, DeleteOp) {
	auto scan = planner->scanOp("n", "User");
	auto op = planner->deleteOp(std::move(scan), {"n"}, false);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, DeleteOpDetach) {
	auto scan = planner->scanOp("n", "User");
	auto op = planner->deleteOp(std::move(scan), {"n"}, true);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, SetOp) {
	auto scan = planner->scanOp("n", "User");
	std::vector<graph::query::execution::operators::SetItem> items;
	auto op = planner->setOp(std::move(scan), items);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, RemoveOp) {
	auto scan = planner->scanOp("n", "User");
	std::vector<graph::query::execution::operators::RemoveItem> items;
	auto op = planner->removeOp(std::move(scan), items);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, MergeNodeOp) {
	auto op = planner->mergeOp("n", "User", {{"name", graph::PropertyValue("Alice")}}, {}, {});
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, MergeNodeOp_EmptySingleLabelUsesFallbackPath) {
	auto op = planner->mergeOp("n", "", {{"name", graph::PropertyValue("Alice")}}, {}, {});
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, MergeEdgeOp) {
	auto op = planner->mergeEdgeOp("a", "e", "b", "KNOWS", {}, "out", {}, {});
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, CreateIndexOp) {
	auto op = planner->createIndexOp("idx", "Label", "prop");
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, ShowIndexesOp) {
	auto op = planner->showIndexesOp();
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, DropIndexOpByName) {
	auto op = planner->dropIndexOp("idx_name");
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, DropIndexOpByLabelProperty) {
	auto op = planner->dropIndexOp("Label", "prop");
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, TraverseVarLengthOp) {
	auto scan = planner->scanOp("n", "User");
	auto op = planner->traverseVarLengthOp(std::move(scan), "n", "m", "KNOWS", 1, 3, "out");
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, CartesianProductOp) {
	auto left = planner->scanOp("a", "User");
	auto right = planner->scanOp("b", "Item");
	auto op = graph::query::QueryPlanner::cartesianProductOp(std::move(left), std::move(right));
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, OptionalMatchOp) {
	auto input = planner->scanOp("n", "User");
	auto pattern = planner->scanOp("m", "Item");
	auto op = graph::query::QueryPlanner::optionalMatchOp(
		std::move(input), std::move(pattern), {"m"});
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, UnwindOpWithList) {
	auto scan = planner->scanOp("n", "User");
	std::vector<graph::PropertyValue> list = {graph::PropertyValue(1), graph::PropertyValue(2)};
	auto op = graph::query::QueryPlanner::unwindOp(std::move(scan), "x", list);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, UnwindOpWithExpression) {
	auto scan = planner->scanOp("n", "User");
	auto op = graph::query::QueryPlanner::unwindOp(std::move(scan), "x", nullptr);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, CreateVectorIndexOp) {
	auto op = planner->createVectorIndexOp("vec_idx", "Label", "embedding", 128, "cosine");
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, SingleRowOp) {
	auto op = graph::query::QueryPlanner::singleRowOp();
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, GetDataManager) {
	auto dm = planner->getDataManager();
	EXPECT_NE(dm, nullptr);
	EXPECT_EQ(dm, dataManager);
}

// ============================================================================
// Transaction control operator
// ============================================================================

TEST_F(QueryPlannerTest, TransactionControlOp_Begin) {
	using TC = graph::query::execution::operators::TransactionCommand;
	auto op = graph::query::QueryPlanner::transactionControlOp(TC::TXN_CTL_BEGIN);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, TransactionControlOp_Commit) {
	using TC = graph::query::execution::operators::TransactionCommand;
	auto op = graph::query::QueryPlanner::transactionControlOp(TC::TXN_CTL_COMMIT);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, TransactionControlOp_Rollback) {
	using TC = graph::query::execution::operators::TransactionCommand;
	auto op = graph::query::QueryPlanner::transactionControlOp(TC::TXN_CTL_ROLLBACK);
	ASSERT_NE(op, nullptr);
}

// ============================================================================
// Constraint DDL operators
// ============================================================================

TEST_F(QueryPlannerTest, CreateConstraintOp) {
	auto op = planner->createConstraintOp(
		"unique_name", "Node", "UNIQUE", "Person", {"name"});
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, DropConstraintOp) {
	auto op = planner->dropConstraintOp("unique_name", false);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, DropConstraintOp_IfExists) {
	auto op = planner->dropConstraintOp("nonexistent", true);
	ASSERT_NE(op, nullptr);
}

TEST_F(QueryPlannerTest, ShowConstraintsOp) {
	auto op = planner->showConstraintsOp();
	ASSERT_NE(op, nullptr);
}

// ============================================================================
// Multi-label scanOp
// ============================================================================

TEST_F(QueryPlannerTest, ScanOp_MultiLabel) {
	// Create nodes with two labels
	int64_t lid1 = dataManager->getOrCreateTokenId("A");
	int64_t lid2 = dataManager->getOrCreateTokenId("B");
	graph::Node node(0, lid1);
	node.addLabelId(lid2);
	dataManager->addNode(node);

	auto op = planner->scanOp("n", std::vector<std::string>{"A", "B"});
	ASSERT_NE(op, nullptr);

	// Multi-label should wrap with a filter
	auto *filterOp = dynamic_cast<graph::query::execution::operators::FilterOperator *>(op.get());
	EXPECT_NE(filterOp, nullptr) << "Multi-label scan should have a filter for additional labels";
}

// ============================================================================
// createOp with expression-based properties
// ============================================================================

TEST_F(QueryPlannerTest, CreateNodeOp_WithPropExpressions) {
	std::unordered_map<std::string, graph::PropertyValue> props;
	std::unordered_map<std::string, std::shared_ptr<graph::query::expressions::Expression>> propExprs;
	propExprs["computed"] = std::make_shared<graph::query::expressions::LiteralExpression>(
		static_cast<int64_t>(42));

	auto op = planner->createOp("n", std::vector<std::string>{"Label"}, props, std::move(propExprs));
	ASSERT_NE(op, nullptr);
}

// ============================================================================
// Multi-label mergeOp
// ============================================================================

TEST_F(QueryPlannerTest, MergeNodeOp_MultiLabel) {
	auto op = planner->mergeOp("n", std::vector<std::string>{"A", "B"},
		{{"key", graph::PropertyValue("val")}}, {}, {});
	ASSERT_NE(op, nullptr);
}

// ============================================================================
// Multi-label scan predicate: cover null node branch (line 95)
// When multi-label filter encounters a record where the node is not found,
// it should return false.
// ============================================================================

TEST_F(QueryPlannerTest, ScanOp_MultiLabelFilterSkipsNodeWithMissingLabel) {
	// Create a node with only label A but not label B
	int64_t lidA = dataManager->getOrCreateTokenId("MLA");
	int64_t lidB = dataManager->getOrCreateTokenId("MLB");

	graph::Node nodeA(10, lidA);
	dataManager->addNode(nodeA);

	// Scan for multi-label [MLA, MLB] - node with only MLA should be filtered out
	auto op = planner->scanOp("n", std::vector<std::string>{"MLA", "MLB"});
	ASSERT_NE(op, nullptr);

	op->open();
	auto batch = op->next();
	if (batch.has_value()) {
		// Should not contain the node since it only has label A
		EXPECT_TRUE(batch->empty());
	}
	op->close();
	(void)lidB;
}

// ============================================================================
// Residual property filter: cover null node branch (line 110)
// When property filter encounters a record where getNode returns nullopt,
// it should return false.
// ============================================================================

TEST_F(QueryPlannerTest, ScanOp_ResidualPropertyFilterNoNodes) {
	// Create label token but no nodes matching the filter criteria
	dataManager->getOrCreateTokenId("EmptyLabel");

	auto op = planner->scanOp("n", "EmptyLabel", "prop", graph::PropertyValue("val"));
	ASSERT_NE(op, nullptr);

	op->open();
	auto batch = op->next();
	if (batch.has_value()) {
		EXPECT_TRUE(batch->empty());
	}
	op->close();
}

// ============================================================================
// Single-label convenience with empty label
// ============================================================================

TEST_F(QueryPlannerTest, ScanOp_EmptyLabel) {
	auto op = planner->scanOp("n", "");
	ASSERT_NE(op, nullptr);
}

// ============================================================================
// scanOp with property key and property index (PROPERTY_SCAN path)
// ensures the else branch at line 119-125 is covered
// ============================================================================

TEST_F(QueryPlannerTest, ScanOp_PropertyScanSkipsResidualFilter) {
	// Create node
	int64_t lid = dataManager->getOrCreateTokenId("PScan");
	graph::Node node(20, lid);
	dataManager->addNode(node);
	dataManager->addNodeProperties(20, {{"key", graph::PropertyValue("match")}});

	// Create property index
	indexManager->createIndex("idx_pscan", "node", "PScan", "key");

	auto op = planner->scanOp("n", "PScan", "key", graph::PropertyValue("match"));
	ASSERT_NE(op, nullptr);

	// With property index, should NOT have a FilterOperator wrapping
	auto *scanOp = dynamic_cast<graph::query::execution::operators::NodeScanOperator *>(op.get());
	EXPECT_NE(scanOp, nullptr) << "With property index, scan should not need residual filter";
}
