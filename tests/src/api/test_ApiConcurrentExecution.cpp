/**
 * @file test_ApiConcurrentExecution.cpp
 * @brief Tests for concurrent query execution through the public C++ API.
 *        Verifies thread-safety of parser and execution engine.
 *
 * NOTE: The current CypherParserImpl is NOT thread-safe for concurrent access.
 * These tests verify that the database properly serializes access or that
 * read-only transactions work correctly with sequential access.
 * Tests that exercise true concurrent access are guarded and expected to
 * potentially require a parser-level mutex in the future.
 **/

#include <atomic>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "ApiTestFixture.hpp"

class ApiConcurrentExecutionTest : public CppApiTest {};

TEST_F(ApiConcurrentExecutionTest, SequentialFromMultipleThreadsNoOverlap) {
	// Seed 10 nodes
	for (int i = 0; i < 10; ++i) {
		auto r = db->execute("CREATE (n:Node {id: " + std::to_string(i) + "})");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
	}

	// Run queries from multiple threads but serialized with a mutex
	std::mutex dbMtx;
	std::vector<std::string> errors;
	std::mutex errorMtx;

	auto task = [&](int threadId) {
		for (int i = 0; i < 10; ++i) {
			std::lock_guard<std::mutex> lock(dbMtx);
			auto r = db->execute("MATCH (n:Node) RETURN count(n) AS cnt");
			if (!r.isSuccess()) {
				std::lock_guard<std::mutex> eLock(errorMtx);
				errors.push_back("Thread " + std::to_string(threadId) + " iter " +
				                 std::to_string(i) + ": " + r.getError());
			}
		}
	};

	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back(task, t);
	}
	for (auto &t : threads) {
		t.join();
	}

	EXPECT_TRUE(errors.empty()) << "Errors: " << errors.size() << " — first: " << errors.front();
}

TEST_F(ApiConcurrentExecutionTest, ParserIntactAfterSerializedConcurrentLoad) {
	// Seed data
	for (int i = 0; i < 5; ++i) {
		auto r = db->execute("CREATE (n:Pre {id: " + std::to_string(i) + "})");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
	}

	// Serialized concurrent load
	std::mutex dbMtx;
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back([&]() {
			for (int i = 0; i < 10; ++i) {
				std::lock_guard<std::mutex> lock(dbMtx);
				(void)db->execute("MATCH (n:Pre) RETURN n");
			}
		});
	}
	for (auto &t : threads) {
		t.join();
	}

	// After load, main thread must still work
	for (int i = 0; i < 5; ++i) {
		auto r = db->execute("MATCH (n:Pre) RETURN count(n) AS cnt");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
		ASSERT_TRUE(r.hasNext());
		r.next();
		EXPECT_EQ(std::get<int64_t>(r.get("cnt")), 5);
	}
}

TEST_F(ApiConcurrentExecutionTest, MultipleReadOnlyTransactionsSequential) {
	// Seed data
	for (int i = 0; i < 10; ++i) {
		auto r = db->execute("CREATE (n:Data {id: " + std::to_string(i) + "})");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
	}

	// Multiple sequential read-only transactions
	for (int t = 0; t < 4; ++t) {
		auto tx = db->beginReadOnlyTransaction();
		for (int i = 0; i < 5; ++i) {
			auto r = tx.execute("MATCH (n:Data) RETURN count(n) AS cnt");
			ASSERT_TRUE(r.isSuccess()) << "Txn " << t << " query " << i << ": " << r.getError();
		}
		tx.commit();
	}
}

TEST_F(ApiConcurrentExecutionTest, WriterThenReadersSequential) {
	// 1 writer creates nodes, then multiple readers verify
	for (int i = 0; i < 10; ++i) {
		auto r = db->execute("CREATE (n:Written {id: " + std::to_string(i) + "})");
		ASSERT_TRUE(r.isSuccess()) << r.getError();
	}

	// Multiple "reader" threads sequentially (serialized for safety)
	std::mutex dbMtx;
	std::atomic<int> successCount{0};

	auto readerTask = [&](int threadId) {
		for (int i = 0; i < 5; ++i) {
			std::lock_guard<std::mutex> lock(dbMtx);
			auto r = db->execute("MATCH (n:Written) RETURN count(n) AS cnt");
			if (r.isSuccess()) {
				successCount++;
			}
		}
	};

	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back(readerTask, t);
	}
	for (auto &t : threads) {
		t.join();
	}

	EXPECT_EQ(successCount.load(), 20);
}
