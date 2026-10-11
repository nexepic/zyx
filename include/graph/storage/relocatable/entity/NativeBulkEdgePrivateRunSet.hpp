/**
 * @file NativeBulkEdgePrivateRunSet.hpp
 * @brief Bounded private collection of logical streamed Edge-derived runs.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunMaterializer.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_NATIVE_BULK_RUN_SET_MAGIC = 0x5353595AU; // Little-endian bytes: ZYSS
	constexpr uint16_t RSS_NATIVE_BULK_RUN_SET_VERSION = 1U;
	constexpr uint16_t RSS_NATIVE_BULK_RUN_SET_HEADER_BYTES = 48U;
	constexpr uint16_t RSS_NATIVE_BULK_RUN_SET_REFERENCE_BYTES = 16U;

	struct NativeBulkEdgePrivateRunSetManifest {
		NativeBulkRunFragmentLocation location{};
		uint16_t level = 0U;
		std::vector<NativeBulkRunFragmentLocation> runs;
		std::vector<NativeBulkRunFragmentLocation> children;
		uint64_t run_count = 0U;
	};

	class NativeBulkEdgePrivateRunSetCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const NativeBulkEdgePrivateRunSetManifest &manifest);
		[[nodiscard]] static NativeBulkEdgePrivateRunSetManifest decode(std::span<const std::byte> payload,
																		NativeBulkRunFragmentLocation location);
	};

	// Collects completed logical-run manifests into a bounded private hierarchy.
	// It replaces the otherwise unbounded vector of roots between merge levels.
	class NativeBulkEdgePrivateRunSetBuilder final {
	public:
		NativeBulkEdgePrivateRunSetBuilder(uint16_t maximumDepth,
										   const NativeBulkEdgePrivateCandidateAllocator &allocateCandidate,
										   const NativeBulkEdgePrivateCandidateWriter &writeCandidate);

		void append(NativeBulkRunFragmentLocation runRoot);
		[[nodiscard]] NativeBulkRunFragmentLocation finish();

	private:
		void appendAtLevel(uint16_t level, NativeBulkRunFragmentLocation location, uint64_t runCount);
		void flushLevel(uint16_t level);

		uint16_t maximum_depth_ = 0U;
		// The builder can flush after construction, so it must own callback state
		// passed as temporary closures instead of retaining dangling references.
		NativeBulkEdgePrivateCandidateAllocator allocate_candidate_;
		NativeBulkEdgePrivateCandidateWriter write_candidate_;
		std::vector<std::vector<std::pair<NativeBulkRunFragmentLocation, uint64_t>>> levels_;
	};

	// Opens each logical run in the original bounded-group order. The returned
	// source is callback-scoped and cannot access public metadata or raw slots.
	[[nodiscard]] NativeBulkEdgeRunSource
	nativeBulkEdgePrivateRunSetSource(const NativeGraphPrivateSegmentSet &identities,
									  NativeBulkRunFragmentLocation root, uint16_t maximumDepth,
									  const NativeGraphPrivateSegmentReader &reader);
	// Returns the only logical-run manifest after a reduction has converged. It
	// validates the terminal run-set instead of letting callers mistake the
	// run-set codec for a NativeBulkRun manifest.
	[[nodiscard]] NativeBulkRunFragmentLocation
	nativeBulkEdgePrivateRunSetSingleRun(const NativeGraphPrivateSegmentSet &identities,
									  NativeBulkRunFragmentLocation root,
									  const NativeGraphPrivateSegmentReader &reader);

} // namespace graph::storage
