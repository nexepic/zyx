/**
 * @file test_PageBufferPool.cpp
 * @date 2026/3/31
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

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include "graph/storage/PageBufferPool.hpp"

using graph::storage::PageBufferPool;

// Basic put and get
TEST(PageBufferPoolTest, PutAndGet) {
	PageBufferPool pool(4);
	std::vector<uint8_t> data(1152, 0xAA);
	pool.putPage(1000, std::vector<uint8_t>(data));

	auto *page = pool.getPage(1000);
	ASSERT_NE(page, nullptr);
	EXPECT_EQ(page->segmentOffset, 1000u);
	EXPECT_EQ(page->data.size(), 1152u);
	EXPECT_EQ(page->data[0], 0xAA);
	EXPECT_EQ(pool.size(), 1u);
}

// Miss returns nullptr
TEST(PageBufferPoolTest, MissReturnsNull) {
	PageBufferPool pool(4);
	EXPECT_EQ(pool.getPage(999), nullptr);
}

// LRU eviction
TEST(PageBufferPoolTest, LRUEviction) {
	PageBufferPool pool(2);
	pool.putPage(100, std::vector<uint8_t>(10, 0x01));
	pool.putPage(200, std::vector<uint8_t>(10, 0x02));

	// Both present
	EXPECT_NE(pool.getPage(100), nullptr);
	EXPECT_NE(pool.getPage(200), nullptr);

	// Insert a third page — evicts LRU (100 was accessed before 200, then 200, so 100 is LRU)
	// After getPage(100) then getPage(200): order is [200, 100]. LRU = 100.
	pool.putPage(300, std::vector<uint8_t>(10, 0x03));
	EXPECT_EQ(pool.size(), 2u);
	EXPECT_EQ(pool.getPage(100), nullptr); // evicted
	EXPECT_NE(pool.getPage(200), nullptr);
	EXPECT_NE(pool.getPage(300), nullptr);
}

// Update existing page
TEST(PageBufferPoolTest, UpdateExistingPage) {
	PageBufferPool pool(4);
	pool.putPage(100, std::vector<uint8_t>(10, 0x01));
	pool.putPage(100, std::vector<uint8_t>(20, 0x02));

	EXPECT_EQ(pool.size(), 1u);
	auto *page = pool.getPage(100);
	ASSERT_NE(page, nullptr);
	EXPECT_EQ(page->data.size(), 20u);
	EXPECT_EQ(page->data[0], 0x02);
}

// Invalidate
TEST(PageBufferPoolTest, Invalidate) {
	PageBufferPool pool(4);
	pool.putPage(100, std::vector<uint8_t>(10, 0x01));
	pool.putPage(200, std::vector<uint8_t>(10, 0x02));

	pool.invalidate(100);
	EXPECT_EQ(pool.size(), 1u);
	EXPECT_EQ(pool.getPage(100), nullptr);
	EXPECT_NE(pool.getPage(200), nullptr);
}

// Invalidate nonexistent key is a no-op
TEST(PageBufferPoolTest, InvalidateNonexistent) {
	PageBufferPool pool(4);
	pool.putPage(100, std::vector<uint8_t>(10, 0x01));
	pool.invalidate(999);
	EXPECT_EQ(pool.size(), 1u);
}

// Clear
TEST(PageBufferPoolTest, Clear) {
	PageBufferPool pool(4);
	pool.putPage(100, std::vector<uint8_t>(10, 0x01));
	pool.putPage(200, std::vector<uint8_t>(10, 0x02));
	pool.clear();
	EXPECT_EQ(pool.size(), 0u);
	EXPECT_EQ(pool.getPage(100), nullptr);
	EXPECT_EQ(pool.getPage(200), nullptr);
}

// Zero capacity
TEST(PageBufferPoolTest, ZeroCapacity) {
	PageBufferPool pool(0);
	pool.putPage(100, std::vector<uint8_t>(10, 0x01));
	EXPECT_EQ(pool.size(), 0u);
	EXPECT_EQ(pool.getPage(100), nullptr);
}

// LRU promotion: getPage moves page to front
TEST(PageBufferPoolTest, GetPromotesToFront) {
	PageBufferPool pool(2);
	pool.putPage(100, std::vector<uint8_t>(10, 0x01));
	pool.putPage(200, std::vector<uint8_t>(10, 0x02));

	// Access 100 to promote it, making 200 the LRU
	pool.getPage(100);

	// Insert 300 — should evict 200 (LRU), not 100
	pool.putPage(300, std::vector<uint8_t>(10, 0x03));
	EXPECT_NE(pool.getPage(100), nullptr);
	EXPECT_EQ(pool.getPage(200), nullptr); // evicted
	EXPECT_NE(pool.getPage(300), nullptr);
}

// Concurrent reads and writes
TEST(PageBufferPoolTest, ConcurrentAccess) {
	PageBufferPool pool(100);

	// Pre-populate
	for (uint64_t i = 0; i < 50; ++i) {
		pool.putPage(i * 1152, std::vector<uint8_t>(1152, static_cast<uint8_t>(i)));
	}

	std::vector<std::thread> threads;
	// Reader threads
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back([&pool]() {
			for (int i = 0; i < 200; ++i) {
				uint64_t offset = (i % 50) * 1152;
				pool.getPage(offset);
			}
		});
	}
	// Writer threads
	for (int t = 0; t < 2; ++t) {
		threads.emplace_back([&pool, t]() {
			for (int i = 0; i < 100; ++i) {
				uint64_t offset = (50 + t * 100 + i) * 1152;
				pool.putPage(offset, std::vector<uint8_t>(1152, static_cast<uint8_t>(i)));
			}
		});
	}
	// Invalidator thread
	threads.emplace_back([&pool]() {
		for (int i = 0; i < 50; ++i) {
			pool.invalidate(static_cast<uint64_t>(i) * 1152);
		}
	});

	for (auto &th : threads) {
		th.join();
	}

	// Just verify no crash and size is within bounds
	EXPECT_LE(pool.size(), 100u);
}

// putPage with move semantics
TEST(PageBufferPoolTest, MoveSemantics) {
	PageBufferPool pool(4);
	std::vector<uint8_t> data(1152, 0xFF);
	auto *dataPtr = data.data();
	pool.putPage(100, std::move(data));

	auto *page = pool.getPage(100);
	ASSERT_NE(page, nullptr);
	// The data pointer should be the same (moved, not copied)
	EXPECT_EQ(page->data.data(), dataPtr);
}
