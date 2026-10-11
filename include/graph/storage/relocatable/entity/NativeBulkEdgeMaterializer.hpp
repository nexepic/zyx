/**
 * @file NativeBulkEdgeMaterializer.hpp
 * @brief Typed, bounded conversion of sealed streamed Edge scratch records.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "graph/storage/relocatable/entity/EntityPage.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

namespace graph::storage {

	// The materializer deliberately emits one fully resolved record at a time.
	// The streaming page publisher owns any page-local buffering; callers cannot
	// obtain a record-linear intermediate vector from this boundary.
	struct NativeBulkEdgeMaterializedRecord {
		uint64_t edge_id = 0U;
		EdgeEntityRecord entity;
		PropertyCollectionRoot properties;
	};

	struct NativeBulkEdgeMaterializerResolvers {
		std::function<std::optional<uint64_t>(std::string_view)> relationship_type_token;
		std::function<std::optional<uint32_t>(std::string_view)> property_key_token;
	};

	using NativeBulkEdgeMaterializedRecordVisitor =
			std::function<void(NativeBulkEdgeMaterializedRecord &&)>;

	class NativeBulkEdgeMaterializer final {
	public:
		// Rewinds and validates the sealed source, then emits at most
		// maximum_records records. The exact edge lease is checked per record so
		// a malformed or foreign scratch manifest cannot publish an unleased ID.
		static void materialize(const NativeBulkEdgeRecordSource &source,
								std::span<const NativeGraphLogicalIdLeaseRange> edge_id_ranges,
								size_t maximum_records,
								const NativeBulkEdgeMaterializerResolvers &resolvers,
								const NativeBulkEdgeMaterializedRecordVisitor &visitor);
	};

} // namespace graph::storage
