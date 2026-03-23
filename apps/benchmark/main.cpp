/**
 * @file main.cpp
 * @author Nexepic
 * @date 2025/12/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "BenchmarkCases.hpp"
#include "BenchmarkConcurrency.hpp"
#include "BenchmarkConfig.hpp"
#include "BenchmarkVector.hpp"

using namespace zyx::benchmark;
namespace conf = config;

// Helper to convert string to lowercase
std::string toLower(std::string s) {
	std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
	return s;
}

int main(int argc, char **argv) {
	// 1. Parse Filter Argument
	std::string filter;
	if (argc > 1) {
		filter = toLower(std::string(argv[1]));
	}

	std::cout << "=================================================================\n";
	std::cout << "  ZYX Performance Benchmark Suite\n";
	if (!filter.empty()) {
		std::cout << "  Running tests matching: '" << filter << "'\n";
	} else {
		std::cout << "  Running ALL tests\n";
	}
	std::cout << "  Insert Scale : " << conf::DEFAULT_DATA_SIZE << "\n";
	std::cout << "  Query Scale  : " << conf::DATA_SIZE_QUERY << "\n";
	std::cout << "=================================================================\n";

	std::vector<std::unique_ptr<BenchmarkBase>> tests;

	// Helper to register insert tests
	auto addInsertTests = [&](const std::string &prefix, InsertIndexMode mode, const std::string &modeName) {
		// Single Insert
		tests.push_back(std::make_unique<CypherInsertBench>("Insert Cypher Single (" + modeName + ")",
															std::string(conf::PATH_INSERT_CYPHER) + prefix,
															conf::ITERATIONS_WRITE, conf::DEFAULT_DATA_SIZE, mode));
		tests.push_back(std::make_unique<NativeInsertBench>("Insert Native Single (" + modeName + ")",
															std::string(conf::PATH_INSERT_NATIVE) + prefix,
															conf::ITERATIONS_WRITE, conf::DEFAULT_DATA_SIZE, mode));

		// Batch Insert
		tests.push_back(std::make_unique<CypherBatchInsertBench>(
				"Insert Cypher Batch (" + modeName + ")", std::string(conf::PATH_INSERT_CYPHER) + prefix + "_batch", 1,
				conf::DEFAULT_DATA_SIZE, mode));
		tests.push_back(std::make_unique<NativeBatchInsertBench>(
				"Insert Native Batch (" + modeName + ")", std::string(conf::PATH_INSERT_NATIVE) + prefix + "_batch", 1,
				conf::DEFAULT_DATA_SIZE, mode));
	};

	// --- 1. Register Insert Tests ---
	addInsertTests("_none", InsertIndexMode::NONE, "No Index");
	addInsertTests("_label", InsertIndexMode::LABEL_ONLY, "Label Index");
	addInsertTests("_prop", InsertIndexMode::PROPERTY_ONLY, "Prop Index");
	addInsertTests("_all", InsertIndexMode::BOTH, "All Indexes");

	// --- 2. Register Query Tests ---
	tests.push_back(std::make_unique<CypherQueryBench>(
			"Query (Property Index)", std::string(conf::PATH_QUERY_BASE) + "_prop", conf::ITERATIONS_READ,
			conf::DATA_SIZE_QUERY, QueryMode::PROPERTY_INDEX));

	tests.push_back(std::make_unique<CypherQueryBench>(
			"Query (Label Scan)", std::string(conf::PATH_QUERY_BASE) + "_label", conf::ITERATIONS_READ / 5,
			conf::DATA_SIZE_QUERY, QueryMode::LABEL_SCAN));

	tests.push_back(std::make_unique<CypherQueryBench>(
			"Query (Full Scan)", std::string(conf::PATH_QUERY_BASE) + "_full", conf::ITERATIONS_READ / 20,
			conf::DATA_SIZE_QUERY, QueryMode::FULL_SCAN));

	// --- 3. Register Algo Tests ---
	tests.push_back(std::make_unique<CypherShortestPathBench>("Algo: ShortestPath (Cypher)",
															  std::string(conf::PATH_ALGO_CYPHER),
															  conf::ITERATIONS_ALGO, conf::DATA_SIZE_ALGO));

	tests.push_back(std::make_unique<AlgoShortestPathBench>("Algo: ShortestPath (Native)",
															std::string(conf::PATH_ALGO_NATIVE), conf::ITERATIONS_ALGO,
															conf::DATA_SIZE_ALGO));

	// --- 4. Register Vector Tests ---

	// Dimensions to test
	constexpr int DIM_SMALL = 128; // Typical embedding size (small)
	constexpr int DIM_LARGE = 768; // BERT/LLM embedding size

	// A. Vector Insertion (No Index vs Index)
	tests.push_back(std::make_unique<VectorInsertBench>("Vector Insert (No Index, 128d)",
														config::PATH_VEC_INSERT + "_no_idx", conf::ITERATIONS_WRITE,
														conf::DEFAULT_DATA_SIZE, false, DIM_SMALL));

	tests.push_back(std::make_unique<VectorInsertBench>("Vector Insert (Indexed, 128d)",
														config::PATH_VEC_INSERT + "_idx_128", conf::ITERATIONS_WRITE,
														conf::DEFAULT_DATA_SIZE, true, DIM_SMALL));

	tests.push_back(std::make_unique<VectorInsertBench>("Vector Insert (Indexed, 768d)",
														config::PATH_VEC_INSERT + "_idx_768", conf::ITERATIONS_WRITE,
														conf::DEFAULT_DATA_SIZE, true, DIM_LARGE));

	// B. Vector Search
	// Scale: Use DATA_SIZE_QUERY (5000) or larger if you want to test PQ speedup
	tests.push_back(std::make_unique<VectorSearchBench>("Vector Search (128d, Top-10)",
														config::PATH_VEC_SEARCH + "_128", conf::ITERATIONS_READ,
														conf::DATA_SIZE_QUERY, DIM_SMALL, 10));

	tests.push_back(std::make_unique<VectorSearchBench>("Vector Search (768d, Top-10)",
														config::PATH_VEC_SEARCH + "_768", conf::ITERATIONS_READ,
														conf::DATA_SIZE_QUERY, DIM_LARGE, 10));

	// --- 5. Register Concurrency Tests ---
	// Each test runs twice: single-threaded (baseline) vs multi-threaded
	size_t hwThreads = std::thread::hardware_concurrency();
	if (hwThreads == 0)
		hwThreads = 4;

	constexpr int CONC_DATA_SIZE = 10000;
	constexpr int CONC_ITER = 5;

	// 5A. Parallel Query (NodeScan + Filter)
	tests.push_back(std::make_unique<ConcurrentQueryBench>("Conc Query (1 thread)",
														   "/tmp/zyx_bench_conc_query_1", CONC_ITER, CONC_DATA_SIZE, 1));
	tests.push_back(std::make_unique<ConcurrentQueryBench>("Conc Query (" + std::to_string(hwThreads) + " threads)",
														   "/tmp/zyx_bench_conc_query_n", CONC_ITER, CONC_DATA_SIZE,
														   hwThreads));

	// 5B. Parallel Sort
	tests.push_back(std::make_unique<ConcurrentSortBench>("Conc Sort (1 thread)", "/tmp/zyx_bench_conc_sort_1",
														  CONC_ITER, CONC_DATA_SIZE, 1));
	tests.push_back(std::make_unique<ConcurrentSortBench>("Conc Sort (" + std::to_string(hwThreads) + " threads)",
														  "/tmp/zyx_bench_conc_sort_n", CONC_ITER, CONC_DATA_SIZE,
														  hwThreads));

	// 5C. Parallel Batch Insert + Save
	tests.push_back(std::make_unique<ConcurrentBatchInsertBench>(
			"Conc BatchInsert (1 thread)", "/tmp/zyx_bench_conc_insert_1", 5, CONC_DATA_SIZE, 1));
	tests.push_back(std::make_unique<ConcurrentBatchInsertBench>(
			"Conc BatchInsert (" + std::to_string(hwThreads) + " threads)", "/tmp/zyx_bench_conc_insert_n", 5,
			CONC_DATA_SIZE, hwThreads));

	// 5D. Parallel PQ Training
	tests.push_back(std::make_unique<ConcurrentPQTrainBench>("Conc PQTrain (1 thread)",
															 "/tmp/zyx_bench_conc_pq_1", 1, 1000, 1, DIM_SMALL));
	tests.push_back(std::make_unique<ConcurrentPQTrainBench>(
			"Conc PQTrain (" + std::to_string(hwThreads) + " threads)", "/tmp/zyx_bench_conc_pq_n", 1, 1000,
			hwThreads, DIM_SMALL));

	// 5E. Parallel Vector Search
	tests.push_back(std::make_unique<ConcurrentVectorSearchBench>(
			"Conc VecSearch (1 thread)", "/tmp/zyx_bench_conc_vsearch_1", 5, 1000, 1, DIM_SMALL, 10));
	tests.push_back(std::make_unique<ConcurrentVectorSearchBench>(
			"Conc VecSearch (" + std::to_string(hwThreads) + " threads)", "/tmp/zyx_bench_conc_vsearch_n", 5, 1000,
			hwThreads, DIM_SMALL, 10));

	// ========================================================================
	// Execution Loop with Filter
	// ========================================================================
	printHeader();

	for (auto &t: tests) {
		// [FILTER LOGIC]
		if (!filter.empty()) {
			std::string nameLower = toLower(t->getName());
			// If name doesn't contain filter string, skip
			if (nameLower.find(filter) == std::string::npos) {
				continue;
			}
		}

		try {
			Metrics m = t->execute();

			// Adjust for Batch items
			int itemsPerOp = t->getItemsPerOp();
			if (itemsPerOp > 1) {
				m.opsPerSec *= itemsPerOp;
				m.avgLatencyUs /= itemsPerOp;
				m.p99LatencyUs /= itemsPerOp;
			}

			printRow(t->getName(), m);
		} catch (const std::exception &e) {
			std::cerr << "   [FAILED] " << t->getName() << ": " << e.what() << "\n";
		} catch (...) {
			std::cerr << "   [CRASH] " << t->getName() << ": Unknown error\n";
		}
	}

	std::cout << "=================================================================\n";

	return 0;
}
