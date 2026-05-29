#include "graph/query/execution/Record.hpp"

#include <gtest/gtest.h>
#include <utility>

using graph::Edge;
using graph::Node;
using graph::PropertyValue;
using graph::query::execution::Record;

TEST(RecordTest, RefAccessorsReturnStoredNodeEdgeAndValue) {
	Record record;

	Node node(1, 10);
	node.addProperty("name", PropertyValue("alice"));
	Edge edge(2, 1, 3, 20);
	edge.addProperty("weight", PropertyValue(7));
	record.setNode("n", node);
	record.setEdge("e", edge);
	record.setValue("score", PropertyValue(42));

	auto nodeRef = record.getNodeRef("n");
	ASSERT_TRUE(nodeRef.has_value());
	EXPECT_EQ(nodeRef->get().getId(), 1);
	EXPECT_EQ(nodeRef->get().getProperty("name"), PropertyValue("alice"));

	auto edgeRef = record.getEdgeRef("e");
	ASSERT_TRUE(edgeRef.has_value());
	EXPECT_EQ(edgeRef->get().getId(), 2);
	EXPECT_EQ(edgeRef->get().getProperty("weight"), PropertyValue(7));

	auto valueRef = record.getValueRef("score");
	ASSERT_TRUE(valueRef.has_value());
	EXPECT_EQ(valueRef->get(), PropertyValue(42));
}

TEST(RecordTest, RvalueSettersPreserveStoredValues) {
	Record record;

	Node node(11, 12);
	node.addProperty("country", PropertyValue("CN"));
	Edge edge(13, 11, 12, 14);
	edge.addProperty("rank", PropertyValue(3));
	PropertyValue value("moved");

	record.setNode("n", std::move(node));
	record.setEdge("e", std::move(edge));
	record.setValue("v", std::move(value));

	auto storedNode = record.getNode("n");
	ASSERT_TRUE(storedNode.has_value());
	EXPECT_EQ(storedNode->getId(), 11);
	EXPECT_EQ(storedNode->getProperty("country"), PropertyValue("CN"));

	auto storedEdge = record.getEdge("e");
	ASSERT_TRUE(storedEdge.has_value());
	EXPECT_EQ(storedEdge->getId(), 13);
	EXPECT_EQ(storedEdge->getProperty("rank"), PropertyValue(3));

	auto storedValue = record.getValue("v");
	ASSERT_TRUE(storedValue.has_value());
	EXPECT_EQ(*storedValue, PropertyValue("moved"));
}
