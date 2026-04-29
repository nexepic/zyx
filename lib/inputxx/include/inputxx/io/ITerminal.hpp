#pragma once
#include <string>

namespace inputxx {
namespace io {

enum class KeyCode {
    UNKNOWN = 0,
    ENTER,
    BACKSPACE,
    DELETE_KEY,
    TAB,
    ESC,
    UP,
    DOWN,
    RIGHT,
    LEFT,
    HOME_KEY,
    END_KEY,
    CTRL_A,
    CTRL_B,
    CTRL_C,
    CTRL_D,
    CTRL_E,
    CTRL_F,
    CTRL_K,
    CTRL_L,
    CTRL_N,
    CTRL_P,
    CTRL_T,
    CTRL_U,
    CTRL_W,
    CTRL_LEFT,  // Word left (Ctrl+Arrow)
    CTRL_RIGHT, // Word right (Ctrl+Arrow)
    CHARACTER   // Standard printable character
};

struct KeyEvent {
    KeyCode code{KeyCode::UNKNOWN};
    char character{0};
};

class ITerminal {
public:
    virtual ~ITerminal() = default;

    virtual bool enableRawMode() = 0;
    virtual void disableRawMode() = 0;
    
    virtual KeyEvent readKey() = 0;
    virtual void write(const std::string& str) = 0;
    virtual int getColumns() = 0;
    
    virtual void clearScreen() = 0;
    virtual void beep() = 0;
};

} // namespace io
} // namespace inputxx