#include "inputxx/Session.hpp"
#include "inputxx/frontend/Editor.hpp"
#include "inputxx/io/Terminal.hpp"
#include "inputxx/core/History.hpp"

namespace inputxx {

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

bool Session::saveHistory(const std::string& filename) const {
    return history_->save(filename);
}

bool Session::loadHistory(const std::string& filename) {
    return history_->load(filename);
}

void Session::setCompletionCallback(CompletionCallback cb) {
    editor_->setCompletionCallback(std::move(cb));
}

} // namespace inputxx
