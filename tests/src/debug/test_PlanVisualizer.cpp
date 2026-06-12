/**
 * @file test_PlanVisualizer.cpp
 * @date 2026/04/24
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

#include <optional>
#include <string>
#include <vector>

#include "debug/PlanVisualizer.hpp"

using namespace graph::debug;
using graph::query::execution::PhysicalOperator;
using graph::query::execution::RecordBatch;

// ============================================================================
// Test visualize(nullptr) branch
// Covers: Branch (28:7) True — returns "Null Plan" for null root
// ============================================================================

TEST(PlanVisualizerTest, VisualizeNullRootReturnsNullPlan) {
	std::string result = PlanVisualizer::visualize(nullptr);
	EXPECT_EQ(result, "Null Plan");
}

namespace {

class StubPhysicalOperator : public PhysicalOperator {
public:
	void open() override {}
	std::optional<RecordBatch> next() override { return std::nullopt; }
	void close() override {}
	[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }
	[[nodiscard]] std::string toString() const override { return "Stub"; }
	[[nodiscard]] std::vector<ExplainAttribute> explainAttributes() const override {
		return {{"access_path.kind", "property_index"}, {"access_path.estimated_cardinality", "1"}};
	}
};

} // namespace

TEST(PlanVisualizerTest, VisualizeIncludesExplainAttributes) {
	StubPhysicalOperator root;

	const std::string result = PlanVisualizer::visualize(&root);

	EXPECT_NE(result.find("Stub"), std::string::npos);
	EXPECT_NE(result.find("@ access_path.kind=property_index"), std::string::npos);
	EXPECT_NE(result.find("@ access_path.estimated_cardinality=1"), std::string::npos);
}
