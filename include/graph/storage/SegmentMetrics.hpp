/**
 * @file SegmentMetrics.hpp
 * @brief Read-only Stage 0 measurements for the current segment format.
 *
 * These structures describe the current fixed-record format. They are
 * intentionally observational: they do not imply the relocatable physical layout.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "graph/core/Types.hpp"

namespace graph::storage {

	constexpr size_t SEGMENT_METRICS_TYPE_COUNT = static_cast<size_t>(getMaxEntityType()) + 1;

	/**
	 * Process-lifetime writes that can be attributed exactly to a segment subtype.
	 * File-header and WAL writes are deliberately excluded because the current
	 * format does not carry enough ownership information to attribute them safely.
	 */
	struct SegmentWriteMetrics {
		uint64_t logical_payload_bytes = 0;
		uint64_t data_write_bytes = 0;
		uint64_t metadata_write_bytes = 0;
		uint64_t allocation_write_bytes = 0;
		uint64_t relocation_write_bytes = 0;
		uint64_t attributed_write_bytes = 0;
		double write_amplification = 0.0;
	};

	/**
	 * Space and write measurements for one fixed-record segment subtype.
	 * All byte counts are measured in the current on-disk representation.
	 */
	struct SegmentTypeMetrics {
		uint32_t data_type = 0;
		uint64_t segment_count = 0;
		uint64_t physical_bytes = 0;
		uint64_t metadata_bytes = 0;
		uint64_t data_region_bytes = 0;
		uint64_t slot_capacity_bytes = 0;
		uint64_t live_payload_bytes = 0;
		uint64_t never_used_payload_bytes = 0;
		uint64_t tombstone_payload_bytes = 0;
		uint64_t layout_slack_bytes = 0;
		uint64_t inconsistent_header_count = 0;
		SegmentWriteMetrics writes{};
	};

	/**
	 * Immutable snapshot of current-format segment measurements.
	 * Entries are indexed by the numeric EntityType value.
	 */
	struct SegmentStorageMetrics {
		std::array<SegmentTypeMetrics, SEGMENT_METRICS_TYPE_COUNT> by_type{};
		uint64_t reusable_free_segment_count = 0;
		uint64_t reusable_free_bytes = 0;
	};

} // namespace graph::storage
