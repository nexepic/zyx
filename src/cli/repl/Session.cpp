#include "graph/cli/repl/Session.hpp"
#include "graph/cli/repl/frontend/Editor.hpp"
#include "graph/cli/repl/io/Terminal.hpp"
#include "graph/cli/repl/core/History.hpp"

namespace zyx {
namespace repl {

Session::Session(size_t maxHistorySize)
    : terminal_(std::make_shared<io::Terminal>()),
      history_(std::make_shared<core::History>(maxHistorySize)) {
    editor_ = std::make_unique<frontend::Editor>(terminal_, history_);
}

Session::Session(std::shared_ptr<io::ITerminal> terminal, size_t maxHistorySize)
    : terminal_(std::move(terminal)),
      history_(std::make_shared<core::History>(maxHistorySize)) {
    editor_ = std::make_unique<frontend::Editor>(terminal_, history_);
}

Session::~Session() = default;

std::optional<std::string> Session::readLine(const std::string& prompt) {
    return editor_->readLine(prompt);
}

void Session::addHistory(const std::string& entry) {
    history_->add(entry);
}

void Session::clearHistory() {
    history_->clear();
}

} // namespace repl
} // namespace zyx