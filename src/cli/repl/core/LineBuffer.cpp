#include "graph/cli/repl/core/LineBuffer.hpp"
#include <cctype>

namespace zyx {
namespace repl {
namespace core {

size_t getDisplayWidth(const std::string& str) {
    size_t width = 0;
    bool inAnsi = false;
    for (size_t i = 0; i < str.length();) {
        if (str[i] == '\033') {
            inAnsi = true;
            i++;
        } else if (inAnsi) {
            if (std::isalpha(str[i])) inAnsi = false; // End of ANSI sequence (e.g. 'm')
            i++;
        } else {
            unsigned char c = str[i];
            if ((c & 0x80) == 0) { width += 1; i += 1; }
            else if ((c & 0xE0) == 0xC0) { width += 1; i += 2; }
            else if ((c & 0xF0) == 0xE0) { 
                // E2 block often contains 1-column punctuation (like ❯, », ·)
                // E3-E9 block contains CJK characters which are 2 columns.
                if (c == 0xE2) width += 1;
                else width += 2;
                i += 3; 
            }
            else if ((c & 0xF8) == 0xF0) { width += 2; i += 4; }
            else { i += 1; }
        }
    }
    return width;
}

bool LineBuffer::isContinuationByte(char c) const {
    return (c & 0xC0) == 0x80;
}

void LineBuffer::insert(char c) {
    buffer_.insert(cursor_, 1, c);
    cursor_++;
}

void LineBuffer::insert(const std::string& str) {
    buffer_.insert(cursor_, str);
    cursor_ += str.length();
}

void LineBuffer::backspace() {
    if (cursor_ > 0) {
        size_t oldCursor = cursor_;
        moveLeft(); // Move left respecting UTF-8
        buffer_.erase(cursor_, oldCursor - cursor_);
    }
}

void LineBuffer::deleteChar() {
    if (cursor_ < buffer_.length()) {
        size_t oldCursor = cursor_;
        moveRight(); // Move right respecting UTF-8
        buffer_.erase(oldCursor, cursor_ - oldCursor);
        cursor_ = oldCursor;
    }
}

void LineBuffer::deleteToBeginning() {
    if (cursor_ > 0) {
        buffer_.erase(0, cursor_);
        cursor_ = 0;
    }
}

void LineBuffer::deleteToEnd() {
    if (cursor_ < buffer_.length()) {
        buffer_.erase(cursor_);
    }
}

void LineBuffer::deletePreviousWord() {
    if (cursor_ == 0) return;
    size_t oldCursor = cursor_;
    
    // Skip trailing spaces backwards
    while (cursor_ > 0 && buffer_[cursor_ - 1] == ' ') {
        cursor_--;
    }
    // Skip word characters backwards
    while (cursor_ > 0 && buffer_[cursor_ - 1] != ' ') {
        moveLeft();
    }
    
    buffer_.erase(cursor_, oldCursor - cursor_);
}

void LineBuffer::moveLeft() {
    if (cursor_ > 0) {
        cursor_--;
        // Skip over UTF-8 continuation bytes to jump to the start of the current multi-byte char
        while (cursor_ > 0 && isContinuationByte(buffer_[cursor_])) {
            cursor_--;
        }
    }
}

void LineBuffer::moveRight() {
    if (cursor_ < buffer_.length()) {
        cursor_++;
        // Skip over UTF-8 continuation bytes
        while (cursor_ < buffer_.length() && isContinuationByte(buffer_[cursor_])) {
            cursor_++;
        }
    }
}

void LineBuffer::moveHome() {
    cursor_ = 0;
}

void LineBuffer::moveEnd() {
    cursor_ = buffer_.length();
}

const std::string& LineBuffer::getBuffer() const {
    return buffer_;
}

void LineBuffer::setBuffer(const std::string& buffer) {
    buffer_ = buffer;
    cursor_ = buffer_.length();
}

size_t LineBuffer::getCursorPosition() const {
    return cursor_;
}

size_t LineBuffer::getCursorColumn() const {
    // Determine how many visual terminal columns the substring up to the cursor takes
    return getDisplayWidth(buffer_.substr(0, cursor_));
}

void LineBuffer::clear() {
    buffer_.clear();
    cursor_ = 0;
}

} // namespace core
} // namespace repl
} // namespace zyx