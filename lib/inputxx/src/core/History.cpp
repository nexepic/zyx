#include "inputxx/core/History.hpp"
#include <fstream>

namespace inputxx {
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

bool History::save(const std::string& filename) const {
    std::ofstream ofs(filename, std::ios::trunc);
    if (!ofs.is_open()) return false;

    for (const auto& entry : entries_) {
        ofs << entry << '\n';
    }
    return ofs.good();
}

bool History::load(const std::string& filename) {
    std::ifstream ifs(filename);
    if (!ifs.is_open()) return false;

    entries_.clear();
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        if (entries_.size() >= maxSize_) {
            entries_.erase(entries_.begin());
        }
        entries_.push_back(line);
    }
    return true;
}

} // namespace core
} // namespace inputxx
