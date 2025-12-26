/**
 * @file BenchmarkConfig.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string_view>

namespace metrix::benchmark::config {

	// --- Data Scale Settings ---
	// How many nodes/edges to create during the 'setup' phase.
	constexpr int DATA_SIZE_SMALL = 100;
	constexpr int DATA_SIZE_MEDIUM = 1000;
	constexpr int DATA_SIZE_LARGE = 10000;

	// Default data size to use if not specified
	constexpr int DEFAULT_DATA_SIZE = DATA_SIZE_MEDIUM;

	constexpr int DATA_SIZE_QUERY   = 5000;

	constexpr int DATA_SIZE_ALGO    = 1000;

	// --- Iteration Settings ---
	// How many times to execute the 'run' operation (for averaging latency).
	constexpr int ITERATIONS_WRITE = 1000; // Insertions
	constexpr int ITERATIONS_READ = 500; // Queries
	constexpr int ITERATIONS_ALGO = 100; // Graph Algorithms

	// --- Path Settings ---
	// Base paths for temporary databases
	constexpr std::string_view PATH_INSERT_CYPHER = "/tmp/metrix_bench_insert_cypher";
	constexpr std::string_view PATH_INSERT_NATIVE = "/tmp/metrix_bench_insert_native";
	constexpr std::string_view PATH_QUERY_INDEX = "/tmp/metrix_bench_query_index";
	constexpr std::string_view PATH_ALGO_CYPHER = "/tmp/metrix_bench_algo_cypher";
	constexpr std::string_view PATH_ALGO_NATIVE = "/tmp/metrix_bench_algo_native";

} // namespace metrix::benchmark::config
