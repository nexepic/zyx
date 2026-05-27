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

class VectorizedNodeCountFastPathIntegrationTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_vectorized_count_fast_path_" + boost::uuids::to_string(uuid) + ".zyx");
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

TEST_F(VectorizedNodeCountFastPathIntegrationTest, CountsLabelNodesWithFastPathProfilePhase) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (n:Person) RETURN count(n) AS count"), 4);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
}

TEST_F(VectorizedNodeCountFastPathIntegrationTest, CountsPropertyEqualityNodesWithFastPathProfilePhase) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (n:Person {age: 42}) RETURN count(n) AS count"), 1);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
}

TEST_F(VectorizedNodeCountFastPathIntegrationTest, CountsWhereLowerBoundPropertyFilterWithFastPathProfilePhase) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (n) WHERE n.age >= 30 RETURN count(n) AS count"), 3);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
}

TEST_F(VectorizedNodeCountFastPathIntegrationTest, CountsWhereUpperBoundPropertyFilterWithFastPathProfilePhase) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (n) WHERE n.score < 20 RETURN count(n) AS count"), 2);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
}

TEST_F(VectorizedNodeCountFastPathIntegrationTest, CountsWhereEqualityAndRangeFilterWithFastPathProfilePhase) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	EXPECT_EQ(runCount("MATCH (u:Person) WHERE u.country = 'CN' AND u.age >= 30 RETURN count(u) AS count"), 1);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_TRUE(snapshot.contains("node_scan.count"));
}

TEST_F(VectorizedNodeCountFastPathIntegrationTest, ReturningNodesUsesLegacyPath) {
	debug::PerfTrace::setEnabled(true);
	debug::PerfTrace::reset();

	auto result = execute("MATCH (n:Person) RETURN n");
	EXPECT_EQ(result.rowCount(), 4U);

	const auto snapshot = debug::PerfTrace::snapshotAndReset();
	EXPECT_FALSE(snapshot.contains("node_scan.count"));
}
