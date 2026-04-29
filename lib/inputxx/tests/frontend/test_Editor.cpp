#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "inputxx/frontend/Editor.hpp"
#include "../io/MockTerminal.hpp"

using namespace inputxx;
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
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'A'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'B'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'C'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::HOME_KEY, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::RIGHT, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_B, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_F, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::END_KEY, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_A, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_E, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'D'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));

    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "ABCD");
}

TEST_F(EditorTest, ControlDeletions) {
    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'H'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'i'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, ' '}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'H'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'o'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'w'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_W, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_U, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'N'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'e'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'w'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_A, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::RIGHT, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_K, 0}));
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
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'T'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::UP, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::UP, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::UP, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::DOWN, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::DOWN, 0}));
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
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_P, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_P, 0}));
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
    // When raw mode fails, Editor falls back to std::getline from cin.
    // In test environment cin is not interactive, so it returns nullopt.
    auto result = editor->readLine("> ");
    EXPECT_FALSE(result.has_value());
}

TEST_F(EditorTest, WordMovement) {
    Sequence seq;
    // Type "Hello World Test"
    for (char c : std::string("Hello World Test")) {
        EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, c}));
    }
    // Ctrl+Left: move word left (to "Test")
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_LEFT, 0}));
    // Ctrl+Left: move word left (to "World")
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_LEFT, 0}));
    // Insert 'X'
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'X'}));
    // Enter
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));

    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "Hello XWorld Test");
}

TEST_F(EditorTest, Transpose) {
    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'a'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'b'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CTRL_T, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));

    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "ba");
}

TEST_F(EditorTest, TabCompletionSingle) {
    editor->setCompletionCallback([](const std::string& line, std::vector<std::string>& completions) {
        if (line == "hel") {
            completions.push_back("hello");
        }
    });

    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'h'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'e'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'l'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::TAB, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));

    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "hello");
}

TEST_F(EditorTest, TabCompletionCycle) {
    editor->setCompletionCallback([](const std::string& line, std::vector<std::string>& completions) {
        completions.push_back("match_a");
        completions.push_back("match_b");
        completions.push_back("match_c");
    });

    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'm'}));
    // TAB 1: shows "match_a"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::TAB, 0}));
    // TAB 2: shows "match_b"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::TAB, 0}));
    // TAB 3: shows "match_c"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::TAB, 0}));
    // TAB 4: wraps back to original "m"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::TAB, 0}));
    // TAB 5: back to "match_a"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::TAB, 0}));
    // Accept "match_a" with Enter
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));

    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "match_a");
}

TEST_F(EditorTest, TabCompletionEscReverts) {
    editor->setCompletionCallback([](const std::string& line, std::vector<std::string>& completions) {
        completions.push_back("opt_a");
        completions.push_back("opt_b");
    });

    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'o'}));
    // TAB: shows "opt_a"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::TAB, 0}));
    // ESC: revert to original "o"
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ESC, 0}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));

    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "o");
}

TEST_F(EditorTest, TabNoCallbackBeeps) {
    // No completion callback set
    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::TAB, 0}));
    EXPECT_CALL(*terminal, beep()).Times(1);
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));

    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "");
}

TEST_F(EditorTest, TabNoMatchesBeeps) {
    editor->setCompletionCallback([](const std::string&, std::vector<std::string>&) {
        // Return no completions
    });

    Sequence seq;
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::CHARACTER, 'x'}));
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::TAB, 0}));
    EXPECT_CALL(*terminal, beep()).Times(1);
    EXPECT_CALL(*terminal, readKey()).InSequence(seq).WillOnce(Return(io::KeyEvent{io::KeyCode::ENTER, '\r'}));

    auto result = editor->readLine("> ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "x");
}
