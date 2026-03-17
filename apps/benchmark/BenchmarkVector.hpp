/**
 * @file BenchmarkVector.hpp
 * @author Nexepic
 * @date 2026/1/30
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

#include <random>
#include <sstream>
#include <vector>
#include "BenchmarkFramework.hpp"

namespace zyx::benchmark {

	// Helper to generate random vectors
	inline std::vector<float> generateVector(size_t dim, std::mt19937 &rng) {
		std::uniform_real_distribution<float> dist(0.0f, 1.0f);
		std::vector<float> v(dim);
		for (auto &x: v)
			x = dist(rng);
		return v;
	}

	// ========================================================================
	// Scenario 1: Vector Insertion
	// ========================================================================

	class VectorInsertBench : public BenchmarkBase {
		bool useIndex_;
		int dim_;
		std::vector<std::vector<float>> dataset_;

	public:
		VectorInsertBench(std::string name, std::string path, int iter, int dataSize, bool useIndex, int dim) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), useIndex_(useIndex), dim_(dim) {}

		void setup(Database &db) override {
			if (useIndex_) {
				// Create Vector Index
				// Note: We use a larger dimension to simulate real workload
				std::string q =
						"CREATE VECTOR INDEX vec_bench ON :Item(emb) OPTIONS {dimension: " + std::to_string(dim_) +
						", metric: 'L2'}";
				(void) db.execute(q);
			}

			// Pre-generate data to avoid measuring RNG time
			std::mt19937 rng(42);
			dataset_.reserve(iterations_); // One vector per insert operation
			for (int i = 0; i < iterations_; ++i) {
				dataset_.push_back(generateVector(dim_, rng));
			}
		}

		void run(Database &db) override {
			// We use native API for speed and precision in benchmark
			// Single insert per run() call (BenchmarkBase loop handles iterations)
			static std::size_t idx = 0;
			if (idx >= dataset_.size())
				idx = 0;

			std::unordered_map<std::string, Value> props;

			// Convert float vector to string list (as per current public API limit)
			// Or use internal API if benchmark has access?
			// The Public API `zyx::Value` currently requires `vector<string>` for lists based on your previous fix.
			// Let's assume we pass it as vector<string> of floats.

			const auto &vec = dataset_[idx++];
			std::vector<std::string> strVec;
			strVec.reserve(vec.size());
			for (float f: vec)
				strVec.push_back(std::to_string(f));

			props["emb"] = strVec;
			props["id"] = static_cast<int64_t>(idx);

			db.createNode("Item", props);
		}

		void teardown(Database &) override { dataset_.clear(); }
	};

	// ========================================================================
	// Scenario 2: Vector Search
	// ========================================================================

	class VectorSearchBench : public BenchmarkBase {
		int dim_;
		int k_;
		std::vector<float> queryVec_;

	public:
		VectorSearchBench(std::string name, std::string path, int iter, int dataSize, int dim, int k) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), dim_(dim), k_(k) {}

		void setup(Database &db) override {
			// 1. Create Index
			(void) db.execute("CREATE VECTOR INDEX vec_bench ON :Item(emb) OPTIONS {dimension: " +
							  std::to_string(dim_) + ", metric: 'L2'}");

			// 2. Bulk Load Data
			std::cout << " (Loading " << dataSize_ << " vectors...) " << std::flush;
			std::mt19937 rng(42);

			// Batch insert to speed up setup
			int batchSize = 1000;
			std::vector<std::unordered_map<std::string, Value>> batchData;

			for (int i = 0; i < dataSize_; ++i) {
				auto vec = generateVector(dim_, rng);
				std::vector<std::string> strVec;
				strVec.reserve(vec.size());
				for (const float f: vec)
					strVec.push_back(std::to_string(f));

				std::unordered_map<std::string, Value> props;
				props["emb"] = strVec;
				props["id"] = static_cast<int64_t>(i);

				batchData.push_back(std::move(props));

				if (batchData.size() >= static_cast<std::size_t>(batchSize)) {
					db.createNodes("Item", batchData);
					batchData.clear();
				}
			}
			if (!batchData.empty())
				db.createNodes("Item", batchData);

			// 3. Train (Optional but recommended for large scale)
			// Disabled for faster benchmark execution with smaller datasets
			// if (dataSize_ > 2000) {
			// 	std::cout << " (Training Index...) " << std::flush;
			// 	(void) db.execute("CALL db.index.vector.train('vec_bench')");
			// }

			// 4. Prepare Query
			queryVec_ = generateVector(dim_, rng);
		}

		void run(Database &db) override {
			// Construct Query String
			std::ostringstream oss;
			oss << "CALL db.index.vector.queryNodes('vec_bench', " << k_ << ", [";
			for (size_t i = 0; i < queryVec_.size(); ++i) {
				oss << queryVec_[i] << (i < queryVec_.size() - 1 ? "," : "");
			}
			oss << "]) YIELD node, score RETURN node.id, score";

			(void) db.execute(oss.str());
		}

		void teardown(Database &) override {}
	};

} // namespace zyx::benchmark
