#pragma once

#include <functional>
#include <string>
#include <optional>
#include <memory>
#include <vector>

// Forward declarations to avoid exposing full dependencies
namespace inputxx::frontend { class Editor; }
namespace inputxx::io { class ITerminal; }
namespace inputxx::core { class History; }

namespace inputxx {

using CompletionCallback = std::function<void(const std::string& line, std::vector<std::string>& completions)>;

class Session {
public:
    Session(size_t maxHistorySize = 100);
    // Overloaded constructor for dependency injection in tests
    Session(std::shared_ptr<io::ITerminal> terminal, size_t maxHistorySize = 100);
    ~Session();

    std::optional<std::string> readLine(const std::string& prompt);
    void addHistory(const std::string& entry);
    void clearHistory();

    bool saveHistory(const std::string& filename) const;
    bool loadHistory(const std::string& filename);

    void setCompletionCallback(CompletionCallback cb);

private:
    std::shared_ptr<io::ITerminal> terminal_;
    std::shared_ptr<core::History> history_;
    std::unique_ptr<frontend::Editor> editor_;
};

} // namespace inputxx
