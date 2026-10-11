/**
 * @file NativeBulkEdgePrivateRunPipeline.hpp
 * @brief Bounded private multi-pass merge for streamed Edge-derived run families.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>
#include <utility>

#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateRunSet.hpp"

namespace graph::storage {

	struct NativeBulkEdgePrivateCandidateRange {
		size_t first_ordinal = 0U;
		size_t count = 0U;
	};

	struct NativeBulkEdgePrivateRunPipelineOptions {
		size_t maximum_open_runs = 0U;
		size_t maximum_fragment_bytes = 0U;
		size_t maximum_entries = 0U;
		uint16_t maximum_manifest_depth = 0U;
		NativeBulkEdgeRunDuplicatePolicy duplicate_policy = NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT;
	};

	// A source-specific builder pushes one already sorted initial fragment at a
	// time. It must retain no transaction-wide run list after the callback
	// returns, and it must invoke the callback only with nonempty fragments.
	using NativeBulkEdgePrivateInitialRunProducer =
			std::function<void(const NativeBulkEdgePrivateRunFragmentVisitor &)>;
	// Emits two independently sorted fragment streams during one bounded source
	// pass. Each stream still receives its own candidate range and merge state.
	using NativeBulkEdgePrivateInitialRunPairProducer = std::function<void(
			const NativeBulkEdgePrivateRunFragmentVisitor &, const NativeBulkEdgePrivateRunFragmentVisitor &)>;

	// Writes each initial run and later reduction level into distinct reserved
	// candidates, then reopens only the final manifest. The pipeline's state is
	// bounded by one reducer group, manifest/run-set fan-out buffers, and depth.
	class NativeBulkEdgePrivateRunPipeline final {
	public:
		// Retains every initial fragment and reduction level until the enclosing
		// metadata generation can publish or discard its private candidate suffix.
		[[nodiscard]] static size_t maximumCandidateSegments(size_t initialFragmentCount, size_t maximumOpenRuns);
		[[nodiscard]] static NativeBulkRunFragmentLocation materializeInitialFragments(
				const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange candidateRange,
				const NativeBulkEdgePrivateInitialRunProducer &produceInitialRuns,
				const NativeBulkEdgePrivateRunPipelineOptions &options, const NativeGraphPrivateSegmentReader &reader,
				const NativeGraphPrivateSegmentWriter &writer, const NativeGraphPrivateSegmentFence &fence);
		[[nodiscard]] static std::pair<NativeBulkRunFragmentLocation, NativeBulkRunFragmentLocation>
		materializeInitialFragmentPair(
				const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange firstRange,
				NativeBulkEdgePrivateCandidateRange secondRange,
				const NativeBulkEdgePrivateInitialRunPairProducer &produceInitialRuns,
				const NativeBulkEdgePrivateRunPipelineOptions &firstOptions,
				const NativeBulkEdgePrivateRunPipelineOptions &secondOptions,
				const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
				const NativeGraphPrivateSegmentFence &fence);

		[[nodiscard]] static NativeBulkRunFragmentLocation
		materialize(const NativeGraphPrivateSegmentSet &identities, NativeBulkEdgePrivateCandidateRange candidateRange,
					const NativeBulkEdgeRunSource &initialRuns, const NativeBulkEdgePrivateRunPipelineOptions &options,
					const NativeGraphPrivateSegmentReader &reader, const NativeGraphPrivateSegmentWriter &writer,
					const NativeGraphPrivateSegmentFence &fence);
	};

} // namespace graph::storage
