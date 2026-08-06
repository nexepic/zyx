/**
 * @file StateRecord.hpp
 * @brief Explicit relocatable State record codec for slotted-page storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_STATE_RECORD_MAGIC = 0x5453595AU; // Little-endian bytes: ZYST
	// Version 2 routes external State values through a directory-routed
	// LargeValue identity. Older State records are deliberately
	// rejected rather than being interpreted under the new ownership contract.
	constexpr uint16_t RSS_STATE_RECORD_VERSION = 2;
	constexpr uint16_t RSS_STATE_RECORD_HEADER_BYTES = 48;
	constexpr std::string_view RSS_STATE_LARGE_VALUE_MEDIA_TYPE = "application/vnd.zyx.state-value";

	enum class StateRecordStorageKind : uint8_t {
		SRS_INLINE = 1,
		SRS_LARGE_VALUE = 2,
	};

	struct StateRecord {
		uint64_t state_id = 0;
		std::string key;
		StateRecordStorageKind storage_kind = StateRecordStorageKind::SRS_INLINE;
		LargeValueId large_value_id{};
		std::vector<std::byte> inline_bytes;

		bool operator==(const StateRecord &) const = default;
	};

	class StateRecordCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const StateRecord &record);
		[[nodiscard]] static StateRecord decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
