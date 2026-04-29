#include "inputxx/io/Terminal.hpp"
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#endif

namespace {

// atexit handler to restore terminal state if process crashes in raw mode
#ifndef _WIN32
static struct termios savedOrigTermios;
static bool termiosSaved = false;
#else
static DWORD savedConsoleModeIn = 0;
static DWORD savedConsoleModeOut = 0;
static HANDLE savedHConsoleIn = INVALID_HANDLE_VALUE;
static HANDLE savedHConsoleOut = INVALID_HANDLE_VALUE;
static bool consoleModesSaved = false;
#endif
static bool atExitRegistered = false;

static void terminalAtExit() {
#ifndef _WIN32
    if (termiosSaved) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &savedOrigTermios);
    }
#else
    if (consoleModesSaved) {
        SetConsoleMode(savedHConsoleIn, savedConsoleModeIn);
        SetConsoleMode(savedHConsoleOut, savedConsoleModeOut);
    }
#endif
}

} // anonymous namespace

namespace inputxx {
namespace io {

Terminal::Terminal() {
#ifdef _WIN32
    hConsoleIn_ = GetStdHandle(STD_INPUT_HANDLE);
    hConsoleOut_ = GetStdHandle(STD_OUTPUT_HANDLE);
#endif
}

Terminal::~Terminal() {
    disableRawMode();
}

bool Terminal::enableRawMode() {
    if (isRawMode_) return true;

    // Register atexit handler once to restore terminal on abnormal exit
    if (!atExitRegistered) {
        std::atexit(terminalAtExit);
        atExitRegistered = true;
    }

#ifndef _WIN32
    if (!isatty(STDIN_FILENO)) return false;

    // Check for dumb terminal
    const char* term = std::getenv("TERM");
    if (term && std::strcmp(term, "dumb") == 0) return false;

    if (tcgetattr(STDIN_FILENO, &origTermios_) == -1) return false;

    // Save original state for atexit handler
    if (!termiosSaved) {
        savedOrigTermios = origTermios_;
        termiosSaved = true;
    }

    struct termios raw = origTermios_;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) return false;
    isRawMode_ = true;
    return true;
#else
    // Check if handles are valid consoles
    DWORD modeIn = 0;
    if (!GetConsoleMode(hConsoleIn_, &modeIn)) return false;

    DWORD modeOut = 0;
    GetConsoleMode(hConsoleOut_, &modeOut);

    consolemodeIn_ = modeIn;
    consolemodeOut_ = modeOut;
    hasSavedConsoleMode_ = true;

    // Save for atexit handler
    if (!consoleModesSaved) {
        savedConsoleModeIn = modeIn;
        savedConsoleModeOut = modeOut;
        savedHConsoleIn = hConsoleIn_;
        savedHConsoleOut = hConsoleOut_;
        consoleModesSaved = true;
    }

    // Disable line input and echo; enable window input for resize events
    SetConsoleMode(hConsoleIn_, ENABLE_WINDOW_INPUT);

    // Enable VT processing for ANSI escape sequences in output
    SetConsoleMode(hConsoleOut_, modeOut | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    isRawMode_ = true;
    return true;
#endif
}

void Terminal::disableRawMode() {
    if (!isRawMode_) return;

#ifndef _WIN32
    // Drain typeahead before disabling raw mode (fixes multiline paste empty line bug)
    int bytes_available = 0;
    if (ioctl(STDIN_FILENO, FIONREAD, &bytes_available) == 0 && bytes_available > 0) {
        std::vector<char> tmp(bytes_available);
        int n = read(STDIN_FILENO, tmp.data(), bytes_available);
        if (n > 0) {
            typeaheadQueue_.insert(typeaheadQueue_.end(), tmp.begin(), tmp.begin() + n);
        }
    }

    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &origTermios_) != -1) {
        isRawMode_ = false;
    }
#else
    FlushConsoleInputBuffer(hConsoleIn_);
    if (hasSavedConsoleMode_) {
        SetConsoleMode(hConsoleIn_, consolemodeIn_);
        SetConsoleMode(hConsoleOut_, consolemodeOut_);
    }
    isRawMode_ = false;
#endif
}

int Terminal::readByte() {
    if (!typeaheadQueue_.empty()) {
        unsigned char c = static_cast<unsigned char>(typeaheadQueue_.front());
        typeaheadQueue_.pop_front();
        return c;
    }

#ifndef _WIN32
    char c;
    int nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return -1;
    return static_cast<unsigned char>(c);
#else
    // readByte is not used on Windows (ReadConsoleInput used instead)
    return -1;
#endif
}

KeyEvent Terminal::readKey() {
#ifdef _WIN32
    // Windows: use ReadConsoleInputW for proper key event handling
    // First drain any typeahead (from multi-byte UTF-8 character splitting)
    if (!typeaheadQueue_.empty()) {
        unsigned char c = static_cast<unsigned char>(typeaheadQueue_.front());
        typeaheadQueue_.pop_front();
        return {KeyCode::CHARACTER, static_cast<char>(c)};
    }

    while (true) {
        INPUT_RECORD record;
        DWORD eventsRead;
        if (!ReadConsoleInputW(hConsoleIn_, &record, 1, &eventsRead) || eventsRead == 0) {
            return {KeyCode::UNKNOWN, 0};
        }

        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
            continue;
        }

        auto& ke = record.Event.KeyEvent;
        WORD vk = ke.wVirtualKeyCode;
        wchar_t wc = ke.uChar.UnicodeChar;
        DWORD ctrl = ke.dwControlKeyState;
        bool ctrlPressed = (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        bool altPressed = (ctrl & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

        // Skip pure modifier key presses
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_CAPITAL) {
            continue;
        }

        // Ctrl+key combinations (check VK code for non-character combos)
        if (ctrlPressed && !altPressed) {
            switch (vk) {
                case VK_LEFT:   return {KeyCode::CTRL_LEFT, 0};
                case VK_RIGHT:  return {KeyCode::CTRL_RIGHT, 0};
                case 'A': return {KeyCode::CTRL_A, 0};
                case 'B': return {KeyCode::CTRL_B, 0};
                case 'C': return {KeyCode::CTRL_C, 0};
                case 'D': return {KeyCode::CTRL_D, 0};
                case 'E': return {KeyCode::CTRL_E, 0};
                case 'F': return {KeyCode::CTRL_F, 0};
                case 'K': return {KeyCode::CTRL_K, 0};
                case 'L': return {KeyCode::CTRL_L, 0};
                case 'N': return {KeyCode::CTRL_N, 0};
                case 'P': return {KeyCode::CTRL_P, 0};
                case 'T': return {KeyCode::CTRL_T, 0};
                case 'U': return {KeyCode::CTRL_U, 0};
                case 'W': return {KeyCode::CTRL_W, 0};
            }
        }

        // Special keys (no modifier)
        switch (vk) {
            case VK_RETURN:  return {KeyCode::ENTER, '\r'};
            case VK_BACK:    return {KeyCode::BACKSPACE, 0};
            case VK_DELETE:  return {KeyCode::DELETE_KEY, 0};
            case VK_TAB:     return {KeyCode::TAB, 0};
            case VK_ESCAPE:  return {KeyCode::ESC, 0};
            case VK_UP:      return {KeyCode::UP, 0};
            case VK_DOWN:    return {KeyCode::DOWN, 0};
            case VK_LEFT:    return {KeyCode::LEFT, 0};
            case VK_RIGHT:   return {KeyCode::RIGHT, 0};
            case VK_HOME:    return {KeyCode::HOME_KEY, 0};
            case VK_END:     return {KeyCode::END_KEY, 0};
        }

        // Regular printable character
        if (wc >= 32) {
            // Convert wchar_t to UTF-8
            if (wc < 0x80) {
                return {KeyCode::CHARACTER, static_cast<char>(wc)};
            }
            char utf8[4];
            int len = 0;
            if (wc < 0x800) {
                utf8[0] = static_cast<char>(0xC0 | (wc >> 6));
                utf8[1] = static_cast<char>(0x80 | (wc & 0x3F));
                len = 2;
            } else {
                // BMP character (or unpaired surrogate, which we pass through)
                utf8[0] = static_cast<char>(0xE0 | (wc >> 12));
                utf8[1] = static_cast<char>(0x80 | ((wc >> 6) & 0x3F));
                utf8[2] = static_cast<char>(0x80 | (wc & 0x3F));
                len = 3;
            }
            // Return first byte, queue the rest
            for (int i = 1; i < len; i++) {
                typeaheadQueue_.push_back(utf8[i]);
            }
            return {KeyCode::CHARACTER, utf8[0]};
        }

        // Control characters (Ctrl+letter generates 1-26)
        if (wc >= 1 && wc <= 26) {
            switch (wc) {
                case 1:  return {KeyCode::CTRL_A, 0};
                case 2:  return {KeyCode::CTRL_B, 0};
                case 3:  return {KeyCode::CTRL_C, 0};
                case 4:  return {KeyCode::CTRL_D, 0};
                case 5:  return {KeyCode::CTRL_E, 0};
                case 6:  return {KeyCode::CTRL_F, 0};
                case 11: return {KeyCode::CTRL_K, 0};
                case 12: return {KeyCode::CTRL_L, 0};
                case 14: return {KeyCode::CTRL_N, 0};
                case 16: return {KeyCode::CTRL_P, 0};
                case 20: return {KeyCode::CTRL_T, 0};
                case 21: return {KeyCode::CTRL_U, 0};
                case 23: return {KeyCode::CTRL_W, 0};
                case 9:  return {KeyCode::TAB, 0};
                case 13: return {KeyCode::ENTER, '\r'};
                case 8:  return {KeyCode::BACKSPACE, 0};
                case 127: return {KeyCode::BACKSPACE, 0};
            }
        }
    }
#else
    // Unix: use readByte + escape sequence parsing
    while (true) {
        int c = readByte();
        if (c == -1) return {KeyCode::UNKNOWN, 0};

        // Ignore LF that comes right after CR (handles \r\n in paste)
        if (c == '\n' && previousWasCr_) {
            previousWasCr_ = false;
            continue; // Loop instead of recursion to avoid stack overflow
        }
        previousWasCr_ = (c == '\r');

        switch (c) {
            case '\r':
            case '\n':
                return {KeyCode::ENTER, '\r'};
            case 1:  return {KeyCode::CTRL_A, 0};
            case 2:  return {KeyCode::CTRL_B, 0};
            case 3:  return {KeyCode::CTRL_C, 0};
            case 4:  return {KeyCode::CTRL_D, 0};
            case 5:  return {KeyCode::CTRL_E, 0};
            case 6:  return {KeyCode::CTRL_F, 0};
            case 9:  return {KeyCode::TAB, 0};
            case 11: return {KeyCode::CTRL_K, 0};
            case 12: return {KeyCode::CTRL_L, 0};
            case 14: return {KeyCode::CTRL_N, 0};
            case 16: return {KeyCode::CTRL_P, 0};
            case 20: return {KeyCode::CTRL_T, 0};
            case 21: return {KeyCode::CTRL_U, 0};
            case 23: return {KeyCode::CTRL_W, 0};
            case 127: // Backspace (most terminals)
            case 8:   // Backspace (some terminals)
                return {KeyCode::BACKSPACE, 0};
            case 27: { // Escape sequences
                if (typeaheadQueue_.empty()) {
                    fd_set readfds;
                    FD_ZERO(&readfds);
                    FD_SET(STDIN_FILENO, &readfds);
                    struct timeval tv = {0, 50000}; // 50ms timeout
                    if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv) <= 0) {
                        return {KeyCode::ESC, 0};
                    }
                }

                int seq0 = readByte();
                if (seq0 == -1) return {KeyCode::ESC, 0};

                if (seq0 == '[') {
                    int seq1 = readByte();
                    if (seq1 == -1) return {KeyCode::ESC, 0};

                    if (seq1 >= '0' && seq1 <= '9') {
                        int seq2 = readByte();
                        if (seq2 == -1) return {KeyCode::ESC, 0};
                        if (seq2 == '~') {
                            if (seq1 == '3') return {KeyCode::DELETE_KEY, 0};
                            // 1~ = Home, 4~ = End on some terminals
                            if (seq1 == '1') return {KeyCode::HOME_KEY, 0};
                            if (seq1 == '4') return {KeyCode::END_KEY, 0};
                        } else if (seq2 == ';') {
                            // Modified key: ESC [ 1 ; <modifier> <key>
                            int modifier = readByte();
                            int key = readByte();
                            if (modifier == '5') { // Ctrl modifier
                                switch (key) {
                                    case 'C': return {KeyCode::CTRL_RIGHT, 0};
                                    case 'D': return {KeyCode::CTRL_LEFT, 0};
                                }
                            }
                        }
                    } else {
                        switch (seq1) {
                            case 'A': return {KeyCode::UP, 0};
                            case 'B': return {KeyCode::DOWN, 0};
                            case 'C': return {KeyCode::RIGHT, 0};
                            case 'D': return {KeyCode::LEFT, 0};
                            case 'H': return {KeyCode::HOME_KEY, 0};
                            case 'F': return {KeyCode::END_KEY, 0};
                        }
                    }
                } else if (seq0 == 'O') {
                    int seq1 = readByte();
                    switch (seq1) {
                        case 'H': return {KeyCode::HOME_KEY, 0};
                        case 'F': return {KeyCode::END_KEY, 0};
                    }
                } else if (seq0 == 'b') {
                    // Alt+b: word left (some terminals)
                    return {KeyCode::CTRL_LEFT, 0};
                } else if (seq0 == 'f') {
                    // Alt+f: word right (some terminals)
                    return {KeyCode::CTRL_RIGHT, 0};
                }
                return {KeyCode::UNKNOWN, 0};
            }
            default:
                return {KeyCode::CHARACTER, static_cast<char>(c)};
        }
    }
#endif
}

void Terminal::write(const std::string& str) {
#ifndef _WIN32
    ::write(STDOUT_FILENO, str.c_str(), str.length());
#else
    DWORD written;
    WriteFile(hConsoleOut_, str.c_str(), static_cast<DWORD>(str.length()), &written, NULL);
#endif
}

int Terminal::getColumns() {
#ifndef _WIN32
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return 80;
    }
    return ws.ws_col;
#else
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hConsoleOut_, &csbi)) {
        return 80;
    }
    return csbi.srWindow.Right - csbi.srWindow.Left + 1;
#endif
}

void Terminal::clearScreen() {
    write("\x1b[H\x1b[2J");
}

void Terminal::beep() {
    write("\x7");
}

} // namespace io
} // namespace inputxx
