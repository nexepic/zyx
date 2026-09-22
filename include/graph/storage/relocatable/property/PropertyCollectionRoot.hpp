/**
 * @file PropertyCollectionRoot.hpp
 * @brief Tokenized Property collection roots persisted in relocatable slotted pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/property/LargeValue.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_ENTITY_PROPERTY_COLLECTION_ROOT_MAGIC = 0x4552595AU; // Little-endian bytes: ZYRE
	constexpr uint16_t RSS_ENTITY_PROPERTY_COLLECTION_ROOT_VERSION = 2;
	constexpr uint16_t RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES = 32;
	constexpr uint32_t RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_MAGIC = 0x5050595AU; // Little-endian bytes: ZYPP
	constexpr uint16_t RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_VERSION = 2;
	constexpr uint16_t RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES = 16;
	constexpr uint32_t RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_MAGIC = 0x5650595AU; // Little-endian bytes: ZYPV
	constexpr uint16_t RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_VERSION = 1;
	constexpr uint16_t RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES = 40;
	constexpr const char RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE[] = "application/vnd.zyx.entity-property-root";

	struct PropertyCollectionRootEntry {
		uint32_t key_token = 0;
		PropertyValue value;

		bool operator==(const PropertyCollectionRootEntry &) const = default;
	};

	// A root records one owner's tokenized Property collection. Its keys are
	// database-local Property tokens, never unbounded string copies.
	struct PropertyCollectionRoot {
		uint64_t owner_entity_id = 0;
		uint32_t owner_entity_type = 0;
		std::vector<PropertyCollectionRootEntry> entries;

		bool operator==(const PropertyCollectionRoot &) const = default;
	};

	// The owner-keyed Property Directory is the sole durable identity. The codec
	// records no redundant Property ID per Node or Edge collection root.
	class EntityPropertyCollectionRootCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const PropertyCollectionRoot &root);
		[[nodiscard]] static PropertyCollectionRoot decode(std::span<const std::byte> bytes);
		// Decodes and validates only the fixed owner header. The returned root has
		// no entries and is intended for storage-side ownership authentication.
		[[nodiscard]] static PropertyCollectionRoot decodeOwner(std::span<const std::byte> bytes);
		// Decodes at most one requested token through the canonical offset
		// directory. The containing slotted or LargeValue page authenticates the
		// complete payload before this projection is opened.
		[[nodiscard]] static std::optional<PropertyCollectionRootEntry>
		decodeSelected(std::span<const std::byte> bytes, uint32_t keyToken);
		// Checks token presence without decoding the Property payload. This is used
		// by cardinality-only operators such as COUNT(property).
		[[nodiscard]] static bool containsToken(std::span<const std::byte> bytes, uint32_t keyToken);
		// Decodes several strictly increasing Property tokens from one authenticated
		// owner payload. The result is token-major and preserves one nullopt per
		// absent token, so callers can share header and offset-directory validation.
		[[nodiscard]] static std::vector<std::optional<PropertyCollectionRootEntry>>
		decodeSelected(std::span<const std::byte> bytes, std::span<const uint32_t> keyTokens);
		// Validates the payload directory once and returns both its entry count and
		// the requested values. Index candidate verification needs both facts; the
		// combined contract avoids decoding the same offset directory twice.
		[[nodiscard]] static std::pair<size_t, std::vector<std::optional<PropertyCollectionRootEntry>>>
		decodeSelectedAndCount(std::span<const std::byte> bytes, std::span<const uint32_t> keyTokens);
		[[nodiscard]] static size_t entryCount(std::span<const std::byte> bytes);
	};

	// LargeValue contents deliberately omit the owner: the compact reference in
	// the Property page remains the authoritative owner binding, allowing equal
	// immutable Property payloads to share one content-index entry.
	class EntityPropertyLargeValuePayloadCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(std::span<const PropertyCollectionRootEntry> entries);
		[[nodiscard]] static std::vector<PropertyCollectionRootEntry> decode(std::span<const std::byte> bytes);
		// Reads one token through the canonical offset directory without building
		// or decoding the complete owner-neutral collection.
		[[nodiscard]] static std::optional<PropertyCollectionRootEntry>
		decodeSelected(std::span<const std::byte> bytes, uint32_t keyToken);
		[[nodiscard]] static bool containsToken(std::span<const std::byte> bytes, uint32_t keyToken);
		[[nodiscard]] static std::vector<std::optional<PropertyCollectionRootEntry>>
		decodeSelected(std::span<const std::byte> bytes, std::span<const uint32_t> keyTokens);
		// LargeValue counterpart of the owner-keyed combined selected/count read.
		[[nodiscard]] static std::pair<size_t, std::vector<std::optional<PropertyCollectionRootEntry>>>
		decodeSelectedAndCount(std::span<const std::byte> bytes, std::span<const uint32_t> keyTokens);
		[[nodiscard]] static size_t entryCount(std::span<const std::byte> bytes);
		[[nodiscard]] static bool hasMagic(std::span<const std::byte> bytes);
	};

	// An owner-keyed Property page may hold this compact reference instead of an
	// oversized root. The embedded reference remains entirely logical: resolving
	// it never exposes a SegmentId, PhysicalSlotId, or data-frame ordinal.
	struct EntityPropertyLargeValueReference {
		uint64_t owner_entity_id = 0U;
		uint32_t owner_entity_type = 0U;
		LargeValueRef value;

		bool operator==(const EntityPropertyLargeValueReference &) const = default;
	};

	class EntityPropertyLargeValueReferenceCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const EntityPropertyLargeValueReference &reference);
		[[nodiscard]] static EntityPropertyLargeValueReference decode(std::span<const std::byte> bytes);
		[[nodiscard]] static bool hasMagic(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
