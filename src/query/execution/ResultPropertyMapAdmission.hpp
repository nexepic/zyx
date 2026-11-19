#pragma once

#include <functional>
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/utils/StandardContainerHeapBytes.hpp"

namespace graph::query::execution::detail {
// The public result map has a different layout from a private binding tree.
// Match fresh map reserve(count), key copies and guarded PropertyValue copies;
// this is an allocation-request upper bound, not an RSS or source-wire size.
template<class Entries>
size_t freshResultEntryMapHeapUpper(const Entries &entries, const std::function<void()> &checkpoint) {
    size_t bytes = utils::detail::standardFreshUnorderedMapHeapUpper<std::string, PropertyValue>(entries.size());
    for (const auto &entry : entries) {
        checkpoint();
        bytes = utils::detail::addStandardHeapBytes(bytes,
            utils::detail::standardFreshStringHeapUpper(entry.key.size()));
        bytes = utils::detail::addStandardHeapBytes(bytes,
            storage::CanonicalPropertyValueCodec::guardedCopyHeapUpper(entry.value, checkpoint));
    }
    return bytes;
}
} // namespace graph::query::execution::detail
