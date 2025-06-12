/**
 * @file test_Repl.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include "graph/cli/Repl.hpp"
#include "graph/core/Database.hpp"

class REPLTest : public ::testing::Test {
protected:
    graph::Database db{"test.db"};
    graph::REPLTest repl{db};

    void SetUp() override {
        // Setup code if needed
    }

    void TearDown() override {
        // Cleanup code if needed
    }
};

TEST_F(REPLTest, ShouldExit) {
    ASSERT_TRUE(repl.shouldExit("exit"));
    ASSERT_FALSE(repl.shouldExit("addNode"));
    ASSERT_FALSE(repl.shouldExit("addEdge"));
}
