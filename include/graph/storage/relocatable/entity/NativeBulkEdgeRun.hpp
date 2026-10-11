/**
 * @file NativeBulkEdgeRun.hpp
 * @brief Checked bounded run framing for transaction-private native bulk Edges.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "graph/storage/relocatable/entity/NativeBulkEdgeRecord.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_NATIVE_BULK_EDGE_RUN_MAGIC = 0x5255595AU; // Little-endian bytes: ZYUR
	constexpr uint16_t RSS_NATIVE_BULK_EDGE_RUN_VERSION = 1U;
	constexpr uint16_t RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES = 64U;

	// One run owns a strictly increasing sequence of assigned Edge IDs. The
	// eventual scratch allocator may place it in one or more private segments,
	// but this framing never leaks those locations into graph-visible state.
	struct NativeBulkEdgeRun {
		std::vector<NativeBulkEdgeRecord> records;

		bool operator==(const NativeBulkEdgeRun &) const = default;
	};

	class NativeBulkEdgeRunCodec final {
	public:
		[[nodiscard]] static size_t encodedSize(const NativeBulkEdgeRun &run);
		[[nodiscard]] static std::vector<std::byte> encode(const NativeBulkEdgeRun &run);
		[[nodiscard]] static NativeBulkEdgeRun decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
