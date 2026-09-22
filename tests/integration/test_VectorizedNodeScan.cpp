#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"

namespace fs = std::filesystem;
using namespace graph;

class VectorizedNodeScanIntegrationTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_vectorized_count_scan_path_" + boost::uuids::to_string(uuid) + ".zyx");
		if (fs::exists(testDbPath)) {
			fs::remove_all(testDbPath);
		}

		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		(void) execute("CREATE (:Person {name: 'Alice', age: 42, score: 10})");
		(void) execute("CREATE (:Person {name: 'Bob', age: 7, score: 20})");
		(void) execute("CREATE (:Person {name: 'Cara', age: 65, score: 5, country: 'CN'})");
		(void) execute("CREATE (:Animal {name: 'Cat', age: 42, score: 30})");
		(void) execute("CREATE (:Person {name: 'MissingAge'})");
		(void) execute("MATCH (a:Person {name: 'Alice'}), (b:Person {name: 'Bob'}) "
						"CREATE (a)-[:KNOWS {alpha: 7}]->(b)");
		(void) execute("MATCH (a:Person {name: 'Alice'}), (c:Person {name: 'Cara'}) "
						"CREATE (a)-[:KNOWS {alpha: 8}]->(c)");
		(void) execute("MATCH (b:Person {name: 'Bob'}), (a:Person {name: 'Alice'}) "
						"CREATE (b)-[:KNOWS {alpha: 7}]->(a)");
		(void) execute("MATCH (a:Person {name: 'Alice'}) CREATE (a)-[:KNOWS {alpha: 7}]->(a)");
		(void) execute("MATCH (a:Person {name: 'Alice'}), (b:Person {name: 'Bob'}) "
						"CREATE (a)-[:KNOWS {alpha: 7}]->(b)");
		(void) execute("MATCH (cat:Animal {name: 'Cat'}), (a:Person {name: 'Alice'}) "
						"CREATE (cat)-[:KNOWS {alpha: 7}]->(a)");
	}

	void TearDown() override {
		if (db) {
			db->close();
		}
		db.reset();
		std::error_code ec;
		if (fs::exists(testDbPath)) {
			fs::remove_all(testDbPath, ec);
		}
	}

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

	query::QueryResult executeProfiled(const std::string &query) const { return execute("PROFILE " + query); }

	bool hasLogicalProfileSummary(const query::QueryResult &result, size_t resultRowCount) const {
		if (result.rowCount() != resultRowCount + 1U) {
			return false;
		}

		const auto &summary = result.getRows().back();
		return summary.contains("phase") && summary.contains("total_time_ms") && summary.contains("calls") &&
			   summary.at("phase").asPrimitive().toString() == "logical_plan";
	}

	int64_t runCount(const std::string &query) const {
		auto result = execute(query);
		EXPECT_EQ(result.rowCount(), 1U);
		if (result.rowCount() != 1U) {
			return -1;
		}

		const auto &row = result.getRows()[0];
		EXPECT_EQ(row.size(), 1U);
		if (row.empty()) {
			return -1;
		}

		return std::stoll(row.begin()->second.asPrimitive().toString());
	}

	fs::path testDbPath;
	std::unique_ptr<Database> db;
};

TEST_F(VectorizedNodeScanIntegrationTest, CountsLabelNodesWithLogicalProfileSummary) {
	EXPECT_EQ(runCount("MATCH (n:Person) RETURN count(n) AS count"), 4);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled("MATCH (n:Person) RETURN count(n) AS count"), 1U));
}

TEST_F(VectorizedNodeScanIntegrationTest, CountsPropertyEqualityNodesWithLogicalProfileSummary) {
	EXPECT_EQ(runCount("MATCH (n:Person {age: 42}) RETURN count(n) AS count"), 1);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled("MATCH (n:Person {age: 42}) RETURN count(n) AS count"), 1U));
}

TEST_F(VectorizedNodeScanIntegrationTest, IndexedEqualityCountPreservesQuerySemantics) {
	auto indexResult = execute("CREATE INDEX ON :Person(age)");
	ASSERT_EQ(indexResult.rowCount(), 1U);

	EXPECT_EQ(runCount("MATCH (n:Person {age: 42}) RETURN count(n) AS count"), 1);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled("MATCH (n:Person {age: 42}) RETURN count(n) AS count"), 1U));
}

TEST_F(VectorizedNodeScanIntegrationTest, IndexedInclusiveRangeCountPreservesQuerySemantics) {
	auto indexResult = execute("CREATE INDEX ON :Person(age)");
	ASSERT_EQ(indexResult.rowCount(), 1U);

	EXPECT_EQ(runCount("MATCH (n:Person) WHERE n.age >= 30 AND n.age <= 65 RETURN count(n) AS count"), 2);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled(
				"MATCH (n:Person) WHERE n.age >= 30 AND n.age <= 65 RETURN count(n) AS count"),
				1U));
}

TEST_F(VectorizedNodeScanIntegrationTest, IndexedExclusiveRangeCountPreservesQuerySemantics) {
	auto indexResult = execute("CREATE INDEX ON :Person(age)");
	ASSERT_EQ(indexResult.rowCount(), 1U);

	EXPECT_EQ(runCount("MATCH (n:Person) WHERE n.age >= 30 AND n.age < 65 RETURN count(n) AS count"), 1);
	EXPECT_TRUE(hasLogicalProfileSummary(
			executeProfiled("MATCH (n:Person) WHERE n.age >= 30 AND n.age < 65 RETURN count(n) AS count"), 1U));
}

TEST_F(VectorizedNodeScanIntegrationTest, IndexedPropertyFilterHonorsLimitBeforeGenericPropertyPrefetch) {
	ASSERT_EQ(execute("CREATE INDEX ON :Person(country)").rowCount(), 1U);
	(void) execute("UNWIND range(0, 299) AS index CREATE (:Person {country: 'CN', age: index})");

	const bool valuesWereEnabled = debug::PerfTrace::areValuesEnabled();
	debug::PerfTrace::reset();
	debug::PerfTrace::setValueCollectionEnabled(true);
	const query::QueryResult result = execute(
			"MATCH (n:Person) WHERE n.country = 'CN' RETURN n.age LIMIT 1");
	const debug::PerfTrace::Snapshot trace = debug::PerfTrace::snapshotAndReset();
	debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_EQ(result.rowCount(), 1U);
	ASSERT_TRUE(trace.contains("native_btree.read.range_cursor_root_descent"));
	EXPECT_EQ(trace.at("native_btree.read.range_cursor_root_descent").totalValue, 1);
	// The indexed candidate validator may inspect one bounded batch, but the
	// generic full-scan property prefetch path must not enumerate all 300 Nodes.
	ASSERT_TRUE(trace.contains("native_graph.read.node_sorted_batch_entries"));
	EXPECT_LE(trace.at("native_graph.read.node_sorted_batch_entries").totalValue,
			static_cast<int64_t>(graph::storage::BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES));
}

TEST_F(VectorizedNodeScanIntegrationTest, ScopedIndexDoesNotChangeUnlabeledQuerySemantics) {
	auto indexResult = execute("CREATE INDEX ON :Person(age)");
	ASSERT_EQ(indexResult.rowCount(), 1U);

	EXPECT_EQ(runCount("MATCH (n {age: 42}) RETURN count(n) AS count"), 2);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled("MATCH (n {age: 42}) RETURN count(n) AS count"), 1U));
}

TEST_F(VectorizedNodeScanIntegrationTest, CountsWhereLowerBoundPropertyFilterWithLogicalProfileSummary) {
	EXPECT_EQ(runCount("MATCH (n) WHERE n.age >= 30 RETURN count(n) AS count"), 3);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled("MATCH (n) WHERE n.age >= 30 RETURN count(n) AS count"), 1U));
}

TEST_F(VectorizedNodeScanIntegrationTest, FiltersThroughSnapshotCachedTokensAndSelectedPropertyDecode) {
	const bool valuesWereEnabled = debug::PerfTrace::areValuesEnabled();
	debug::PerfTrace::reset();
	debug::PerfTrace::setValueCollectionEnabled(true);
	EXPECT_EQ(runCount("MATCH (n:Person) WHERE n.age >= 30 RETURN count(n) AS count"), 2);
	const debug::PerfTrace::Snapshot trace = debug::PerfTrace::snapshotAndReset();
	debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(trace.contains("native_graph.read.label_token_resolution"));
	EXPECT_EQ(trace.at("native_graph.read.label_token_resolution").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.property_token_resolution"));
	EXPECT_EQ(trace.at("native_graph.read.property_token_resolution").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_decode"));
	EXPECT_GT(trace.at("native_graph.read.property_selected_decode").totalValue, 0);
	EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
}

TEST_F(VectorizedNodeScanIntegrationTest, FilteredEdgeCountUsesOneGlobalTopologyScanAndSelectedPropertyDecode) {
	const bool valuesWereEnabled = debug::PerfTrace::areValuesEnabled();
	debug::PerfTrace::reset();
	debug::PerfTrace::setValueCollectionEnabled(true);
	EXPECT_EQ(runCount("MATCH ()-[r:KNOWS]->() WHERE r.alpha = 7 RETURN count(r) AS count"), 5);
	const debug::PerfTrace::Snapshot trace = debug::PerfTrace::snapshotAndReset();
	debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_TRUE(trace.contains("logical_plan.direct_relationship_count"));
	EXPECT_EQ(trace.at("logical_plan.direct_relationship_count").totalValue, 1);
	ASSERT_TRUE(trace.contains("logical_plan.direct_relationship_count_edge_records"));
	EXPECT_EQ(trace.at("logical_plan.direct_relationship_count_edge_records").totalValue, 6);
	ASSERT_TRUE(trace.contains("native_graph.read.property_token_resolution"));
	EXPECT_EQ(trace.at("native_graph.read.property_token_resolution").totalValue, 1);
	ASSERT_TRUE(trace.contains("native_graph.read.property_selected_decode"));
	EXPECT_GT(trace.at("native_graph.read.property_selected_decode").totalValue, 0);
	EXPECT_FALSE(trace.contains("native_graph.read.property_full_decode"));
}

TEST_F(VectorizedNodeScanIntegrationTest, DirectEdgeScanPreservesDirectionEndpointAndParallelEdgeSemantics) {
	EXPECT_EQ(runCount("MATCH (a:Person {name: 'Alice'})-[r:KNOWS]->(b:Person {name: 'Bob'}) "
						   "WHERE r.alpha = 7 RETURN count(r) AS count"),
			  2);
	EXPECT_EQ(runCount("MATCH (a:Person {name: 'Alice'})<-[r:KNOWS]-(b:Person) "
						   "WHERE r.alpha = 7 RETURN count(r) AS count"),
			  2);
	EXPECT_EQ(runCount("MATCH (a:Person {name: 'Alice'})-[r:KNOWS]->(a) RETURN count(r) AS count"), 1);
}

TEST_F(VectorizedNodeScanIntegrationTest, DirectEdgeScanStopsBeforeReadingTheRemainingTopology) {
	const bool valuesWereEnabled = debug::PerfTrace::areValuesEnabled();
	debug::PerfTrace::reset();
	debug::PerfTrace::setValueCollectionEnabled(true);
	const query::QueryResult result = execute("MATCH ()-[r:KNOWS]->() RETURN id(r) AS id LIMIT 1");
	const debug::PerfTrace::Snapshot trace = debug::PerfTrace::snapshotAndReset();
	debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);

	ASSERT_EQ(result.rowCount(), 1U);
	ASSERT_TRUE(trace.contains("logical_plan.direct_edge_records"));
	EXPECT_EQ(trace.at("logical_plan.direct_edge_records").totalValue, 1);
}

TEST_F(VectorizedNodeScanIntegrationTest, CountsWhereUpperBoundPropertyFilterWithLogicalProfileSummary) {
	EXPECT_EQ(runCount("MATCH (n) WHERE n.score < 20 RETURN count(n) AS count"), 2);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled("MATCH (n) WHERE n.score < 20 RETURN count(n) AS count"), 1U));
}

TEST_F(VectorizedNodeScanIntegrationTest, CountsWhereEqualityAndRangeFilterWithLogicalProfileSummary) {
	EXPECT_EQ(runCount("MATCH (u:Person) WHERE u.country = 'CN' AND u.age >= 30 RETURN count(u) AS count"), 1);
	EXPECT_TRUE(hasLogicalProfileSummary(
			executeProfiled("MATCH (u:Person) WHERE u.country = 'CN' AND u.age >= 30 RETURN count(u) AS count"), 1U));
}

TEST_F(VectorizedNodeScanIntegrationTest, CountsDistinctPropertyWithLogicalProfileSummary) {
	EXPECT_EQ(runCount("MATCH (u:Person) RETURN count(DISTINCT u.country) AS count"), 1);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled("MATCH (u:Person) RETURN count(DISTINCT u.country) AS count"), 1U));
}

TEST_F(VectorizedNodeScanIntegrationTest, ReturningNodesUsesLogicalProfileSummary) {
	auto result = execute("MATCH (n:Person) RETURN n");
	EXPECT_EQ(result.rowCount(), 4U);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled("MATCH (n:Person) RETURN n"), 4U));
}

TEST_F(VectorizedNodeScanIntegrationTest, TopKPropertySortUsesLogicalProfileSummary) {
	auto result = execute("MATCH (u:Person) RETURN u.name AS name ORDER BY u.score DESC LIMIT 2");
	ASSERT_EQ(result.rowCount(), 2U);
	ASSERT_TRUE(result.getRows()[0].contains("name"));
	ASSERT_TRUE(result.getRows()[1].contains("name"));
	EXPECT_EQ(result.getRows()[0].at("name").asPrimitive().toString(), "Bob");
	EXPECT_EQ(result.getRows()[1].at("name").asPrimitive().toString(), "Alice");
	EXPECT_TRUE(hasLogicalProfileSummary(
			executeProfiled("MATCH (u:Person) RETURN u.name AS name ORDER BY u.score DESC LIMIT 2"), 2U));
}
