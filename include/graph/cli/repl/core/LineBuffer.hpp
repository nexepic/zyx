#pragma once

#include <string>

namespace zyx {
namespace repl {
namespace core {

// Helper to calculate the visual display width of a string, ignoring ANSI color codes
// and correctly estimating UTF-8 character widths.
size_t getDisplayWidth(const std::string& str);

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
} // namespace repl
} // namespace zyx