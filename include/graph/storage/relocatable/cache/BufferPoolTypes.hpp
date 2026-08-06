/**
 * @file BufferPoolTypes.hpp
 * @brief Type-safe identity and telemetry for the native 4 KiB page cache.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	// The buffer cache operates on the format's physical page granularity.
	constexpr size_t RSS_BUFFER_POOL_PAGE_BYTES = RSS_PAGE_SIZE;

	// All enum values have a prefix so Windows headers cannot collide with them.
	enum class BufferPoolReadHint : uint8_t {
		BPRH_NORMAL = 0,
		BPRH_SEQUENTIAL = 1,
		BPRH_PREFETCH = 2,
	};

	// A physical offset alone is not stable across slot reuse. Callers that know
	// the durable generation supply it here; transparent I/O uses generation zero
	// and invalidates the range before a physical overwrite.
	struct BufferPoolPageKey {
		uint64_t page_offset = 0U;
		uint64_t generation = 0U;

		constexpr auto operator<=>(const BufferPoolPageKey &) const = default;
	};

	struct BufferPoolStats {
		size_t capacity_bytes = 0U;
		size_t resident_bytes = 0U;
		size_t pinned_bytes = 0U;
		size_t dirty_bytes = 0U;
		uint64_t hits = 0U;
		uint64_t misses = 0U;
		uint64_t evictions = 0U;
		uint64_t prefetch_requests = 0U;
		uint64_t prefetch_hits = 0U;
		uint64_t disk_read_bytes = 0U;
		uint64_t disk_write_bytes = 0U;

		constexpr bool operator==(const BufferPoolStats &) const = default;
	};

} // namespace graph::storage
