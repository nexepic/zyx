#pragma once
#include <stdexcept>

namespace graph::query::execution::detail {
// Swapping with a cold map destroys the old buckets before the caller can
// reuse their allocator credit. Never release storage containing live entries.
template<class Map> void releaseEmptyMapStorage(Map &map) {
    if (!map.empty()) throw std::logic_error("cannot release storage of a nonempty cache");
    Map replacement(0U, map.hash_function(), map.key_eq(), map.get_allocator());
    map.swap(replacement);
}
}
