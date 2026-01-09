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
#include <vector>

#include "BenchmarkCases.hpp"
#include "BenchmarkConfig.hpp"

using namespace metrix::benchmark;
namespace conf = config;

// Helper to convert string to lowercase
std::string toLower(std::string s) {
	std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
	return s;
}

int main(int argc, char **argv) {
	// 1. Parse Filter Argument
	std::string filter = "";
	if (argc > 1) {
		filter = toLower(std::string(argv[1]));
	}

	std::cout << "=================================================================\n";
	std::cout << "  MetrixDB Performance Benchmark Suite\n";
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

		Metrics m = t->execute();

		// Adjust for Batch items
		int itemsPerOp = t->getItemsPerOp();
		if (itemsPerOp > 1) {
			m.opsPerSec *= itemsPerOp;
			m.avgLatencyUs /= itemsPerOp;
			m.p99LatencyUs /= itemsPerOp;
		}

		printRow(t->getName(), m);
	}

	std::cout << "=================================================================\n";

	return 0;
}
