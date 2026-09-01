/**
 * @file NativeStreamedGraphLargeValueContentRunBuilder.cpp
 * @brief Bounded content-index runs for streamed LargeValue publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueContentRunBuilder.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunPipeline.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] size_t encodedRunEntryBytes(const BTreePageEntry &entry) {
			const size_t encodedSize = BTreePageEntryCodec::encodedSize(entry);
		if (encodedSize > std::numeric_limits<size_t>::max() - RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES) { // ZYX_COV_EXCL_LINE: BTreePageEntryCodec keys are bounded well below size_t overflow.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native streamed Edge LargeValue content-index entry size overflowed");
			}
			return RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + encodedSize;
		}

		[[nodiscard]] size_t entriesPerRun(const LargeValueStore &store, size_t maximumRunBytes) {
			LargeValueRef representative;
			representative.value_id = LargeValueId(1U);
			representative.kind = LargeValueKind::LVK_BYTES;
			representative.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
			const size_t entryBytes =
					encodedRunEntryBytes({store.contentIndexKey(representative), representative.value_id.value()});
			if (maximumRunBytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES ||
				entryBytes > maximumRunBytes - RSS_NATIVE_BULK_RUN_HEADER_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native streamed Edge LargeValue content-index entry cannot fit in a private run");
			}
			return (maximumRunBytes - RSS_NATIVE_BULK_RUN_HEADER_BYTES) / entryBytes;
		}

		void emitRun(NativeBulkRunFragment &run, size_t maximumFragments, size_t &emittedFragments,
					 const NativeStreamedGraphLargeValueContentRunVisitor &visitor) {
			if (run.entries.empty()) { // ZYX_COV_EXCL_LINE: emitRun is called only after appending an entry or at finalization with a nonempty current run.
				return;
			}
			std::ranges::sort(run.entries, {}, &BTreePageEntry::key);
			if (std::ranges::adjacent_find(run.entries, {}, &BTreePageEntry::key) != run.entries.end()) { // ZYX_COV_EXCL_LINE: content-index keys include the unique LargeValue identity suffix.
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge LargeValue content-index run has duplicate durable keys");
			}
			if (emittedFragments == maximumFragments) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native streamed Edge LargeValue content-index run exceeds its candidate fragment bound");
			}
			visitor(std::move(run));
			++emittedFragments;
			run = {};
		}

	} // namespace

	NativeStreamedGraphLargeValueContentRunPlan NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
			const LargeValueStore &store, size_t valueCount, size_t maximumRunBytes, size_t maximumOpenRuns) {
		if (maximumOpenRuns < 2U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue content-index plan requires a usable merge fan-in");
		}
		if (valueCount == 0U) {
			return {};
		}
		const size_t perRun = entriesPerRun(store, maximumRunBytes);
		if (valueCount - 1U > std::numeric_limits<size_t>::max() - perRun) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native streamed Edge LargeValue content-index fragment count overflowed");
		}
		NativeStreamedGraphLargeValueContentRunPlan result;
		result.initial_fragment_count = (valueCount - 1U) / perRun + 1U;
		result.candidate_segment_count = NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(
				result.initial_fragment_count, maximumOpenRuns);
		return result;
	}

	NativeStreamedGraphLargeValueContentRunPlan NativeStreamedGraphLargeValueContentRunBuilder::derivePlan(
			const NativeStreamedGraphDeferredPublication &publication, const LargeValueStore &store,
			LargeValueId firstValueId, const NativeStreamedGraphLargeValuePlan &plan,
			const NativeStreamedGraphLargeValuePlanningOptions &options, size_t maximumRunBytes,
			size_t maximumOpenRuns) {
		if (maximumOpenRuns < 2U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue content-index plan requires a usable merge fan-in");
		}
		if (plan.value_count == 0U) {
			return {};
		}
		NativeStreamedGraphLargeValueContentRunPlan result;
		buildRuns(publication, store, firstValueId, plan, options, maximumRunBytes, std::numeric_limits<size_t>::max(),
				  [&result](NativeBulkRunFragment &&) {
					if (result.initial_fragment_count == std::numeric_limits<size_t>::max()) { // ZYX_COV_EXCL_LINE: materialized LargeValue count is bounded by the source and candidate layout.
						  fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
							   "native streamed Edge LargeValue content-index fragment count overflowed");
					  }
					  ++result.initial_fragment_count;
				  });
		const NativeStreamedGraphLargeValueContentRunPlan counted =
				derivePlanForValueCount(store, plan.value_count, maximumRunBytes, maximumOpenRuns);
		if (result.initial_fragment_count != counted.initial_fragment_count) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native streamed Edge LargeValue content-index source changed its fixed entry shape");
		}
		result.candidate_segment_count = counted.candidate_segment_count;
		return result;
	}

	void NativeStreamedGraphLargeValueContentRunBuilder::buildRuns(
			const NativeStreamedGraphDeferredPublication &publication, const LargeValueStore &store,
			LargeValueId firstValueId, const NativeStreamedGraphLargeValuePlan &plan,
			const NativeStreamedGraphLargeValuePlanningOptions &options, size_t maximumRunBytes,
			size_t maximumFragments, const NativeStreamedGraphLargeValueContentRunVisitor &visitor) {
		if (!firstValueId.isValid() || plan.value_count == 0U || maximumRunBytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES ||
			maximumRunBytes > RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES || maximumFragments == 0U || !visitor) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge LargeValue content-index runs require finite nonempty bounds and a consumer");
		}

		NativeBulkRunFragment current;
		size_t currentBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
		size_t emittedFragments = 0U;
		NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
				publication, store, firstValueId, plan, options, [&](NativeStreamedGraphLargeValueItem &&item) {
					const BTreePageEntry entry{store.contentIndexKey(item.stored_reference.value),
											   item.stored_reference.value.value_id.value()};
					const size_t entryBytes = encodedRunEntryBytes(entry);
					if (entryBytes > maximumRunBytes - RSS_NATIVE_BULK_RUN_HEADER_BYTES) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "native streamed Edge LargeValue content-index entry cannot fit in a private run");
					}
					if (currentBytes > maximumRunBytes - entryBytes) {
						emitRun(current, maximumFragments, emittedFragments, visitor);
						currentBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
					}
					current.entries.push_back(entry);
					currentBytes += entryBytes;
				});
		emitRun(current, maximumFragments, emittedFragments, visitor);
	}

} // namespace graph::storage
