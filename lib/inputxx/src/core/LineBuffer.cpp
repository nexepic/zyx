#include "inputxx/core/LineBuffer.hpp"
#include <cctype>

namespace inputxx {
namespace core {

uint32_t decodeUtf8(const std::string& str, size_t& i) {
    if (i >= str.length()) return 0xFFFD;
    unsigned char c = static_cast<unsigned char>(str[i]);
    uint32_t cp;
    int len;

    if ((c & 0x80) == 0)      { cp = c;           len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { i++; return 0xFFFD; } // Invalid leading byte

    if (i + len > str.length()) { i++; return 0xFFFD; } // Truncated sequence

    for (int j = 1; j < len; j++) {
        unsigned char cb = static_cast<unsigned char>(str[i + j]);
        if ((cb & 0xC0) != 0x80) { i++; return 0xFFFD; } // Invalid continuation
        cp = (cp << 6) | (cb & 0x3F);
    }
    i += len;
    return cp;
}

int codepointWidth(uint32_t cp) {
    // C0/C1 control characters
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;

    // Combining characters and zero-width marks
    if ((cp >= 0x0300 && cp <= 0x036F) ||   // Combining Diacritical Marks
        (cp >= 0x0483 && cp <= 0x0489) ||   // Cyrillic combining marks
        (cp >= 0x0591 && cp <= 0x05BD) ||   // Hebrew combining marks
        cp == 0x05BF ||
        (cp >= 0x05C1 && cp <= 0x05C2) ||
        (cp >= 0x05C4 && cp <= 0x05C5) ||
        cp == 0x05C7 ||
        (cp >= 0x0610 && cp <= 0x061A) ||   // Arabic combining marks
        (cp >= 0x064B && cp <= 0x065F) ||
        cp == 0x0670 ||
        (cp >= 0x06D6 && cp <= 0x06DC) ||
        (cp >= 0x06DF && cp <= 0x06E4) ||
        (cp >= 0x06E7 && cp <= 0x06E8) ||
        (cp >= 0x06EA && cp <= 0x06ED) ||
        cp == 0x0711 ||
        (cp >= 0x0730 && cp <= 0x074A) ||   // Syriac
        (cp >= 0x07A6 && cp <= 0x07B0) ||   // Thaana
        (cp >= 0x07EB && cp <= 0x07F3) ||   // NKo
        (cp >= 0x0816 && cp <= 0x0819) ||
        (cp >= 0x081B && cp <= 0x0823) ||
        (cp >= 0x0825 && cp <= 0x0827) ||
        (cp >= 0x0829 && cp <= 0x082D) ||
        (cp >= 0x0859 && cp <= 0x085B) ||
        (cp >= 0x08D4 && cp <= 0x08E1) ||
        (cp >= 0x08E3 && cp <= 0x0902) ||
        (cp >= 0x093A && cp <= 0x093C) ||   // Devanagari
        (cp >= 0x0941 && cp <= 0x0948) ||
        cp == 0x094D ||
        (cp >= 0x0951 && cp <= 0x0957) ||
        (cp >= 0x0962 && cp <= 0x0963) ||
        cp == 0x0981 ||
        cp == 0x09BC ||
        (cp >= 0x09C1 && cp <= 0x09C4) ||
        cp == 0x09CD ||
        (cp >= 0x09E2 && cp <= 0x09E3) ||
        (cp >= 0x0A01 && cp <= 0x0A02) ||
        cp == 0x0A3C ||
        (cp >= 0x0A41 && cp <= 0x0A42) ||
        (cp >= 0x0A47 && cp <= 0x0A48) ||
        (cp >= 0x0A4B && cp <= 0x0A4D) ||
        cp == 0x0A51 ||
        (cp >= 0x0A70 && cp <= 0x0A71) ||
        cp == 0x0A75 ||
        (cp >= 0x0A81 && cp <= 0x0A82) ||
        cp == 0x0ABC ||
        (cp >= 0x0AC1 && cp <= 0x0AC5) ||
        (cp >= 0x0AC7 && cp <= 0x0AC8) ||
        cp == 0x0ACD ||
        (cp >= 0x0AE2 && cp <= 0x0AE3) ||
        cp == 0x0E31 ||                     // Thai
        (cp >= 0x0E34 && cp <= 0x0E3A) ||
        (cp >= 0x0E47 && cp <= 0x0E4E) ||
        cp == 0x0EB1 ||                     // Lao
        (cp >= 0x0EB4 && cp <= 0x0EB9) ||
        (cp >= 0x0EBB && cp <= 0x0EBC) ||
        (cp >= 0x0EC8 && cp <= 0x0ECD) ||
        (cp >= 0x1AB0 && cp <= 0x1AFF) ||   // Combining Diacritical Marks Extended
        (cp >= 0x1DC0 && cp <= 0x1DFF) ||   // Combining Diacritical Marks Supplement
        (cp >= 0x20D0 && cp <= 0x20FF) ||   // Combining Diacriticals for Symbols
        (cp >= 0xFE00 && cp <= 0xFE0F) ||   // Variation Selectors
        (cp >= 0xFE20 && cp <= 0xFE2F) ||   // Combining Half Marks
        (cp >= 0xE0100 && cp <= 0xE01EF))   // Variation Selectors Supplement
        return 0;

    // Other zero-width characters
    if (cp == 0x00AD ||                     // Soft hyphen
        cp == 0x200B || cp == 0x200C ||     // Zero-width space/non-joiner
        cp == 0x200D ||                     // Zero-width joiner
        (cp >= 0x200E && cp <= 0x200F) ||   // LRM/RLM
        (cp >= 0x2028 && cp <= 0x202E) ||   // Line/paragraph separators, bidi
        (cp >= 0x2060 && cp <= 0x2069) ||   // Word joiner, invisible separators
        cp == 0xFEFF)                       // BOM / Zero-width no-break space
        return 0;

    // East Asian Wide and Fullwidth characters
    if ((cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
        cp == 0x2329 || cp == 0x232A ||     // Left/Right-pointing angle bracket
        (cp >= 0x2E80 && cp <= 0x303E) ||   // CJK Radicals, Kangxi, Ideographic Description, CJK Symbols
        (cp >= 0x3040 && cp <= 0x33FF) ||   // Hiragana, Katakana, Bopomofo, Hangul Compat, Kanbun, CJK Compat
        (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Unified Ideographs Extension A
        (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified Ideographs
        (cp >= 0xA000 && cp <= 0xA4CF) ||   // Yi Syllables + Yi Radicals
        (cp >= 0xA960 && cp <= 0xA97F) ||   // Hangul Jamo Extended-A
        (cp >= 0xAC00 && cp <= 0xD7AF) ||   // Hangul Syllables
        (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compatibility Ideographs
        (cp >= 0xFE10 && cp <= 0xFE19) ||   // Vertical Forms
        (cp >= 0xFE30 && cp <= 0xFE6F) ||   // CJK Compatibility Forms + Small Form Variants
        (cp >= 0xFF00 && cp <= 0xFF60) ||   // Fullwidth Forms
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||   // Fullwidth Signs
        (cp >= 0x1F000 && cp <= 0x1F02F) || // Mahjong Tiles
        (cp >= 0x1F030 && cp <= 0x1F09F) || // Domino Tiles
        (cp >= 0x1F0A0 && cp <= 0x1F0FF) || // Playing Cards
        (cp >= 0x1F200 && cp <= 0x1F2FF) || // Enclosed Ideographic Supplement
        (cp >= 0x1F300 && cp <= 0x1F64F) || // Misc Symbols/Pictographs + Emoticons
        (cp >= 0x1F680 && cp <= 0x1F6FF) || // Transport/Map Symbols
        (cp >= 0x1F900 && cp <= 0x1F9FF) || // Supplemental Symbols and Pictographs
        (cp >= 0x1FA00 && cp <= 0x1FA6F) || // Chess Symbols
        (cp >= 0x1FA70 && cp <= 0x1FAFF) || // Symbols and Pictographs Extended-A
        (cp >= 0x20000 && cp <= 0x2FFFF) || // CJK Extensions B-F
        (cp >= 0x30000 && cp <= 0x3FFFF))   // CJK Extension G+
        return 2;

    return 1;
}

size_t getDisplayWidth(const std::string& str) {
    size_t width = 0;
    bool inAnsi = false;
    for (size_t i = 0; i < str.length();) {
        unsigned char c = static_cast<unsigned char>(str[i]);
        if (c == '\033') {
            inAnsi = true;
            i++;
        } else if (inAnsi) {
            if (std::isalpha(c)) inAnsi = false;
            i++;
        } else {
            uint32_t cp = decodeUtf8(str, i);
            int w = codepointWidth(cp);
            if (w > 0) width += w;
        }
    }
    return width;
}

size_t displayColToByteOffset(const std::string& str, size_t targetCols) {
    size_t cols = 0;
    bool inAnsi = false;
    for (size_t i = 0; i < str.length();) {
        if (cols >= targetCols) return i;
        unsigned char c = static_cast<unsigned char>(str[i]);
        if (c == '\033') {
            inAnsi = true;
            i++;
        } else if (inAnsi) {
            if (std::isalpha(c)) inAnsi = false;
            i++;
        } else {
            size_t charStart = i;
            uint32_t cp = decodeUtf8(str, i);
            int w = codepointWidth(cp);
            if (w > 0) {
                if (cols + w > targetCols) return charStart; // Don't split wide char
                cols += w;
            }
        }
    }
    return str.length();
}

bool LineBuffer::isContinuationByte(char c) const {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
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
        moveLeft();
        buffer_.erase(cursor_, oldCursor - cursor_);
    }
}

void LineBuffer::deleteChar() {
    if (cursor_ < buffer_.length()) {
        size_t oldCursor = cursor_;
        moveRight();
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

    // Skip trailing spaces backwards (byte-level is safe: ASCII space 0x20 is never a continuation byte)
    while (cursor_ > 0 && buffer_[cursor_ - 1] == ' ') {
        cursor_--;
    }
    // Skip word characters backwards (use moveLeft for UTF-8 safety)
    while (cursor_ > 0 && buffer_[cursor_ - 1] != ' ') {
        moveLeft();
    }

    buffer_.erase(cursor_, oldCursor - cursor_);
}

void LineBuffer::moveLeft() {
    if (cursor_ > 0) {
        cursor_--;
        while (cursor_ > 0 && isContinuationByte(buffer_[cursor_])) {
            cursor_--;
        }
    }
}

void LineBuffer::moveRight() {
    if (cursor_ < buffer_.length()) {
        cursor_++;
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

void LineBuffer::moveWordLeft() {
    if (cursor_ == 0) return;
    // Skip spaces backward
    while (cursor_ > 0 && buffer_[cursor_ - 1] == ' ') {
        cursor_--;
    }
    // Skip word characters backward (use moveLeft for UTF-8 safety)
    while (cursor_ > 0 && buffer_[cursor_ - 1] != ' ') {
        moveLeft();
    }
}

void LineBuffer::moveWordRight() {
    if (cursor_ >= buffer_.length()) return;
    // Skip current word characters forward
    while (cursor_ < buffer_.length() && buffer_[cursor_] != ' ') {
        moveRight();
    }
    // Skip spaces forward
    while (cursor_ < buffer_.length() && buffer_[cursor_] == ' ') {
        cursor_++;
    }
}

void LineBuffer::transpose() {
    if (buffer_.length() < 2 || cursor_ == 0) return;

    // If at end of line, move cursor back so we're between the last two chars
    bool wasAtEnd = (cursor_ == buffer_.length());
    if (wasAtEnd) {
        moveLeft();
    }

    // cursor_ now points at the second character
    size_t secondStart = cursor_;

    // Find the end of the second character
    size_t savedCursor = cursor_;
    moveRight();
    size_t secondEnd = cursor_;

    // Find the start of the first character
    cursor_ = secondStart;
    moveLeft();
    size_t firstStart = cursor_;

    // Extract both characters
    std::string firstChar = buffer_.substr(firstStart, secondStart - firstStart);
    std::string secondChar = buffer_.substr(secondStart, secondEnd - secondStart);

    // Replace: swap them
    buffer_.replace(firstStart, secondEnd - firstStart, secondChar + firstChar);

    // Position cursor after both characters
    cursor_ = secondEnd;
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
    return getDisplayWidth(buffer_.substr(0, cursor_));
}

void LineBuffer::clear() {
    buffer_.clear();
    cursor_ = 0;
}

} // namespace core
} // namespace inputxx
