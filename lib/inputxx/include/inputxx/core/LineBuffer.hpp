#pragma once

#include <cstdint>
#include <string>

namespace inputxx {
namespace core {

// Decode a single UTF-8 code point from str starting at position i.
// Advances i past the character. Returns the Unicode code point.
uint32_t decodeUtf8(const std::string& str, size_t& i);

// Returns the terminal display width of a Unicode code point.
int codepointWidth(uint32_t cp);

// Returns the total visual display width of a string,
// correctly handling ANSI escape codes, combining characters,
// and East Asian wide characters.
size_t getDisplayWidth(const std::string& str);

// Returns the byte offset in str corresponding to a given visual column offset.
// If targetCols exceeds the string's display width, returns str.length().
size_t displayColToByteOffset(const std::string& str, size_t targetCols);

class LineBuffer {
public:
    LineBuffer() = default;

    void insert(char c);
    void insert(const std::string& str);

    void backspace();
    void deleteChar();

    void deleteToBeginning();
    void deleteToEnd();
    void deletePreviousWord();

    void moveLeft();
    void moveRight();
    void moveHome();
    void moveEnd();
    void moveWordLeft();
    void moveWordRight();

    void transpose();

    const std::string& getBuffer() const;
    void setBuffer(const std::string& buffer);

    size_t getCursorPosition() const; // Byte offset
    size_t getCursorColumn() const;   // Visual terminal column offset
    void clear();

private:
    bool isContinuationByte(char c) const;

    std::string buffer_;
    size_t cursor_{0};
};

} // namespace core
} // namespace inputxx
