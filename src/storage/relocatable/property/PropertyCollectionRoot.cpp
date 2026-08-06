/**
 * @file PropertyCollectionRoot.cpp
 * @brief Explicit codec for tokenized relocatable Property collection roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t ENTITY_OWNER_ID_OFFSET = 8;
		constexpr size_t ENTITY_OWNER_TYPE_OFFSET = 16;
		constexpr size_t ENTITY_ENTRY_COUNT_OFFSET = 20;
		constexpr size_t ENTITY_PAYLOAD_BYTES_OFFSET = 24;
		constexpr size_t ENTITY_RESERVED_OFFSET = 28;
		constexpr size_t LARGE_VALUE_FLAGS_OFFSET = 8;
		constexpr size_t LARGE_VALUE_OWNER_ID_OFFSET = 12;
		constexpr size_t LARGE_VALUE_OWNER_TYPE_OFFSET = 20;
		constexpr size_t LARGE_VALUE_REFERENCE_BYTES_OFFSET = 24;
		constexpr size_t LARGE_VALUE_RESERVED_OFFSET = 26;
		constexpr size_t LARGE_VALUE_PAYLOAD_CRC_OFFSET = 28;
		constexpr size_t LARGE_VALUE_HEADER_CRC_OFFSET = 32;
		constexpr size_t LARGE_VALUE_TRAILING_RESERVED_OFFSET = 36;

		[[noreturn]] void throwRootError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] bool isOwnerType(uint32_t type) { return type == Node::typeId || type == Edge::typeId; }

		void requireU32Size(size_t value, const char *message) {
			if (value > std::numeric_limits<uint32_t>::max()) { // ZYX_COV_EXCL_LINE: an in-memory value cannot
																// reach this 4 GiB wire limit in tests.
				throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
		}

		void validateOwnerAndEntries(const PropertyCollectionRoot &root) {
			if (root.owner_entity_id == 0U || !isOwnerType(root.owner_entity_type)) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable Property collection root has an invalid owner type or owner identity");
			}
			uint32_t previous_token = 0;
			for (const PropertyCollectionRootEntry &entry: root.entries) {
				if (entry.key_token == 0U || entry.key_token <= previous_token) {
					throwRootError(FormatError::FERR_INVALID_HEADER,
								   "relocatable Property collection root tokens are not strictly ordered");
				}
				previous_token = entry.key_token;
			}
		}

		void appendU32(std::vector<std::byte> &bytes, uint32_t value) {
			const size_t offset = bytes.size();
			bytes.resize(offset + sizeof(value));
			LittleEndianCodec::writeU32(bytes, offset, value);
		}

		[[nodiscard]] std::vector<std::byte> encodePayload(const PropertyCollectionRoot &root) {
			requireU32Size(root.entries.size(), "relocatable Property collection root has too many entries");
			std::vector<std::byte> payload;
			for (const PropertyCollectionRootEntry &entry: root.entries) {
				appendU32(payload, entry.key_token);
				CanonicalPropertyValueCodec::append(payload, entry.value);
			}
			requireU32Size(payload.size(), "relocatable Property collection root payload is too large");
			return payload;
		}

		void decodeEntries(PropertyCollectionRoot &root, std::span<const std::byte> payload, uint32_t entryCount) {
			if (entryCount > payload.size() / 10U) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable Property collection root entry count exceeds its payload");
			}
			root.entries.reserve(entryCount);
			size_t offset = 0;
		uint32_t previousToken = 0;
		for (uint32_t index = 0; index < entryCount; ++index) {
			const uint32_t token = LittleEndianCodec::readU32(payload, offset);
				offset += sizeof(token);
				if (token == 0U || token <= previousToken) {
					throwRootError(FormatError::FERR_INVALID_HEADER,
								   "relocatable Property collection root tokens are not strictly ordered");
				}
				root.entries.push_back({token, CanonicalPropertyValueCodec::decodeNext(payload, offset)});
				previousToken = token;
			}
			if (offset != payload.size()) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable Property collection root payload has trailing bytes");
			}
		}

		void validateLargeValueReference(const EntityPropertyLargeValueReference &reference) {
			if (reference.owner_entity_id == 0U || !isOwnerType(reference.owner_entity_type)) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable Entity Property LargeValue reference has an invalid owner");
			}
			// Reuse the canonical LargeValue codec validation, including content hash,
			// enum, media-type, reserved-byte, and length checks.
			static_cast<void>(LargeValueRefCodec::encode(reference.value));
		}

	} // namespace

	std::vector<std::byte> EntityPropertyCollectionRootCodec::encode(const PropertyCollectionRoot &root) {
		validateOwnerAndEntries(root);
		const std::vector<std::byte> payload = encodePayload(root);
		std::vector<std::byte> bytes(RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, 0, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_MAGIC);
		LittleEndianCodec::writeU16(bytes, 4, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_VERSION);
		LittleEndianCodec::writeU16(bytes, 6, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES);
		LittleEndianCodec::writeU64(bytes, ENTITY_OWNER_ID_OFFSET, root.owner_entity_id);
		LittleEndianCodec::writeU32(bytes, ENTITY_OWNER_TYPE_OFFSET, root.owner_entity_type);
		LittleEndianCodec::writeU32(bytes, ENTITY_ENTRY_COUNT_OFFSET, static_cast<uint32_t>(root.entries.size()));
		LittleEndianCodec::writeU32(bytes, ENTITY_PAYLOAD_BYTES_OFFSET, static_cast<uint32_t>(payload.size()));
		LittleEndianCodec::writeU32(bytes, ENTITY_RESERVED_OFFSET, 0U);
		bytes.insert(bytes.end(), payload.begin(), payload.end());
		return bytes;
	}

	PropertyCollectionRoot EntityPropertyCollectionRootCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES) {
			throwRootError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable Entity Property root is truncated");
		}
		if (LittleEndianCodec::readU32(bytes, 0) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_MAGIC) {
			throwRootError(FormatError::FERR_INVALID_MAGIC, "relocatable Entity Property root magic does not match");
		}
		if (LittleEndianCodec::readU16(bytes, 4) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_VERSION) {
			throwRootError(FormatError::FERR_UNSUPPORTED_VERSION,
						   "relocatable Entity Property root version is unsupported");
		}
		if (LittleEndianCodec::readU16(bytes, 6) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES ||
			LittleEndianCodec::readU32(bytes, ENTITY_RESERVED_OFFSET) != 0U) {
			throwRootError(FormatError::FERR_RESERVED_BITS,
						   "relocatable Entity Property root has invalid fixed or reserved fields");
		}
		const uint32_t payloadBytes = LittleEndianCodec::readU32(bytes, ENTITY_PAYLOAD_BYTES_OFFSET);
		if (payloadBytes != bytes.size() - RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES) {
			throwRootError(FormatError::FERR_INVALID_HEADER,
						   "relocatable Entity Property root payload length does not match its bytes");
		}
		PropertyCollectionRoot root;
		root.owner_entity_id = LittleEndianCodec::readU64(bytes, ENTITY_OWNER_ID_OFFSET);
		root.owner_entity_type = LittleEndianCodec::readU32(bytes, ENTITY_OWNER_TYPE_OFFSET);
		decodeEntries(root, bytes.subspan(RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES),
					  LittleEndianCodec::readU32(bytes, ENTITY_ENTRY_COUNT_OFFSET));
		validateOwnerAndEntries(root);
		return root;
	}

	std::vector<std::byte>
	EntityPropertyLargeValueReferenceCodec::encode(const EntityPropertyLargeValueReference &reference) {
		validateLargeValueReference(reference);
		const std::vector<std::byte> encodedReference = LargeValueRefCodec::encode(reference.value);
		if (encodedReference.size() >
			std::numeric_limits<uint16_t>::max()) { // ZYX_COV_EXCL_LINE: LargeValue codec bound.
			throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW,
						   "relocatable Entity Property LargeValue reference exceeds its wire length field");
		}
		std::vector<std::byte> bytes(RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES + encodedReference.size());
		LittleEndianCodec::writeU32(bytes, 0U, RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_MAGIC);
		LittleEndianCodec::writeU16(bytes, 4U, RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_VERSION);
		LittleEndianCodec::writeU16(bytes, 6U, RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, LARGE_VALUE_FLAGS_OFFSET, 0U);
		LittleEndianCodec::writeU64(bytes, LARGE_VALUE_OWNER_ID_OFFSET, reference.owner_entity_id);
		LittleEndianCodec::writeU32(bytes, LARGE_VALUE_OWNER_TYPE_OFFSET, reference.owner_entity_type);
		LittleEndianCodec::writeU16(bytes, LARGE_VALUE_REFERENCE_BYTES_OFFSET,
									static_cast<uint16_t>(encodedReference.size()));
		LittleEndianCodec::writeU16(bytes, LARGE_VALUE_RESERVED_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, LARGE_VALUE_PAYLOAD_CRC_OFFSET,
									utils::calculateCrc(encodedReference.data(), encodedReference.size()));
		LittleEndianCodec::writeU32(bytes, LARGE_VALUE_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, LARGE_VALUE_TRAILING_RESERVED_OFFSET, 0U);
		std::copy(encodedReference.begin(), encodedReference.end(),
				  bytes.begin() + RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES);
		LittleEndianCodec::writeU32(
				bytes, LARGE_VALUE_HEADER_CRC_OFFSET,
				utils::calculateCrc(bytes.data(), RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES));
		return bytes;
	}

	EntityPropertyLargeValueReference EntityPropertyLargeValueReferenceCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES) {
			throwRootError(FormatError::FERR_BUFFER_TOO_SMALL, "Entity Property LargeValue reference is truncated");
		}
		std::array<std::byte, RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, LARGE_VALUE_HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, LARGE_VALUE_HEADER_CRC_OFFSET, 0U);
		if (utils::calculateCrc(header.data(), header.size()) != expectedHeaderCrc) {
			throwRootError(FormatError::FERR_CHECKSUM_MISMATCH,
						   "Entity Property LargeValue reference header checksum does not match");
		}
		const uint16_t referenceBytes = LittleEndianCodec::readU16(bytes, LARGE_VALUE_REFERENCE_BYTES_OFFSET);
		const uint8_t invalid =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, 0U) !=
									 RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, 4U) !=
									 RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, 6U) !=
									 RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, LARGE_VALUE_FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, LARGE_VALUE_RESERVED_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, LARGE_VALUE_TRAILING_RESERVED_OFFSET) != 0U) |
				static_cast<uint8_t>(bytes.size() !=
									 RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES + referenceBytes);
		if (invalid != 0U) {
			throwRootError(FormatError::FERR_INVALID_HEADER, "Entity Property LargeValue reference header is invalid");
		}
		const std::span<const std::byte> encodedReference =
				bytes.subspan(RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES, referenceBytes);
		if (utils::calculateCrc(encodedReference.data(), encodedReference.size()) !=
			LittleEndianCodec::readU32(bytes, LARGE_VALUE_PAYLOAD_CRC_OFFSET)) {
			throwRootError(FormatError::FERR_CHECKSUM_MISMATCH,
						   "Entity Property LargeValue reference payload checksum does not match");
		}
		EntityPropertyLargeValueReference reference;
		reference.owner_entity_id = LittleEndianCodec::readU64(bytes, LARGE_VALUE_OWNER_ID_OFFSET);
		reference.owner_entity_type = LittleEndianCodec::readU32(bytes, LARGE_VALUE_OWNER_TYPE_OFFSET);
		reference.value = LargeValueRefCodec::decode(encodedReference);
		validateLargeValueReference(reference);
		return reference;
	}

	bool EntityPropertyLargeValueReferenceCodec::hasMagic(std::span<const std::byte> bytes) {
		return bytes.size() >= sizeof(uint32_t) &&
			   LittleEndianCodec::readU32(bytes, 0U) == RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_MAGIC;
	}

} // namespace graph::storage
