#pragma once

#include <functional>
#include <map>
#include <set>
#include "graph/utils/OwnedPmrAllocator.hpp"

namespace graph::utils {
// Internal work tables capture the active allocation scope and retain its owner.
// Keys/values with independent buffers must use the same owner themselves.
template<class Key, class Value, class Compare = std::less<Key>>
using OwnedMap = std::map<Key, Value, Compare, OwnedPmrAllocator<std::pair<const Key, Value>>>;
template<class Key, class Compare = std::less<Key>>
using OwnedSet = std::set<Key, Compare, OwnedPmrAllocator<Key>>;
} // namespace graph::utils
