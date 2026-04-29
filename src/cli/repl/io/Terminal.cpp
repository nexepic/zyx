#include "graph/cli/repl/io/Terminal.hpp"
#include <iostream>
#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#else
#include <windows.h>
#include <io.h>
#endif

namespace zyx {
namespace repl {
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

// ...

bool Terminal::enableRawMode() {
    if (isRawMode_) return true;

#ifndef _WIN32
    if (!isatty(STDIN_FILENO)) return false;
    
    if (tcgetattr(STDIN_FILENO, &origTermios_) == -1) return false;

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
    if (GetConsoleMode(hConsoleIn_, &consolemodeIn_)) {
        // 0x0200 is ENABLE_VIRTUAL_TERMINAL_INPUT (allows Windows to send ESC sequences for arrow keys)
        SetConsoleMode(hConsoleIn_, ENABLE_PROCESSED_INPUT | 0x0200);
    }
    if (GetConsoleMode(hConsoleOut_, &consolemodeOut_)) {
        SetConsoleMode(hConsoleOut_, consolemodeOut_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    isRawMode_ = true;
    return true;
#endif
}

void Terminal::disableRawMode() {
    if (!isRawMode_) return;

#ifndef _WIN32
    // Drain typeahead before disabling raw mode, solving the multiline paste empty line bug
    int bytes_available = 0;
    if (ioctl(STDIN_FILENO, FIONREAD, &bytes_available) == 0 && bytes_available > 0) {
        char* tmp = new char[bytes_available];
        int n = read(STDIN_FILENO, tmp, bytes_available);
        if (n > 0) {
            typeaheadQueue_.append(tmp, n);
        }
        delete[] tmp;
    }

    if (tcsetattr(STDIN_FILENO, TCSADRAIN, &origTermios_) != -1) {
        isRawMode_ = false;
    }
#else
    if (consolemodeIn_) SetConsoleMode(hConsoleIn_, consolemodeIn_);
    if (consolemodeOut_) SetConsoleMode(hConsoleOut_, consolemodeOut_);
    isRawMode_ = false;
#endif
}

int Terminal::readByte() {
    if (!typeaheadQueue_.empty()) {
        char c = typeaheadQueue_.front();
        typeaheadQueue_.erase(0, 1);
        return c;
    }

    char c;
#ifndef _WIN32
    int nread = read(STDIN_FILENO, &c, 1);
#else
    DWORD nread = 0;
    if (!ReadFile(hConsoleIn_, &c, 1, &nread, NULL)) nread = 0;
#endif
    if (nread <= 0) return -1;
    return static_cast<unsigned char>(c);
}

KeyEvent Terminal::readKey() {
    int c = readByte();
    if (c == -1) return {KeyCode::UNKNOWN, 0};

    // Ignore LF if it comes right after CR in a paste
    static bool previousWasCr = false;
    bool currentIsCr = (c == '\r');

    if (c == '\n' && previousWasCr) {
        previousWasCr = currentIsCr;
        return readKey(); // Recursively get the next actual key
    }
    previousWasCr = currentIsCr;

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
        case 11: return {KeyCode::CTRL_K, 0};
        case 12: return {KeyCode::CTRL_L, 0};
        case 14: return {KeyCode::CTRL_N, 0};
        case 16: return {KeyCode::CTRL_P, 0};
        case 20: return {KeyCode::CTRL_T, 0};
        case 21: return {KeyCode::CTRL_U, 0};
        case 23: return {KeyCode::CTRL_W, 0};
        case 127: // Backspace
        case 8:
            return {KeyCode::BACKSPACE, 0};
        case 27: // Escape sequences
            if (typeaheadQueue_.empty()) {
#ifndef _WIN32
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(STDIN_FILENO, &readfds);
                struct timeval tv = {0, 50000}; // 50ms timeout
                if (select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv) <= 0) {
                    return {KeyCode::ESC, 0};
                }
#else
                // For Windows, WaitForSingleObject with 50ms timeout
                if (WaitForSingleObject(hConsoleIn_, 50) != WAIT_OBJECT_0) {
                    return {KeyCode::ESC, 0};
                }
#endif
            }
            
            char seq[3];
            seq[0] = readByte();
            if (seq[0] == -1) return {KeyCode::ESC, 0};
            
            if (seq[0] == '[') {
                seq[1] = readByte();
                if (seq[1] == -1) return {KeyCode::ESC, 0};
                
                if (seq[1] >= '0' && seq[1] <= '9') {
                    seq[2] = readByte();
                    if (seq[2] == '~' && seq[1] == '3') {
                        return {KeyCode::DELETE_KEY, 0};
                    }
                } else {
                    switch (seq[1]) {
                        case 'A': return {KeyCode::UP, 0};
                        case 'B': return {KeyCode::DOWN, 0};
                        case 'C': return {KeyCode::RIGHT, 0};
                        case 'D': return {KeyCode::LEFT, 0};
                        case 'H': return {KeyCode::HOME_KEY, 0};
                        case 'F': return {KeyCode::END_KEY, 0};
                    }
                }
            } else if (seq[0] == 'O') {
                seq[1] = readByte();
                switch (seq[1]) {
                    case 'H': return {KeyCode::HOME_KEY, 0};
                    case 'F': return {KeyCode::END_KEY, 0};
                }
            }
            return {KeyCode::UNKNOWN, 0};
        default:
            return {KeyCode::CHARACTER, static_cast<char>(c)};
    }
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
} // namespace repl
} // namespace zyx