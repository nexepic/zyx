#pragma once
#include "graph/core/OwnedPropertyVector.hpp"
namespace graph::query::logical {
struct PropertyPredicateCopyPolicy {
    static size_t copyBytes(const std::pair<std::string, graph::PropertyValue> &value) {
        return utils::detail::addStandardHeapBytes(utils::detail::standardFreshStringHeapUpper(value.first.size()),
            graph::property_heap::copyRequestUpper(value.second));
    }
};
using OwnedPropertyPredicates = utils::OwnedCopyVector<std::pair<std::string, graph::PropertyValue>, PropertyPredicateCopyPolicy>;
}
