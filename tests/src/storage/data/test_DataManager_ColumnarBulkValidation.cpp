/**
 * @file test_DataManager_ColumnarBulkValidation.cpp
 * @brief Covers columnar bulk input validation and validator callbacks.
 */

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "DataManagerTestFixture.hpp"
#include "graph/storage/constraints/IEntityValidator.hpp"

namespace {

class ColumnarInsertValidator final : public graph::storage::constraints::IEntityValidator {
public:
	void validateNodeInsert(
			const graph::Node &node,
			const std::unordered_map<std::string, graph::PropertyValue> &props) override {
		++nodeInsertCount;
		observedNodeLabels.push_back(node.getLabelId());
		observedNodeProps.push_back(props);
	}

	void validateEdgeInsert(
			const graph::Edge &edge,
			const std::unordered_map<std::string, graph::PropertyValue> &props) override {
		++edgeInsertCount;
		observedEdgeSources.push_back(edge.getSourceNodeId());
		observedEdgeTargets.push_back(edge.getTargetNodeId());
		observedEdgeTypes.push_back(edge.getTypeId());
		observedEdgeProps.push_back(props);
	}

	size_t nodeInsertCount = 0;
	size_t edgeInsertCount = 0;
	std::vector<int64_t> observedNodeLabels;
	std::vector<int64_t> observedEdgeSources;
	std::vector<int64_t> observedEdgeTargets;
	std::vector<int64_t> observedEdgeTypes;
	std::vector<std::unordered_map<std::string, graph::PropertyValue>> observedNodeProps;
	std::vector<std::unordered_map<std::string, graph::PropertyValue>> observedEdgeProps;
};

} // namespace

TEST_F(DataManagerTest, ColumnarNodeInputRejectsInvalidColumns) {
	const int64_t labelId = dataManager->getOrCreateTokenId("ColumnarUser");

	EXPECT_THROW(
			(void) dataManager->addNodesColumnar(
					labelId,
					1,
					std::vector<BulkPropertyColumn>{{"", {graph::PropertyValue("u1")}}}),
			std::invalid_argument);
	EXPECT_THROW(
			(void) dataManager->addNodesColumnar(
					labelId,
					2,
					std::vector<BulkPropertyColumn>{{"id", {graph::PropertyValue("u1")}}}),
			std::invalid_argument);
	EXPECT_THROW(
			(void) dataManager->addNodesColumnar(
					labelId,
					1,
					std::vector<BulkPropertyColumn>{
							{"id", {graph::PropertyValue("u1")}},
							{"id", {graph::PropertyValue("u2")}}}),
			std::invalid_argument);
}

TEST_F(DataManagerTest, ColumnarEdgeInputRejectsMismatchedRowsAndInvalidColumns) {
	const int64_t typeId = dataManager->getOrCreateTokenId("COLUMNAR_REL");

	EXPECT_THROW(
			(void) dataManager->addEdgesColumnar(typeId, {1, 2}, {3}, {}),
			std::invalid_argument);
	EXPECT_THROW(
			(void) dataManager->addEdgesColumnar(
					typeId,
					{1},
					{2},
					std::vector<BulkPropertyColumn>{{"", {graph::PropertyValue(1)}}}),
			std::invalid_argument);
	EXPECT_THROW(
			(void) dataManager->addEdgesColumnar(
					typeId,
					{1, 2},
					{3, 4},
					std::vector<BulkPropertyColumn>{{"weight", {graph::PropertyValue(1)}}}),
			std::invalid_argument);
	EXPECT_THROW(
			(void) dataManager->addEdgesColumnar(
					typeId,
					{1},
					{2},
					std::vector<BulkPropertyColumn>{
							{"weight", {graph::PropertyValue(1)}},
							{"weight", {graph::PropertyValue(2)}}}),
			std::invalid_argument);
}

TEST_F(DataManagerTest, ColumnarZeroRowsReturnEmptyResults) {
	const int64_t labelId = dataManager->getOrCreateTokenId("ColumnarEmpty");
	const int64_t typeId = dataManager->getOrCreateTokenId("COLUMNAR_EMPTY");

	EXPECT_TRUE(dataManager->addNodesColumnar(labelId, 0, {}).empty());
	EXPECT_TRUE(dataManager->addEdgesColumnar(typeId, {}, {}, {}).empty());
}

TEST_F(DataManagerTest, ColumnarBulkInvokesNodeAndEdgeValidatorsWithMaterializedRows) {
	const auto validator = std::make_shared<ColumnarInsertValidator>();
	dataManager->registerValidator(validator);

	const int64_t labelId = dataManager->getOrCreateTokenId("ColumnarValidatedUser");
	const std::vector<BulkPropertyColumn> nodeColumns{
			{"id", {graph::PropertyValue("u1"), graph::PropertyValue("u2")}},
			{"score", {graph::PropertyValue(int64_t{10}), graph::PropertyValue(int64_t{20})}}};
	const auto nodeIds = dataManager->addNodesColumnar(labelId, 2, nodeColumns);
	ASSERT_EQ(nodeIds.size(), 2U);
	EXPECT_EQ(validator->nodeInsertCount, 2U);
	EXPECT_EQ(validator->observedNodeLabels, std::vector<int64_t>({labelId, labelId}));
	ASSERT_EQ(validator->observedNodeProps.size(), 2U);
	EXPECT_EQ(validator->observedNodeProps[0].at("id"), graph::PropertyValue("u1"));
	EXPECT_EQ(validator->observedNodeProps[1].at("score"), graph::PropertyValue(int64_t{20}));

	const int64_t typeId = dataManager->getOrCreateTokenId("COLUMNAR_VALIDATED_REL");
	const std::vector<BulkPropertyColumn> edgeColumns{
			{"weight", {graph::PropertyValue(int64_t{7}), graph::PropertyValue(int64_t{11})}},
			{"kind", {graph::PropertyValue("strong"), graph::PropertyValue("weak")}}};
	const auto edgeIds = dataManager->addEdgesColumnar(
			typeId,
			{nodeIds[0], nodeIds[1]},
			{nodeIds[1], nodeIds[0]},
			edgeColumns);

	ASSERT_EQ(edgeIds.size(), 2U);
	EXPECT_EQ(validator->edgeInsertCount, 2U);
	EXPECT_EQ(validator->observedEdgeSources, std::vector<int64_t>({nodeIds[0], nodeIds[1]}));
	EXPECT_EQ(validator->observedEdgeTargets, std::vector<int64_t>({nodeIds[1], nodeIds[0]}));
	EXPECT_EQ(validator->observedEdgeTypes, std::vector<int64_t>({typeId, typeId}));
	ASSERT_EQ(validator->observedEdgeProps.size(), 2U);
	EXPECT_EQ(validator->observedEdgeProps[0].at("weight"), graph::PropertyValue(int64_t{7}));
	EXPECT_EQ(validator->observedEdgeProps[1].at("kind"), graph::PropertyValue("weak"));
}
