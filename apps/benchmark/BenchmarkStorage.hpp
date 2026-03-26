/**
 * @file BenchmarkStorage.hpp
 * @author Nexepic
 * @date 2026/3/25
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "BenchmarkConfig.hpp"
#include "BenchmarkFramework.hpp"
#include "BenchmarkSelection.hpp"

namespace zyx::benchmark {

	struct StorageWorkloadStats {
		int64_t nodes = 0;
		int64_t edges = 0;
		int64_t properties = 0;
	};

	struct StorageProfileRow {
		std::string scenario;
		int64_t nodes = 0;
		int64_t edges = 0;
		int64_t properties = 0;
		uint64_t dbBytes = 0;
		uint64_t walBytes = 0;
		uint64_t totalBytes = 0;
		double bytesPerNode = 0.0;
	};

	struct StorageScenario {
		std::string id;
		std::string name;
		std::string pathSuffix;
		std::function<StorageWorkloadStats(Database &)> populate;
	};

	inline void cleanStoragePaths(const std::filesystem::path &dbPath) {
		std::error_code ec;
		if (std::filesystem::exists(dbPath, ec)) {
			(void) std::filesystem::remove_all(dbPath, ec);
		}

		const std::filesystem::path walPath = dbPath.string() + "-wal";
		ec.clear();
		if (std::filesystem::exists(walPath, ec)) {
			(void) std::filesystem::remove_all(walPath, ec);
		}
	}

	inline uint64_t getPathSizeBytes(const std::filesystem::path &path) {
		std::error_code ec;
		if (!std::filesystem::exists(path, ec)) {
			return 0;
		}

		ec.clear();
		if (std::filesystem::is_regular_file(path, ec)) {
			return std::filesystem::file_size(path, ec);
		}

		ec.clear();
		if (!std::filesystem::is_directory(path, ec)) {
			return 0;
		}

		uint64_t total = 0;
		std::filesystem::recursive_directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied,
														  ec);
		const std::filesystem::recursive_directory_iterator end;
		while (!ec && it != end) {
			if (it->is_regular_file(ec)) {
				total += it->file_size(ec);
			}
			it.increment(ec);
		}
		return total;
	}

	inline std::string randomAlphaString(size_t len, std::mt19937 &rng) {
		static constexpr const char charset[] =
				"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
		std::uniform_int_distribution<int> pick(0, static_cast<int>(sizeof(charset) - 2));

		std::string out;
		out.reserve(len);
		for (size_t i = 0; i < len; ++i) {
			out.push_back(charset[pick(rng)]);
		}
		return out;
	}

	inline std::vector<float> randomFloatVector(int dim, std::mt19937 &rng) {
		std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

		std::vector<float> vec;
		vec.reserve(static_cast<size_t>(dim));
		for (int i = 0; i < dim; ++i) {
			vec.push_back(dist(rng));
		}
		return vec;
	}

	inline std::vector<StorageScenario> buildStorageScenarios() {
		std::vector<StorageScenario> scenarios;

		scenarios.push_back(StorageScenario{
				"storage.numeric",
				"Storage Numeric (int64)",
				"_numeric",
				[](Database &db) {
					constexpr int batchSize = 500;
					for (int start = 0; start < config::STORAGE_PROFILE_NODE_COUNT; start += batchSize) {
						const int end = std::min(start + batchSize, config::STORAGE_PROFILE_NODE_COUNT);
						std::vector<std::unordered_map<std::string, Value>> batch;
						batch.reserve(static_cast<size_t>(end - start));
						for (int i = start; i < end; ++i) {
							std::unordered_map<std::string, Value> props;
							props["uid"] = static_cast<int64_t>(i);
							batch.push_back(std::move(props));
						}
						db.createNodes("StoreNum", batch);
					}
					return StorageWorkloadStats{config::STORAGE_PROFILE_NODE_COUNT, 0, config::STORAGE_PROFILE_NODE_COUNT};
				}});

		scenarios.push_back(StorageScenario{
				"storage.short_text",
				"Storage ShortText (32 chars)",
				"_short_text",
				[](Database &db) {
					std::mt19937 rng(99);
					constexpr int batchSize = 500;
					for (int start = 0; start < config::STORAGE_PROFILE_NODE_COUNT; start += batchSize) {
						const int end = std::min(start + batchSize, config::STORAGE_PROFILE_NODE_COUNT);
						std::vector<std::unordered_map<std::string, Value>> batch;
						batch.reserve(static_cast<size_t>(end - start));
						for (int i = start; i < end; ++i) {
							std::unordered_map<std::string, Value> props;
							props.reserve(2);
							props["uid"] = static_cast<int64_t>(i);
							props["name"] = randomAlphaString(32, rng);
							batch.push_back(std::move(props));
						}
						db.createNodes("StoreShortText", batch);
					}
					return StorageWorkloadStats{config::STORAGE_PROFILE_NODE_COUNT, 0,
												config::STORAGE_PROFILE_NODE_COUNT * 2LL};
				}});

		scenarios.push_back(StorageScenario{
				"storage.long_text",
				"Storage LongText (512 chars)",
				"_long_text",
				[](Database &db) {
					std::mt19937 rng(42);
					constexpr int batchSize = 200;
					for (int start = 0; start < config::STORAGE_PROFILE_LONG_TEXT_NODE_COUNT; start += batchSize) {
						const int end = std::min(start + batchSize, config::STORAGE_PROFILE_LONG_TEXT_NODE_COUNT);
						std::vector<std::unordered_map<std::string, Value>> batch;
						batch.reserve(static_cast<size_t>(end - start));
						for (int i = start; i < end; ++i) {
							std::unordered_map<std::string, Value> props;
							props.reserve(2);
							props["uid"] = static_cast<int64_t>(i);
							props["bio"] = randomAlphaString(512, rng);
							batch.push_back(std::move(props));
						}
						db.createNodes("StoreLongText", batch);
					}
					return StorageWorkloadStats{config::STORAGE_PROFILE_LONG_TEXT_NODE_COUNT, 0,
												config::STORAGE_PROFILE_LONG_TEXT_NODE_COUNT * 2LL};
				}});

		scenarios.push_back(StorageScenario{
				"storage.mixed",
				"Storage Mixed (bool/int/double/list)",
				"_mixed",
				[](Database &db) {
					std::mt19937 rng(7);
					std::uniform_real_distribution<double> scoreDist(0.0, 100.0);
					constexpr int batchSize = 300;

					for (int start = 0; start < config::STORAGE_PROFILE_NODE_COUNT; start += batchSize) {
						const int end = std::min(start + batchSize, config::STORAGE_PROFILE_NODE_COUNT);
						std::vector<std::unordered_map<std::string, Value>> batch;
						batch.reserve(static_cast<size_t>(end - start));

						for (int i = start; i < end; ++i) {
							std::unordered_map<std::string, Value> props;
							props.reserve(6);
							props["uid"] = static_cast<int64_t>(i);
							props["active"] = (i % 2 == 0);
							props["score"] = scoreDist(rng);
							props["name"] = "MixUser_" + std::to_string(i);
							props["city"] = "City_" + std::to_string(i % 128);
							props["tags"] = std::vector<std::string>{"red", "green", "blue", std::to_string(i % 100)};
							batch.push_back(std::move(props));
						}

						db.createNodes("StoreMixed", batch);
					}

					return StorageWorkloadStats{config::STORAGE_PROFILE_NODE_COUNT, 0,
												config::STORAGE_PROFILE_NODE_COUNT * 6LL};
				}});

		scenarios.push_back(StorageScenario{
				"storage.graph_indexes",
				"Storage Graph+Indexes (nodes+edges)",
				"_graph_idx",
				[](Database &db) {
					(void) db.execute("CALL dbms.createLabelIndex()");
					(void) db.execute("CREATE INDEX ON :RelUser(uid)");

					std::vector<int64_t> nodeIds;
					nodeIds.reserve(static_cast<size_t>(config::STORAGE_PROFILE_GRAPH_NODE_COUNT));

					for (int i = 0; i < config::STORAGE_PROFILE_GRAPH_NODE_COUNT; ++i) {
						int64_t id = db.createNodeRetId(
								"RelUser", {{"uid", static_cast<int64_t>(i)}, {"tier", static_cast<int64_t>(i % 10)}});
						nodeIds.push_back(id);
					}

					for (int i = 0; i < config::STORAGE_PROFILE_GRAPH_NODE_COUNT; ++i) {
						const int64_t src = nodeIds[static_cast<size_t>(i)];
						const int64_t nearDst =
								nodeIds[static_cast<size_t>((i + 1) % config::STORAGE_PROFILE_GRAPH_NODE_COUNT)];
						const int64_t skipDst =
								nodeIds[static_cast<size_t>((i + 17) % config::STORAGE_PROFILE_GRAPH_NODE_COUNT)];

						db.createEdgeById(src, nearDst, "FOLLOWS", {{"weight", 1.0}, {"kind", "near"}});
						db.createEdgeById(src, skipDst, "FOLLOWS", {{"weight", 2.0}, {"kind", "skip"}});
					}

					const int64_t edges = static_cast<int64_t>(config::STORAGE_PROFILE_GRAPH_NODE_COUNT) * 2LL;
					const int64_t properties =
							static_cast<int64_t>(config::STORAGE_PROFILE_GRAPH_NODE_COUNT) * 2LL + edges * 2LL;
					return StorageWorkloadStats{config::STORAGE_PROFILE_GRAPH_NODE_COUNT, edges, properties};
				}});

		scenarios.push_back(StorageScenario{
				"storage.vector_index",
				"Storage Vector128+Index",
				"_vector",
				[](Database &db) {
					(void) db.execute("CREATE VECTOR INDEX vec_store ON :VecItem(emb) OPTIONS {dimension: " +
									  std::to_string(config::STORAGE_PROFILE_VECTOR_DIM) + ", metric: 'L2'}");

					std::mt19937 rng(2026);
					constexpr int batchSize = 100;
					for (int start = 0; start < config::STORAGE_PROFILE_VECTOR_NODE_COUNT; start += batchSize) {
						const int end = std::min(start + batchSize, config::STORAGE_PROFILE_VECTOR_NODE_COUNT);
						std::vector<std::unordered_map<std::string, Value>> batch;
						batch.reserve(static_cast<size_t>(end - start));
						for (int i = start; i < end; ++i) {
							std::unordered_map<std::string, Value> props;
							props.reserve(2);
							props["uid"] = static_cast<int64_t>(i);
							props["emb"] = randomFloatVector(config::STORAGE_PROFILE_VECTOR_DIM, rng);
							batch.push_back(std::move(props));
						}
						db.createNodes("VecItem", batch);
					}
					return StorageWorkloadStats{config::STORAGE_PROFILE_VECTOR_NODE_COUNT, 0,
												config::STORAGE_PROFILE_VECTOR_NODE_COUNT * 2LL};
				}});

		return scenarios;
	}

	inline StorageProfileRow runStorageScenario(const StorageScenario &scenario, const std::filesystem::path &dbPath) {
		cleanStoragePaths(dbPath);

		std::cout << "   [Storage Setup] " << scenario.name << "..." << std::flush;
		StorageWorkloadStats workloadStats;
		{
			Database db(dbPath.string());
			db.open();
			workloadStats = scenario.populate(db);
			db.save();
			db.close();
		}
		std::cout << " Done." << std::endl;

		const std::filesystem::path walPath = dbPath.string() + "-wal";
		const uint64_t dbBytes = getPathSizeBytes(dbPath);
		const uint64_t walBytes = getPathSizeBytes(walPath);
		const uint64_t totalBytes = dbBytes + walBytes;
		const double bytesPerNode =
				workloadStats.nodes > 0 ? static_cast<double>(totalBytes) / static_cast<double>(workloadStats.nodes) : 0.0;

		return StorageProfileRow{
				scenario.name, workloadStats.nodes, workloadStats.edges, workloadStats.properties,
				dbBytes,		   walBytes,				totalBytes,			 bytesPerNode};
	}

	inline std::string storageSeparator() {
		return "+" + std::string(41, '-') + "+" + std::string(10, '-') + "+" + std::string(10, '-') + "+" +
			   std::string(12, '-') + "+" + std::string(12, '-') + "+" + std::string(12, '-') + "+" +
			   std::string(12, '-') + "+" + std::string(12, '-') + "+";
	}

	inline void printStorageHeader() {
		const std::string sep = storageSeparator();
		std::cout << "\n=================================================================\n";
		std::cout << "  Storage Footprint Profiling Suite\n";
		std::cout << "=================================================================\n";
		std::cout << sep << "\n"
				  << "| " << std::left << std::setw(40) << "Scenario"
				  << "| " << std::setw(9) << "Nodes"
				  << "| " << std::setw(9) << "Edges"
				  << "| " << std::setw(11) << "Props"
				  << "| " << std::setw(11) << "DB(MB)"
				  << "| " << std::setw(11) << "WAL(MB)"
				  << "| " << std::setw(11) << "Total(MB)"
				  << "| " << std::setw(11) << "Bytes/Node"
				  << "|\n"
				  << sep << "\n";
	}

	inline void printStorageRow(const StorageProfileRow &row) {
		const double dbMB = static_cast<double>(row.dbBytes) / (1024.0 * 1024.0);
		const double walMB = static_cast<double>(row.walBytes) / (1024.0 * 1024.0);
		const double totalMB = static_cast<double>(row.totalBytes) / (1024.0 * 1024.0);

		std::cout << "| " << std::left << std::setw(40) << row.scenario
				  << "| " << std::setw(9) << row.nodes
				  << "| " << std::setw(9) << row.edges
				  << "| " << std::setw(11) << row.properties
				  << "| " << std::setw(11) << std::fixed << std::setprecision(3) << dbMB
				  << "| " << std::setw(11) << std::fixed << std::setprecision(3) << walMB
				  << "| " << std::setw(11) << std::fixed << std::setprecision(3) << totalMB
				  << "| " << std::setw(11) << std::fixed << std::setprecision(1) << row.bytesPerNode << "|\n";
	}

	inline std::string writeStorageCsv(const std::vector<StorageProfileRow> &rows) {
		std::ofstream out(std::string(config::PATH_STORAGE_CSV), std::ios::trunc);
		out << "scenario,nodes,edges,properties,db_bytes,wal_bytes,total_bytes,bytes_per_node\n";
		for (const auto &row: rows) {
			out << '"' << row.scenario << '"' << ","
				<< row.nodes << ","
				<< row.edges << ","
				<< row.properties << ","
				<< row.dbBytes << ","
				<< row.walBytes << ","
				<< row.totalBytes << ","
				<< std::fixed << std::setprecision(4) << row.bytesPerNode << "\n";
		}
		return std::string(config::PATH_STORAGE_CSV);
	}

	inline std::vector<StorageProfileRow> runStorageBenchmarks(const BenchmarkSelector &selector) {
		std::vector<StorageProfileRow> rows;
		const auto scenarios = buildStorageScenarios();

		bool hasMatch = false;
		for (const auto &scenario: scenarios) {
			if (matchesSelection(selector, BenchmarkType::STORAGE, scenario.id, scenario.name)) {
				hasMatch = true;
				break;
			}
		}

		if (!hasMatch) {
			return rows;
		}

		printStorageHeader();
		for (const auto &scenario: scenarios) {
			if (!matchesSelection(selector, BenchmarkType::STORAGE, scenario.id, scenario.name)) {
				continue;
			}

			try {
				const std::filesystem::path dbPath = std::string(config::PATH_STORAGE_BASE) + scenario.pathSuffix;
				auto row = runStorageScenario(scenario, dbPath);
				rows.push_back(row);
				printStorageRow(row);
			} catch (const std::exception &e) {
				std::cerr << "   [FAILED] " << scenario.name << ": " << e.what() << "\n";
			} catch (...) {
				std::cerr << "   [CRASH] " << scenario.name << ": Unknown error\n";
			}
		}

		std::cout << storageSeparator() << "\n";
		if (!rows.empty()) {
			std::string csvPath = writeStorageCsv(rows);
			std::cout << "   [Export] Storage CSV: " << csvPath << "\n";
		}

		return rows;
	}

} // namespace zyx::benchmark
