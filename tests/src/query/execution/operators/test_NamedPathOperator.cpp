/**
 * @file test_NamedPathOperator.cpp
 * @brief Unit tests for NamedPathOperator physical operator.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

#include "graph/core/Database.hpp"
#include "graph/query/execution/operators/NamedPathOperator.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

// =============================================================================
// A trivial child operator that yields a fixed set of batches.
// =============================================================================
class StubOperator : public PhysicalOperator {
public:
	explicit StubOperator(std::vector<RecordBatch> batches) : batches_(std::move(batches)) {}

	void open() override { idx_ = 0; }

	std::optional<RecordBatch> next() override {
		if (idx_ >= batches_.size()) return std::nullopt;
		return batches_[idx_++];
	}

	void close() override {}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		return childVars_;
	}

	[[nodiscard]] std::string toString() const override { return "Stub"; }

	void setChildVars(std::vector<std::string> vars) { childVars_ = std::move(vars); }

private:
	std::vector<RecordBatch> batches_;
	size_t idx_ = 0;
	std::vector<std::string> childVars_;
};

// =============================================================================
// Null-child tests (no DataManager needed)
// =============================================================================

TEST(NamedPathOperatorTest, NullChild_OpenCloseNoThrow) {
	NamedPathOperator op(nullptr, nullptr, "p", {"a"}, {"r"});
	EXPECT_NO_THROW(op.open());
	EXPECT_NO_THROW(op.close());
}

TEST(NamedPathOperatorTest, NullChild_NextReturnsNullopt) {
	NamedPathOperator op(nullptr, nullptr, "p", {"a"}, {"r"});
	op.open();
	auto batch = op.next();
	EXPECT_FALSE(batch.has_value());
}

TEST(NamedPathOperatorTest, ToString) {
	NamedPathOperator op(nullptr, nullptr, "myPath", {"a"}, {"r"});
	EXPECT_EQ(op.toString(), "NamedPath(myPath)");
}

TEST(NamedPathOperatorTest, GetOutputVariables_NullChild) {
	NamedPathOperator op(nullptr, nullptr, "p", {"a"}, {"r"});
	auto vars = op.getOutputVariables();
	ASSERT_EQ(vars.size(), 1u);
	EXPECT_EQ(vars[0], "p");
}

TEST(NamedPathOperatorTest, GetChildren_NullChild) {
	NamedPathOperator op(nullptr, nullptr, "p", {"a"}, {"r"});
	auto children = op.getChildren();
	EXPECT_TRUE(children.empty());
}

TEST(NamedPathOperatorTest, GetOutputVariables_WithChild) {
	auto stub = std::make_unique<StubOperator>(std::vector<RecordBatch>{});
	stub->setChildVars({"a", "r", "b"});
	NamedPathOperator op(std::move(stub), nullptr, "p", {"a", "b"}, {"r"});
	auto vars = op.getOutputVariables();
	ASSERT_EQ(vars.size(), 4u);
	EXPECT_EQ(vars.back(), "p");
}

TEST(NamedPathOperatorTest, GetChildren_WithChild) {
	auto stub = std::make_unique<StubOperator>(std::vector<RecordBatch>{});
	auto* ptr = stub.get();
	NamedPathOperator op(std::move(stub), nullptr, "p", {"a"}, {"r"});
	auto children = op.getChildren();
	ASSERT_EQ(children.size(), 1u);
	EXPECT_EQ(children[0], ptr);
}

// =============================================================================
// Tests with records but no DataManager (dm == nullptr)
// =============================================================================

TEST(NamedPathOperatorTest, EmptyBatch_PassesThrough) {
	RecordBatch emptyBatch;
	auto stub = std::make_unique<StubOperator>(std::vector<RecordBatch>{emptyBatch});
	NamedPathOperator op(std::move(stub), nullptr, "p", {"a"}, {"r"});
	op.open();
	auto batch = op.next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_TRUE(batch->empty());
	EXPECT_FALSE(op.next().has_value());
}

TEST(NamedPathOperatorTest, RecordWithNode_BuildsPathList_NoDM) {
	Record rec;
	Node n(100, 1);
	rec.setNode("a", n);

	RecordBatch batch = {rec};
	auto stub = std::make_unique<StubOperator>(std::vector<RecordBatch>{batch});
	NamedPathOperator op(std::move(stub), nullptr, "p", {"a"}, {});
	op.open();

	auto result = op.next();
	ASSERT_TRUE(result.has_value());
	ASSERT_EQ(result->size(), 1u);

	auto pathVal = (*result)[0].getValue("p");
	ASSERT_TRUE(pathVal.has_value());
	EXPECT_EQ(pathVal->getType(), PropertyType::LIST);
	auto& pathList = std::get<std::vector<PropertyValue>>(pathVal->getVariant());
	ASSERT_EQ(pathList.size(), 1u); // one node, no edges
	// Node map should have _type and _id
	auto& nodeMap = std::get<std::unordered_map<std::string, PropertyValue>>(pathList[0].getVariant());
	EXPECT_EQ(std::get<std::string>(nodeMap.at("_type").getVariant()), "node");
}

TEST(NamedPathOperatorTest, RecordWithNodeAndEdge_BuildsAlternatingPath) {
	Record rec;
	Node n1(100, 1);
	Node n2(200, 1);
	Edge e(300, 100, 200, 1);
	rec.setNode("a", n1);
	rec.setNode("b", n2);
	rec.setEdge("r", e);

	RecordBatch batch = {rec};
	auto stub = std::make_unique<StubOperator>(std::vector<RecordBatch>{batch});
	NamedPathOperator op(std::move(stub), nullptr, "p", {"a", "b"}, {"r"});
	op.open();

	auto result = op.next();
	ASSERT_TRUE(result.has_value());
	auto pathVal = (*result)[0].getValue("p");
	ASSERT_TRUE(pathVal.has_value());

	auto& pathList = std::get<std::vector<PropertyValue>>(pathVal->getVariant());
	// Should be: node a, edge r, node b
	ASSERT_EQ(pathList.size(), 3u);

	auto& nodeAMap = std::get<std::unordered_map<std::string, PropertyValue>>(pathList[0].getVariant());
	EXPECT_EQ(std::get<std::string>(nodeAMap.at("_type").getVariant()), "node");

	auto& edgeMap = std::get<std::unordered_map<std::string, PropertyValue>>(pathList[1].getVariant());
	EXPECT_EQ(std::get<std::string>(edgeMap.at("_type").getVariant()), "relationship");

	auto& nodeBMap = std::get<std::unordered_map<std::string, PropertyValue>>(pathList[2].getVariant());
	EXPECT_EQ(std::get<std::string>(nodeBMap.at("_type").getVariant()), "node");
}

TEST(NamedPathOperatorTest, MissingNodeInRecord_SkipsInPath) {
	Record rec;
	// nodeVariables = {"a", "b"} but only "a" is in record
	Node n1(100, 1);
	rec.setNode("a", n1);

	RecordBatch batch = {rec};
	auto stub = std::make_unique<StubOperator>(std::vector<RecordBatch>{batch});
	NamedPathOperator op(std::move(stub), nullptr, "p", {"a", "b"}, {"r"});
	op.open();

	auto result = op.next();
	ASSERT_TRUE(result.has_value());
	auto pathVal = (*result)[0].getValue("p");
	ASSERT_TRUE(pathVal.has_value());
	// Only node a should be in path (b and edge r are missing)
	auto& pathList = std::get<std::vector<PropertyValue>>(pathVal->getVariant());
	EXPECT_EQ(pathList.size(), 1u);
}

TEST(NamedPathOperatorTest, MissingEdgeInRecord_SkipsEdge) {
	Record rec;
	Node n1(100, 1);
	Node n2(200, 1);
	rec.setNode("a", n1);
	rec.setNode("b", n2);
	// edgeVariables = {"r"} but edge "r" is not in record

	RecordBatch batch = {rec};
	auto stub = std::make_unique<StubOperator>(std::vector<RecordBatch>{batch});
	NamedPathOperator op(std::move(stub), nullptr, "p", {"a", "b"}, {"r"});
	op.open();

	auto result = op.next();
	auto pathVal = (*result)[0].getValue("p");
	auto& pathList = std::get<std::vector<PropertyValue>>(pathVal->getVariant());
	// Should have 2 nodes, no edge (edge was missing)
	EXPECT_EQ(pathList.size(), 2u);
}

TEST(NamedPathOperatorTest, MultipleBatches) {
	Record r1;
	Node n1(1, 1);
	r1.setNode("a", n1);

	Record r2;
	Node n2(2, 1);
	r2.setNode("a", n2);

	auto stub = std::make_unique<StubOperator>(
		std::vector<RecordBatch>{{r1}, {r2}});
	NamedPathOperator op(std::move(stub), nullptr, "p", {"a"}, {});
	op.open();

	auto batch1 = op.next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1u);

	auto batch2 = op.next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), 1u);

	EXPECT_FALSE(op.next().has_value());
}

// =============================================================================
// Tests with real DataManager (fetches properties)
// =============================================================================

class NamedPathOperatorDMTest : public ::testing::Test {
protected:
	fs::path dbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() / ("test_namedpath_" + boost::uuids::to_string(uuid) + ".db");
		db = std::make_unique<Database>(dbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(dbPath)) fs::remove(dbPath, ec);
	}
};

TEST_F(NamedPathOperatorDMTest, FetchesNodeProperties) {
	auto labelId = dm->getOrCreateTokenId("Person");
	Node n(0, labelId);
	dm->addNode(n);
	auto nodeId = n.getId();
	dm->addNodeProperties(nodeId, {{"name", PropertyValue(std::string("Alice"))}});

	Record rec;
	rec.setNode("a", n);

	RecordBatch batch = {rec};
	auto stub = std::make_unique<StubOperator>(std::vector<RecordBatch>{batch});
	NamedPathOperator op(std::move(stub), dm, "p", {"a"}, {});
	op.open();

	auto result = op.next();
	ASSERT_TRUE(result.has_value());
	auto pathVal = (*result)[0].getValue("p");
	auto& pathList = std::get<std::vector<PropertyValue>>(pathVal->getVariant());
	ASSERT_EQ(pathList.size(), 1u);

	auto& nodeMap = std::get<std::unordered_map<std::string, PropertyValue>>(pathList[0].getVariant());
	EXPECT_EQ(std::get<std::string>(nodeMap.at("_type").getVariant()), "node");
	EXPECT_EQ(std::get<int64_t>(nodeMap.at("_id").getVariant()), nodeId);
	EXPECT_EQ(std::get<std::string>(nodeMap.at("name").getVariant()), "Alice");
}

TEST_F(NamedPathOperatorDMTest, FetchesEdgeProperties) {
	auto labelId = dm->getOrCreateTokenId("Person");
	auto typeId = dm->getOrCreateTokenId("KNOWS");

	Node n1(0, labelId);
	dm->addNode(n1);
	Node n2(0, labelId);
	dm->addNode(n2);

	Edge e(0, n1.getId(), n2.getId(), typeId);
	dm->addEdge(e);
	dm->addEdgeProperties(e.getId(), {{"since", PropertyValue(int64_t(2020))}});

	Record rec;
	rec.setNode("a", n1);
	rec.setNode("b", n2);
	rec.setEdge("r", e);

	RecordBatch batch = {rec};
	auto stub = std::make_unique<StubOperator>(std::vector<RecordBatch>{batch});
	NamedPathOperator op(std::move(stub), dm, "p", {"a", "b"}, {"r"});
	op.open();

	auto result = op.next();
	auto pathVal = (*result)[0].getValue("p");
	auto& pathList = std::get<std::vector<PropertyValue>>(pathVal->getVariant());
	ASSERT_EQ(pathList.size(), 3u);

	// Check edge properties
	auto& edgeMap = std::get<std::unordered_map<std::string, PropertyValue>>(pathList[1].getVariant());
	EXPECT_EQ(std::get<std::string>(edgeMap.at("_type").getVariant()), "relationship");
	EXPECT_EQ(std::get<int64_t>(edgeMap.at("_id").getVariant()), e.getId());
	EXPECT_EQ(std::get<int64_t>(edgeMap.at("since").getVariant()), 2020);
}

TEST_F(NamedPathOperatorDMTest, LongPath_ThreeNodesAndTwoEdges) {
	auto labelId = dm->getOrCreateTokenId("City");
	auto typeId = dm->getOrCreateTokenId("ROAD");

	Node n1(0, labelId), n2(0, labelId), n3(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);
	dm->addNode(n3);

	Edge e1(0, n1.getId(), n2.getId(), typeId);
	dm->addEdge(e1);
	Edge e2(0, n2.getId(), n3.getId(), typeId);
	dm->addEdge(e2);

	Record rec;
	rec.setNode("a", n1);
	rec.setNode("b", n2);
	rec.setNode("c", n3);
	rec.setEdge("r1", e1);
	rec.setEdge("r2", e2);

	RecordBatch batch = {rec};
	auto stub = std::make_unique<StubOperator>(std::vector<RecordBatch>{batch});
	NamedPathOperator op(std::move(stub), dm, "p", {"a", "b", "c"}, {"r1", "r2"});
	op.open();

	auto result = op.next();
	auto pathVal = (*result)[0].getValue("p");
	auto& pathList = std::get<std::vector<PropertyValue>>(pathVal->getVariant());
	// 3 nodes + 2 edges = 5 elements
	EXPECT_EQ(pathList.size(), 5u);
}

TEST_F(NamedPathOperatorDMTest, MultipleRecordsInBatch) {
	auto labelId = dm->getOrCreateTokenId("X");

	Node n1(0, labelId), n2(0, labelId);
	dm->addNode(n1);
	dm->addNode(n2);

	Record r1, r2;
	r1.setNode("a", n1);
	r2.setNode("a", n2);

	RecordBatch batch = {r1, r2};
	auto stub = std::make_unique<StubOperator>(std::vector<RecordBatch>{batch});
	NamedPathOperator op(std::move(stub), dm, "p", {"a"}, {});
	op.open();

	auto result = op.next();
	ASSERT_TRUE(result.has_value());
	ASSERT_EQ(result->size(), 2u);

	// Both records should have path variable "p"
	EXPECT_TRUE((*result)[0].getValue("p").has_value());
	EXPECT_TRUE((*result)[1].getValue("p").has_value());
}
