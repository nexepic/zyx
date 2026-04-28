#include "graph/cli/repl/core/History.hpp"

namespace zyx {
namespace repl {
namespace core {

History::History(size_t maxSize) : maxSize_(maxSize) {}

void History::add(const std::string& entry) {
    if (entry.empty() || maxSize_ == 0) return;

    if (!entries_.empty() && entries_.back() == entry) {
        return; // Deduplicate consecutive identical entries
    }

    if (entries_.size() >= maxSize_) {
        entries_.erase(entries_.begin());
    }
    entries_.push_back(entry);
}

void History::setMaxSize(size_t maxSize) {
    maxSize_ = maxSize;
    if (entries_.size() > maxSize_) {
        size_t toRemove = entries_.size() - maxSize_;
        entries_.erase(entries_.begin(), entries_.begin() + toRemove);
    }
}

size_t History::getMaxSize() const {
    return maxSize_;
}

const std::vector<std::string>& History::getEntries() const {
    return entries_;
}

void History::clear() {
    entries_.clear();
}

} // namespace core
} // namespace repl
} // namespace zyx