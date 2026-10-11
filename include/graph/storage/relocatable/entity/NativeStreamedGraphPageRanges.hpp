/**
 * @file NativeStreamedGraphPageRanges.hpp
 * @brief Contiguous logical PageId ranges for streamed Edge publication.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>

#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationShape.hpp"

namespace graph::storage {

	// MetadataWriteBatch owns allocation of the first PageId. This value object
	// maps the already reserved contiguous range to typed page families without
	// storing a per-page allocation table.
	struct NativeStreamedGraphPageRanges {
		PageId first_entity_page{};
		PageId first_property_page{};
		// A LargeValue emits data frames followed by its Manifest frames. Keep this
		// one contiguous stream so the bounded Page Directory append session never
		// needs to retain per-value Manifest locations for a later ordered pass.
		PageId first_large_value_page{};
		NativeStreamedGraphPublicationShape shape;

		[[nodiscard]] static NativeStreamedGraphPageRanges derive(PageId firstPageId,
														 const NativeStreamedGraphPublicationShape &shape);
		[[nodiscard]] PageId entityPageAt(size_t ordinal) const;
		[[nodiscard]] PageId propertyPageAt(size_t ordinal) const;
		[[nodiscard]] PageId largeValuePageAt(size_t ordinal) const;
	};

} // namespace graph::storage
