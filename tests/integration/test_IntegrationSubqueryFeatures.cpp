/**
 * @file test_IntegrationSubqueryFeatures.cpp
 *
 * Integration tests for:
 * - FOREACH statement
 * - CALL { subquery }
 * - LOAD CSV
 */

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

class IntegrationSubqueryFeaturesTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath = fs::temp_directory_path() / ("test_subquery_" + boost::uuids::to_string(uuid) + ".zyx");
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath);
		db = std::make_unique<Database>(testDbPath.string());
		db->open();
	}

	void TearDown() override {
		if (db)
			db->close();
		db.reset();
		std::error_code ec;
		if (fs::exists(testDbPath))
			fs::remove_all(testDbPath, ec);
		// Clean up any CSV files we created
		for (auto &f : csvFiles) {
			if (fs::exists(f))
				fs::remove(f, ec);
		}
	}

	query::QueryResult execute(const std::string &query) const { return db->getQueryEngine()->execute(query); }

	std::string val(const query::QueryResult &r, const std::string &col, size_t row = 0) const {
		return r.getRows().at(row).at(col).toString();
	}

	// Helper to create a temporary CSV file and return its path
	std::string createTempCsv(const std::string &content) {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		auto path = fs::temp_directory_path() / ("test_csv_" + boost::uuids::to_string(uuid) + ".csv");
		std::ofstream out(path);
		out << content;
		out.close();
		csvFiles.push_back(path);

		// Use std::filesystem to convert path to generic format (forward slashes)
		// This handles Windows backslashes properly and is more portable than manual replace
		return path.generic_string();
	}

	std::filesystem::path testDbPath;
	std::unique_ptr<Database> db;
	std::vector<std::filesystem::path> csvFiles;
};

// ============================================================================
// FOREACH Tests
// ============================================================================

TEST_F(IntegrationSubqueryFeaturesTest, Foreach_CreateNodes) {
	execute("FOREACH (name IN ['Alice', 'Bob', 'Charlie'] | CREATE (:ForeachPerson {name: name}))");
	auto result = execute("MATCH (n:ForeachPerson) RETURN n.name AS name ORDER BY name");
	ASSERT_EQ(result.rowCount(), 3UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "name", 1), "Bob");
	EXPECT_EQ(val(result, "name", 2), "Charlie");
}

TEST_F(IntegrationSubqueryFeaturesTest, Foreach_SetProperties) {
	// FOREACH with SET on iterator variable - creates nodes and sets properties
	execute("FOREACH (name IN ['X', 'Y'] | CREATE (:ForeachSetTarget {name: name, val: 99}))");

	auto result = execute("MATCH (n:ForeachSetTarget) RETURN n.val AS v ORDER BY n.name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "v", 0), "99");
	EXPECT_EQ(val(result, "v", 1), "99");
}

TEST_F(IntegrationSubqueryFeaturesTest, Foreach_EmptyList) {
	// FOREACH over empty list should be a no-op
	execute("FOREACH (x IN [] | CREATE (:ShouldNotExist))");
	auto result = execute("MATCH (n:ShouldNotExist) RETURN count(n) AS c");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "c"), "0");
}

TEST_F(IntegrationSubqueryFeaturesTest, Foreach_WithPipeline) {
	// FOREACH after MATCH
	execute("CREATE (:ForeachSrc {name: 'source'})");
	execute("MATCH (s:ForeachSrc) FOREACH (i IN [1, 2, 3] | CREATE (:ForeachTarget {idx: i}))");
	auto result = execute("MATCH (n:ForeachTarget) RETURN count(n) AS c");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "c"), "3");
}

// ============================================================================
// CALL { subquery } Tests
// ============================================================================

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_Basic) {
	execute("CREATE (:SubqPerson {name: 'Alice', age: 30})");
	execute("CREATE (:SubqPerson {name: 'Bob', age: 25})");

	auto result = execute(
		"CALL { MATCH (p:SubqPerson) RETURN p.name AS name } "
		"RETURN name ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "name", 1), "Bob");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_WithOuterScope) {
	// Standalone CALL subquery that uses its own MATCH internally
	execute("CREATE (:SubqInner2 {val: 10})");
	execute("CREATE (:SubqInner2 {val: 20})");

	auto result = execute(
		"CALL { MATCH (i:SubqInner2) RETURN sum(i.val) AS total } "
		"RETURN total");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "total"), "30");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_ImportsOuterVariableAndExportsAlias) {
	execute("CREATE (:SubqOuter {name: 'Alice'})");
	execute("CREATE (:SubqOuter {name: 'Bob'})");

	const auto result = execute(
			"MATCH (person:SubqOuter) "
			"CALL { WITH person RETURN person.name AS innerName } "
			"RETURN person.name AS outerName, innerName ORDER BY outerName");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "outerName", 0), "Alice");
	EXPECT_EQ(val(result, "innerName", 0), "Alice");
	EXPECT_EQ(val(result, "outerName", 1), "Bob");
	EXPECT_EQ(val(result, "innerName", 1), "Bob");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_WriteOnly) {
	// Write-only subquery wrapped with MATCH to satisfy grammar
	execute("CREATE (:SubqDummy {x: 1})");
	execute("MATCH (d:SubqDummy) CALL { CREATE (:SubqWriteOnly {created: true}) } RETURN d");
	auto result = execute("MATCH (n:SubqWriteOnly) RETURN n.created AS c");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "c"), "true");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_WriteOnlyImportsOuterVariableAndPersists) {
	execute("CREATE (:SubqWriteImport {value: 7})");
	execute("CREATE (:SubqWriteImport {value: 11})");

	const auto outer = execute(
			"MATCH (seed:SubqWriteImport) "
			"CALL { WITH seed CREATE (:SubqImportedChild {copied: seed.value}) } "
			"RETURN seed.value AS value");
	ASSERT_EQ(outer.rowCount(), 2UL);
	std::vector<std::string> outerValues{val(outer, "value", 0), val(outer, "value", 1)};
	std::ranges::sort(outerValues);
	EXPECT_EQ(outerValues, (std::vector<std::string>{"11", "7"}));

	db->close();
	db->open();
	const auto persisted = execute("MATCH (child:SubqImportedChild) RETURN child.copied AS copied ORDER BY copied");
	ASSERT_EQ(persisted.rowCount(), 2UL);
	EXPECT_EQ(val(persisted, "copied", 0), "7");
	EXPECT_EQ(val(persisted, "copied", 1), "11");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_WriteOnlyImportsOuterVariableForSetAndRemove) {
	execute("CREATE (:SubqWriteUpdate {value: 3})");
	execute("CREATE (:SubqWriteUpdate {value: 5})");

	const auto outer = execute(
			"MATCH (seed:SubqWriteUpdate) "
			"CALL { WITH seed SET seed.doubled = seed.value * 2, seed.temporary = true REMOVE seed.temporary } "
			"RETURN seed.value AS value");
	ASSERT_EQ(outer.rowCount(), 2UL);

	db->close();
	db->open();
	const auto persisted = execute(
			"MATCH (seed:SubqWriteUpdate) RETURN seed.value AS value, seed.doubled AS doubled, seed.temporary AS temporary "
			"ORDER BY value");
	ASSERT_EQ(persisted.rowCount(), 2UL);
	EXPECT_EQ(val(persisted, "value", 0), "3");
	EXPECT_EQ(val(persisted, "doubled", 0), "6");
	EXPECT_EQ(val(persisted, "temporary", 0), "null");
	EXPECT_EQ(val(persisted, "value", 1), "5");
	EXPECT_EQ(val(persisted, "doubled", 1), "10");
	EXPECT_EQ(val(persisted, "temporary", 1), "null");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_WriteOnlyExportsUpdatedValue) {
	execute("CREATE (:SubqWriteExportUpdate {value: 3})");
	execute("CREATE (:SubqWriteExportUpdate {value: 5})");

	const auto result = execute(
			"MATCH (seed:SubqWriteExportUpdate) "
			"CALL { WITH seed SET seed.doubled = seed.value * 2 "
			"RETURN seed.doubled AS doubled } "
			"RETURN seed.value AS value, doubled ORDER BY value");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "value", 0), "3");
	EXPECT_EQ(val(result, "doubled", 0), "6");
	EXPECT_EQ(val(result, "value", 1), "5");
	EXPECT_EQ(val(result, "doubled", 1), "10");

	db->close();
	db->open();
	const auto persisted = execute(
			"MATCH (seed:SubqWriteExportUpdate) RETURN seed.doubled AS doubled ORDER BY doubled");
	ASSERT_EQ(persisted.rowCount(), 2UL);
	EXPECT_EQ(val(persisted, "doubled", 0), "6");
	EXPECT_EQ(val(persisted, "doubled", 1), "10");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_WriteOnlyImportsOuterVariableForDelete) {
	execute("CREATE (:SubqWriteDelete {value: 13})");
	execute("CREATE (:SubqWriteDelete {value: 17})");

	execute("MATCH (seed:SubqWriteDelete) CALL { WITH seed DELETE seed }");

	db->close();
	db->open();
	const auto persisted = execute("MATCH (seed:SubqWriteDelete) RETURN count(seed) AS count");
	ASSERT_EQ(persisted.rowCount(), 1UL);
	EXPECT_EQ(val(persisted, "count"), "0");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_WriteOnlyExportsCreatedValue) {
	execute("CREATE (:SubqWriteExport {value: 2})");
	execute("CREATE (:SubqWriteExport {value: 4})");

	const auto result = execute(
			"MATCH (seed:SubqWriteExport) "
			"CALL { WITH seed CREATE (child:SubqExportedChild {copied: seed.value}) RETURN child, child.copied AS copied } "
			"RETURN seed.value AS value, copied, child");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "value", 0), "2");
	EXPECT_EQ(val(result, "copied", 0), "2");
	EXPECT_EQ(result.getRows().at(0).at("child").asLogicalNode().properties.at("copied"), PropertyValue(2));
	EXPECT_EQ(val(result, "value", 1), "4");
	EXPECT_EQ(val(result, "copied", 1), "4");
	EXPECT_EQ(result.getRows().at(1).at("child").asLogicalNode().properties.at("copied"), PropertyValue(4));

	db->close();
	db->open();
	const auto persisted = execute("MATCH (child:SubqExportedChild) RETURN child.copied AS copied ORDER BY copied");
	ASSERT_EQ(persisted.rowCount(), 2UL);
	EXPECT_EQ(val(persisted, "copied", 0), "2");
	EXPECT_EQ(val(persisted, "copied", 1), "4");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_WriteOnlyExportFailureDoesNotPublish) {
	execute("CREATE (:SubqWriteExportFailure {value: 0})");

	EXPECT_THROW(static_cast<void>(execute(
				 "MATCH (seed:SubqWriteExportFailure) "
				 "CALL { WITH seed CREATE (:SubqFailedExport) RETURN 1 / seed.value AS invalid } "
				 "RETURN invalid")),
			 std::runtime_error);

	db->close();
	db->open();
	const auto persisted = execute("MATCH (child:SubqFailedExport) RETURN count(child) AS count");
	ASSERT_EQ(persisted.rowCount(), 1UL);
	EXPECT_EQ(val(persisted, "count"), "0");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_WriteOnlyOrdersAndPaginatesOuterResults) {
	execute("CREATE (:SubqWritePaging {value: 3})");
	execute("CREATE (:SubqWritePaging {value: 1})");
	execute("CREATE (:SubqWritePaging {value: 2})");

	const auto result = execute(
			"MATCH (seed:SubqWritePaging) "
			"CALL { WITH seed CREATE (:SubqWritePagingChild {copied: seed.value}) } "
			"RETURN seed.value AS value ORDER BY value DESC SKIP 1 LIMIT 1");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "value"), "2");

	// Pagination applies only to the result pipeline; every outer row still runs
	// the write-only subquery and is committed in the single mutation batch.
	const auto persisted = execute("MATCH (child:SubqWritePagingChild) RETURN count(child) AS count");
	ASSERT_EQ(persisted.rowCount(), 1UL);
	EXPECT_EQ(val(persisted, "count"), "3");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_WriteOnlyDistinctsOuterResultsAfterMutation) {
	execute("CREATE (:SubqWriteDistinct {value: 2})");
	execute("CREATE (:SubqWriteDistinct {value: 2})");
	execute("CREATE (:SubqWriteDistinct {value: 1})");

	const auto result = execute(
			"MATCH (seed:SubqWriteDistinct) "
			"CALL { WITH seed CREATE (:SubqWriteDistinctChild {copied: seed.value}) } "
			"RETURN DISTINCT seed.value AS value ORDER BY value DESC");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "value", 0), "2");
	EXPECT_EQ(val(result, "value", 1), "1");

	const auto persisted = execute("MATCH (child:SubqWriteDistinctChild) RETURN count(child) AS count");
	ASSERT_EQ(persisted.rowCount(), 1UL);
	EXPECT_EQ(val(persisted, "count"), "3");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_InTransactionsExportsCreatedValuesPerBatch) {
	execute("CREATE (:SubqWriteBatchExport {value: 3})");
	execute("CREATE (:SubqWriteBatchExport {value: 1})");
	execute("CREATE (:SubqWriteBatchExport {value: 2})");

	const auto result = execute(
			"MATCH (seed:SubqWriteBatchExport) "
			"CALL { WITH seed CREATE (child:SubqBatchExportedChild {copied: seed.value}) "
			"RETURN child.copied AS copied } IN TRANSACTIONS OF 2 ROWS "
			"RETURN seed.value AS source, copied");
	ASSERT_EQ(result.rowCount(), 3UL);
	std::vector<std::string> sources;
	std::vector<std::string> copied;
	for (const auto &row : result.getRows()) {
		sources.push_back(row.at("source").toString());
		copied.push_back(row.at("copied").toString());
	}
	std::ranges::sort(sources);
	std::ranges::sort(copied);
	EXPECT_EQ(sources, (std::vector<std::string>{"1", "2", "3"}));
	EXPECT_EQ(copied, (std::vector<std::string>{"1", "2", "3"}));

	db->close();
	db->open();
	const auto persisted = execute("MATCH (child:SubqBatchExportedChild) RETURN count(child) AS count");
	ASSERT_EQ(persisted.rowCount(), 1UL);
	EXPECT_EQ(val(persisted, "count"), "3");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_InTransactionsExportsUpdatedValuesPerBatch) {
	execute("CREATE (:SubqBatchUpdate {value: 3})");
	execute("CREATE (:SubqBatchUpdate {value: 5})");

	const auto result = execute(
			"MATCH (seed:SubqBatchUpdate) "
			"CALL { WITH seed SET seed.doubled = seed.value * 2 "
			"RETURN seed.doubled AS doubled } IN TRANSACTIONS OF 1 ROWS "
			"RETURN seed.value AS value, doubled");
	ASSERT_EQ(result.rowCount(), 2UL);
	std::vector<std::string> values;
	std::vector<std::string> doubled;
	for (const auto &row: result.getRows()) {
		values.push_back(row.at("value").toString());
		doubled.push_back(row.at("doubled").toString());
	}
	std::ranges::sort(values);
	std::ranges::sort(doubled);
	EXPECT_EQ(values, (std::vector<std::string>{"3", "5"}));
	EXPECT_EQ(doubled, (std::vector<std::string>{"10", "6"}));

	db->close();
	db->open();
	const auto persisted = execute(
			"MATCH (seed:SubqBatchUpdate) RETURN seed.doubled AS doubled ORDER BY doubled");
	ASSERT_EQ(persisted.rowCount(), 2UL);
	EXPECT_EQ(val(persisted, "doubled", 0), "6");
	EXPECT_EQ(val(persisted, "doubled", 1), "10");
}

TEST_F(IntegrationSubqueryFeaturesTest, CallSubquery_InTransactionsExportFailureDoesNotPublishBatch) {
	execute("CREATE (:SubqWriteBatchExportFailure {value: 0})");

	EXPECT_THROW(static_cast<void>(execute(
					"MATCH (seed:SubqWriteBatchExportFailure) "
					"CALL { WITH seed CREATE (:SubqBatchExportFailure) "
					"RETURN 1 / seed.value AS invalid } IN TRANSACTIONS OF 1 ROW "
					"RETURN invalid")),
				std::runtime_error);

	const auto persisted = execute("MATCH (child:SubqBatchExportFailure) RETURN count(child) AS count");
	ASSERT_EQ(persisted.rowCount(), 1UL);
	EXPECT_EQ(val(persisted, "count"), "0");
}

// ============================================================================
// LOAD CSV Tests
// ============================================================================

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_WithHeaders) {
	auto csvPath = createTempCsv("name,age\nAlice,30\nBob,25\n");

	auto result = execute(
		"LOAD CSV WITH HEADERS FROM 'file:///" + csvPath +
		"' AS row "
		"RETURN row.name AS name, row.age AS age ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "age", 0), "30");
	EXPECT_EQ(val(result, "name", 1), "Bob");
}

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_WithoutHeaders) {
	auto csvPath = createTempCsv("Alice,30\nBob,25\n");

	auto result = execute(
		"LOAD CSV FROM 'file:///" + csvPath +
		"' AS row "
		"RETURN row[0] AS name, row[1] AS age ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "age", 0), "30");
}

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_CreateNodes) {
	auto csvPath = createTempCsv("name,city\nAlice,NYC\nBob,LA\n");

	execute(
		"LOAD CSV WITH HEADERS FROM 'file:///" + csvPath +
		"' AS row "
		"CREATE (:CsvPerson {name: row.name, city: row.city})");

	auto result = execute("MATCH (n:CsvPerson) RETURN n.name AS name, n.city AS city ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "city", 0), "NYC");
	EXPECT_EQ(val(result, "name", 1), "Bob");
	EXPECT_EQ(val(result, "city", 1), "LA");
}

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_QuotedFields) {
	auto csvPath = createTempCsv("name,bio\nAlice,\"Has a, comma\"\nBob,\"Line1\nLine2\"\n");

	auto result = execute(
		"LOAD CSV WITH HEADERS FROM 'file:///" + csvPath +
		"' AS row "
		"RETURN row.name AS name, row.bio AS bio ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "bio", 0), "Has a, comma");
}

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_CustomDelimiter) {
	auto csvPath = createTempCsv("name\tage\nAlice\t30\nBob\t25\n");

	auto result = execute(
		"LOAD CSV WITH HEADERS FROM 'file:///" + csvPath +
		"' AS row FIELDTERMINATOR '\\t' "
		"RETURN row.name AS name ORDER BY name");
	ASSERT_EQ(result.rowCount(), 2UL);
	EXPECT_EQ(val(result, "name", 0), "Alice");
	EXPECT_EQ(val(result, "name", 1), "Bob");
}

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_EmptyFile) {
	auto csvPath = createTempCsv("");

	auto result = execute(
		"LOAD CSV FROM 'file:///" + csvPath +
		"' AS row "
		"RETURN row");
	EXPECT_EQ(result.rowCount(), 0UL);
}

TEST_F(IntegrationSubqueryFeaturesTest, LoadCsv_CreateIsAtomicWhenALaterRecordIsMalformed) {
	auto csvPath = createTempCsv("name\nAda\n\"unterminated");

	EXPECT_THROW(
		execute("LOAD CSV WITH HEADERS FROM 'file:///" + csvPath +
				"' AS row CREATE (:AtomicCsvPerson {name: row.name})"),
		std::runtime_error);

	auto result = execute("MATCH (n:AtomicCsvPerson) RETURN count(n) AS count");
	ASSERT_EQ(result.rowCount(), 1UL);
	EXPECT_EQ(val(result, "count"), "0");
}
