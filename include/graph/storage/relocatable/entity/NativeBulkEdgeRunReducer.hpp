/**
 * @file NativeBulkEdgeRunReducer.hpp
 * @brief One bounded fan-in reduction level for streamed Edge-derived runs.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>
#include <optional>

#include "graph/storage/relocatable/index/NativeBulkRun.hpp"

namespace graph::storage {

	// The enum stays private to run reduction; it prevents a token-name duplicate
	// from being confused with a duplicate adjacency membership key.
	enum class NativeBulkEdgeRunDuplicatePolicy : uint8_t {
		NBERDP_REJECT = 1U,
		NBERDP_KEEP_ONE = 2U,
	};

	// A run is an independently rewindable ordered entry source. It may be
	// backed by several physical fragments under one private manifest.
	using NativeBulkEdgeRunCursor = std::function<std::optional<OrderedBTreeEntrySource>()>;
	using NativeBulkEdgeRunSource = std::function<NativeBulkEdgeRunCursor()>;
	// `closes_run` distinguishes a physical fragment boundary from a logical run
	// boundary. The candidate stager must build one manifest per completed run;
	// treating every split fragment as a later merge input can prevent fan-in
	// reduction from converging for large runs.
	using NativeBulkEdgeRunFragmentVisitor =
			std::function<void(NativeBulkRunFragment &&, bool closesRun)>;

	// Reduces exactly one sorted-run level. The caller persists each emitted
	// fragment in its writer-owned private candidate set and opens that set again
	// for a later level. This keeps peak resident state to one fan-in group,
	// rather than every run in a streamed transaction.
	class NativeBulkEdgeRunReducer final {
	public:
		static void reduceOneLevel(const NativeBulkEdgeRunSource &input, size_t maximumOpenRuns,
							   size_t maximumFragmentBytes, size_t maximumEntries,
							   const NativeBulkEdgeRunFragmentVisitor &visitor,
							   NativeBulkEdgeRunDuplicatePolicy duplicatePolicy =
									   NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT);
	};

} // namespace graph::storage
