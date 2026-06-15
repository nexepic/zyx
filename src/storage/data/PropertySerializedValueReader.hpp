#pragma once

#include "graph/storage/data/DataManager.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "graph/utils/Serializer.hpp"

namespace graph::storage {
	namespace {
		class membuf : public std::streambuf {
		public:
			membuf(char *base, size_t size) { this->setg(base, base, base + size); }
			[[nodiscard]] size_t consumed() const { return static_cast<size_t>(this->gptr() - this->eback()); }
		};

		size_t remainingBytes(const char *cursor, const char *end) { return static_cast<size_t>(end - cursor); }

		bool readRawBytes(const char *&cursor, const char *end, void *out, size_t size) {
			if (remainingBytes(cursor, end) < size) { // ZYX_COV_EXCL_LINE
				return false; // ZYX_COV_EXCL_LINE
			}
			std::memcpy(out, cursor, size);
			cursor += size;
			return true;
		}

		template<typename T>
		bool readPod(const char *&cursor, const char *end, T &out) {
			static_assert(std::is_trivial_v<T>, "readPod can only read trivial values");
			return readRawBytes(cursor, end, &out, sizeof(T));
		}

		template<typename T>
		void readUncheckedPod(const char *&cursor, T &out) {
			static_assert(std::is_trivial_v<T>, "readUncheckedPod can only read trivial values");
			std::memcpy(&out, cursor, sizeof(T));
			cursor += sizeof(T);
		}

		bool readString(const char *&cursor, const char *end, std::string &out) {
			uint32_t size = 0;
			if (!readPod(cursor, end, size) || remainingBytes(cursor, end) < size) { // ZYX_COV_EXCL_LINE
				return false; // ZYX_COV_EXCL_LINE
			}
			out.assign(cursor, cursor + size);
			cursor += size;
			return true;
		}

		struct SerializedStringView {
			const char *data = nullptr;
			uint32_t size = 0;
		};

		struct PropertyRecordHeader {
			int64_t propertyId = 0;
			int64_t entityId = 0;
			uint32_t entityType = 0;
			bool active = false;
			uint32_t propertyCount = 0;
		};

		bool readPropertyRecordHeader(const char *&cursor, const char *end, PropertyRecordHeader &header) {
			constexpr size_t headerBytes = sizeof(header.propertyId) + sizeof(header.entityId) +
										   sizeof(header.entityType) + sizeof(header.active) +
										   sizeof(header.propertyCount);
			if (remainingBytes(cursor, end) < headerBytes) { // ZYX_COV_EXCL_LINE
				return false; // ZYX_COV_EXCL_LINE
			}
			readUncheckedPod(cursor, header.propertyId);
			readUncheckedPod(cursor, header.entityId);
			readUncheckedPod(cursor, header.entityType);
			readUncheckedPod(cursor, header.active);
			readUncheckedPod(cursor, header.propertyCount);
			return true;
		}

		std::optional<PropertyRecordHeader> readActivePropertyRecordHeader(const char *&cursor, const char *end) {
			PropertyRecordHeader header;
			if (!readPropertyRecordHeader(cursor, end, header)) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
			if (!header.active || header.propertyId == 0) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
			return header;
		}

		bool readStringView(const char *&cursor, const char *end, SerializedStringView &out) {
			uint32_t size = 0;
			if (!readPod(cursor, end, size) || remainingBytes(cursor, end) < size) { // ZYX_COV_EXCL_LINE
				return false; // ZYX_COV_EXCL_LINE
			}
			out = {cursor, size};
			cursor += size;
			return true;
		}

		bool stringViewEquals(const SerializedStringView &view, const std::string &value) {
			return view.size == value.size() &&
				   (view.size == 0 || std::memcmp(view.data, value.data(), view.size) == 0); // ZYX_COV_EXCL_LINE
		}

		bool skipBytes(const char *&cursor, const char *end, size_t size) {
			if (remainingBytes(cursor, end) < size) { // ZYX_COV_EXCL_LINE
				return false; // ZYX_COV_EXCL_LINE
			}
			cursor += size;
			return true;
		}

		bool skipString(const char *&cursor, const char *end) {
			uint32_t size = 0;
			return readPod(cursor, end, size) && skipBytes(cursor, end, size); // ZYX_COV_EXCL_LINE
		}

		bool skipPropertyValue(const char *&cursor, const char *end) {
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return false; // ZYX_COV_EXCL_LINE
			}

			switch (type) {
				case PropertyType::NULL_TYPE:
					return true;
				case PropertyType::BOOLEAN:
					return skipBytes(cursor, end, sizeof(bool));
				case PropertyType::INTEGER:
					return skipBytes(cursor, end, sizeof(int64_t));
				case PropertyType::DOUBLE:
					return skipBytes(cursor, end, sizeof(double));
				case PropertyType::STRING:
					return skipString(cursor, end);
				case PropertyType::LIST: {
					uint32_t count = 0;
					if (!readPod(cursor, end, count)) { // ZYX_COV_EXCL_LINE
						return false; // ZYX_COV_EXCL_LINE
					}
					for (uint32_t i = 0; i < count; ++i) {
						if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
							return false; // ZYX_COV_EXCL_LINE
						}
					}
					return true;
				}
				case PropertyType::MAP: {
					uint32_t count = 0;
					if (!readPod(cursor, end, count)) { // ZYX_COV_EXCL_LINE
						return false; // ZYX_COV_EXCL_LINE
					}
					for (uint32_t i = 0; i < count; ++i) {
						if (!skipString(cursor, end) || !skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
							return false; // ZYX_COV_EXCL_LINE
						}
					}
					return true;
				}
				case PropertyType::DATE:
					return skipBytes(cursor, end, sizeof(int32_t));
				case PropertyType::DATETIME:
					return skipBytes(cursor, end, sizeof(int64_t));
				case PropertyType::DURATION:
					return skipBytes(cursor, end, sizeof(int64_t) * 3);
				default: // ZYX_COV_EXCL_LINE
					return false; // ZYX_COV_EXCL_LINE
			}
		}

		std::optional<PropertyValue> readSerializedPropertyValueFallback(const char *&cursor, const char *end) {
			if (cursor > end) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
			try {
				membuf valueBuffer(const_cast<char *>(cursor), static_cast<size_t>(end - cursor));
				std::istream stream(&valueBuffer);
				auto value = utils::Serializer::deserialize<PropertyValue>(stream);
				const size_t consumed = valueBuffer.consumed();
				if (consumed == 0 || static_cast<size_t>(end - cursor) < consumed) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
				cursor += consumed;
				return value;
			} catch (...) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}
		}

		std::optional<PropertyValue> readSerializedPropertyValue(const char *&cursor, const char *end) {
			const char *valueStart = cursor;
			PropertyType type = PropertyType::UNKNOWN;
			if (!readPod(cursor, end, type)) { // ZYX_COV_EXCL_LINE
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			switch (type) {
				case PropertyType::NULL_TYPE:
					return PropertyValue();
				case PropertyType::BOOLEAN: {
					bool value = false;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(value);
				}
				case PropertyType::INTEGER: {
					int64_t value = 0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(value);
				}
				case PropertyType::DOUBLE: {
					double value = 0.0;
					if (!readPod(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(value);
				}
				case PropertyType::STRING: {
					std::string value;
					if (!readString(cursor, end, value)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(std::move(value));
				}
				case PropertyType::DATE: {
					TemporalDate value;
					if (!readPod(cursor, end, value.epochDays)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(value);
				}
				case PropertyType::DATETIME: {
					TemporalDateTime value;
					if (!readPod(cursor, end, value.epochMillis)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(value);
				}
				case PropertyType::DURATION: {
					TemporalDuration value;
					if (!readPod(cursor, end, value.months) || !readPod(cursor, end, value.days) || // ZYX_COV_EXCL_LINE
						!readPod(cursor, end, value.nanos)) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					return PropertyValue(value);
				}
				case PropertyType::LIST:
				case PropertyType::MAP:
				case PropertyType::COMPOSITE:
				case PropertyType::UNKNOWN:
				default:
					cursor = valueStart;
					return readSerializedPropertyValueFallback(cursor, end);
			}
		}

		[[maybe_unused]] std::optional<std::unordered_map<std::string, PropertyValue>>
		readSelectedPropertyValues(const char *buf, const std::unordered_set<std::string> &requestedKeys) {
			const char *cursor = buf;
			const char *end = buf + Property::TOTAL_PROPERTY_SIZE;

			auto header = readActivePropertyRecordHeader(cursor, end);
			if (!header.has_value()) {
				return std::nullopt; // ZYX_COV_EXCL_LINE
			}

			std::unordered_map<std::string, PropertyValue> values;
			values.reserve(std::min<size_t>(header->propertyCount, requestedKeys.size()));
			for (uint32_t i = 0; i < header->propertyCount; ++i) {
				std::string key;
				if (!readString(cursor, end, key)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}

				if (requestedKeys.contains(key)) {
					auto value = readSerializedPropertyValue(cursor, end);
					if (!value.has_value()) { // ZYX_COV_EXCL_LINE
						return std::nullopt; // ZYX_COV_EXCL_LINE
					}
					values.emplace(std::move(key), std::move(*value));
				} else if (!skipPropertyValue(cursor, end)) { // ZYX_COV_EXCL_LINE
					return std::nullopt; // ZYX_COV_EXCL_LINE
				}
			}
			return values;
		}

	} // namespace
} // namespace graph::storage
