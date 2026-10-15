#pragma once

#include "graph/query/api/ResultValue.hpp"
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
