#include <gtest/gtest.h>
#include "inputxx/core/History.hpp"
#include <cstdio>
#include <unistd.h>

using namespace inputxx::core;

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

TEST(HistoryTest, SaveAndLoad) {
    std::string tmpfile = "/tmp/zyx_test_history_" + std::to_string(::getpid()) + ".txt";

    History h1(50);
    h1.add("MATCH (n) RETURN n");
    h1.add("CREATE (n:Person {name: 'Alice'})");
    h1.add("MATCH (n) DELETE n");
    EXPECT_TRUE(h1.save(tmpfile));

    History h2(50);
    EXPECT_TRUE(h2.load(tmpfile));
    EXPECT_EQ(h2.getEntries().size(), 3u);
    EXPECT_EQ(h2.getEntries()[0], "MATCH (n) RETURN n");
    EXPECT_EQ(h2.getEntries()[1], "CREATE (n:Person {name: 'Alice'})");
    EXPECT_EQ(h2.getEntries()[2], "MATCH (n) DELETE n");

    std::remove(tmpfile.c_str());
}

TEST(HistoryTest, LoadRespectMaxSize) {
    std::string tmpfile = "/tmp/zyx_test_history_max_" + std::to_string(::getpid()) + ".txt";

    History h1(100);
    h1.add("cmd1");
    h1.add("cmd2");
    h1.add("cmd3");
    h1.add("cmd4");
    h1.add("cmd5");
    h1.save(tmpfile);

    History h2(3); // Only keep last 3
    h2.load(tmpfile);
    EXPECT_EQ(h2.getEntries().size(), 3u);
    EXPECT_EQ(h2.getEntries()[0], "cmd3");
    EXPECT_EQ(h2.getEntries()[2], "cmd5");

    std::remove(tmpfile.c_str());
}

TEST(HistoryTest, SaveToInvalidPathFails) {
    History h;
    h.add("test");
    EXPECT_FALSE(h.save("/nonexistent/dir/history.txt"));
}

TEST(HistoryTest, LoadFromMissingFileFails) {
    History h;
    EXPECT_FALSE(h.load("/nonexistent/file.txt"));
}
