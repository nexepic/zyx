/**
 * @file BenchmarkCases.hpp
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

#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "BenchmarkFramework.hpp"

namespace zyx::benchmark {

	// ========================================================================
	// Enums & Helpers
	// ========================================================================

	enum class InsertIndexMode {
		NONE, // No indexes (Raw storage speed)
		LABEL_ONLY, // Only Label Index enabled
		PROPERTY_ONLY, // Only Property Index enabled
		BOTH // Both Label and Property Indexes enabled
	};

	inline void setupInsertIndexes(const Database &db, const InsertIndexMode mode) {
		if (mode == InsertIndexMode::LABEL_ONLY || mode == InsertIndexMode::BOTH) {
			(void) db.execute("CALL dbms.createLabelIndex()");
		}
		if (mode == InsertIndexMode::PROPERTY_ONLY || mode == InsertIndexMode::BOTH) {
			(void) db.execute("CREATE INDEX ON :BenchUser(uid)");
		}
	}

	// ========================================================================
	// Scenario 1: Single Node Creation (Baseline)
	// ========================================================================

	class CypherInsertBench : public BenchmarkBase {
		InsertIndexMode mode_;

	public:
		CypherInsertBench(std::string name, std::string path, int iter, int dataSize, InsertIndexMode mode) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), mode_(mode) {}

		void setup(Database &db) override { setupInsertIndexes(db, mode_); }

		void run(Database &db) override { (void)db.execute("CREATE (n:BenchUser {id: 1, name: 'TestUser'})"); }

		void teardown(Database &) override {}
	};

	class NativeInsertBench : public BenchmarkBase {
		InsertIndexMode mode_;

	public:
		NativeInsertBench(std::string name, std::string path, int iter, int dataSize, InsertIndexMode mode) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), mode_(mode) {}

		void setup(Database &db) override { setupInsertIndexes(db, mode_); }

		void run(Database &db) override {
			db.createNode("BenchUser", {{"id", static_cast<int64_t>(1)}, {"name", "TestUser"}});
		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Scenario 2: Batch Insertion (High Throughput / Bulk Load)
	// ========================================================================

	class CypherBatchInsertBench : public BenchmarkBase {
		std::string query_;
		InsertIndexMode mode_;

	public:
		CypherBatchInsertBench(std::string name, std::string path, int iter, int dataSize, InsertIndexMode mode) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), mode_(mode) {}

		void setup(Database &db) override {
			setupInsertIndexes(db, mode_);

			// Generate massive CREATE statement
			std::ostringstream oss;
			oss << "CREATE ";
			for (int i = 0; i < dataSize_; ++i) {
				oss << "(:BenchUser {id: " << i << ", name: 'User_" << i << "'})";
				if (i < dataSize_ - 1) {
					oss << ", ";
				}
			}
			query_ = oss.str();
		}

		void run(Database &db) override { (void)db.execute(query_); }

		void teardown(Database &) override {}

		[[nodiscard]] int getItemsPerOp() const override { return dataSize_; }
	};

	class NativeBatchInsertBench : public BenchmarkBase {
		std::vector<std::unordered_map<std::string, Value>> preparedData_;
		std::string label_ = "BenchUser";
		InsertIndexMode mode_;

	public:
		NativeBatchInsertBench(std::string name, std::string path, int iter, int dataSize, InsertIndexMode mode) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), mode_(mode) {}

		void setup(Database &db) override {
			setupInsertIndexes(db, mode_);
			std::cout << " (Preparing memory [Map Standard]...) " << std::flush;

			preparedData_.reserve(dataSize_);
			for (int i = 0; i < dataSize_; ++i) {
				std::unordered_map<std::string, Value> props;
				props.reserve(2);
				props["id"] = static_cast<int64_t>(i);
				props["name"] = "User_" + std::to_string(i);

				preparedData_.push_back(std::move(props));
			}
		}

		void run(Database &db) override { db.createNodes(label_, preparedData_); }

		void teardown(Database &) override { preparedData_.clear(); }

		[[nodiscard]] int getItemsPerOp() const override { return dataSize_; }
	};

	// ========================================================================
	// Scenario 3: Query Performance (Index vs Scan)
	// ========================================================================

	enum class QueryMode { PROPERTY_INDEX, LABEL_SCAN, FULL_SCAN };

	class CypherQueryBench : public BenchmarkBase {
		QueryMode mode_;

	public:
		CypherQueryBench(std::string name, std::string path, int iter, int dataSize, QueryMode mode) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), mode_(mode) {}

		void setup(Database &db) override {
			// 1. Setup Index
			if (mode_ == QueryMode::PROPERTY_INDEX) {
				(void)db.execute("CREATE INDEX ON :User(uid)");
			} else if (mode_ == QueryMode::LABEL_SCAN) {
				(void)db.execute("CALL dbms.createLabelIndex()");
			}

			// 2. Pre-fill data using batch API
			std::cout << " (Filling " << dataSize_ << " users...) " << std::flush;

			std::vector<std::unordered_map<std::string, Value>> batch;
			constexpr size_t BATCH_CHUNK = 1000;
			batch.reserve(BATCH_CHUNK);

			for (int i = 0; i < dataSize_; ++i) {
				std::unordered_map<std::string, Value> props;
				props.reserve(1);
				props["uid"] = static_cast<int64_t>(i);

				batch.push_back(std::move(props));

				if (batch.size() >= BATCH_CHUNK) {
					db.createNodes("User", batch);
					batch.clear();
				}
			}
			if (!batch.empty())
				db.createNodes("User", batch);

			// 3. Inject Noise Data
			int noiseCount = dataSize_ * 4;
			std::cout << " (Filling " << noiseCount << " noise nodes...) " << std::flush;

			std::vector<std::string> noiseLabels = {"Product", "Order", "Category", "Log"};
			int perLabel = std::max(1, noiseCount / static_cast<int>(noiseLabels.size()));

			for (const auto &lbl: noiseLabels) {
				batch.clear();
				for (int i = 0; i < perLabel; ++i) {
					std::unordered_map<std::string, Value> props;
					props.reserve(2);
					props["uid"] = static_cast<int64_t>(i);
					props["noise"] = "true";

					batch.push_back(std::move(props));

					if (batch.size() >= BATCH_CHUNK) {
						db.createNodes(lbl, batch);
						batch.clear();
					}
				}
				if (!batch.empty())
					db.createNodes(lbl, batch);
			}

			db.save();
		}

		void run(Database &db) override {
			int64_t searchId = rand() % dataSize_;
			std::string q;

			switch (mode_) {
				case QueryMode::PROPERTY_INDEX:
				case QueryMode::LABEL_SCAN:
					q = "MATCH (n:User {uid: " + std::to_string(searchId) + "}) RETURN n.uid";
					break;
				case QueryMode::FULL_SCAN:
					q = "MATCH (n {uid: " + std::to_string(searchId) + "}) RETURN n.uid";
					break;
			}

			auto res = db.execute(q);

			if (!res.hasNext()) {
				std::cerr << "\n[FATAL] ID " << searchId << " missing!\n";
				std::abort();
			}

			// Optional: Value check

		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Scenario 4: Graph Algorithms
	// ========================================================================

	class AlgoShortestPathBench : public BenchmarkBase {
		int64_t startId = 0, endId = 0;

	public:
		using BenchmarkBase::BenchmarkBase;
		void setup(Database &db) override {
			int chainLen = dataSize_;
			std::cout << " (Building chain " << chainLen << ") " << std::flush;
			int64_t prevId = -1;
			for (int i = 0; i < chainLen; ++i) {
				int64_t curr = db.createNodeRetId("Node", {{"id", static_cast<int64_t>(i)}});
				if (prevId != -1)
					db.createEdgeById(prevId, curr, "NEXT");
				prevId = curr;
				if (i == 0)
					startId = curr;
				if (i == chainLen - 1)
					endId = curr;
			}
			db.save();
		}
		void run(Database &db) override { (void)db.getShortestPath(startId, endId); }
		void teardown(Database &) override {}
	};

	class CypherShortestPathBench : public BenchmarkBase {
		int64_t startId = 0, endId = 0;

	public:
		using BenchmarkBase::BenchmarkBase;
		void setup(Database &db) override {
			int chainLen = dataSize_;
			std::cout << " (Building chain " << chainLen << ") " << std::flush;
			int64_t prevId = -1;
			for (int i = 0; i < chainLen; ++i) {
				int64_t curr = db.createNodeRetId("Node", {{"id", static_cast<int64_t>(i)}});
				if (prevId != -1)
					db.createEdgeById(prevId, curr, "NEXT");
				prevId = curr;
				if (i == 0)
					startId = curr;
				if (i == chainLen - 1)
					endId = curr;
			}
			db.save();
		}
		void run(Database &db) override {
			std::string q = "CALL algo.shortestPath(" + std::to_string(startId) + ", " + std::to_string(endId) + ")";
			(void)db.execute(q);
		}
		void teardown(Database &) override {}
	};

	// ========================================================================
	// Scenario 5: Transaction Performance (Implicit vs Explicit)
	// ========================================================================

	class ImplicitTxnInsertBench : public BenchmarkBase {
	public:
		ImplicitTxnInsertBench(std::string name, std::string path, int iter, int dataSize) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize) {}

		void setup(Database &) override {}

		void run(Database &db) override {
			// Each CREATE is auto-committed individually (implicit transaction)
			for (int i = 0; i < dataSize_; ++i) {
				(void) db.execute("CREATE (n:TxnBench {id: " + std::to_string(i) + "})");
			}
		}

		void teardown(Database &) override {}

		[[nodiscard]] int getItemsPerOp() const override { return dataSize_; }
	};

	class ExplicitTxnInsertBench : public BenchmarkBase {
	public:
		ExplicitTxnInsertBench(std::string name, std::string path, int iter, int dataSize) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize) {}

		void setup(Database &) override {}

		void run(Database &db) override {
			// Single explicit transaction wrapping all CREATEs
			(void) db.execute("BEGIN");
			for (int i = 0; i < dataSize_; ++i) {
				(void) db.execute("CREATE (n:TxnBench {id: " + std::to_string(i) + "})");
			}
			(void) db.execute("COMMIT");
		}

		void teardown(Database &) override {}

		[[nodiscard]] int getItemsPerOp() const override { return dataSize_; }
	};

	class ExplicitTxnBatchInsertBench : public BenchmarkBase {
	public:
		ExplicitTxnBatchInsertBench(std::string name, std::string path, int iter, int dataSize) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize) {}

		void setup(Database &) override {}

		void run(Database &db) override {
			// Single explicit transaction with a single batch CREATE
			(void) db.execute("BEGIN");
			std::ostringstream oss;
			oss << "CREATE ";
			for (int i = 0; i < dataSize_; ++i) {
				oss << "(:TxnBench {id: " << i << "})";
				if (i < dataSize_ - 1) {
					oss << ", ";
				}
			}
			(void) db.execute(oss.str());
			(void) db.execute("COMMIT");
		}

		void teardown(Database &) override {}

		[[nodiscard]] int getItemsPerOp() const override { return dataSize_; }
	};

} // namespace zyx::benchmark
