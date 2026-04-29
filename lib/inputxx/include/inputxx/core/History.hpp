#pragma once

#include <vector>
#include <string>

namespace inputxx {
namespace core {

class History {
public:
    History(size_t maxSize = 100);

    void add(const std::string& entry);
    void setMaxSize(size_t maxSize);
    size_t getMaxSize() const;
    const std::vector<std::string>& getEntries() const;
    void clear();

    bool save(const std::string& filename) const;
    bool load(const std::string& filename);

private:
    std::vector<std::string> entries_;
    size_t maxSize_;
};

} // namespace core
} // namespace inputxx