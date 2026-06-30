#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <set>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/RelationshipFrontierExecutor.hpp"

namespace fs = std::filesystem;
using namespace graph;
using namespace graph::query::execution;

class RelationshipFrontierExecutorTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		dbPath = fs::temp_directory_path() /
				 ("test_relationship_frontier_executor_" + boost::uuids::to_string(uuid) + ".zyx");
		db = std::make_unique<Database>(dbPath.string());
		db->open();
		dm = db->getStorage()->getDataManager();
		labelId = dm->getOrCreateTokenId("Node");
		typeId = dm->getOrCreateTokenId("NEXT");
	}

	void TearDown() override {
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		fs::remove(dbPath, ec);
	}

	Node addNode() {
		Node node(0, labelId);
		dm->addNode(node);
		return node;
	}

	void addEdge(int64_t source, int64_t target) {
		Edge edge(0, source, target, typeId);
		dm->addEdge(edge);
	}

	RelationshipFrontierPath makePath(const Node &node) const {
		Record record;
		record.setNode("src", node);
		RelationshipFrontierPath path;
		path.baseRecord = record;
		path.nodeId = node.getId();
		path.depth = 0;
		path.visitedNodeIds.push_back(node.getId());
		return path;
	}

	traversal::RelationshipTraversalOptions options(traversal::RelationshipDirectionKind direction) const {
		traversal::RelationshipTraversalOptions opts;
		opts.direction = direction;
		opts.integrity = traversal::RelationshipTraversalIntegrity::RTI_BOUND_BY_EDGE_COUNT;
		opts.activeOnly = true;
		opts.typeId = typeId;
		return opts;
	}

	fs::path dbPath;
	std::unique_ptr<Database> db;
	std::shared_ptr<storage::DataManager> dm;
	int64_t labelId = 0;
	int64_t typeId = 0;
};

TEST_F(RelationshipFrontierExecutorTest, EmptyAndNullInputsReturnNoNextFrontier) {
	RelationshipFrontierExecutor nullExecutor(nullptr, nullptr);
	EXPECT_TRUE(nullExecutor.expand({}, options(traversal::RelationshipDirectionKind::RDK_OUT)).empty());

	const Node source = addNode();
	EXPECT_TRUE(nullExecutor
						.expand({makePath(source)}, options(traversal::RelationshipDirectionKind::RDK_OUT))
						.empty());

	RelationshipFrontierExecutor executor(dm, nullptr);
	EXPECT_TRUE(executor.expand({}, options(traversal::RelationshipDirectionKind::RDK_OUT)).empty());
}

TEST_F(RelationshipFrontierExecutorTest, FrontierStateRebuildsSparsePathStateLazily) {
	Node source = addNode();
	Node middle = addNode();
	Node target = addNode();

	Record emptyRecord;
	RelationshipFrontierState emptyState;
	EXPECT_EQ(emptyState.currentDepth(), 0);

	RelationshipFrontierState state;
	Record sourceRecord;
	sourceRecord.setNode("src", source);
	state.addPath(sourceRecord, source.getId(), 0, {});
	ASSERT_EQ(state.sourceCount(), 1U);
	ASSERT_EQ(state.entryCount(), 1U);
	ASSERT_EQ(state.size(), 1U);
	auto sourcePath = state.materializePath(0);
	EXPECT_EQ(sourcePath.visitedNodeIds, (std::vector<int64_t>{source.getId()}));

	Record targetRecord;
	targetRecord.setNode("src", source);
	state.addPath(targetRecord, target.getId(), 2, {source.getId(), middle.getId()});
	ASSERT_EQ(state.size(), 2U);
	auto targetPath = state.materializePath(1);
	EXPECT_EQ(targetPath.nodeId, target.getId());
	EXPECT_EQ(targetPath.visitedNodeIds,
			  (std::vector<int64_t>{source.getId(), middle.getId(), target.getId()}));

	state.filterFrontier([](const RelationshipFrontierEntry &) { return false; });
	EXPECT_TRUE(state.empty());
	EXPECT_EQ(state.currentDepth(), 0);
}

TEST_F(RelationshipFrontierExecutorTest, OutgoingExpansionSkipsAncestorAndInactiveTargets) {
	Node source = addNode();
	Node middle = addNode();
	Node active = addNode();
	Node inactive = addNode();
	addEdge(source.getId(), middle.getId());
	addEdge(middle.getId(), source.getId());
	addEdge(middle.getId(), active.getId());
	addEdge(middle.getId(), inactive.getId());

	dm->deleteNode(inactive);

	auto traversalOptions = options(traversal::RelationshipDirectionKind::RDK_OUT);
	traversalOptions.activeOnly = false;
	RelationshipFrontierExecutor executor(dm, nullptr);

	RelationshipFrontierState state;
	Record record;
	record.setNode("src", source);
	state.addSource(std::move(record), source.getId());
	executor.expandInPlace(state, traversalOptions);
	ASSERT_EQ(state.size(), 1U);
	EXPECT_EQ(state.frontierEntry(0).nodeId, middle.getId());

	executor.expandInPlace(state, traversalOptions);

	ASSERT_EQ(state.size(), 1U);
	const auto next = state.materializePath(0);
	EXPECT_EQ(next.nodeId, active.getId());
	EXPECT_EQ(next.depth, 2);
	ASSERT_EQ(next.visitedNodeIds.size(), 3U);
	EXPECT_EQ(next.visitedNodeIds.front(), source.getId());
	EXPECT_EQ(next.visitedNodeIds.back(), active.getId());
	EXPECT_TRUE(next.baseRecord.getNode("src").has_value());
}

TEST_F(RelationshipFrontierExecutorTest, IncomingAndBothDirectionExpansionSelectsNeighborSide) {
	Node left = addNode();
	Node center = addNode();
	Node right = addNode();
	addEdge(left.getId(), center.getId());
	addEdge(center.getId(), right.getId());

	RelationshipFrontierExecutor executor(dm, nullptr);

	const auto incoming = executor.expand(
			{makePath(center)}, options(traversal::RelationshipDirectionKind::RDK_IN));
	ASSERT_EQ(incoming.size(), 1U);
	EXPECT_EQ(incoming[0].nodeId, left.getId());

	const auto both = executor.expand(
			{makePath(center)}, options(traversal::RelationshipDirectionKind::RDK_BOTH));
	std::set<int64_t> targets;
	for (const auto &path: both) {
		targets.insert(path.nodeId);
	}
	EXPECT_EQ(targets, (std::set<int64_t>{left.getId(), right.getId()}));
}

TEST_F(RelationshipFrontierExecutorTest, LegacyExpandPreservesExistingPathState) {
	Node source = addNode();
	Node middle = addNode();
	Node target = addNode();
	addEdge(source.getId(), middle.getId());
	addEdge(middle.getId(), source.getId());
	addEdge(middle.getId(), target.getId());

	RelationshipFrontierPath path = makePath(source);
	path.nodeId = middle.getId();
	path.depth = 1;
	path.visitedNodeIds.push_back(middle.getId());

	RelationshipFrontierExecutor executor(dm, nullptr);
	const auto next = executor.expand({path}, options(traversal::RelationshipDirectionKind::RDK_OUT));

	ASSERT_EQ(next.size(), 1U);
	EXPECT_EQ(next[0].nodeId, target.getId());
	EXPECT_EQ(next[0].depth, 2);
	EXPECT_EQ(next[0].visitedNodeIds, (std::vector<int64_t>{source.getId(), middle.getId(), target.getId()}));
	EXPECT_TRUE(next[0].baseRecord.getNode("src").has_value());
}

TEST_F(RelationshipFrontierExecutorTest, ExpansionSkipsDanglingTargetNode) {
	Node source = addNode();
	Edge edge(0, source.getId(), 999999, typeId);
	dm->addEdge(edge);

	RelationshipFrontierExecutor executor(dm, nullptr);
	const auto next = executor.expand(
			{makePath(source)},
			options(traversal::RelationshipDirectionKind::RDK_OUT));

	EXPECT_TRUE(next.empty());
}

TEST_F(RelationshipFrontierExecutorTest, ParallelExpansionEmitsDecisionTelemetry) {
	static constexpr size_t kSources = 64;
	static constexpr size_t kFanout = 64;
	std::vector<RelationshipFrontierPath> frontier;
	frontier.reserve(kSources);

	for (size_t i = 0; i < kSources; ++i) {
		Node source = addNode();
		frontier.push_back(makePath(source));
		for (size_t j = 0; j < kFanout; ++j) {
			Node target = addNode();
			addEdge(source.getId(), target.getId());
		}
	}

	graph::concurrent::ThreadPool pool(4);
	RelationshipFrontierExecutor executor(dm, &pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	const auto next = executor.expand(
			frontier,
			options(traversal::RelationshipDirectionKind::RDK_OUT),
			"test.relationship_frontier.expand");
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_EQ(next.size(), kSources * kFanout);
	ASSERT_TRUE(snapshot.contains("test.relationship_frontier.expand.decision.parallel"));
	ASSERT_TRUE(snapshot.contains("test.relationship_frontier.expand.workers"));
	EXPECT_GE(snapshot.at("test.relationship_frontier.expand.workers").totalValue, 2);
}

TEST_F(RelationshipFrontierExecutorTest, ParallelExpansionWithNoEdgesKeepsFrontierEmpty) {
	Node first = addNode();
	Node second = addNode();
	std::vector<RelationshipFrontierPath> frontier{makePath(first), makePath(second)};

	graph::concurrent::ThreadPool pool(4);
	RelationshipFrontierExecutor executor(dm, &pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	const auto next = executor.expand(
			frontier,
			options(traversal::RelationshipDirectionKind::RDK_OUT),
			"");
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_TRUE(next.empty());
	EXPECT_TRUE(snapshot.contains("relationship_frontier.estimated_edges"));
}

TEST_F(RelationshipFrontierExecutorTest, SingleHighFanoutSourceParallelizesEdgeRefProcessing) {
	static constexpr size_t kFanout = 4096;
	Node source = addNode();
	for (size_t i = 0; i < kFanout; ++i) {
		Node target = addNode();
		addEdge(source.getId(), target.getId());
	}

	graph::concurrent::ThreadPool pool(4);
	RelationshipFrontierExecutor executor(dm, &pool);

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	const auto next = executor.expand(
			{makePath(source)},
			options(traversal::RelationshipDirectionKind::RDK_OUT),
			"test.relationship_frontier.single_high_fanout");
	const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
	graph::debug::PerfTrace::setEnabled(false);

	EXPECT_EQ(next.size(), kFanout);
	ASSERT_TRUE(snapshot.contains("test.relationship_frontier.single_high_fanout.decision.parallel"));
	ASSERT_TRUE(snapshot.contains("test.relationship_frontier.single_high_fanout.workers"));
	EXPECT_GE(snapshot.at("test.relationship_frontier.single_high_fanout.workers").totalValue, 2);
}

TEST_F(RelationshipFrontierExecutorTest, SingleHighFanoutSourceWithOnlyVisitedTargetsReturnsEmpty) {
	static constexpr size_t kFanout = 4096;
	Node source = addNode();
	for (size_t i = 0; i < kFanout; ++i) {
		addEdge(source.getId(), source.getId());
	}

	graph::concurrent::ThreadPool pool(4);
	RelationshipFrontierExecutor executor(dm, &pool);

	const auto next = executor.expand(
			{makePath(source)},
			options(traversal::RelationshipDirectionKind::RDK_OUT));

	EXPECT_TRUE(next.empty());
}

TEST_F(RelationshipFrontierExecutorTest, TypeMismatchProducesEmptyFrontier) {
	const int64_t otherTypeId = dm->getOrCreateTokenId("OTHER");
	Node source = addNode();
	Node target = addNode();
	Edge edge(0, source.getId(), target.getId(), otherTypeId);
	dm->addEdge(edge);

	RelationshipFrontierExecutor executor(dm, nullptr);
	const auto next = executor.expand(
			{makePath(source)},
			options(traversal::RelationshipDirectionKind::RDK_OUT));

	EXPECT_TRUE(next.empty());
}
