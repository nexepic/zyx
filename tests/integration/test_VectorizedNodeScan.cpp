#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#include "graph/core/Database.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/api/QueryResult.hpp"

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
		debug::PerfTrace::reset();
		debug::PerfTrace::setEnabled(false);
	}

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

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

TEST_F(VectorizedNodeScanIntegrationTest, CountsLabelNodesWithScanProfilePhase) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (n:Person) RETURN count(n) AS count"), 4);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
}

TEST_F(VectorizedNodeScanIntegrationTest, CountsPropertyEqualityNodesWithScanProfilePhase) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (n:Person {age: 42}) RETURN count(n) AS count"), 1);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
}

TEST_F(VectorizedNodeScanIntegrationTest, IndexedEqualityCountAvoidsPropertyMaterialization) {
	auto indexResult = execute("CREATE INDEX ON :Person(age)");
	ASSERT_EQ(indexResult.rowCount(), 1U);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (n:Person {age: 42}) RETURN count(n) AS count"), 1);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
	EXPECT_TRUE(snapshot.contains("node_scan.candidates"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
	EXPECT_FALSE(snapshot.contains("node_scan.label_check"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_property_entities"));
}

TEST_F(VectorizedNodeScanIntegrationTest, IndexedInclusiveRangeCountAvoidsPropertyMaterialization) {
	auto indexResult = execute("CREATE INDEX ON :Person(age)");
	ASSERT_EQ(indexResult.rowCount(), 1U);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (n:Person) WHERE n.age >= 30 AND n.age <= 65 RETURN count(n) AS count"), 2);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
	EXPECT_TRUE(snapshot.contains("node_scan.candidates"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
	EXPECT_FALSE(snapshot.contains("node_scan.label_check"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_property_entities"));
}

TEST_F(VectorizedNodeScanIntegrationTest, IndexedExclusiveRangeCountAvoidsPropertyMaterialization) {
	auto indexResult = execute("CREATE INDEX ON :Person(age)");
	ASSERT_EQ(indexResult.rowCount(), 1U);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (n:Person) WHERE n.age >= 30 AND n.age < 65 RETURN count(n) AS count"), 1);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
	EXPECT_TRUE(snapshot.contains("node_scan.candidates"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_nodes"));
	EXPECT_FALSE(snapshot.contains("node_scan.label_check"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_properties"));
	EXPECT_FALSE(snapshot.contains("node_scan.load_property_entities"));
}

TEST_F(VectorizedNodeScanIntegrationTest, ScopedIndexDoesNotServeUnlabeledCount) {
	auto indexResult = execute("CREATE INDEX ON :Person(age)");
	ASSERT_EQ(indexResult.rowCount(), 1U);
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (n {age: 42}) RETURN count(n) AS count"), 2);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
	EXPECT_TRUE(snapshot.contains("node_scan.candidates"));
	EXPECT_TRUE(snapshot.contains("node_scan.load_nodes"));
	EXPECT_TRUE(snapshot.contains("node_scan.load_properties"));
}

TEST_F(VectorizedNodeScanIntegrationTest, CountsWhereLowerBoundPropertyFilterWithScanProfilePhase) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (n) WHERE n.age >= 30 RETURN count(n) AS count"), 3);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
}

TEST_F(VectorizedNodeScanIntegrationTest, CountsWhereUpperBoundPropertyFilterWithScanProfilePhase) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (n) WHERE n.score < 20 RETURN count(n) AS count"), 2);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
}

TEST_F(VectorizedNodeScanIntegrationTest, CountsWhereEqualityAndRangeFilterWithScanProfilePhase) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (u:Person) WHERE u.country = 'CN' AND u.age >= 30 RETURN count(u) AS count"), 1);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
}

TEST_F(VectorizedNodeScanIntegrationTest, CountsDistinctPropertyWithColumnarScanPath) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (u:Person) RETURN count(DISTINCT u.country) AS count"), 1);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.distinct_count"));
	EXPECT_FALSE(snapshot.contains("scan.sequential"));
}

TEST_F(VectorizedNodeScanIntegrationTest, ReturningNodesUsesLegacyPath) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	auto result = execute("MATCH (n:Person) RETURN n");
	EXPECT_EQ(result.rowCount(), 4U);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_FALSE(snapshot.contains("node_scan.count"));
}

TEST_F(VectorizedNodeScanIntegrationTest, TopKPropertySortUsesColumnarScanPath) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	auto result = execute("MATCH (u:Person) RETURN u.name AS name ORDER BY u.score DESC LIMIT 2");
	ASSERT_EQ(result.rowCount(), 2U);
	ASSERT_TRUE(result.getRows()[0].contains("name"));
	ASSERT_TRUE(result.getRows()[1].contains("name"));
	EXPECT_EQ(result.getRows()[0].at("name").asPrimitive().toString(), "Bob");
	EXPECT_EQ(result.getRows()[1].at("name").asPrimitive().toString(), "Alice");

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.topk"));
	EXPECT_FALSE(snapshot.contains("scan.sequential"));
}
