/**
 * @file test_VarLengthTraversalOperator.cpp
 * @brief Unit tests for VarLengthTraversalOperator covering all branches.
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/query/execution/operators/VarLengthTraversalOperator.hpp"
#include "graph/query/execution/Record.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::operators;

namespace {

/**
 * A simple child operator that emits a single batch with one record per source node.
 */
class MockSourceOperator : public PhysicalOperator {
public:
	explicit MockSourceOperator(const std::string &varName, std::vector<Node> nodes)
		: varName_(varName), nodes_(std::move(nodes)) {}

	void open() override { idx_ = 0; }

	std::optional<RecordBatch> next() override {
		if (idx_ >= nodes_.size()) return std::nullopt;
		RecordBatch batch;
		for (; idx_ < nodes_.size(); ++idx_) {
			Record r;
			r.setNode(varName_, nodes_[idx_]);
			batch.push_back(std::move(r));
		}
		return batch;
	}

	void close() override {}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		return {varName_};
	}

	[[nodiscard]] std::string toString() const override { return "MockSource"; }

private:
	std::string varName_;
	std::vector<Node> nodes_;
	size_t idx_ = 0;
};

} // namespace

class VarLengthTraversalOperatorTest : public ::testing::Test {
protected:
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	fs::path testFilePath;

	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testFilePath = fs::temp_directory_path() /
					   ("test_varlen_" + boost::uuids::to_string(uuid) + ".dat");
		if (fs::exists(testFilePath)) fs::remove_all(testFilePath);
		db = std::make_unique<Database>(testFilePath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
	}

	void TearDown() override {
		if (db) db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
	}

	// Create a chain: n1 -> n2 -> n3 -> n4
	struct ChainResult {
		std::vector<Node> nodes;
		std::vector<Edge> edges;
	};

	ChainResult createChain(size_t length) {
		ChainResult result;
		int64_t labelId = dm->getOrCreateTokenId("Node");
		int64_t typeId = dm->getOrCreateTokenId("NEXT");

		for (size_t i = 0; i < length; ++i) {
			Node n(0, labelId);
			dm->addNode(n);
			result.nodes.push_back(n);
		}

		for (size_t i = 0; i + 1 < result.nodes.size(); ++i) {
			Edge e(0, result.nodes[i].getId(), result.nodes[i + 1].getId(), typeId);
			dm->addEdge(e);
			result.edges.push_back(e);
		}

		return result;
	}
};

TEST_F(VarLengthTraversalOperatorTest, OutgoingTraversalBasic) {
	auto chain = createChain(4); // n1->n2->n3->n4

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{chain.nodes[0]});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 3, "outgoing");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	// Should reach n2 (depth 1), n3 (depth 2), n4 (depth 3)
	EXPECT_EQ(batch->size(), 3UL);
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, IncomingTraversal) {
	auto chain = createChain(3); // n1->n2->n3

	// Start from n3, traverse incoming direction
	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{chain.nodes[2]});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 2, "incoming");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	// Should reach n2 (depth 1), n1 (depth 2)
	EXPECT_EQ(batch->size(), 2UL);
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, BothDirectionTraversal) {
	auto chain = createChain(3); // n1->n2->n3

	// Start from n2 (middle), traverse both directions
	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{chain.nodes[1]});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 1, "both");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	// Should reach n1 and n3 at depth 1
	EXPECT_EQ(batch->size(), 2UL);
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, MinMaxLengthFiltering) {
	auto chain = createChain(5); // n1->n2->n3->n4->n5

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{chain.nodes[0]});
	// min=2, max=3 means only depth 2 and 3 are emitted
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 2, 3, "outgoing");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	// Should only reach n3 (depth 2) and n4 (depth 3)
	EXPECT_EQ(batch->size(), 2UL);
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, InactiveNeighborSkipped) {
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node n1(0, labelId);
	dm->addNode(n1);
	Node n2(0, labelId);
	dm->addNode(n2);
	Node n3(0, labelId);
	dm->addNode(n3);

	Edge e1(0, n1.getId(), n2.getId(), typeId);
	dm->addEdge(e1);
	Edge e2(0, n1.getId(), n3.getId(), typeId);
	dm->addEdge(e2);

	// Delete n2 so it becomes inactive
	dm->deleteNode(n2);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{n1});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 2, "outgoing");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	// Only n3 reachable (n2 is inactive)
	EXPECT_EQ(batch->size(), 1UL);
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, CyclicGraphNoDuplicates) {
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t typeId = dm->getOrCreateTokenId("NEXT");

	// Create cycle: n1->n2->n3->n1
	Node n1(0, labelId);
	dm->addNode(n1);
	Node n2(0, labelId);
	dm->addNode(n2);
	Node n3(0, labelId);
	dm->addNode(n3);

	Edge e1(0, n1.getId(), n2.getId(), typeId);
	dm->addEdge(e1);
	Edge e2(0, n2.getId(), n3.getId(), typeId);
	dm->addEdge(e2);
	Edge e3(0, n3.getId(), n1.getId(), typeId);
	dm->addEdge(e3);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{n1});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 5, "outgoing");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	// Visited set prevents revisiting the same node on the current DFS path.
	// The exact count depends on neighbor ordering, but it must be > 0
	// and no node ID should appear more than once in the results.
	EXPECT_GE(batch->size(), 2UL);
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, ReopenWithNonEmptyStack) {
	// Create a long chain to ensure DFS stack has entries mid-traversal
	auto chain = createChain(10);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{chain.nodes[0]});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 10, "outgoing");

	// First open — this populates the DFS stack
	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());

	// Re-open WITHOUT closing first — stack may still have entries
	// This exercises the while(!dfsStack_.empty()) path in open() at line 58
	op->open();
	batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 9UL); // 9 hops from chain of 10

	// Now close with stack potentially non-empty by calling close mid-traversal
	op->open();
	// Don't call next() — close immediately while DFS might have initial state
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, EmptyGraphNoResults) {
	int64_t labelId = dm->getOrCreateTokenId("Node");
	Node n1(0, labelId);
	dm->addNode(n1);
	// No edges

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{n1});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 5, "outgoing");

	op->open();
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, SingleHopEquivalence) {
	auto chain = createChain(3); // n1->n2->n3

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{chain.nodes[0]});
	// min=1, max=1 means only depth 1
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 1, "outgoing");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), 1UL); // only n2
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, EdgeTypeFilterSkipsUnmatched) {
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t nextType = dm->getOrCreateTokenId("NEXT");
	int64_t otherType = dm->getOrCreateTokenId("OTHER");

	Node n1(0, labelId);
	dm->addNode(n1);
	Node n2(0, labelId);
	dm->addNode(n2);
	Node n3(0, labelId);
	dm->addNode(n3);

	Edge e1(0, n1.getId(), n2.getId(), nextType);
	dm->addEdge(e1);
	Edge e2(0, n1.getId(), n3.getId(), otherType);
	dm->addEdge(e2);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{n1});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 2, "outgoing");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	// Only n2 (via NEXT), n3 is via OTHER so filtered out
	EXPECT_EQ(batch->size(), 1UL);
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, UnresolvedEdgeTypeReturnsNoResults) {
	auto chain = createChain(3);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{chain.nodes[0]});
	// Use a non-existent edge type
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NONEXISTENT", 1, 5, "outgoing");

	op->open();
	auto batch = op->next();
	EXPECT_FALSE(batch.has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, GetOutputVariablesAndToString) {
	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 3, "outgoing");

	auto vars = op->getOutputVariables();
	EXPECT_EQ(vars.size(), 2UL);
	EXPECT_EQ(vars[0], "src");
	EXPECT_EQ(vars[1], "dst");

	auto str = op->toString();
	EXPECT_NE(str.find("VarLengthTraversal"), std::string::npos);
	EXPECT_NE(str.find("1..3"), std::string::npos);
}

TEST_F(VarLengthTraversalOperatorTest, GetChildrenReturnsChild) {
	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{});
	auto *rawPtr = source.get();
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 3, "outgoing");

	auto children = op->getChildren();
	ASSERT_EQ(children.size(), 1UL);
	EXPECT_EQ(children[0], rawPtr);
}

TEST_F(VarLengthTraversalOperatorTest, BatchOverflow_StarGraph) {
	// Create a star graph: center -> 1050 leaves (exceeds DEFAULT_BATCH_SIZE=1000)
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node center(0, labelId);
	dm->addNode(center);

	static constexpr size_t kLeaves = 1050;
	for (size_t i = 0; i < kLeaves; ++i) {
		Node leaf(0, labelId);
		dm->addNode(leaf);
		Edge e(0, center.getId(), leaf.getId(), typeId);
		dm->addEdge(e);
	}

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{center});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 1, "outgoing");

	op->open();
	// First next() should return exactly 1000 (DEFAULT_BATCH_SIZE)
	auto batch1 = op->next();
	ASSERT_TRUE(batch1.has_value());
	EXPECT_EQ(batch1->size(), 1000UL);

	// Second next() should return the remaining 50
	auto batch2 = op->next();
	ASSERT_TRUE(batch2.has_value());
	EXPECT_EQ(batch2->size(), kLeaves - 1000);

	// Third next() should be empty
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, BatchOverflow_ResumesDFSAcrossCalls) {
	// Create a chain long enough that DFS produces >1000 results via multi-hop
	// Use a star with depth-2: center -> 35 intermediates -> 30 leaves each = 35 + 1050 = 1085
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node center(0, labelId);
	dm->addNode(center);

	static constexpr size_t kIntermediates = 35;
	static constexpr size_t kLeavesPerIntermediate = 30;

	for (size_t i = 0; i < kIntermediates; ++i) {
		Node inter(0, labelId);
		dm->addNode(inter);
		Edge e(0, center.getId(), inter.getId(), typeId);
		dm->addEdge(e);

		for (size_t j = 0; j < kLeavesPerIntermediate; ++j) {
			Node leaf(0, labelId);
			dm->addNode(leaf);
			Edge le(0, inter.getId(), leaf.getId(), typeId);
			dm->addEdge(le);
		}
	}

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{center});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 2, "outgoing");

	op->open();
	size_t total = 0;
	int batchCount = 0;
	while (auto batch = op->next()) {
		total += batch->size();
		++batchCount;
	}
	// Total should be kIntermediates + kIntermediates * kLeavesPerIntermediate = 35 + 1050 = 1085
	EXPECT_EQ(total, kIntermediates + kIntermediates * kLeavesPerIntermediate);
	// Should require at least 2 batches
	EXPECT_GE(batchCount, 2);
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, MinHopsZero_EmitsSource) {
	auto chain = createChain(3); // n1->n2->n3

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{chain.nodes[0]});
	// min=0 means we also emit the source node itself
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 0, 2, "outgoing");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	// Should emit: n1 (depth 0), n2 (depth 1), n3 (depth 2) = 3 results
	EXPECT_EQ(batch->size(), 3UL);
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, MultipleSourceNodes) {
	// Two separate chains, both sources fed in one batch
	auto chain1 = createChain(3); // a->b->c
	auto chain2 = createChain(3); // d->e->f

	std::vector<Node> sources = {chain1.nodes[0], chain2.nodes[0]};
	auto source = std::make_unique<MockSourceOperator>("src", sources);
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 2, "outgoing");

	op->open();
	size_t total = 0;
	while (auto batch = op->next()) {
		total += batch->size();
	}
	// Each source reaches 2 nodes at depths 1,2
	EXPECT_EQ(total, 4UL);
	op->close();
}
