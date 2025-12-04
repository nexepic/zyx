/**
 * @file test_Log.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/4
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <sstream>
#include <streambuf>
#include "graph/log/Log.hpp"

// Use a test fixture to handle stream redirection and state reset
class LogTest : public ::testing::Test {
protected:
	std::stringstream cout_buffer;
	std::stringstream cerr_buffer;
	std::streambuf *original_cout_buf = nullptr;
	std::streambuf *original_cerr_buf = nullptr;

	// Before each test, redirect std::cout and std::cerr
	void SetUp() override {
		original_cout_buf = std::cout.rdbuf(cout_buffer.rdbuf());
		original_cerr_buf = std::cerr.rdbuf(cerr_buffer.rdbuf());
		// Ensure debug mode is off at the start of each test
		graph::utils::Log::setDebug(false);
	}

	// After each test, restore std::cout and std::cerr
	void TearDown() override {
		std::cout.rdbuf(original_cout_buf);
		std::cerr.rdbuf(original_cerr_buf);
	}
};

// Test setting and querying the debug flag
TEST_F(LogTest, HandlesDebugFlag) {
	// Default should be false
	ASSERT_FALSE(graph::utils::Log::isDebugEnabled());

	graph::utils::Log::setDebug(true);
	ASSERT_TRUE(graph::utils::Log::isDebugEnabled());

	graph::utils::Log::setDebug(false);
	ASSERT_FALSE(graph::utils::Log::isDebugEnabled());
}

// Test info logs
TEST_F(LogTest, PrintsInfoLogs) {
	graph::utils::Log::info("This is an info message with number ", 123);
	std::string expected = "[INFO] This is an info message with number 123\n";
	ASSERT_EQ(cout_buffer.str(), expected);
	// Ensure cerr has no output
	ASSERT_TRUE(cerr_buffer.str().empty());
}

// Test error logs
TEST_F(LogTest, PrintsErrorLogs) {
	graph::utils::Log::error("This is an error message with number ", 456);
	std::string expected = "\033[1;31m[ERROR] This is an error message with number 456\033[0m\n";
	ASSERT_EQ(cerr_buffer.str(), expected);
	// Ensure cout has no output
	ASSERT_TRUE(cout_buffer.str().empty());
}

// Test that debug logs are not printed when debug mode is disabled
TEST_F(LogTest, DoesNotPrintDebugLogsWhenDisabled) {
	// Ensure debug mode is disabled
	ASSERT_FALSE(graph::utils::Log::isDebugEnabled());
	graph::utils::Log::debug("This should not be printed");
	// Ensure both cout and cerr have no output
	ASSERT_TRUE(cout_buffer.str().empty());
	ASSERT_TRUE(cerr_buffer.str().empty());
}

// Test that debug logs are printed when debug mode is enabled
TEST_F(LogTest, PrintsDebugLogsWhenEnabled) {
	graph::utils::Log::setDebug(true);
	ASSERT_TRUE(graph::utils::Log::isDebugEnabled());

	graph::utils::Log::debug("This is a debug message with number ", 789);
	std::string expected = "\033[1;33m[DEBUG] This is a debug message with number 789\033[0m\n";
	ASSERT_EQ(cout_buffer.str(), expected);
	// Ensure cerr has no output
	ASSERT_TRUE(cerr_buffer.str().empty());
}
