#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace graph::storage {
// Read-only decoded metadata. Cache hits share immutable storage; cache misses
// retain the decoded page inline without requiring another control allocation.
// Copying is disabled so an uncached view cannot silently clone its payload.
template<class Page> class DecodedMetadataPageView final {
public:
    explicit DecodedMetadataPageView(std::shared_ptr<const Page> page) : shared_(std::move(page)) {
        if (!shared_) throw std::invalid_argument("decoded metadata view requires a live page");
    }
    explicit DecodedMetadataPageView(Page page) : owned_(std::move(page)) {}
    DecodedMetadataPageView(const DecodedMetadataPageView &) = delete;
    DecodedMetadataPageView &operator=(const DecodedMetadataPageView &) = delete;
    DecodedMetadataPageView(DecodedMetadataPageView &&) = default;
    DecodedMetadataPageView &operator=(DecodedMetadataPageView &&) = default;
    [[nodiscard]] const Page *get() const noexcept { return shared_ ? shared_.get() : owned_ ? &*owned_ : nullptr; }
    [[nodiscard]] const Page *operator->() const noexcept { return get(); }
    [[nodiscard]] const Page &operator*() const noexcept { return *get(); }
private:
    std::shared_ptr<const Page> shared_;
    std::optional<Page> owned_;
};
} // namespace graph::storage
