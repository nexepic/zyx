#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/TypedScalarValue.hpp"
#include "graph/query/execution/TypedValueKey.hpp"

namespace graph::query::execution {

	struct TypedGroupCount {
		PropertyValue value;
		int64_t count = 0;
	};

	template<typename Key, typename Hash = std::hash<Key>, typename Equal = std::equal_to<Key>>
	class CompactGroupMap {
	public:
		void add(Key value, int64_t count) {
			if (count <= 0) {
				return;
			}
			if (promoted_) {
				large_[std::move(value)] += count;
				return;
			}
			for (auto &[existing, existingCount] : small_) {
				if (equal_(existing, value)) {
					existingCount += count;
					return;
				}
			}
			if (small_.size() < kInlineLimit) {
				small_.emplace_back(std::move(value), count);
				return;
			}
			promote();
			large_[std::move(value)] += count;
		}

		[[nodiscard]] size_t size() const {
			return promoted_ ? large_.size() : small_.size();
		}

		template<typename Visitor>
		void forEach(Visitor &&visitor) const {
			if (promoted_) {
				for (const auto &[value, count] : large_) {
					visitor(value, count);
				}
				return;
			}
			for (const auto &[value, count] : small_) {
				visitor(value, count);
			}
		}

		void clear() {
			small_.clear();
			large_.clear();
			promoted_ = false;
		}

	private:
		static constexpr size_t kInlineLimit = 16;

		void promote() {
			large_.reserve(kInlineLimit * 2);
			for (auto &[value, count] : small_) {
				large_.emplace(std::move(value), count);
			}
			small_.clear();
			promoted_ = true;
		}

		bool promoted_ = false;
		Equal equal_{};
		std::vector<std::pair<Key, int64_t>> small_;
		std::unordered_map<Key, int64_t, Hash, Equal> large_;
	};

	class StringCompactGroupMap {
	public:
		void add(std::string_view value, int64_t count) {
			if (count <= 0) {
				return;
			}
			if (promoted_) {
				large_[std::string(value)] += count;
				return;
			}
			for (auto &[existing, existingCount] : small_) {
				if (std::string_view(existing.data(), existing.size()) == value) {
					existingCount += count;
					return;
				}
			}
			if (small_.size() < kInlineLimit) {
				small_.emplace_back(std::string(value), count);
				return;
			}
			promote();
			large_[std::string(value)] += count;
		}

		[[nodiscard]] size_t size() const {
			return promoted_ ? large_.size() : small_.size();
		}

		template<typename Visitor>
		void forEach(Visitor &&visitor) const {
			if (promoted_) {
				for (const auto &[value, count] : large_) {
					visitor(value, count);
				}
				return;
			}
			for (const auto &[value, count] : small_) {
				visitor(value, count);
			}
		}

		void clear() {
			small_.clear();
			large_.clear();
			promoted_ = false;
		}

	private:
		static constexpr size_t kInlineLimit = 16;

		void promote() {
			large_.reserve(kInlineLimit * 2);
			for (auto &[value, count] : small_) {
				large_.emplace(std::move(value), count);
			}
			small_.clear();
			promoted_ = true;
		}

		bool promoted_ = false;
		std::vector<std::pair<std::string, int64_t>> small_;
		std::unordered_map<std::string, int64_t> large_;
	};

	class TypedGroupCounter {
	public:
		void addNull(int64_t count = 1) {
			if (count > 0) {
				nullCount_ += count;
			}
		}

		void addBoolean(bool value, int64_t count = 1) {
			if (count > 0) {
				boolCounts_[value ? 1U : 0U] += count;
			}
		}

		void addInteger(int64_t value, int64_t count = 1) {
			if (count > 0) {
				integerCounts_.add(value, count);
			}
		}

		void addDouble(double value, int64_t count = 1) {
			if (count > 0) {
				doubleCounts_.add(value, count);
			}
		}

		void addString(std::string_view value, int64_t count = 1) {
			if (count > 0) {
				stringCounts_.add(value, count);
			}
		}

		void addDateEpochDays(int32_t epochDays, int64_t count = 1) {
			if (count > 0) {
				dateCounts_.add(epochDays, count);
			}
		}

		void addDateTimeEpochMillis(int64_t epochMillis, int64_t count = 1) {
			if (count > 0) {
				dateTimeCounts_.add(epochMillis, count);
			}
		}

		void addScalar(const TypedScalarValue &value, int64_t count = 1) {
			if (count <= 0) {
				return;
			}

			switch (value.type) {
				case PropertyType::NULL_TYPE:
					addNull(count);
					break;
				case PropertyType::BOOLEAN:
					addBoolean(value.boolValue, count);
					break;
				case PropertyType::INTEGER:
					addInteger(value.intValue, count);
					break;
				case PropertyType::DOUBLE:
					addDouble(value.doubleValue, count);
					break;
				case PropertyType::STRING:
					addString(value.stringValue, count);
					break;
				case PropertyType::DATE:
					addDateEpochDays(static_cast<int32_t>(value.intValue), count);
					break;
				case PropertyType::DATETIME:
					addDateTimeEpochMillis(value.intValue, count);
					break;
				case PropertyType::DURATION:
					add(PropertyValue(value.durationValue), count);
					break;
				case PropertyType::LIST:
				case PropertyType::MAP:
				case PropertyType::COMPOSITE:
				case PropertyType::UNKNOWN:
				default:
					add(value.fallbackValue != nullptr ? *value.fallbackValue : PropertyValue(), count);
					break;
			}
		}

		void add(const PropertyValue &value, int64_t count = 1) {
			if (count <= 0) {
				return;
			}

			switch (value.getType()) {
				case PropertyType::NULL_TYPE:
					addNull(count);
					break;
				case PropertyType::BOOLEAN:
					addBoolean(std::get<bool>(value.getVariant()), count);
					break;
				case PropertyType::INTEGER:
					addInteger(std::get<int64_t>(value.getVariant()), count);
					break;
				case PropertyType::DOUBLE:
					addDouble(std::get<double>(value.getVariant()), count);
					break;
				case PropertyType::STRING:
					addString(std::get<std::string>(value.getVariant()), count);
					break;
				case PropertyType::DATE:
					addDateEpochDays(std::get<TemporalDate>(value.getVariant()).epochDays, count);
					break;
				case PropertyType::DATETIME:
					addDateTimeEpochMillis(std::get<TemporalDateTime>(value.getVariant()).epochMillis, count);
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

		void mergeFrom(const TypedGroupCounter &other) {
			addNull(other.nullCount_);
			addBoolean(false, other.boolCounts_[0]);
			addBoolean(true, other.boolCounts_[1]);
			other.integerCounts_.forEach([&](const auto &value, int64_t count) {
				addInteger(value, count);
			});
			other.doubleCounts_.forEach([&](const auto &value, int64_t count) {
				addDouble(value, count);
			});
			other.stringCounts_.forEach([&](const auto &value, int64_t count) {
				addString(value, count);
			});
			other.dateCounts_.forEach([&](const auto &value, int64_t count) {
				addDateEpochDays(value, count);
			});
			other.dateTimeCounts_.forEach([&](const auto &value, int64_t count) {
				addDateTimeEpochMillis(value, count);
			});
			for (const auto &[_, group] : other.fallbackCounts_) {
				add(group.value, group.count);
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
			integerCounts_.forEach([&](const auto &value, int64_t count) {
				groups.push_back({PropertyValue(value), count});
			});
			doubleCounts_.forEach([&](const auto &value, int64_t count) {
				groups.push_back({PropertyValue(value), count});
			});
			stringCounts_.forEach([&](const auto &value, int64_t count) {
				groups.push_back({PropertyValue(value), count});
			});
			dateCounts_.forEach([&](const auto &value, int64_t count) {
				groups.push_back({PropertyValue(TemporalDate{value}), count});
			});
			dateTimeCounts_.forEach([&](const auto &value, int64_t count) {
				groups.push_back({PropertyValue(TemporalDateTime{value}), count});
			});
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
		CompactGroupMap<int64_t> integerCounts_;
		CompactGroupMap<double> doubleCounts_;
		StringCompactGroupMap stringCounts_;
		CompactGroupMap<int32_t> dateCounts_;
		CompactGroupMap<int64_t> dateTimeCounts_;
		std::unordered_map<TypedEqualityKey, TypedGroupCount, GroupHash> fallbackCounts_;
	};

} // namespace graph::query::execution
