/**
 * @file NativeBulkRun.hpp
 * @brief Checked single-file scratch-run fragments for native bulk materialization.
 *
 * A fragment is a bounded, immutable sequence of already-sorted B+Tree
 * entries. Its bytes are intended for transaction-private storage inside the
 * database file; no caller path or host temporary file is part of this
 * contract. A future scratch allocator owns fragment placement and lifetime.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"

namespace graph::storage {

    // Capacity was refused and the fragment's Catalog publication did not occur.
    // Logical ID high-water marks may advance; no fragment ownership escaped.
    class NativeBulkRunCapacityDenied final : public std::system_error {
    public:
        NativeBulkRunCapacityDenied() : std::system_error(std::make_error_code(std::errc::no_space_on_device),
            "native scratch capacity denied before fragment publication") {}
    };

	constexpr uint32_t RSS_NATIVE_BULK_RUN_MAGIC = 0x5253595AU; // Little-endian bytes: ZYSR
	constexpr uint16_t RSS_NATIVE_BULK_RUN_VERSION = 1;
	constexpr uint16_t RSS_NATIVE_BULK_RUN_HEADER_BYTES = 64;
	constexpr uint16_t RSS_NATIVE_BULK_RUN_KNOWN_FLAGS = 0;
	constexpr uint32_t RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES = 8;
	constexpr uint32_t RSS_NATIVE_BULK_RUN_MANIFEST_MAGIC = 0x4D53595AU; // Little-endian bytes: ZYSM
	constexpr uint16_t RSS_NATIVE_BULK_RUN_MANIFEST_VERSION = 1;
	constexpr uint16_t RSS_NATIVE_BULK_RUN_MANIFEST_HEADER_BYTES = 64;
	constexpr uint16_t RSS_NATIVE_BULK_RUN_MANIFEST_ENTRY_BYTES = 16;

	struct NativeBulkRunFragment {
		utils::OwnedStdVector<BTreePageEntry> entries;

		constexpr bool operator==(const NativeBulkRunFragment &) const = default;
	};

	struct NativeBulkRunFragmentRange {
		size_t first_entry_ordinal = 0U;
		size_t entry_count = 0U;
		size_t encoded_bytes = 0U;

		constexpr bool operator==(const NativeBulkRunFragmentRange &) const = default;
	};

	struct NativeBulkRunFragmentLayout {
		std::vector<NativeBulkRunFragmentRange> fragments;
		uint64_t entry_count = 0U;
		uint64_t encoded_bytes = 0U;

		constexpr bool operator==(const NativeBulkRunFragmentLayout &) const = default;
	};

	// Builds a compact random-access partition in one pass. Deferred producers can
	// later materialize fragment N from its exact source range instead of rescanning
	// every preceding entry. Memory is proportional to fragment count, not input.
	class NativeBulkRunFragmentLayoutBuilder final {
	public:
		explicit NativeBulkRunFragmentLayoutBuilder(size_t maximumFragmentBytes);
		void append(size_t encodedEntryBytes);
		[[nodiscard]] NativeBulkRunFragmentLayout finish();

	private:
		void flush();

		NativeBulkRunFragmentLayout layout_;
		size_t maximum_fragment_bytes_ = 0U;
		size_t current_first_entry_ordinal_ = 0U;
		size_t current_entry_count_ = 0U;
		size_t current_encoded_bytes_ = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
		bool finished_ = false;
	};

	struct NativeBulkRunFragmentLocation {
		SegmentId segment_id{};
		Generation generation{};
		uint64_t transaction_id = 0U;

		[[nodiscard]] constexpr bool isValid() const {
			return segment_id.isValid() && generation.isValid() && transaction_id != 0U;
		}
		constexpr bool operator==(const NativeBulkRunFragmentLocation &) const = default;
	};

	class NativeBulkRunStore;
	class NativeBulkEdgeRunStreamingWriter;
	void releaseNativeBulkEdgeManifest(NativeBulkRunStore &, NativeBulkRunFragmentLocation, LogSequenceNumber);

	// A manifest describes one globally ordered logical run. Its fragments are
	// separate full segments so a run is never constrained by one segment's
	// capacity; their order is part of the durable integrity contract.
	struct NativeBulkRunManifest {
		NativeBulkRunFragmentLocation location{};
		// A level-zero manifest points at fragments. Every higher level points at
		// manifests from exactly the preceding level, keeping traversal depth and
		// fan-out explicit instead of imposing one-segment run-size limits.
		uint16_t level = 0U;
		std::vector<NativeBulkRunFragmentLocation> fragments;
		std::vector<NativeBulkRunFragmentLocation> children;
		uint64_t entry_count = 0U;
		uint64_t encoded_bytes = 0U;

		[[nodiscard]] bool isValid() const {
			return location.isValid() && ((level == 0U && !fragments.empty() && children.empty()) ||
														  (level != 0U && fragments.empty() && !children.empty())) &&
				   entry_count != 0U && encoded_bytes != 0U;
		}
		constexpr bool operator==(const NativeBulkRunManifest &) const = default;
	};

	struct NativeBulkRunStoreOptions {
		PhysicalSlotId first_slot{};
		PhysicalSlotId last_slot{};
		size_t maximum_fragment_bytes = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
		// Catalog allocation batches amortize metadata publication, but must remain
		// independent of manifest fan-out: the latter can be large enough that
		// retaining every encoded fragment would defeat streaming ingest.
		size_t maximum_batch_fragments = 64U;
		uint16_t maximum_manifest_depth = 8U;
		SegmentKind scratch_kind = SegmentKind::SGK_BULK_SCRATCH;
	};

	struct NativeBulkRunStreamingOptions {
		size_t maximum_buffered_bytes = 0U;
		uint64_t maximum_entries = 0U;
	};

	struct NativeBulkRunMergeOptions {
		size_t maximum_open_runs = 0U;
		NativeBulkRunStreamingOptions output;
	};

	// A reader capability for an already authenticated private scratch route.
	// The location still carries SegmentId, generation, and transaction owner;
	// implementations must reject identities outside their scoped candidate set.
	using NativeBulkRunPayloadReader =
			std::function<std::vector<std::byte>(const NativeBulkRunFragmentLocation &)>;

	// The persistent encoding is deliberately independent from the scratch-pool
	// allocator. This lets the allocator verify exact bytes before a Catalog
	// entry becomes recoverable and makes corruption checks available to every
	// eventual run reader and merger.
	class NativeBulkRunFragmentCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const NativeBulkRunFragment &fragment);
		[[nodiscard]] static NativeBulkRunFragment decode(std::span<const std::byte> bytes);
	};

	class NativeBulkRunManifestCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const NativeBulkRunManifest &manifest);
		[[nodiscard]] static NativeBulkRunManifest decode(std::span<const std::byte> bytes,
														 NativeBulkRunFragmentLocation location);
	};

	// Owns only transaction-private full-segment fragments. Catalog visibility is
	// allocator bookkeeping, never a public graph root: logical readers have no
	// route to these segments. A writable reopen retires and releases all such
	// fragments before the backend constructs any graph reader.
	class NativeBulkRunStore final {
	public:
		NativeBulkRunStore(PhysicalSegmentAllocator &physical, RelocatableMetadataStore &metadata,
						   NativeBulkRunStoreOptions options);
		// A final graph publication must bind its scratch source to this exact
		// metadata file. This prevents a caller from replaying a valid-looking
		// manifest through a store backed by another database instance.
		[[nodiscard]] bool usesMetadataStore(const RelocatableMetadataStore &metadata) const noexcept {
			return &metadata_ == &metadata;
		}

		[[nodiscard]] NativeBulkRunFragmentLocation write(uint64_t transactionId,
															 const NativeBulkRunFragment &fragment,
															 LogSequenceNumber checkpointLsn);
		// Persists one bounded, transaction-private typed payload. This is the
		// shared placement primitive for scratch formats that are not B+Tree entry
		// runs (for example streamed Edge records). The caller remains responsible
		// for its own canonical codec; no graph-visible directory can name this
		// location.
		[[nodiscard]] NativeBulkRunFragmentLocation writeOpaquePayload(uint64_t transactionId,
																	std::vector<std::byte> payload,
																	LogSequenceNumber checkpointLsn);
		// Publishes one bounded group of opaque scratch payloads through a single
		// logical-ID reservation and physical-allocation generation. Ownership and
		// ordering match writeBatch(), but callers retain their own typed codec.
		[[nodiscard]] std::vector<NativeBulkRunFragmentLocation>
		writeOpaquePayloadBatch(uint64_t transactionId, std::vector<std::vector<std::byte>> payloads,
								LogSequenceNumber checkpointLsn);
		// Writes a bounded group of fragments through one Catalog/allocator
		// publication.  The group is the natural unit for a leaf manifest and
		// avoids advancing the Superblock once per fragment during bulk ingest.
		[[nodiscard]] std::vector<NativeBulkRunFragmentLocation>
		writeBatch(uint64_t transactionId, std::span<const NativeBulkRunFragment> fragments,
					 LogSequenceNumber checkpointLsn);
		[[nodiscard]] NativeBulkRunFragment read(const MetadataSnapshotPin &snapshot,
															 const NativeBulkRunFragmentLocation &location) const;
		[[nodiscard]] std::vector<std::byte> readOpaquePayload(
				const MetadataSnapshotPin &snapshot, const NativeBulkRunFragmentLocation &location) const;
		// Pins one immutable metadata generation for the lifetime of the returned
		// reader. Rewindable typed sources request a fresh reader per pass, avoiding
		// per-fragment snapshot acquisition and transaction-sized route tables.
		[[nodiscard]] NativeBulkRunPayloadReader openOpaquePayloadReader() const;
		[[nodiscard]] NativeBulkRunManifest writeManifest(uint64_t transactionId,
														  std::span<const NativeBulkRunFragmentLocation> fragments,
														  LogSequenceNumber checkpointLsn);
		[[nodiscard]] NativeBulkRunManifest writeManifestParent(uint64_t transactionId,
																				std::span<const NativeBulkRunFragmentLocation> children,
																				LogSequenceNumber checkpointLsn);
		[[nodiscard]] NativeBulkRunManifest readManifest(const MetadataSnapshotPin &snapshot,
															 const NativeBulkRunFragmentLocation &location) const;
		// The returned source opens one private metadata snapshot per pass and
		// buffers no more than one immutable fragment. It is suitable for the
		// independently rewindable two-pass BulkBTreeBuilder contract.
		[[nodiscard]] OrderedBTreeEntrySource
		orderedSource(const NativeBulkRunFragmentLocation &manifestLocation) const;
		// Builds the same rewindable source from a writer-owned private reader.
		// This never opens a MetadataSnapshotPin, which lets one unpublished graph
		// candidate feed both bulk-tree passes without a temporary publication.
		[[nodiscard]] static OrderedBTreeEntrySource orderedSourceFromPayloadReader(
				const NativeBulkRunFragmentLocation &manifestLocation, uint16_t maximumManifestDepth,
				NativeBulkRunPayloadReader payloadReader);
		void releaseManifest(const NativeBulkRunFragmentLocation &manifestLocation, LogSequenceNumber checkpointLsn);
		[[nodiscard]] size_t maximumManifestReferences() const;
		[[nodiscard]] size_t maximumBatchFragments() const;
		[[nodiscard]] uint16_t maximumManifestDepth() const;
		[[nodiscard]] size_t maximumFragmentBytes() const;
		void release(const NativeBulkRunFragmentLocation &location, LogSequenceNumber checkpointLsn);
		// Authenticates and retires one bounded scratch group in a single metadata
		// publication, then runs one snapshot-safe reclamation pass.
		void releaseBatch(std::span<const NativeBulkRunFragmentLocation> locations,
							  LogSequenceNumber checkpointLsn);
		// No public root can refer to this kind, so a leftover fragment after a
		// process crash is safe to reclaim at the next writable open.
		void reclaimUnpublishedOnOpen(LogSequenceNumber checkpointLsn);

	private:
		friend class NativeBulkRunStreamingWriter;
		friend class NativeBulkEdgeRunStreamingWriter;
		friend void releaseNativeBulkEdgeManifest(NativeBulkRunStore &, NativeBulkRunFragmentLocation,
																  LogSequenceNumber);
		PhysicalSegmentAllocator &physical_;
		RelocatableMetadataStore &metadata_;
		NativeBulkRunStoreOptions options_;

	};

	// Appends an already ordered stream using only one bounded fragment and a
	// bounded manifest fan-out per level. It deliberately does not sort input:
	// the eventual bounded merge cursor is responsible for global ordering.
	class NativeBulkRunStreamingWriter final {
	public:
		NativeBulkRunStreamingWriter(NativeBulkRunStore &store, uint64_t transactionId,
											 NativeBulkRunStreamingOptions options, LogSequenceNumber firstCheckpointLsn);

		void add(BTreePageEntry entry);
		[[nodiscard]] NativeBulkRunManifest finish();

	private:
		NativeBulkRunStore &store_;
		uint64_t transaction_id_ = 0U;
		NativeBulkRunStreamingOptions options_;
		LogSequenceNumber next_checkpoint_lsn_{};
		NativeBulkRunFragment buffered_;
		size_t buffered_bytes_ = 0U;
		uint64_t entry_count_ = 0U;
		std::optional<std::vector<std::byte>> last_key_;
		std::vector<NativeBulkRunFragment> pending_fragment_batch_;
		std::vector<NativeBulkRunFragmentLocation> pending_fragments_;
		std::vector<std::vector<NativeBulkRunFragmentLocation>> manifest_levels_;
		bool finished_ = false;

		void flushFragment();
		void sealLeafManifest();
		void appendManifestAtLevel(NativeBulkRunFragmentLocation location, size_t level);
		void sealParentManifest(size_t level);
		[[nodiscard]] size_t maximumManifestReferences() const;
		void advanceCheckpointLsn();
	};

	// Performs one bounded fan-in merge. Callers reduce a larger run set in
	// bounded groups; no operation opens or materializes every input run.
	class NativeBulkRunMerger final {
	public:
		// Exposes the read-only half of bounded fan-in merge for a writer-owned
		// candidate. Unlike merge(), this neither allocates scratch nor publishes a
		// Catalog/Superblock generation: BulkBTreeBuilder can rewind the returned
		// source for both layout and materialization passes while retaining at most
		// one entry per input run. Callers reduce larger run sets before invoking
		// this primitive, so `maximum_open_runs` is a hard resource boundary.
		[[nodiscard]] static OrderedBTreeEntrySource
		orderedSource(std::span<const OrderedBTreeEntrySource> inputs, size_t maximumOpenRuns,
					  size_t maximumEntries);
		[[nodiscard]] static NativeBulkRunManifest merge(NativeBulkRunStore &store, uint64_t transactionId,
																					 std::span<const NativeBulkRunFragmentLocation> inputs,
																					 NativeBulkRunMergeOptions options,
																					 LogSequenceNumber firstCheckpointLsn);
	};

} // namespace graph::storage
