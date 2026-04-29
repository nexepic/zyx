#pragma once

#include "inputxx/io/ITerminal.hpp"
#include <gmock/gmock.h>

namespace inputxx {
namespace io {

class MockTerminal : public ITerminal {
public:
    MOCK_METHOD(bool, enableRawMode, (), (override));
    MOCK_METHOD(void, disableRawMode, (), (override));
    MOCK_METHOD(KeyEvent, readKey, (), (override));
    MOCK_METHOD(void, write, (const std::string& str), (override));
    MOCK_METHOD(int, getColumns, (), (override));
    MOCK_METHOD(void, clearScreen, (), (override));
    MOCK_METHOD(void, beep, (), (override));
};

} // namespace io
} // namespace inputxx