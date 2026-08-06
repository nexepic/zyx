/**
 * @file IndexKeyCodec.cpp
 * @brief Explicit order-preserving B+Tree index key codec.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>
#include <variant>

namespace graph::storage {

	namespace {

		constexpr std::byte TAG_BOOLEAN = std::byte{1};
		constexpr std::byte TAG_INTEGER = std::byte{2};
		constexpr std::byte TAG_DOUBLE = std::byte{3};
		constexpr std::byte TAG_STRING = std::byte{4};
		constexpr size_t ENTITY_ID_BYTES = sizeof(uint64_t);

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		class IndexKeyTooLarge final {};

		void appendBigEndianU64(std::vector<std::byte> &bytes, uint64_t value) {
			for (uint32_t shift = 56U;; shift -= 8U) {
				bytes.push_back(static_cast<std::byte>((value >> shift) & UINT64_C(0xFF)));
				if (shift == 0U) {
					return;
				}
			}
		}

		[[nodiscard]] uint64_t readBigEndianU64(std::span<const std::byte> bytes, size_t offset) {
			// ZYX_COV_EXCL_START: decodeEntryEntityId validates the complete suffix before this helper is called.
			if (bytes.size() - offset < ENTITY_ID_BYTES) {
				fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable index entry key has no complete Entity ID suffix");
			}
			// ZYX_COV_EXCL_STOP
			uint64_t value = 0;
			for (size_t index = 0; index < ENTITY_ID_BYTES; ++index) {
				value = (value << 8U) | std::to_integer<uint8_t>(bytes[offset + index]);
			}
			return value;
		}

		void requireCapacity(size_t current, size_t additional) {
			if (current > RSS_INDEX_KEY_MAX_BYTES || additional > RSS_INDEX_KEY_MAX_BYTES - current) {
				throw IndexKeyTooLarge{};
			}
		}

		template<typename Build>
		[[nodiscard]] std::vector<std::byte> requireRepresentableIndexKey(Build &&build) {
			try {
				return build();
			} catch (const IndexKeyTooLarge &) {
				fail(FormatError::FERR_INVALID_HEADER, "relocatable index key exceeds its configured byte bound");
			}
		}

		template<typename Build>
		[[nodiscard]] std::optional<std::vector<std::byte>> tryBuildIndexKey(Build &&build) {
			try {
				return build();
			} catch (const IndexKeyTooLarge &) {
				return std::nullopt;
			}
		}

		void appendEscapedString(std::vector<std::byte> &bytes, const std::string &value) {
			for (const unsigned char character: value) {
				requireCapacity(bytes.size(), character == 0U ? 2U : 1U);
				bytes.push_back(static_cast<std::byte>(character));
				if (character == 0U) {
					bytes.push_back(std::byte{0xFF});
				}
			}
			requireCapacity(bytes.size(), 2U);
			bytes.push_back(std::byte{0});
			bytes.push_back(std::byte{0});
		}

		void appendValue(std::vector<std::byte> &bytes, const PropertyValue &value) {
			std::visit(
					[&bytes]<typename T>(const T &argument) {
						if constexpr (std::is_same_v<T, bool>) {
							requireCapacity(bytes.size(), 2U);
							bytes.push_back(TAG_BOOLEAN);
							bytes.push_back(argument ? std::byte{1} : std::byte{0});
						} else if constexpr (std::is_same_v<T, int64_t>) {
							requireCapacity(bytes.size(), 1U + sizeof(uint64_t));
							bytes.push_back(TAG_INTEGER);
							appendBigEndianU64(bytes, std::bit_cast<uint64_t>(argument) ^ (UINT64_C(1) << 63U));
						} else if constexpr (std::is_same_v<T, double>) {
							static_assert(std::numeric_limits<double>::is_iec559);
							if (!std::isfinite(argument)) {
								fail(FormatError::FERR_INVALID_HEADER,
									 "relocatable ordered index keys reject non-finite floating-point values");
							}
							requireCapacity(bytes.size(), 1U + sizeof(uint64_t));
							bytes.push_back(TAG_DOUBLE);
							// PropertyValue compares -0.0 and +0.0 as equal; emit one canonical key.
							const double canonical = argument == 0.0 ? 0.0 : argument;
							const uint64_t bits = std::bit_cast<uint64_t>(canonical);
							const uint64_t ordered = (bits & (UINT64_C(1) << 63U)) != 0U ? ~bits : bits ^ (UINT64_C(1) << 63U);
							appendBigEndianU64(bytes, ordered);
						} else if constexpr (std::is_same_v<T, std::string>) {
							requireCapacity(bytes.size(), 1U);
							bytes.push_back(TAG_STRING);
							appendEscapedString(bytes, argument);
						} else {
							fail(FormatError::FERR_INVALID_ENUM,
								 "relocatable ordered index keys support only Boolean, Integer, Double, and String values");
						}
					},
					value.getVariant());
		}

		[[nodiscard]] std::vector<std::byte> appendEntityId(std::vector<std::byte> key, uint64_t entityId) {
			if (entityId == 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "relocatable index entry key requires a nonzero Entity ID");
			}
			requireCapacity(key.size(), ENTITY_ID_BYTES);
			appendBigEndianU64(key, entityId);
			return key;
		}

		[[nodiscard]] std::vector<std::byte> buildValue(const PropertyValue &value) {
			std::vector<std::byte> bytes;
			bytes.reserve(1U + sizeof(uint64_t));
			appendValue(bytes, value);
			return bytes;
		}

		[[nodiscard]] std::vector<std::byte> buildComposite(std::span<const PropertyValue> values) {
			if (values.empty() || values.size() > RSS_INDEX_KEY_MAX_COMPONENTS) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable composite index key requires a bounded nonempty component sequence");
			}
			std::vector<std::byte> bytes;
			for (const PropertyValue &value: values) {
				appendValue(bytes, value);
			}
			return bytes;
		}

	} // namespace

	std::vector<std::byte> IndexKeyCodec::encodeValue(const PropertyValue &value) {
		return requireRepresentableIndexKey([&] { return buildValue(value); });
	}

	std::vector<std::byte> IndexKeyCodec::encodeComposite(std::span<const PropertyValue> values) {
		return requireRepresentableIndexKey([&] { return buildComposite(values); });
	}

	std::vector<std::byte> IndexKeyCodec::encodeEntryKey(const PropertyValue &value, uint64_t entityId) {
		return requireRepresentableIndexKey([&] { return appendEntityId(buildValue(value), entityId); });
	}

	std::vector<std::byte> IndexKeyCodec::encodeCompositeEntryKey(std::span<const PropertyValue> values,
																				 uint64_t entityId) {
		return requireRepresentableIndexKey([&] { return appendEntityId(buildComposite(values), entityId); });
	}

	std::optional<std::vector<std::byte>> IndexKeyCodec::tryEncodeEntryKey(const PropertyValue &value,
																					uint64_t entityId) {
		return tryBuildIndexKey([&] { return appendEntityId(buildValue(value), entityId); });
	}

	std::optional<std::vector<std::byte>>
	IndexKeyCodec::tryEncodeCompositeEntryKey(std::span<const PropertyValue> values, uint64_t entityId) {
		return tryBuildIndexKey([&] { return appendEntityId(buildComposite(values), entityId); });
	}

	std::vector<std::byte> IndexKeyCodec::lowerBound(const PropertyValue &value) {
		return requireRepresentableIndexKey([&] { return appendEntityId(buildValue(value), 1U); });
	}

	std::vector<std::byte> IndexKeyCodec::upperBound(const PropertyValue &value) {
		return requireRepresentableIndexKey([&] {
			std::vector<std::byte> key = buildValue(value);
			requireCapacity(key.size(), ENTITY_ID_BYTES);
			appendBigEndianU64(key, std::numeric_limits<uint64_t>::max());
			return key;
		});
	}

	uint64_t IndexKeyCodec::decodeEntryEntityId(std::span<const std::byte> key) {
		if (key.size() <= ENTITY_ID_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL,
				 "relocatable index entry key has no value prefix before its Entity ID suffix");
		}
		const uint64_t entityId = readBigEndianU64(key, key.size() - ENTITY_ID_BYTES);
		if (entityId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable index entry key has a zero Entity ID suffix");
		}
		return entityId;
	}

} // namespace graph::storage
