/**
 * @file BenchmarkOptimizer.hpp
 * @brief Benchmark cases designed to measure query optimizer effectiveness.
 *
 * These benchmarks exercise scenarios where the optimizer rules can make a
 * measurable difference:
 *  - FilterPushdownRule: WHERE clause filter pushed into NodeScan
 *  - ProjectionPushdownRule: Reducing intermediate row widths
 *  - Multi-condition AND splitting + pushdown
 *  - Multi-pattern MATCH (cross-join) with selective filters
 *
 * Licensed under the Apache License, Version 2.0
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
	// Scenario A: WHERE clause filter pushdown into NodeScan
	//
	// Query: MATCH (n:User) WHERE n.uid = X RETURN n.uid
	// Without optimizer: scan all User nodes, then filter
	// With optimizer: FilterPushdownRule merges n.uid=X into scan predicates
	//                 → property index lookup
	// ========================================================================

	class OptimizerFilterPushdownBench : public BenchmarkBase {
	public:
		OptimizerFilterPushdownBench(std::string name, std::string path, int iter, int dataSize) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize) {}

		void setup(Database &db) override {
			// Create property index on uid
			(void) db.execute("CREATE INDEX ON :User(uid)");

			// Pre-fill data
			std::cout << " (Filling " << dataSize_ << " users...) " << std::flush;
			std::vector<std::unordered_map<std::string, Value>> batch;
			constexpr size_t BATCH_CHUNK = 1000;
			batch.reserve(BATCH_CHUNK);

			for (int i = 0; i < dataSize_; ++i) {
				std::unordered_map<std::string, Value> props;
				props["uid"] = static_cast<int64_t>(i);
				props["name"] = "User_" + std::to_string(i);
				props["age"] = static_cast<int64_t>(20 + (i % 50));
				batch.push_back(std::move(props));

				if (batch.size() >= BATCH_CHUNK) {
					db.createNodes("User", batch);
					batch.clear();
				}
			}
			if (!batch.empty())
				db.createNodes("User", batch);

			// Add noise
			int noiseCount = dataSize_ * 4;
			std::cout << " (Filling " << noiseCount << " noise...) " << std::flush;
			std::vector<std::string> noiseLabels = {"Product", "Order", "Category", "Log"};
			int perLabel = std::max(1, noiseCount / static_cast<int>(noiseLabels.size()));
			for (const auto &lbl : noiseLabels) {
				batch.clear();
				for (int i = 0; i < perLabel; ++i) {
					std::unordered_map<std::string, Value> props;
					props["uid"] = static_cast<int64_t>(i);
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
			// Uses WHERE clause — optimizer can push filter into scan
			std::string q = "MATCH (n:User) WHERE n.uid = " + std::to_string(searchId) + " RETURN n.uid";
			auto res = db.execute(q);
			if (!res.hasNext()) {
				std::cerr << "\n[FATAL] WHERE pushdown: ID " << searchId << " missing!\n";
				std::abort();
			}
		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Scenario B: Multi-condition AND filter splitting
	//
	// Query: MATCH (n:User) WHERE n.age > 25 AND n.uid < 100 RETURN n.uid
	// Without optimizer: single filter with AND predicate
	// With optimizer: AND is split into two filters, each pushed closer to scan
	// ========================================================================

	class OptimizerAndSplitBench : public BenchmarkBase {
	public:
		OptimizerAndSplitBench(std::string name, std::string path, int iter, int dataSize) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize) {}

		void setup(Database &db) override {
			(void) db.execute("CALL dbms.createLabelIndex()");

			std::cout << " (Filling " << dataSize_ << " users...) " << std::flush;
			std::vector<std::unordered_map<std::string, Value>> batch;
			constexpr size_t BATCH_CHUNK = 1000;
			batch.reserve(BATCH_CHUNK);

			for (int i = 0; i < dataSize_; ++i) {
				std::unordered_map<std::string, Value> props;
				props["uid"] = static_cast<int64_t>(i);
				props["age"] = static_cast<int64_t>(18 + (i % 60));
				props["city"] = (i % 3 == 0) ? "NYC" : ((i % 3 == 1) ? "LA" : "SF");
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
			// Multi-condition AND: optimizer splits and pushes each independently
			std::string q = "MATCH (n:User) WHERE n.age > 30 AND n.uid < " +
			                std::to_string(dataSize_ / 2) + " RETURN n.uid, n.age";
			auto res = db.execute(q);
			// Consume results
			while (res.hasNext()) {
				(void) res.next();
			}
		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Scenario C: Multi-pattern MATCH (cross join) with selective filter
	//
	// Query: MATCH (a:User), (b:Product) WHERE a.uid = X RETURN a.uid, b.uid
	// Without optimizer: cross join all Users × Products, then filter
	// With optimizer: FilterPushdownRule pushes a.uid=X to left side of join
	//                 → only 1 user × N products instead of N × N
	// ========================================================================

	class OptimizerJoinFilterBench : public BenchmarkBase {
		int userCount_;
		int productCount_;

	public:
		OptimizerJoinFilterBench(std::string name, std::string path, int iter,
		                          int userCount, int productCount) :
			BenchmarkBase(std::move(name), std::move(path), iter, userCount),
			userCount_(userCount), productCount_(productCount) {}

		void setup(Database &db) override {
			(void) db.execute("CREATE INDEX ON :User(uid)");
			(void) db.execute("CALL dbms.createLabelIndex()");

			std::cout << " (Filling " << userCount_ << " users + " << productCount_
			          << " products...) " << std::flush;

			std::vector<std::unordered_map<std::string, Value>> batch;
			constexpr size_t BATCH_CHUNK = 500;

			// Create users
			batch.clear();
			for (int i = 0; i < userCount_; ++i) {
				std::unordered_map<std::string, Value> props;
				props["uid"] = static_cast<int64_t>(i);
				batch.push_back(std::move(props));
				if (batch.size() >= BATCH_CHUNK) {
					db.createNodes("User", batch);
					batch.clear();
				}
			}
			if (!batch.empty())
				db.createNodes("User", batch);

			// Create products
			batch.clear();
			for (int i = 0; i < productCount_; ++i) {
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
			int64_t searchId = rand() % userCount_;
			// Cross-join with selective filter on left side
			// Optimizer pushes a.uid=X into the User scan
			std::string q = "MATCH (a:User), (b:Product) WHERE a.uid = " +
			                std::to_string(searchId) + " RETURN a.uid, b.uid";
			auto res = db.execute(q);
			int count = 0;
			while (res.hasNext()) {
				(void) res.next();
				++count;
			}
			if (count == 0) {
				std::cerr << "\n[WARN] JoinFilter: 0 results for uid=" << searchId << "\n";
			}
		}

		void teardown(Database &) override {}
	};

	// ========================================================================
	// Scenario D: Inline property (baseline) vs WHERE filter (optimizer target)
	//
	// Query: MATCH (n:User {uid: X}) RETURN n.uid  (inline, no optimizer needed)
	//    vs: MATCH (n:User) WHERE n.uid = X RETURN n.uid  (optimizer pushes down)
	//
	// This pair allows direct comparison of the optimizer's filter-into-scan
	// pushdown effectiveness against the hand-optimized inline form.
	// ========================================================================

	class OptimizerInlineBaselineBench : public BenchmarkBase {
	public:
		OptimizerInlineBaselineBench(std::string name, std::string path, int iter, int dataSize) :
			BenchmarkBase(std::move(name), std::move(path), iter, dataSize) {}

		void setup(Database &db) override {
			(void) db.execute("CREATE INDEX ON :User(uid)");

			std::cout << " (Filling " << dataSize_ << " users...) " << std::flush;
			std::vector<std::unordered_map<std::string, Value>> batch;
			constexpr size_t BATCH_CHUNK = 1000;
			batch.reserve(BATCH_CHUNK);

			for (int i = 0; i < dataSize_; ++i) {
				std::unordered_map<std::string, Value> props;
				props["uid"] = static_cast<int64_t>(i);
				batch.push_back(std::move(props));
				if (batch.size() >= BATCH_CHUNK) {
					db.createNodes("User", batch);
					batch.clear();
				}
			}
			if (!batch.empty())
				db.createNodes("User", batch);

			// Add noise
			int noiseCount = dataSize_ * 4;
			std::cout << " (Filling " << noiseCount << " noise...) " << std::flush;
			std::vector<std::string> noiseLabels = {"Product", "Order", "Category", "Log"};
			int perLabel = std::max(1, noiseCount / static_cast<int>(noiseLabels.size()));
			for (const auto &lbl : noiseLabels) {
				batch.clear();
				for (int i = 0; i < perLabel; ++i) {
					std::unordered_map<std::string, Value> props;
					props["uid"] = static_cast<int64_t>(i);
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
			// Inline property — already in scan, optimizer has nothing to push
			std::string q = "MATCH (n:User {uid: " + std::to_string(searchId) + "}) RETURN n.uid";
			auto res = db.execute(q);
			if (!res.hasNext()) {
				std::cerr << "\n[FATAL] Inline baseline: ID " << searchId << " missing!\n";
				std::abort();
			}
		}

		void teardown(Database &) override {}
	};

} // namespace zyx::benchmark
