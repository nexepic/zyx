#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include "graph/core/PropertyTypes.hpp"

namespace graph::property_copy {
// Copy nested containers incrementally so cancellation can unwind partially
// constructed values without modifying their source. Large scalar buffers
// are copied in bounded chunks after reserving their final storage.
template<typename Checkpoint>
std::string copyPropertyStringGuarded(const std::string &source, Checkpoint &checkpoint) {
	checkpoint();
	std::string result;
	result.reserve(source.size());
	for (size_t offset = 0U; offset < source.size();) {
		checkpoint();
		const size_t count = std::min(size_t{64U * 1024U}, source.size() - offset);
		result.append(source.data() + offset, count);
		offset += count;
	}
	return result;
}

template<typename Checkpoint>
std::vector<std::string> copyPropertyStringsGuarded(const std::vector<std::string> &source, Checkpoint checkpoint) {
	checkpoint();
	std::vector<std::string> result;
	result.reserve(source.size());
	for (const auto &text : source)
		result.push_back(copyPropertyStringGuarded(text, checkpoint));
	checkpoint();
	return result;
}

template<typename Checkpoint>
PropertyValue copyPropertyValueImpl(const PropertyValue &value, Checkpoint &checkpoint, size_t &remaining, bool preserveMapLayout = false) {
	if (--remaining == 0U) {
		checkpoint();
		remaining = 256U;
	}
	if (value.getType() == PropertyType::LIST) {
		std::vector<PropertyValue> result;
		result.reserve(value.getList().size());
		for (const auto &element : value.getList())
			result.push_back(copyPropertyValueImpl(element, checkpoint, remaining, preserveMapLayout));
		return PropertyValue(std::move(result));
	}
	if (value.getType() == PropertyType::MAP) {
		PropertyValue::MapType result;
		if (preserveMapLayout) {
			result.max_load_factor(value.getMap().max_load_factor());
			if (!value.getMap().empty()) result.rehash(value.getMap().bucket_count());
		} else result.reserve(value.getMap().size());
		for (const auto &[key, element] : value.getMap())
			result.emplace(copyPropertyStringGuarded(key, checkpoint),
				copyPropertyValueImpl(element, checkpoint, remaining, preserveMapLayout));
		return PropertyValue(std::move(result));
	}
	if (const auto *text = std::get_if<std::string>(&value.getVariant()))
		return PropertyValue(copyPropertyStringGuarded(*text, checkpoint));
	if (value.getType() == PropertyType::BYTES) {
		const auto &source = value.getBytes();
		PropertyValue::Bytes result;
		result.reserve(source.size());
		for (size_t offset = 0U; offset < source.size();) {
			checkpoint();
			const size_t count = std::min(size_t{64U * 1024U}, source.size() - offset);
			result.insert(result.end(), source.begin() + offset, source.begin() + offset + count);
			offset += count;
		}
		return PropertyValue(std::move(result));
	}
	return value;
}

template<typename Checkpoint>
PropertyValue copyPropertyValueGuarded(const PropertyValue &value, Checkpoint checkpoint, bool preserveMapLayout = false) {
	checkpoint();
	size_t remaining = 256U;
	PropertyValue result = copyPropertyValueImpl(value, checkpoint, remaining, preserveMapLayout);
	checkpoint();
	return result;
}
template<typename Checkpoint>
PropertyValue::MapType copyPropertyMapGuarded(const PropertyValue::MapType &source, Checkpoint checkpoint) {
	checkpoint();
	size_t remaining = 256U;
	PropertyValue::MapType result;
	result.reserve(source.size());
	for (const auto &[key, value] : source)
		result.emplace(copyPropertyStringGuarded(key, checkpoint),
			copyPropertyValueImpl(value, checkpoint, remaining));
	checkpoint();
	return result;
}
} // namespace graph::query::execution::detail
