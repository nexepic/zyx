#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

class RelationshipExpandCountScanIntegrationTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_relationship_expand_scan_path_" + boost::uuids::to_string(uuid) + ".zyx");
		if (fs::exists(testDbPath)) {
			fs::remove_all(testDbPath);
		}

		db = std::make_unique<Database>(testDbPath.string());
		db->open();
		(void)execute("CREATE (:User {id: 'u1'})");
		(void)execute("CREATE (:User {id: 'u2'})");
		(void)execute("CREATE (:User {id: 'u3'})");
		(void)execute("CREATE (:User {id: 'u4'})");
		(void)execute("CREATE INDEX ON :User(id)");
		(void)execute("MATCH (a:User {id: 'u1'}), (b:User {id: 'u2'}) CREATE (a)-[:FOLLOWS {weight: 1}]->(b)");
		(void)execute("MATCH (a:User {id: 'u1'}), (b:User {id: 'u3'}) CREATE (a)-[:FOLLOWS {weight: 2}]->(b)");
		(void)execute("MATCH (a:User {id: 'u2'}), (b:User {id: 'u4'}) CREATE (a)-[:FOLLOWS {weight: 1}]->(b)");
		(void)execute("MATCH (a:User {id: 'u3'}), (b:User {id: 'u4'}) CREATE (a)-[:FOLLOWS {weight: 1}]->(b)");
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

TEST_F(RelationshipExpandCountScanIntegrationTest, CountsOneHopExpandWithLogicalProfileSummary) {
	EXPECT_EQ(runCount("MATCH (:User {id: 'u1'})-[:FOLLOWS]->(v:User) RETURN count(v) AS count"), 2);
	EXPECT_TRUE(hasLogicalProfileSummary(
			executeProfiled("MATCH (:User {id: 'u1'})-[:FOLLOWS]->(v:User) RETURN count(v) AS count"), 1U));
}

TEST_F(RelationshipExpandCountScanIntegrationTest, CountsTwoHopExpandWithLogicalProfileSummary) {
	EXPECT_EQ(runCount("MATCH (:User {id: 'u1'})-[:FOLLOWS]->(:User)-[:FOLLOWS]->(v:User) RETURN count(v) AS count"), 2);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled(
				"MATCH (:User {id: 'u1'})-[:FOLLOWS]->(:User)-[:FOLLOWS]->(v:User) RETURN count(v) AS count"),
				1U));
}

TEST_F(RelationshipExpandCountScanIntegrationTest, ReturningTraversalRowsUsesLogicalProfileSummary) {
	auto result = execute("MATCH (:User {id: 'u1'})-[:FOLLOWS]->(v:User) RETURN v");
	EXPECT_EQ(result.rowCount(), 2U);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled("MATCH (:User {id: 'u1'})-[:FOLLOWS]->(v:User) RETURN v"), 2U));
}

TEST_F(RelationshipExpandCountScanIntegrationTest, EdgePropertyFilterUsesLogicalProfileSummary) {
	EXPECT_EQ(runCount("MATCH (:User {id: 'u1'})-[r:FOLLOWS]->(v:User) WHERE r.weight = 1 RETURN count(v) AS count"), 1);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled(
				"MATCH (:User {id: 'u1'})-[r:FOLLOWS]->(v:User) WHERE r.weight = 1 RETURN count(v) AS count"),
				1U));
}

TEST_F(RelationshipExpandCountScanIntegrationTest, CountsUnanchoredRelationshipTypeWithLogicalProfileSummary) {
	EXPECT_EQ(runCount("MATCH ()-[r:FOLLOWS]->() RETURN count(r) AS count"), 4);
	EXPECT_TRUE(hasLogicalProfileSummary(executeProfiled("MATCH ()-[r:FOLLOWS]->() RETURN count(r) AS count"), 1U));
}

TEST_F(RelationshipExpandCountScanIntegrationTest, CountsUnanchoredRelationshipPropertyWithLogicalProfileSummary) {
	EXPECT_EQ(runCount("MATCH ()-[r:FOLLOWS]->() WHERE r.weight = 1 RETURN count(r) AS count"), 3);
	EXPECT_TRUE(hasLogicalProfileSummary(
			executeProfiled("MATCH ()-[r:FOLLOWS]->() WHERE r.weight = 1 RETURN count(r) AS count"), 1U));
}

TEST_F(RelationshipExpandCountScanIntegrationTest, AnchoredExpandWithoutSeedIndexPreservesQuerySemantics) {
	EXPECT_EQ(runCount("MATCH (:User {missing: 'u1'})-[:FOLLOWS]->(v:User) RETURN count(v) AS count"), 0);
	EXPECT_TRUE(hasLogicalProfileSummary(
			executeProfiled("MATCH (:User {missing: 'u1'})-[:FOLLOWS]->(v:User) RETURN count(v) AS count"), 1U));
}
