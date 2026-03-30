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

#include <CLI/CLI.hpp>

#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "BenchmarkCases.hpp"
#include "BenchmarkConcurrency.hpp"
#include "BenchmarkConfig.hpp"
#include "BenchmarkOptimizer.hpp"
#include "BenchmarkSelection.hpp"
#include "BenchmarkStorage.hpp"
#include "BenchmarkVector.hpp"

using namespace zyx::benchmark;
namespace conf = config;

namespace {

	constexpr int DIM_SMALL = 128;
	constexpr int DIM_LARGE = 768;
	constexpr int CONC_DATA_SIZE = 10000;
	constexpr int CONC_ITER = 5;

	using BenchmarkFactory = std::function<std::unique_ptr<BenchmarkBase>()>;

	struct BenchmarkRegistration {
		std::string id;
		BenchmarkType type;
		std::string displayName;
		BenchmarkFactory factory;
	};

	std::string joinValues(const std::vector<std::string> &values) {
		std::string output;
		for (size_t i = 0; i < values.size(); ++i) {
			output += values[i];
			if (i + 1 < values.size()) {
				output += ", ";
			}
		}
		return output;
	}

	std::vector<BenchmarkRegistration> buildBenchmarkRegistry() {
		std::vector<BenchmarkRegistration> registry;

		auto add = [&](std::string id, const BenchmarkType type, std::string displayName, BenchmarkFactory factory) {
			registry.push_back(BenchmarkRegistration{normalizeToken(std::move(id)), type, std::move(displayName),
																	std::move(factory)});
		};

		auto addInsertTests = [&](const std::string &modeId, const InsertIndexMode mode, const std::string &modeName,
								 const std::string &pathSuffix) {
			const std::string cypherSingleName = "Insert Cypher Single (" + modeName + ")";
			const std::string nativeSingleName = "Insert Native Single (" + modeName + ")";
			const std::string cypherBatchName = "Insert Cypher Batch (" + modeName + ")";
			const std::string nativeBatchName = "Insert Native Batch (" + modeName + ")";
			const std::string cypherSinglePath = std::string(conf::PATH_INSERT_CYPHER) + pathSuffix;
			const std::string nativeSinglePath = std::string(conf::PATH_INSERT_NATIVE) + pathSuffix;
			const std::string cypherBatchPath = std::string(conf::PATH_INSERT_CYPHER) + pathSuffix + "_batch";
			const std::string nativeBatchPath = std::string(conf::PATH_INSERT_NATIVE) + pathSuffix + "_batch";

			add("insert.cypher.single." + modeId, BenchmarkType::INSERT, cypherSingleName,
					[cypherSingleName, cypherSinglePath, mode]() {
						return std::make_unique<CypherInsertBench>(cypherSingleName, cypherSinglePath,
															conf::ITERATIONS_WRITE, conf::DEFAULT_DATA_SIZE, mode);
					});
			add("insert.native.single." + modeId, BenchmarkType::INSERT, nativeSingleName,
					[nativeSingleName, nativeSinglePath, mode]() {
						return std::make_unique<NativeInsertBench>(nativeSingleName, nativeSinglePath,
															conf::ITERATIONS_WRITE, conf::DEFAULT_DATA_SIZE, mode);
					});
			add("insert.cypher.batch." + modeId, BenchmarkType::INSERT, cypherBatchName,
					[cypherBatchName, cypherBatchPath, mode]() {
						return std::make_unique<CypherBatchInsertBench>(cypherBatchName, cypherBatchPath, 1,
															 conf::DEFAULT_DATA_SIZE, mode);
					});
			add("insert.native.batch." + modeId, BenchmarkType::INSERT, nativeBatchName,
					[nativeBatchName, nativeBatchPath, mode]() {
						return std::make_unique<NativeBatchInsertBench>(nativeBatchName, nativeBatchPath, 1,
															 conf::DEFAULT_DATA_SIZE, mode);
					});
		};

		addInsertTests("none", InsertIndexMode::NONE, "No Index", "_none");
		addInsertTests("label", InsertIndexMode::LABEL_ONLY, "Label Index", "_label");
		addInsertTests("prop", InsertIndexMode::PROPERTY_ONLY, "Prop Index", "_prop");
		addInsertTests("all", InsertIndexMode::BOTH, "All Indexes", "_all");

		const std::string queryPropName = "Query (Property Index)";
		const std::string queryLabelName = "Query (Label Scan)";
		const std::string queryFullName = "Query (Full Scan)";
		add("query.cypher.property_index", BenchmarkType::QUERY, queryPropName, [queryPropName]() {
			return std::make_unique<CypherQueryBench>(queryPropName, std::string(conf::PATH_QUERY_BASE) + "_prop",
													  conf::ITERATIONS_READ, conf::DATA_SIZE_QUERY,
													  QueryMode::PROPERTY_INDEX);
		});
		add("query.cypher.label_scan", BenchmarkType::QUERY, queryLabelName, [queryLabelName]() {
			return std::make_unique<CypherQueryBench>(queryLabelName, std::string(conf::PATH_QUERY_BASE) + "_label",
													  conf::ITERATIONS_READ / 5, conf::DATA_SIZE_QUERY,
													  QueryMode::LABEL_SCAN);
		});
		add("query.cypher.full_scan", BenchmarkType::QUERY, queryFullName, [queryFullName]() {
			return std::make_unique<CypherQueryBench>(queryFullName, std::string(conf::PATH_QUERY_BASE) + "_full",
													  conf::ITERATIONS_READ / 20, conf::DATA_SIZE_QUERY,
													  QueryMode::FULL_SCAN);
		});

		const std::string algoCypherName = "Algo: ShortestPath (Cypher)";
		const std::string algoNativeName = "Algo: ShortestPath (Native)";
		add("algo.cypher.shortest_path", BenchmarkType::ALGO, algoCypherName, [algoCypherName]() {
			return std::make_unique<CypherShortestPathBench>(algoCypherName, std::string(conf::PATH_ALGO_CYPHER),
															 conf::ITERATIONS_ALGO, conf::DATA_SIZE_ALGO);
		});
		add("algo.native.shortest_path", BenchmarkType::ALGO, algoNativeName, [algoNativeName]() {
			return std::make_unique<AlgoShortestPathBench>(algoNativeName, std::string(conf::PATH_ALGO_NATIVE),
														   conf::ITERATIONS_ALGO, conf::DATA_SIZE_ALGO);
		});

		const std::string vecInsertNoIndexName = "Vector Insert (No Index, 128d)";
		const std::string vecInsertIdx128Name = "Vector Insert (Indexed, 128d)";
		const std::string vecInsertIdx768Name = "Vector Insert (Indexed, 768d)";
		const std::string vecSearch128Name = "Vector Search (128d, Top-10)";
		const std::string vecSearch768Name = "Vector Search (768d, Top-10)";

		add("vector.insert.no_index.128d", BenchmarkType::VECTOR, vecInsertNoIndexName, [vecInsertNoIndexName]() {
			return std::make_unique<VectorInsertBench>(vecInsertNoIndexName, conf::PATH_VEC_INSERT + "_no_idx",
															  conf::ITERATIONS_WRITE,
															  conf::DEFAULT_DATA_SIZE, false, DIM_SMALL);
		});
		add("vector.insert.indexed.128d", BenchmarkType::VECTOR, vecInsertIdx128Name, [vecInsertIdx128Name]() {
			return std::make_unique<VectorInsertBench>(vecInsertIdx128Name, conf::PATH_VEC_INSERT + "_idx_128",
															  conf::ITERATIONS_WRITE,
															  conf::DEFAULT_DATA_SIZE, true, DIM_SMALL);
		});
		add("vector.insert.indexed.768d", BenchmarkType::VECTOR, vecInsertIdx768Name, [vecInsertIdx768Name]() {
			return std::make_unique<VectorInsertBench>(vecInsertIdx768Name, conf::PATH_VEC_INSERT + "_idx_768",
															  conf::ITERATIONS_WRITE,
															  conf::DEFAULT_DATA_SIZE, true, DIM_LARGE);
		});
		add("vector.search.128d.top10", BenchmarkType::VECTOR, vecSearch128Name, [vecSearch128Name]() {
			return std::make_unique<VectorSearchBench>(vecSearch128Name, conf::PATH_VEC_SEARCH + "_128",
															  conf::ITERATIONS_READ, conf::DATA_SIZE_QUERY,
															  DIM_SMALL, 10);
		});
		add("vector.search.768d.top10", BenchmarkType::VECTOR, vecSearch768Name, [vecSearch768Name]() {
			return std::make_unique<VectorSearchBench>(vecSearch768Name, conf::PATH_VEC_SEARCH + "_768",
															  conf::ITERATIONS_READ, conf::DATA_SIZE_QUERY,
															  DIM_LARGE, 10);
		});

		size_t hwThreads = std::thread::hardware_concurrency();
		if (hwThreads == 0) {
			hwThreads = 4;
		}

		const std::string concQueryOneName = "Conc Query (1 thread)";
		const std::string concQueryManyName = "Conc Query (" + std::to_string(hwThreads) + " threads)";
		const std::string concSortOneName = "Conc Sort (1 thread)";
		const std::string concSortManyName = "Conc Sort (" + std::to_string(hwThreads) + " threads)";
		const std::string concInsertOneName = "Conc BatchInsert (1 thread)";
		const std::string concInsertManyName = "Conc BatchInsert (" + std::to_string(hwThreads) + " threads)";
		const std::string concPqOneName = "Conc PQTrain (1 thread)";
		const std::string concPqManyName = "Conc PQTrain (" + std::to_string(hwThreads) + " threads)";
		const std::string concVecSearchOneName = "Conc VecSearch (1 thread)";
		const std::string concVecSearchManyName = "Conc VecSearch (" + std::to_string(hwThreads) + " threads)";

		add("concurrency.query.single_thread", BenchmarkType::CONCURRENCY, concQueryOneName, [concQueryOneName]() {
			return std::make_unique<ConcurrentQueryBench>(concQueryOneName, "/tmp/zyx_bench_conc_query_1", CONC_ITER,
															  CONC_DATA_SIZE, 1);
		});
		add("concurrency.query.multi_thread", BenchmarkType::CONCURRENCY, concQueryManyName,
				[concQueryManyName, hwThreads]() {
					return std::make_unique<ConcurrentQueryBench>(concQueryManyName, "/tmp/zyx_bench_conc_query_n",
																  CONC_ITER, CONC_DATA_SIZE, hwThreads);
				});
		add("concurrency.sort.single_thread", BenchmarkType::CONCURRENCY, concSortOneName, [concSortOneName]() {
			return std::make_unique<ConcurrentSortBench>(concSortOneName, "/tmp/zyx_bench_conc_sort_1", CONC_ITER,
															 CONC_DATA_SIZE, 1);
		});
		add("concurrency.sort.multi_thread", BenchmarkType::CONCURRENCY, concSortManyName,
				[concSortManyName, hwThreads]() {
					return std::make_unique<ConcurrentSortBench>(concSortManyName, "/tmp/zyx_bench_conc_sort_n",
																 CONC_ITER, CONC_DATA_SIZE, hwThreads);
				});
		add("concurrency.batch_insert.single_thread", BenchmarkType::CONCURRENCY, concInsertOneName,
				[concInsertOneName]() {
					return std::make_unique<ConcurrentBatchInsertBench>(concInsertOneName, "/tmp/zyx_bench_conc_insert_1",
																	5, CONC_DATA_SIZE, 1);
				});
		add("concurrency.batch_insert.multi_thread", BenchmarkType::CONCURRENCY, concInsertManyName,
				[concInsertManyName, hwThreads]() {
					return std::make_unique<ConcurrentBatchInsertBench>(
							concInsertManyName, "/tmp/zyx_bench_conc_insert_n", 5, CONC_DATA_SIZE, hwThreads);
				});
		add("concurrency.pq_train.single_thread", BenchmarkType::CONCURRENCY, concPqOneName, [concPqOneName]() {
			return std::make_unique<ConcurrentPQTrainBench>(concPqOneName, "/tmp/zyx_bench_conc_pq_1", 1, 1000, 1,
															DIM_SMALL);
		});
		add("concurrency.pq_train.multi_thread", BenchmarkType::CONCURRENCY, concPqManyName,
				[concPqManyName, hwThreads]() {
					return std::make_unique<ConcurrentPQTrainBench>(concPqManyName, "/tmp/zyx_bench_conc_pq_n", 1, 1000,
																hwThreads, DIM_SMALL);
				});
		add("concurrency.vector_search.single_thread", BenchmarkType::CONCURRENCY, concVecSearchOneName,
				[concVecSearchOneName]() {
					return std::make_unique<ConcurrentVectorSearchBench>(concVecSearchOneName,
																 "/tmp/zyx_bench_conc_vsearch_1", 5, 1000, 1, DIM_LARGE,
																 10);
				});
		add("concurrency.vector_search.multi_thread", BenchmarkType::CONCURRENCY, concVecSearchManyName,
				[concVecSearchManyName, hwThreads]() {
					return std::make_unique<ConcurrentVectorSearchBench>(
							concVecSearchManyName, "/tmp/zyx_bench_conc_vsearch_n", 5, 1000, hwThreads, DIM_LARGE, 10);
				});

		const std::string concReadOneName = "Conc Read (1 thread)";
		const std::string concReadManyName = "Conc Read (" + std::to_string(hwThreads) + " threads)";

		add("concurrency.read.single_thread", BenchmarkType::CONCURRENCY, concReadOneName, [concReadOneName]() {
			return std::make_unique<ConcurrentReadBench>(concReadOneName, "/tmp/zyx_bench_conc_read_1", CONC_ITER,
															 CONC_DATA_SIZE, 1);
		});
		add("concurrency.read.multi_thread", BenchmarkType::CONCURRENCY, concReadManyName,
				[concReadManyName, hwThreads]() {
					return std::make_unique<ConcurrentReadBench>(concReadManyName, "/tmp/zyx_bench_conc_read_n",
																 CONC_ITER, CONC_DATA_SIZE, hwThreads);
				});

		// --- Optimizer benchmarks ---
		const std::string optFilterPushName = "Optimizer: WHERE→Scan Pushdown";
		const std::string optInlineBaseName = "Optimizer: Inline Property (Baseline)";
		const std::string optAndSplitName = "Optimizer: AND Split+Pushdown";
		const std::string optJoinFilterName = "Optimizer: Join Filter Pushdown";

		add("query.optimizer.filter_pushdown", BenchmarkType::QUERY, optFilterPushName, [optFilterPushName]() {
			return std::make_unique<OptimizerFilterPushdownBench>(
				optFilterPushName, "/tmp/zyx_bench_opt_filter", conf::ITERATIONS_READ, conf::DATA_SIZE_QUERY);
		});
		add("query.optimizer.inline_baseline", BenchmarkType::QUERY, optInlineBaseName, [optInlineBaseName]() {
			return std::make_unique<OptimizerInlineBaselineBench>(
				optInlineBaseName, "/tmp/zyx_bench_opt_inline", conf::ITERATIONS_READ, conf::DATA_SIZE_QUERY);
		});
		add("query.optimizer.and_split", BenchmarkType::QUERY, optAndSplitName, [optAndSplitName]() {
			return std::make_unique<OptimizerAndSplitBench>(
				optAndSplitName, "/tmp/zyx_bench_opt_and", conf::ITERATIONS_READ / 5, conf::DATA_SIZE_QUERY);
		});
		add("query.optimizer.join_filter", BenchmarkType::QUERY, optJoinFilterName, [optJoinFilterName]() {
			return std::make_unique<OptimizerJoinFilterBench>(
				optJoinFilterName, "/tmp/zyx_bench_opt_join", conf::ITERATIONS_READ / 10, 200, 100);
		});

		const std::string implicitTxnName = "Implicit Txn Insert (1000 nodes)";
		const std::string explicitTxnName = "Explicit Txn Insert (1000 nodes)";
		const std::string explicitBatchTxnName = "Explicit Txn Batch Insert (1000 nodes)";

		add("transaction.implicit_insert", BenchmarkType::TRANSACTION, implicitTxnName, [implicitTxnName]() {
			return std::make_unique<ImplicitTxnInsertBench>(implicitTxnName, "/tmp/zyx_bench_txn_implicit", 1, 1000);
		});
		add("transaction.explicit_insert", BenchmarkType::TRANSACTION, explicitTxnName, [explicitTxnName]() {
			return std::make_unique<ExplicitTxnInsertBench>(explicitTxnName, "/tmp/zyx_bench_txn_explicit", 1, 1000);
		});
		add("transaction.explicit_batch_insert", BenchmarkType::TRANSACTION, explicitBatchTxnName,
				[explicitBatchTxnName]() {
					return std::make_unique<ExplicitTxnBatchInsertBench>(explicitBatchTxnName,
																		"/tmp/zyx_bench_txn_explicit_batch", 1, 1000);
				});

		return registry;
	}

	void printTypeCatalog() {
		std::cout << "Supported benchmark types:\n";
		for (const auto &info: BENCHMARK_TYPE_INFOS) {
			std::cout << "  - " << info.token << ": " << info.description << "\n";
		}
		std::cout << "  - all: all benchmark types\n";
	}

	void printBenchmarkCatalog(const std::vector<BenchmarkRegistration> &registry,
							  const std::vector<StorageScenario> &storageScenarios) {
		std::cout << "\nSupported benchmark IDs:\n";
		for (const auto &typeInfo: BENCHMARK_TYPE_INFOS) {
			std::cout << "\n[" << typeInfo.token << "]\n";
			if (typeInfo.type == BenchmarkType::STORAGE) {
				for (const auto &scenario: storageScenarios) {
					std::cout << "  - " << scenario.id << "\n"
							  << "    " << scenario.name << "\n";
				}
				continue;
			}

			for (const auto &registration: registry) {
				if (registration.type != typeInfo.type) {
					continue;
				}
				std::cout << "  - " << registration.id << "\n"
						  << "    " << registration.displayName << "\n";
			}
		}

		std::cout << "\nExamples:\n";
		std::cout << "  ./zyx-bench --type insert\n";
		std::cout << "  ./zyx-bench --type query,vector\n";
		std::cout << "  ./zyx-bench --bench query.cypher.property_index\n";
		std::cout << "  ./zyx-bench --bench storage.vector_index\n";
	}

	std::set<std::string> buildKnownIds(const std::vector<BenchmarkRegistration> &registry,
									const std::vector<StorageScenario> &storageScenarios) {
		std::set<std::string> knownIds;
		for (const auto &registration: registry) {
			knownIds.insert(registration.id);
		}
		for (const auto &scenario: storageScenarios) {
			knownIds.insert(normalizeToken(scenario.id));
		}
		return knownIds;
	}

	std::vector<std::string> selectedTypeTokens(const BenchmarkSelector &selector) {
		std::vector<std::string> tokens;
		for (const auto type: selector.selectedTypes) {
			tokens.push_back(benchmarkTypeToken(type));
		}
		return tokens;
	}

} // namespace

int main(const int argc, char **argv) {
	CLI::App app{"ZYX Performance Benchmark Suite"};
	app.set_help_all_flag("--help-all", "Expand all help");

	std::vector<std::string> typeTokens;
	std::vector<std::string> benchmarkTokens;
	bool listOnly = false;
	bool listTypesOnly = false;

	app.add_option("-t,--type", typeTokens,
				   "Benchmark types (comma-separated or repeated): insert/query/algo/vector/concurrency/storage/transaction/all")
			->delimiter(',');
	app.add_option("-b,--bench", benchmarkTokens,
				   "Exact benchmark ID(s), comma-separated or repeated. Use --list to inspect IDs")
			->delimiter(',');
	app.add_flag("--list", listOnly, "List all benchmark types and benchmark IDs, then exit");
	app.add_flag("--list-types", listTypesOnly, "List supported benchmark types, then exit");

	try {
		app.parse(argc, argv);
	} catch (const CLI::CallForHelp &e) {
		return app.exit(e);
	} catch (const CLI::ParseError &e) {
		return app.exit(e);
	} catch (const std::exception &e) {
		std::cerr << "Runtime Error: " << e.what() << "\n";
		return 1;
	}

	auto registry = buildBenchmarkRegistry();
	const auto storageScenarios = buildStorageScenarios();

	if (listTypesOnly) {
		printTypeCatalog();
		return 0;
	}

	if (listOnly) {
		printTypeCatalog();
		printBenchmarkCatalog(registry, storageScenarios);
		return 0;
	}

	BenchmarkSelector selector;
	bool useAllTypes = false;
	for (const auto &token: typeTokens) {
		const std::string normalized = normalizeToken(token);
		if (normalized.empty()) {
			continue;
		}
		if (normalized == "all" || normalized == "*") {
			useAllTypes = true;
			selector.selectedTypes.clear();
			continue;
		}

		auto parsed = parseBenchmarkType(normalized);
		if (!parsed.has_value()) {
			std::cerr << "Unknown benchmark type: '" << token << "'\n";
			printTypeCatalog();
			return 1;
		}

		if (!useAllTypes) {
			selector.selectedTypes.insert(*parsed);
		}
	}

	for (const auto &token: benchmarkTokens) {
		const std::string normalized = normalizeToken(token);
		if (!normalized.empty()) {
			selector.selectedIds.insert(normalized);
		}
	}

	const auto knownIds = buildKnownIds(registry, storageScenarios);
	std::vector<std::string> unknownIds;
	for (const auto &id: selector.selectedIds) {
		if (!knownIds.contains(id)) {
			unknownIds.push_back(id);
		}
	}
	if (!unknownIds.empty()) {
		std::cerr << "Unknown benchmark ID(s): " << joinValues(unknownIds) << "\n";
		std::cerr << "Use --list to inspect all supported IDs.\n";
		return 1;
	}

	std::cout << "=================================================================\n";
	std::cout << "  ZYX Performance Benchmark Suite\n";
	if (!selector.selectedTypes.empty()) {
		std::cout << "  Types        : " << joinValues(selectedTypeTokens(selector)) << "\n";
	}
	if (!selector.selectedIds.empty()) {
		std::vector<std::string> ids(selector.selectedIds.begin(), selector.selectedIds.end());
		std::cout << "  Bench IDs    : " << joinValues(ids) << "\n";
	}
	if (selector.selectedTypes.empty() && selector.selectedIds.empty()) {
		std::cout << "  Scope        : ALL benchmarks\n";
	}
	std::cout << "  Insert Scale : " << conf::DEFAULT_DATA_SIZE << "\n";
	std::cout << "  Query Scale  : " << conf::DATA_SIZE_QUERY << "\n";
	std::cout << "=================================================================\n";

	std::vector<const BenchmarkRegistration *> selectedBenchmarks;
	selectedBenchmarks.reserve(registry.size());
	for (const auto &registration: registry) {
		if (matchesSelection(selector, registration.type, registration.id, registration.displayName)) {
			selectedBenchmarks.push_back(&registration);
		}
	}

	size_t executed = 0;
	if (!selectedBenchmarks.empty()) {
		printHeader();
		for (const auto *registration: selectedBenchmarks) {
			auto benchmark = registration->factory();
			++executed;
			try {
				Metrics metrics = benchmark->execute();
				const int itemsPerOp = benchmark->getItemsPerOp();
				if (itemsPerOp > 1) {
					metrics.opsPerSec *= itemsPerOp;
					metrics.avgLatencyUs /= itemsPerOp;
					metrics.p99LatencyUs /= itemsPerOp;
				}
				printRow(benchmark->getName(), metrics);
				printStageBreakdown(metrics);
			} catch (const std::exception &e) {
				std::cerr << "   [FAILED] " << benchmark->getName() << ": " << e.what() << "\n";
			} catch (...) {
				std::cerr << "   [CRASH] " << benchmark->getName() << ": Unknown error\n";
			}
		}
	}

	std::cout << "=================================================================\n";

	const auto storageRows = runStorageBenchmarks(selector);
	if (executed == 0 && storageRows.empty()) {
		std::cerr << "[WARN] No benchmark matched current selection. Use --list to inspect supported targets.\n";
		return 1;
	}

	return 0;
}
