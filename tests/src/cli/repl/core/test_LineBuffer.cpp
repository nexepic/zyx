#include <gtest/gtest.h>
#include "graph/cli/repl/core/LineBuffer.hpp"

using namespace zyx::repl::core;

TEST(LineBufferTest, DefaultConstructor) {
    LineBuffer b;
    EXPECT_TRUE(b.getBuffer().empty());
    EXPECT_EQ(b.getCursorPosition(), 0u);
}

TEST(LineBufferTest, InsertChar) {
    LineBuffer b;
    b.insert('H');
    b.insert('e');
    b.insert('l');
    b.insert('l');
    b.insert('o');
    
    EXPECT_EQ(b.getBuffer(), "Hello");
    EXPECT_EQ(b.getCursorPosition(), 5);
}

TEST(LineBufferTest, InsertString) {
    LineBuffer b;
    b.insert("Hello");
    EXPECT_EQ(b.getBuffer(), "Hello");
    EXPECT_EQ(b.getCursorPosition(), 5);
}

TEST(LineBufferTest, MoveCursor) {
    LineBuffer b;
    b.insert("World");
    b.moveHome();
    EXPECT_EQ(b.getCursorPosition(), 0u);
    
    b.moveRight();
    b.moveRight();
    EXPECT_EQ(b.getCursorPosition(), 2);
    
    b.insert("o");
    EXPECT_EQ(b.getBuffer(), "Woorld");
    EXPECT_EQ(b.getCursorPosition(), 3);
    
    b.moveLeft();
    EXPECT_EQ(b.getCursorPosition(), 2);
    
    b.moveEnd();
    EXPECT_EQ(b.getCursorPosition(), 6);
}

TEST(LineBufferTest, Backspace) {
    LineBuffer b;
    b.insert("Test");
    b.backspace();
    EXPECT_EQ(b.getBuffer(), "Tes");
    EXPECT_EQ(b.getCursorPosition(), 3);
    
    b.moveHome();
    b.backspace(); // Should do nothing
    EXPECT_EQ(b.getBuffer(), "Tes");
    EXPECT_EQ(b.getCursorPosition(), 0u);
    
    b.moveRight(); // cursor at 1
    b.backspace(); // removes 'T'
    EXPECT_EQ(b.getBuffer(), "es");
    EXPECT_EQ(b.getCursorPosition(), 0u);
}

TEST(LineBufferTest, DeleteChar) {
    LineBuffer b;
    b.insert("Test");
    b.deleteChar(); // at end, does nothing
    EXPECT_EQ(b.getBuffer(), "Test");
    
    b.moveHome();
    b.deleteChar(); // removes 'T'
    EXPECT_EQ(b.getBuffer(), "est");
    EXPECT_EQ(b.getCursorPosition(), 0u);
}

TEST(LineBufferTest, Clear) {
    LineBuffer b;
    b.insert("Hello");
    b.clear();
    EXPECT_TRUE(b.getBuffer().empty());
    EXPECT_EQ(b.getCursorPosition(), 0u);
}

TEST(LineBufferTest, DeleteToBeginning) {
    LineBuffer b;
    b.insert("Hello World");
    b.moveLeft();
    b.moveLeft(); // cursor at 'l' (index 9)
    b.deleteToBeginning();
    EXPECT_EQ(b.getBuffer(), "ld");
    EXPECT_EQ(b.getCursorPosition(), 0u);
}

TEST(LineBufferTest, DeleteToEnd) {
    LineBuffer b;
    b.insert("Hello World");
    b.moveLeft();
    b.moveLeft(); // cursor at 'l' (index 9)
    b.deleteToEnd();
    EXPECT_EQ(b.getBuffer(), "Hello Wor");
    EXPECT_EQ(b.getCursorPosition(), 9u);
}

TEST(LineBufferTest, DeletePreviousWord) {
    LineBuffer b;
    b.insert("Hello World Test");
    b.deletePreviousWord();
    EXPECT_EQ(b.getBuffer(), "Hello World ");
    EXPECT_EQ(b.getCursorPosition(), 12u);
    
    b.deletePreviousWord();
    EXPECT_EQ(b.getBuffer(), "Hello ");
    EXPECT_EQ(b.getCursorPosition(), 6u);
}

TEST(LineBufferTest, UTF8WidthSupport) {
    LineBuffer b;
    b.insert("好"); // 3 bytes, 2 display width
    EXPECT_EQ(b.getCursorPosition(), 3u);
    EXPECT_EQ(b.getCursorColumn(), 2u);
    
    b.moveLeft(); // Should jump back 3 bytes
    EXPECT_EQ(b.getCursorPosition(), 0u);
    EXPECT_EQ(b.getCursorColumn(), 0u);
    
    b.moveRight();
    EXPECT_EQ(b.getCursorPosition(), 3u);
    EXPECT_EQ(b.getCursorColumn(), 2u);

    b.clear();
    b.insert("\xE2\x9D\xAF"); // ❯
    EXPECT_EQ(b.getCursorPosition(), 3u);
    EXPECT_EQ(b.getCursorColumn(), 1u);
    
    b.clear();
    b.insert("\xF0\x9F\x9A\x80"); // 🚀 (4 bytes, usually 2 columns)
    EXPECT_EQ(b.getCursorPosition(), 4u);
    EXPECT_EQ(b.getCursorColumn(), 2u);
}

TEST(LineBufferTest, AnsiEscapeSequenceWidth) {
    LineBuffer b;
    b.insert("\033[1;32mTest\033[0m");
    // "Test" is 4 columns, ANSI escapes should be 0 columns
    EXPECT_EQ(b.getCursorColumn(), 4u);
}

TEST(LineBufferTest, DeletePreviousWordEdgeCases) {
    LineBuffer b;
    b.deletePreviousWord(); // cursor at 0
    EXPECT_EQ(b.getBuffer(), "");
    EXPECT_EQ(b.getCursorPosition(), 0u);

    b.insert("   ");
    b.deletePreviousWord(); // only spaces
    EXPECT_EQ(b.getBuffer(), "");
}

TEST(LineBufferTest, ReplaceBuffer) {
    LineBuffer b;
    b.insert("Old");
    b.setBuffer("NewBuffer");
    EXPECT_EQ(b.getBuffer(), "NewBuffer");
    EXPECT_EQ(b.getCursorPosition(), 9);
}
