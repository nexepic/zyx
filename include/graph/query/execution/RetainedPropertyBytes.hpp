#pragma once

#include <algorithm>
#include <limits>
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/StringBufferBytes.hpp"

namespace graph::query::execution::detail {
inline size_t addPropertyBufferBytes(size_t left, size_t right) {
	if (right > std::numeric_limits<size_t>::max() - left)
		throw std::overflow_error("retained Property buffer size overflowed");
	return left + right;
}
inline size_t multiplyPropertyBufferBytes(size_t count, size_t width) {
	if (width && count > std::numeric_limits<size_t>::max() / width)
		throw std::overflow_error("retained Property buffer size overflowed");
	return count * width;
}

inline size_t propertyHeapBufferBytes(const PropertyValue &value);

inline size_t propertyMapHeapBufferBytes(const PropertyValue::MapType &map) {
	size_t total = multiplyPropertyBufferBytes(map.bucket_count(), sizeof(void *));
	// Node allocator metadata is estimated; buckets and payload capacities are observed.
	constexpr size_t nodeBytes = sizeof(PropertyValue::MapType::value_type) + 2U * sizeof(void *) +
		alignof(PropertyValue::MapType::value_type) - 1U;
	total = addPropertyBufferBytes(total, multiplyPropertyBufferBytes(map.size(), nodeBytes));
	for (const auto &[key, element] : map) {
		total = addPropertyBufferBytes(total, dynamicStringBufferBytes(key));
		total = addPropertyBufferBytes(total, propertyHeapBufferBytes(element));
	}
	return total;
}

// Heap payload only; the owning PropertyValue slot is charged by its container.
inline size_t propertyHeapBufferBytes(const PropertyValue &value) {
	const auto &variant = value.getVariant();
	if (const auto *text = std::get_if<std::string>(&variant)) return dynamicStringBufferBytes(*text);
	if (const auto *bytes = std::get_if<PropertyValue::Bytes>(&variant)) return bytes->capacity();
	if (const auto *list = std::get_if<std::vector<PropertyValue>>(&variant)) {
		size_t total = multiplyPropertyBufferBytes(list->capacity(), sizeof(PropertyValue));
		for (const auto &element : *list) total = addPropertyBufferBytes(total, propertyHeapBufferBytes(element));
		return total;
	}
	if (const auto *map = std::get_if<PropertyValue::MapType>(&variant))
		return propertyMapHeapBufferBytes(*map);
	return 0U;
}
// Compute both estimates in one traversal so the logical-size fallback does
// not introduce a second uninterruptible walk over a large nested value.
struct PropertyBufferEstimates {
	size_t logicalBytes;
	size_t heapBytes;
};
template<typename Checkpoint>
inline PropertyBufferEstimates propertyBufferEstimatesGuarded(
	const PropertyValue &value, Checkpoint &checkpoint, size_t &untilCheckpoint) {
	if (--untilCheckpoint == 0U) {
		checkpoint();
		untilCheckpoint = 256U;
	}
	const auto &variant = value.getVariant();
	if (const auto *list = std::get_if<std::vector<PropertyValue>>(&variant)) {
		PropertyBufferEstimates bytes{sizeof(uint32_t), multiplyPropertyBufferBytes(list->capacity(), sizeof(PropertyValue))};
		for (const auto &element : *list) {
			const auto child = propertyBufferEstimatesGuarded(element, checkpoint, untilCheckpoint);
			bytes.logicalBytes = addPropertyBufferBytes(bytes.logicalBytes, child.logicalBytes);
			bytes.heapBytes = addPropertyBufferBytes(bytes.heapBytes, child.heapBytes);
		}
		return bytes;
	}
	if (const auto *map = std::get_if<PropertyValue::MapType>(&variant)) {
		constexpr size_t nodeBytes = sizeof(PropertyValue::MapType::value_type) + 2U * sizeof(void *) +
			alignof(PropertyValue::MapType::value_type) - 1U;
		PropertyBufferEstimates bytes{sizeof(uint32_t), addPropertyBufferBytes(
			multiplyPropertyBufferBytes(map->bucket_count(), sizeof(void *)),
			multiplyPropertyBufferBytes(map->size(), nodeBytes))};
		for (const auto &[key, element] : *map) {
			const auto child = propertyBufferEstimatesGuarded(element, checkpoint, untilCheckpoint);
			bytes.logicalBytes = addPropertyBufferBytes(bytes.logicalBytes,
				addPropertyBufferBytes(addPropertyBufferBytes(key.size(), sizeof(size_t)), child.logicalBytes));
			bytes.heapBytes = addPropertyBufferBytes(bytes.heapBytes,
				addPropertyBufferBytes(dynamicStringBufferBytes(key), child.heapBytes));
		}
		return bytes;
	}
	return {property_utils::getPropertyValueSize(value), propertyHeapBufferBytes(value)};
}
template<typename Checkpoint>
inline PropertyBufferEstimates propertyBufferEstimates(const PropertyValue &value, Checkpoint checkpoint) {
	checkpoint();
	size_t untilCheckpoint = 256U;
	const auto bytes = propertyBufferEstimatesGuarded(value, checkpoint, untilCheckpoint);
	checkpoint();
	return bytes;
}
template<typename Checkpoint>
inline size_t retainedPropertyValueBytes(const PropertyValue &value, Checkpoint checkpoint) {
	const auto bytes = propertyBufferEstimates(value, checkpoint);
	return std::max(bytes.logicalBytes, bytes.heapBytes);
}
// Includes the owning map object. Preserve the legacy per-value lower bound
// while computing observed container capacity in the same guarded traversal.
template<typename Checkpoint>
inline size_t retainedPropertyMapBytes(const PropertyValue::MapType &map, Checkpoint checkpoint) {
	checkpoint();
	size_t untilCheckpoint = 256U;
	size_t logicalBytes = sizeof(map);
	constexpr size_t nodeBytes = sizeof(PropertyValue::MapType::value_type) + 2U * sizeof(void *) +
		alignof(PropertyValue::MapType::value_type) - 1U;
	size_t heapBytes = addPropertyBufferBytes(sizeof(map), addPropertyBufferBytes(
		multiplyPropertyBufferBytes(map.bucket_count(), sizeof(void *)),
		multiplyPropertyBufferBytes(map.size(), nodeBytes)));
	for (const auto &[key, value] : map) {
		const auto child = propertyBufferEstimatesGuarded(value, checkpoint, untilCheckpoint);
		logicalBytes = addPropertyBufferBytes(logicalBytes, addPropertyBufferBytes(
			addPropertyBufferBytes(key.size(), sizeof(PropertyValue)), std::max(child.logicalBytes, child.heapBytes)));
		heapBytes = addPropertyBufferBytes(heapBytes,
			addPropertyBufferBytes(dynamicStringBufferBytes(key), child.heapBytes));
	}
	checkpoint();
	return std::max(logicalBytes, heapBytes);
}
inline size_t retainedPropertyValueBytes(const PropertyValue &value) {
	return std::max(property_utils::getPropertyValueSize(value), propertyHeapBufferBytes(value));
}
} // namespace graph::query::execution::detail
