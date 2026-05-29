#include <gtest/gtest.h>

#include "graph/query/execution/NodeColumnBatch.hpp"
#include "graph/query/execution/NodeScanRequirementUtils.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"

namespace {

TEST(NodeScanRequirementsTest, DefaultsPreserveFullNodeCompatibility) {
	graph::query::execution::NodeScanRequirements requirements;

	EXPECT_EQ(requirements.materialization,
	          graph::query::execution::NodeMaterializationMode::NSM_FULL_NODE);
	EXPECT_TRUE(requirements.needsLabels);
	EXPECT_TRUE(requirements.needsActiveCheck);
	EXPECT_FALSE(requirements.countOnly);
	EXPECT_TRUE(requirements.needsFullNode());
	EXPECT_TRUE(requirements.needsProperties());
}

TEST(NodeScanRequirementsTest, SelectedPropertiesRequirePropertyLoading) {
	graph::query::execution::NodeScanRequirements requirements;
	requirements.materialization = graph::query::execution::NodeMaterializationMode::NSM_SELECTED_PROPERTIES;
	requirements.requiredProperties = {"age", "score"};

	EXPECT_FALSE(requirements.needsFullNode());
	EXPECT_TRUE(requirements.needsProperties());
}

TEST(NodeScanRequirementsTest, IdOnlyWithoutRequiredPropertiesDoesNotNeedProperties) {
	graph::query::execution::NodeScanRequirements requirements;
	requirements.materialization = graph::query::execution::NodeMaterializationMode::NSM_ID_ONLY;

	EXPECT_TRUE(requirements.requiredProperties.empty());
	EXPECT_FALSE(requirements.needsFullNode());
	EXPECT_FALSE(requirements.needsProperties());
}

TEST(NodeScanRequirementsTest, RelaxSatisfiedCandidateChecksClearsOnlyGuaranteedChecks) {
	graph::query::execution::NodeScanRequirements requirements;
	graph::query::execution::NodeCandidateSet candidates;
	candidates.activeOnly = true;
	candidates.labelsSatisfied = false;

	auto relaxed = graph::query::execution::relaxSatisfiedCandidateChecks(requirements, candidates);
	EXPECT_FALSE(relaxed.needsActiveCheck);
	EXPECT_TRUE(relaxed.needsLabels);

	candidates.labelsSatisfied = true;
	relaxed = graph::query::execution::relaxSatisfiedCandidateChecks(requirements, candidates);
	EXPECT_FALSE(relaxed.needsActiveCheck);
	EXPECT_FALSE(relaxed.needsLabels);
}

TEST(NodeScanRequirementsTest, ColumnarBatchSizeUsesWideBatchesAboveThreshold) {
	EXPECT_EQ(graph::query::execution::chooseColumnarNodeBatchSize(100, nullptr, 1000), 100U);
	EXPECT_EQ(graph::query::execution::chooseColumnarNodeBatchSize(4095, nullptr, 1000), 1000U);
	EXPECT_EQ(graph::query::execution::chooseColumnarNodeBatchSize(4096, nullptr, 1000), 4096U);
	EXPECT_EQ(graph::query::execution::chooseColumnarNodeBatchSize(70000, nullptr, 1000), 65536U);
}

TEST(NodeColumnBatchTest, SizeAndSelectedCountUseSelectionVector) {
	graph::query::execution::NodeColumnBatch batch;
	batch.nodeIds = {10, 20, 30, 40};
	batch.selected = {1, 0, 1, 0};

	EXPECT_EQ(batch.size(), 4U);
	EXPECT_TRUE(batch.isSelected(0));
	EXPECT_FALSE(batch.isSelected(1));
	EXPECT_TRUE(batch.isSelected(2));
	EXPECT_FALSE(batch.isSelected(3));
	EXPECT_EQ(batch.selectedCount(), 2U);
}

TEST(NodeColumnBatchTest, EmptySelectionMeansAllRowsSelected) {
	graph::query::execution::NodeColumnBatch batch;
	batch.nodeIds = {10, 20, 30};

	EXPECT_TRUE(batch.selected.empty());
	EXPECT_TRUE(batch.isSelected(0));
	EXPECT_TRUE(batch.isSelected(1));
	EXPECT_TRUE(batch.isSelected(2));
	EXPECT_EQ(batch.selectedCount(), 3U);
}

TEST(NodeColumnBatchTest, EnsureSelectionVectorPopulatesSelectedWithOnes) {
	graph::query::execution::NodeColumnBatch batch;
	batch.nodeIds = {10, 20, 30};

	batch.ensureSelectionVector();

	ASSERT_EQ(batch.selected.size(), 3U);
	EXPECT_EQ(batch.selected[0], 1U);
	EXPECT_EQ(batch.selected[1], 1U);
	EXPECT_EQ(batch.selected[2], 1U);
}

TEST(NodeColumnBatchTest, HasPropertyColumnReportsPresence) {
	graph::query::execution::NodeColumnBatch batch;
	batch.propertyColumns["age"] = {graph::PropertyValue(42), std::nullopt};

	EXPECT_TRUE(batch.hasPropertyColumn("age"));
	EXPECT_FALSE(batch.hasPropertyColumn("score"));
}

} // namespace
