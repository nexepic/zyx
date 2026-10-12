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
		constexpr size_t LARGE_VALUE_PAYLOAD_ENTRY_COUNT_OFFSET = 8;
		constexpr size_t LARGE_VALUE_PAYLOAD_BYTES_OFFSET = 12;
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

		[[nodiscard]] std::vector<std::byte> encodeEntries(std::span<const PropertyCollectionRootEntry> entries) {
			requireU32Size(entries.size(), "relocatable Property collection root has too many entries");
			if (entries.size() > std::numeric_limits<size_t>::max() / sizeof(uint32_t)) {
				throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW,
							   "relocatable Property collection root offset directory is too large");
			}
			const size_t directoryBytes = entries.size() * sizeof(uint32_t);
			std::vector<std::byte> entryBytes;
			std::vector<uint32_t> offsets;
			offsets.reserve(entries.size());
			uint32_t previousToken = 0U;
			for (const PropertyCollectionRootEntry &entry: entries) {
				if (entry.key_token == 0U || entry.key_token <= previousToken) {
					throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable Property collection root tokens are not strictly ordered");
				}
				const size_t entryOffset = directoryBytes + entryBytes.size();
				requireU32Size(entryOffset, "relocatable Property collection root entry offset is too large");
				offsets.push_back(static_cast<uint32_t>(entryOffset));
				appendU32(entryBytes, entry.key_token);
				CanonicalPropertyValueCodec::append(entryBytes, entry.value);
				previousToken = entry.key_token;
			}
			std::vector<std::byte> payload(directoryBytes);
			for (size_t index = 0U; index < offsets.size(); ++index) {
				LittleEndianCodec::writeU32(payload, index * sizeof(uint32_t), offsets[index]);
			}
			payload.insert(payload.end(), entryBytes.begin(), entryBytes.end());
			requireU32Size(payload.size(), "relocatable Property collection root payload is too large");
			return payload;
		}

		[[nodiscard]] std::vector<std::byte>
		encodeLargeValueEntries(std::span<const PropertyCollectionRootEntry> entries) {
			requireU32Size(entries.size(), "relocatable LargeValue Property root has too many entries");
		if (entries.size() > std::numeric_limits<size_t>::max() / (sizeof(uint32_t) * 2U)) {
			throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW,
						   "relocatable LargeValue Property root directory is too large");
		}
		const size_t directoryBytes = entries.size() * sizeof(uint32_t) * 2U;
		std::vector<std::byte> entryBytes;
		std::vector<EntityPropertyLargeValueEntryLocation> locations;
		locations.reserve(entries.size());
		uint32_t previousToken = 0U;
		for (const PropertyCollectionRootEntry &entry: entries) {
			if (entry.key_token == 0U || entry.key_token <= previousToken) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable LargeValue Property root tokens are not strictly ordered");
			}
			const size_t entryOffset = directoryBytes + entryBytes.size();
			requireU32Size(entryOffset, "relocatable LargeValue Property root entry offset is too large");
			const size_t before = entryBytes.size();
			appendU32(entryBytes, entry.key_token);
			CanonicalPropertyValueCodec::append(entryBytes, entry.value);
			const size_t entryLength = entryBytes.size() - before;
			requireU32Size(entryLength, "relocatable LargeValue Property root entry is too large");
			locations.push_back({entry.key_token, static_cast<uint32_t>(entryOffset),
									static_cast<uint32_t>(entryLength)});
			previousToken = entry.key_token;
		}
		std::vector<std::byte> payload(directoryBytes);
		for (size_t index = 0U; index < locations.size(); ++index) {
			const size_t offset = index * sizeof(uint32_t) * 2U;
			LittleEndianCodec::writeU32(payload, offset, locations[index].key_token);
			LittleEndianCodec::writeU32(payload, offset + sizeof(uint32_t), locations[index].offset);
		}
		payload.insert(payload.end(), entryBytes.begin(), entryBytes.end());
		requireU32Size(payload.size(), "relocatable LargeValue Property root payload is too large");
		return payload;
		}

		[[nodiscard]] size_t largeValueDirectoryBytes(uint32_t entryCount) {
		if constexpr (sizeof(size_t) < sizeof(uint64_t)) {
			if (entryCount > std::numeric_limits<size_t>::max() / (sizeof(uint32_t) * 2U)) {
				throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW,
							   "relocatable LargeValue Property root directory is too large");
			}
		}
			return static_cast<size_t>(entryCount) * sizeof(uint32_t) * 2U;
		}

		[[nodiscard]] std::vector<EntityPropertyLargeValueEntryLocation>
		decodeLargeValueDirectory(std::span<const std::byte> bytes, const SelectedPropertyReadAdmission *slotsAdmission = nullptr,
			const CancellationCheckpoint *cancellation = nullptr) {
			if (cancellation) cancellation->check();
			if (bytes.size() < RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES) {
				throwRootError(FormatError::FERR_BUFFER_TOO_SMALL,
							   "Entity Property LargeValue payload directory is truncated");
			}
			if (LittleEndianCodec::readU32(bytes, 0U) != RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_MAGIC ||
				LittleEndianCodec::readU16(bytes, 4U) != RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_VERSION ||
				LittleEndianCodec::readU16(bytes, 6U) != RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "Entity Property LargeValue payload directory header is invalid");
			}
			const uint32_t entryCount = LittleEndianCodec::readU32(bytes, LARGE_VALUE_PAYLOAD_ENTRY_COUNT_OFFSET);
			const size_t directoryBytes = largeValueDirectoryBytes(entryCount);
			if (LittleEndianCodec::readU32(bytes, LARGE_VALUE_PAYLOAD_BYTES_OFFSET) < directoryBytes ||
				bytes.size() < RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + directoryBytes) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "Entity Property LargeValue payload directory is truncated");
			}
			const uint32_t payloadBytes = LittleEndianCodec::readU32(bytes, LARGE_VALUE_PAYLOAD_BYTES_OFFSET);
			std::vector<EntityPropertyLargeValueEntryLocation> locations;
			if (slotsAdmission == nullptr) locations.reserve(entryCount);
			size_t expectedOffset = directoryBytes;
			uint32_t previousToken = 0U;
			for (uint32_t index = 0U; index < entryCount; ++index) {
				if (cancellation) cancellation->check();
				const size_t directoryOffset = RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES +
						static_cast<size_t>(index) * sizeof(uint32_t) * 2U;
				const uint32_t token = LittleEndianCodec::readU32(bytes, directoryOffset + 0U);
				const uint32_t offset = LittleEndianCodec::readU32(bytes, directoryOffset + sizeof(uint32_t));
				const uint32_t end = index + 1U == entryCount
						? payloadBytes
						: LittleEndianCodec::readU32(bytes, directoryOffset + sizeof(uint32_t) * 2U + sizeof(uint32_t));
				if (token == 0U || token <= previousToken || offset != expectedOffset || offset >= end ||
					end > payloadBytes || end - offset < sizeof(uint32_t)) {
					throwRootError(FormatError::FERR_INVALID_HEADER,
								   "Entity Property LargeValue payload directory is noncanonical");
				}
				if (slotsAdmission == nullptr) locations.push_back({token, offset, end - offset});
				expectedOffset = end;
				previousToken = token;
			}
			if (expectedOffset != payloadBytes) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "Entity Property LargeValue payload directory leaves trailing bytes");
			}
			if (slotsAdmission != nullptr) {
				if (entryCount > std::numeric_limits<size_t>::max() / sizeof(EntityPropertyLargeValueEntryLocation)) {
					throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue directory slots overflowed");
				}
				(*slotsAdmission)(entryCount * sizeof(EntityPropertyLargeValueEntryLocation));
				if (cancellation) cancellation->check();
				locations.reserve(entryCount);
				(*slotsAdmission)(locations.capacity() * sizeof(EntityPropertyLargeValueEntryLocation));
				if (cancellation) cancellation->check();
				for (uint32_t index = 0U; index < entryCount; ++index) {
					if (cancellation) cancellation->check();
					const size_t base = RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + static_cast<size_t>(index) * 8U;
					const uint32_t offset = LittleEndianCodec::readU32(bytes, base + 4U);
					const uint32_t end = index + 1U == entryCount ? payloadBytes : LittleEndianCodec::readU32(bytes, base + 12U);
					locations.push_back({LittleEndianCodec::readU32(bytes, base), offset, end - offset});
				}
			}
			return locations;
		}

		[[nodiscard]] PropertyCollectionRootEntry decodeLargeValueEntry(std::span<const std::byte> bytes,
																		uint32_t expectedToken) {
			if (bytes.size() < sizeof(uint32_t)) {
				throwRootError(FormatError::FERR_BUFFER_TOO_SMALL,
							   "Entity Property LargeValue entry is truncated");
			}
			const uint32_t token = LittleEndianCodec::readU32(bytes, 0U);
			if (token == 0U || token != expectedToken) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "Entity Property LargeValue entry token disagrees with its directory");
			}
			size_t offset = sizeof(uint32_t);
			PropertyValue value = CanonicalPropertyValueCodec::decodeNext(bytes, offset);
			if (offset != bytes.size()) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "Entity Property LargeValue entry has trailing bytes");
			}
			return {token, std::move(value)};
		}

		[[nodiscard]] size_t offsetDirectoryBytes(const std::span<const std::byte> payload,
																			  const uint32_t entryCount) {
			if (entryCount > payload.size() / sizeof(uint32_t)) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable Property collection root offset directory exceeds its payload");
			}
			return static_cast<size_t>(entryCount) * sizeof(uint32_t);
		}

		[[nodiscard]] std::pair<size_t, size_t>
		entryBounds(const std::span<const std::byte> payload, const size_t directoryBytes, const uint32_t entryCount,
					const uint32_t index) {
			if (index >= entryCount) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable Property collection root entry index is outside its directory");
			}
			const size_t start = LittleEndianCodec::readU32(payload, static_cast<size_t>(index) * sizeof(uint32_t));
			const size_t end = index + 1U == entryCount
							   ? payload.size()
							   : LittleEndianCodec::readU32(payload, static_cast<size_t>(index + 1U) * sizeof(uint32_t));
			if (start < directoryBytes || start >= end || end > payload.size() || end - start < sizeof(uint32_t)) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable Property collection root offset directory is noncanonical");
			}
			return {start, end};
		}

		void validateOffsetDirectory(const std::span<const std::byte> payload, const size_t directoryBytes,
																 const uint32_t entryCount, const CancellationCheckpoint *cancellation = nullptr) {
			size_t expectedStart = directoryBytes;
			uint32_t previousToken = 0U;
			for (uint32_t index = 0U; index < entryCount; ++index) {
				if (cancellation) cancellation->check();
				const auto [start, end] = entryBounds(payload, directoryBytes, entryCount, index);
				const uint32_t token = LittleEndianCodec::readU32(payload, start);
				if (start != expectedStart || token == 0U || token <= previousToken) {
					throwRootError(FormatError::FERR_INVALID_HEADER,
								   "relocatable Property collection root offset directory is not canonical");
				}
				expectedStart = end;
				previousToken = token;
			}
			if (expectedStart != payload.size()) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable Property collection root offset directory leaves trailing bytes");
			}
		}

		[[nodiscard]] bool containsTokenInEntries(const std::span<const std::byte> payload,
																		 const uint32_t entryCount, const uint32_t keyToken, const bool requireNonNull) {
			if (keyToken == 0U) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
									"relocatable Property selection requires a nonzero token");
			}
			const size_t directoryBytes = offsetDirectoryBytes(payload, entryCount);
			validateOffsetDirectory(payload, directoryBytes, entryCount);
			uint32_t lower = 0U;
			uint32_t upper = entryCount;
			while (lower < upper) {
				const uint32_t middle = lower + (upper - lower) / 2U;
				const auto [start, end] = entryBounds(payload, directoryBytes, entryCount, middle);
				static_cast<void>(end);
				const uint32_t token = LittleEndianCodec::readU32(payload, start);
				if (token < keyToken) {
					lower = middle + 1U;
				} else {
					upper = middle;
				}
			}
			if (lower == entryCount) {
				return false;
			}
			const auto [start, end] = entryBounds(payload, directoryBytes, entryCount, lower);
			static_cast<void>(end);
			if (LittleEndianCodec::readU32(payload, start) != keyToken) return false;
			return !requireNonNull || !CanonicalPropertyValueCodec::isNullHeader(
				payload.subspan(start + sizeof(uint32_t), end - start - sizeof(uint32_t)), end - start - sizeof(uint32_t));
		}

		void decodeEntries(PropertyCollectionRoot &root, std::span<const std::byte> payload, uint32_t entryCount,
				const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) {
			const size_t directoryBytes = offsetDirectoryBytes(payload, entryCount);
			validateOffsetDirectory(payload, directoryBytes, entryCount);
			// Authenticate the entire token sequence before a resource rejection
			// can hide a later malformed token. Admission is collection-wide and
			// precedes reserve and all canonical variable-value decoding.
			if (admission != nullptr || slotsAdmission != nullptr) {
				uint32_t validatedToken = 0U;
				for (uint32_t index = 0U; index < entryCount; ++index) {
					const auto [start, end] = entryBounds(payload, directoryBytes, entryCount, index);
					const uint32_t token = LittleEndianCodec::readU32(payload, start);
					if (token == 0U || token <= validatedToken) throwRootError(FormatError::FERR_INVALID_HEADER,
						"relocatable Property collection root tokens are not strictly ordered");
					validatedToken = token;
				}
				for (uint32_t index = 0U; index < entryCount; ++index) {
					const auto [start, end] = entryBounds(payload, directoryBytes, entryCount, index);
					if (admission != nullptr) (*admission)(end - start);
				}
			}
			if (entryCount > std::numeric_limits<size_t>::max() / sizeof(PropertyCollectionRootEntry)) {
				throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW, "Property root slot size overflowed");
			}
			if (slotsAdmission != nullptr) (*slotsAdmission)(entryCount * sizeof(PropertyCollectionRootEntry));
			root.entries.reserve(entryCount);
			if (slotsAdmission != nullptr) (*slotsAdmission)(root.entries.capacity() * sizeof(PropertyCollectionRootEntry));
			uint32_t previousToken = 0U;
			for (uint32_t index = 0U; index < entryCount; ++index) {
				const auto [start, end] = entryBounds(payload, directoryBytes, entryCount, index);
				size_t offset = start;
				const uint32_t token = LittleEndianCodec::readU32(payload, offset);
				offset += sizeof(token);
				if (token == 0U || token <= previousToken) {
					throwRootError(FormatError::FERR_INVALID_HEADER,
								   "relocatable Property collection root tokens are not strictly ordered");
				}
				root.entries.push_back({token, CanonicalPropertyValueCodec::decodeNext(payload, offset)});
				if (offset != end) {
					throwRootError(FormatError::FERR_INVALID_HEADER,
								   "relocatable Property collection root entry length disagrees with its offset directory");
				}
				previousToken = token;
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
		const std::vector<std::byte> payload = encodeEntries(root.entries);
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

	[[nodiscard]] std::vector<std::optional<PropertyCollectionRootEntry>>
	decodeSelectedEntriesValidated(const std::span<const std::byte> payload, const size_t directoryBytes,
										 const uint32_t entryCount, const std::span<const uint32_t> requestedTokens,
				const SelectedPropertyReadAdmission *admission = nullptr, const SelectedPropertyReadAdmission *slotsAdmission = nullptr) {
		if (requestedTokens.size() > std::numeric_limits<size_t>::max() / sizeof(std::optional<PropertyCollectionRootEntry>))
			throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW, "inline selected slots overflowed");
		if (slotsAdmission) (*slotsAdmission)(requestedTokens.size() * sizeof(std::optional<PropertyCollectionRootEntry>));
		std::vector<std::optional<PropertyCollectionRootEntry>> result(requestedTokens.size());
		if (slotsAdmission) (*slotsAdmission)(result.capacity() * sizeof(std::optional<PropertyCollectionRootEntry>));
		uint32_t previousToken = 0U;
		for (size_t requestedIndex = 0U; requestedIndex < requestedTokens.size(); ++requestedIndex) {
			const uint32_t requestedToken = requestedTokens[requestedIndex];
			if (requestedToken == 0U || requestedToken <= previousToken) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable Property selection requires strictly increasing nonzero tokens");
			}
			previousToken = requestedToken;
			uint32_t lower = 0U;
			uint32_t upper = entryCount;
			while (lower < upper) {
				const uint32_t middle = lower + (upper - lower) / 2U;
				const auto [start, end] = entryBounds(payload, directoryBytes, entryCount, middle);
				static_cast<void>(end);
				const uint32_t token = LittleEndianCodec::readU32(payload, start);
				if (token == 0U) {
					throwRootError(FormatError::FERR_INVALID_HEADER,
								   "relocatable Property collection root offset directory references an invalid token");
				}
				if (token < requestedToken) {
					lower = middle + 1U;
				} else {
					upper = middle;
				}
			}
			if (lower == entryCount) {
				continue;
			}
			const auto [start, end] = entryBounds(payload, directoryBytes, entryCount, lower);
			const uint32_t token = LittleEndianCodec::readU32(payload, start);
			if (token != requestedToken) {
				continue;
			}
			if (admission != nullptr) (*admission)(end - start);
			size_t offset = start + sizeof(uint32_t);
			PropertyValue value = CanonicalPropertyValueCodec::decodeNext(payload, offset);
			if (offset != end) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable Property collection root selected entry length is noncanonical");
			}
			result[requestedIndex] = PropertyCollectionRootEntry{token, std::move(value)};
		}
		return result;
	}

	[[nodiscard]] std::vector<std::optional<PropertyCollectionRootEntry>>
	decodeSelectedEntries(const std::span<const std::byte> payload, const uint32_t entryCount,
										 const std::span<const uint32_t> requestedTokens,
				const SelectedPropertyReadAdmission *admission = nullptr, const SelectedPropertyReadAdmission *slotsAdmission = nullptr) {
		const size_t directoryBytes = offsetDirectoryBytes(payload, entryCount);
		validateOffsetDirectory(payload, directoryBytes, entryCount);
		return decodeSelectedEntriesValidated(payload, directoryBytes, entryCount, requestedTokens, admission, slotsAdmission);
	}

	PropertyCollectionRoot EntityPropertyCollectionRootCodec::decode(std::span<const std::byte> bytes,
			const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) {
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
		if (LittleEndianCodec::readU64(bytes, ENTITY_OWNER_ID_OFFSET) == 0U ||
			!isOwnerType(LittleEndianCodec::readU32(bytes, ENTITY_OWNER_TYPE_OFFSET))) {
			throwRootError(FormatError::FERR_INVALID_HEADER,
						   "relocatable Entity Property root has an invalid owner");
		}
		PropertyCollectionRoot root;
		root.owner_entity_id = LittleEndianCodec::readU64(bytes, ENTITY_OWNER_ID_OFFSET);
		root.owner_entity_type = LittleEndianCodec::readU32(bytes, ENTITY_OWNER_TYPE_OFFSET);
		decodeEntries(root, bytes.subspan(RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES),
					  LittleEndianCodec::readU32(bytes, ENTITY_ENTRY_COUNT_OFFSET), admission, slotsAdmission);
		validateOwnerAndEntries(root);
		return root;
	}

	PropertyCollectionRoot EntityPropertyCollectionRootCodec::decodeOwner(const std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES) {
			throwRootError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable Entity Property root is truncated");
		}
		const uint8_t invalid =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, 0U) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, 4U) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, 6U) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, ENTITY_RESERVED_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, ENTITY_PAYLOAD_BYTES_OFFSET) !=
									 bytes.size() - RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, ENTITY_OWNER_ID_OFFSET) == 0U) |
				static_cast<uint8_t>(!isOwnerType(LittleEndianCodec::readU32(bytes, ENTITY_OWNER_TYPE_OFFSET)));
		if (invalid != 0U) {
			throwRootError(FormatError::FERR_INVALID_HEADER, "relocatable Entity Property root header is invalid");
		}
		return {LittleEndianCodec::readU64(bytes, ENTITY_OWNER_ID_OFFSET),
				LittleEndianCodec::readU32(bytes, ENTITY_OWNER_TYPE_OFFSET), {}};
	}

	std::optional<PropertyCollectionRootEntry>
	EntityPropertyCollectionRootCodec::decodeSelected(const std::span<const std::byte> bytes, const uint32_t keyToken) {
		const std::array<uint32_t, 1U> tokens{keyToken};
		return decodeSelected(bytes, tokens).front();
	}

	bool EntityPropertyCollectionRootCodec::containsToken(const std::span<const std::byte> bytes,
																							 const uint32_t keyToken, const bool requireNonNull) {
			if (bytes.size() < RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES) {
				throwRootError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable Entity Property root is truncated");
			}
			if (LittleEndianCodec::readU32(bytes, 0U) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_MAGIC ||
				LittleEndianCodec::readU16(bytes, 4U) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_VERSION ||
				LittleEndianCodec::readU16(bytes, 6U) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES ||
				LittleEndianCodec::readU32(bytes, ENTITY_RESERVED_OFFSET) != 0U) {
				throwRootError(FormatError::FERR_INVALID_HEADER, "relocatable Entity Property root header is invalid");
			}
			const uint32_t payloadBytes = LittleEndianCodec::readU32(bytes, ENTITY_PAYLOAD_BYTES_OFFSET);
		if (payloadBytes != bytes.size() - RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES) {
			throwRootError(FormatError::FERR_INVALID_HEADER,
									"relocatable Entity Property root payload length does not match its bytes");
		}
		// The point-presence API is also a public codec boundary. Validate the
		// owner just as selected/full decode does before trusting its directory.
		static_cast<void>(decodeOwner(bytes));
		const auto payload = bytes.subspan(RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES);
		return containsTokenInEntries(payload, LittleEndianCodec::readU32(bytes, ENTITY_ENTRY_COUNT_OFFSET), keyToken, requireNonNull);
	}

	std::vector<std::optional<PropertyCollectionRootEntry>>
	EntityPropertyCollectionRootCodec::decodeSelected(const std::span<const std::byte> bytes,
																			 const std::span<const uint32_t> keyTokens,
			const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) {
		if (bytes.size() < RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES) {
			throwRootError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable Entity Property root is truncated");
		}
		if (LittleEndianCodec::readU32(bytes, 0U) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_MAGIC ||
			LittleEndianCodec::readU16(bytes, 4U) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_VERSION ||
			LittleEndianCodec::readU16(bytes, 6U) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES ||
			LittleEndianCodec::readU32(bytes, ENTITY_RESERVED_OFFSET) != 0U) {
			throwRootError(FormatError::FERR_INVALID_HEADER, "relocatable Entity Property root header is invalid");
		}
		const uint32_t payloadBytes = LittleEndianCodec::readU32(bytes, ENTITY_PAYLOAD_BYTES_OFFSET);
		if (payloadBytes != bytes.size() - RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES) {
			throwRootError(FormatError::FERR_INVALID_HEADER,
						   "relocatable Entity Property root payload length does not match its bytes");
		}
		if (LittleEndianCodec::readU64(bytes, ENTITY_OWNER_ID_OFFSET) == 0U ||
			!isOwnerType(LittleEndianCodec::readU32(bytes, ENTITY_OWNER_TYPE_OFFSET))) {
			throwRootError(FormatError::FERR_INVALID_HEADER, "relocatable Entity Property root has an invalid owner");
		}
		return decodeSelectedEntries(bytes.subspan(RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES),
																				 LittleEndianCodec::readU32(bytes, ENTITY_ENTRY_COUNT_OFFSET), keyTokens, admission, slotsAdmission);
	}

	std::pair<size_t, std::vector<std::optional<PropertyCollectionRootEntry>>>
	EntityPropertyCollectionRootCodec::decodeSelectedAndCount(const std::span<const std::byte> bytes,
																					 const std::span<const uint32_t> keyTokens, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) {
				if (bytes.size() < RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES) {
					throwRootError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable Entity Property root is truncated");
				}
				if (LittleEndianCodec::readU32(bytes, 0U) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_MAGIC ||
					LittleEndianCodec::readU16(bytes, 4U) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_VERSION ||
					LittleEndianCodec::readU16(bytes, 6U) != RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES ||
					LittleEndianCodec::readU32(bytes, ENTITY_RESERVED_OFFSET) != 0U) {
					throwRootError(FormatError::FERR_INVALID_HEADER, "relocatable Entity Property root header is invalid");
				}
				const uint32_t payloadBytes = LittleEndianCodec::readU32(bytes, ENTITY_PAYLOAD_BYTES_OFFSET);
				if (payloadBytes != bytes.size() - RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES ||
					LittleEndianCodec::readU64(bytes, ENTITY_OWNER_ID_OFFSET) == 0U ||
					!isOwnerType(LittleEndianCodec::readU32(bytes, ENTITY_OWNER_TYPE_OFFSET))) {
					throwRootError(FormatError::FERR_INVALID_HEADER,
									   "relocatable Entity Property root header or owner is invalid");
				}
				const uint32_t entryCount = LittleEndianCodec::readU32(bytes, ENTITY_ENTRY_COUNT_OFFSET);
				const std::span<const std::byte> payload = bytes.subspan(RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES);
				const size_t directoryBytes = offsetDirectoryBytes(payload, entryCount);
				validateOffsetDirectory(payload, directoryBytes, entryCount);
				return {entryCount, decodeSelectedEntriesValidated(payload, directoryBytes, entryCount, keyTokens, admission, slotsAdmission)};
	}

	std::vector<uint32_t> EntityPropertyCollectionRootCodec::decodeTokens(
		const std::span<const std::byte> bytes, const SelectedPropertyReadAdmission *slotsAdmission, const CancellationCheckpoint *cancellation) {
		if (cancellation) cancellation->check();
		static_cast<void>(decodeOwner(bytes));
		const uint32_t count = LittleEndianCodec::readU32(bytes, ENTITY_ENTRY_COUNT_OFFSET);
		const auto payload = bytes.subspan(RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES);
		const size_t directoryBytes = offsetDirectoryBytes(payload, count);
		validateOffsetDirectory(payload, directoryBytes, count, cancellation);
		// offsetDirectoryBytes has already checked count * sizeof(uint32_t).
		if (slotsAdmission != nullptr) (*slotsAdmission)(directoryBytes);
		if (cancellation) cancellation->check();
		std::vector<uint32_t> tokens;
		tokens.reserve(count);
		if (slotsAdmission != nullptr) (*slotsAdmission)(tokens.capacity() * sizeof(uint32_t));
		if (cancellation) cancellation->check();
		for (uint32_t index = 0U; index < count; ++index) {
			if (cancellation) cancellation->check();
			const auto [start, end] = entryBounds(payload, directoryBytes, count, index);
			static_cast<void>(end);
			tokens.push_back(LittleEndianCodec::readU32(payload, start));
		}
		return tokens;
	}

	size_t EntityPropertyCollectionRootCodec::entryCount(const std::span<const std::byte> bytes) {
		static_cast<void>(decodeOwner(bytes));
		const uint32_t entryCount = LittleEndianCodec::readU32(bytes, ENTITY_ENTRY_COUNT_OFFSET);
		const std::span<const std::byte> payload = bytes.subspan(RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES);
		const size_t directoryBytes = offsetDirectoryBytes(payload, entryCount);
		validateOffsetDirectory(payload, directoryBytes, entryCount);
		return entryCount;
	}

	std::vector<std::byte>
	EntityPropertyLargeValuePayloadCodec::encode(const std::span<const PropertyCollectionRootEntry> entries) {
		const std::vector<std::byte> payload = encodeLargeValueEntries(entries);
		std::vector<std::byte> bytes(RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, 0U, RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_MAGIC);
		LittleEndianCodec::writeU16(bytes, 4U, RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_VERSION);
		LittleEndianCodec::writeU16(bytes, 6U, RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, LARGE_VALUE_PAYLOAD_ENTRY_COUNT_OFFSET,
								static_cast<uint32_t>(entries.size()));
		LittleEndianCodec::writeU32(bytes, LARGE_VALUE_PAYLOAD_BYTES_OFFSET, static_cast<uint32_t>(payload.size()));
		bytes.insert(bytes.end(), payload.begin(), payload.end());
		return bytes;
	}

	std::vector<PropertyCollectionRootEntry>
	EntityPropertyLargeValuePayloadCodec::decode(const std::span<const std::byte> bytes,
			const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) {
		std::vector<PropertyCollectionRootEntry> result;
		{
			const auto locations = decodeLargeValueDirectory(bytes, slotsAdmission);
			const size_t directoryBytes = locations.capacity() * sizeof(EntityPropertyLargeValueEntryLocation);
			const uint32_t payloadBytes = LittleEndianCodec::readU32(bytes, LARGE_VALUE_PAYLOAD_BYTES_OFFSET);
			if (bytes.size() != RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + payloadBytes) {
				throwRootError(FormatError::FERR_INVALID_HEADER, "Entity Property LargeValue payload length is invalid");
			}
			if (admission != nullptr) for (const auto &location: locations) (*admission)(location.length);
			if (locations.size() > (std::numeric_limits<size_t>::max() - directoryBytes) / sizeof(PropertyCollectionRootEntry)) {
				throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue Property slot size overflowed");
		}
		if (slotsAdmission != nullptr) (*slotsAdmission)(directoryBytes + locations.size() * sizeof(PropertyCollectionRootEntry));
		result.reserve(locations.size());
		if (result.capacity() > (std::numeric_limits<size_t>::max() - directoryBytes) / sizeof(PropertyCollectionRootEntry)) {
			throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue retained directory and Property slots overflowed");
		}
		if (slotsAdmission != nullptr) (*slotsAdmission)(directoryBytes + result.capacity() * sizeof(PropertyCollectionRootEntry));
		for (const auto &location: locations) {
			const size_t begin = RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + location.offset;
			result.push_back(decodeEntry(bytes.subspan(begin, location.length), location.key_token));
		}
		}
		if (slotsAdmission != nullptr) (*slotsAdmission)(result.capacity() * sizeof(PropertyCollectionRootEntry));
		return result;
	}

	std::vector<EntityPropertyLargeValueEntryLocation>
	EntityPropertyLargeValuePayloadCodec::decodeDirectory(const std::span<const std::byte> bytes,
		const SelectedPropertyReadAdmission *slotsAdmission, const CancellationCheckpoint *cancellation) {
		return decodeLargeValueDirectory(bytes, slotsAdmission, cancellation);
	}

	PropertyCollectionRootEntry EntityPropertyLargeValuePayloadCodec::decodeEntry(
			const std::span<const std::byte> bytes, const uint32_t expectedToken) {
		return decodeLargeValueEntry(bytes, expectedToken);
	}

	std::optional<PropertyCollectionRootEntry>
	EntityPropertyLargeValuePayloadCodec::decodeSelected(const std::span<const std::byte> bytes,
																			 const uint32_t keyToken) {
		const std::array<uint32_t, 1U> tokens{keyToken};
		return decodeSelected(bytes, tokens).front();
	}

	bool EntityPropertyLargeValuePayloadCodec::containsToken(const std::span<const std::byte> bytes,
																				 const uint32_t keyToken) {
			if (keyToken == 0U) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable LargeValue Property selection requires a nonzero token");
			}
			const auto locations = decodeDirectory(bytes);
			return std::ranges::binary_search(locations, keyToken,
													{}, &EntityPropertyLargeValueEntryLocation::key_token);
	}

	std::vector<std::optional<PropertyCollectionRootEntry>>
	EntityPropertyLargeValuePayloadCodec::decodeSelected(const std::span<const std::byte> bytes,
																				 const std::span<const uint32_t> keyTokens,
			const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) {
		return decodeSelectedAndCount(bytes, keyTokens, admission, slotsAdmission).second;
	}

	std::pair<size_t, std::vector<std::optional<PropertyCollectionRootEntry>>>
	EntityPropertyLargeValuePayloadCodec::decodeSelectedAndCount(const std::span<const std::byte> bytes,
																					 const std::span<const uint32_t> keyTokens, const SelectedPropertyReadAdmission *admission, const SelectedPropertyReadAdmission *slotsAdmission) {
		std::vector<std::optional<PropertyCollectionRootEntry>> result;
		size_t count = 0U;
		{
		const auto locations = decodeDirectory(bytes, slotsAdmission);
		count = locations.size();
		const size_t directoryBytes = locations.capacity() * sizeof(EntityPropertyLargeValueEntryLocation);
		const auto admitResult = [&](size_t capacity) {
			if (slotsAdmission == nullptr) return;
			if (capacity > (std::numeric_limits<size_t>::max() - directoryBytes) / sizeof(std::optional<PropertyCollectionRootEntry>))
				throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue selected slots overflowed");
			(*slotsAdmission)(directoryBytes + capacity * sizeof(std::optional<PropertyCollectionRootEntry>));
		};
		admitResult(keyTokens.size());
		result.reserve(keyTokens.size());
		admitResult(result.capacity());
		for (const uint32_t keyToken: keyTokens) {
			if (keyToken == 0U) {
				throwRootError(FormatError::FERR_INVALID_HEADER,
							   "relocatable LargeValue Property selection requires a nonzero token");
			}
			const auto found = std::ranges::lower_bound(locations, keyToken, {},
																&EntityPropertyLargeValueEntryLocation::key_token);
				if (found == locations.end() || found->key_token != keyToken) {
					result.emplace_back(std::nullopt);
					continue;
				}
				const size_t begin = RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + found->offset;
				if (admission != nullptr) (*admission)(found->length);
			result.emplace_back(decodeEntry(bytes.subspan(begin, found->length), keyToken));
		}
		}
		if (slotsAdmission != nullptr) (*slotsAdmission)(result.capacity() * sizeof(std::optional<PropertyCollectionRootEntry>));
		return {count, std::move(result)};
	}

	size_t EntityPropertyLargeValuePayloadCodec::entryCount(const std::span<const std::byte> bytes) {
		return decodeDirectory(bytes).size();
	}

	bool EntityPropertyLargeValuePayloadCodec::hasMagic(const std::span<const std::byte> bytes) {
		return bytes.size() >= sizeof(uint32_t) &&
			   LittleEndianCodec::readU32(bytes, 0U) == RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_MAGIC;
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
