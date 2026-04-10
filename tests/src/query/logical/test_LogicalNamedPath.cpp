/**
 * @file test_LogicalNamedPath.cpp
 * @brief Unit tests for LogicalNamedPath logical operator.
 */

#include <gtest/gtest.h>
#include "graph/query/logical/operators/LogicalNamedPath.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"

using namespace graph::query::logical;

TEST(LogicalNamedPathTest, GetType) {
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalNamedPath op(std::move(child), "p", {"a", "b"}, {"r"});
	EXPECT_EQ(op.getType(), LogicalOpType::LOP_NAMED_PATH);
}

TEST(LogicalNamedPathTest, GetPathVariable) {
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalNamedPath op(std::move(child), "myPath", {"x"}, {"e"});
	EXPECT_EQ(op.getPathVariable(), "myPath");
}

TEST(LogicalNamedPathTest, GetNodeVariables) {
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalNamedPath op(std::move(child), "p", {"a", "b", "c"}, {"r1", "r2"});
	auto nodeVars = op.getNodeVariables();
	ASSERT_EQ(nodeVars.size(), 3u);
	EXPECT_EQ(nodeVars[0], "a");
	EXPECT_EQ(nodeVars[1], "b");
	EXPECT_EQ(nodeVars[2], "c");
}

TEST(LogicalNamedPathTest, GetEdgeVariables) {
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalNamedPath op(std::move(child), "p", {"a", "b"}, {"r1", "r2"});
	auto edgeVars = op.getEdgeVariables();
	ASSERT_EQ(edgeVars.size(), 2u);
	EXPECT_EQ(edgeVars[0], "r1");
	EXPECT_EQ(edgeVars[1], "r2");
}

TEST(LogicalNamedPathTest, GetChildren) {
	auto child = std::make_unique<LogicalSingleRow>();
	auto* childPtr = child.get();
	LogicalNamedPath op(std::move(child), "p", {"a"}, {"r"});
	auto children = op.getChildren();
	ASSERT_EQ(children.size(), 1u);
	EXPECT_EQ(children[0], childPtr);
}

TEST(LogicalNamedPathTest, GetOutputVariables_IncludesChildVarsAndPathVar) {
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalNamedPath op(std::move(child), "p", {"a", "b"}, {"r"});
	auto vars = op.getOutputVariables();
	// SingleRow has no output vars, so just the path variable
	EXPECT_FALSE(vars.empty());
	EXPECT_EQ(vars.back(), "p");
}

TEST(LogicalNamedPathTest, GetOutputVariables_NullChild) {
	LogicalNamedPath op(nullptr, "p", {"a"}, {"r"});
	auto vars = op.getOutputVariables();
	ASSERT_EQ(vars.size(), 1u);
	EXPECT_EQ(vars[0], "p");
}

TEST(LogicalNamedPathTest, Clone) {
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalNamedPath op(std::move(child), "p", {"a", "b"}, {"r"});
	auto cloned = op.clone();
	ASSERT_NE(cloned, nullptr);
	EXPECT_EQ(cloned->getType(), LogicalOpType::LOP_NAMED_PATH);

	auto* clonedPath = dynamic_cast<LogicalNamedPath*>(cloned.get());
	ASSERT_NE(clonedPath, nullptr);
	EXPECT_EQ(clonedPath->getPathVariable(), "p");
	EXPECT_EQ(clonedPath->getNodeVariables().size(), 2u);
	EXPECT_EQ(clonedPath->getEdgeVariables().size(), 1u);
}

TEST(LogicalNamedPathTest, CloneWithNullChild) {
	LogicalNamedPath op(nullptr, "p", {"a"}, {"r"});
	auto cloned = op.clone();
	ASSERT_NE(cloned, nullptr);
	auto children = cloned->getChildren();
	// Child should be null
	EXPECT_EQ(children.size(), 1u);
	EXPECT_EQ(children[0], nullptr);
}

TEST(LogicalNamedPathTest, ToString) {
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalNamedPath op(std::move(child), "myPath", {"a"}, {"r"});
	EXPECT_EQ(op.toString(), "NamedPath(myPath)");
}

TEST(LogicalNamedPathTest, SetChild) {
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalNamedPath op(std::move(child), "p", {"a"}, {"r"});

	auto newChild = std::make_unique<LogicalSingleRow>();
	auto* newChildPtr = newChild.get();
	op.setChild(0, std::move(newChild));

	auto children = op.getChildren();
	EXPECT_EQ(children[0], newChildPtr);
}

TEST(LogicalNamedPathTest, SetChildNonZeroIndex) {
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalNamedPath op(std::move(child), "p", {"a"}, {"r"});

	auto newChild = std::make_unique<LogicalSingleRow>();
	op.setChild(1, std::move(newChild));
	// Index 1 should be ignored, original child unchanged
	auto children = op.getChildren();
	EXPECT_NE(children[0], nullptr);
}

TEST(LogicalNamedPathTest, DetachChild) {
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalNamedPath op(std::move(child), "p", {"a"}, {"r"});

	auto detached = op.detachChild(0);
	ASSERT_NE(detached, nullptr);
	EXPECT_EQ(detached->getType(), LogicalOpType::LOP_SINGLE_ROW);

	// After detach, child should be null
	auto children = op.getChildren();
	EXPECT_EQ(children[0], nullptr);
}

TEST(LogicalNamedPathTest, DetachChildNonZeroIndex) {
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalNamedPath op(std::move(child), "p", {"a"}, {"r"});

	auto detached = op.detachChild(1);
	EXPECT_EQ(detached, nullptr);

	// Original child should still be intact
	auto children = op.getChildren();
	EXPECT_NE(children[0], nullptr);
}

TEST(LogicalNamedPathTest, EmptyVariableLists) {
	auto child = std::make_unique<LogicalSingleRow>();
	LogicalNamedPath op(std::move(child), "p", {}, {});
	EXPECT_TRUE(op.getNodeVariables().empty());
	EXPECT_TRUE(op.getEdgeVariables().empty());
}
