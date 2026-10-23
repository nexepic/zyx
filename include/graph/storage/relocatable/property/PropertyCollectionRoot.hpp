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
#include <limits>
#include <stdexcept>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/relocatable/property/LargeValue.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"

namespace graph::storage {

	// Called for each present selected entry before its variable-sized payload
	// is read or decoded. Bytes include the token and canonical value encoding,
	// not container capacity or decoded-object overhead. Throw to reject the
	// read; the callback and its accounting remain owned by the caller.
	using SelectedPropertyReadAdmission = std::function<void(size_t)>;

	// Adapt incremental encoded-entry charges to an absolute source-batch total.
	// Create one callback per read so released prior batches are not charged again.
	[[nodiscard]] inline SelectedPropertyReadAdmission makeSelectedPropertyBatchAdmission(
		const std::function<void(size_t)> &admission) {
		if (!admission) return {};
		return [admission, bytes = size_t{0U}](size_t next) mutable {
			if (next > (std::numeric_limits<size_t>::max)() - bytes)
				throw std::overflow_error("selected Property source batch byte count overflowed");
			bytes += next;
			admission(bytes);
		};
	}


	constexpr uint32_t RSS_ENTITY_PROPERTY_COLLECTION_ROOT_MAGIC = 0x4552595AU; // Little-endian bytes: ZYRE
	constexpr uint16_t RSS_ENTITY_PROPERTY_COLLECTION_ROOT_VERSION = 2;
	constexpr uint16_t RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES = 32;
	constexpr uint32_t RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_MAGIC = 0x5050595AU; // Little-endian bytes: ZYPP
	// Version 3 stores token and offset together in the LargeValue directory.
	// This lets selected reads resolve tokens from the directory without loading
	// the complete variable-sized payload.
	constexpr uint16_t RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_VERSION = 3;
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

    // A synchronous encoding view over authenticated, immutable source values.
    // The source values and supplied output account must outlive encoding.
    struct BorrowedPropertyCollectionRootEntry {
        uint32_t key_token = 0;
        const PropertyValue *value = nullptr;
    };

	// A root records one owner's tokenized Property collection. Its keys are
	// database-local Property tokens, never unbounded string copies.
	struct PropertyCollectionRoot {
		uint64_t owner_entity_id = 0;
		uint32_t owner_entity_type = 0;
		std::vector<PropertyCollectionRootEntry> entries;

		bool operator==(const PropertyCollectionRoot &) const = default;
	};

	struct EntityPropertyLargeValueEntryLocation {
		uint32_t key_token = 0U;
		uint32_t offset = 0U;
		uint32_t length = 0U;

		bool operator==(const EntityPropertyLargeValueEntryLocation &) const = default;
	};

	// The owner-keyed Property Directory is the sole durable identity. The codec
	// records no redundant Property ID per Node or Edge collection root.
	// Full decode validates the directory/tokens, then admits every encoded
	// entry (token plus value) before constructing decoded Property payloads.
	// The callback does not cover input bytes or directory/entry-slot capacity.
	class EntityPropertyCollectionRootCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const PropertyCollectionRoot &root);
        [[nodiscard]] static size_t encodedSizeBorrowed(uint64_t owner, uint32_t ownerType,
            std::span<const BorrowedPropertyCollectionRootEntry> entries, const std::function<void()> &checkpoint = {});
        [[nodiscard]] static std::pmr::vector<std::byte> encodeBorrowed(uint64_t owner, uint32_t ownerType,
            std::span<const BorrowedPropertyCollectionRootEntry> entries, std::pmr::memory_resource &memory,
            const std::function<void()> &checkpoint = {});
		[[nodiscard]] static PropertyCollectionRoot decode(std::span<const std::byte> bytes,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const SelectedPropertyReadAdmission *slotsAdmission = nullptr);
		// Decodes and validates only the fixed owner header. The returned root has
		// no entries and is intended for storage-side ownership authentication.
		[[nodiscard]] static PropertyCollectionRoot decodeOwner(std::span<const std::byte> bytes);
        using EncodedSelectionSink = std::function<void(size_t, std::optional<std::span<const std::byte>>)>;
        // Validates the complete owner/directory, then borrows selected value
        // ranges synchronously. Missing cells are reported as nullopt.
        static void visitSelectedEncoded(std::span<const std::byte> bytes,
            uint64_t expectedOwnerId, uint32_t expectedOwnerType, std::span<const uint32_t> tokens,
            const EncodedSelectionSink &sink, const SelectedPropertyReadAdmission *admission = nullptr,
            const CancellationCheckpoint *cancellation = nullptr);
		// Decodes at most one requested token through the canonical offset
		// directory. The containing slotted or LargeValue page authenticates the
		// complete payload before this projection is opened.
		[[nodiscard]] static std::optional<PropertyCollectionRootEntry>
		decodeSelected(std::span<const std::byte> bytes, uint32_t keyToken);
		// Checks token presence without decoding the Property payload. This is used
		// for key existence; requireNonNull also checks the value header for COUNT.
		[[nodiscard]] static bool containsToken(std::span<const std::byte> bytes, uint32_t keyToken, bool requireNonNull = false);
		// Decodes several strictly increasing Property tokens from one authenticated
		// owner payload. The result is token-major and preserves one nullopt per
		// absent token, so callers can share header and offset-directory validation.
		[[nodiscard]] static std::vector<std::optional<PropertyCollectionRootEntry>>
		decodeSelected(std::span<const std::byte> bytes, std::span<const uint32_t> keyTokens,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const SelectedPropertyReadAdmission *slotsAdmission = nullptr);
		// Validates the payload directory once and returns both its entry count and
		// the requested values. Index candidate verification needs both facts; the
		// combined contract avoids decoding the same offset directory twice.
		[[nodiscard]] static std::pair<size_t, std::vector<std::optional<PropertyCollectionRootEntry>>>
		decodeSelectedAndCount(std::span<const std::byte> bytes, std::span<const uint32_t> keyTokens,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const SelectedPropertyReadAdmission *slotsAdmission = nullptr);
		[[nodiscard]] static size_t entryCount(std::span<const std::byte> bytes);
		// Validates the owner and complete offset/token directory without decoding
		// values. The caller authenticates containing pages; this is not a value
		// integrity scan. Capacity is admitted before allocating the token vector.
		// Reports absolute requested bytes, then actual vector capacity bytes;
		// callbacks must replace their prior charge rather than accumulate it.
		[[nodiscard]] static std::vector<uint32_t> decodeTokens(std::span<const std::byte> bytes,
				const SelectedPropertyReadAdmission *slotsAdmission = nullptr,
				const CancellationCheckpoint *cancellation = nullptr);
	};

	// LargeValue contents deliberately omit the owner: the compact reference in
	// the Property page remains the authoritative owner binding, allowing equal
	// immutable Property payloads to share one content-index entry.
	class EntityPropertyLargeValuePayloadCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(std::span<const PropertyCollectionRootEntry> entries);
        [[nodiscard]] static std::pmr::vector<std::byte> encodeBorrowed(
            std::span<const BorrowedPropertyCollectionRootEntry> entries, std::pmr::memory_resource &memory,
            const std::function<void()> &checkpoint = {});
        // Identical version-3 owner-neutral bytes without whole-payload standard
        // vector copies. Entries must already be strictly ordered; canonical
        // nested-map workspace and output use the supplied resource.
        [[nodiscard]] static std::pmr::vector<std::byte> encode(std::span<const PropertyCollectionRootEntry> entries,
            std::pmr::memory_resource &memory, const std::function<void()> &checkpoint = {});
        // Identical version-3 wire bytes for one field, with all output and
        // canonical sorting workspace on the supplied resource. That resource
        // must outlive the returned PMR vector.
        [[nodiscard]] static std::pmr::vector<std::byte> encodeSingleField(uint32_t token,
            const PropertyValue &value, std::pmr::memory_resource &memory);
		[[nodiscard]] static std::vector<PropertyCollectionRootEntry> decode(std::span<const std::byte> bytes,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const SelectedPropertyReadAdmission *slotsAdmission = nullptr);
		// Validates the header and token/offset directory. The input may contain
		// only the header plus directory; entry payload bytes are not required.
		[[nodiscard]] static std::vector<EntityPropertyLargeValueEntryLocation>
		decodeDirectory(std::span<const std::byte> headerAndDirectory,
				const SelectedPropertyReadAdmission *slotsAdmission = nullptr,
				const CancellationCheckpoint *cancellation = nullptr);
		// Decodes one complete entry range returned by decodeDirectory.
		[[nodiscard]] static PropertyCollectionRootEntry
		decodeEntry(std::span<const std::byte> bytes, uint32_t expectedToken);
		// Reads one token through the canonical offset directory without building
		// or decoding the complete owner-neutral collection.
		[[nodiscard]] static std::optional<PropertyCollectionRootEntry>
		decodeSelected(std::span<const std::byte> bytes, uint32_t keyToken);
		[[nodiscard]] static bool containsToken(std::span<const std::byte> bytes, uint32_t keyToken);
		[[nodiscard]] static std::vector<std::optional<PropertyCollectionRootEntry>>
		decodeSelected(std::span<const std::byte> bytes, std::span<const uint32_t> keyTokens,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const SelectedPropertyReadAdmission *slotsAdmission = nullptr);
		// LargeValue counterpart of the owner-keyed combined selected/count read.
		[[nodiscard]] static std::pair<size_t, std::vector<std::optional<PropertyCollectionRootEntry>>>
		decodeSelectedAndCount(std::span<const std::byte> bytes, std::span<const uint32_t> keyTokens,
				const SelectedPropertyReadAdmission *admission = nullptr,
				const SelectedPropertyReadAdmission *slotsAdmission = nullptr);
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
