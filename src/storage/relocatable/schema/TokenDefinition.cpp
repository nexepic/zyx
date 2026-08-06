/**
 * @file TokenDefinition.cpp
 * @brief Explicit, bounded codec for relocatable logical Token definitions.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/schema/TokenDefinition.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t TOKEN_ID_OFFSET = 8;
		constexpr size_t NAME_BYTES_OFFSET = 16;
		constexpr size_t RESERVED_OFFSET = 20;

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validate(const TokenDefinition &definition) {
			if (definition.token_id == 0U || definition.name.empty() ||
				definition.name.size() > RSS_TOKEN_DEFINITION_MAX_NAME_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Token definition has an invalid ID or a name outside its byte bound");
			}
		}

	} // namespace

	std::vector<std::byte> TokenDefinitionCodec::encode(const TokenDefinition &definition) {
		validate(definition);
		std::vector<std::byte> bytes(RSS_TOKEN_DEFINITION_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, 0, RSS_TOKEN_DEFINITION_MAGIC);
		LittleEndianCodec::writeU16(bytes, 4, RSS_TOKEN_DEFINITION_VERSION);
		LittleEndianCodec::writeU16(bytes, 6, RSS_TOKEN_DEFINITION_HEADER_BYTES);
		LittleEndianCodec::writeU64(bytes, TOKEN_ID_OFFSET, definition.token_id);
		LittleEndianCodec::writeU32(bytes, NAME_BYTES_OFFSET, static_cast<uint32_t>(definition.name.size()));
		LittleEndianCodec::writeU32(bytes, RESERVED_OFFSET, 0U);
		const std::byte *begin = reinterpret_cast<const std::byte *>(definition.name.data());
		bytes.insert(bytes.end(), begin, begin + static_cast<std::ptrdiff_t>(definition.name.size()));
		return bytes;
	}

	TokenDefinition TokenDefinitionCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_TOKEN_DEFINITION_HEADER_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable Token definition is truncated");
		}
		if (LittleEndianCodec::readU32(bytes, 0) != RSS_TOKEN_DEFINITION_MAGIC) {
			fail(FormatError::FERR_INVALID_MAGIC, "relocatable Token definition magic does not match");
		}
		if (LittleEndianCodec::readU16(bytes, 4) != RSS_TOKEN_DEFINITION_VERSION) {
			fail(FormatError::FERR_UNSUPPORTED_VERSION, "relocatable Token definition version is unsupported");
		}
		if (LittleEndianCodec::readU16(bytes, 6) != RSS_TOKEN_DEFINITION_HEADER_BYTES ||
			LittleEndianCodec::readU32(bytes, RESERVED_OFFSET) != 0U) {
			fail(FormatError::FERR_RESERVED_BITS, "relocatable Token definition fixed or reserved fields are invalid");
		}
		const uint32_t nameBytes = LittleEndianCodec::readU32(bytes, NAME_BYTES_OFFSET);
		if (nameBytes > RSS_TOKEN_DEFINITION_MAX_NAME_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Token definition name exceeds its byte bound");
		}
		if (nameBytes != bytes.size() - RSS_TOKEN_DEFINITION_HEADER_BYTES) {
			fail(nameBytes > bytes.size() - RSS_TOKEN_DEFINITION_HEADER_BYTES ? FormatError::FERR_BUFFER_TOO_SMALL
																								 : FormatError::FERR_INVALID_HEADER,
				 "relocatable Token definition name length does not match its bytes");
		}
		const char *name = reinterpret_cast<const char *>(bytes.data() + RSS_TOKEN_DEFINITION_HEADER_BYTES);
		TokenDefinition definition{LittleEndianCodec::readU64(bytes, TOKEN_ID_OFFSET), {name, nameBytes}};
		validate(definition);
		return definition;
	}

} // namespace graph::storage
