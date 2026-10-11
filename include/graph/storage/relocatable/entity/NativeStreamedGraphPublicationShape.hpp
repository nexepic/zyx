/**
 * @file NativeStreamedGraphPublicationShape.hpp
 * @brief Exact public-page cardinalities for one streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValuePlanner.hpp"

namespace graph::storage {

	// The final coordinator needs all logical-page identities before it writes a
	// single public frame. Keep that allocation shape scalar-only: it is derived
	// from rewindable sources and never retains an import-sized page or value list.
	struct NativeStreamedGraphPublicationShape {
		size_t entity_page_count = 0U;
		size_t property_page_count = 0U;
		size_t property_root_count = 0U;
		size_t large_value_count = 0U;
		size_t large_value_data_page_count = 0U;
		size_t large_value_manifest_page_count = 0U;
		size_t total_public_page_count = 0U;

		[[nodiscard]] static NativeStreamedGraphPublicationShape
		derive(const NativeStreamedGraphPublicationPlan &plan, const NativeStreamedGraphLargeValuePlan &largeValues);
	};

} // namespace graph::storage
