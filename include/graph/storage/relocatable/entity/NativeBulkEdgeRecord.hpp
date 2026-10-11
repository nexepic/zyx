/**
 * @file NativeBulkEdgeRecord.hpp
 * @brief Canonical bounded scratch record for one streamed native Edge.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "graph/storage/LogicalGraphWrite.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_NATIVE_BULK_EDGE_RECORD_MAGIC = 0x4542595AU; // Little-endian bytes: ZYBE
	constexpr uint16_t RSS_NATIVE_BULK_EDGE_RECORD_VERSION = 1U;
	constexpr uint16_t RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES = 64U;

	// The logical Edge ID is assigned before the record enters scratch. This
	// makes record order deterministic and lets materialization preserve public
	// returned-ID order without retaining every request in memory.
	struct NativeBulkEdgeRecord {
		uint64_t edge_id = 0U;
		uint64_t source_node_id = 0U;
		uint64_t target_node_id = 0U;
		std::string type;
		std::vector<LogicalGraphWriteProperty> properties;

		bool operator==(const NativeBulkEdgeRecord &) const = default;
	};

	class NativeBulkEdgeRecordCodec final {
	public:
		// The exact encoded size is calculated without first encoding Property
		// values. A caller can enforce a segment-local record budget before it
		// allocates an extra byte buffer for the scratch image.
		[[nodiscard]] static size_t encodedSize(const NativeBulkEdgeRecord &record);
		[[nodiscard]] static std::vector<std::byte> encode(const NativeBulkEdgeRecord &record);
		[[nodiscard]] static NativeBulkEdgeRecord decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
