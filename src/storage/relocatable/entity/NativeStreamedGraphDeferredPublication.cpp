/**
 * @file NativeStreamedGraphDeferredPublication.cpp
 * @brief Callback-scoped typed page sources for one streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphDeferredPublication.hpp"

#include <limits>
#include <utility>

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateLeaseRanges(std::span<const NativeGraphLogicalIdLeaseRange> ranges) {
			if (ranges.empty()) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native streamed graph deferred publication requires an exact Edge-ID lease");
			}
			uint64_t previousLast = 0U;
			for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
				if (range.first_id == 0U || range.count == 0U ||
					range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id ||
					(previousLast != 0U && range.first_id <= previousLast)) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native streamed graph deferred publication has invalid ordered Edge-ID leases");
				}
				previousLast = range.first_id + range.count - 1U;
			}
		}

		[[nodiscard]] bool containsLeaseId(std::span<const NativeGraphLogicalIdLeaseRange> ranges, uint64_t id) {
			for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
				if (id < range.first_id) {
					return false;
				}
				if (id - range.first_id < range.count) {
					return true;
				}
			}
			return false;
		}

		struct SourceSpan {
			uint64_t first_edge_id = 0U;
			uint64_t last_edge_id = 0U;
		};

		[[nodiscard]] SourceSpan deriveSourceSpan(const NativeBulkEdgeRecordSource &source, uint64_t expectedCount) {
			if (!source || expectedCount == 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native streamed graph deferred publication cannot derive an empty source span");
			}
			SourceSpan result;
			uint64_t previous = 0U;
			uint64_t count = 0U;
			NativeBulkEdgeRecordCursor cursor = source();
			while (const std::optional<NativeBulkEdgeRecord> record = cursor()) {
				if (record->edge_id == 0U || record->edge_id <= previous || count == expectedCount) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed graph deferred source diverged while deriving its Edge-ID span");
				}
				if (count == 0U) {
					result.first_edge_id = record->edge_id;
				}
				result.last_edge_id = record->edge_id;
				previous = record->edge_id;
				++count;
			}
			if (count != expectedCount || result.first_edge_id == 0U || result.last_edge_id < result.first_edge_id) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed graph deferred source has no canonical Edge-ID span");
			}
			return result;
		}

		[[nodiscard]] std::optional<SourceSpan>
		deriveCompleteSingleLeaseSpan(std::span<const NativeGraphLogicalIdLeaseRange> ranges, uint64_t recordCount) {
			if (ranges.size() != 1U || ranges.front().count != recordCount) {
				return std::nullopt;
			}
			return SourceSpan{ranges.front().first_id, ranges.front().first_id + recordCount - 1U};
		}

	} // namespace

	NativeStreamedGraphDeferredPublication NativeStreamedGraphDeferredPublication::create(
			const NativeBulkEdgeRecordSource &source, std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
			const NativeStreamedGraphPublicationPlan &publicationPlan,
			const NativeBulkEdgeMaterializerResolvers &resolvers) {
		if (!source || publicationPlan.record_count == 0U || publicationPlan.edge_entity_page_count == 0U ||
			publicationPlan.record_count > std::numeric_limits<size_t>::max() || !resolvers.relationship_type_token ||
			!resolvers.property_key_token) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed graph deferred publication requires a complete private typed source");
		}
		validateLeaseRanges(edgeIdRanges);
		const bool hasDeclaredSpan = publicationPlan.first_edge_id != 0U || publicationPlan.last_edge_id != 0U;
		// Legacy count-only fixtures can describe exactly one fully consumed lease.
		// Do not reopen their source here: every source pass must remain observable
		// by the materializers so replay divergence is detected at that boundary.
		const std::optional<SourceSpan> completeLeaseSpan =
				deriveCompleteSingleLeaseSpan(edgeIdRanges, publicationPlan.record_count);
		const SourceSpan sourceSpan = hasDeclaredSpan
										 ? SourceSpan{publicationPlan.first_edge_id, publicationPlan.last_edge_id}
										 : completeLeaseSpan.has_value()
													? *completeLeaseSpan
													: deriveSourceSpan(source, publicationPlan.record_count);
		if (sourceSpan.first_edge_id == 0U || sourceSpan.last_edge_id < sourceSpan.first_edge_id) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed graph deferred publication has an incomplete Edge-ID span");
		}
		if (!containsLeaseId(edgeIdRanges, sourceSpan.first_edge_id) ||
			!containsLeaseId(edgeIdRanges, sourceSpan.last_edge_id)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed graph deferred publication has an Edge-ID span outside its durable leases");
		}
		const size_t capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_EDGE);
		const size_t recordCount = static_cast<size_t>(publicationPlan.record_count);
		if (capacity == 0U || (recordCount - 1U) / capacity + 1U != publicationPlan.edge_entity_page_count) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed graph deferred publication has a noncanonical Entity-page shape");
		}
		return {source,
				std::vector<NativeGraphLogicalIdLeaseRange>(edgeIdRanges.begin(), edgeIdRanges.end()),
				resolvers,
				publicationPlan.record_count,
				sourceSpan.first_edge_id,
				sourceSpan.last_edge_id,
				publicationPlan.edge_entity_page_count};
	}

	void NativeStreamedGraphDeferredPublication::visitEntityPages(
			const NativeBulkEdgeEntityPageBatchVisitor &visitor) const {
		if (!source_ || edge_id_ranges_.empty() || !visitor) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed graph deferred Entity materialization escaped its publication scope");
		}
		size_t pageCount = 0U;
		uint64_t recordCount = 0U;
		uint64_t firstEdgeId = 0U;
		uint64_t lastEdgeId = 0U;
		NativeBulkEdgeEntityPageBatcher::materializePages(
				source_, edge_id_ranges_, static_cast<size_t>(record_count_), resolvers_,
				[&visitor, &pageCount, &recordCount, &firstEdgeId, &lastEdgeId](NativeBulkEdgeEntityPageBatch &&batch) {
					if (batch.records.empty() || batch.records.size() > std::numeric_limits<uint64_t>::max() - recordCount ||
						pageCount == std::numeric_limits<size_t>::max()) {
						fail(FormatError::FERR_INVALID_STATE,
								 "native streamed graph deferred Entity source has an invalid page batch");
					}
					if (firstEdgeId == 0U) {
						firstEdgeId = batch.records.front().edge_id;
					}
					lastEdgeId = batch.records.back().edge_id;
					recordCount += batch.records.size();
					++pageCount;
					visitor(std::move(batch));
				});
		if (recordCount != record_count_ || pageCount != entity_page_count_ || firstEdgeId != first_edge_id_ ||
			lastEdgeId != last_edge_id_) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed graph deferred Entity source diverged from its immutable publication shape");
		}
	}

	void NativeStreamedGraphDeferredPublication::visitPropertyRoots(
			const NativeStreamedGraphPropertyRootVisitor &visitor) const {
		if (!source_ || edge_id_ranges_.empty() || !visitor) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed graph deferred Property materialization escaped its publication scope");
		}
		uint64_t recordCount = 0U;
		uint64_t firstEdgeId = 0U;
		uint64_t lastEdgeId = 0U;
		NativeBulkEdgeMaterializer::materialize(
				source_, edge_id_ranges_, static_cast<size_t>(record_count_), resolvers_,
				[&visitor, &recordCount, &firstEdgeId, &lastEdgeId](NativeBulkEdgeMaterializedRecord &&record) {
					if (recordCount == std::numeric_limits<uint64_t>::max()) {
						fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
								 "native streamed graph deferred Property record count overflowed");
					}
					if (firstEdgeId == 0U) {
						firstEdgeId = record.edge_id;
					}
					lastEdgeId = record.edge_id;
					++recordCount;
					visitor(std::move(record.properties));
				});
		if (recordCount != record_count_ || firstEdgeId != first_edge_id_ || lastEdgeId != last_edge_id_) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed graph deferred Property source diverged from its immutable publication shape");
		}
	}

} // namespace graph::storage
