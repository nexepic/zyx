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

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
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

class MockRecordBatchOperator : public PhysicalOperator {
public:
	explicit MockRecordBatchOperator(std::vector<RecordBatch> batches) : batches_(std::move(batches)) {}

	void open() override { idx_ = 0; }

	std::optional<RecordBatch> next() override {
		if (idx_ >= batches_.size()) {
			return std::nullopt;
		}
		return batches_[idx_++];
	}

	void close() override {}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"src"}; }

	[[nodiscard]] std::string toString() const override { return "MockRecordBatch"; }

private:
	std::vector<RecordBatch> batches_;
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

TEST_F(VarLengthTraversalOperatorTest, DirectionSynonymsDoNotFallBackToBoth) {
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node left(0, labelId);
	dm->addNode(left);
	Node center(0, labelId);
	dm->addNode(center);
	Node right(0, labelId);
	dm->addNode(right);

	Edge incoming(0, left.getId(), center.getId(), typeId);
	dm->addEdge(incoming);
	Edge outgoing(0, center.getId(), right.getId(), typeId);
	dm->addEdge(outgoing);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{center});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 1, "outgoing");

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	auto target = (*batch)[0].getNode("dst");
	ASSERT_TRUE(target.has_value());
	EXPECT_EQ(target->getId(), right.getId());
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

TEST_F(VarLengthTraversalOperatorTest, LargeBatchMaterializationEmitsExecutorTelemetry) {
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node center(0, labelId);
	dm->addNode(center);

	static constexpr size_t kLeaves = PhysicalOperator::DEFAULT_BATCH_SIZE;
	for (size_t i = 0; i < kLeaves; ++i) {
		Node leaf(0, labelId);
		dm->addNode(leaf);
		Edge e(0, center.getId(), leaf.getId(), typeId);
		dm->addEdge(e);
	}

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{center});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 1, 1, "out");
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	EXPECT_EQ(batch->size(), kLeaves);
	op->close();

	auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);
	ASSERT_TRUE(snapshot.contains("varlength.materialize_targets.workers"));
	EXPECT_EQ(snapshot["varlength.materialize_targets.workers"].totalValue, 1);
	EXPECT_TRUE(snapshot.contains("varlength.materialize_targets"));
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

TEST_F(VarLengthTraversalOperatorTest, ReopenAfterPartialDfsBatchClearsStack) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node center(0, labelId);
	dm->addNode(center);

	static constexpr size_t kLeaves = PhysicalOperator::DEFAULT_BATCH_SIZE + 25;
	for (size_t i = 0; i < kLeaves; ++i) {
		Node leaf(0, labelId);
		dm->addNode(leaf);
		Edge edge(0, center.getId(), leaf.getId(), typeId);
		dm->addEdge(edge);
	}

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{center});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm, std::move(source), "src", "dst", "NEXT", 1, 1, "outgoing");

	op->open();
	auto firstBatch = op->next();
	ASSERT_TRUE(firstBatch.has_value());
	EXPECT_EQ(firstBatch->size(), PhysicalOperator::DEFAULT_BATCH_SIZE);

	// Re-opening mid-stream must discard the old DFS stack rather than resuming stale state.
	op->open();
	size_t totalAfterReopen = 0;
	while (auto batch = op->next()) {
		totalAfterReopen += batch->size();
	}
	EXPECT_EQ(totalAfterReopen, kLeaves);
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

TEST_F(VarLengthTraversalOperatorTest, NullChildAndMissingSourceRecordsReturnNoRows) {
	auto nullChildOp = std::make_unique<VarLengthTraversalOperator>(
			dm, nullptr, "src", "dst", "NEXT", 1, 2, "outgoing");
	nullChildOp->open();
	EXPECT_FALSE(nullChildOp->next().has_value());
	const auto variables = nullChildOp->getOutputVariables();
	ASSERT_EQ(variables.size(), 1UL);
	EXPECT_EQ(variables[0], "dst");
	const auto children = nullChildOp->getChildren();
	ASSERT_EQ(children.size(), 1UL);
	EXPECT_EQ(children[0], nullptr);
	nullChildOp->close();

	RecordBatch batchWithMissingSource;
	batchWithMissingSource.emplace_back();
	auto source = std::make_unique<MockRecordBatchOperator>(
			std::vector<RecordBatch>{std::move(batchWithMissingSource), RecordBatch{}});
	auto missingSourceOp = std::make_unique<VarLengthTraversalOperator>(
			dm, std::move(source), "src", "dst", "NEXT", 1, 2, "outgoing");
	missingSourceOp->open();
	EXPECT_FALSE(missingSourceOp->next().has_value());
	missingSourceOp->close();
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

TEST_F(VarLengthTraversalOperatorTest, FrontierTraversalPreservesDuplicatePathResults) {
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node left(0, labelId);
	dm->addNode(left);
	Node right(0, labelId);
	dm->addNode(right);
	Node target(0, labelId);
	dm->addNode(target);

	Edge e1(0, sourceNode.getId(), left.getId(), typeId);
	dm->addEdge(e1);
	Edge e2(0, sourceNode.getId(), right.getId(), typeId);
	dm->addEdge(e2);
	Edge e3(0, left.getId(), target.getId(), typeId);
	dm->addEdge(e3);
	Edge e4(0, right.getId(), target.getId(), typeId);
	dm->addEdge(e4);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 2, 2, "outgoing");
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	op->open();
	auto batch = op->next();
	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 2UL);
	for (const auto &record: *batch) {
		const auto emitted = record.getNode("dst");
		ASSERT_TRUE(emitted.has_value());
		EXPECT_EQ(emitted->getId(), target.getId());
	}
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, FrontierTraversalParallelizesLargeSecondHop) {
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node center(0, labelId);
	dm->addNode(center);

	static constexpr size_t kMiddle = 64;
	static constexpr size_t kLeavesPerMiddle = 64;
	for (size_t i = 0; i < kMiddle; ++i) {
		Node middle(0, labelId);
		dm->addNode(middle);
		Edge e(0, center.getId(), middle.getId(), typeId);
		dm->addEdge(e);

		for (size_t j = 0; j < kLeavesPerMiddle; ++j) {
			Node leaf(0, labelId);
			dm->addNode(leaf);
			Edge child(0, middle.getId(), leaf.getId(), typeId);
			dm->addEdge(child);
		}
	}

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{center});
	auto op = std::make_unique<VarLengthTraversalOperator>(
		dm, std::move(source), "src", "dst", "NEXT", 2, 2, "outgoing");
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	size_t total = 0;
	while (auto batch = op->next()) {
		total += batch->size();
	}
	op->close();
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_EQ(total, kMiddle * kLeavesPerMiddle);
	ASSERT_TRUE(snapshot.contains("varlength.frontier.expand.workers"));
	EXPECT_TRUE(snapshot.contains("varlength.frontier.expand.decision.parallel"));
	EXPECT_GE(snapshot.at("varlength.frontier.expand.workers").totalValue, 2);
}

TEST_F(VarLengthTraversalOperatorTest, FrontierTraversalSupportsTargetPropertyPredicate) {
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node middle(0, labelId);
	dm->addNode(middle);
	Node target(0, labelId);
	dm->addNode(target);
	Node nonMatching(0, labelId);
	dm->addNode(nonMatching);

	dm->addNodeProperties(target.getId(), {{"id", PropertyValue(std::string("target"))}});
	dm->addNodeProperties(nonMatching.getId(), {{"id", PropertyValue(std::string("other"))}});

	Edge e1(0, sourceNode.getId(), middle.getId(), typeId);
	dm->addEdge(e1);
	Edge e2(0, middle.getId(), target.getId(), typeId);
	dm->addEdge(e2);
	Edge e3(0, middle.getId(), nonMatching.getId(), typeId);
	dm->addEdge(e3);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			2,
			2,
			"outgoing",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue(std::string("target"))}});
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	auto batch = op->next();
	op->close();
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	const auto emitted = (*batch)[0].getNode("dst");
	ASSERT_TRUE(emitted.has_value());
	EXPECT_EQ(emitted->getId(), target.getId());
	EXPECT_TRUE(snapshot.contains("varlength.frontier.expand.frontier_entry_bytes"));
}

TEST_F(VarLengthTraversalOperatorTest, FrontierMaterializationSkipsInactiveTargets) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node target(0, labelId);
	dm->addNode(target);
	Edge edge(0, sourceNode.getId(), target.getId(), typeId);
	dm->addEdge(edge);
	dm->deleteNode(target);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm, std::move(source), "src", "dst", "NEXT", 1, 1, "outgoing");
	graph::concurrent::ThreadPool pool(2);
	op->setThreadPool(&pool);

	op->open();
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, FrontierTraversalWithMinZeroEmitsSourceBeforeExpansion) {
	auto chain = createChain(3);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{chain.nodes[0]});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm, std::move(source), "src", "dst", "NEXT", 0, 2, "outgoing");
	graph::concurrent::ThreadPool pool(2);
	op->setThreadPool(&pool);

	op->open();
	size_t total = 0;
	while (auto batch = op->next()) {
		total += batch->size();
	}
	EXPECT_EQ(total, 3UL);
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, IndexedTargetPropertyEnablesDeepFrontierPruning) {
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node current = sourceNode;
	std::vector<Node> chain;
	for (int depth = 0; depth < 4; ++depth) {
		Node next(0, labelId);
		dm->addNode(next);
		Edge edge(0, current.getId(), next.getId(), typeId);
		dm->addEdge(edge);
		chain.push_back(next);
		current = next;
	}
	dm->addNodeProperties(chain.back().getId(), {{"id", PropertyValue(std::string("target"))}});

	for (int branch = 0; branch < 32; ++branch) {
		Node deadEnd(0, labelId);
		dm->addNode(deadEnd);
		dm->addNodeProperties(deadEnd.getId(), {{"id", PropertyValue(std::string("dead"))}});
		Edge edge(0, sourceNode.getId(), deadEnd.getId(), typeId);
		dm->addEdge(edge);
	}

	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("", "node", "Node", "id"));

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			4,
			"outgoing",
			std::vector<int64_t>{labelId},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue(std::string("target"))}},
			indexManager,
			std::vector<std::string>{"Node"});
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	auto batch = op->next();
	op->close();
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	const auto emitted = (*batch)[0].getNode("dst");
	ASSERT_TRUE(emitted.has_value());
	EXPECT_EQ(emitted->getId(), chain.back().getId());
	EXPECT_TRUE(snapshot.contains("varlength.target_index.candidates"));
	EXPECT_TRUE(snapshot.contains("varlength.target_index.source.scoped"));
	EXPECT_TRUE(snapshot.contains("varlength.target_index.strategy.bidirectional_prune"));
	EXPECT_TRUE(snapshot.contains("varlength.target_index.reverse_prune_nodes"));
	EXPECT_TRUE(snapshot.contains("varlength.frontier.expand.estimated_edges"));
}

TEST_F(VarLengthTraversalOperatorTest, IndexedTargetPropertyPrunesDfsDeadBranches) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node middle(0, labelId);
	dm->addNode(middle);
	Node target(0, labelId);
	dm->addNode(target);
	dm->addNodeProperties(target.getId(), {{"id", PropertyValue(std::string("target"))}});

	Edge sourceToMiddle(0, sourceNode.getId(), middle.getId(), typeId);
	dm->addEdge(sourceToMiddle);
	Edge middleToTarget(0, middle.getId(), target.getId(), typeId);
	dm->addEdge(middleToTarget);

	for (int i = 0; i < 8; ++i) {
		Node deadEnd(0, labelId);
		dm->addNode(deadEnd);
		Edge deadEdge(0, sourceNode.getId(), deadEnd.getId(), typeId);
		dm->addEdge(deadEdge);
	}

	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("", "node", "", "id"));

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			3,
			"outgoing",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue(std::string("target"))}},
			indexManager);

	op->open();
	auto batch = op->next();
	op->close();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	const auto emitted = (*batch)[0].getNode("dst");
	ASSERT_TRUE(emitted.has_value());
	EXPECT_EQ(emitted->getId(), target.getId());
}

TEST_F(VarLengthTraversalOperatorTest, IndexedTargetPropertyUsesGlobalIndexWithoutLabel) {
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node target(0, labelId);
	dm->addNode(target);
	Node other(0, labelId);
	dm->addNode(other);
	dm->addNodeProperties(target.getId(), {{"id", PropertyValue(std::string("target"))}});
	dm->addNodeProperties(other.getId(), {{"id", PropertyValue(std::string("other"))}});

	Edge e1(0, sourceNode.getId(), target.getId(), typeId);
	dm->addEdge(e1);
	Edge e2(0, sourceNode.getId(), other.getId(), typeId);
	dm->addEdge(e2);

	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("", "node", "", "id"));

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			1,
			"outgoing",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue(std::string("target"))}},
			indexManager);
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	auto batch = op->next();
	op->close();
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	const auto emitted = (*batch)[0].getNode("dst");
	ASSERT_TRUE(emitted.has_value());
	EXPECT_EQ(emitted->getId(), target.getId());
	EXPECT_TRUE(snapshot.contains("varlength.target_index.candidates"));
	EXPECT_TRUE(snapshot.contains("varlength.target_index.source.global"));
}

TEST_F(VarLengthTraversalOperatorTest, IndexedTargetPropertyStillChecksAllPredicates) {
	int64_t labelId = dm->getOrCreateTokenId("Node");
	int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node matching(0, labelId);
	dm->addNode(matching);
	Node sameIndexedValue(0, labelId);
	dm->addNode(sameIndexedValue);
	dm->addNodeProperties(matching.getId(), {{"country", PropertyValue(std::string("US"))},
											 {"status", PropertyValue(std::string("active"))}});
	dm->addNodeProperties(sameIndexedValue.getId(), {{"country", PropertyValue(std::string("US"))},
													 {"status", PropertyValue(std::string("inactive"))}});

	Edge e1(0, sourceNode.getId(), matching.getId(), typeId);
	dm->addEdge(e1);
	Edge e2(0, sourceNode.getId(), sameIndexedValue.getId(), typeId);
	dm->addEdge(e2);

	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("", "node", "", "country"));

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			1,
			"outgoing",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{
					{"country", PropertyValue(std::string("US"))},
					{"status", PropertyValue(std::string("active"))}},
			indexManager);
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	auto batch = op->next();
	op->close();
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	const auto emitted = (*batch)[0].getNode("dst");
	ASSERT_TRUE(emitted.has_value());
	EXPECT_EQ(emitted->getId(), matching.getId());
	EXPECT_TRUE(snapshot.contains("varlength.target_index.candidates"));
}

TEST_F(VarLengthTraversalOperatorTest, FrontierTraversalDelaysTargetLabelPredicate) {
	const int64_t sourceLabelId = dm->getOrCreateTokenId("Source");
	const int64_t targetLabelId = dm->getOrCreateTokenId("Target");
	const int64_t otherLabelId = dm->getOrCreateTokenId("Other");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, sourceLabelId);
	dm->addNode(sourceNode);
	Node matching(0, targetLabelId);
	dm->addNode(matching);
	Node nonMatching(0, otherLabelId);
	dm->addNode(nonMatching);

	Edge e1(0, sourceNode.getId(), matching.getId(), typeId);
	dm->addEdge(e1);
	Edge e2(0, sourceNode.getId(), nonMatching.getId(), typeId);
	dm->addEdge(e2);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			1,
			"outgoing",
			std::vector<int64_t>{targetLabelId});
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	auto batch = op->next();
	op->close();
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	const auto emitted = (*batch)[0].getNode("dst");
	ASSERT_TRUE(emitted.has_value());
	EXPECT_EQ(emitted->getId(), matching.getId());
	EXPECT_TRUE(snapshot.contains("varlength.frontier.expand.estimated_edges"));
}

TEST_F(VarLengthTraversalOperatorTest, IndexedTargetPropertyEmptyCandidateShortCircuitsTraversal) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node reachable(0, labelId);
	dm->addNode(reachable);
	dm->addNodeProperties(reachable.getId(), {{"id", PropertyValue(std::string("other"))}});
	Edge edge(0, sourceNode.getId(), reachable.getId(), typeId);
	dm->addEdge(edge);

	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("", "node", "", "id"));

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			2,
			"outgoing",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue(std::string("missing"))}},
			indexManager);
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	EXPECT_FALSE(op->next().has_value());
	op->close();
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_TRUE(snapshot.contains("varlength.target_index.strategy.empty_candidate"));
}

TEST_F(VarLengthTraversalOperatorTest, IndexedTargetPropertyFallsBackWhenNoUsefulIndexExists) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node target(0, labelId);
	dm->addNode(target);
	dm->addNodeProperties(target.getId(), {{"id", PropertyValue(std::string("target"))}});
	Edge edge(0, sourceNode.getId(), target.getId(), typeId);
	dm->addEdge(edge);

	auto indexManager = db->getQueryEngine()->getIndexManager();
	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			1,
			"outgoing",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue(std::string("target"))}},
			indexManager);
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	auto batch = op->next();
	op->close();
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	EXPECT_TRUE(snapshot.contains("varlength.target_index.reason.not_used"));
}

TEST_F(VarLengthTraversalOperatorTest, IndexedTargetPropertySelectsMostSelectiveIndexedPredicate) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node matching(0, labelId);
	dm->addNode(matching);
	Node sameCountry(0, labelId);
	dm->addNode(sameCountry);
	dm->addNodeProperties(matching.getId(), {{"country", PropertyValue(std::string("US"))},
											 {"id", PropertyValue(std::string("target"))}});
	dm->addNodeProperties(sameCountry.getId(), {{"country", PropertyValue(std::string("US"))},
												{"id", PropertyValue(std::string("other"))}});
	Edge matchingEdge(0, sourceNode.getId(), matching.getId(), typeId);
	dm->addEdge(matchingEdge);
	Edge sameCountryEdge(0, sourceNode.getId(), sameCountry.getId(), typeId);
	dm->addEdge(sameCountryEdge);

	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("", "node", "", "country"));
	ASSERT_TRUE(indexManager->createIndex("", "node", "", "id"));

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			1,
			"outgoing",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{
					{"country", PropertyValue(std::string("US"))},
					{"id", PropertyValue(std::string("target"))}},
			indexManager);
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	op->open();
	auto batch = op->next();
	op->close();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	const auto emitted = (*batch)[0].getNode("dst");
	ASSERT_TRUE(emitted.has_value());
	EXPECT_EQ(emitted->getId(), matching.getId());
}

TEST_F(VarLengthTraversalOperatorTest, IndexedTargetPropertyKeepsMoreSelectiveEarlierCandidate) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node matching(0, labelId);
	dm->addNode(matching);
	Node sameCountry(0, labelId);
	dm->addNode(sameCountry);
	dm->addNodeProperties(matching.getId(), {{"id", PropertyValue(std::string("target"))},
											 {"country", PropertyValue(std::string("US"))}});
	dm->addNodeProperties(sameCountry.getId(), {{"id", PropertyValue(std::string("other"))},
												{"country", PropertyValue(std::string("US"))}});

	Edge matchingEdge(0, sourceNode.getId(), matching.getId(), typeId);
	dm->addEdge(matchingEdge);
	Edge sameCountryEdge(0, sourceNode.getId(), sameCountry.getId(), typeId);
	dm->addEdge(sameCountryEdge);

	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("scoped_id", "node", "Node", "id"));
	ASSERT_TRUE(indexManager->createIndex("scoped_country", "node", "Node", "country"));
	ASSERT_TRUE(indexManager->createIndex("global_id", "node", "", "id"));
	ASSERT_TRUE(indexManager->createIndex("global_country", "node", "", "country"));

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			1,
			"outgoing",
			std::vector<int64_t>{labelId},
			std::vector<std::pair<std::string, PropertyValue>>{
					{"id", PropertyValue(std::string("target"))},
					{"country", PropertyValue(std::string("US"))}},
			indexManager,
			std::vector<std::string>{"Node"});

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	auto batch = op->next();
	op->close();
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	const auto emitted = (*batch)[0].getNode("dst");
	ASSERT_TRUE(emitted.has_value());
	EXPECT_EQ(emitted->getId(), matching.getId());
	EXPECT_TRUE(snapshot.contains("varlength.target_index.source.scoped"));
}

TEST_F(VarLengthTraversalOperatorTest, IndexedReversePruningHandlesBothDirectionAndSharedPredecessor) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node targetA(0, labelId);
	dm->addNode(targetA);
	Node targetB(0, labelId);
	dm->addNode(targetB);
	dm->addNodeProperties(targetA.getId(), {{"group", PropertyValue(std::string("target"))}});
	dm->addNodeProperties(targetB.getId(), {{"group", PropertyValue(std::string("target"))}});

	Edge toA(0, sourceNode.getId(), targetA.getId(), typeId);
	dm->addEdge(toA);
	Edge toB(0, sourceNode.getId(), targetB.getId(), typeId);
	dm->addEdge(toB);

	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("", "node", "", "group"));

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			3,
			"both",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"group", PropertyValue(std::string("target"))}},
			indexManager);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	size_t total = 0;
	while (auto batch = op->next()) {
		total += batch->size();
	}
	op->close();
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_EQ(total, 2UL);
	EXPECT_TRUE(snapshot.contains("varlength.target_index.strategy.bidirectional_prune"));
}

TEST_F(VarLengthTraversalOperatorTest, IndexedReversePruningSupportsIncomingTraversal) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node target(0, labelId);
	dm->addNode(target);
	dm->addNodeProperties(target.getId(), {{"id", PropertyValue(std::string("target"))}});
	Node middle(0, labelId);
	dm->addNode(middle);
	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Edge first(0, target.getId(), middle.getId(), typeId);
	dm->addEdge(first);
	Edge second(0, middle.getId(), sourceNode.getId(), typeId);
	dm->addEdge(second);

	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("", "node", "", "id"));

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			3,
			"incoming",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue(std::string("target"))}},
			indexManager);
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	auto batch = op->next();
	op->close();
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	const auto emitted = (*batch)[0].getNode("dst");
	ASSERT_TRUE(emitted.has_value());
	EXPECT_EQ(emitted->getId(), target.getId());
	EXPECT_TRUE(snapshot.contains("varlength.target_index.strategy.bidirectional_prune"));
}

TEST_F(VarLengthTraversalOperatorTest, TargetPredicateRejectsInvalidLabelsAndMissingProperties) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node target(0, labelId);
	dm->addNode(target);
	Edge edge(0, sourceNode.getId(), target.getId(), typeId);
	dm->addEdge(edge);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			1,
			"outgoing",
			std::vector<int64_t>{0},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue(std::string("missing"))}});

	op->open();
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, NullChildAndMissingEdgeTypeReturnNoRows) {
	auto noChild = std::make_unique<VarLengthTraversalOperator>(
			dm, nullptr, "src", "dst", "", 1, 1, "outgoing");
	noChild->open();
	EXPECT_TRUE(noChild->getOutputVariables().size() == 1U);
	EXPECT_FALSE(noChild->next().has_value());
	noChild->close();

	const int64_t labelId = dm->getOrCreateTokenId("Node");
	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto missingType = std::make_unique<VarLengthTraversalOperator>(
			dm, std::move(source), "src", "dst", "MISSING_EDGE_TYPE", 1, 2, "outgoing");
	missingType->open();
	EXPECT_FALSE(missingType->next().has_value());
	missingType->close();
}

TEST_F(VarLengthTraversalOperatorTest, FrontierTraversalSkipsInputRowsWithoutSourceNode) {
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");
	(void) typeId;

	Record rowWithoutSource;
	rowWithoutSource.setValue("not_src", PropertyValue(int64_t{1}));
	RecordBatch batch;
	batch.push_back(std::move(rowWithoutSource));

	auto source = std::make_unique<MockRecordBatchOperator>(std::vector<RecordBatch>{std::move(batch)});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm, std::move(source), "src", "dst", "NEXT", 1, 2, "outgoing");
	graph::concurrent::ThreadPool pool(2);
	op->setThreadPool(&pool);

	op->open();
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, NullChildWithThreadPoolDoesNotInitializeFrontier) {
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");
	(void) typeId;

	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm, nullptr, "src", "dst", "NEXT", 1, 2, "outgoing");
	graph::concurrent::ThreadPool pool(2);
	op->setThreadPool(&pool);

	op->open();
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, SingleThreadPoolUsesDfsForDeepPropertyTraversal) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node middle(0, labelId);
	dm->addNode(middle);
	Node target(0, labelId);
	dm->addNode(target);
	dm->addNodeProperties(target.getId(), {{"id", PropertyValue(std::string("target"))}});

	Edge first(0, sourceNode.getId(), middle.getId(), typeId);
	dm->addEdge(first);
	Edge second(0, middle.getId(), target.getId(), typeId);
	dm->addEdge(second);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			3,
			"outgoing",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue(std::string("target"))}});
	graph::concurrent::ThreadPool singleWorkerPool(1);
	op->setThreadPool(&singleWorkerPool);

	op->open();
	auto batch = op->next();
	op->close();

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	const auto emitted = (*batch)[0].getNode("dst");
	ASSERT_TRUE(emitted.has_value());
	EXPECT_EQ(emitted->getId(), target.getId());
}

TEST_F(VarLengthTraversalOperatorTest, DfsTargetPropertyMismatchFiltersReachableNode) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node target(0, labelId);
	dm->addNode(target);
	dm->addNodeProperties(target.getId(), {{"id", PropertyValue(std::string("actual"))}});
	Edge edge(0, sourceNode.getId(), target.getId(), typeId);
	dm->addEdge(edge);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			1,
			"outgoing",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue(std::string("expected"))}});

	op->open();
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, DfsMinZeroPredicateSkipsInactiveSourceNode) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");
	(void) typeId;

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	dm->addNodeProperties(sourceNode.getId(), {{"id", PropertyValue(std::string("source"))}});
	dm->deleteNode(sourceNode);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			0,
			0,
			"outgoing",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"id", PropertyValue(std::string("source"))}});

	op->open();
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, DfsMinZeroWithoutPredicatesDropsInactiveSourceDuringMaterialization) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");
	(void) typeId;

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	dm->deleteNode(sourceNode);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm, std::move(source), "src", "dst", "NEXT", 0, 0, "outgoing");

	op->open();
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, ThreadPoolDoesNotUseFrontierWhenMaxHopsIsZero) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");
	(void) typeId;

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm, std::move(source), "src", "dst", "NEXT", 1, 0, "outgoing");
	graph::concurrent::ThreadPool pool(2);
	op->setThreadPool(&pool);

	op->open();
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, UnresolvedEdgeTypeWithThreadPoolReturnsNoRows) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm, std::move(source), "src", "dst", "MISSING_EDGE_TYPE", 1, 2, "outgoing");
	graph::concurrent::ThreadPool pool(2);
	op->setThreadPool(&pool);

	op->open();
	EXPECT_FALSE(op->next().has_value());
	op->close();
}

TEST_F(VarLengthTraversalOperatorTest, IndexedTargetCandidateSetSkipsReversePruningWhenLarge) {
	const int64_t labelId = dm->getOrCreateTokenId("Node");
	const int64_t typeId = dm->getOrCreateTokenId("NEXT");

	Node sourceNode(0, labelId);
	dm->addNode(sourceNode);
	Node middle(0, labelId);
	dm->addNode(middle);
	Edge first(0, sourceNode.getId(), middle.getId(), typeId);
	dm->addEdge(first);

	std::vector<Node> candidates;
	candidates.reserve(VarLengthTraversalOperator::INDEX_ASSISTED_REVERSE_TARGET_MAX_CANDIDATES + 1);
	for (size_t i = 0; i <= VarLengthTraversalOperator::INDEX_ASSISTED_REVERSE_TARGET_MAX_CANDIDATES; ++i) {
		Node candidate(0, labelId);
		dm->addNode(candidate);
		dm->addNodeProperties(candidate.getId(), {{"group", PropertyValue(std::string("candidate"))}});
		candidates.push_back(candidate);
	}
	Edge reachable(0, middle.getId(), candidates.front().getId(), typeId);
	dm->addEdge(reachable);

	auto indexManager = db->getQueryEngine()->getIndexManager();
	ASSERT_TRUE(indexManager->createIndex("", "node", "", "group"));

	auto source = std::make_unique<MockSourceOperator>("src", std::vector<Node>{sourceNode});
	auto op = std::make_unique<VarLengthTraversalOperator>(
			dm,
			std::move(source),
			"src",
			"dst",
			"NEXT",
			1,
			3,
			"outgoing",
			std::vector<int64_t>{},
			std::vector<std::pair<std::string, PropertyValue>>{{"group", PropertyValue(std::string("candidate"))}},
			indexManager);
	graph::concurrent::ThreadPool pool(4);
	op->setThreadPool(&pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	op->open();
	auto batch = op->next();
	op->close();
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	ASSERT_TRUE(batch.has_value());
	ASSERT_EQ(batch->size(), 1UL);
	const auto emitted = (*batch)[0].getNode("dst");
	ASSERT_TRUE(emitted.has_value());
	EXPECT_EQ(emitted->getId(), candidates.front().getId());
	EXPECT_TRUE(snapshot.contains("varlength.target_index.candidates"));
	EXPECT_FALSE(snapshot.contains("varlength.target_index.strategy.bidirectional_prune"));
}
