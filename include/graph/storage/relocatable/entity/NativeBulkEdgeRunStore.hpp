/**
 * @file NativeBulkEdgeRunStore.hpp
 * @brief Bounded hierarchical scratch manifests for streamed native Edges.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/entity/NativeBulkEdgeRun.hpp"
#include "graph/storage/relocatable/index/NativeBulkRun.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_NATIVE_BULK_EDGE_MANIFEST_MAGIC = 0x4D45595AU; // Little-endian bytes: ZYEM
	constexpr uint16_t RSS_NATIVE_BULK_EDGE_MANIFEST_VERSION = 1U;
	constexpr uint16_t RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES = 96U;
	constexpr uint16_t RSS_NATIVE_BULK_EDGE_MANIFEST_REFERENCE_BYTES = 16U;

	// A manifest references runs at level zero and same-format manifests at all
	// higher levels.  It deliberately carries only logical identities, never a
	// physical slot, so it remains private to the scratch owner.
	struct NativeBulkEdgeRunManifest {
		NativeBulkRunFragmentLocation location{};
		uint16_t level = 0U;
		std::vector<NativeBulkRunFragmentLocation> runs;
		std::vector<NativeBulkRunFragmentLocation> children;
		uint64_t record_count = 0U;
		uint64_t encoded_bytes = 0U;
		uint64_t first_edge_id = 0U;
		uint64_t last_edge_id = 0U;

		[[nodiscard]] bool isValid() const {
			return location.isValid() && record_count != 0U && encoded_bytes != 0U && first_edge_id != 0U &&
				   first_edge_id <= last_edge_id &&
				   ((level == 0U && !runs.empty() && children.empty()) ||
					(level != 0U && runs.empty() && !children.empty()));
		}
		constexpr bool operator==(const NativeBulkEdgeRunManifest &) const = default;
	};

	class NativeBulkEdgeRunManifestCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const NativeBulkEdgeRunManifest &manifest);
		[[nodiscard]] static NativeBulkEdgeRunManifest decode(std::span<const std::byte> bytes,
													  NativeBulkRunFragmentLocation location);
	};

	struct NativeBulkEdgeRunStreamingOptions {
		size_t maximum_run_bytes = 0U;
		uint64_t maximum_records = 0U;
		uint16_t maximum_manifest_depth = 0U;
	};

	using NativeBulkEdgeRecordCursor = std::function<std::optional<NativeBulkEdgeRecord>()>;
	using NativeBulkEdgeRecordSource = std::function<NativeBulkEdgeRecordCursor()>;

	// Creates a rewindable source over a sealed private manifest. The reader
	// capability remains responsible for authenticating the location; this
	// decoder additionally enforces owner, hierarchy, count, and global ID order.
	[[nodiscard]] NativeBulkEdgeRecordSource nativeBulkEdgeRecordSource(
			NativeBulkRunFragmentLocation manifestLocation, uint16_t maximumManifestDepth,
			NativeBulkRunPayloadReader payloadReader);
	// Opens one allocator-authenticated scratch snapshot per rewind. The caller
	// must keep `store` alive for the complete source lifetime.
	[[nodiscard]] NativeBulkEdgeRecordSource nativeBulkEdgeRecordSource(
			NativeBulkRunStore &store, NativeBulkRunFragmentLocation manifestLocation,
			uint16_t maximumManifestDepth);
	// Adapts a writer-scoped private candidate set into the typed Edge source
	// without publishing a scratch Catalog entry. Every manifest child is
	// authenticated against the exact candidate set before the scoped reader is
	// called, so no public snapshot, physical slot, or foreign Segment identity
	// can leak into final graph materialization.
	[[nodiscard]] NativeBulkEdgeRecordSource nativeBulkEdgeRecordSourceFromPrivateReader(
			const NativeGraphPrivateSegmentSet &privateSegments,
			NativeGraphPrivateSegmentMaterialization manifestIdentity, uint16_t maximumManifestDepth,
			const NativeGraphPrivateSegmentReader &privateSegmentReader);
	// Rollback/recovery releases every run and manifest reachable from one root.
	// It rejects cycles, foreign owners, invalid level transitions, and a
	// hierarchy deeper than the store's configured bound before changing
	// allocator state.
	void releaseNativeBulkEdgeManifest(NativeBulkRunStore &store,
									NativeBulkRunFragmentLocation manifestLocation,
									LogSequenceNumber checkpointLsn);

	// This is the typed Edge counterpart to NativeBulkRunStreamingWriter.  It
	// retains only one run plus one bounded fan-out at each manifest level; the
	// accumulated Edge payload and run descriptor list are never held in RAM.
	class NativeBulkEdgeRunStreamingWriter final {
	public:
		NativeBulkEdgeRunStreamingWriter(NativeBulkRunStore &store, uint64_t transactionId,
												  NativeBulkEdgeRunStreamingOptions options,
												  LogSequenceNumber firstCheckpointLsn);

		void add(NativeBulkEdgeRecord record);
		// A caller that permits unrelated metadata publications between input
		// batches refreshes this floor while holding its short publication gate.
		// Already-consumed LSNs never move backwards.
		void refreshCheckpointFloor(LogSequenceNumber checkpointLsn);
		[[nodiscard]] NativeBulkEdgeRunManifest finish();
		// Converts the currently buffered run hierarchy into one validated root,
		// then retires all of it.  Transaction rollback uses this eager cleanup so
		// a healthy process does not retain scratch space until its next reopen.
		void abort(LogSequenceNumber firstReleaseCheckpointLsn);

	private:
		NativeBulkRunStore &store_;
		uint64_t transaction_id_ = 0U;
		NativeBulkEdgeRunStreamingOptions options_;
		LogSequenceNumber next_checkpoint_lsn_{};
		NativeBulkEdgeRun buffered_;
		size_t buffered_bytes_ = RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES;
		uint64_t record_count_ = 0U;
		uint64_t last_edge_id_ = 0U;
		std::vector<std::vector<NativeBulkRunFragmentLocation>> manifest_levels_;
		std::vector<std::vector<std::byte>> pending_run_payloads_;
		bool finished_ = false;

		void flushRun();
		void flushRunBatch();
		void appendManifestAtLevel(NativeBulkRunFragmentLocation location, size_t level);
		void sealManifestLevel(size_t level);
		void advanceCheckpointLsn();
		[[nodiscard]] size_t maximumManifestReferences() const;
		[[nodiscard]] NativeBulkEdgeRunManifest readManifest(NativeBulkRunFragmentLocation location) const;
		[[nodiscard]] NativeBulkEdgeRunManifest writeManifest(
				std::span<const NativeBulkRunFragmentLocation> items, uint16_t level);
	};

} // namespace graph::storage
