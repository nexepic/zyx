/**
 * @file BenchmarkConfig.hpp
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

#pragma once

#include <string_view>

namespace zyx::benchmark::config {

	// --- Data Scale Settings ---
	// How many nodes/edges to create during the 'setup' phase.
	constexpr int DATA_SIZE_SMALL = 100;
	constexpr int DATA_SIZE_MEDIUM = 1000;
	constexpr int DATA_SIZE_LARGE = 10000;

	// Default data size to use if not specified
	constexpr int DEFAULT_DATA_SIZE = DATA_SIZE_SMALL;  // Optimized for faster benchmark execution

	constexpr int DATA_SIZE_QUERY = 500;  // Reduced from 5000 for faster vector search setup

	constexpr int DATA_SIZE_ALGO = 1000;

	// --- Storage Profiling Scale ---
	constexpr int STORAGE_PROFILE_NODE_COUNT = 5000;
	constexpr int STORAGE_PROFILE_LONG_TEXT_NODE_COUNT = 2000;
	constexpr int STORAGE_PROFILE_GRAPH_NODE_COUNT = 3000;
	constexpr int STORAGE_PROFILE_VECTOR_NODE_COUNT = 1200;
	constexpr int STORAGE_PROFILE_VECTOR_DIM = 128;

	// --- Iteration Settings ---
	// How many times to execute the 'run' operation (for averaging latency).
	constexpr int ITERATIONS_WRITE = 200; // Insertions (reduced for faster benchmark)
	constexpr int ITERATIONS_READ = 100; // Queries (reduced for faster benchmark)
	constexpr int ITERATIONS_ALGO = 100; // Graph Algorithms

	constexpr int BATCH_WRITE_SIZE = 100; // For batch insertion benchmarks

	// --- Path Settings ---
	// Base paths for temporary databases
	constexpr std::string_view PATH_INSERT_CYPHER = "/tmp/zyx_bench_insert_cypher";
	constexpr std::string_view PATH_INSERT_NATIVE = "/tmp/zyx_bench_insert_native";
	constexpr std::string_view PATH_QUERY_INDEX = "/tmp/zyx_bench_query_index";
	constexpr std::string_view PATH_QUERY_BASE = "/tmp/zyx_bench_query_base";
	constexpr std::string_view PATH_ALGO_CYPHER = "/tmp/zyx_bench_algo_cypher";
	constexpr std::string_view PATH_ALGO_NATIVE = "/tmp/zyx_bench_algo_native";
	constexpr std::string_view PATH_STORAGE_BASE = "/tmp/zyx_bench_storage";
	constexpr std::string_view PATH_STORAGE_CSV = "/tmp/zyx_storage_profile.csv";

	const std::string PATH_VEC_INSERT = "/tmp/zyx_bench_vec_insert";
	const std::string PATH_VEC_SEARCH = "/tmp/zyx_bench_vec_search";

} // namespace zyx::benchmark::config
