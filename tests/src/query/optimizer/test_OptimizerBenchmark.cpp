/**
 * @file test_OptimizerBenchmark.cpp
 * @brief Benchmark tests measuring query optimizer performance impact.
 *
 * Measures execution time for various query patterns to evaluate
 * the effectiveness of the query optimizer on different workloads.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace fs = std::filesystem;
using namespace graph;

/**
 * Benchmark test fixture for query optimizer performance measurement.
 *
 * Uses SetUpTestSuite/TearDownTestSuite so the database and test data
 * are created only once for all test cases in the suite.
 *
 * Data set:
 * - 100 Person nodes with name, age, city, category properties
 * - 60 Company nodes with name, size, industry properties
 * - 60 Product nodes with name, price, category properties
 * - 100 WORKS_AT edges (Person -> Company)
 * - ~200 BOUGHT edges (Person -> Product)
 */
class OptimizerBenchmarkTest : public ::testing::Test {
protected:
	struct BenchResult {
		double executionTimeMs;
		size_t rowCount;
	};

	static void SetUpTestSuite() {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		testDbPath_ = fs::temp_directory_path() / ("test_optbench_" + boost::uuids::to_string(uuid) + ".zyx");
		if (fs::exists(testDbPath_))
			fs::remove_all(testDbPath_);

		db_ = std::make_unique<Database>(testDbPath_.string());
		db_->open();

		populateTestData();
	}

	static void TearDownTestSuite() {
		if (db_)
			db_->close();
		db_.reset();
		std::error_code ec;
		if (fs::exists(testDbPath_))
			fs::remove_all(testDbPath_, ec);
	}

	static void populateTestData() {
		const std::vector<std::string> cities = {"NYC", "LA", "Chicago", "Houston", "Phoenix"};
		const std::vector<std::string> categories = {"A", "B", "C", "D", "E"};
		const std::vector<std::string> industries = {"Tech", "Finance", "Healthcare", "Retail", "Energy"};
		const auto writer = db_->getLogicalGraphWriteBackend();
		if (!writer) {
			throw std::runtime_error("optimizer benchmark requires the native logical write backend");
		}

		// This benchmark measures query planning/execution, not individual write
		// latency.  Batch the deterministic fixture through the production logical
		// boundary so each GoogleTest shard does not repeatedly rescan it while
		// constructing the same graph.
		storage::LogicalGraphMutationBatch batch;
		batch.node_creates.reserve(220U);
		batch.edge_creates.reserve(300U);
		for (int i = 0; i < 100; ++i) {
			batch.node_creates.push_back({{"Person"},
									  {{"name", PropertyValue("Person_" + std::to_string(i))},
									   {"age", PropertyValue(int64_t{20 + (i % 50)})},
									   {"city", PropertyValue(cities[static_cast<size_t>(i) % cities.size()])},
									   {"category", PropertyValue(categories[static_cast<size_t>(i) % categories.size()])}}});
		}

		for (int i = 0; i < 60; ++i) {
			batch.node_creates.push_back({{"Company"},
									  {{"name", PropertyValue("Company_" + std::to_string(i))},
									   {"size", PropertyValue(int64_t{10 + (i % 200)})},
									   {"industry", PropertyValue(industries[static_cast<size_t>(i) % industries.size()])}}});
		}

		for (int i = 0; i < 60; ++i) {
			batch.node_creates.push_back({{"Product"},
									  {{"name", PropertyValue("Product_" + std::to_string(i))},
									   {"price", PropertyValue(int64_t{5 + (i % 100)})},
									   {"category", PropertyValue(categories[static_cast<size_t>(i) % categories.size()])}}});
		}

		const auto createdNode = [](uint64_t ordinal) {
			return storage::LogicalGraphMutationEntityReference{storage::LogicalGraphMutationEntityKind::LGMK_NODE,
															 storage::LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, ordinal};
		};
		constexpr uint64_t companyOffset = 100U;
		constexpr uint64_t productOffset = 160U;
		for (int i = 0; i < 100; ++i) {
			batch.edge_creates.push_back(
					{createdNode(static_cast<uint64_t>(i)), createdNode(companyOffset + static_cast<uint64_t>(i % 60)),
					 "WORKS_AT", {{"since", PropertyValue(int64_t{2000 + (i % 25)})}}});
		}

		for (int i = 0; i < 100; ++i) {
			for (int j = 0; j < 2; ++j) {
				batch.edge_creates.push_back(
						{createdNode(static_cast<uint64_t>(i)),
						 createdNode(productOffset + static_cast<uint64_t>((i * 2 + j) % 60)),
						 "BOUGHT", {{"quantity", PropertyValue(int64_t{1 + (i % 5)})}}});
			}
		}

		const storage::LogicalGraphMutationBatchResult result = writer->executeLogicalGraphMutationBatch(batch);
		if (result.created_node_ids.size() != batch.node_creates.size() ||
			result.created_edge_ids.size() != batch.edge_creates.size()) {
			throw std::runtime_error("optimizer benchmark created an unexpected logical entity ID count");
		}
	}

	static query::QueryResult exec(const std::string &cypher) {
		return db_->getQueryEngine()->execute(cypher);
	}

	/**
	 * Execute a query and return timing and row count (single run).
	 */
	BenchResult timeQuery(const std::string &cypher) {
		auto start = std::chrono::high_resolution_clock::now();
		auto result = exec(cypher);
		auto end = std::chrono::high_resolution_clock::now();

		double ms = std::chrono::duration<double, std::milli>(end - start).count();
		return {ms, result.rowCount()};
	}

	/**
	 * Report benchmark results to stderr and gtest RecordProperty.
	 */
	void reportResult(const std::string &label, const BenchResult &result) {
		std::cerr << "[BENCHMARK] " << label
				  << ": " << std::fixed << std::setprecision(3) << result.executionTimeMs << " ms"
				  << " (" << result.rowCount << " rows)" << std::endl;
		RecordProperty(label + "_ms", std::to_string(result.executionTimeMs));
		RecordProperty(label + "_rows", std::to_string(result.rowCount));
	}

	static fs::path testDbPath_;
	static std::unique_ptr<Database> db_;
};

fs::path OptimizerBenchmarkTest::testDbPath_;
std::unique_ptr<Database> OptimizerBenchmarkTest::db_;

// =============================================================================
// Benchmark 1: Filter Pushdown
// =============================================================================

/**
 * Broad filter: age > 25 matches most Person nodes.
 */
TEST_F(OptimizerBenchmarkTest, FilterPushdown_ManyResults) {
	auto result = timeQuery("MATCH (n:Person) WHERE n.age > 25 RETURN n.name, n.age");
	reportResult("FilterPushdown_ManyResults", result);

	EXPECT_GT(result.rowCount, 0UL);
	EXPECT_LE(result.rowCount, 100UL);
}

/**
 * Selective filter: age = 69 matches very few Person nodes.
 */
TEST_F(OptimizerBenchmarkTest, FilterPushdown_FewResults) {
	auto result = timeQuery("MATCH (n:Person) WHERE n.age = 69 RETURN n.name, n.age");
	reportResult("FilterPushdown_FewResults", result);

	EXPECT_LE(result.rowCount, 100UL);
}

/**
 * String equality filter on city property.
 */
TEST_F(OptimizerBenchmarkTest, FilterPushdown_StringEquality) {
	auto result = timeQuery("MATCH (n:Person) WHERE n.city = 'NYC' RETURN n.name");
	reportResult("FilterPushdown_StringEquality", result);

	// NYC assigned to every 5th person -> ~20 rows
	EXPECT_GT(result.rowCount, 0UL);
}

// =============================================================================
// Benchmark 2: Multi-property Match
// =============================================================================

/**
 * Inline property matching with two properties.
 */
TEST_F(OptimizerBenchmarkTest, MultiPropertyMatch) {
	auto result = timeQuery(
		"MATCH (n:Person {city: 'LA', category: 'B'}) RETURN n.name, n.age");
	reportResult("MultiPropertyMatch", result);

	EXPECT_LE(result.rowCount, 100UL);
}

/**
 * Inline property matching with three properties (highly selective).
 */
TEST_F(OptimizerBenchmarkTest, MultiPropertyMatch_ThreeProperties) {
	auto result = timeQuery(
		"MATCH (n:Person {city: 'Chicago', category: 'C', age: 22}) RETURN n.name");
	reportResult("MultiPropertyMatch_ThreeProperties", result);

	EXPECT_LE(result.rowCount, 100UL);
}

// =============================================================================
// Benchmark 3: Traversal with Target Label Filter
// =============================================================================

/**
 * Basic traversal with typed relationship and target label.
 */
TEST_F(OptimizerBenchmarkTest, TraversalWithTargetLabel) {
	auto result = timeQuery(
		"MATCH (a:Person)-[r:WORKS_AT]->(b:Company) RETURN a.name, b.name");
	reportResult("TraversalWithTargetLabel", result);

	EXPECT_GT(result.rowCount, 0UL);
}

/**
 * Traversal with filter on target node property.
 */
TEST_F(OptimizerBenchmarkTest, TraversalWithTargetLabelAndFilter) {
	auto result = timeQuery(
		"MATCH (a:Person)-[r:WORKS_AT]->(b:Company) "
		"WHERE b.industry = 'Tech' RETURN a.name, b.name");
	reportResult("TraversalWithTargetLabelAndFilter", result);

	EXPECT_GT(result.rowCount, 0UL);
}

/**
 * Traversal with filters on both source and target nodes.
 */
TEST_F(OptimizerBenchmarkTest, TraversalWithSourceAndTargetFilter) {
	auto result = timeQuery(
		"MATCH (a:Person)-[r:BOUGHT]->(b:Product) "
		"WHERE a.city = 'NYC' AND b.category = 'A' "
		"RETURN a.name, b.name");
	reportResult("TraversalWithSourceAndTargetFilter", result);

	EXPECT_GE(result.rowCount, 0UL);
}

// =============================================================================
// Benchmark 4: Aggregation with Group By
// =============================================================================

/**
 * Count aggregation grouped by city.
 */
TEST_F(OptimizerBenchmarkTest, AggregationCountGroupBy) {
	auto result = timeQuery(
		"MATCH (n:Person) RETURN n.city, count(n) AS cnt");
	reportResult("AggregationCountGroupBy", result);

	// 5 distinct cities
	EXPECT_EQ(result.rowCount, 5UL);
}

/**
 * Sum aggregation grouped by category.
 */
TEST_F(OptimizerBenchmarkTest, AggregationSumGroupBy) {
	auto result = timeQuery(
		"MATCH (n:Product) RETURN n.category, sum(n.price) AS totalPrice");
	reportResult("AggregationSumGroupBy", result);

	// 5 distinct categories
	EXPECT_EQ(result.rowCount, 5UL);
}

/**
 * Multiple aggregate functions (count, min, max) grouped by category.
 */
TEST_F(OptimizerBenchmarkTest, AggregationMultipleFunctions) {
	auto result = timeQuery(
		"MATCH (n:Person) RETURN n.category, count(n) AS cnt, min(n.age) AS minAge, max(n.age) AS maxAge");
	reportResult("AggregationMultipleFunctions", result);

	EXPECT_EQ(result.rowCount, 5UL);
}

// =============================================================================
// Benchmark 5: Large Result with LIMIT
// =============================================================================

/**
 * LIMIT 10 on a scan across all node labels.
 */
TEST_F(OptimizerBenchmarkTest, LargeResultWithLimit) {
	auto result = timeQuery("MATCH (n) RETURN n LIMIT 10");
	reportResult("LargeResultWithLimit", result);

	EXPECT_EQ(result.rowCount, 10UL);
}

/**
 * LIMIT 1 on a label scan.
 */
TEST_F(OptimizerBenchmarkTest, LargeResultWithSmallLimit) {
	auto result = timeQuery("MATCH (n:Person) RETURN n.name LIMIT 1");
	reportResult("LargeResultWithSmallLimit", result);

	EXPECT_EQ(result.rowCount, 1UL);
}

/**
 * ORDER BY with LIMIT: requires full scan then sort then truncation.
 */
TEST_F(OptimizerBenchmarkTest, LargeResultWithLimitAndOrder) {
	auto result = timeQuery(
		"MATCH (n:Person) RETURN n.name, n.age ORDER BY n.age DESC LIMIT 5");
	reportResult("LargeResultWithLimitAndOrder", result);

	EXPECT_EQ(result.rowCount, 5UL);
}

// =============================================================================
// Benchmark 6: Combined patterns
// =============================================================================

/**
 * Combined traversal with filters on both ends and the relationship.
 */
TEST_F(OptimizerBenchmarkTest, CombinedFilterTraversal) {
	auto result = timeQuery(
		"MATCH (p:Person)-[:WORKS_AT]->(c:Company) "
		"WHERE p.age > 30 AND c.industry = 'Finance' "
		"RETURN p.name, c.name");
	reportResult("CombinedFilterTraversal", result);

	EXPECT_GE(result.rowCount, 0UL);
}

/**
 * Baseline: full label scan without any filter, for comparison.
 */
TEST_F(OptimizerBenchmarkTest, FullScanNoFilter) {
	auto result = timeQuery("MATCH (n:Person) RETURN n.name");
	reportResult("FullScanNoFilter", result);

	EXPECT_EQ(result.rowCount, 100UL);
}

// =============================================================================
// Summary report
// =============================================================================

/**
 * Runs all benchmark patterns and prints a compact timing summary.
 */
TEST_F(OptimizerBenchmarkTest, TimingSummary) {
	struct BenchCase {
		std::string label;
		std::string query;
	};

	std::vector<BenchCase> cases = {
		{"FullScan_100",        "MATCH (n:Person) RETURN n.name"},
		{"Filter_Selective",    "MATCH (n:Person) WHERE n.age = 69 RETURN n.name"},
		{"Filter_Broad",        "MATCH (n:Person) WHERE n.age > 25 RETURN n.name"},
		{"MultiProp_2",         "MATCH (n:Person {city: 'LA', category: 'B'}) RETURN n.name"},
		{"Traversal_WORKS_AT",  "MATCH (a:Person)-[:WORKS_AT]->(b:Company) RETURN a.name, b.name"},
		{"Traversal_Filtered",  "MATCH (a:Person)-[:WORKS_AT]->(b:Company) WHERE b.industry = 'Tech' RETURN a.name"},
		{"Agg_CountByCity",     "MATCH (n:Person) RETURN n.city, count(n) AS cnt"},
		{"Limit_10",            "MATCH (n) RETURN n LIMIT 10"},
		{"Limit_1",             "MATCH (n:Person) RETURN n.name LIMIT 1"},
		{"OrderLimit_5",        "MATCH (n:Person) RETURN n.name ORDER BY n.age DESC LIMIT 5"},
	};

	std::cerr << "\n====== OPTIMIZER BENCHMARK SUMMARY ======" << std::endl;

	for (const auto &bc : cases) {
		auto result = timeQuery(bc.query);
		std::cerr << "  " << std::left << std::setw(25) << bc.label
				  << std::right << std::setw(10) << std::fixed << std::setprecision(3)
				  << result.executionTimeMs << " ms"
				  << "  (" << result.rowCount << " rows)" << std::endl;
	}

	std::cerr << "=========================================" << std::endl;

	SUCCEED();
}
