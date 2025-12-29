/**
 * @file main.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <iostream>
#include <vector>
#include <memory>
#include <string>

#include "BenchmarkCases.hpp"
#include "BenchmarkConfig.hpp"

using namespace metrix::benchmark;
namespace conf = config;

int main() {
    // Force disable debug logging to ensure accurate measurements
    // metrix::graph::log::Log::setDebug(false);

    std::cout << "=================================================================\n";
    std::cout << "  Metrix Database Performance Benchmark\n";
    std::cout << "  Default Data Scale: " << conf::DEFAULT_DATA_SIZE << " entities\n";
    std::cout << "=================================================================\n";

    std::vector<std::unique_ptr<BenchmarkBase>> tests;

    // ========================================================================
    // 1. Insertion Benchmarks (Write Throughput)
    // ========================================================================
	constexpr int TOTAL_ITEMS_TO_INSERT = conf::ITERATIONS_WRITE;

	tests.push_back(std::make_unique<CypherInsertBench>(
		"Insert (Cypher)",
		std::string(conf::PATH_INSERT_CYPHER),
		TOTAL_ITEMS_TO_INSERT,
		conf::DEFAULT_DATA_SIZE
	));

	tests.push_back(std::make_unique<NativeInsertBench>(
		"Insert (Native)",
		std::string(conf::PATH_INSERT_NATIVE),
		TOTAL_ITEMS_TO_INSERT,
		conf::DEFAULT_DATA_SIZE
	));

	constexpr int BATCH_ITERATIONS = TOTAL_ITEMS_TO_INSERT / conf::BATCH_WRITE_SIZE;

	tests.push_back(std::make_unique<CypherBatchInsertBench>(
		"Insert (Batch Cypher UNWIND)",
		std::string(conf::PATH_INSERT_CYPHER) + "_batch",
		BATCH_ITERATIONS,
		conf::BATCH_WRITE_SIZE
	));

	tests.push_back(std::make_unique<NativeBatchInsertBench>(
		"Insert (Batch Native API)",
		std::string(conf::PATH_INSERT_NATIVE) + "_batch",
		BATCH_ITERATIONS,
		conf::BATCH_WRITE_SIZE
	));

    // ========================================================================
    // 2. Query Benchmarks (Read Latency & Index Efficiency)
    //    We use a larger data set (50k) here to make O(N) vs O(log N) obvious.
    // ========================================================================

    // Case A: Property Index Scan (Expected: O(log N) -> Fastest)
    // Uses B-Tree to find exact ID.
    tests.push_back(std::make_unique<CypherQueryBench>(
        "Query (Property Index)",
        std::string(conf::PATH_QUERY_INDEX) + "_prop",
        conf::ITERATIONS_READ,
        conf::DATA_SIZE_QUERY,
        QueryMode::PROPERTY_INDEX
    ));

    // Case B: Label Index Scan (Expected: O(N_label) -> Medium)
    // Uses Label Index to filter nodes, then checks property.
    // We reduce iterations because scanning 50k nodes repeatedly takes time.
    tests.push_back(std::make_unique<CypherQueryBench>(
        "Query (Label Scan)",
        std::string(conf::PATH_QUERY_INDEX) + "_label",
        conf::ITERATIONS_READ / 10,
        conf::DATA_SIZE_QUERY,
        QueryMode::LABEL_SCAN
    ));

    // Case C: Full Scan (Expected: O(N_total) -> Slowest)
    // Scans every node in the database.
    tests.push_back(std::make_unique<CypherQueryBench>(
        "Query (Full Scan)",
        std::string(conf::PATH_QUERY_INDEX) + "_full",
        conf::ITERATIONS_READ / 10,
        conf::DATA_SIZE_QUERY,
        QueryMode::FULL_SCAN
    ));

    // ========================================================================
    // 3. Algorithm Benchmarks (Compute / Traversal)
    // ========================================================================
    // Graph algorithms perform many random accesses. We use a smaller dataset here
    // to keep the benchmark runtime reasonable while still testing logic.

    tests.push_back(std::make_unique<CypherShortestPathBench>(
        "Algo: ShortestPath (Cypher)",
        std::string(conf::PATH_ALGO_CYPHER),
        conf::ITERATIONS_ALGO,
        conf::DATA_SIZE_ALGO
    ));

    tests.push_back(std::make_unique<AlgoShortestPathBench>(
        "Algo: ShortestPath (Native)",
        std::string(conf::PATH_ALGO_NATIVE),
        conf::ITERATIONS_ALGO,
        conf::DATA_SIZE_ALGO
    ));

    // ========================================================================
    // Execution Loop
    // ========================================================================
    printHeader();

	for (auto& t : tests) {
		Metrics m = t->execute();

		// Use the test's self-reported batch size.
		// If it's a single op test, getItemsPerOp returns 1.
		int itemsPerOp = t->getItemsPerOp();

		if (itemsPerOp > 1) {
			// Convert Batches/Sec -> Items/Sec
			m.opsPerSec *= itemsPerOp;

			// Convert Latency/Batch -> Latency/Item
			m.avgLatencyUs /= itemsPerOp;
			m.p99LatencyUs /= itemsPerOp;
		}

		printRow(t->getName(), m);
	}

    std::cout << "=================================================================\n";

    return 0;
}