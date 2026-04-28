#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "graph/cli/repl/Session.hpp"
#include "io/MockTerminal.hpp"

using namespace zyx::repl;
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
