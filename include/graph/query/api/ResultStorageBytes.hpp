#pragma once

#include <type_traits>
#include <vector>

#include "graph/query/api/ResultValue.hpp"
#include "graph/core/PropertyHeapRequestBytes.hpp"
#include "graph/query/execution/RetainedPropertyBytes.hpp"

namespace graph::query::detail {
// Observed container/string capacities plus the existing conservative node
// metadata estimate. These are retained ownership bytes, not RSS measurements.
inline size_t resultStringsCopyHeapUpper(const std::vector<std::string> &strings) {
    using namespace utils::detail;
    if (strings.size() > (std::numeric_limits<size_t>::max)()/sizeof(std::string))
        throw std::overflow_error("result string backing overflowed");
    size_t bytes = addStandardHeapBytes(standardContainerProxyBytes(),
        standardAllocatorRequestBytes(strings.size()*sizeof(std::string), alignof(std::string)));
    for (const auto &text : strings) bytes = addStandardHeapBytes(bytes, standardFreshStringHeapUpper(text.size()));
    return bytes;
}
inline size_t resultStringsHeapBytes(const std::vector<std::string> &strings) {
	using namespace execution::detail;
	size_t bytes = standardVectorHeapBufferBytes(strings);
	for (const auto &text : strings) bytes = addPropertyBufferBytes(bytes, dynamicStringBufferBytes(text));
	return bytes;
}
inline size_t resultValueHeapBytes(const ResultValue &value) {
	using namespace execution::detail;
	if (value.isPrimitive()) return propertyHeapBufferBytes(value.asPrimitive());
	if (value.isLogicalNode()) {
		const auto &node = value.asLogicalNode();
		return addPropertyBufferBytes(resultStringsHeapBytes(node.labels), propertyMapHeapBufferBytes(node.properties));
	}
	if (value.isLogicalEdge()) {
		const auto &edge = value.asLogicalEdge();
		return addPropertyBufferBytes(dynamicStringBufferBytes(edge.type), propertyMapHeapBufferBytes(edge.properties));
	}
	return value.isNode() ? propertyMapHeapBufferBytes(value.asNode().getProperties())
		: propertyMapHeapBufferBytes(value.asEdge().getProperties());
}
template<class Map>
size_t resultPropertyMapCopyHeapUpper(const Map &map) {
    using namespace utils::detail;
    size_t bytes = standardUnorderedMapStructuralHeapBytes(map);
    for (const auto &[key, value] : map) {
        bytes = addStandardHeapBytes(bytes, standardFreshStringHeapUpper(key.size()));
        bytes = addStandardHeapBytes(bytes, property_heap::copyRequestUpper(value));
    }
    return bytes;
}
inline size_t resultValueCopyHeapUpper(const ResultValue &value) {
    using namespace utils::detail;
    if (value.isPrimitive()) return property_heap::copyRequestUpper(value.asPrimitive());
    if (value.isLogicalNode()) {
        const auto &node = value.asLogicalNode();
        return addStandardHeapBytes(resultStringsCopyHeapUpper(node.labels), resultPropertyMapCopyHeapUpper(node.properties));
    }
    if (value.isLogicalEdge()) {
        const auto &edge = value.asLogicalEdge();
        return addStandardHeapBytes(standardFreshStringHeapUpper(edge.type.size()), resultPropertyMapCopyHeapUpper(edge.properties));
    }
    return value.isNode() ? resultPropertyMapCopyHeapUpper(value.asNode().getProperties())
        : resultPropertyMapCopyHeapUpper(value.asEdge().getProperties());
}
template<class Row>
size_t resultRowCopyHeapUpper(const Row &row) {
    using namespace utils::detail;
    size_t bytes = standardUnorderedMapStructuralHeapBytes(row);
    for (const auto &[key, value] : row) {
        bytes = addStandardHeapBytes(bytes, standardFreshStringHeapUpper(key.size()));
        bytes = addStandardHeapBytes(bytes, resultValueCopyHeapUpper(value));
    }
    return bytes;
}
// Reallocating a row vector can deep-copy hash maps when their move
// constructor may throw. Admit the entire simultaneous copy before reserve.
template<class Row>
size_t resultRowsRelocationHeapUpper(const std::vector<Row> &rows) {
    size_t bytes = 0U;
    if constexpr (!std::is_nothrow_move_constructible_v<Row>) {
        for (const auto &row : rows)
            bytes = utils::detail::addStandardHeapBytes(bytes, resultRowCopyHeapUpper(row));
    } else {
#if defined(_MSVC_STL_VERSION)
        bytes = execution::detail::multiplyPropertyBufferBytes(rows.size(),
            utils::detail::standardFreshUnorderedMapHeapUpper<std::string, ResultValue>(0U));
#endif
    }
    return bytes;
}
template<typename Row>
size_t resultRowHeapBytes(const Row &row) {
	using namespace execution::detail;
    size_t bytes = standardUnorderedMapStructuralHeapBytes(row);
	for (const auto &[name, value] : row) {
		bytes = addPropertyBufferBytes(bytes, dynamicStringBufferBytes(name));
		bytes = addPropertyBufferBytes(bytes, resultValueHeapBytes(value));
	}
	return bytes;
}
} // namespace graph::query::detail
