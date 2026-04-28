#pragma once

#include "graph/cli/repl/core/LineBuffer.hpp"
#include "graph/cli/repl/core/History.hpp"
#include "graph/cli/repl/io/ITerminal.hpp"
#include <string>
#include <memory>
#include <optional>

namespace zyx {
namespace repl {
namespace frontend {

class Editor {
public:
    Editor(std::shared_ptr<io::ITerminal> terminal,
           std::shared_ptr<core::History> history);

    // Reads a single line, returns std::nullopt on EOF (Ctrl-D on empty or Ctrl-C)
    std::optional<std::string> readLine(const std::string& prompt);

private:
    void refreshLine(const std::string& prompt);
    bool handleKey(const io::KeyEvent& key, const std::string& prompt);

    std::shared_ptr<io::ITerminal> terminal_;
    std::shared_ptr<core::History> history_;
    core::LineBuffer buffer_;
    int historyIndex_{0}; // 0 means current buffer, >0 means historical
    std::string currentBufferBackup_; // Backs up current edit when navigating history
};

} // namespace frontend
} // namespace repl
} // namespace zyx