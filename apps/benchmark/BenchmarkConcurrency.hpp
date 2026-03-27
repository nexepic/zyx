/**
 * @file BenchmarkConcurrency.hpp
 * @author Nexepic
 * @date 2026/3/23
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

#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "BenchmarkFramework.hpp"

namespace zyx::benchmark {

	// ========================================================================
	// Concurrency Benchmark: Parallel Query (Full Scan)
	// Tests: NodeScanOperator + FilterOperator parallelization
	// ========================================================================

	class ConcurrentQueryBench : public BenchmarkBase {
		size_t threadCount_;

	public:
		ConcurrentQueryBench(std::string name, std::string path, int iter, int dataSize, size_t threadCount) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), threadCount_(threadCount) {}

		void setup(Database &db) override {
			// Configure thread pool (reopen to apply)
			db.setThreadPoolSize(threadCount_);

			// Create label index for comparison
			(void) db.execute("CALL dbms.createLabelIndex()");

			// Pre-fill data
			std::cout << " (Filling " << dataSize_ << " users...) " << std::flush;

			std::vector<std::unordered_map<std::string, Value>> batch;
			constexpr size_t BATCH_CHUNK = 1000;
			batch.reserve(BATCH_CHUNK);

			for (int i = 0; i < dataSize_; ++i) {
				std::unordered_map<std::string, Value> props;
				props["uid"] = static_cast<int64_t>(i);
				props["name"] = "User_" + std::to_string(i);
				props["score"] = static_cast<double>(i % 100);
				batch.push_back(std::move(props));

				if (batch.size() >= BATCH_CHUNK) {
					db.createNodes("User", batch);
					batch.clear();
				}
			}
			if (!batch.empty())
				db.createNodes("User", batch);

			// Add noise
			batch.clear();
			for (int i = 0; i < dataSize_ * 2; ++i) {
				std::unordered_map<std::string, Value> props;
				props["uid"] = static_cast<int64_t>(i);
				batch.push_back(std::move(props));
				if (batch.size() >= BATCH_CHUNK) {
					db.createNodes("Product", batch);
					batch.clear();
				}
			}
			if (!batch.empty())
				db.createNodes("Product", batch);

			db.save();
		}

		void run(Database &db) override {
			// Query that exercises NodeScan + Filter
			auto res = db.execute("MATCH (n:User) WHERE n.score > 50 RETURN n.uid, n.name");
			while (res.hasNext())
				res.next();
		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Concurrency Benchmark: Parallel Sort
	// Tests: SortOperator parallelization on large result sets
	// ========================================================================

	class ConcurrentSortBench : public BenchmarkBase {
		size_t threadCount_;

	public:
		ConcurrentSortBench(std::string name, std::string path, int iter, int dataSize, size_t threadCount) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), threadCount_(threadCount) {}

		void setup(Database &db) override {
			db.setThreadPoolSize(threadCount_);

			std::cout << " (Filling " << dataSize_ << " nodes...) " << std::flush;

			std::vector<std::unordered_map<std::string, Value>> batch;
			constexpr size_t BATCH_CHUNK = 1000;
			batch.reserve(BATCH_CHUNK);

			std::mt19937 rng(42);
			std::uniform_real_distribution<double> dist(0.0, 10000.0);

			for (int i = 0; i < dataSize_; ++i) {
				std::unordered_map<std::string, Value> props;
				props["id"] = static_cast<int64_t>(i);
				props["score"] = dist(rng);
				batch.push_back(std::move(props));

				if (batch.size() >= BATCH_CHUNK) {
					db.createNodes("Item", batch);
					batch.clear();
				}
			}
			if (!batch.empty())
				db.createNodes("Item", batch);

			db.save();
		}

		void run(Database &db) override {
			auto res = db.execute("MATCH (n:Item) RETURN n.id, n.score ORDER BY n.score DESC");
			while (res.hasNext())
				res.next();
		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Concurrency Benchmark: Parallel Batch Insert
	// Tests: FileStorage::save() parallel preparation
	// ========================================================================

	class ConcurrentBatchInsertBench : public BenchmarkBase {
		size_t threadCount_;
		std::vector<std::unordered_map<std::string, Value>> preparedData_;

	public:
		ConcurrentBatchInsertBench(std::string name, std::string path, int iter, int dataSize, size_t threadCount) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), threadCount_(threadCount) {}

		void setup(Database &db) override {
			db.setThreadPoolSize(threadCount_);

			std::cout << " (Preparing " << dataSize_ << " records...) " << std::flush;
			preparedData_.reserve(dataSize_);
			for (int i = 0; i < dataSize_; ++i) {
				std::unordered_map<std::string, Value> props;
				props["id"] = static_cast<int64_t>(i);
				props["name"] = "BatchUser_" + std::to_string(i);
				preparedData_.push_back(std::move(props));
			}
		}

		void run(Database &db) override {
			db.createNodes("BatchUser", preparedData_);
			db.save();
		}

		void teardown(Database &) override { preparedData_.clear(); }

		[[nodiscard]] int getItemsPerOp() const override { return dataSize_; }
	};

	// ========================================================================
	// Concurrency Benchmark: Parallel PQ Training
	// Tests: NativeProductQuantizer parallel subspace training
	// ========================================================================

	class ConcurrentPQTrainBench : public BenchmarkBase {
		size_t threadCount_;
		int dim_;

	public:
		ConcurrentPQTrainBench(std::string name, std::string path, int iter, int dataSize, size_t threadCount,
							   int dim) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), threadCount_(threadCount), dim_(dim) {}

		void setup(Database &db) override {
			db.setThreadPoolSize(threadCount_);

			// Create vector index
			(void) db.execute("CREATE VECTOR INDEX vec_train ON :TrainItem(emb) OPTIONS {dimension: " +
							  std::to_string(dim_) + ", metric: 'L2'}");

			// Bulk load enough vectors to trigger training
			std::cout << " (Loading " << dataSize_ << " vectors...) " << std::flush;
			std::mt19937 rng(42);
			std::uniform_real_distribution<float> dist(0.0f, 1.0f);

			int batchSize = 500;
			std::vector<std::unordered_map<std::string, Value>> batchData;
			batchData.reserve(batchSize);

			for (int i = 0; i < dataSize_; ++i) {
				std::vector<float> vec;
				vec.reserve(dim_);
				for (int d = 0; d < dim_; ++d)
					vec.push_back(dist(rng));

				std::unordered_map<std::string, Value> props;
				props["emb"] = vec;
				props["id"] = static_cast<int64_t>(i);
				batchData.push_back(std::move(props));

				if (batchData.size() >= static_cast<size_t>(batchSize)) {
					db.createNodes("TrainItem", batchData);
					batchData.clear();
				}
			}
			if (!batchData.empty())
				db.createNodes("TrainItem", batchData);
		}

		void run(Database &db) override {
			(void) db.execute("CALL db.index.vector.train('vec_train')");
		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Concurrency Benchmark: Parallel Vector Search
	// Tests: DiskANN search re-ranking + PQ distance table parallelization
	// ========================================================================

	class ConcurrentVectorSearchBench : public BenchmarkBase {
		size_t threadCount_;
		int dim_;
		int k_;
		std::vector<float> queryVec_;

	public:
		ConcurrentVectorSearchBench(std::string name, std::string path, int iter, int dataSize, size_t threadCount,
									int dim, int k) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), threadCount_(threadCount), dim_(dim),
			k_(k) {}

		void setup(Database &db) override {
			db.setThreadPoolSize(threadCount_);

			(void) db.execute("CREATE VECTOR INDEX vec_search ON :SearchItem(emb) OPTIONS {dimension: " +
							  std::to_string(dim_) + ", metric: 'L2'}");

			std::cout << " (Loading " << dataSize_ << " vectors...) " << std::flush;
			std::mt19937 rng(42);
			std::uniform_real_distribution<float> dist(0.0f, 1.0f);

			int batchSize = 500;
			std::vector<std::unordered_map<std::string, Value>> batchData;

			for (int i = 0; i < dataSize_; ++i) {
				std::vector<float> vec;
				vec.reserve(dim_);
				for (int d = 0; d < dim_; ++d)
					vec.push_back(dist(rng));

				std::unordered_map<std::string, Value> props;
				props["emb"] = vec;
				props["id"] = static_cast<int64_t>(i);
				batchData.push_back(std::move(props));

				if (batchData.size() >= static_cast<size_t>(batchSize)) {
					db.createNodes("SearchItem", batchData);
					batchData.clear();
				}
			}
			if (!batchData.empty())
				db.createNodes("SearchItem", batchData);

			// Prepare query vector
			queryVec_.resize(dim_);
			for (int d = 0; d < dim_; ++d)
				queryVec_[d] = dist(rng);
		}

		void run(Database &db) override {
			std::ostringstream oss;
			oss << "CALL db.index.vector.queryNodes('vec_search', " << k_ << ", [";
			for (size_t i = 0; i < queryVec_.size(); ++i) {
				oss << queryVec_[i] << (i < queryVec_.size() - 1 ? "," : "");
			}
			oss << "]) YIELD node, score RETURN node.id, score";
			(void) db.execute(oss.str());
		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Concurrency Benchmark: Concurrent Read Transactions
	// Tests: Multiple reader threads with snapshot isolation
	// ========================================================================

	class ConcurrentReadBench : public BenchmarkBase {
		size_t threadCount_;

	public:
		ConcurrentReadBench(std::string name, std::string path, int iter, int dataSize, size_t threadCount) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), threadCount_(threadCount) {}

		void setup(Database &db) override {
			std::cout << " (Filling " << dataSize_ << " users...) " << std::flush;

			std::vector<std::unordered_map<std::string, Value>> batch;
			constexpr size_t BATCH_CHUNK = 1000;
			batch.reserve(BATCH_CHUNK);

			for (int i = 0; i < dataSize_; ++i) {
				std::unordered_map<std::string, Value> props;
				props["uid"] = static_cast<int64_t>(i);
				props["name"] = "User_" + std::to_string(i);
				props["score"] = static_cast<double>(i % 100);
				batch.push_back(std::move(props));

				if (batch.size() >= BATCH_CHUNK) {
					db.createNodes("User", batch);
					batch.clear();
				}
			}
			if (!batch.empty())
				db.createNodes("User", batch);

			db.save();
		}

		void run(Database &db) override {
			// Launch threadCount_ reader threads, each running a read query
			std::vector<std::thread> threads;
			threads.reserve(threadCount_);
			std::atomic<int> completed{0};

			for (size_t i = 0; i < threadCount_; ++i) {
				threads.emplace_back([&db, &completed, i, this]() {
					// Each thread queries a different UID
					int64_t uid = static_cast<int64_t>(i % dataSize_);
					auto res = db.execute("MATCH (n:User) WHERE n.uid = " + std::to_string(uid) + " RETURN n.name");
					while (res.hasNext())
						res.next();
					completed.fetch_add(1);
				});
			}

			for (auto &t : threads) t.join();
		}

		void teardown(Database &) override {}

		[[nodiscard]] int getItemsPerOp() const override { return static_cast<int>(threadCount_); }
	};

} // namespace zyx::benchmark
