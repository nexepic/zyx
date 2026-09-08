/**
 * @file NativeBulkEdgePublicationPreflight.cpp
 * @brief Bounded streamed Edge publication preflight implementation.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/relocatable/entity/NativeBulkEdgePublicationPreflight.hpp"

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "graph/storage/relocatable/allocation/SlottedPage.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeAdjacencyRunBuilder.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeEndpointCache.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeTokenRunBuilder.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/utils/Sha256.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool isLeased(std::span<const NativeGraphLogicalIdLeaseRange> ranges, uint64_t edgeId) {
			return std::ranges::any_of(
					ranges, [edgeId](const NativeGraphLogicalIdLeaseRange &range) { return range.contains(edgeId); });
		}

		void validateLeases(std::span<const NativeGraphLogicalIdLeaseRange> ranges) {
			if (ranges.empty()) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge publication preflight requires an exact logical-ID lease");
			}
			uint64_t previousEnd = 0U;
			for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
				if (range.first_id == 0U || range.count == 0U ||
					range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id ||
					(previousEnd != 0U && range.first_id <= previousEnd)) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native bulk Edge publication preflight requires ordered nonoverlapping logical-ID leases");
				}
				previousEnd = range.first_id + range.count - 1U;
			}
		}

		void checkedIncrement(size_t &value, const char *message) {
			if (value == std::numeric_limits<size_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
			++value;
		}

		// Each source pass remains bounded to one encoded record plus an incremental
		// digest. The digest makes a changed replay fail before it can seed private
		// token or adjacency candidates that disagree with the Entity/Property source.
		struct SourceReplayFingerprint {
			size_t record_count = 0U;
			uint64_t first_edge_id = 0U;
			uint64_t last_edge_id = 0U;
			utils::Sha256 digest;

			void append(const NativeBulkEdgeRecord &record) {
				const std::vector<std::byte> encoded = NativeBulkEdgeRecordCodec::encode(record);
				std::array<std::byte, sizeof(uint64_t)> length{};
				const uint64_t encodedBytes = encoded.size();
				for (size_t index = 0U; index < length.size(); ++index) {
					length[length.size() - 1U - index] =
							static_cast<std::byte>((encodedBytes >> (index * 8U)) & UINT64_C(0xFF));
				}
				// Prefix each canonical record with its size so the replay digest has
				// unambiguous record boundaries without retaining source-sized state.
				digest.update(length);
				digest.update(encoded);
				if (record_count == 0U) {
					first_edge_id = record.edge_id;
				}
				last_edge_id = record.edge_id;
				checkedIncrement(record_count, "native bulk Edge replay fingerprint record count overflowed");
			}

			[[nodiscard]] bool matches(const SourceReplayFingerprint &other) const {
				return record_count == other.record_count && first_edge_id == other.first_edge_id &&
						last_edge_id == other.last_edge_id && digest.finalize() == other.digest.finalize();
			}
		};

		[[nodiscard]] NativeBulkEdgeRecordSource
		verifiedReplaySource(const NativeBulkEdgeRecordSource &source, SourceReplayFingerprint expected) {
			return [source, expected = std::move(expected)] {
				struct ReplayState {
					SourceReplayFingerprint observed;
				};
				auto state = std::make_shared<ReplayState>();
				NativeBulkEdgeRecordCursor cursor = source();
				return [cursor = std::move(cursor), state, expected]() mutable -> std::optional<NativeBulkEdgeRecord> {
					const std::optional<NativeBulkEdgeRecord> record = cursor();
					if (!record.has_value()) {
						if (!state->observed.matches(expected)) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native bulk Edge source replay diverged from its sealed preflight pass");
						}
						return std::nullopt;
					}
					state->observed.append(*record);
					return record;
				};
			};
		}

		[[nodiscard]] size_t propertyRootEncodedBytes(const NativeBulkEdgeRecord &record) {
			size_t result = RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES;
			for (const LogicalGraphWriteProperty &property: record.properties) {
				const size_t valueBytes = CanonicalPropertyValueCodec::encodedSize(property.value);
				if (valueBytes > std::numeric_limits<size_t>::max() - sizeof(uint32_t) ||
					valueBytes + sizeof(uint32_t) > std::numeric_limits<size_t>::max() - result) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "native bulk Edge publication preflight Property root size overflowed");
				}
				result += sizeof(uint32_t) + valueBytes;
			}
			return result;
		}

		[[nodiscard]] constexpr size_t inlinePropertyRootCapacity() {
			return RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES - RSS_SLOTTED_PAGE_SLOT_BYTES;
		}

	} // namespace

	size_t NativeBulkEdgePublicationPreflightResult::derivedCandidateFragments() const {
		if (outgoing_adjacency_fragments > std::numeric_limits<size_t>::max() - token_fragments) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native bulk Edge publication preflight candidate fragment count overflowed");
		}
		const size_t withOutgoing = token_fragments + outgoing_adjacency_fragments;
		if (incoming_adjacency_fragments > std::numeric_limits<size_t>::max() - withOutgoing) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native bulk Edge publication preflight candidate fragment count overflowed");
		}
		return withOutgoing + incoming_adjacency_fragments;
	}

	NativeBulkEdgePublicationPreflightResult
	NativeBulkEdgePublicationPreflight::validate(const NativeBulkEdgeRecordSource &source,
												 std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
												 const NativeBulkEdgePublicationPreflightOptions &options,
												 const NativeBulkEdgePersistedNodeValidator &persistedNodeExists) {
		if (!source || !persistedNodeExists || options.maximum_records == 0U ||
			options.sort_run_bytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES || options.maximum_sort_runs < 2U ||
			options.maximum_candidate_fragments == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge publication preflight requires finite source, sort, candidate, and endpoint bounds");
		}
		validateLeases(edgeIdRanges);

		NativeBulkEdgePublicationPreflightResult result;
		SourceReplayFingerprint sourceFingerprint;
		NativeBulkEdgeEndpointCache validatedEndpoints;
		uint64_t previousEdgeId = 0U;
		size_t sourceRunBytes = RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES;
		NativeBulkEdgeRecordCursor cursor = source();
		while (const std::optional<NativeBulkEdgeRecord> record = cursor()) {
			if (record->edge_id == 0U || record->source_node_id == 0U || record->target_node_id == 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge publication preflight requires nonzero logical identities");
			}
			const size_t encodedBytes = NativeBulkEdgeRecordCodec::encodedSize(*record);
			if (encodedBytes > std::numeric_limits<size_t>::max() - RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native bulk Edge publication preflight source record size overflowed");
			}
			const size_t sourceRecordBytes = encodedBytes + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES;
			if (result.record_count == options.maximum_records ||
				record->edge_id <= previousEdgeId || !isLeased(edgeIdRanges, record->edge_id) ||
				!validatedEndpoints.validate(record->source_node_id, persistedNodeExists) ||
				!validatedEndpoints.validate(record->target_node_id, persistedNodeExists) ||
				sourceRecordBytes > options.sort_run_bytes - RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge publication preflight found an invalid lease, order, or persisted endpoint");
			}
			if (sourceRunBytes > options.sort_run_bytes - sourceRecordBytes) {
				checkedIncrement(result.source_run_count,
								 "native bulk Edge publication preflight source fragment count overflowed");
				sourceRunBytes = RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES;
			}
			sourceRunBytes += sourceRecordBytes;
			if (result.record_count == 0U) {
				result.first_edge_id = record->edge_id;
			}
			sourceFingerprint.append(*record);
			result.last_edge_id = record->edge_id;
			previousEdgeId = record->edge_id;
			if (!record->type.empty()) {
				checkedIncrement(result.token_entry_count,
								 "native bulk Edge publication preflight token count overflowed");
			}
			for (const LogicalGraphWriteProperty &property: record->properties) {
				static_cast<void>(property);
				checkedIncrement(result.token_entry_count,
								 "native bulk Edge publication preflight token count overflowed");
			}
			if (!record->properties.empty()) {
				checkedIncrement(result.property_root_count,
								 "native bulk Edge publication preflight Property-root count overflowed");
				if (propertyRootEncodedBytes(*record) > inlinePropertyRootCapacity()) {
					checkedIncrement(result.large_value_count,
										 "native bulk Edge publication preflight LargeValue count overflowed");
				}
			}
			checkedIncrement(result.record_count, "native bulk Edge publication preflight record count overflowed");
		}
		// Per-record validation already enforces positive, strictly increasing Edge IDs;
		// only an empty source remains possible at this boundary.
		if (result.record_count == 0U) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native bulk Edge publication preflight requires a nonempty sealed source");
		}
		checkedIncrement(result.source_run_count,
						 "native bulk Edge publication preflight source fragment count overflowed");
		const NativeBulkEdgeRecordSource replaySource = verifiedReplaySource(source, std::move(sourceFingerprint));

		const auto countToken = [&result](NativeBulkRunFragment &&) {
			checkedIncrement(result.token_fragments, "native bulk Edge token fragment count overflowed");
		};
		NativeBulkEdgeTokenRunBuilder::buildRuns(replaySource, options.maximum_records, options.sort_run_bytes,
											 options.maximum_candidate_fragments, countToken);
		const auto countOutgoing = [&result](NativeBulkRunFragment &&) {
			checkedIncrement(result.outgoing_adjacency_fragments,
							 "native bulk Edge outgoing adjacency fragment count overflowed");
		};
		NativeBulkEdgeAdjacencyRunBuilder::buildRuns(replaySource, edgeIdRanges, options.maximum_records,
													 options.sort_run_bytes, options.maximum_candidate_fragments,
													 GlobalAdjacencyDirection::GAD_OUT, countOutgoing);
		const auto countIncoming = [&result](NativeBulkRunFragment &&) {
			checkedIncrement(result.incoming_adjacency_fragments,
							 "native bulk Edge incoming adjacency fragment count overflowed");
		};
		NativeBulkEdgeAdjacencyRunBuilder::buildRuns(replaySource, edgeIdRanges, options.maximum_records,
													 options.sort_run_bytes, options.maximum_candidate_fragments,
													 GlobalAdjacencyDirection::GAD_IN, countIncoming);
		if (result.derivedCandidateFragments() > options.maximum_candidate_fragments) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native bulk Edge publication preflight exceeds its candidate fragment bound");
		}
		return result;
	}

} // namespace graph::storage
