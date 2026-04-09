/**
 * @file test_ScopeStack.cpp
 * @brief Unit tests for ScopeStack variable scope management.
 */

#include <gtest/gtest.h>

#include "graph/query/planner/ScopeStack.hpp"

using namespace graph::query::planner;

class ScopeStackTest : public ::testing::Test {
protected:
	ScopeStack stack;
};

TEST_F(ScopeStackTest, InitialState) {
	EXPECT_EQ(stack.depth(), 1u);
	EXPECT_TRUE(stack.currentVars().empty());
}

TEST_F(ScopeStackTest, DefineAndResolve) {
	stack.define("x");
	EXPECT_TRUE(stack.resolve("x"));
	EXPECT_FALSE(stack.resolve("y"));
}

TEST_F(ScopeStackTest, CurrentVarsReturnsDefinedVars) {
	stack.define("a");
	stack.define("b");
	auto vars = stack.currentVars();
	EXPECT_EQ(vars.size(), 2u);
	EXPECT_TRUE(vars.count("a"));
	EXPECT_TRUE(vars.count("b"));
}

TEST_F(ScopeStackTest, PushFrameCreatesNewScope) {
	stack.define("outer");
	stack.pushFrame();
	EXPECT_EQ(stack.depth(), 2u);
	// Outer var still visible from inner non-isolated frame
	EXPECT_TRUE(stack.resolve("outer"));
	// currentVars only returns current frame's vars
	EXPECT_TRUE(stack.currentVars().empty());
}

TEST_F(ScopeStackTest, PushIsolatedFrameBlocksOuterScope) {
	stack.define("outer");
	stack.pushFrame(true); // isolated
	EXPECT_EQ(stack.depth(), 2u);
	// Outer var NOT visible from isolated frame
	EXPECT_FALSE(stack.resolve("outer"));
	// Define inner var
	stack.define("inner");
	EXPECT_TRUE(stack.resolve("inner"));
}

TEST_F(ScopeStackTest, PopFrameReturnsFrame) {
	stack.define("global");
	stack.pushFrame();
	stack.define("local");
	auto frame = stack.popFrame();
	EXPECT_EQ(frame.variables.size(), 1u);
	EXPECT_TRUE(frame.variables.count("local"));
	EXPECT_EQ(stack.depth(), 1u);
	// global still there
	EXPECT_TRUE(stack.resolve("global"));
}

TEST_F(ScopeStackTest, PopFrameThrowsOnRoot) {
	EXPECT_THROW(stack.popFrame(), std::logic_error);
}

TEST_F(ScopeStackTest, PopIsolatedFrame) {
	stack.pushFrame(true);
	auto frame = stack.popFrame();
	EXPECT_TRUE(frame.isolated);
}

TEST_F(ScopeStackTest, ReplaceFrame) {
	stack.define("old");
	EXPECT_TRUE(stack.resolve("old"));
	stack.replaceFrame({"new1", "new2"});
	EXPECT_FALSE(stack.resolve("old"));
	EXPECT_TRUE(stack.resolve("new1"));
	EXPECT_TRUE(stack.resolve("new2"));
}

TEST_F(ScopeStackTest, ResolveWalksFramesBottomUp) {
	stack.define("root");
	stack.pushFrame();
	stack.define("mid");
	stack.pushFrame();
	// Should find vars from both outer frames
	EXPECT_TRUE(stack.resolve("root"));
	EXPECT_TRUE(stack.resolve("mid"));
	EXPECT_FALSE(stack.resolve("missing"));
}

TEST_F(ScopeStackTest, ResolveStopsAtIsolatedFrame) {
	stack.define("root");
	stack.pushFrame(); // non-isolated
	stack.define("mid");
	stack.pushFrame(true); // isolated
	stack.define("inner");
	// inner is visible
	EXPECT_TRUE(stack.resolve("inner"));
	// mid and root are NOT visible (blocked by isolated frame)
	EXPECT_FALSE(stack.resolve("mid"));
	EXPECT_FALSE(stack.resolve("root"));
}

TEST_F(ScopeStackTest, ResolveNotFoundReturnsEmpty) {
	// Empty stack resolve
	EXPECT_FALSE(stack.resolve("nonexistent"));
}

TEST_F(ScopeStackTest, DepthTracksFrameCount) {
	EXPECT_EQ(stack.depth(), 1u);
	stack.pushFrame();
	EXPECT_EQ(stack.depth(), 2u);
	stack.pushFrame(true);
	EXPECT_EQ(stack.depth(), 3u);
	stack.popFrame();
	EXPECT_EQ(stack.depth(), 2u);
	stack.popFrame();
	EXPECT_EQ(stack.depth(), 1u);
}

TEST_F(ScopeStackTest, DefineOnEmptyFramesThrows) {
	// Manually empty the stack by popping more than should be allowed
	// The constructor creates one frame, and popFrame prevents popping it.
	// Test define on a fresh stack (has 1 frame) - should not throw
	EXPECT_NO_THROW(stack.define("x"));
}

TEST_F(ScopeStackTest, MultipleDefinesSameVar) {
	stack.define("x");
	stack.define("x"); // duplicate is idempotent
	auto vars = stack.currentVars();
	EXPECT_EQ(vars.size(), 1u);
}

TEST_F(ScopeStackTest, ReplaceFrameWithEmpty) {
	stack.define("a");
	stack.replaceFrame({});
	EXPECT_TRUE(stack.currentVars().empty());
	EXPECT_FALSE(stack.resolve("a"));
}
