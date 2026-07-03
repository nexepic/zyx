#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "graph/core/PropertyTypes.hpp"

namespace graph::query::execution {

	class TypedOrderKey {
	public:
		TypedOrderKey() = default;

		static TypedOrderKey from(const PropertyValue &value) {
			TypedOrderKey key;
			key.variantIndex_ = value.getVariant().index();
			switch (value.getType()) {
				case PropertyType::BOOLEAN:
					key.boolValue_ = std::get<bool>(value.getVariant());
					break;
				case PropertyType::INTEGER:
					key.intValue_ = std::get<int64_t>(value.getVariant());
					break;
				case PropertyType::DOUBLE:
					key.doubleValue_ = std::get<double>(value.getVariant());
					break;
				case PropertyType::STRING:
					key.stringValue_ = std::get<std::string>(value.getVariant());
					break;
				case PropertyType::DATE:
					key.intValue_ = std::get<TemporalDate>(value.getVariant()).epochDays;
					break;
				case PropertyType::DATETIME:
					key.intValue_ = std::get<TemporalDateTime>(value.getVariant()).epochMillis;
					break;
				case PropertyType::DURATION:
					key.durationValue_ = std::get<TemporalDuration>(value.getVariant());
					break;
				case PropertyType::NULL_TYPE:
				case PropertyType::LIST:
				case PropertyType::MAP:
				case PropertyType::COMPOSITE: // ZYX_COV_EXCL_LINE: PropertyValue has no public composite-value constructor.
				case PropertyType::UNKNOWN: // ZYX_COV_EXCL_LINE: UNKNOWN is a defensive fallback for future value variants.
					key.fallbackValue_ = value;
					break;
			}
			return key;
		}

		static TypedOrderKey fromNull() { return TypedOrderKey{}; }

		static TypedOrderKey fromBoolean(bool value) {
			TypedOrderKey key;
			key.variantIndex_ = 1;
			key.boolValue_ = value;
			return key;
		}

		static TypedOrderKey fromInteger(int64_t value) {
			TypedOrderKey key;
			key.variantIndex_ = 2;
			key.intValue_ = value;
			return key;
		}

		static TypedOrderKey fromDouble(double value) {
			TypedOrderKey key;
			key.variantIndex_ = 3;
			key.doubleValue_ = value;
			return key;
		}

		static TypedOrderKey fromString(std::string_view value) {
			TypedOrderKey key;
			key.variantIndex_ = 4;
			key.stringValue_.assign(value.data(), value.size());
			return key;
		}

		static TypedOrderKey fromDateEpochDays(int64_t epochDays) {
			TypedOrderKey key;
			key.variantIndex_ = 7;
			key.intValue_ = epochDays;
			return key;
		}

		static TypedOrderKey fromDateTimeEpochMillis(int64_t epochMillis) {
			TypedOrderKey key;
			key.variantIndex_ = 8;
			key.intValue_ = epochMillis;
			return key;
		}

		static TypedOrderKey fromDuration(const TemporalDuration &value) {
			TypedOrderKey key;
			key.variantIndex_ = 9;
			key.durationValue_ = value;
			return key;
		}

		[[nodiscard]] int compare(const TypedOrderKey &other) const {
			if (variantIndex_ != other.variantIndex_) {
				return variantIndex_ < other.variantIndex_ ? -1 : 1;
			}
			switch (variantIndex_) {
				case 1:
					return compareScalars(boolValue_, other.boolValue_);
				case 2:
					return compareScalars(intValue_, other.intValue_);
				case 3:
					return compareScalars(doubleValue_, other.doubleValue_);
				case 4:
					return compareScalars(stringValue_, other.stringValue_);
				case 7:
					return compareScalars(intValue_, other.intValue_);
				case 8:
					return compareScalars(intValue_, other.intValue_);
				case 9:
					return durationValue_ < other.durationValue_ ? -1 : (other.durationValue_ < durationValue_ ? 1 : 0);
				default:
					if (variantIndex_ == 6) {
						return 0;
					}
					if (fallbackValue_ == other.fallbackValue_) {
						return 0;
					}
					return fallbackValue_ < other.fallbackValue_ ? -1 : 1;
			}
		}

	private:
		template<typename T>
		static int compareScalars(const T &left, const T &right) {
			if (left < right) {
				return -1;
			}
			if (right < left) {
				return 1;
			}
			return 0;
		}

		size_t variantIndex_ = 0;
		bool boolValue_ = false;
		int64_t intValue_ = 0;
		double doubleValue_ = 0.0;
		std::string stringValue_;
		TemporalDuration durationValue_;
		PropertyValue fallbackValue_;
	};

	class TypedEqualityKey {
	public:
		TypedEqualityKey() = default;

		static TypedEqualityKey from(const PropertyValue &value) {
			TypedEqualityKey key;
			key.variantIndex_ = value.getVariant().index();
			switch (value.getType()) {
				case PropertyType::NULL_TYPE:
					key.hash_ = combine(key.variantIndex_, size_t{0});
					break;
				case PropertyType::BOOLEAN:
					key.boolValue_ = std::get<bool>(value.getVariant());
					key.hash_ = combine(key.variantIndex_, std::hash<bool>{}(key.boolValue_));
					break;
				case PropertyType::INTEGER:
					key.intValue_ = std::get<int64_t>(value.getVariant());
					key.hash_ = combine(key.variantIndex_, std::hash<int64_t>{}(key.intValue_));
					break;
				case PropertyType::DOUBLE:
					key.doubleValue_ = std::get<double>(value.getVariant());
					key.hash_ = combine(key.variantIndex_, std::hash<double>{}(key.doubleValue_));
					break;
				case PropertyType::STRING:
					key.stringValue_ = std::get<std::string>(value.getVariant());
					key.hash_ = combine(key.variantIndex_, std::hash<std::string>{}(key.stringValue_));
					break;
				case PropertyType::DATE:
					key.intValue_ = std::get<TemporalDate>(value.getVariant()).epochDays;
					key.hash_ = combine(key.variantIndex_, std::hash<int64_t>{}(key.intValue_));
					break;
				case PropertyType::DATETIME:
					key.intValue_ = std::get<TemporalDateTime>(value.getVariant()).epochMillis;
					key.hash_ = combine(key.variantIndex_, std::hash<int64_t>{}(key.intValue_));
					break;
				case PropertyType::DURATION: {
					key.durationValue_ = std::get<TemporalDuration>(value.getVariant());
					size_t hash = std::hash<int64_t>{}(key.durationValue_.months);
					hash = combine(hash, std::hash<int64_t>{}(key.durationValue_.days));
					hash = combine(hash, std::hash<int64_t>{}(key.durationValue_.nanos));
					key.hash_ = combine(key.variantIndex_, hash);
					break;
				}
				case PropertyType::LIST:
				case PropertyType::MAP:
				case PropertyType::COMPOSITE: // ZYX_COV_EXCL_LINE: PropertyValue has no public composite-value constructor.
				case PropertyType::UNKNOWN: // ZYX_COV_EXCL_LINE: UNKNOWN is a defensive fallback for future value variants.
					key.fallbackValue_ = value;
					key.hash_ = PropertyValueHash{}(value);
					break;
			}
			return key;
		}

		[[nodiscard]] size_t hash() const { return hash_; }

		[[nodiscard]] bool operator==(const TypedEqualityKey &other) const {
			if (variantIndex_ != other.variantIndex_) {
				return false;
			}
			switch (variantIndex_) {
				case 0:
					return true;
				case 1:
					return boolValue_ == other.boolValue_;
				case 2:
				case 7:
				case 8:
					return intValue_ == other.intValue_;
				case 3:
					return doubleValue_ == other.doubleValue_;
				case 4:
					return stringValue_ == other.stringValue_;
				case 9:
					return durationValue_ == other.durationValue_;
				default:
					return fallbackValue_ == other.fallbackValue_;
			}
		}

	private:
		static size_t combine(size_t seed, size_t valueHash) {
			seed ^= valueHash + 0x9e3779b9 + (seed << 6) + (seed >> 2);
			return seed;
		}

		size_t variantIndex_ = 0;
		size_t hash_ = combine(size_t{0}, size_t{0});
		bool boolValue_ = false;
		int64_t intValue_ = 0;
		double doubleValue_ = 0.0;
		std::string stringValue_;
		TemporalDuration durationValue_;
		PropertyValue fallbackValue_;
	};

} // namespace graph::query::execution
