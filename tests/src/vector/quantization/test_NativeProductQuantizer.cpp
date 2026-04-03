/**
 * @file test_NativeProductQuantizer.cpp
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
#include <sstream>
#include <random>
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/vector/quantization/NativeProductQuantizer.hpp"

using namespace graph::vector;

class NativePQTest : public ::testing::Test {
protected:
	void SetUp() override {
		std::mt19937 rng(42);
		std::uniform_real_distribution<float> dist(0.0f, 1.0f);
		for (int i = 0; i < 100; ++i) {
			std::vector<float> v(16);
			for (auto &x: v)
				x = dist(rng);
			trainData.push_back(v);
		}
	}
	std::vector<std::vector<float>> trainData;
};

TEST_F(NativePQTest, TrainAndEncode) {
	// Dim=16, M=4 (4 subspaces, 4 dims each), 256 centroids
	NativeProductQuantizer pq(16, 4, 16);
	pq.train(trainData);
	EXPECT_TRUE(pq.isTrained());

	std::vector<float> vec(16, 0.5f);
	auto codes = pq.encode(vec);
	EXPECT_EQ(codes.size(), 4UL);
}

TEST_F(NativePQTest, EncodeWithoutTrain_Throws) {
	NativeProductQuantizer pq(16, 4);
	std::vector<float> vec(16);
	EXPECT_THROW((void) pq.encode(vec), std::runtime_error);
}

TEST_F(NativePQTest, DimensionMismatch_Throws) {
	NativeProductQuantizer pq(16, 4);
	pq.train(trainData);
	std::vector<float> badVec(15);
	EXPECT_THROW((void) pq.encode(badVec), std::runtime_error);
}

TEST_F(NativePQTest, ExceptionPaths) {
	NativeProductQuantizer pq(16, 4);

	std::vector<float> vec(16, 0.0f);

	// 1. Encode before Train
	EXPECT_THROW({ (void) pq.encode(vec); }, std::runtime_error); // "PQ not trained"

	// Train it
	pq.train(trainData);

	// 2. Dimension Mismatch
	std::vector<float> badVec(10, 0.0f);
	EXPECT_THROW({ (void) pq.encode(badVec); }, std::runtime_error); // "Encode vector dimension mismatch"

	// 3. Invalid Constructor (Dim not divisible by subspaces)
	EXPECT_THROW(
			{
				NativeProductQuantizer badPQ(10, 3); // 10 % 3 != 0
			},
			std::invalid_argument);
}

TEST_F(NativePQTest, TrainWithEmptyDataReturnsEarly) {
	NativeProductQuantizer pq(16, 4, 8);
	pq.train({});
	EXPECT_FALSE(pq.isTrained());
}

TEST_F(NativePQTest, TrainWithDimensionMismatchThrows) {
	NativeProductQuantizer pq(8, 2, 4);
	std::vector<std::vector<float>> badData = {
		{1.0f, 2.0f, 3.0f}
	};
	EXPECT_THROW(pq.train(badData), std::runtime_error);
}

TEST_F(NativePQTest, ParallelTrainEncodeAndDistanceTablePath) {
	std::vector<std::vector<float>> data(12, std::vector<float>(64, 0.0f));
	for (size_t i = 0; i < data.size(); ++i) {
		for (size_t d = 0; d < data[i].size(); ++d) {
			data[i][d] = static_cast<float>((i + d) % 7) / 7.0f;
		}
	}

	NativeProductQuantizer pq(64, 64, 4);
	graph::concurrent::ThreadPool pool(2);
	pq.train(data, &pool);
	ASSERT_TRUE(pq.isTrained());

	std::vector<float> query(64, 0.25f);
	auto codes = pq.encode(query, &pool);
	EXPECT_EQ(codes.size(), 64UL);

	auto table = pq.computeDistanceTable(query, &pool);
	EXPECT_EQ(table.size(), 64UL * 4UL);
}

TEST_F(NativePQTest, SerializeAndDeserializeUntrainedQuantizer) {
	NativeProductQuantizer pq(16, 4, 8);

	std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
	pq.serialize(ss);
	ss.seekg(0);

	auto restored = NativeProductQuantizer::deserialize(ss);
	ASSERT_NE(restored, nullptr);
	EXPECT_FALSE(restored->isTrained());

	std::vector<float> vec(16, 0.5f);
	EXPECT_THROW((void)restored->encode(vec), std::runtime_error);
}
