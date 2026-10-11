/**
 * @file test_SingleWriterSaturatingCounter.cpp
 * @brief Unit tests for serialized maintenance metric saturation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/maintenance/SingleWriterSaturatingCounter.hpp"

using graph::storage::SingleWriterSaturatingCounter;

TEST(SingleWriterSaturatingCounterTest, AddsAndIncrementsWithoutOverflow) {
	SingleWriterSaturatingCounter counter(4U);

	counter.add(7U);
	counter.increment();

	EXPECT_EQ(counter.load(), 12U);
}

TEST(SingleWriterSaturatingCounterTest, SaturatesAtTheLargestRepresentableValue) {
	SingleWriterSaturatingCounter counter(std::numeric_limits<uint64_t>::max() - 1U);

	counter.add(2U);
	EXPECT_EQ(counter.load(), std::numeric_limits<uint64_t>::max());

	counter.increment();
	EXPECT_EQ(counter.load(), std::numeric_limits<uint64_t>::max());
}

TEST(SingleWriterSaturatingCounterTest, ObservedProgressPublishesPriorSingleWriterEffects) {
	SingleWriterSaturatingCounter completed;
	std::atomic<uint64_t> publishedResult{0U};

	std::thread producer([&] {
		publishedResult.store(77U, std::memory_order_relaxed);
		completed.increment();
	});
	while (completed.load() == 0U) {
		std::this_thread::yield();
	}

	// `completed` is the completion publication boundary. Once it is observed,
	// the result written by the serialized producer must also be observable.
	EXPECT_EQ(publishedResult.load(std::memory_order_relaxed), 77U);
	producer.join();
}
