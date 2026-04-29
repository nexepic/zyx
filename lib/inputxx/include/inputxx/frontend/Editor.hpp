#pragma once

#include "inputxx/core/LineBuffer.hpp"
#include "inputxx/core/History.hpp"
#include "inputxx/io/ITerminal.hpp"
#include <functional>
#include <string>
#include <memory>
#include <optional>
#include <vector>

namespace inputxx {
namespace frontend {

using CompletionCallback = std::function<void(const std::string& line, std::vector<std::string>& completions)>;

class Editor {
public:
    Editor(std::shared_ptr<io::ITerminal> terminal,
           std::shared_ptr<core::History> history);

    // Reads a single line, returns std::nullopt on EOF (Ctrl-D on empty or Ctrl-C)
    std::optional<std::string> readLine(const std::string& prompt);

    void setCompletionCallback(CompletionCallback cb);

private:
    void refreshLine(const std::string& prompt);
    void handleKey(const io::KeyEvent& key, const std::string& prompt);

    std::shared_ptr<io::ITerminal> terminal_;
    std::shared_ptr<core::History> history_;
    core::LineBuffer buffer_;
    int historyIndex_{0};
    std::string currentBufferBackup_;

    // Tab completion state
    CompletionCallback completionCallback_;
    struct CompletionState {
        bool active{false};
        std::vector<std::string> completions;
        int index{-1}; // -1 = original, 0+ = completion index
        std::string originalBuffer;
    };
    CompletionState completionState_;
};

} // namespace frontend
} // namespace inputxx
