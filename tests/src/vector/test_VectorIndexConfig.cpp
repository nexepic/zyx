/**
 * @file test_VectorIndexConfig.cpp
 * @author Nexepic
 * @date 2025/1/29
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

#include <gtest/gtest.h>
#include <unordered_map>
#include "graph/core/PropertyTypes.hpp"
#include "graph/vector/VectorIndexConfig.hpp"

class VectorIndexConfigTest : public ::testing::Test {
protected:
	void SetUp() override {
		// No setup needed
	}

	void TearDown() override {
		// No cleanup needed
	}
};

TEST_F(VectorIndexConfigTest, DefaultConstructor) {
	graph::vector::VectorIndexConfig config;

	EXPECT_EQ(config.dimension, 0U);
	EXPECT_EQ(config.metricType, 0U);
	EXPECT_EQ(config.pqSubspaces, 0U);
	EXPECT_EQ(config.mappingIndexId, 0L);
	EXPECT_EQ(config.entryPointNodeId, 0L);
	EXPECT_TRUE(config.codebookKey.empty());
	EXPECT_FALSE(config.isTrained);
}

TEST_F(VectorIndexConfigTest, ToProperties) {
	graph::vector::VectorIndexConfig config;
	config.dimension = 128;
	config.metricType = 1;
	config.pqSubspaces = 8;
	config.mappingIndexId = 100;
	config.entryPointNodeId = 200;
	config.codebookKey = "test_codebook";
	config.isTrained = true;

	auto props = config.toProperties();

	EXPECT_EQ(props.size(), 7UL);
	EXPECT_TRUE(props.contains("dim"));
	EXPECT_TRUE(props.contains("metric"));
	EXPECT_TRUE(props.contains("pq_m"));
	EXPECT_TRUE(props.contains("map_root"));
	EXPECT_TRUE(props.contains("entry"));
	EXPECT_TRUE(props.contains("cb_key"));
	EXPECT_TRUE(props.contains("trained"));

	EXPECT_EQ(std::get<int64_t>(props.at("dim").getVariant()), 128);
	EXPECT_EQ(std::get<int64_t>(props.at("metric").getVariant()), 1);
	EXPECT_EQ(std::get<int64_t>(props.at("pq_m").getVariant()), 8);
	EXPECT_EQ(std::get<int64_t>(props.at("map_root").getVariant()), 100);
	EXPECT_EQ(std::get<int64_t>(props.at("entry").getVariant()), 200);
	EXPECT_EQ(std::get<std::string>(props.at("cb_key").getVariant()), "test_codebook");
	EXPECT_TRUE(std::get<bool>(props.at("trained").getVariant()));
}

TEST_F(VectorIndexConfigTest, FromProperties_Full) {
	std::unordered_map<std::string, graph::PropertyValue> props = {
			{"dim", graph::PropertyValue(128)},		{"metric", graph::PropertyValue(1)},
			{"pq_m", graph::PropertyValue(8)},		{"map_root", graph::PropertyValue(100)},
			{"entry", graph::PropertyValue(200)},	{"cb_key", graph::PropertyValue("test_codebook")},
			{"trained", graph::PropertyValue(true)}};

	auto config = graph::vector::VectorIndexConfig::fromProperties(props);

	EXPECT_EQ(config.dimension, 128U);
	EXPECT_EQ(config.metricType, 1U);
	EXPECT_EQ(config.pqSubspaces, 8U);
	EXPECT_EQ(config.mappingIndexId, 100L);
	EXPECT_EQ(config.entryPointNodeId, 200L);
	EXPECT_EQ(config.codebookKey, "test_codebook");
	EXPECT_TRUE(config.isTrained);
}

TEST_F(VectorIndexConfigTest, FromProperties_Partial) {
	std::unordered_map<std::string, graph::PropertyValue> props = {{"dim", graph::PropertyValue(64)},
																   {"metric", graph::PropertyValue(2)}};

	auto config = graph::vector::VectorIndexConfig::fromProperties(props);

	EXPECT_EQ(config.dimension, 64U);
	EXPECT_EQ(config.metricType, 2U);
	EXPECT_EQ(config.pqSubspaces, 0U); // Default value
	EXPECT_EQ(config.mappingIndexId, 0L); // Default value
	EXPECT_EQ(config.entryPointNodeId, 0L); // Default value
	EXPECT_TRUE(config.codebookKey.empty()); // Default value
	EXPECT_FALSE(config.isTrained); // Default value
}

TEST_F(VectorIndexConfigTest, FromProperties_Empty) {
	std::unordered_map<std::string, graph::PropertyValue> props;

	auto config = graph::vector::VectorIndexConfig::fromProperties(props);

	// All fields should have default values
	EXPECT_EQ(config.dimension, 0U);
	EXPECT_EQ(config.metricType, 0U);
	EXPECT_EQ(config.pqSubspaces, 0U);
	EXPECT_EQ(config.mappingIndexId, 0L);
	EXPECT_EQ(config.entryPointNodeId, 0L);
	EXPECT_TRUE(config.codebookKey.empty());
	EXPECT_FALSE(config.isTrained);
}

TEST_F(VectorIndexConfigTest, FromProperties_TrainedBool) {
	std::unordered_map<std::string, graph::PropertyValue> props = {{"trained", graph::PropertyValue(true)}};

	auto config = graph::vector::VectorIndexConfig::fromProperties(props);

	EXPECT_TRUE(config.isTrained);
}

TEST_F(VectorIndexConfigTest, FromProperties_TrainedIntNonZero) {
	std::unordered_map<std::string, graph::PropertyValue> props = {{"trained", graph::PropertyValue(1)}};

	auto config = graph::vector::VectorIndexConfig::fromProperties(props);

	EXPECT_TRUE(config.isTrained);
}

TEST_F(VectorIndexConfigTest, FromProperties_TrainedIntZero) {
	std::unordered_map<std::string, graph::PropertyValue> props = {{"trained", graph::PropertyValue(0)}};

	auto config = graph::vector::VectorIndexConfig::fromProperties(props);

	EXPECT_FALSE(config.isTrained);
}

TEST_F(VectorIndexConfigTest, FromProperties_TrainedIntNegative) {
	std::unordered_map<std::string, graph::PropertyValue> props = {{"trained", graph::PropertyValue(-1)}};

	auto config = graph::vector::VectorIndexConfig::fromProperties(props);

	EXPECT_TRUE(config.isTrained); // Non-zero is treated as trained
}

TEST_F(VectorIndexConfigTest, RoundTrip) {
	// Create a config
	graph::vector::VectorIndexConfig original;
	original.dimension = 256;
	original.metricType = 2;
	original.pqSubspaces = 16;
	original.mappingIndexId = 999;
	original.entryPointNodeId = 1234;
	original.codebookKey = "roundtrip_key";
	original.isTrained = true;

	// Convert to properties and back
	auto props = original.toProperties();
	auto restored = graph::vector::VectorIndexConfig::fromProperties(props);

	// Verify the round-trip
	EXPECT_EQ(restored.dimension, original.dimension);
	EXPECT_EQ(restored.metricType, original.metricType);
	EXPECT_EQ(restored.pqSubspaces, original.pqSubspaces);
	EXPECT_EQ(restored.mappingIndexId, original.mappingIndexId);
	EXPECT_EQ(restored.entryPointNodeId, original.entryPointNodeId);
	EXPECT_EQ(restored.codebookKey, original.codebookKey);
	EXPECT_EQ(restored.isTrained, original.isTrained);
}

TEST_F(VectorIndexConfigTest, FromProperties_LargeDimension) {
	std::unordered_map<std::string, graph::PropertyValue> props = {{"dim", graph::PropertyValue(4096)}};

	auto config = graph::vector::VectorIndexConfig::fromProperties(props);

	EXPECT_EQ(config.dimension, static_cast<decltype(config.dimension)>(4096));
}

TEST_F(VectorIndexConfigTest, FromProperties_MetricTypes) {
	// Test different metric type values
	for (int metric = 0; metric < 10; ++metric) {
		std::unordered_map<std::string, graph::PropertyValue> props = {{"metric", graph::PropertyValue(metric)}};

		auto config = graph::vector::VectorIndexConfig::fromProperties(props);

		EXPECT_EQ(config.metricType, static_cast<decltype(config.metricType)>(metric));
	}
}
