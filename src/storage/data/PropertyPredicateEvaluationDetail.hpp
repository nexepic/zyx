#pragma once

#include "PropertySerializedValueReader.hpp"
#include "PropertyScanTypes.hpp"
#include "PropertyEntityScanPrimitives.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace graph::storage {
	namespace {
		struct CompiledPropertyValue {
			const PropertyValue *value = nullptr;
			PropertyType type = PropertyType::UNKNOWN;
			bool boolValue = false;
			int64_t intValue = 0;
			double doubleValue = 0.0;
			const std::string *stringValue = nullptr;
			TemporalDate dateValue;
			TemporalDateTime dateTimeValue;
			TemporalDuration durationValue;
		};

		CompiledPropertyValue compilePropertyValue(const PropertyValue &value) {
			CompiledPropertyValue compiled;
			compiled.value = &value;
			compiled.type = value.getType();
			const auto &variant = value.getVariant();
			switch (compiled.type) {
				case PropertyType::BOOLEAN:
					compiled.boolValue = std::get<bool>(variant);
					break;
				case PropertyType::INTEGER:
					compiled.intValue = std::get<int64_t>(variant);
					break;
				case PropertyType::DOUBLE:
					compiled.doubleValue = std::get<double>(variant);
					break;
				case PropertyType::STRING:
					compiled.stringValue = &std::get<std::string>(variant);
					break;
				case PropertyType::DATE:
					compiled.dateValue = std::get<TemporalDate>(variant);
					break;
				case PropertyType::DATETIME:
					compiled.dateTimeValue = std::get<TemporalDateTime>(variant);
					break;
				case PropertyType::DURATION:
					compiled.durationValue = std::get<TemporalDuration>(variant);
					break;
				default:
					break;
			}
			return compiled;
		}

		struct PredicateExpectation {
			const std::string *key = nullptr;
			CompiledPropertyValue value;
		};

		struct SinglePredicateExpectation {
			const std::string *key = nullptr;
			CompiledPropertyValue value;
		};

		struct PredicateSpecExpectation {
			const std::string *key = nullptr;
			const PropertyValue *value = nullptr;
			const PropertyValue *upperValue = nullptr;
			PropertyEntityPredicateOp op = PropertyEntityPredicateOp::PEP_EQ;
		};

		struct PredicateSpecGroup {
			const std::string *key = nullptr;
			std::vector<const PredicateSpecExpectation *> predicates;
		};

		bool readSelectedPropertyColumns(const char *buf,
										 const std::unordered_map<std::string, size_t> &requestedKeyIndices,
										 const std::vector<std::vector<std::optional<PropertyValue>> *> &columnTargets,
										 const std::vector<PropertyEntityRowRef> &refs, size_t refBegin,
										 size_t refEnd) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return false; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				std::string key;
				if (!readString(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return false; // ZYX_COV_EXCL_LINE
				}

				auto keyIt = requestedKeyIndices.find(key);
				if (keyIt == requestedKeyIndices.end()) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return false; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return false; // ZYX_COV_EXCL_LINE
				}

				auto &column = *columnTargets[keyIt->second];
				if (refEnd == refBegin + 1) {
					column[refs[refBegin].row] = std::move(*value);
				} else {
					for (size_t ref = refBegin; ref < refEnd; ++ref) {
						column[refs[ref].row] = *value;
					}
				}
			}
			return true;
		}

		bool readSelectedPropertyColumnsOne(
				const char *buf,
				const std::unordered_map<std::string, size_t> &requestedKeyIndices,
				const std::vector<std::vector<std::optional<PropertyValue>> *> &columnTargets,
				size_t row) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return false; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				std::string key;
				if (!readString(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return false; // ZYX_COV_EXCL_LINE
				}

				auto keyIt = requestedKeyIndices.find(key);
				if (keyIt == requestedKeyIndices.end()) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return false; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return false; // ZYX_COV_EXCL_LINE
				}
				(*columnTargets[keyIt->second])[row] = std::move(*value);
			}
			return true;
		}

		std::optional<size_t> visitSelectedPropertyValue(const char *buf, const std::string &requestedKey,
														 const std::vector<PropertyEntityRowRef> &refs, size_t refBegin,
														 size_t refEnd, const PropertyEntityValueVisitor &visitor) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, requestedKey)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				for (size_t ref = refBegin; ref < refEnd; ++ref) {
					visitor(refs[ref].row, *value);
				}
				return refEnd - refBegin;
			}
			return size_t{0};
		}

		std::optional<size_t> visitSelectedPropertyValueOne(const char *buf,
															const std::string &requestedKey,
															size_t row,
															const PropertyEntityValueVisitor &visitor) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, requestedKey)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				visitor(row, *value);
				return size_t{1};
			}
			return size_t{0};
		}

		std::optional<PropertyEntityOwnerValue> readPropertyOwnerValue(
				const char *buf,
				EntityType ownerType,
				const std::string &requestedKey,
				std::span<const int64_t> sortedOwnerIds) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
			if (header->entityType != toUnderlying(ownerType) ||
				!ownerFilterContains(sortedOwnerIds, header->entityId)) {
				return std::nullopt;
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, requestedKey)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				return PropertyEntityOwnerValue{header->entityId, std::move(*value)};
			}
			return std::nullopt;
		}

		std::vector<PropertyEntityOwnerKeyValue> readPropertyOwnerKeyValues(
				const char *buf,
				EntityType ownerType,
				std::span<const std::string> requestedKeys,
				std::span<const int64_t> sortedOwnerIds) {
			std::vector<PropertyEntityOwnerKeyValue> values;
			if (requestedKeys.empty()) {
				return values;
			}

			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return {}; // ZYX_COV_EXCL_LINE
			}
			if (header->entityType != toUnderlying(ownerType) ||
				!ownerFilterContains(sortedOwnerIds, header->entityId)) {
				return values;
			}

			values.reserve(std::min<size_t>(header->propertyCount, requestedKeys.size()));
			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return {}; // ZYX_COV_EXCL_LINE
				}

				const std::string *matchedKey = nullptr;
				for (const auto &requestedKey: requestedKeys) {
					if (stringViewEquals(key, requestedKey)) {
						matchedKey = &requestedKey;
						break;
					}
				}

				if (matchedKey == nullptr) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return {}; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto value = readSerializedPropertyValue(cursor, end);
				if (!value.has_value()) { // ZYX_COV_EXCL_LINE
					return {}; // ZYX_COV_EXCL_LINE
				}
				values.push_back(PropertyEntityOwnerKeyValue{header->entityId, *matchedKey, std::move(*value)});
			}
			return values;
		}

		std::optional<PropertyEntityScalarValue>
		readSerializedPropertyScalarValue(const char *&cursor, const char *end,
										  std::optional<PropertyValue> &fallbackStorage) {
			const char *valueStart = cursor;
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			PropertyEntityScalarValue scalar;
			scalar.type = type;
			switch (type) {
				case PropertyType::NULL_TYPE:
					return scalar;
				case PropertyType::BOOLEAN:
					if (!readPod(cursor, end, scalar.boolValue)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return scalar;
				case PropertyType::INTEGER:
					if (!readPod(cursor, end, scalar.intValue)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return scalar;
				case PropertyType::DOUBLE:
					if (!readPod(cursor, end, scalar.doubleValue)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return scalar;
				case PropertyType::STRING: {
					SerializedStringView value;
					if (!readStringView(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					scalar.stringValue = std::string_view(value.data, value.size);
					return scalar;
				}
				case PropertyType::DATE: {
					int32_t epochDays = 0;
					if (!readPod(cursor, end, epochDays)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					scalar.intValue = epochDays;
					return scalar;
				}
				case PropertyType::DATETIME:
					if (!readPod(cursor, end, scalar.intValue)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return scalar;
				case PropertyType::DURATION:
					if (!readPod(cursor, end, scalar.durationValue.months) ||
						!readPod(cursor, end, scalar.durationValue.days) || // ZYX_COV_EXCL_LINE
						!readPod(cursor, end, scalar.durationValue.nanos)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return scalar;
				case PropertyType::LIST:
				case PropertyType::MAP:
				case PropertyType::COMPOSITE:
				case PropertyType::UNKNOWN:
				default:
					cursor = valueStart;
					fallbackStorage = readSerializedPropertyValue(cursor, end);
					if (!fallbackStorage.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					scalar.type = fallbackStorage->getType();
					scalar.fallbackValue = &*fallbackStorage;
					return scalar;
			}
		}

		std::optional<size_t> visitSelectedPropertyScalarValue(const char *buf, const std::string &requestedKey,
															   const std::vector<PropertyEntityRowRef> &refs,
															   size_t refBegin, size_t refEnd,
															   const PropertyEntityScalarValueVisitor &visitor) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, requestedKey)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				std::optional<PropertyValue> fallbackStorage;
				auto scalar = readSerializedPropertyScalarValue(cursor, end, fallbackStorage);
				if (!scalar.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				for (size_t ref = refBegin; ref < refEnd; ++ref) {
					visitor(refs[ref].row, *scalar);
				}
				return refEnd - refBegin;
			}
			return size_t{0};
		}

		std::optional<size_t> visitSelectedPropertyScalarValueOne(const char *buf,
																  const std::string &requestedKey,
																  size_t row,
																  const PropertyEntityScalarValueVisitor &visitor) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, requestedKey)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				std::optional<PropertyValue> fallbackStorage;
				auto scalar = readSerializedPropertyScalarValue(cursor, end, fallbackStorage);
				if (!scalar.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				visitor(row, *scalar);
				return size_t{1};
			}
			return size_t{0};
		}

		std::optional<bool> serializedPropertyValueEquals(const char *&cursor, const char *end,
														  const CompiledPropertyValue &expected) {
			const char *valueStart = cursor;
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
			if (type != expected.type) {
				return false; // ZYX_COV_EXCL_LINE
			}

			switch (type) {
				case PropertyType::NULL_TYPE: // ZYX_COV_EXCL_LINE
					return true; // ZYX_COV_EXCL_LINE
				case PropertyType::BOOLEAN: {
					bool value = false;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return value == expected.boolValue;
				}
				case PropertyType::INTEGER: {
					int64_t value = 0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return value == expected.intValue;
				}
				case PropertyType::DOUBLE: {
					double value = 0.0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return value == expected.doubleValue;
				}
				case PropertyType::STRING: {
					SerializedStringView value;
					if (!readStringView(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return expected.stringValue != nullptr && stringViewEquals(value, *expected.stringValue);
				}
				case PropertyType::DATE: {
					int32_t epochDays = 0;
					if (!readPod(cursor, end, epochDays)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return epochDays == expected.dateValue.epochDays;
				}
				case PropertyType::DATETIME: {
					int64_t epochMillis = 0;
					if (!readPod(cursor, end, epochMillis)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return epochMillis == expected.dateTimeValue.epochMillis;
				}
				case PropertyType::DURATION: {
					TemporalDuration value;
					if (!readPod(cursor, end, value.months) || !readPod(cursor, end, value.days) || // ZYX_COV_EXCL_LINE
						!readPod(cursor, end, value.nanos)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return value == expected.durationValue;
				}
				case PropertyType::LIST: // ZYX_COV_EXCL_LINE
				case PropertyType::MAP: { // ZYX_COV_EXCL_LINE
					cursor = valueStart;
					auto value = readSerializedPropertyValue(cursor, end);
					if (!value.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return expected.value != nullptr && value.value() == *expected.value;
				}
				default: // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
			}
		}

		const PredicateExpectation *findPredicateExpectation(const SerializedStringView &key,
															 const std::vector<PredicateExpectation> &expected) {
			for (const auto &entry: expected) {
				if (stringViewEquals(key, *entry.key)) {
					return &entry;
				}
			}
			return nullptr;
		}

		std::optional<bool> readPropertyEntityPredicateMatch(const char *buf,
															 const std::vector<PredicateExpectation> &expected) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			size_t matchedKeys = 0;
			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				const auto *expectedEntry = findPredicateExpectation(key, expected);
				if (expectedEntry == nullptr) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				auto matches = serializedPropertyValueEquals(cursor, end, expectedEntry->value);
				if (!matches.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				if (!matches.value()) {
					return false; // ZYX_COV_EXCL_LINE
				}
				++matchedKeys;
				if (matchedKeys == expected.size()) {
					return true;
				}
			}
			return matchedKeys == expected.size();
		}

		std::optional<bool> readPropertyEntitySinglePredicateMatch(const char *buf,
																   const SinglePredicateExpectation &expected) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, *expected.key)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				return serializedPropertyValueEquals(cursor, end, expected.value);
			}
			return false;
		}

		bool propertyValueSatisfiesPredicate(const PropertyValue &actual, const PredicateSpecExpectation &expected) {
			switch (expected.op) {
				case PropertyEntityPredicateOp::PEP_EQ:
					return actual == *expected.value;
				case PropertyEntityPredicateOp::PEP_NE: // ZYX_COV_EXCL_LINE
					return actual != *expected.value; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_LT: // ZYX_COV_EXCL_LINE
					return actual < *expected.value; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_LE: // ZYX_COV_EXCL_LINE
					return actual <= *expected.value; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_GT: // ZYX_COV_EXCL_LINE
					return actual > *expected.value; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_GE: // ZYX_COV_EXCL_LINE
					return actual >= *expected.value; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_RANGE_CLOSED: // ZYX_COV_EXCL_LINE
					return expected.upperValue != nullptr && actual >= *expected.value && // ZYX_COV_EXCL_LINE
						   actual <= *expected.upperValue; // ZYX_COV_EXCL_LINE
			}
			return false; // ZYX_COV_EXCL_LINE
		}

		template<typename T>
		bool typedValueSatisfiesPredicate(const T &actual, const PredicateSpecExpectation &expected) {
			const auto *expectedValue = std::get_if<T>(&expected.value->getVariant());
			if (expectedValue == nullptr) {
				return propertyValueSatisfiesPredicate(PropertyValue(actual), expected);
			}

			switch (expected.op) {
				case PropertyEntityPredicateOp::PEP_EQ: // ZYX_COV_EXCL_LINE
					return actual == *expectedValue; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_NE: // ZYX_COV_EXCL_LINE
					return actual != *expectedValue; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_LT: // ZYX_COV_EXCL_LINE
					return actual < *expectedValue; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_LE: // ZYX_COV_EXCL_LINE
					return actual <= *expectedValue; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_GT: // ZYX_COV_EXCL_LINE
					return actual > *expectedValue; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_GE: // ZYX_COV_EXCL_LINE
					return actual >= *expectedValue; // ZYX_COV_EXCL_LINE
				case PropertyEntityPredicateOp::PEP_RANGE_CLOSED: { // ZYX_COV_EXCL_LINE
					if (expected.upperValue == nullptr) { // ZYX_COV_EXCL_LINE
						return false; // ZYX_COV_EXCL_LINE
					} // ZYX_COV_EXCL_LINE
					const auto *upperValue = std::get_if<T>(&expected.upperValue->getVariant()); // ZYX_COV_EXCL_LINE
					return upperValue != nullptr && actual >= *expectedValue && actual <= *upperValue; // ZYX_COV_EXCL_LINE
				} // ZYX_COV_EXCL_LINE
			}
			return false; // ZYX_COV_EXCL_LINE
		}

		int compareStringView(const SerializedStringView &view, const std::string &value) {
			const size_t commonSize = std::min<size_t>(view.size, value.size());
			const int commonCompare =
					commonSize == 0 ? 0 : std::memcmp(view.data, value.data(), commonSize); // ZYX_COV_EXCL_LINE
			if (commonCompare != 0) {
				return commonCompare;
			}
			if (view.size == value.size()) {
				return 0;
			}
			return view.size < value.size() ? -1 : 1;
		}

		bool stringViewSatisfiesPredicate(const SerializedStringView &actual,
										  const PredicateSpecExpectation &expected) {
			const auto *expectedValue = std::get_if<std::string>(&expected.value->getVariant());
			if (expectedValue == nullptr) {
				return propertyValueSatisfiesPredicate(PropertyValue(std::string(actual.data, actual.size)), expected);
			}
			const int comparison = compareStringView(actual, *expectedValue);
			switch (expected.op) {
				case PropertyEntityPredicateOp::PEP_EQ:
					return comparison == 0;
				case PropertyEntityPredicateOp::PEP_NE:
					return comparison != 0;
				case PropertyEntityPredicateOp::PEP_LT:
					return comparison < 0;
				case PropertyEntityPredicateOp::PEP_LE:
					return comparison <= 0;
				case PropertyEntityPredicateOp::PEP_GT:
					return comparison > 0;
				case PropertyEntityPredicateOp::PEP_GE:
					return comparison >= 0;
				case PropertyEntityPredicateOp::PEP_RANGE_CLOSED: {
					if (expected.upperValue == nullptr) {
						return false; // ZYX_COV_EXCL_LINE
					}
					const auto *upperValue = std::get_if<std::string>(&expected.upperValue->getVariant());
					return upperValue != nullptr && comparison >= 0 && compareStringView(actual, *upperValue) <= 0;
				}
			}
			return false; // ZYX_COV_EXCL_LINE
		}

		std::vector<PredicateSpecGroup>
		groupPredicateSpecExpectations(const std::vector<PredicateSpecExpectation> &expected) {
			std::vector<PredicateSpecGroup> groups;
			groups.reserve(expected.size());
			for (const auto &entry: expected) {
				auto groupIt = std::find_if(groups.begin(), groups.end(),
											[&](const PredicateSpecGroup &group) { return *group.key == *entry.key; });
				if (groupIt == groups.end()) {
					PredicateSpecGroup group;
					group.key = entry.key;
					group.predicates.push_back(&entry);
					groups.push_back(std::move(group));
				} else {
					groupIt->predicates.push_back(&entry);
				}
			}
			return groups;
		}

		const PredicateSpecGroup *findPredicateSpecGroup(const SerializedStringView &key,
														 const std::vector<PredicateSpecGroup> &groups) {
			for (const auto &group: groups) {
				if (stringViewEquals(key, *group.key)) {
					return &group;
				}
			}
			return nullptr;
		}

		std::optional<bool> readSerializedPropertyValueSatisfiesPredicate(const char *&cursor, const char *end,
																		  const PredicateSpecExpectation &expected) {
			const char *valueStart = cursor;
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			if (type != expected.value->getType()) {
				cursor = valueStart;
				auto actual = readSerializedPropertyValue(cursor, end);
				if (!actual.has_value()) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				return propertyValueSatisfiesPredicate(*actual, expected);
			}

			switch (type) {
				case PropertyType::BOOLEAN: {
					bool value = false;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::INTEGER: {
					int64_t value = 0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::DOUBLE: {
					double value = 0.0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::STRING: {
					SerializedStringView value;
					if (!readStringView(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return stringViewSatisfiesPredicate(value, expected);
				}
				case PropertyType::DATE: {
					TemporalDate value;
					if (!readPod(cursor, end, value.epochDays)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::DATETIME: {
					TemporalDateTime value;
					if (!readPod(cursor, end, value.epochMillis)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				case PropertyType::DURATION: {
					TemporalDuration value;
					if (!readPod(cursor, end, value.months) || !readPod(cursor, end, value.days) || // ZYX_COV_EXCL_LINE
						!readPod(cursor, end, value.nanos)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return typedValueSatisfiesPredicate(value, expected);
				}
				default:
					cursor = valueStart;
					auto actual = readSerializedPropertyValue(cursor, end);
					if (!actual.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return propertyValueSatisfiesPredicate(*actual, expected);
			}
		}

		std::optional<bool> readPropertyEntityPredicateMatch(const char *buf,
															 const std::vector<PredicateSpecGroup> &expectedGroups,
															 size_t expectedPredicateCount) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			size_t matchedPredicates = 0;
			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				const auto *group = findPredicateSpecGroup(key, expectedGroups);
				if (group == nullptr) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				for (const auto *entry: group->predicates) {
					const char *valueCursor = cursor;
					auto matches = readSerializedPropertyValueSatisfiesPredicate(valueCursor, end, *entry);
					if (!matches.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					if (!matches.value()) {
						return false; // ZYX_COV_EXCL_LINE
					}
					++matchedPredicates;
				}
				if (matchedPredicates == expectedPredicateCount) {
					return true;
				}
				if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
			}
			return matchedPredicates == expectedPredicateCount;
		}

		std::optional<bool> readPropertyEntitySinglePredicateSpecMatch(
				const char *buf, const PredicateSpecExpectation &expected) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				SerializedStringView key;
				if (!readStringView(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (!stringViewEquals(key, *expected.key)) {
					if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					continue;
				}

				return readSerializedPropertyValueSatisfiesPredicate(cursor, end, expected);
			}
			return false;
		}

	} // namespace
} // namespace graph::storage
