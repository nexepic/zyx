/**
 * @file test_Log.cpp
 * @author Nexepic
 * @date 2025/12/4
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
		graph::log::Log::setDebug(false);
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
	ASSERT_FALSE(graph::log::Log::isDebugEnabled());

	graph::log::Log::setDebug(true);
	ASSERT_TRUE(graph::log::Log::isDebugEnabled());

	graph::log::Log::setDebug(false);
	ASSERT_FALSE(graph::log::Log::isDebugEnabled());
}

// Test info logs with a single argument
TEST_F(LogTest, PrintsInfoLogs) {
	graph::log::Log::info("This is an info message with number {}", 123);
	std::string expected = "[INFO] This is an info message with number 123\n";
	ASSERT_EQ(cout_buffer.str(), expected);
	// Ensure cerr has no output
	ASSERT_TRUE(cerr_buffer.str().empty());
}

// Test warning logs with a single argument
TEST_F(LogTest, PrintsWarningLogs) {
	graph::log::Log::warn("This is a warning message with number {}", 789);
	std::string expected = "\033[1;35m[WARN] This is a warning message with number 789\033[0m\n";
	ASSERT_EQ(cout_buffer.str(), expected);
	// Ensure cerr has no output
	ASSERT_TRUE(cerr_buffer.str().empty());
}

// Test error logs with a single argument
TEST_F(LogTest, PrintsErrorLogs) {
	graph::log::Log::error("This is an error message with number {}", 456);
	std::string expected = "\033[1;31m[ERROR] This is an error message with number 456\033[0m\n";
	ASSERT_EQ(cerr_buffer.str(), expected);
	// Ensure cout has no output
	ASSERT_TRUE(cout_buffer.str().empty());
}

// Test that debug logs are not printed when debug mode is disabled
TEST_F(LogTest, DoesNotPrintDebugLogsWhenDisabled) {
	// Ensure debug mode is disabled
	ASSERT_FALSE(graph::log::Log::isDebugEnabled());
	graph::log::Log::debug("This should not be printed");
	// Ensure both cout and cerr have no output
	ASSERT_TRUE(cout_buffer.str().empty());
	ASSERT_TRUE(cerr_buffer.str().empty());
}

// Test that debug logs are printed when debug mode is enabled
TEST_F(LogTest, PrintsDebugLogsWhenEnabled) {
	graph::log::Log::setDebug(true);
	ASSERT_TRUE(graph::log::Log::isDebugEnabled());

	graph::log::Log::debug("This is a debug message with number {}", 789);
	std::string expected = "\033[1;33m[DEBUG] This is a debug message with number 789\033[0m\n";
	ASSERT_EQ(cout_buffer.str(), expected);
	// Ensure cerr has no output
	ASSERT_TRUE(cerr_buffer.str().empty());
}

// Test logging with multiple arguments of different types
TEST_F(LogTest, PrintsLogsWithMultipleArguments) {
	std::string str_arg = "world";
	double float_arg = 3.14;
	graph::log::Log::info("Hello {}, the number is {} and the float is {}", str_arg, 42, float_arg);
	std::string expected = "[INFO] Hello world, the number is 42 and the float is 3.14\n";
	ASSERT_EQ(cout_buffer.str(), expected);
}
