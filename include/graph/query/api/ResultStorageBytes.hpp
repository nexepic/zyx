#pragma once

#include <type_traits>
#include <utility>
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
// Preserve map payloads even on STLs whose map move constructor can throw.
// Complete every allocating step before swapping any original row, so a
// failed destination construction leaves the complete source untouched.
template<class Row>
void reserveResultRowsBySwap(std::vector<Row> &rows, size_t capacity) {
    static_assert(std::is_nothrow_swappable_v<Row>);
    if (capacity <= rows.capacity()) return;
    if (rows.empty()) { rows.reserve(capacity); return; }
    std::vector<Row> replacement;
    replacement.reserve(capacity);
    replacement.resize(rows.size());
    for (size_t index = 0U; index < rows.size(); ++index) {
        using std::swap;
        swap(replacement[index], rows[index]);
    }
    rows.swap(replacement);
}

// The swap relocation above retains both arrays, a temporary vector proxy,
// and every destination empty map until the old array is destroyed. Its
// payloads are transferred, never copied or discounted while still alive.
template<class Row>
size_t resultRowsRelocationHeapUpper([[maybe_unused]] const std::vector<Row> &rows) {
    if (rows.empty()) return 0U;
    size_t bytes = utils::detail::standardContainerProxyBytes();
#if defined(_MSVC_STL_VERSION)
    bytes = utils::detail::addStandardHeapBytes(bytes,
        execution::detail::multiplyPropertyBufferBytes(rows.size(),
            utils::detail::standardFreshUnorderedMapHeapUpper<std::string, ResultValue>(0U)));
#endif
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
