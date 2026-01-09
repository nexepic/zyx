/**
 * @file test_RegistryConcurrency.cpp
 * @author Nexepic
 * @date 2025/12/2
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

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <random>
#include <thread>
#include <vector>
#include "graph/core/Node.hpp"
#include "graph/storage/DirtyEntityRegistry.hpp"

using namespace graph::storage;
using namespace graph;

class RegistryConcurrencyTest : public ::testing::Test {
protected:
	DirtyEntityRegistry<Node> registry;

	static Node createNode(int64_t id, const std::string &label) {
		Node node;
		node.setId(id);
		node.setLabel(label);
		return node;
	}
};

// SCENARIO 1: Multi-threaded Writers
// Multiple threads writing distinct entities simultaneously.
// Verifies: Mutex protection on upsert.
TEST_F(RegistryConcurrencyTest, MultiThreadedWrites) {
	constexpr int NUM_THREADS = 10;
	constexpr int OPS_PER_THREAD = 1000;

	std::vector<std::thread> threads;
	std::atomic activeThreads{0};

	// Lambda for worker thread
	auto worker = [&](int threadId) {
		++activeThreads;
		for (int i = 0; i < OPS_PER_THREAD; ++i) {
			int64_t id = threadId * OPS_PER_THREAD + i;
			auto info = DirtyEntityInfo<Node>(EntityChangeType::ADDED, createNode(id, "T" + std::to_string(threadId)));
			registry.upsert(info);
		}
		--activeThreads;
	};

	// Launch threads
	threads.reserve(NUM_THREADS);
	for (int i = 0; i < NUM_THREADS; ++i) {
		threads.emplace_back(worker, i);
	}

	// Join threads
	for (auto &t: threads) {
		t.join();
	}

	// Verification
	EXPECT_EQ(registry.size(), static_cast<size_t>(NUM_THREADS * OPS_PER_THREAD));

	// Check random sample
	auto info = registry.getInfo(55); // Thread 0, item 55
	ASSERT_TRUE(info.has_value());
	EXPECT_EQ(info->backup->getLabel(), "T0");
}

// SCENARIO 2: The "Hot Flush" Test
// One thread constantly updates a specific set of entities.
// Another thread constantly Flushes (Snapshots and Commits).
// Verifies: Locking mechanism between Active and Flushing layers.
TEST_F(RegistryConcurrencyTest, ConcurrentWriteAndFlush) {
	std::atomic<bool> running{true};
	constexpr int64_t ENTITY_ID = 1;
	constexpr int ITERATIONS = 10000;

	// Writer Thread: Continuously updates ID 1 with increasing version numbers
	std::thread writer([&]() {
		for (int i = 0; i < ITERATIONS; ++i) {
			auto info =
					DirtyEntityInfo<Node>(EntityChangeType::MODIFIED, createNode(ENTITY_ID, "V" + std::to_string(i)));
			registry.upsert(info);
			// Small yield to allow flusher to intervene
			if (i % 100 == 0)
				std::this_thread::yield();
		}
		running = false;
	});

	// Flusher Thread: Continuously moves data to flushing and commits it
	// This simulates the FileStorage::save() loop running in parallel
	std::thread flusher([&]() {
		while (running) {
			// 1. Snapshot
			auto snapshot = registry.createFlushSnapshot();

			// Simulate IO delay (very short)
			std::this_thread::sleep_for(std::chrono::microseconds(10));

			// 2. Commit
			registry.commitFlush();
		}
		// Do one final cleanup
		registry.createFlushSnapshot();
		registry.commitFlush();
	});

	writer.join();
	flusher.join();

	// Verification
	// After everything is done, the registry might be empty (if last flush caught the last write)
	// or contain the very last write (if write happened after last snapshot).
	// The key checks are:
	// 1. The program didn't crash (Segfault/Deadlock).
	// 2. Internal counters are consistent.

	// We add a new write now to verify registry is still usable
	registry.upsert(DirtyEntityInfo<Node>(EntityChangeType::ADDED, createNode(2, "Final")));
	EXPECT_TRUE(registry.contains(2));
}

// SCENARIO 3: Reader vs Writer vs Flusher
// Simulates a heavy load production environment.
// - Writer: Adds new data.
// - Flusher: Persists data.
// - Reader: Reads data (Query Engine).
TEST_F(RegistryConcurrencyTest, ReaderWriterFlusherRace) {
	std::atomic<bool> stop{false};
	constexpr int RANGE = 1000;

	// 1. Writer: Writes IDs 0-999 randomly
	std::thread writer([&]() {
		std::mt19937 gen(1);
		std::uniform_int_distribution<> dis(0, RANGE - 1);
		while (!stop) {
			int64_t id = dis(gen);
			registry.upsert(DirtyEntityInfo<Node>(EntityChangeType::MODIFIED, createNode(id, "Updated")));
		}
	});

	// 2. Reader: Reads IDs 0-999 randomly
	std::thread reader([&]() {
		std::mt19937 gen(2);
		std::uniform_int_distribution<> dis(0, RANGE - 1);
		while (!stop) {
			int64_t id = dis(gen);
			// Just access it to test read-lock contention
			auto info = registry.getInfo(id);
			if (info.has_value()) {
				volatile auto val = info->backup->getId(); // Force read
				(void) val;
			}
		}
	});

	// 3. Flusher: Periodically flushes
	std::thread flusher([&]() {
		while (!stop) {
			auto snap = registry.createFlushSnapshot();
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // IO simulation
			registry.commitFlush();
			std::this_thread::sleep_for(std::chrono::milliseconds(2)); // Gap between flushes
		}
	});

	// Run for 200ms
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	stop = true;

	writer.join();
	reader.join();
	flusher.join();

	// Verify consistency: Check count
	// It's hard to predict exact count, but we ensure basic sanity
	registry.upsert(DirtyEntityInfo<Node>(EntityChangeType::ADDED, createNode(10001, "Check")));
	auto check = registry.getInfo(10001);
	ASSERT_TRUE(check.has_value());
	EXPECT_EQ(check->backup->getLabel(), "Check");
}
