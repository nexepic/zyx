#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "graph/cli/repl/frontend/Editor.hpp"
#include "../io/MockTerminal.hpp"

using namespace zyx::repl;
using ::testing::Return;
using ::testing::Sequence;
using ::testing::_;
using ::testing::NiceMock;

class EditorTest : public ::testing::Test {
protected:
    void SetUp() override {
        terminal = std::make_shared<NiceMock<io::MockTerminal>>();
        history = std::make_shared<core::History>();
        editor = std::make_unique<frontend::Editor>(terminal, history);
        
        ON_CALL(*terminal, getColumns()).WillByDefault(Return(80));
        ON_CALL(*terminal, enableRawMode()).WillByDefault(Return(true));
    }

    std::shared_ptr<NiceMock<io::MockTerminal>> terminal;
    std::shared_ptr<core::History> history;
    std::unique_ptr<frontend::Editor> editor;
};

TEST_F(EditorTest, SimpleInputAndEnter) {
    Sequence seq;
    EXPECT_CALL(*terminal, enableRawMode()).InSequence(seq).WillOnce(Return(true));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'H'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'i'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));
    EXPECT_CALL(*terminal, disableRawMode()).InSequence(seq);
    
    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "Hi");
}

TEST_F(EditorTest, CtrlCCancelsInput) {
    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'H'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_C, 0}));
    
    auto result = editor->readLine("> ");
    EXPECT_FALSE(result.has_value());
}

TEST_F(EditorTest, CtrlDEmptyCancelsInput) {
    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_D, 0}));
    
    auto result = editor->readLine("> ");
    EXPECT_FALSE(result.has_value());
}

TEST_F(EditorTest, CtrlDNonEmptyDeletesChar) {
    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'A'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::LEFT, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_D, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));
    
    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "");
}

TEST_F(EditorTest, BackspaceAndDeleteDeletesChar) {
    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'a'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'b'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::BACKSPACE, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'c'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::LEFT, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::DELETE_KEY, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));
    
    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "a");
}

TEST_F(EditorTest, BasicAndCtrlNavigation) {
    Sequence seq;
    // Enter ABC
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'A'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'B'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'C'}));
    // Home
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::HOME_KEY, 0}));
    // Right
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::RIGHT, 0}));
    // Ctrl-B (Left)
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_B, 0}));
    // Ctrl-F (Right)
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_F, 0}));
    // End
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::END_KEY, 0}));
    // Ctrl-A (Home)
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_A, 0}));
    // Ctrl-E (End)
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_E, 0}));
    // Add D
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'D'}));
    // Enter
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));
    
    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "ABCD");
}

TEST_F(EditorTest, ControlDeletions) {
    Sequence seq;
    // Enter "Hello World"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'H'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'i'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, ' '}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'H'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'o'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'w'}));
    // Ctrl-W (Delete word)
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_W, 0}));
    // Ctrl-U (Delete to beginning)
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_U, 0}));
    
    // Type new string
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'N'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'e'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'w'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_A, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::RIGHT, 0}));
    // Ctrl-K (Delete to end)
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_K, 0}));
    // Ctrl-L (Clear screen)
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_L, 0}));
    
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));
    
    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "N");
}

TEST_F(EditorTest, HistoryNavigation) {
    history->add("First");
    history->add("Second");

    Sequence seq;
    // Enter "Temp"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'T'}));
    // UP twice -> "Second", "First"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::UP, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::UP, 0}));
    // UP again (bounded)
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::UP, 0}));
    // DOWN once -> "Second"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::DOWN, 0}));
    // DOWN again -> back to original buffer "T"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::DOWN, 0}));
    // DOWN again (bounded)
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::DOWN, 0}));
    
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));

    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "T");
}

TEST_F(EditorTest, CtrlPAndCtrlNHistory) {
    history->add("A");
    history->add("B");

    Sequence seq;
    // Ctrl-P (Up) -> "B"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_P, 0}));
    // Ctrl-P (Up) -> "A"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_P, 0}));
    // Ctrl-N (Down) -> "B"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_N, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));

    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "B");
}

TEST_F(EditorTest, UnknownKeyIgnored) {
    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::UNKNOWN, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));

    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "");
}

TEST_F(EditorTest, EnableRawModeFails) {
    EXPECT_CALL(*terminal, enableRawMode()).WillOnce(Return(false));
    auto result = editor->readLine("> ");
    EXPECT_FALSE(result.has_value());
}