/**
 * @file NativeBulkEdgeTokenRunBuilder.hpp
 * @brief Bounded sorted Catalog-token runs from sealed streamed Edge scratch.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <functional>

#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"

namespace graph::storage {

	// These values are private scratch sort keys, not persisted Catalog enums.
	// Prefixing the names keeps the enum safe from Windows macros.
	enum class NativeBulkEdgeTokenKind : uint8_t {
		NBETK_PROPERTY = 1U,
		NBETK_RELATIONSHIP_TYPE = 2U,
	};

	using NativeBulkEdgeTokenRunVisitor = std::function<void(NativeBulkRunFragment &&)>;

	class NativeBulkEdgeTokenRunBuilder final {
	public:
		// Rewinds a sealed Edge source and emits independently sorted, deduplicated
		// bounded runs. A later publication coordinator merges these runs while it
		// owns one SystemCatalog write session; this builder never creates tokens or
		// retains a transaction-wide name map.
		static void buildRuns(const NativeBulkEdgeRecordSource &source, size_t maximumRecords,
							  size_t maximumRunBytes, size_t maximumFragments,
							  const NativeBulkEdgeTokenRunVisitor &visitor);
		[[nodiscard]] static std::vector<std::byte> encodeKey(NativeBulkEdgeTokenKind kind, std::string_view name);
	};

} // namespace graph::storage
