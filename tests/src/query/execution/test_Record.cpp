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

TEST(RecordTest, LvalueSettersAndMissingLookupsPreserveValueSemantics) {
	Record record;
	Node node(21, 22);
	Edge edge(23, 21, 22, 24);
	PropertyValue value("copied");

	record.setNode("n", node);
	record.setEdge("e", edge);
	record.setValue("value", value);

	EXPECT_EQ(record.getNode("n")->getId(), 21U);
	EXPECT_EQ(record.getEdge("e")->getId(), 23U);
	EXPECT_EQ(record.getValue("value"), value);
	EXPECT_FALSE(record.getNode("missing").has_value());
	EXPECT_FALSE(record.getNodeRef("missing").has_value());
	EXPECT_FALSE(record.getEdge("missing").has_value());
	EXPECT_FALSE(record.getEdgeRef("missing").has_value());
	EXPECT_FALSE(record.getValue("missing").has_value());
	EXPECT_FALSE(record.getValueRef("missing").has_value());
}

TEST(RecordTest, MergeCopiesEveryDomainAndOverwritesConflictingBindings) {
	Record destination;
	destination.setNode("shared", Node(1, 10));
	destination.setEdge("shared", Edge(2, 1, 3, 20));
	destination.setValue("shared", PropertyValue(int64_t{1}));

	Record source;
	source.setNode("shared", Node(11, 12));
	source.setNode("sourceNode", Node(13, 14));
	source.setEdge("shared", Edge(15, 13, 14, 16));
	source.setEdge("sourceEdge", Edge(17, 13, 14, 18));
	source.setValue("shared", PropertyValue("replacement"));
	source.setValue("sourceValue", PropertyValue(int64_t{99}));

	destination.merge(source);

	EXPECT_EQ(destination.getNode("shared")->getId(), 11U);
	EXPECT_EQ(destination.getNode("sourceNode")->getId(), 13U);
	EXPECT_EQ(destination.getEdge("shared")->getId(), 15U);
	EXPECT_EQ(destination.getEdge("sourceEdge")->getId(), 17U);
	EXPECT_EQ(destination.getValue("shared"), PropertyValue("replacement"));
	EXPECT_EQ(destination.getValue("sourceValue"), PropertyValue(int64_t{99}));
}
