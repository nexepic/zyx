#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <vector>
#include <cstdio>
#include <unistd.h>
#include "inputxx/Session.hpp"
#include "io/MockTerminal.hpp"

using namespace inputxx;
using ::testing::Return;
using ::testing::Sequence;
using ::testing::NiceMock;

TEST(SessionTest, InitializeAndBasicUsage) {
    Session session(50);
    session.addHistory("First command");
    session.addHistory("Second command");
    session.clearHistory();
}

TEST(SessionTest, MockedReadLine) {
    auto terminal = std::make_shared<NiceMock<io::MockTerminal>>();

    // Simulate typing "A" and hitting ENTER
    Sequence seq;
    EXPECT_CALL(*terminal, enableRawMode()).InSequence(seq).WillOnce(Return(true));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'A'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));
    EXPECT_CALL(*terminal, disableRawMode()).InSequence(seq);

    Session session(terminal, 50);
    auto result = session.readLine("prompt> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "A");
}

TEST(SessionTest, CompletionCallback) {
    auto terminal = std::make_shared<NiceMock<io::MockTerminal>>();
    ON_CALL(*terminal, enableRawMode()).WillByDefault(Return(true));
    ON_CALL(*terminal, getColumns()).WillByDefault(Return(80));

    Session session(terminal, 50);
    session.setCompletionCallback([](const std::string& line, std::vector<std::string>& completions) {
        if (line == "MA") {
            completions.push_back("MATCH");
        }
    });

    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'M'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'A'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::TAB, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));

    auto result = session.readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "MATCH");
}

TEST(SessionTest, SaveAndLoadHistory) {
    std::string tmpfile = "/tmp/inputxx_session_test_" + std::to_string(::getpid()) + ".txt";

    {
        auto terminal = std::make_shared<NiceMock<io::MockTerminal>>();
        Session session(terminal, 50);
        session.addHistory("cmd1");
        session.addHistory("cmd2");
        EXPECT_TRUE(session.saveHistory(tmpfile));
    }

    {
        auto terminal = std::make_shared<NiceMock<io::MockTerminal>>();
        Session session(terminal, 50);
        EXPECT_TRUE(session.loadHistory(tmpfile));
    }

    std::remove(tmpfile.c_str());
}

TEST(SessionTest, SaveHistoryToInvalidPath) {
    auto terminal = std::make_shared<NiceMock<io::MockTerminal>>();
    Session session(terminal, 50);
    session.addHistory("test");
    EXPECT_FALSE(session.saveHistory("/nonexistent/dir/history.txt"));
}

TEST(SessionTest, LoadHistoryFromMissingFile) {
    auto terminal = std::make_shared<NiceMock<io::MockTerminal>>();
    Session session(terminal, 50);
    EXPECT_FALSE(session.loadHistory("/nonexistent/file.txt"));
}
