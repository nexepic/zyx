#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/TypedValueKey.hpp"

namespace graph::query::execution {

	struct TypedGroupCount {
		PropertyValue value;
		int64_t count = 0;
	};

	class TypedGroupCounter {
	public:
		void add(const PropertyValue &value, int64_t count = 1) {
			if (count <= 0) {
				return;
			}

			switch (value.getType()) {
				case PropertyType::NULL_TYPE:
					nullCount_ += count;
					break;
				case PropertyType::BOOLEAN:
					boolCounts_[std::get<bool>(value.getVariant()) ? 1U : 0U] += count;
					break;
				case PropertyType::INTEGER:
					integerCounts_[std::get<int64_t>(value.getVariant())] += count;
					break;
				case PropertyType::DOUBLE:
					doubleCounts_[std::get<double>(value.getVariant())] += count;
					break;
				case PropertyType::STRING:
					stringCounts_[std::get<std::string>(value.getVariant())] += count;
					break;
				case PropertyType::DATE:
					dateCounts_[std::get<TemporalDate>(value.getVariant()).epochDays] += count;
					break;
				case PropertyType::DATETIME:
					dateTimeCounts_[std::get<TemporalDateTime>(value.getVariant()).epochMillis] += count;
					break;
				case PropertyType::DURATION:
				case PropertyType::LIST:
				case PropertyType::MAP:
				case PropertyType::COMPOSITE:
				case PropertyType::UNKNOWN:
					addFallback(value, count);
					break;
			}
		}

		[[nodiscard]] size_t size() const {
			return (nullCount_ > 0 ? size_t{1} : size_t{0}) +
				   (boolCounts_[0] > 0 ? size_t{1} : size_t{0}) +
				   (boolCounts_[1] > 0 ? size_t{1} : size_t{0}) +
				   integerCounts_.size() + doubleCounts_.size() + stringCounts_.size() +
				   dateCounts_.size() + dateTimeCounts_.size() + fallbackCounts_.size();
		}

		[[nodiscard]] std::vector<TypedGroupCount> toVector() const {
			std::vector<TypedGroupCount> groups;
			groups.reserve(size());
			if (nullCount_ > 0) {
				groups.push_back({PropertyValue(), nullCount_});
			}
			if (boolCounts_[0] > 0) {
				groups.push_back({PropertyValue(false), boolCounts_[0]});
			}
			if (boolCounts_[1] > 0) {
				groups.push_back({PropertyValue(true), boolCounts_[1]});
			}
			for (const auto &[value, count] : integerCounts_) {
				groups.push_back({PropertyValue(value), count});
			}
			for (const auto &[value, count] : doubleCounts_) {
				groups.push_back({PropertyValue(value), count});
			}
			for (const auto &[value, count] : stringCounts_) {
				groups.push_back({PropertyValue(value), count});
			}
			for (const auto &[value, count] : dateCounts_) {
				groups.push_back({PropertyValue(TemporalDate{value}), count});
			}
			for (const auto &[value, count] : dateTimeCounts_) {
				groups.push_back({PropertyValue(TemporalDateTime{value}), count});
			}
			for (const auto &[_, group] : fallbackCounts_) {
				groups.push_back(group);
			}
			return groups;
		}

		void clear() {
			nullCount_ = 0;
			boolCounts_ = {0, 0};
			integerCounts_.clear();
			doubleCounts_.clear();
			stringCounts_.clear();
			dateCounts_.clear();
			dateTimeCounts_.clear();
			fallbackCounts_.clear();
		}

	private:
		struct GroupHash {
			size_t operator()(const TypedEqualityKey &key) const { return key.hash(); }
		};

		void addFallback(const PropertyValue &value, int64_t count) {
			auto key = TypedEqualityKey::from(value);
			auto [it, inserted] = fallbackCounts_.emplace(std::move(key), TypedGroupCount{value, 0});
			it->second.count += count;
		}

		int64_t nullCount_ = 0;
		std::array<int64_t, 2> boolCounts_{0, 0};
		std::unordered_map<int64_t, int64_t> integerCounts_;
		std::unordered_map<double, int64_t> doubleCounts_;
		std::unordered_map<std::string, int64_t> stringCounts_;
		std::unordered_map<int32_t, int64_t> dateCounts_;
		std::unordered_map<int64_t, int64_t> dateTimeCounts_;
		std::unordered_map<TypedEqualityKey, TypedGroupCount, GroupHash> fallbackCounts_;
	};

} // namespace graph::query::execution
