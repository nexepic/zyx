/**
 * @file BenchmarkCases.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "BenchmarkFramework.hpp"

namespace metrix::benchmark {

	// ========================================================================
	// Enums & Helpers
	// ========================================================================

	enum class InsertIndexMode {
		NONE, // No indexes (Raw storage speed)
		LABEL_ONLY, // Only Label Index enabled
		PROPERTY_ONLY, // Only Property Index enabled
		BOTH // Both Label and Property Indexes enabled
	};

	inline std::string getModeName(InsertIndexMode mode) {
		switch (mode) {
			case InsertIndexMode::NONE:
				return "No Index";
			case InsertIndexMode::LABEL_ONLY:
				return "Label Index";
			case InsertIndexMode::PROPERTY_ONLY:
				return "Prop Index";
			case InsertIndexMode::BOTH:
				return "All Indexes";
			default:
				return "Unknown";
		}
	}

	// Helper to setup indexes based on mode
	// Note: We use "BenchUser" as the standard label for insertion tests.
	inline void setupInsertIndexes(Database &db, InsertIndexMode mode) {
		if (mode == InsertIndexMode::LABEL_ONLY || mode == InsertIndexMode::BOTH) {
			db.execute("CALL dbms.createLabelIndex()");
		}
		if (mode == InsertIndexMode::PROPERTY_ONLY || mode == InsertIndexMode::BOTH) {
			// Index on 'id' property for label 'BenchUser'
			db.execute("CREATE INDEX ON :BenchUser(id)");
		}
	}

	// ========================================================================
	// Scenario 1: Single Node Creation (Baseline)
	// Metric: Write Throughput (Ops/Sec) for transactional/interactive use.
	// Logic: 'run' inserts 1 node. Framework calls it 'iterations_' times.
	// ========================================================================

	class CypherInsertBench : public BenchmarkBase {
		InsertIndexMode mode_;

	public:
		CypherInsertBench(std::string name, std::string path, int iter, int dataSize, InsertIndexMode mode) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), mode_(mode) {}

		void setup(Database &db) override { setupInsertIndexes(db, mode_); }

		void run(Database &db) override {
			// Uses Parser -> Planner -> Executor -> Storage
			db.execute("CREATE (n:BenchUser {id: 1, name: 'TestUser'})");
		}

		void teardown(Database &) override {}
	};

	class NativeInsertBench : public BenchmarkBase {
		InsertIndexMode mode_;

	public:
		NativeInsertBench(std::string name, std::string path, int iter, int dataSize, InsertIndexMode mode) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), mode_(mode) {}

		void setup(Database &db) override { setupInsertIndexes(db, mode_); }

		void run(Database &db) override {
			// Bypass Parser, uses Internal Builder -> Executor -> Storage
			db.createNode("BenchUser", {{"id", static_cast<int64_t>(1)}, {"name", "TestUser"}});
		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Scenario 2: Batch Insertion (High Throughput / Bulk Load)
	// Comparison: Cypher UNWIND vs Native vector<Node>
	// Logic: 'setup' prepares the payload. 'run' executes ONE massive batch.
	// ========================================================================

	class CypherBatchInsertBench : public BenchmarkBase {
		std::string query_;
		InsertIndexMode mode_;

	public:
		CypherBatchInsertBench(std::string name, std::string path, int iter, int dataSize, InsertIndexMode mode) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), mode_(mode) {}

		void setup(Database &db) override {
			setupInsertIndexes(db, mode_);

			std::ostringstream oss;
			oss << "CREATE ";
			for (int i = 0; i < dataSize_; ++i) {
				oss << "(:BatchUser {id: " << i << ", name: 'User_" << i << "'})";
				if (i < dataSize_ - 1) {
					oss << ", ";
				}
			}
			query_ = oss.str();
		}

		void run(Database &db) override { db.execute(query_); }

		void teardown(Database &) override {}

		int getItemsPerOp() const override { return dataSize_; }
	};

	class NativeBatchInsertBench : public BenchmarkBase {
		std::vector<std::vector<std::pair<std::string, metrix::Value>>> preparedData_;
		std::string label_ = "BenchUser";
		InsertIndexMode mode_;

	public:
		NativeBatchInsertBench(std::string name, std::string path, int iter, int dataSize, InsertIndexMode mode) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), mode_(mode) {}

		void setup(Database &db) override {
			setupInsertIndexes(db, mode_);

			preparedData_.reserve(dataSize_);
			for (int i = 0; i < dataSize_; ++i) {
				// Construct linear vector, faster to iterate than map
				std::vector<std::pair<std::string, metrix::Value>> props;
				props.reserve(2);
				props.emplace_back("id", static_cast<int64_t>(i));
				props.emplace_back("name", "User_" + std::to_string(i));

				preparedData_.push_back(std::move(props));
			}
		}

		void run(Database &db) override {
			// Call the optimized overload
			db.createNodes(label_, preparedData_);
		}

		void teardown(Database &) override { preparedData_.clear(); }

		int getItemsPerOp() const override { return dataSize_; }
	};

	// ========================================================================
	// Scenario 3: Query Performance (Index vs Scan)
	// Metric: Read Latency (Avg/P99)
	// ========================================================================

	enum class QueryMode {
		PROPERTY_INDEX, // Best: Use specific index on uid
		LABEL_SCAN, // Baseline: Use Label Index
		FULL_SCAN // Worst: Scan entire DB
	};

	class CypherQueryBench : public BenchmarkBase {
		QueryMode mode_;

	public:
		CypherQueryBench(std::string name, std::string path, int iter, int dataSize, QueryMode mode) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize), mode_(mode) {}

		void setup(Database &db) override {
			// 1. Setup Index based on mode
			if (mode_ == QueryMode::PROPERTY_INDEX) {
				db.execute("CREATE INDEX ON :User(uid)");
			} else if (mode_ == QueryMode::LABEL_SCAN) {
				db.execute("CALL dbms.createLabelIndex()");
			}

			// 2. Pre-fill primary data (Users)
			std::cout << " (Filling " << dataSize_ << " users...) " << std::flush;

			// [OPTIMIZATION] Use vector<pair> for faster setup
			std::vector<std::vector<std::pair<std::string, Value>>> batch;
			constexpr size_t BATCH_CHUNK = 1000; // Flush every 1000 items
			batch.reserve(BATCH_CHUNK);

			for (int i = 0; i < dataSize_; ++i) {
				std::vector<std::pair<std::string, Value>> props;
				props.reserve(1);
				props.emplace_back("uid", static_cast<int64_t>(i));

				batch.push_back(std::move(props));

				// Flush in chunks to keep memory usage low
				if (batch.size() >= BATCH_CHUNK) {
					db.createNodes("User", batch);
					batch.clear();
				}
			}
			if (!batch.empty()) {
				db.createNodes("User", batch);
			}

			// 3. Inject Noise Data
			// 4x Noise to make Full Scan visibly slower than Label Scan
			int noiseCount = dataSize_ * 4;
			std::cout << " (Filling " << noiseCount << " noise nodes...) " << std::flush;

			std::vector<std::string> noiseLabels = {"Product", "Order", "Category", "Log"};
			int perLabel = std::max(1, noiseCount / static_cast<int>(noiseLabels.size()));

			for (const auto &lbl: noiseLabels) {
				batch.clear();
				for (int i = 0; i < perLabel; ++i) {
					std::vector<std::pair<std::string, Value>> props;
					props.reserve(2);
					props.emplace_back("uid", static_cast<int64_t>(i)); // Collision on property to test filter speed
					props.emplace_back("noise", "true");

					batch.push_back(std::move(props));

					if (batch.size() >= BATCH_CHUNK) {
						db.createNodes(lbl, batch);
						batch.clear();
					}
				}
				if (!batch.empty()) {
					db.createNodes(lbl, batch);
				}
			}

			// 4. Flush to disk
			db.save();
		}

		void run(Database &db) override {
			int64_t searchId = rand() % dataSize_;
			std::string q;

			switch (mode_) {
				case QueryMode::PROPERTY_INDEX:
				case QueryMode::LABEL_SCAN:
					// Use Label to trigger Index/Scan
					q = "MATCH (n:User {uid: " + std::to_string(searchId) + "}) RETURN n.uid";
					break;
				case QueryMode::FULL_SCAN:
					// No Label -> Full Scan
					q = "MATCH (n {uid: " + std::to_string(searchId) + "}) RETURN n.uid";
					break;
			}

			auto res = db.execute(q);

			if (!res.hasNext()) {
				std::cerr << "\n[FATAL] ID " << searchId << " missing!\n";
				std::abort();
			}
		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Scenario 4: Graph Algorithms
	// Metric: Compute Latency
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
		void run(Database &db) override { db.getShortestPath(startId, endId); }
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
			db.execute(q);
		}
		void teardown(Database &) override {}
	};

} // namespace metrix::benchmark
