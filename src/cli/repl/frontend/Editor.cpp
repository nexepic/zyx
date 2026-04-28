#include "graph/cli/repl/frontend/Editor.hpp"
#include <cctype>

namespace zyx {
namespace repl {
namespace frontend {

Editor::Editor(std::shared_ptr<io::ITerminal> terminal, std::shared_ptr<core::History> history)
    : terminal_(std::move(terminal)), history_(std::move(history)) {}

std::optional<std::string> Editor::readLine(const std::string& prompt) {
    if (!terminal_->enableRawMode()) {
        // Fallback for non-tty or unsupported term could go here, 
        // but for now we expect raw mode to work.
        return std::nullopt;
    }

    buffer_.clear();
    historyIndex_ = 0;
    currentBufferBackup_.clear();

    terminal_->write(prompt);

    while (true) {
        io::KeyEvent key = terminal_->readKey();
        if (key.code == io::KeyCode::UNKNOWN) continue; // Skip unknown

        if (key.code == io::KeyCode::ENTER) {
            terminal_->write("\r\n");
            break;
        }

        if (key.code == io::KeyCode::CTRL_C) {
            terminal_->disableRawMode();
            terminal_->write("\r\n");
            return std::nullopt;
        }

        if (key.code == io::KeyCode::CTRL_D) {
            if (buffer_.getBuffer().empty()) {
                terminal_->disableRawMode();
                terminal_->write("\r\n");
                return std::nullopt;
            } else {
                buffer_.deleteChar();
            }
        } else {
            handleKey(key, prompt);
        }
        
        refreshLine(prompt);
    }

    terminal_->disableRawMode();
    return buffer_.getBuffer();
}

bool Editor::handleKey(const io::KeyEvent& key, const std::string& prompt) {
    switch (key.code) {
        case io::KeyCode::CHARACTER:
            buffer_.insert(key.character);
            break;
        case io::KeyCode::BACKSPACE:
            buffer_.backspace();
            break;
        case io::KeyCode::DELETE_KEY:
            buffer_.deleteChar();
            break;
        case io::KeyCode::LEFT:
        case io::KeyCode::CTRL_B:
            buffer_.moveLeft();
            break;
        case io::KeyCode::RIGHT:
        case io::KeyCode::CTRL_F:
            buffer_.moveRight();
            break;
        case io::KeyCode::HOME_KEY:
        case io::KeyCode::CTRL_A:
            buffer_.moveHome();
            break;
        case io::KeyCode::END_KEY:
        case io::KeyCode::CTRL_E:
            buffer_.moveEnd();
            break;
        case io::KeyCode::CTRL_U:
            buffer_.deleteToBeginning();
            break;
        case io::KeyCode::CTRL_K:
            buffer_.deleteToEnd();
            break;
        case io::KeyCode::CTRL_W:
            buffer_.deletePreviousWord();
            break;
        case io::KeyCode::CTRL_L:
            terminal_->clearScreen();
            break;
        case io::KeyCode::UP:
        case io::KeyCode::CTRL_P: {
            const auto& entries = history_->getEntries();
            if (entries.empty()) break;
            if (historyIndex_ == 0) {
                currentBufferBackup_ = buffer_.getBuffer();
            }
            if (historyIndex_ < static_cast<int>(entries.size())) {
                historyIndex_++;
                buffer_.setBuffer(entries[entries.size() - historyIndex_]);
            }
            break;
        }
        case io::KeyCode::DOWN:
        case io::KeyCode::CTRL_N: {
            const auto& entries = history_->getEntries();
            if (entries.empty()) break;
            if (historyIndex_ > 1) {
                historyIndex_--;
                buffer_.setBuffer(entries[entries.size() - historyIndex_]);
            } else if (historyIndex_ == 1) {
                historyIndex_ = 0;
                buffer_.setBuffer(currentBufferBackup_);
            }
            break;
        }
        default:
            return false;
    }
    return true;
}

void Editor::refreshLine(const std::string& prompt) {
    std::string seq;
    // Move cursor to left edge
    seq += "\r";
    // Write prompt and buffer
    seq += prompt;
    seq += buffer_.getBuffer();
    // Clear to end of line (deletes leftover text if string shrank)
    seq += "\x1b[0K";
    
    // Move cursor back to the correct position
    // Calculate total display width of prompt + buffer before cursor
    size_t promptCols = core::getDisplayWidth(prompt);
    size_t offsetCols = promptCols + buffer_.getCursorColumn();

    seq += "\r";
    if (offsetCols > 0) {
        seq += "\x1b[" + std::to_string(offsetCols) + "C";
    }

    terminal_->write(seq);
}

} // namespace frontend
} // namespace repl
} // namespace zyx