#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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
				case PropertyType::COMPOSITE:
				case PropertyType::UNKNOWN:
					key.fallbackValue_ = value;
					break;
			}
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

} // namespace graph::query::execution
