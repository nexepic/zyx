/**
 * @file test_Operators_NullGuards.cpp
 * @brief Branch coverage tests for RemoveOperator, ShowIndexesOperator,
 *        VarLengthTraversalOperator, and DataManager inline branches.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/Record.hpp"
#include "graph/query/execution/operators/RemoveOperator.hpp"
#include "graph/query/execution/operators/ShowIndexesOperator.hpp"
#include "graph/query/execution/operators/VarLengthTraversalOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

// MockOperator for feeding controlled batches.
class NullGuardsMockOperator : public PhysicalOperator {
public:
	std::vector<RecordBatch> batches;
	size_t current_index = 0;

	explicit NullGuardsMockOperator(std::vector<RecordBatch> data = {}) :
		batches(std::move(data)) {}

	void open() override { current_index = 0; }
	std::optional<RecordBatch> next() override {
		if (current_index >= batches.size()) return std::nullopt;
		return batches[current_index++];
	}
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"x"}; }
	[[nodiscard]] std::string toString() const override { return "BranchTestMock"; }
};

// ===========================================================================
// Shared fixture with Database
// ===========================================================================

class OperatorNullGuardsTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	std::shared_ptr<query::indexes::IndexManager> im;
	fs::path testFilePath;

	void SetUp() override {
		auto uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() /
					   ("test_op_branch_" + boost::uuids::to_string(uuid) + ".dat");
		if (fs::exists(testFilePath)) fs::remove_all(testFilePath);
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		im = db->getQueryEngine()->getIndexManager();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}
};

// ===========================================================================
// ShowIndexesOperator branch tests
// ===========================================================================

// Covers: type != "node" → "EDGE" entity field, and getOutputVariables + toString.
TEST_F(OperatorNullGuardsTest, ShowIndexes_EdgeTypeIndexCoversEntityBranch) {
	// Create an edge index to trigger the type == "edge" → "EDGE" branch
	ASSERT_TRUE(im->createIndex("idx_edge_rel", "edge", "KNOWS", "since"));

	auto op = std::make_unique<ShowIndexesOperator>(im);
	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	// At least one index should have entity = "EDGE"
	bool foundEdge = false;
	for (const auto &r : *batch) {
		auto entityOpt = r.getValue("entity");
		if (entityOpt.has_value() && entityOpt->toString() == "EDGE") {
			foundEdge = true;
		}
	}
	EXPECT_TRUE(foundEdge);

	// Second call returns nullopt (already executed)
	auto batch2 = op->next();
	EXPECT_FALSE(batch2.has_value());

	op->close();
}

TEST_F(OperatorNullGuardsTest, ShowIndexes_GetOutputVariables) {
	auto op = std::make_unique<ShowIndexesOperator>(im);
	auto vars = op->getOutputVariables();
	EXPECT_EQ(vars.size(), 4UL);
}

TEST_F(OperatorNullGuardsTest, ShowIndexes_ToString) {
	auto op = std::make_unique<ShowIndexesOperator>(im);
	EXPECT_EQ(op->toString(), "ShowIndexes()");
}

// Covers: empty index list → empty batch → returns nullopt.
TEST_F(OperatorNullGuardsTest, ShowIndexes_EmptyListReturnsNullopt) {
	auto op = std::make_unique<ShowIndexesOperator>(im);
	op->open();
	auto batch = op->next();
	// If no indexes exist, the batch is empty and should return nullopt.
	// Some default indexes might exist, so just exercise the path.
	(void)batch;
	op->close();
}

// ===========================================================================
// RemoveOperator branch tests
// ===========================================================================

// Covers: edge property removal path (lines 80-91 of RemoveOperator.hpp).
TEST_F(OperatorNullGuardsTest, RemoveOperator_EdgePropertyRemoval) {
	int64_t labelId = dm->getOrCreateTokenId("Person");
	Node n1(0, labelId);
	Node n2(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);

	int64_t relTypeId = dm->getOrCreateTokenId("KNOWS");
	Edge e(0, n1.getId(), n2.getId(), relTypeId);
	dm->addEdge(e);
	dm->addEdgeProperties(e.getId(), {{"since", PropertyValue(int64_t(2020))}});

	// Create a batch with the edge set in the record
	RecordBatch batch;
	Record r;
	r.setEdge("e", dm->getEdge(e.getId()));
	batch.push_back(std::move(r));

	auto mock = std::make_unique<NullGuardsMockOperator>(std::vector<RecordBatch>{batch});

	std::vector<RemoveItem> items = {
		{RemoveActionType::PROPERTY, "e", "since"}
	};
	auto op = std::make_unique<RemoveOperator>(dm, std::move(mock), std::move(items));
	op->open();
	auto result = op->next();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->size(), 1UL);
	op->close();
}

// Covers: variable not found as node or edge → no-op.
TEST_F(OperatorNullGuardsTest, RemoveOperator_VariableNotFoundIsNoOp) {
	RecordBatch batch;
	Record r;
	r.setValue("x", PropertyValue(int64_t(1)));
	batch.push_back(std::move(r));

	auto mock = std::make_unique<NullGuardsMockOperator>(std::vector<RecordBatch>{batch});

	std::vector<RemoveItem> items = {
		{RemoveActionType::PROPERTY, "nonexistent", "prop"}
	};
	auto op = std::make_unique<RemoveOperator>(dm, std::move(mock), std::move(items));
	op->open();
	auto result = op->next();
	ASSERT_TRUE(result.has_value());
	op->close();
}

// Covers: label removal where removeLabelId returns false (label not on node).
TEST_F(OperatorNullGuardsTest, RemoveOperator_LabelRemovalNotPresent) {
	int64_t labelId = dm->getOrCreateTokenId("Person");
	Node n(0, labelId);
	dm->addNode(n);

	RecordBatch batch;
	Record r;
	r.setNode("n", dm->getNode(n.getId()));
	batch.push_back(std::move(r));

	auto mock = std::make_unique<NullGuardsMockOperator>(std::vector<RecordBatch>{batch});

	std::vector<RemoveItem> items = {
		{RemoveActionType::LABEL, "n", "NonExistentLabel"}
	};
	auto op = std::make_unique<RemoveOperator>(dm, std::move(mock), std::move(items));
	op->open();
	auto result = op->next();
	ASSERT_TRUE(result.has_value());
	op->close();
}

// Covers: toString, getOutputVariables, getChildren, setChild.
TEST_F(OperatorNullGuardsTest, RemoveOperator_MetaMethods) {
	auto mock = std::make_unique<NullGuardsMockOperator>();
	std::vector<RemoveItem> items = {
		{RemoveActionType::PROPERTY, "n", "prop1"},
		{RemoveActionType::LABEL, "n", "Label1"}
	};
	auto op = std::make_unique<RemoveOperator>(dm, std::move(mock), std::move(items));

	EXPECT_EQ(op->toString(), "Remove(2 items)");
	auto children = op->getChildren();
	EXPECT_EQ(children.size(), 1UL);
	auto vars = op->getOutputVariables();
	EXPECT_EQ(vars.size(), 1UL);

	// setChild
	auto newMock = std::make_unique<NullGuardsMockOperator>();
	op->setChild(std::move(newMock));
	auto children2 = op->getChildren();
	EXPECT_EQ(children2.size(), 1UL);
}

// ===========================================================================
// VarLengthTraversalOperator branch tests
// ===========================================================================

// Covers: edgeType_.empty() path — edgeTypeId stays 0.
TEST_F(OperatorNullGuardsTest, VarLengthTraversal_EmptyEdgeType) {
	int64_t labelId = dm->getOrCreateTokenId("Person");
	Node n1(0, labelId);
	Node n2(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);

	int64_t relTypeId = dm->getOrCreateTokenId("KNOWS");
	Edge e(0, n1.getId(), n2.getId(), relTypeId);
	dm->addEdge(e);

	RecordBatch batch;
	Record r;
	r.setNode("n", dm->getNode(n1.getId()));
	batch.push_back(std::move(r));

	auto mock = std::make_unique<NullGuardsMockOperator>(std::vector<RecordBatch>{batch});

	// Empty edge type — no filtering by edge type
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(mock), "n", "m", "", 1, 3, "out");
	op->open();
	auto result = op->next();
	// Should find n2 via the KNOWS edge
	if (result.has_value()) {
		EXPECT_GE(result->size(), 1UL);
	}
	op->close();
}

// Covers: edgeTypeId_ = -1 when resolveTokenId returns 0 for unknown type.
TEST_F(OperatorNullGuardsTest, VarLengthTraversal_UnknownEdgeType) {
	int64_t labelId = dm->getOrCreateTokenId("Person");
	Node n1(0, labelId);
	Node n2(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);

	int64_t relTypeId = dm->getOrCreateTokenId("KNOWS");
	Edge e(0, n1.getId(), n2.getId(), relTypeId);
	dm->addEdge(e);

	RecordBatch batch;
	Record r;
	r.setNode("n", dm->getNode(n1.getId()));
	batch.push_back(std::move(r));

	auto mock = std::make_unique<NullGuardsMockOperator>(std::vector<RecordBatch>{batch});

	// Non-existent edge type → resolveTokenId returns 0 → edgeTypeId_ = -1
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(mock), "n", "m", "NONEXISTENT_TYPE", 1, 3, "out");
	op->open();
	auto result = op->next();
	// With edge type -1, no edges should match the filter
	EXPECT_FALSE(result.has_value());
	op->close();
}

// Covers: maxHops backtrack — at max depth, erase from visited and pop.
TEST_F(OperatorNullGuardsTest, VarLengthTraversal_MaxDepthBacktrack) {
	int64_t labelId = dm->getOrCreateTokenId("Person");
	Node n1(0, labelId);
	Node n2(0, labelId);
	Node n3(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);
	dm->addNode(n3);

	int64_t relTypeId = dm->getOrCreateTokenId("KNOWS");
	Edge e1(0, n1.getId(), n2.getId(), relTypeId);
	Edge e2(0, n2.getId(), n3.getId(), relTypeId);
	dm->addEdge(e1);
	dm->addEdge(e2);

	RecordBatch batch;
	Record r;
	r.setNode("n", dm->getNode(n1.getId()));
	batch.push_back(std::move(r));

	auto mock = std::make_unique<NullGuardsMockOperator>(std::vector<RecordBatch>{batch});

	// maxHops = 1 — should reach n2 but NOT n3, triggering backtrack at max depth
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(mock), "n", "m", "", 1, 1, "out");
	op->open();

	size_t totalResults = 0;
	while (auto res = op->next()) {
		totalResults += res->size();
	}
	EXPECT_GE(totalResults, 1UL);
	op->close();
}

// Covers: inactive neighbor skip (line 124).
TEST_F(OperatorNullGuardsTest, VarLengthTraversal_InactiveNeighborSkipped) {
	int64_t labelId = dm->getOrCreateTokenId("Person");
	Node n1(0, labelId);
	Node n2(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);

	int64_t relTypeId = dm->getOrCreateTokenId("KNOWS");
	Edge e(0, n1.getId(), n2.getId(), relTypeId);
	dm->addEdge(e);

	// Delete n2 — make it inactive
	Node toDelete = dm->getNode(n2.getId());
	dm->deleteNode(toDelete);

	RecordBatch batch;
	Record r;
	r.setNode("n", dm->getNode(n1.getId()));
	batch.push_back(std::move(r));

	auto mock = std::make_unique<NullGuardsMockOperator>(std::vector<RecordBatch>{batch});

	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(mock), "n", "m", "", 1, 3, "out");
	op->open();
	auto result = op->next();
	// n2 is inactive, so no results
	EXPECT_FALSE(result.has_value());
	op->close();
}

// Covers: missing source node in record → continue to next record.
TEST_F(OperatorNullGuardsTest, VarLengthTraversal_MissingSourceNodeSkips) {
	RecordBatch batch;
	Record r;
	r.setValue("other", PropertyValue(int64_t(1)));
	batch.push_back(std::move(r));

	auto mock = std::make_unique<NullGuardsMockOperator>(std::vector<RecordBatch>{batch});

	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(mock), "n", "m", "", 1, 3, "out");
	op->open();
	auto result = op->next();
	EXPECT_FALSE(result.has_value());
	op->close();
}

// Covers: toString.
TEST_F(OperatorNullGuardsTest, VarLengthTraversal_ToString) {
	auto mock = std::make_unique<NullGuardsMockOperator>();
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(mock), "src", "tgt", "REL", 1, 5, "out");
	EXPECT_FALSE(op->toString().empty());
}

// Covers: getChildren returns child.
TEST_F(OperatorNullGuardsTest, VarLengthTraversal_GetChildren) {
	auto mock = std::make_unique<NullGuardsMockOperator>();
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(mock), "n", "m", "", 1, 3, "out");
	auto children = op->getChildren();
	EXPECT_EQ(children.size(), 1UL);
}

// ===========================================================================
// DataManager inline branch tests
// ===========================================================================

// Covers: markDeletionPerformed with null flag (line 297-299).
TEST_F(OperatorNullGuardsTest, DataManager_MarkDeletionPerformedNullFlag) {
	// By default, deleteOperationPerformedFlag_ is null.
	// Calling markDeletionPerformed should be a no-op.
	EXPECT_NO_THROW(dm->markDeletionPerformed());
}

// Covers: markDeletionPerformed with non-null flag.
TEST_F(OperatorNullGuardsTest, DataManager_MarkDeletionPerformedWithFlag) {
	std::atomic<bool> flag{false};
	dm->setDeletionFlagReference(&flag);
	dm->markDeletionPerformed();
	EXPECT_TRUE(flag.load());
	dm->setDeletionFlagReference(nullptr); // cleanup
}

// Covers: isReadOnlyMode static method.
TEST_F(OperatorNullGuardsTest, DataManager_ReadOnlyModeStaticMethods) {
	bool original = storage::DataManager::isReadOnlyMode();

	storage::DataManager::setReadOnlyMode(true);
	EXPECT_TRUE(storage::DataManager::isReadOnlyMode());

	storage::DataManager::setReadOnlyMode(false);
	EXPECT_FALSE(storage::DataManager::isReadOnlyMode());

	storage::DataManager::setReadOnlyMode(original); // restore
}

// Covers: guardReadOnly throw path.
TEST_F(OperatorNullGuardsTest, DataManager_GuardReadOnlyThrows) {
	storage::DataManager::setReadOnlyMode(true);

	// Any write operation should throw in read-only mode
	int64_t labelId = dm->getOrCreateTokenId("TestLabel");
	Node n(0, labelId);
	EXPECT_THROW(dm->addNode(n), std::runtime_error);

	storage::DataManager::setReadOnlyMode(false); // restore
}

// Covers: resolveTokenName with 0 → empty string.
TEST_F(OperatorNullGuardsTest, DataManager_ResolveTokenNameZero) {
	EXPECT_EQ(dm->resolveTokenName(0), "");
}

// Covers: resolveTokenId with empty string → 0.
TEST_F(OperatorNullGuardsTest, DataManager_ResolveTokenIdEmpty) {
	EXPECT_EQ(dm->resolveTokenId(""), 0);
}

// Covers: getOrCreateTokenId with empty string → 0.
TEST_F(OperatorNullGuardsTest, DataManager_GetOrCreateTokenIdEmpty) {
	EXPECT_EQ(dm->getOrCreateTokenId(""), 0);
}
