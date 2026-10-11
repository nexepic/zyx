#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_set>

#include "graph/core/PropertyTypes.hpp"
#include "graph/core/TemporalTypes.hpp"
#include "graph/query/execution/TypedScalarValue.hpp"
#include "graph/query/execution/RetainedPropertyBytes.hpp"

namespace graph::query::execution {

	class TypedDistinctSet {
	public:
		bool insertNull() { return markNull(); }

		bool insertBoolean(bool value) { return markBool(value); }

		bool insertInteger(int64_t value) { return integers_.insert(value).second; }

		bool insertDouble(double value) { return doubles_.insert(value).second; }

		bool insertString(std::string_view value) { return strings_.insert(std::string(value)).second; }

		bool insertDateEpochDays(int32_t epochDays) { return dates_.insert(epochDays).second; }

		bool insertDateTimeEpochMillis(int64_t epochMillis) { return dateTimes_.insert(epochMillis).second; }

		bool insertDuration(const TemporalDuration &value) { return fallback_.insert(PropertyValue(value)).second; }

		bool insertScalar(const TypedScalarValue &value) {
			switch (value.type) {
				case PropertyType::NULL_TYPE:
					return insertNull();
				case PropertyType::BOOLEAN:
					return insertBoolean(value.boolValue);
				case PropertyType::INTEGER:
					return insertInteger(value.intValue);
				case PropertyType::DOUBLE:
					return insertDouble(value.doubleValue);
				case PropertyType::STRING:
					return insertString(value.stringValue);
				case PropertyType::DATE:
					return insertDateEpochDays(static_cast<int32_t>(value.intValue));
				case PropertyType::DATETIME:
					return insertDateTimeEpochMillis(value.intValue);
				case PropertyType::DURATION:
					return insertDuration(value.durationValue);
				case PropertyType::BYTES:
					return insert(value.fallbackValue != nullptr ? *value.fallbackValue : PropertyValue());
				case PropertyType::LIST:
				case PropertyType::MAP:
				case PropertyType::COMPOSITE:
				case PropertyType::UNKNOWN:
				default:
					return insert(value.fallbackValue != nullptr ? *value.fallbackValue : PropertyValue());
			}
		}

		bool insert(const PropertyValue &value) {
			switch (value.getType()) {
				case PropertyType::NULL_TYPE:
					return insertNull();
				case PropertyType::BOOLEAN:
					return insertBoolean(std::get<bool>(value.getVariant()));
				case PropertyType::INTEGER:
					return insertInteger(std::get<int64_t>(value.getVariant()));
				case PropertyType::DOUBLE:
					return insertDouble(std::get<double>(value.getVariant()));
				case PropertyType::STRING:
					return insertString(std::get<std::string>(value.getVariant()));
				case PropertyType::DATE:
					return insertDateEpochDays(std::get<TemporalDate>(value.getVariant()).epochDays);
				case PropertyType::DATETIME:
					return insertDateTimeEpochMillis(std::get<TemporalDateTime>(value.getVariant()).epochMillis);
				case PropertyType::DURATION:
				case PropertyType::BYTES:
				case PropertyType::LIST:
				case PropertyType::MAP:
				case PropertyType::COMPOSITE:
				case PropertyType::UNKNOWN:
					return fallback_.insert(value).second;
			}
			return fallback_.insert(value).second;
		}

		[[nodiscard]] bool contains(const PropertyValue &value) const {
			switch (value.getType()) {
				case PropertyType::NULL_TYPE: return hasNull_;
				case PropertyType::BOOLEAN: return seenBools_[std::get<bool>(value.getVariant()) ? 1U : 0U];
				case PropertyType::INTEGER: return integers_.contains(std::get<int64_t>(value.getVariant()));
				case PropertyType::DOUBLE: return doubles_.contains(std::get<double>(value.getVariant()));
				case PropertyType::STRING: return strings_.contains(std::get<std::string>(value.getVariant()));
				case PropertyType::DATE: return dates_.contains(std::get<TemporalDate>(value.getVariant()).epochDays);
				case PropertyType::DATETIME: return dateTimes_.contains(std::get<TemporalDateTime>(value.getVariant()).epochMillis);
				default: return fallback_.contains(value);
			}
		}

		// Transfer an already guarded copy of a fallback value without copying its payload again.
		bool insertFallbackOwned(PropertyValue value) { return fallback_.insert(std::move(value)).second; }

		[[nodiscard]] size_t size() const {
			return (hasNull_ ? size_t{1} : size_t{0}) + (seenBools_[0] ? size_t{1} : size_t{0}) +
			       (seenBools_[1] ? size_t{1} : size_t{0}) + integers_.size() + doubles_.size() +
			       strings_.size() + dates_.size() + dateTimes_.size() + fallback_.size();
		}

		// Payload accounting used by bounded aggregate execution. Container
		// allocator overhead is intentionally excluded; the execution layer also
		// charges its row/key structures, while this method accounts for values
		// retained by DISTINCT state.
		[[nodiscard]] size_t memoryBytes() const {
			size_t total = 0U;
			const auto add = [&total](size_t bytes) {
				if (bytes > std::numeric_limits<size_t>::max() - total) {
					throw std::overflow_error("typed distinct-set memory accounting overflowed");
				}
				total += bytes;
			};
			const auto multiply = [](size_t count, size_t width) {
				if (width != 0U && count > std::numeric_limits<size_t>::max() / width) {
					throw std::overflow_error("typed distinct-set memory accounting overflowed");
				}
				return count * width;
			};
			add((hasNull_ ? 1U : 0U) + (seenBools_[0] ? 1U : 0U) + (seenBools_[1] ? 1U : 0U));
			add(multiply(integers_.size(), sizeof(int64_t)));
			add(multiply(doubles_.size(), sizeof(double)));
			add(multiply(dates_.size(), sizeof(int32_t)));
			add(multiply(dateTimes_.size(), sizeof(int64_t)));
		for (const auto &value: strings_) {
			add(std::max(value.size(), detail::dynamicStringBufferBytes(value)));
		}
		for (const auto &value: fallback_) {
			add(detail::retainedPropertyValueBytes(value));
		}
		return total;
		}

		void clear() {
			hasNull_ = false;
			seenBools_ = {false, false};
			integers_.clear();
			doubles_.clear();
			strings_.clear();
			dates_.clear();
			dateTimes_.clear();
			fallback_.clear();
		}

		void mergeFrom(const TypedDistinctSet &other) {
			if (other.hasNull_) {
				insertNull();
			}
			if (other.seenBools_[0]) {
				insertBoolean(false);
			}
			if (other.seenBools_[1]) {
				insertBoolean(true);
			}
			for (const auto value : other.integers_) {
				insertInteger(value);
			}
			for (const auto value : other.doubles_) {
				insertDouble(value);
			}
			for (const auto &value : other.strings_) {
				insertString(value);
			}
			for (const auto value : other.dates_) {
				insertDateEpochDays(value);
			}
			for (const auto value : other.dateTimes_) {
				insertDateTimeEpochMillis(value);
			}
			for (const auto &value : other.fallback_) {
				insert(value);
			}
		}

	private:
		bool markNull() {
			if (hasNull_) {
				return false;
			}
			hasNull_ = true;
			return true;
		}

		bool markBool(bool value) {
			auto &slot = seenBools_[value ? 1U : 0U];
			if (slot) {
				return false;
			}
			slot = true;
			return true;
		}

		bool hasNull_ = false;
		std::array<bool, 2> seenBools_{false, false};
		std::unordered_set<int64_t> integers_;
		std::unordered_set<double> doubles_;
		std::unordered_set<std::string> strings_;
		std::unordered_set<int32_t> dates_;
		std::unordered_set<int64_t> dateTimes_;
		std::unordered_set<PropertyValue, PropertyValueHash> fallback_;
	};

} // namespace graph::query::execution
