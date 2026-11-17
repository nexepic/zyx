#pragma once
#include <span>
#include <string_view>
#include "query/execution/CellScratchVector.hpp"

namespace graph::query::execution::detail {
// A single selected key is already owned by the caller's key-view array.
// Borrow it and use its implicit ordinal instead of retaining duplicate scratch.
class CellKeySelection final {
    struct Multiple {
        CellScratchVector<size_t> columns;
        CellScratchVector<std::string_view> keys;
        explicit Multiple(size_t count) { columns.reserve(count); keys.reserve(count); }
    };
public:
    static size_t workingSetUpper(size_t count) {
        return count <= 1U ? 0U : utils::detail::addStandardHeapBytes(
            CellScratchVector<size_t>::workingSetUpper(count),
            CellScratchVector<std::string_view>::workingSetUpper(count));
    }
    explicit CellKeySelection(std::span<const std::string_view> keys) : source_(keys) {
        if (keys.size() > 1U) multiple_.emplace(keys.size());
    }
    void clear() noexcept {
        single_selected_ = false;
        if (multiple_) { multiple_->columns.clear(); multiple_->keys.clear(); }
    }
    void append(size_t column) {
        if (column >= source_.size()) throw std::out_of_range("Property cell selection column is invalid");
        if (multiple_) {
            if (multiple_->columns.size() >= source_.size())
                throw std::length_error("Property cell selection exceeds admitted storage");
            multiple_->columns.push_back(column);
            multiple_->keys.push_back(source_[column]);
        } else {
            if (single_selected_) throw std::length_error("Property cell single selection is duplicated");
            single_selected_ = true;
        }
    }
    size_t size() const noexcept { return multiple_ ? multiple_->columns.size() : size_t(single_selected_); }
    bool empty() const noexcept { return size() == 0U; }
    size_t column(size_t selected) const {
        if (selected >= size()) throw std::out_of_range("Property cell selection ordinal is invalid");
        return multiple_ ? multiple_->columns[selected] : 0U;
    }
    std::span<const std::string_view> keys() const noexcept {
        return multiple_ ? std::span<const std::string_view>(multiple_->keys.data(), multiple_->keys.size())
                         : source_.first(size());
    }
    size_t heapBytes() const {
        return multiple_ ? utils::detail::addStandardHeapBytes(multiple_->columns.heapBytes(), multiple_->keys.heapBytes()) : 0U;
    }
private:
    std::span<const std::string_view> source_;
    std::optional<Multiple> multiple_;
    bool single_selected_ = false;
};
}
