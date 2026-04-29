#include <gtest/gtest.h>
#include "inputxx/core/LineBuffer.hpp"

using namespace inputxx::core;

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

TEST(LineBufferTest, MoveWordLeft) {
    LineBuffer b;
    b.insert("Hello World Test");
    b.moveWordLeft();
    EXPECT_EQ(b.getCursorPosition(), 12u); // Before "Test"
    b.moveWordLeft();
    EXPECT_EQ(b.getCursorPosition(), 6u); // Before "World"
    b.moveWordLeft();
    EXPECT_EQ(b.getCursorPosition(), 0u); // Before "Hello"
    b.moveWordLeft(); // At start, no-op
    EXPECT_EQ(b.getCursorPosition(), 0u);
}

TEST(LineBufferTest, MoveWordRight) {
    LineBuffer b;
    b.insert("Hello World Test");
    b.moveHome();
    b.moveWordRight();
    EXPECT_EQ(b.getCursorPosition(), 6u); // After "Hello "
    b.moveWordRight();
    EXPECT_EQ(b.getCursorPosition(), 12u); // After "World "
    b.moveWordRight();
    EXPECT_EQ(b.getCursorPosition(), 16u); // End
    b.moveWordRight(); // At end, no-op
    EXPECT_EQ(b.getCursorPosition(), 16u);
}

TEST(LineBufferTest, TransposeASCII) {
    LineBuffer b;
    b.insert("ab");
    b.transpose(); // At end: swap 'a' and 'b' -> "ba", cursor at end
    EXPECT_EQ(b.getBuffer(), "ba");
    EXPECT_EQ(b.getCursorPosition(), 2u);
}

TEST(LineBufferTest, TransposeMiddle) {
    LineBuffer b;
    b.insert("abc");
    b.moveLeft(); // cursor at 'c' (position 2)
    b.transpose(); // Swap 'b' and 'c' -> "acb", cursor after 'cb' at 3
    EXPECT_EQ(b.getBuffer(), "acb");
    EXPECT_EQ(b.getCursorPosition(), 3u);
}

TEST(LineBufferTest, TransposeEdgeCases) {
    LineBuffer b;
    b.transpose(); // Empty buffer, no-op
    EXPECT_EQ(b.getBuffer(), "");

    b.insert("a");
    b.transpose(); // Only 1 char, no-op
    EXPECT_EQ(b.getBuffer(), "a");

    b.clear();
    b.insert("ab");
    b.moveHome();
    b.transpose(); // Cursor at 0, no-op
    EXPECT_EQ(b.getBuffer(), "ab");
}

TEST(LineBufferTest, TransposeUTF8) {
    LineBuffer b;
    // Two CJK characters: 你好
    b.insert("\xe4\xbd\xa0\xe5\xa5\xbd"); // "你好"
    b.transpose(); // At end: swap "你" and "好" -> "好你"
    EXPECT_EQ(b.getBuffer(), "\xe5\xa5\xbd\xe4\xbd\xa0"); // "好你"
}

TEST(LineBufferTest, CodepointWidthBasic) {
    EXPECT_EQ(codepointWidth('A'), 1);
    EXPECT_EQ(codepointWidth(' '), 1);
    EXPECT_EQ(codepointWidth(0x4E00), 2);  // CJK '一'
    EXPECT_EQ(codepointWidth(0x0300), 0);  // Combining grave accent
    EXPECT_EQ(codepointWidth(0x200B), 0);  // Zero-width space
    EXPECT_EQ(codepointWidth(0x1F600), 2); // Grinning face emoji
    EXPECT_EQ(codepointWidth(0xFEFF), 0);  // BOM
    EXPECT_EQ(codepointWidth(0xFF01), 2);  // Fullwidth exclamation mark
    EXPECT_EQ(codepointWidth(0xAC00), 2);  // Hangul syllable '가'
}

TEST(LineBufferTest, DisplayColToByteOffset) {
    // ASCII
    EXPECT_EQ(displayColToByteOffset("Hello", 0), 0u);
    EXPECT_EQ(displayColToByteOffset("Hello", 3), 3u);
    EXPECT_EQ(displayColToByteOffset("Hello", 5), 5u);
    EXPECT_EQ(displayColToByteOffset("Hello", 10), 5u); // past end

    // CJK: "你好" = 6 bytes, 4 display cols
    std::string cjk = "\xe4\xbd\xa0\xe5\xa5\xbd";
    EXPECT_EQ(displayColToByteOffset(cjk, 0), 0u);
    EXPECT_EQ(displayColToByteOffset(cjk, 2), 3u); // after "你"
    EXPECT_EQ(displayColToByteOffset(cjk, 4), 6u); // after "好"
    // Column 1 is middle of "你" (width 2), should not split
    EXPECT_EQ(displayColToByteOffset(cjk, 1), 0u);
}

TEST(LineBufferTest, GetDisplayWidthWithCombining) {
    // 'e' + combining acute accent = 1 column
    std::string combined = "e\xcc\x81"; // e + U+0301
    EXPECT_EQ(getDisplayWidth(combined), 1u);
}

// --- decodeUtf8 error paths ---

TEST(LineBufferTest, DecodeUtf8PastEnd) {
    std::string s = "A";
    size_t i = 5; // past end
    EXPECT_EQ(decodeUtf8(s, i), 0xFFFDu);
}

TEST(LineBufferTest, DecodeUtf8InvalidLeadingByte) {
    // 0xFF is not a valid UTF-8 leading byte
    std::string s("\xFF", 1);
    size_t i = 0;
    EXPECT_EQ(decodeUtf8(s, i), 0xFFFDu);
    EXPECT_EQ(i, 1u);
}

TEST(LineBufferTest, DecodeUtf8TruncatedSequence) {
    // 2-byte leader (0xC0-0xDF) but string is only 1 byte long
    std::string s("\xC3", 1);
    size_t i = 0;
    EXPECT_EQ(decodeUtf8(s, i), 0xFFFDu);
    EXPECT_EQ(i, 1u);
}

TEST(LineBufferTest, DecodeUtf8InvalidContinuation) {
    // 2-byte sequence: 0xC3 expects a continuation byte (0x80-0xBF),
    // but second byte is 0x00 (not a continuation)
    std::string s("\xC3\x00", 2);
    size_t i = 0;
    EXPECT_EQ(decodeUtf8(s, i), 0xFFFDu);
    EXPECT_EQ(i, 1u);
}

// --- codepointWidth: cover remaining Unicode range categories ---

TEST(LineBufferTest, CodepointWidthControlChars) {
    // C0 control characters
    EXPECT_EQ(codepointWidth(0x00), 0);  // NUL
    EXPECT_EQ(codepointWidth(0x01), 0);  // SOH
    EXPECT_EQ(codepointWidth(0x1F), 0);  // US

    // C1 control characters
    EXPECT_EQ(codepointWidth(0x7F), 0);  // DEL
    EXPECT_EQ(codepointWidth(0x80), 0);  // padding character
    EXPECT_EQ(codepointWidth(0x9F), 0);  // APC
}

TEST(LineBufferTest, CodepointWidthCombiningMarks) {
    // Cyrillic combining marks
    EXPECT_EQ(codepointWidth(0x0483), 0);
    // Hebrew combining marks
    EXPECT_EQ(codepointWidth(0x0591), 0);
    EXPECT_EQ(codepointWidth(0x05BF), 0);
    EXPECT_EQ(codepointWidth(0x05C1), 0);
    EXPECT_EQ(codepointWidth(0x05C4), 0);
    EXPECT_EQ(codepointWidth(0x05C7), 0);
    // Arabic combining marks
    EXPECT_EQ(codepointWidth(0x0610), 0);
    EXPECT_EQ(codepointWidth(0x064B), 0);
    EXPECT_EQ(codepointWidth(0x0670), 0);
    EXPECT_EQ(codepointWidth(0x06D6), 0);
    EXPECT_EQ(codepointWidth(0x06DF), 0);
    EXPECT_EQ(codepointWidth(0x06E7), 0);
    EXPECT_EQ(codepointWidth(0x06EA), 0);
    EXPECT_EQ(codepointWidth(0x0711), 0);
    // Syriac
    EXPECT_EQ(codepointWidth(0x0730), 0);
    // Thaana
    EXPECT_EQ(codepointWidth(0x07A6), 0);
    // NKo
    EXPECT_EQ(codepointWidth(0x07EB), 0);
    // Various Indic/Southeast Asian
    EXPECT_EQ(codepointWidth(0x0816), 0);
    EXPECT_EQ(codepointWidth(0x081B), 0);
    EXPECT_EQ(codepointWidth(0x0825), 0);
    EXPECT_EQ(codepointWidth(0x0829), 0);
    EXPECT_EQ(codepointWidth(0x0859), 0);
    EXPECT_EQ(codepointWidth(0x08D4), 0);
    EXPECT_EQ(codepointWidth(0x08E3), 0);
    // Devanagari
    EXPECT_EQ(codepointWidth(0x093A), 0);
    EXPECT_EQ(codepointWidth(0x0941), 0);
    EXPECT_EQ(codepointWidth(0x094D), 0);
    EXPECT_EQ(codepointWidth(0x0951), 0);
    EXPECT_EQ(codepointWidth(0x0962), 0);
    // Bengali
    EXPECT_EQ(codepointWidth(0x0981), 0);
    EXPECT_EQ(codepointWidth(0x09BC), 0);
    EXPECT_EQ(codepointWidth(0x09C1), 0);
    EXPECT_EQ(codepointWidth(0x09CD), 0);
    EXPECT_EQ(codepointWidth(0x09E2), 0);
    // Gurmukhi
    EXPECT_EQ(codepointWidth(0x0A01), 0);
    EXPECT_EQ(codepointWidth(0x0A3C), 0);
    EXPECT_EQ(codepointWidth(0x0A41), 0);
    EXPECT_EQ(codepointWidth(0x0A47), 0);
    EXPECT_EQ(codepointWidth(0x0A4B), 0);
    EXPECT_EQ(codepointWidth(0x0A51), 0);
    EXPECT_EQ(codepointWidth(0x0A70), 0);
    EXPECT_EQ(codepointWidth(0x0A75), 0);
    // Gujarati
    EXPECT_EQ(codepointWidth(0x0A81), 0);
    EXPECT_EQ(codepointWidth(0x0ABC), 0);
    EXPECT_EQ(codepointWidth(0x0AC1), 0);
    EXPECT_EQ(codepointWidth(0x0AC7), 0);
    EXPECT_EQ(codepointWidth(0x0ACD), 0);
    EXPECT_EQ(codepointWidth(0x0AE2), 0);
    // Thai
    EXPECT_EQ(codepointWidth(0x0E31), 0);
    EXPECT_EQ(codepointWidth(0x0E34), 0);
    EXPECT_EQ(codepointWidth(0x0E47), 0);
    // Lao
    EXPECT_EQ(codepointWidth(0x0EB1), 0);
    EXPECT_EQ(codepointWidth(0x0EB4), 0);
    EXPECT_EQ(codepointWidth(0x0EBB), 0);
    EXPECT_EQ(codepointWidth(0x0EC8), 0);
    // Extended combining marks
    EXPECT_EQ(codepointWidth(0x1AB0), 0);
    EXPECT_EQ(codepointWidth(0x1DC0), 0);
    EXPECT_EQ(codepointWidth(0x20D0), 0);
    // Variation selectors
    EXPECT_EQ(codepointWidth(0xFE00), 0);
    EXPECT_EQ(codepointWidth(0xFE20), 0);
    // Variation Selectors Supplement
    EXPECT_EQ(codepointWidth(0xE0100), 0);
}

TEST(LineBufferTest, CodepointWidthZeroWidthChars) {
    EXPECT_EQ(codepointWidth(0x00AD), 0);  // Soft hyphen
    EXPECT_EQ(codepointWidth(0x200C), 0);  // Zero-width non-joiner
    EXPECT_EQ(codepointWidth(0x200D), 0);  // Zero-width joiner
    EXPECT_EQ(codepointWidth(0x200E), 0);  // LRM
    EXPECT_EQ(codepointWidth(0x200F), 0);  // RLM
    EXPECT_EQ(codepointWidth(0x2028), 0);  // Line separator
    EXPECT_EQ(codepointWidth(0x202E), 0);  // Bidi override
    EXPECT_EQ(codepointWidth(0x2060), 0);  // Word joiner
    EXPECT_EQ(codepointWidth(0x2069), 0);  // Pop directional isolate
}

TEST(LineBufferTest, CodepointWidthEastAsianWide) {
    // Hangul Jamo
    EXPECT_EQ(codepointWidth(0x1100), 2);
    // Angle brackets
    EXPECT_EQ(codepointWidth(0x2329), 2);
    EXPECT_EQ(codepointWidth(0x232A), 2);
    // CJK radicals / Kangxi
    EXPECT_EQ(codepointWidth(0x2E80), 2);
    // Hiragana
    EXPECT_EQ(codepointWidth(0x3040), 2);
    // CJK Extension A
    EXPECT_EQ(codepointWidth(0x3400), 2);
    // Yi Syllables
    EXPECT_EQ(codepointWidth(0xA000), 2);
    // Hangul Jamo Extended-A
    EXPECT_EQ(codepointWidth(0xA960), 2);
    // CJK Compatibility Ideographs
    EXPECT_EQ(codepointWidth(0xF900), 2);
    // Vertical Forms
    EXPECT_EQ(codepointWidth(0xFE10), 2);
    // CJK Compatibility Forms
    EXPECT_EQ(codepointWidth(0xFE30), 2);
    // Fullwidth Signs
    EXPECT_EQ(codepointWidth(0xFFE0), 2);
    // Mahjong Tiles
    EXPECT_EQ(codepointWidth(0x1F000), 2);
    // Domino Tiles
    EXPECT_EQ(codepointWidth(0x1F030), 2);
    // Playing Cards
    EXPECT_EQ(codepointWidth(0x1F0A0), 2);
    // Enclosed Ideographic Supplement
    EXPECT_EQ(codepointWidth(0x1F200), 2);
    // Misc Symbols/Pictographs
    EXPECT_EQ(codepointWidth(0x1F300), 2);
    // Supplemental Symbols and Pictographs
    EXPECT_EQ(codepointWidth(0x1F900), 2);
    // Chess Symbols
    EXPECT_EQ(codepointWidth(0x1FA00), 2);
    // Symbols and Pictographs Extended-A
    EXPECT_EQ(codepointWidth(0x1FA70), 2);
    // CJK Extensions B-F
    EXPECT_EQ(codepointWidth(0x20000), 2);
    // CJK Extension G+
    EXPECT_EQ(codepointWidth(0x30000), 2);
}

// --- displayColToByteOffset with ANSI escapes ---

TEST(LineBufferTest, DisplayColToByteOffsetWithAnsi) {
    // "\033[31mAB\033[0m" — 'A' at col 0, 'B' at col 1
    std::string s = "\033[31mAB\033[0m";
    EXPECT_EQ(displayColToByteOffset(s, 0), 0u);
    EXPECT_EQ(displayColToByteOffset(s, 1), 6u);  // after ESC[31m + 'A'
    EXPECT_EQ(displayColToByteOffset(s, 2), 7u);  // after 'B'
    EXPECT_EQ(displayColToByteOffset(s, 10), s.length()); // past end
}

// --- getDisplayWidth edge cases ---

TEST(LineBufferTest, GetDisplayWidthEmpty) {
    EXPECT_EQ(getDisplayWidth(""), 0u);
}

TEST(LineBufferTest, GetDisplayWidthZeroWidthOnly) {
    // Zero-width space U+200B = \xE2\x80\x8B
    EXPECT_EQ(getDisplayWidth("\xE2\x80\x8B"), 0u);
}

// --- moveLeft/moveRight boundary ---

TEST(LineBufferTest, MoveLeftAtBeginning) {
    LineBuffer b;
    b.insert("A");
    b.moveHome();
    b.moveLeft(); // no-op
    EXPECT_EQ(b.getCursorPosition(), 0u);
}

TEST(LineBufferTest, MoveRightAtEnd) {
    LineBuffer b;
    b.insert("A");
    b.moveRight(); // no-op
    EXPECT_EQ(b.getCursorPosition(), 1u);
}

// --- deleteToBeginning/deleteToEnd at boundary ---

TEST(LineBufferTest, DeleteToBeginningAtStart) {
    LineBuffer b;
    b.insert("test");
    b.moveHome();
    b.deleteToBeginning(); // cursor at 0, no-op
    EXPECT_EQ(b.getBuffer(), "test");
}

TEST(LineBufferTest, DeleteToEndAtEnd) {
    LineBuffer b;
    b.insert("test");
    b.deleteToEnd(); // cursor at end, no-op
    EXPECT_EQ(b.getBuffer(), "test");
}
