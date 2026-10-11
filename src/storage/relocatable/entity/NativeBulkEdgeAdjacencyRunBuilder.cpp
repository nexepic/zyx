/**
 * @file NativeBulkEdgeAdjacencyRunBuilder.cpp
 * @brief Bounded sorted adjacency runs from sealed native Edge scratch.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeBulkEdgeAdjacencyRunBuilder.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateLeases(std::span<const NativeGraphLogicalIdLeaseRange> ranges) {
			if (ranges.empty()) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge adjacency-run builder requires an exact logical-ID lease");
			}
			uint64_t previousEnd = 0U;
			for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
				if (range.first_id == 0U || range.count == 0U ||
					range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id ||
					(previousEnd != 0U && range.first_id <= previousEnd)) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native bulk Edge adjacency-run builder requires ordered nonoverlapping logical-ID leases");
				}
				previousEnd = range.first_id + range.count - 1U;
			}
		}

		[[nodiscard]] size_t encodedRunEntryBytes(const BTreePageEntry &entry) {
			const size_t encodedSize = BTreePageEntryCodec::encodedSize(entry);
			if (encodedSize > std::numeric_limits<size_t>::max() - RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native bulk Edge adjacency-run entry byte size overflows");
			}
			return RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + encodedSize;
		}

		void emitRun(NativeBulkRunFragment &run, size_t maximumFragments, size_t &emittedFragments,
					 const NativeBulkEdgeAdjacencyRunVisitor &visitor) {
			if (run.entries.empty()) {
				return;
			}
			// Append-order sources whose endpoint order is already canonical need no
			// sort. Unordered input still takes the same canonicalizing sort path.
			if (!std::ranges::is_sorted(run.entries, {}, &BTreePageEntry::key)) {
				std::ranges::sort(run.entries, {}, &BTreePageEntry::key);
			}
			if (std::ranges::adjacent_find(run.entries, {}, &BTreePageEntry::key) != run.entries.end()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge adjacency-run builder found duplicate canonical adjacency keys");
			}

			// A sort run is an in-memory budget, not the on-disk fragment size. The
			// latter must fit one candidate scratch segment so the metadata writer can
			// seal it without an external temporary file. Preserve the sort order while
			// splitting a larger sorted run into independently valid fragments; the
			// future bounded fan-in merge consumes all fragments as ordered inputs.
			constexpr size_t maximumFragmentBytes = RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES;
			NativeBulkRunFragment fragment;
			size_t fragmentBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
			for (BTreePageEntry &entry: run.entries) {
				const size_t entryBytes = encodedRunEntryBytes(entry);
				if (entryBytes > maximumFragmentBytes - RSS_NATIVE_BULK_RUN_HEADER_BYTES) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge adjacency key cannot fit in one private scratch segment");
				}
				if (fragmentBytes > maximumFragmentBytes - entryBytes) {
					if (emittedFragments == maximumFragments) {
						fail(FormatError::FERR_INVALID_STATE,
							 "native bulk Edge adjacency-run builder exceeds its candidate fragment bound");
					}
					visitor(std::move(fragment));
					++emittedFragments;
					fragment = {};
					fragmentBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
				}
				fragment.entries.push_back(std::move(entry));
				fragmentBytes += entryBytes;
			}
			if (!fragment.entries.empty()) {
				if (emittedFragments == maximumFragments) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge adjacency-run builder exceeds its candidate fragment bound");
				}
				visitor(std::move(fragment));
				++emittedFragments;
			}
			run = {};
		}

	} // namespace

	void NativeBulkEdgeAdjacencyRunBuilder::buildRuns(
			const NativeBulkEdgeRecordSource &source,
			std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges, size_t maximumRecords, size_t maximumRunBytes,
			size_t maximumFragments,
			GlobalAdjacencyDirection direction, const NativeBulkEdgeAdjacencyRunVisitor &visitor) {
		if (!source || maximumRecords == 0U || maximumRunBytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES ||
			maximumFragments == 0U || !visitor ||
			(direction != GlobalAdjacencyDirection::GAD_OUT && direction != GlobalAdjacencyDirection::GAD_IN)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge adjacency-run builder requires finite bounds, one direction, a source, and a visitor");
		}
		validateLeases(edgeIdRanges);

		NativeBulkRunFragment current;
		size_t currentBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
		size_t recordCount = 0U;
		size_t emittedFragments = 0U;
		uint64_t previousEdgeId = 0U;
		size_t leaseRangeIndex = 0U;
		NativeBulkEdgeRecordCursor cursor = source();
		while (const std::optional<NativeBulkEdgeRecord> record = cursor()) {
			static_cast<void>(NativeBulkEdgeRecordCodec::encodedSize(*record));
			if (++recordCount > maximumRecords || record->edge_id == 0U || record->edge_id <= previousEdgeId) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge adjacency-run builder found unordered, over-bound, or unleased scratch records");
			}
			// The source and validated lease ranges are both strictly ordered, so a
			// monotonic cursor keeps lease verification bounded by the range count.
			while (leaseRangeIndex < edgeIdRanges.size() &&
				   !edgeIdRanges[leaseRangeIndex].contains(record->edge_id) &&
				   record->edge_id > edgeIdRanges[leaseRangeIndex].first_id) {
				++leaseRangeIndex;
			}
			if (leaseRangeIndex == edgeIdRanges.size() || !edgeIdRanges[leaseRangeIndex].contains(record->edge_id)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge adjacency-run builder found unordered, over-bound, or unleased scratch records");
			}
			previousEdgeId = record->edge_id;

			const uint64_t endpoint = direction == GlobalAdjacencyDirection::GAD_OUT ? record->source_node_id
																											 : record->target_node_id;
			// Deferred adjacency-tree publication consumes a canonical operation
			// stream: one denotes an UPSERT and is converted to the key-only leaf
			// representation only when the final tree is materialized.
			BTreePageEntry entry{AdjacencyKeyCodec::encode({direction, endpoint, record->edge_id}), 1U};
			const size_t entryBytes = encodedRunEntryBytes(entry);
			if (entryBytes > maximumRunBytes - RSS_NATIVE_BULK_RUN_HEADER_BYTES) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge adjacency key cannot fit in its configured private run");
			}
			if (currentBytes > maximumRunBytes - entryBytes) {
				emitRun(current, maximumFragments, emittedFragments, visitor);
				currentBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
			}
			current.entries.push_back(std::move(entry));
			currentBytes += entryBytes;
		}
		if (recordCount == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native bulk Edge adjacency-run builder requires a nonempty sealed source");
		}
		emitRun(current, maximumFragments, emittedFragments, visitor);
	}

	void NativeBulkEdgeAdjacencyRunBuilder::buildRunsPair(
			const NativeBulkEdgeRecordSource &source, std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
			size_t maximumRecords, size_t maximumRunBytes, size_t maximumOutgoingFragments,
			size_t maximumIncomingFragments, const NativeBulkEdgeAdjacencyRunVisitor &outgoingVisitor,
			const NativeBulkEdgeAdjacencyRunVisitor &incomingVisitor) {
		if (!source || maximumRecords == 0U || maximumRunBytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES ||
			maximumOutgoingFragments == 0U || maximumIncomingFragments == 0U || !outgoingVisitor || !incomingVisitor) {
			fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge paired adjacency-run builder requires finite bounds and visitors");
		}
		validateLeases(edgeIdRanges);
		NativeBulkRunFragment outgoing;
		NativeBulkRunFragment incoming;
		size_t outgoingBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
		size_t incomingBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
		size_t emittedOutgoing = 0U;
		size_t emittedIncoming = 0U;
		size_t recordCount = 0U;
		size_t leaseRangeIndex = 0U;
		uint64_t previousEdgeId = 0U;
		NativeBulkEdgeRecordCursor cursor = source();
		while (const std::optional<NativeBulkEdgeRecord> record = cursor()) {
			static_cast<void>(NativeBulkEdgeRecordCodec::encodedSize(*record));
			if (++recordCount > maximumRecords || record->edge_id == 0U || record->edge_id <= previousEdgeId) {
				fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge paired adjacency-run builder found unordered or over-bound scratch records");
			}
			while (leaseRangeIndex < edgeIdRanges.size() && !edgeIdRanges[leaseRangeIndex].contains(record->edge_id) &&
				   record->edge_id > edgeIdRanges[leaseRangeIndex].first_id) {
				++leaseRangeIndex;
			}
			if (leaseRangeIndex == edgeIdRanges.size() || !edgeIdRanges[leaseRangeIndex].contains(record->edge_id)) {
				fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge paired adjacency-run builder found an unleased scratch record");
			}
			previousEdgeId = record->edge_id;
			const auto append = [&](NativeBulkRunFragment &run, size_t &runBytes, size_t maximumFragments,
								size_t &emittedFragments, const NativeBulkEdgeAdjacencyRunVisitor &visitor,
								GlobalAdjacencyDirection direction, uint64_t endpoint) {
				BTreePageEntry entry{AdjacencyKeyCodec::encode({direction, endpoint, record->edge_id}), 1U};
				const size_t entryBytes = encodedRunEntryBytes(entry);
				if (entryBytes > maximumRunBytes - RSS_NATIVE_BULK_RUN_HEADER_BYTES) {
					fail(FormatError::FERR_INVALID_STATE,
							 "native bulk Edge paired adjacency key cannot fit in its configured private run");
				}
				if (runBytes > maximumRunBytes - entryBytes) {
					emitRun(run, maximumFragments, emittedFragments, visitor);
					runBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
				}
				run.entries.push_back(std::move(entry));
				runBytes += entryBytes;
			};
			append(outgoing, outgoingBytes, maximumOutgoingFragments, emittedOutgoing, outgoingVisitor,
					GlobalAdjacencyDirection::GAD_OUT, record->source_node_id);
			append(incoming, incomingBytes, maximumIncomingFragments, emittedIncoming, incomingVisitor,
					GlobalAdjacencyDirection::GAD_IN, record->target_node_id);
		}
		if (recordCount == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge paired adjacency-run builder requires a nonempty sealed source");
		}
		emitRun(outgoing, maximumOutgoingFragments, emittedOutgoing, outgoingVisitor);
		emitRun(incoming, maximumIncomingFragments, emittedIncoming, incomingVisitor);
	}

} // namespace graph::storage
