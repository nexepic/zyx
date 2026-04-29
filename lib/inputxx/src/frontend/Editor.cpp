#include "inputxx/frontend/Editor.hpp"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

namespace inputxx {
namespace frontend {

Editor::Editor(std::shared_ptr<io::ITerminal> terminal, std::shared_ptr<core::History> history)
    : terminal_(std::move(terminal)), history_(std::move(history)) {}

void Editor::setCompletionCallback(CompletionCallback cb) {
    completionCallback_ = std::move(cb);
}

std::optional<std::string> Editor::readLine(const std::string& prompt) {
    if (!terminal_->enableRawMode()) {
        // Non-TTY or dumb terminal fallback: use standard line input
        std::cout << prompt << std::flush;
        std::string line;
        if (std::getline(std::cin, line)) {
            return line;
        }
        return std::nullopt;
    }

    buffer_.clear();
    historyIndex_ = 0;
    currentBufferBackup_.clear();
    completionState_ = CompletionState{};

    terminal_->write(prompt);

    while (true) {
        io::KeyEvent key = terminal_->readKey();
        if (key.code == io::KeyCode::UNKNOWN) continue;

        if (key.code == io::KeyCode::ENTER) {
            // If in completion mode, accept current selection
            completionState_ = CompletionState{};
            terminal_->write("\r\n");
            break;
        }

        if (key.code == io::KeyCode::CTRL_C) {
            completionState_ = CompletionState{};
            terminal_->disableRawMode();
            terminal_->write("\r\n");
            return std::nullopt;
        }

        if (key.code == io::KeyCode::CTRL_D) {
            if (buffer_.getBuffer().empty()) {
                completionState_ = CompletionState{};
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

void Editor::handleKey(const io::KeyEvent& key, const std::string& prompt) {
    // Handle completion state transitions for non-TAB keys
    if (completionState_.active && key.code != io::KeyCode::TAB) {
        if (key.code == io::KeyCode::ESC) {
            // ESC during completion: revert to original
            buffer_.setBuffer(completionState_.originalBuffer);
            completionState_ = CompletionState{};
            return;
        }
        // Any other key: accept current completion and process the key
        completionState_ = CompletionState{};
    }

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
        case io::KeyCode::CTRL_LEFT:
            buffer_.moveWordLeft();
            break;
        case io::KeyCode::CTRL_RIGHT:
            buffer_.moveWordRight();
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
        case io::KeyCode::CTRL_T:
            buffer_.transpose();
            break;
        case io::KeyCode::CTRL_L:
            terminal_->clearScreen();
            break;
        case io::KeyCode::TAB: {
            if (!completionCallback_) {
                terminal_->beep();
                break;
            }
            if (!completionState_.active) {
                // Start new completion
                std::vector<std::string> completions;
                completionCallback_(buffer_.getBuffer(), completions);
                if (completions.empty()) {
                    terminal_->beep();
                    break;
                }
                if (completions.size() == 1) {
                    buffer_.setBuffer(completions[0]);
                    break;
                }
                completionState_.active = true;
                completionState_.completions = std::move(completions);
                completionState_.originalBuffer = buffer_.getBuffer();
                completionState_.index = 0;
                buffer_.setBuffer(completionState_.completions[0]);
            } else {
                // Cycle through completions
                completionState_.index++;
                if (completionState_.index >= static_cast<int>(completionState_.completions.size())) {
                    completionState_.index = -1;
                    buffer_.setBuffer(completionState_.originalBuffer);
                } else {
                    buffer_.setBuffer(completionState_.completions[completionState_.index]);
                }
            }
            break;
        }
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
            break;
    }
}

void Editor::refreshLine(const std::string& prompt) {
    int cols = terminal_->getColumns();
    size_t promptWidth = core::getDisplayWidth(prompt);
    size_t cursorCol = buffer_.getCursorColumn();
    const std::string& buf = buffer_.getBuffer();
    size_t bufDisplayWidth = core::getDisplayWidth(buf);

    // Available columns for buffer content
    int availCols = cols - static_cast<int>(promptWidth);
    if (availCols < 1) availCols = 1;

    // Calculate horizontal scroll offset (in visual columns) to keep cursor visible
    size_t scrollCols = 0;
    if (cursorCol >= static_cast<size_t>(availCols)) {
        scrollCols = cursorCol - availCols + 1;
    }

    // Find byte offset corresponding to scrollCols
    size_t startByte = core::displayColToByteOffset(buf, scrollCols);

    // Find the visible portion of the buffer that fits in availCols
    std::string visibleBuf;
    size_t visibleWidth = 0;
    {
        size_t i = startByte;
        bool inAnsi = false;
        while (i < buf.length() && visibleWidth < static_cast<size_t>(availCols)) {
            unsigned char c = static_cast<unsigned char>(buf[i]);
            if (c == '\033') {
                // Include ANSI sequences without counting width
                size_t seqStart = i;
                inAnsi = true;
                i++;
                while (i < buf.length() && inAnsi) {
                    if (std::isalpha(static_cast<unsigned char>(buf[i]))) {
                        inAnsi = false;
                    }
                    i++;
                }
                visibleBuf.append(buf, seqStart, i - seqStart);
            } else {
                size_t charStart = i;
                uint32_t cp = core::decodeUtf8(buf, i);
                int w = core::codepointWidth(cp);
                if (w > 0 && visibleWidth + w > static_cast<size_t>(availCols)) {
                    break; // Don't split a wide character
                }
                visibleBuf.append(buf, charStart, i - charStart);
                if (w > 0) visibleWidth += w;
            }
        }
    }

    // Build output sequence
    std::string seq;
    seq += "\r";
    seq += prompt;
    seq += visibleBuf;
    seq += "\x1b[0K"; // Clear to end of line

    // Position cursor
    size_t cursorVisualCol = promptWidth + cursorCol - scrollCols;
    seq += "\r";
    if (cursorVisualCol > 0) {
        seq += "\x1b[" + std::to_string(cursorVisualCol) + "C";
    }

    terminal_->write(seq);
}

} // namespace frontend
} // namespace inputxx
