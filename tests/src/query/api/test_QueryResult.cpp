/**
 * @file test_QueryResult.cpp
 * @brief Focused unit tests for query::QueryResult data container behavior.
 */

#include <gtest/gtest.h>

#include "graph/query/api/QueryResult.hpp"

using namespace graph::query;

TEST(QueryResultTest, NotificationsAreStoredAndReturned) {
	QueryResult result;

	result.addNotification("slow query");
	result.addNotification("used fallback plan");

	const auto &notifications = result.getNotifications();
	ASSERT_EQ(notifications.size(), 2U);
	EXPECT_EQ(notifications[0], "slow query");
	EXPECT_EQ(notifications[1], "used fallback plan");
}

TEST(QueryResultTest, DefaultStateAndBasicAccessors) {
	QueryResult result;

	EXPECT_TRUE(result.isEmpty());
	EXPECT_EQ(result.rowCount(), 0U);
	EXPECT_EQ(result.getDuration(), 0.0);
	EXPECT_TRUE(result.getNotifications().empty());

	QueryResult::Row row;
	row.emplace("x", ResultValue(int64_t(42)));
	result.addRow(std::move(row));
	result.setColumns({"x"});
	result.setDuration(12.5);

	ASSERT_FALSE(result.isEmpty());
	EXPECT_EQ(result.rowCount(), 1U);
	ASSERT_EQ(result.getColumns().size(), 1U);
	EXPECT_EQ(result.getColumns()[0], "x");
	EXPECT_EQ(result.getDuration(), 12.5);
}

TEST(QueryResultTest, LogicalEntityResultsRetainNamesEndpointsAndPropertiesWithoutTokens) {
	LogicalNodeResult node{42U, {"Person", "VIP"}, {{"name", graph::PropertyValue("Ada")}}};
	LogicalEdgeResult edge{7U, 42U, 99U, "KNOWS", {{"weight", graph::PropertyValue(3)}}};
	ResultValue nodeValue(node);
	ResultValue edgeValue(edge);

	EXPECT_TRUE(nodeValue.isLogicalNode());
	EXPECT_TRUE(nodeValue.isGraphNode());
	EXPECT_TRUE(nodeValue.isNode());
	EXPECT_EQ(nodeValue.asNode().getId(), 42U);
	EXPECT_EQ(nodeValue.asNode().getLabelCount(), 2U);
	EXPECT_EQ(nodeValue.asNode().getLabels(), (std::vector<std::string>{"Person", "VIP"}));
	EXPECT_EQ(nodeValue.asNode().getProperties().at("name"), graph::PropertyValue("Ada"));
	EXPECT_EQ(nodeValue.asLogicalNode(), node);
	EXPECT_EQ(nodeValue.toString(), "(:Person:VIP {name: Ada})");
	EXPECT_TRUE(edgeValue.isLogicalEdge());
	EXPECT_TRUE(edgeValue.isGraphEdge());
	EXPECT_TRUE(edgeValue.isEdge());
	EXPECT_EQ(edgeValue.asEdge().getType(), "KNOWS");
	EXPECT_EQ(edgeValue.asEdge().getSourceNodeId(), 42U);
	EXPECT_EQ(edgeValue.asEdge().getTargetNodeId(), 99U);
	EXPECT_EQ(edgeValue.asEdge().getProperties().at("weight"), graph::PropertyValue(3));
	EXPECT_EQ(edgeValue.asLogicalEdge(), edge);
	EXPECT_EQ(edgeValue.toString(), "[:KNOWS {weight: 3}]");
}

TEST(ResultValueTest, FormatsPhysicalAndLogicalEntitiesAndRejectsInvalidViews) {
	graph::Node node(12, 9);
	node.addLabelId(5);
	node.addProperty("age", graph::PropertyValue(int64_t{37}));
	node.addProperty("name", graph::PropertyValue("Ada"));
	ResultValue physicalNode(node);
	EXPECT_TRUE(physicalNode.isNode());
	EXPECT_TRUE(physicalNode.isGraphNode());
	EXPECT_FALSE(physicalNode.isLogicalNode());
	EXPECT_FALSE(physicalNode.isEdge());
	EXPECT_FALSE(physicalNode.isPrimitive());
	EXPECT_EQ(physicalNode.asNode().getId(), 12U);
	EXPECT_EQ(physicalNode.asNode().getLabelCount(), 2U);
	EXPECT_EQ(physicalNode.asNode().getProperties().at("name"), graph::PropertyValue("Ada"));
	EXPECT_FALSE(physicalNode.asNode().hasResolvedLabels());
	EXPECT_THROW(static_cast<void>(physicalNode.asNode().getLabels()), std::logic_error);
	EXPECT_EQ(physicalNode.toString([](const int64_t token) { return token == 9 ? "Person" : "VIP"; }),
			  "(:Person:VIP {age: 37, name: Ada})");

	graph::Edge edge(13, 12, 14, 16);
	edge.addProperty("weight", graph::PropertyValue(int64_t{3}));
	ResultValue physicalEdge(edge);
	EXPECT_TRUE(physicalEdge.isEdge());
	EXPECT_TRUE(physicalEdge.isGraphEdge());
	EXPECT_FALSE(physicalEdge.isLogicalEdge());
	EXPECT_EQ(physicalEdge.asEdge().getId(), 13U);
	EXPECT_EQ(physicalEdge.asEdge().getSourceNodeId(), 12U);
	EXPECT_EQ(physicalEdge.asEdge().getTargetNodeId(), 14U);
	EXPECT_EQ(physicalEdge.asEdge().getProperties().at("weight"), graph::PropertyValue(int64_t{3}));
	EXPECT_FALSE(physicalEdge.asEdge().hasResolvedType());
	EXPECT_THROW(static_cast<void>(physicalEdge.asEdge().getType()), std::logic_error);
	EXPECT_EQ(physicalEdge.toString(), "[:?16 {weight: 3}]");

	ResultValue primitive(graph::PropertyValue(int64_t{7}));
	EXPECT_TRUE(primitive.isPrimitive());
	EXPECT_FALSE(primitive.isNode());
	EXPECT_EQ(primitive.asPrimitive(), graph::PropertyValue(int64_t{7}));
	EXPECT_EQ(primitive.toString(), "7");
	EXPECT_THROW(static_cast<void>(primitive.asNode()), std::bad_variant_access);
	EXPECT_THROW(static_cast<void>(primitive.asEdge()), std::bad_variant_access);

	ResultValue emptyLogicalNode(LogicalNodeResult{21U, {}, {}});
	ResultValue emptyLogicalEdge(LogicalEdgeResult{22U, 21U, 23U, "", {}});
	EXPECT_TRUE(emptyLogicalNode.asNode().hasResolvedLabels());
	EXPECT_TRUE(emptyLogicalNode.asNode().getLabels().empty());
	EXPECT_EQ(emptyLogicalNode.toString(), "()");
	EXPECT_TRUE(emptyLogicalNode.asNode().getProperties().empty());
	EXPECT_TRUE(emptyLogicalEdge.asEdge().hasResolvedType());
	EXPECT_EQ(emptyLogicalEdge.asEdge().getId(), 22U);
	EXPECT_EQ(emptyLogicalEdge.asEdge().getSourceNodeId(), 21U);
	EXPECT_EQ(emptyLogicalEdge.asEdge().getTargetNodeId(), 23U);
	EXPECT_TRUE(emptyLogicalEdge.asEdge().getProperties().empty());
	EXPECT_EQ(emptyLogicalEdge.toString(), "[]");

	ResultValue defaultValue;
	EXPECT_TRUE(defaultValue.isPrimitive());
	EXPECT_TRUE(std::holds_alternative<graph::PropertyValue>(defaultValue.getVariant()));

	graph::Node zeroLabelNode(24, 0);
	zeroLabelNode.getMutableMetadata().labelIds[0] = 0;
	zeroLabelNode.getMutableMetadata().labelCount = 1;
	EXPECT_EQ(ResultValue(zeroLabelNode).toString(), "()");
	EXPECT_EQ(ResultValue(graph::Edge(25, 24, 26, 0)).toString(), "[]");
}

TEST(ResultValueTest, RejectsNegativePhysicalEntityIdentifiers) {
	ResultValue negativeNode(graph::Node(-1, 0));
	EXPECT_THROW(static_cast<void>(negativeNode.asNode().getId()), std::domain_error);

	ResultValue negativeEdgeId(graph::Edge(-1, 1, 2, 0));
	EXPECT_THROW(static_cast<void>(negativeEdgeId.asEdge().getId()), std::domain_error);
	ResultValue negativeSource(graph::Edge(1, -2, 2, 0));
	EXPECT_THROW(static_cast<void>(negativeSource.asEdge().getSourceNodeId()), std::domain_error);
	ResultValue negativeTarget(graph::Edge(1, 2, -3, 0));
	EXPECT_THROW(static_cast<void>(negativeTarget.asEdge().getTargetNodeId()), std::domain_error);
}
