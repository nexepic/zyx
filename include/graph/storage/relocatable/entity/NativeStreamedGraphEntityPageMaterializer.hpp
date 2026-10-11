/**
 * @file NativeStreamedGraphEntityPageMaterializer.hpp
 * @brief Bounded Edge Entity-page images for streamed graph publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>

#include "graph/storage/relocatable/entity/NativeStreamedGraphDeferredPublication.hpp"

namespace graph::storage {

	// Metadata owns final PageId and pool placement. This value carries only the
	// already-assigned owner identity needed by the fixed Entity-page codec.
	struct NativeStreamedGraphEntityPageAssignment {
		PageOwnerHeader owner;
	};

	struct NativeStreamedGraphEntityPage {
		PageOwnerHeader owner;
		std::vector<std::byte> payload;
		std::vector<EntityPageLocation> edge_locations;
	};

	using NativeStreamedGraphEntityPageAssignmentSource =
			std::function<NativeStreamedGraphEntityPageAssignment(size_t pageOrdinal)>;
	using NativeStreamedGraphEntityPageVisitor = std::function<void(NativeStreamedGraphEntityPage &&)>;

	// Converts the callback-scoped typed Edge source into complete fixed-page
	// images. The maximum retained state is one codec-sized Entity page plus its
	// mappings; callers must consume each image before the next source batch.
	class NativeStreamedGraphEntityPageMaterializer final {
	public:
		static void materialize(const NativeStreamedGraphDeferredPublication &publication,
									const NativeStreamedGraphEntityPageAssignmentSource &assign,
									const NativeStreamedGraphEntityPageVisitor &visit);
	};

} // namespace graph::storage
