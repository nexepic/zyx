/**
 * @file PropertyValueCodec.cpp
 * @brief Canonical, bounded PropertyValue wire codec for native storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace graph::storage {

	namespace {

		constexpr uint32_t MAX_NESTING_DEPTH = 64U;

		enum class PropertyValueWireKind : uint8_t {
			PVW_NULL = 1, PVW_BOOLEAN = 2, PVW_INTEGER = 3, PVW_DOUBLE = 4, PVW_STRING = 5,
			PVW_LIST = 6, PVW_MAP = 7, PVW_DATE = 8, PVW_DATETIME = 9, PVW_DURATION = 10, PVW_BYTES = 11,
		};

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool bytewiseLess(std::string_view lhs, std::string_view rhs) {
			return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), [](char left, char right) {
				return static_cast<unsigned char>(left) < static_cast<unsigned char>(right);
			});
		}

		void appendU8(std::vector<std::byte> &bytes, uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
		void appendU32(std::vector<std::byte> &bytes, uint32_t value) {
			const size_t offset = bytes.size();
			bytes.resize(offset + sizeof(value));
			LittleEndianCodec::writeU32(bytes, offset, value);
		}
		void appendU64(std::vector<std::byte> &bytes, uint64_t value) {
			const size_t offset = bytes.size();
			bytes.resize(offset + sizeof(value));
			LittleEndianCodec::writeU64(bytes, offset, value);
		}
		void appendBytes(std::vector<std::byte> &bytes, std::string_view value) {
			bytes.insert(bytes.end(), reinterpret_cast<const std::byte *>(value.data()),
						 reinterpret_cast<const std::byte *>(value.data() + value.size()));
		}
		void appendBytes(std::vector<std::byte> &bytes, std::span<const std::byte> value) {
			bytes.insert(bytes.end(), value.begin(), value.end());
		}
		void requireU32Size(size_t value, const char *message) {
			if (value > std::numeric_limits<uint32_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
		}

		struct ByteReader {
			std::span<const std::byte> bytes;
			size_t offset = 0U;
			[[nodiscard]] size_t remaining() const { return bytes.size() - offset; }
			[[nodiscard]] uint8_t readU8() {
				if (remaining() < 1U) fail(FormatError::FERR_BUFFER_TOO_SMALL, "Property value is truncated");
				return static_cast<uint8_t>(bytes[offset++]);
			}
			[[nodiscard]] uint32_t readU32() {
				if (remaining() < sizeof(uint32_t)) fail(FormatError::FERR_BUFFER_TOO_SMALL, "Property value is truncated");
				const uint32_t value = LittleEndianCodec::readU32(bytes, offset);
				offset += sizeof(value);
				return value;
			}
			[[nodiscard]] uint64_t readU64() {
				if (remaining() < sizeof(uint64_t)) fail(FormatError::FERR_BUFFER_TOO_SMALL, "Property value is truncated");
				const uint64_t value = LittleEndianCodec::readU64(bytes, offset);
				offset += sizeof(value);
				return value;
			}
			[[nodiscard]] std::string readString(uint32_t count) {
				if (count > remaining()) fail(FormatError::FERR_BUFFER_TOO_SMALL, "Property string is truncated");
				const char *data = reinterpret_cast<const char *>(bytes.data() + offset);
				offset += count;
				return {data, count};
			}
			[[nodiscard]] std::span<const std::byte> readBytes(uint32_t count) {
				if (count > remaining()) fail(FormatError::FERR_BUFFER_TOO_SMALL, "Property payload is truncated");
				const auto result = bytes.subspan(offset, count);
				offset += count;
				return result;
			}
		};

		[[nodiscard]] PropertyValueWireKind kindFor(const PropertyValue &value) {
			return std::visit([]<typename T>(const T &) {
				if constexpr (std::is_same_v<T, std::monostate>) return PropertyValueWireKind::PVW_NULL;
				else if constexpr (std::is_same_v<T, bool>) return PropertyValueWireKind::PVW_BOOLEAN;
				else if constexpr (std::is_same_v<T, int64_t>) return PropertyValueWireKind::PVW_INTEGER;
				else if constexpr (std::is_same_v<T, double>) return PropertyValueWireKind::PVW_DOUBLE;
				else if constexpr (std::is_same_v<T, std::string>) return PropertyValueWireKind::PVW_STRING;
				else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) return PropertyValueWireKind::PVW_LIST;
				else if constexpr (std::is_same_v<T, PropertyValue::MapType>) return PropertyValueWireKind::PVW_MAP;
				else if constexpr (std::is_same_v<T, TemporalDate>) return PropertyValueWireKind::PVW_DATE;
				else if constexpr (std::is_same_v<T, TemporalDateTime>) return PropertyValueWireKind::PVW_DATETIME;
				else if constexpr (std::is_same_v<T, TemporalDuration>) return PropertyValueWireKind::PVW_DURATION;
				else { static_assert(std::is_same_v<T, PropertyValue::Bytes>); return PropertyValueWireKind::PVW_BYTES; }
			}, value.getVariant());
		}

		void encodeValue(std::vector<std::byte> &bytes, const PropertyValue &value, uint32_t depth);

		void encodePayload(std::vector<std::byte> &payload, const PropertyValue &value, uint32_t depth) {
			std::visit([&payload, depth]<typename T>(const T &argument) {
				if constexpr (std::is_same_v<T, std::monostate>) {
					return;
				} else if constexpr (std::is_same_v<T, bool>) {
					appendU8(payload, argument ? 1U : 0U);
				} else if constexpr (std::is_same_v<T, int64_t>) {
					appendU64(payload, std::bit_cast<uint64_t>(argument));
				} else if constexpr (std::is_same_v<T, double>) {
					static_assert(std::numeric_limits<double>::is_iec559);
					appendU64(payload, std::bit_cast<uint64_t>(argument));
				} else if constexpr (std::is_same_v<T, std::string>) {
					requireU32Size(argument.size(), "Property string is too large");
					appendU32(payload, static_cast<uint32_t>(argument.size()));
					appendBytes(payload, argument);
				} else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) {
					requireU32Size(argument.size(), "Property list has too many elements");
					appendU32(payload, static_cast<uint32_t>(argument.size()));
					for (const auto &entry: argument) encodeValue(payload, entry, depth + 1U);
				} else if constexpr (std::is_same_v<T, PropertyValue::MapType>) {
					std::vector<std::pair<std::string_view, const PropertyValue *>> entries;
					entries.reserve(argument.size());
					for (const auto &[key, entry]: argument) entries.emplace_back(key, &entry);
					std::ranges::sort(entries, [](const auto &left, const auto &right) {
						return bytewiseLess(left.first, right.first);
					});
					requireU32Size(entries.size(), "Property map has too many entries");
					appendU32(payload, static_cast<uint32_t>(entries.size()));
					for (const auto &[key, entry]: entries) {
						requireU32Size(key.size(), "Property map key is too large");
						appendU32(payload, static_cast<uint32_t>(key.size()));
						appendBytes(payload, key);
						encodeValue(payload, *entry, depth + 1U);
					}
				} else if constexpr (std::is_same_v<T, TemporalDate>) {
					appendU32(payload, std::bit_cast<uint32_t>(argument.epochDays));
					} else if constexpr (std::is_same_v<T, TemporalDateTime>) {
						appendU64(payload, std::bit_cast<uint64_t>(argument.epochMillis));
					} else if constexpr (std::is_same_v<T, TemporalDuration>) {
						appendU64(payload, std::bit_cast<uint64_t>(argument.months));
						appendU64(payload, std::bit_cast<uint64_t>(argument.days));
						appendU64(payload, std::bit_cast<uint64_t>(argument.nanos));
					} else if constexpr (std::is_same_v<T, PropertyValue::Bytes>) {
						requireU32Size(argument.size(), "Property binary value is too large");
						appendU32(payload, static_cast<uint32_t>(argument.size()));
						appendBytes(payload, std::span<const std::byte>(argument.data(), argument.size()));
					} else {
						static_assert(std::is_same_v<T, void>, "Unhandled PropertyValue variant");
					}
			}, value.getVariant());
		}

		void encodeValue(std::vector<std::byte> &bytes, const PropertyValue &value, uint32_t depth) {
			if (depth > MAX_NESTING_DEPTH) fail(FormatError::FERR_INVALID_STATE, "Property value nesting is too deep");
			std::vector<std::byte> payload;
			encodePayload(payload, value, depth);
			requireU32Size(payload.size(), "Property value payload is too large");
			appendU8(bytes, static_cast<uint8_t>(kindFor(value)));
			appendU8(bytes, 0U);
			appendU32(bytes, static_cast<uint32_t>(payload.size()));
			appendBytes(bytes, payload);
		}

		[[nodiscard]] PropertyValue decodeValue(ByteReader &reader, uint32_t depth) {
			if (depth > MAX_NESTING_DEPTH) fail(FormatError::FERR_INVALID_STATE, "Property value nesting is too deep");
			const auto kind = static_cast<PropertyValueWireKind>(reader.readU8());
			if (reader.readU8() != 0U) fail(FormatError::FERR_RESERVED_BITS, "Property value has nonzero flags");
			ByteReader payload{reader.readBytes(reader.readU32())};
			auto requireComplete = [&payload] {
				if (payload.remaining() != 0U) fail(FormatError::FERR_INVALID_HEADER, "Property payload has trailing bytes");
			};
			switch (kind) {
				case PropertyValueWireKind::PVW_NULL: requireComplete(); return PropertyValue(std::monostate{});
				case PropertyValueWireKind::PVW_BOOLEAN: {
					const auto value = payload.readU8();
					if (value > 1U) fail(FormatError::FERR_INVALID_ENUM, "Property Boolean value is invalid");
					requireComplete(); return PropertyValue(value != 0U);
				}
				case PropertyValueWireKind::PVW_INTEGER: {
					const auto value = std::bit_cast<int64_t>(payload.readU64()); requireComplete(); return PropertyValue(value);
				}
				case PropertyValueWireKind::PVW_DOUBLE: {
					static_assert(std::numeric_limits<double>::is_iec559);
					const auto value = std::bit_cast<double>(payload.readU64()); requireComplete(); return PropertyValue(value);
				}
				case PropertyValueWireKind::PVW_STRING: {
					const auto value = payload.readString(payload.readU32()); requireComplete(); return PropertyValue(value);
				}
				case PropertyValueWireKind::PVW_LIST: {
					const uint32_t count = payload.readU32();
					if (count > payload.remaining() / 6U) fail(FormatError::FERR_INVALID_HEADER, "Property list count exceeds payload");
					std::vector<PropertyValue> values; values.reserve(count);
					for (uint32_t index = 0U; index < count; ++index) values.push_back(decodeValue(payload, depth + 1U));
					requireComplete(); return PropertyValue(std::move(values));
				}
				case PropertyValueWireKind::PVW_MAP: {
					const uint32_t count = payload.readU32();
					if (count > payload.remaining() / 10U) fail(FormatError::FERR_INVALID_HEADER, "Property map count exceeds payload");
					PropertyValue::MapType values; std::string previous;
					for (uint32_t index = 0U; index < count; ++index) {
						const auto key = payload.readString(payload.readU32());
						if (index != 0U && !bytewiseLess(previous, key)) fail(FormatError::FERR_INVALID_HEADER, "Property map keys are not canonical");
						values.emplace(key, decodeValue(payload, depth + 1U)); previous = key;
					}
					requireComplete(); return PropertyValue(std::move(values));
				}
				case PropertyValueWireKind::PVW_DATE: {
					const TemporalDate value{std::bit_cast<int32_t>(payload.readU32())}; requireComplete(); return PropertyValue(value);
				}
				case PropertyValueWireKind::PVW_DATETIME: {
					const TemporalDateTime value{std::bit_cast<int64_t>(payload.readU64())}; requireComplete(); return PropertyValue(value);
				}
				case PropertyValueWireKind::PVW_DURATION: {
					const TemporalDuration value{std::bit_cast<int64_t>(payload.readU64()), std::bit_cast<int64_t>(payload.readU64()), std::bit_cast<int64_t>(payload.readU64())};
					requireComplete(); return PropertyValue(value);
				}
				case PropertyValueWireKind::PVW_BYTES: {
					const uint32_t size = payload.readU32();
					const auto source = payload.readBytes(size);
					PropertyValue::Bytes value(source.begin(), source.end());
					requireComplete(); return PropertyValue(std::move(value));
				}
				default: fail(FormatError::FERR_INVALID_ENUM, "Property value kind is unknown");
			}
		}

	} // namespace

	void CanonicalPropertyValueCodec::append(std::vector<std::byte> &bytes, const PropertyValue &value) {
		encodeValue(bytes, value, 0U);
	}

	PropertyValue CanonicalPropertyValueCodec::decodeNext(std::span<const std::byte> bytes, size_t &offset) {
		if (offset > bytes.size()) fail(FormatError::FERR_INVALID_HEADER, "Property cursor exceeds payload");
		ByteReader reader{bytes.subspan(offset)};
		PropertyValue value = decodeValue(reader, 0U);
		offset += reader.offset;
		return value;
	}

} // namespace graph::storage
