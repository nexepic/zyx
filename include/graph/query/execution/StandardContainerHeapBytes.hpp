#pragma once
#include "graph/utils/StandardContainerHeapBytes.hpp"
namespace graph::query::execution::detail {
using utils::detail::standardContainerProxyBytes;
using utils::detail::addStandardHeapBytes;
using utils::detail::standardAllocatorRequestBytes;
using utils::detail::standardVectorHeapBufferBytes;
using utils::detail::standardUnorderedMapStructuralHeapBytes;
} // namespace graph::query::execution::detail
