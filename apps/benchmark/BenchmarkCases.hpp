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
#include <string>
#include "BenchmarkFramework.hpp"

namespace metrix::benchmark {

	// ========================================================================
	// Scenario 1: Bulk Node Creation
	// Metric: Write Throughput (Ops/Sec)
	// Logic: 'run' inserts 1 node. Framework calls it 'iterations_' times.
	// ========================================================================

	class CypherInsertBench : public BenchmarkBase {
	public:
		using BenchmarkBase::BenchmarkBase;

		void setup(Database &) override {
			// Start with empty DB
		}

		void run(Database &db) override {
			// Uses Parser -> Planner -> Executor -> Storage
			db.execute("CREATE (n:BenchUser {id: 1, name: 'TestUser'})");
		}

		void teardown(Database &) override {}
	};

	class NativeInsertBench : public BenchmarkBase {
	public:
		using BenchmarkBase::BenchmarkBase;

		void setup(Database &) override {
			// Start with empty DB
		}

		void run(Database &db) override {
			// Bypass Parser, uses Internal Builder -> Executor -> Storage
			db.createNode("BenchUser", {{"id", static_cast<int64_t>(1)}, {"name", "TestUser"}});
		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Scenario 2: Index Lookup
	// Metric: Read Latency (Avg/P99)
	// Logic: 'setup' populates 'dataSize_' nodes. 'run' performs 1 random lookup.
	// ========================================================================

	enum class QueryMode {
		PROPERTY_INDEX, // Best: Use specific index on uid
		LABEL_SCAN, // Baseline: Use Label Index (Default ON)
		FULL_SCAN // Worst: Scan entire DB (No Label)
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
			}

			// 2. Pre-fill data
			std::cout << " (Pre-filling " << dataSize_ << " nodes) " << std::flush;
			for (int i = 0; i < dataSize_; ++i) {
				db.createNode("User", {{"uid", static_cast<int64_t>(i)}});
			}

			// Add some noise (non-User nodes) to verify Full Scan overhead
			// If we have 50k Users and 50k Products, Full Scan will check 100k items.
			// Label Scan will only check 50k.
			for (int i = 0; i < dataSize_ / 2; ++i) {
				db.createNode("Product", {{"uid", static_cast<int64_t>(i)}});
			}

			db.save();
		}

		void run(Database &db) override {
			int64_t searchId = rand() % dataSize_;
			std::string q;

			// [OPTIMIZATION] Return n.uid directly to verify value easily
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

			// 1. Check Existence
			if (!res.hasNext()) {
				std::cerr << "\n[FATAL] ID " << searchId << " missing!\n";
				std::abort();
			}

			// 2. Check Value
			auto val = res.get("n.uid"); // Requires ProjectOperator supporting "n.uid"

			// Check variant holds int64_t and matches searchId
			try {
				int64_t foundId = std::get<int64_t>(val);
				if (foundId != searchId) {
					std::cerr << "\n[FATAL] Data Mismatch! Expected: " << searchId << " Got: " << foundId << "\n";
					std::abort();
				}
			} catch (...) {
				// Handle type mismatch or empty monostate
				std::cerr << "\n[FATAL] Invalid return type for ID!\n";
				std::abort();
			}
		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Scenario 3: Graph Algorithms (Shortest Path)
	// Metric: Compute Latency
	// Logic: 'setup' creates a chain of length 'dataSize_'. 'run' finds path from start to end.
	// ========================================================================

	class AlgoShortestPathBench : public BenchmarkBase {
		int64_t startId = 0;
		int64_t endId = 0;

	public:
		using BenchmarkBase::BenchmarkBase;

		void setup(Database &db) override {
			int chainLen = dataSize_;

			std::cout << " (Building chain of " << chainLen << " nodes) " << std::flush;

			int64_t prevId = -1;

			for (int i = 0; i < chainLen; ++i) {
				// Use fast API for setup
				int64_t curr = db.createNodeRetId("Node", {{"id", static_cast<int64_t>(i)}});

				if (prevId != -1) {
					db.createEdgeById(prevId, curr, "NEXT");
				}

				prevId = curr;

				if (i == 0)
					startId = curr;
				if (i == chainLen - 1)
					endId = curr;
			}
			db.save();
		}

		void run(Database &db) override {
			// Native API Call (Bypasses Parser/Planner)
			db.getShortestPath(startId, endId);
		}

		void teardown(Database &) override {}
	};

	class CypherShortestPathBench : public BenchmarkBase {
		int64_t startId = 0;
		int64_t endId = 0;

	public:
		using BenchmarkBase::BenchmarkBase;

		void setup(Database &db) override {
			int chainLen = dataSize_;

			std::cout << " (Building chain of " << chainLen << " nodes) " << std::flush;

			int64_t prevId = -1;
			for (int i = 0; i < chainLen; ++i) {
				int64_t curr = db.createNodeRetId("Node", {{"id", static_cast<int64_t>(i)}});
				if (prevId != -1) {
					db.createEdgeById(prevId, curr, "NEXT");
				}
				prevId = curr;
				if (i == 0)
					startId = curr;
				if (i == chainLen - 1)
					endId = curr;
			}
			db.save();
		}

		void run(Database &db) override {
			// Cypher Procedure Call (Includes Parsing/Planning overhead)
			std::string q = "CALL algo.shortestPath(" + std::to_string(startId) + ", " + std::to_string(endId) + ")";
			db.execute(q);
		}

		void teardown(Database &) override {}
	};

} // namespace metrix::benchmark
