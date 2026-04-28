#pragma once

#include <string>
#include <optional>
#include <memory>

// Forward declarations to avoid exposing full dependencies
namespace zyx::repl::frontend { class Editor; }
namespace zyx::repl::io { class ITerminal; }
namespace zyx::repl::core { class History; }

namespace zyx {
namespace repl {

class Session {
public:
    Session(size_t maxHistorySize = 100);
    // Overloaded constructor for dependency injection in tests
    Session(std::shared_ptr<io::ITerminal> terminal, size_t maxHistorySize = 100);
    ~Session();

    std::optional<std::string> readLine(const std::string& prompt);
    void addHistory(const std::string& entry);
    void clearHistory();

private:
    std::shared_ptr<io::ITerminal> terminal_;
    std::shared_ptr<core::History> history_;
    std::unique_ptr<frontend::Editor> editor_;
};

} // namespace repl
} // namespace zyx