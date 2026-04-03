/**
 * @file test_KMeans.cpp
 * @author Nexepic
 * @date 2026/1/26
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
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/vector/quantization/KMeans.hpp"

using namespace graph::vector;

TEST(KMeansTest, SimpleClustering) {
	// Create 2 clusters: around (0,0) and (10,10)
	std::vector<std::vector<float>> data;
	for(int i=0; i<10; ++i) data.push_back({0.1f * i, 0.1f * i});
	for(int i=0; i<10; ++i) data.push_back({10.0f + 0.1f * i, 10.0f + 0.1f * i});

	size_t K = 2;
	auto centroids = KMeans::run(data, K, 10);

	ASSERT_EQ(centroids.size(), K);

	// Check if one centroid is near 0,0 and other near 10,10
	bool foundZero = false;
	bool foundTen = false;

	for(const auto& c : centroids) {
		float sum = c[0] + c[1];
		if (sum < 5.0f) foundZero = true;
		if (sum > 15.0f) foundTen = true;
	}

	EXPECT_TRUE(foundZero);
	EXPECT_TRUE(foundTen);
}

TEST(KMeansTest, EmptyData) {
	std::vector<std::vector<float>> data;
	auto res = KMeans::run(data, 5);
	EXPECT_TRUE(res.empty());
}

TEST(KMeansTest, ParallelPathWithThreadPool) {
	std::vector<std::vector<float>> data;
	data.reserve(256);
	for (int i = 0; i < 256; ++i) {
		const float v = static_cast<float>(i) * 0.25f;
		data.push_back({v, v * 0.5f});
	}

	graph::concurrent::ThreadPool pool(4);
	auto centroids = KMeans::run(data, 8, 5, &pool);

	ASSERT_EQ(centroids.size(), 8UL);
	for (const auto &c : centroids) {
		EXPECT_EQ(c.size(), 2UL);
	}
}

TEST(KMeansTest, ParallelPathReseedsEmptyClustersWhenKExceedsDataSize) {
	std::vector<std::vector<float>> data;
	data.reserve(256);
	for (int i = 0; i < 256; ++i) {
		data.push_back({static_cast<float>(i), static_cast<float>(i % 7)});
	}

	graph::concurrent::ThreadPool pool(4);
	auto centroids = KMeans::run(data, 300, 3, &pool);

	ASSERT_EQ(centroids.size(), 300UL);
	for (const auto &c : centroids) {
		EXPECT_EQ(c.size(), 2UL);
	}
}
