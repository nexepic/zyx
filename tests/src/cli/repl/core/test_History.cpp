#include <gtest/gtest.h>
#include "graph/cli/repl/core/History.hpp"

using namespace zyx::repl::core;

TEST(HistoryTest, DefaultConstructor) {
    History h;
    EXPECT_EQ(h.getMaxSize(), 100u);
    EXPECT_TRUE(h.getEntries().empty());
}

TEST(HistoryTest, AddEntry) {
    History h;
    h.add("CREATE (n)");
    EXPECT_EQ(h.getEntries().size(), 1);
    EXPECT_EQ(h.getEntries().front(), "CREATE (n)");
}

TEST(HistoryTest, AddEmptyEntryIsIgnored) {
    History h;
    h.add("");
    EXPECT_TRUE(h.getEntries().empty());
}

TEST(HistoryTest, AddDuplicateEntry) {
    History h;
    h.add("MATCH (n) RETURN n");
    h.add("MATCH (n) RETURN n");
    EXPECT_EQ(h.getEntries().size(), 1);
}

TEST(HistoryTest, MaxSizeIsEnforced) {
    History h;
    h.setMaxSize(3);
    h.add("cmd 1");
    h.add("cmd 2");
    h.add("cmd 3");
    h.add("cmd 4");
    
    EXPECT_EQ(h.getEntries().size(), 3);
    EXPECT_EQ(h.getEntries()[0], "cmd 2");
    EXPECT_EQ(h.getEntries()[2], "cmd 4");
}

TEST(HistoryTest, ShrinkMaxSize) {
    History h;
    h.setMaxSize(5);
    h.add("cmd 1");
    h.add("cmd 2");
    h.add("cmd 3");
    h.add("cmd 4");
    
    h.setMaxSize(2);
    EXPECT_EQ(h.getEntries().size(), 2);
    EXPECT_EQ(h.getEntries()[0], "cmd 3");
    EXPECT_EQ(h.getEntries()[1], "cmd 4");
}

TEST(HistoryTest, Clear) {
    History h;
    h.add("cmd");
    h.clear();
    EXPECT_TRUE(h.getEntries().empty());
}
