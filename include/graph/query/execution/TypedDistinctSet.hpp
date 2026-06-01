#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <unordered_set>

#include "graph/core/PropertyTypes.hpp"
#include "graph/core/TemporalTypes.hpp"

namespace graph::query::execution {

	class TypedDistinctSet {
	public:
		bool insert(const PropertyValue &value) {
			switch (value.getType()) {
				case PropertyType::NULL_TYPE:
					return markNull();
				case PropertyType::BOOLEAN:
					return markBool(std::get<bool>(value.getVariant()));
				case PropertyType::INTEGER:
					return integers_.insert(std::get<int64_t>(value.getVariant())).second;
				case PropertyType::DOUBLE:
					return doubles_.insert(std::get<double>(value.getVariant())).second;
				case PropertyType::STRING:
					return strings_.insert(std::get<std::string>(value.getVariant())).second;
				case PropertyType::DATE:
					return dates_.insert(std::get<TemporalDate>(value.getVariant()).epochDays).second;
				case PropertyType::DATETIME:
					return dateTimes_.insert(std::get<TemporalDateTime>(value.getVariant()).epochMillis).second;
				case PropertyType::DURATION:
				case PropertyType::LIST:
				case PropertyType::MAP:
				case PropertyType::COMPOSITE:
				case PropertyType::UNKNOWN:
					return fallback_.insert(value).second;
			}
			return fallback_.insert(value).second;
		}

		[[nodiscard]] size_t size() const {
			return (hasNull_ ? size_t{1} : size_t{0}) + (seenBools_[0] ? size_t{1} : size_t{0}) +
			       (seenBools_[1] ? size_t{1} : size_t{0}) + integers_.size() + doubles_.size() +
			       strings_.size() + dates_.size() + dateTimes_.size() + fallback_.size();
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
