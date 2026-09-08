/**
 * @file NativeBulkEdgePrivateRunPipeline.cpp
 * @brief Bounded private multi-pass merge implementation.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunPipeline.hpp"

#include <limits>
#include <utility>

#include "graph/debug/PerfTrace.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		class RangeAllocator final {
		public:
			RangeAllocator(const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange range) :
				identities_(identities), range_(range) {
				if (range_.count == 0U || range_.first_ordinal > identities_.size() ||
					range_.count > identities_.size() - range_.first_ordinal) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native bulk Edge private run pipeline candidate range is invalid");
				}
			}

			[[nodiscard]] NativeGraphPrivateSegmentMaterialization allocate() {
				if (next_ordinal_ == range_.count) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge private run pipeline exhausted its immutable candidate range");
				}
				return identities_[range_.first_ordinal + next_ordinal_++];
			}

		private:
			const NativeGraphPrivateSegmentSet &identities_;
			NativeBulkEdgePrivateCandidateRange range_;
			size_t next_ordinal_ = 0U;
		};

		[[nodiscard]] NativeBulkEdgePrivateRunMaterializationOptions
		materializerOptions(const NativeBulkEdgePrivateRunPipelineOptions &options) {
			return {options.maximum_open_runs, options.maximum_fragment_bytes, options.maximum_entries,
					options.maximum_manifest_depth, options.duplicate_policy};
		}

		void validate(const NativeBulkEdgePrivateRunPipelineOptions &options,
					  const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
					  const NativeGraphPrivateSegmentFence &fence) {
			if (options.maximum_open_runs < 2U || options.maximum_open_runs == std::numeric_limits<size_t>::max() ||
				options.maximum_fragment_bytes <= RSS_NATIVE_BULK_RUN_HEADER_BYTES ||
				options.maximum_entries == 0U || options.maximum_entries == std::numeric_limits<size_t>::max() ||
				options.maximum_manifest_depth == 0U || !reader || !writer || !fence) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge private run pipeline requires finite bounds and scoped private callbacks");
			}
			if (options.maximum_fragment_bytes > RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge private run pipeline has an invalid candidate-fragment bound");
			}
		}

		[[nodiscard]] NativeBulkRunFragmentLocation
		reduceFromRunSet(const NativeGraphPrivateSegmentSet &identities, RangeAllocator &allocator,
						 NativeBulkRunFragmentLocation current, const NativeBulkEdgePrivateRunPipelineOptions &options,
						 const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
						 const NativeGraphPrivateSegmentFence &fence) {
			for (;;) {
				debug::ScopedPerfTimer timer("native_graph.streamed.private_prepare.run_reduce");
				const NativeBulkEdgeRunSource input =
						nativeBulkEdgePrivateRunSetSource(identities, current, options.maximum_manifest_depth, reader);
				NativeBulkEdgeRunCursor cursor = input();
				const std::optional<OrderedBTreeEntrySource> first = cursor();
				if (!first.has_value()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge private run pipeline reopened an empty run-set");
				}
				if (!cursor().has_value()) {
					return nativeBulkEdgePrivateRunSetSingleRun(identities, current, reader);
				}

				const NativeBulkEdgePrivateCandidateAllocator allocate = [&allocator] { return allocator.allocate(); };
				NativeBulkEdgePrivateRunSetBuilder nextSet(options.maximum_manifest_depth, allocate, writer);
				NativeBulkEdgePrivateRunMaterializer::materializeOneLevel(
						input, materializerOptions(options), allocate, writer,
						[&nextSet](NativeBulkRunFragmentLocation root) { nextSet.append(root); });
				current = nextSet.finish();
				fence();
			}
		}

	} // namespace

	NativeBulkRunFragmentLocation NativeBulkEdgePrivateRunPipeline::materialize(
			const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange candidateRange,
			const NativeBulkEdgeRunSource &initialRuns, const NativeBulkEdgePrivateRunPipelineOptions &options,
			const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
			const NativeGraphPrivateSegmentFence &fence) {
		if (!initialRuns) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge private run pipeline requires an initial source");
		}
		validate(options, reader, writer, fence);
		RangeAllocator allocator(identities, candidateRange);
		const NativeBulkEdgePrivateCandidateAllocator allocate = [&allocator] { return allocator.allocate(); };
		NativeBulkEdgePrivateRunSetBuilder initialSet(options.maximum_manifest_depth, allocate, writer);
		NativeBulkEdgePrivateRunMaterializer::materializeOneLevel(
				initialRuns, materializerOptions(options), allocate, writer,
				[&initialSet](NativeBulkRunFragmentLocation root) { initialSet.append(root); });
		NativeBulkRunFragmentLocation current = initialSet.finish();
		fence();

		return reduceFromRunSet(identities, allocator, current, options, reader, writer, fence);
	}

	size_t NativeBulkEdgePrivateRunPipeline::maximumCandidateSegments(size_t initialFragmentCount,
																	  size_t maximumOpenRuns) {
		if (maximumOpenRuns < 2U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge private run pipeline requires a usable merge fan-in");
		}
		if (initialFragmentCount == 0U) {
			return 0U;
		}
		const auto appendLevel = [](size_t &candidateCount, size_t fragmentCount) {
			if (fragmentCount > std::numeric_limits<size_t>::max() / 3U ||
				fragmentCount * 3U > std::numeric_limits<size_t>::max() - candidateCount) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native bulk Edge private run pipeline candidate count overflowed");
			}
			candidateCount += fragmentCount * 3U;
		};
		size_t result = 0U;
		appendLevel(result, initialFragmentCount);
		for (size_t runCount = initialFragmentCount; runCount > 1U;) {
			appendLevel(result, initialFragmentCount);
			runCount = (runCount - 1U) / maximumOpenRuns + 1U;
		}
		return result;
	}

	NativeBulkRunFragmentLocation NativeBulkEdgePrivateRunPipeline::materializeInitialFragments(
			const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange candidateRange,
			const NativeBulkEdgePrivateInitialRunProducer &produceInitialRuns,
			const NativeBulkEdgePrivateRunPipelineOptions &options, const NativeGraphPrivateSegmentReader &reader,
			const NativeGraphPrivateSegmentWriter &writer, const NativeGraphPrivateSegmentFence &fence) {
		if (!produceInitialRuns) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native bulk Edge private run pipeline requires an initial fragment producer");
		}
		validate(options, reader, writer, fence);
		RangeAllocator allocator(identities, candidateRange);
		const NativeBulkEdgePrivateCandidateAllocator allocate = [&allocator] { return allocator.allocate(); };
		NativeBulkEdgePrivateRunSetBuilder initialSet(options.maximum_manifest_depth, allocate, writer);
		bool sawInitialRun = false;
		produceInitialRuns([&](NativeBulkRunFragment &&fragment) {
			if (fragment.entries.empty()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge private run pipeline received an empty initial fragment");
			}
			sawInitialRun = true;
			initialSet.append(NativeBulkEdgePrivateRunMaterializer::materializeInitialFragment(
					std::move(fragment), options.maximum_manifest_depth, allocate, writer));
		});
		if (!sawInitialRun) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native bulk Edge private run pipeline initial fragment producer emitted no runs");
		}
		const NativeBulkRunFragmentLocation current = initialSet.finish();
		fence();
		return reduceFromRunSet(identities, allocator, current, options, reader, writer, fence);
	}

	std::pair<NativeBulkRunFragmentLocation, NativeBulkRunFragmentLocation>
	NativeBulkEdgePrivateRunPipeline::materializeInitialFragmentPair(
			const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange firstRange,
			NativeBulkEdgePrivateCandidateRange secondRange,
			const NativeBulkEdgePrivateInitialRunPairProducer &produceInitialRuns,
			const NativeBulkEdgePrivateRunPipelineOptions &firstOptions,
			const NativeBulkEdgePrivateRunPipelineOptions &secondOptions, const NativeGraphPrivateSegmentReader &reader,
			const NativeGraphPrivateSegmentWriter &writer, const NativeGraphPrivateSegmentFence &fence) {
		if (!produceInitialRuns) {
			fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge private run pipeline requires paired initial fragment producers");
		}
		validate(firstOptions, reader, writer, fence);
		validate(secondOptions, reader, writer, fence);
		RangeAllocator firstAllocator(identities, firstRange);
		RangeAllocator secondAllocator(identities, secondRange);
		const bool rangesOverlap = firstRange.first_ordinal < secondRange.first_ordinal
										? firstRange.count > secondRange.first_ordinal - firstRange.first_ordinal
										: secondRange.count > firstRange.first_ordinal - secondRange.first_ordinal;
		if (rangesOverlap) {
			fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge private run pipeline paired candidate ranges overlap");
		}
		const NativeBulkEdgePrivateCandidateAllocator firstAllocate = [&firstAllocator] {
			return firstAllocator.allocate();
		};
		const NativeBulkEdgePrivateCandidateAllocator secondAllocate = [&secondAllocator] {
			return secondAllocator.allocate();
		};
		NativeBulkEdgePrivateRunSetBuilder firstSet(firstOptions.maximum_manifest_depth, firstAllocate, writer);
		NativeBulkEdgePrivateRunSetBuilder secondSet(secondOptions.maximum_manifest_depth, secondAllocate, writer);
		bool sawFirst = false;
		bool sawSecond = false;
		const auto append = [&writer](NativeBulkEdgePrivateRunSetBuilder &set, bool &saw,
									 const NativeBulkEdgePrivateRunPipelineOptions &options,
									 const NativeBulkEdgePrivateCandidateAllocator &allocate,
									 NativeBulkRunFragment &&fragment) {
			if (fragment.entries.empty()) {
				fail(FormatError::FERR_INVALID_STATE,
						 "native bulk Edge private run pipeline received an empty paired initial fragment");
			}
			saw = true;
			set.append(NativeBulkEdgePrivateRunMaterializer::materializeInitialFragment(
					std::move(fragment), options.maximum_manifest_depth, allocate, writer));
		};
		produceInitialRuns(
				[&](NativeBulkRunFragment &&fragment) {
					append(firstSet, sawFirst, firstOptions, firstAllocate, std::move(fragment));
				},
				[&](NativeBulkRunFragment &&fragment) {
					append(secondSet, sawSecond, secondOptions, secondAllocate, std::move(fragment));
				});
		if (!sawFirst || !sawSecond) {
			fail(FormatError::FERR_INVALID_STATE,
					 "native bulk Edge private run pipeline paired producer omitted one direction");
		}
		const NativeBulkRunFragmentLocation firstCurrent = firstSet.finish();
		const NativeBulkRunFragmentLocation secondCurrent = secondSet.finish();
		fence();
		return {reduceFromRunSet(identities, firstAllocator, firstCurrent, firstOptions, reader, writer, fence),
				reduceFromRunSet(identities, secondAllocator, secondCurrent, secondOptions, reader, writer, fence)};
	}

} // namespace graph::storage
