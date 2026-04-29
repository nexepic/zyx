#pragma once

#include "inputxx/io/ITerminal.hpp"
#include <string>
#include <deque>

#ifndef _WIN32
#include <termios.h>
#endif

namespace inputxx {
namespace io {

class Terminal : public ITerminal {
public:
    Terminal();
    ~Terminal() override;

    bool enableRawMode() override;
    void disableRawMode() override;

    KeyEvent readKey() override;
    void write(const std::string& str) override;
    int getColumns() override;

    void clearScreen() override;
    void beep() override;

private:
    int readByte();

    bool isRawMode_{false};
    bool previousWasCr_{false};
    std::deque<char> typeaheadQueue_;

#ifndef _WIN32
    struct termios origTermios_;
#else
    bool hasSavedConsoleMode_{false};
    unsigned long consolemodeIn_{0};
    unsigned long consolemodeOut_{0};
    void* hConsoleIn_{nullptr};
    void* hConsoleOut_{nullptr};
#endif
};

} // namespace io
} // namespace inputxx
