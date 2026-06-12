#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/TypedValueKey.hpp"

namespace graph::query::execution {

	struct TypedScalarValue {
		PropertyType type = PropertyType::UNKNOWN;
		bool boolValue = false;
		int64_t intValue = 0;
		double doubleValue = 0.0;
		std::string_view stringValue;
		TemporalDuration durationValue;
		const PropertyValue *fallbackValue = nullptr;
	};

	inline PropertyValue propertyValueFromScalar(const TypedScalarValue &value) {
		switch (value.type) {
			case PropertyType::NULL_TYPE:
				return PropertyValue();
			case PropertyType::BOOLEAN:
				return PropertyValue(value.boolValue);
			case PropertyType::INTEGER:
				return PropertyValue(value.intValue);
			case PropertyType::DOUBLE:
				return PropertyValue(value.doubleValue);
			case PropertyType::STRING:
				return PropertyValue(std::string(value.stringValue));
			case PropertyType::DATE:
				return PropertyValue(TemporalDate{static_cast<int32_t>(value.intValue)});
			case PropertyType::DATETIME:
				return PropertyValue(TemporalDateTime{value.intValue});
			case PropertyType::DURATION:
				return PropertyValue(value.durationValue);
			case PropertyType::LIST:
			case PropertyType::MAP:
			case PropertyType::COMPOSITE:
			case PropertyType::UNKNOWN:
			default:
				return value.fallbackValue != nullptr ? *value.fallbackValue : PropertyValue();
		}
	}

	inline TypedOrderKey orderKeyFromScalar(const TypedScalarValue &value) {
		switch (value.type) {
			case PropertyType::NULL_TYPE:
				return TypedOrderKey::fromNull();
			case PropertyType::BOOLEAN:
				return TypedOrderKey::fromBoolean(value.boolValue);
			case PropertyType::INTEGER:
				return TypedOrderKey::fromInteger(value.intValue);
			case PropertyType::DOUBLE:
				return TypedOrderKey::fromDouble(value.doubleValue);
			case PropertyType::STRING:
				return TypedOrderKey::fromString(value.stringValue);
			case PropertyType::DATE:
				return TypedOrderKey::fromDateEpochDays(value.intValue);
			case PropertyType::DATETIME:
				return TypedOrderKey::fromDateTimeEpochMillis(value.intValue);
			case PropertyType::DURATION:
				return TypedOrderKey::fromDuration(value.durationValue);
			case PropertyType::LIST:
			case PropertyType::MAP:
			case PropertyType::COMPOSITE:
			case PropertyType::UNKNOWN:
			default:
				return value.fallbackValue != nullptr ? TypedOrderKey::from(*value.fallbackValue)
													  : TypedOrderKey::fromNull();
		}
	}

	inline bool isNullScalar(const TypedScalarValue &value) {
		return value.type == PropertyType::NULL_TYPE ||
			   ((value.type == PropertyType::UNKNOWN || value.type == PropertyType::COMPOSITE) &&
				value.fallbackValue == nullptr);
	}

} // namespace graph::query::execution
