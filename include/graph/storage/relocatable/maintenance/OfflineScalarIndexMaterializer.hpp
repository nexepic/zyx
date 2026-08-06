/**
 * @file OfflineScalarIndexMaterializer.hpp
 * @brief Semantic scalar, label, and composite relocatable index materialization.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "graph/storage/relocatable/index/ExternalBTreeEntrySorter.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/schema/IndexDefinition.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

namespace graph::storage {

	struct OfflineScalarIndexMaterializationOptions {
	BulkBTreeBuildOptions build;
	ExternalBTreeEntrySorterOptions sort;
	size_t maximum_definitions = 0;
	size_t maximum_property_names = 0;
	};

	struct OfflineScalarIndexMaterializationResult {
		uint64_t materialized_indexes = 0;
		uint64_t verified_indexes = 0;
		uint64_t deferred_vector_indexes = 0;
		uint64_t empty_indexes_with_unassigned_property_keys = 0;
		uint64_t indexed_entries = 0;
		// These fields describe logical tree contents, rather than this attempt's
		// writes, so a resumed verification produces the same checkpoint digest.
		uint64_t semantic_index_count = 0;
		uint64_t semantic_entry_count = 0;
		uint32_t semantic_crc = 0;

		constexpr bool operator==(const OfflineScalarIndexMaterializationResult &) const = default;
	};

	// Rebuilds logical label, scalar-property, and composite indexes exclusively
	// from native Entity Map records and owner-keyed Property roots. Every source scan is
	// snapshot-pinned, every final tree is bulk built, and an existing root must
	// match the complete sorted semantic stream exactly before resume can skip it.
	class OfflineScalarIndexMaterializer final {
	public:
		// Lets the migration coordinator reject malformed capacities before it
		// publishes any independently recoverable schema metadata.
		static void validateOptions(const OfflineScalarIndexMaterializationOptions &options);
		[[nodiscard]] static OfflineScalarIndexMaterializationResult
		materialize(std::span<const IndexDefinition> definitions, BTreePageStore &pages, EntityPageStore &entities,
					EntityLabelSetStore &labels,
					PropertyCollectionRootStore &properties, SystemCatalog &catalog,
					RelocatableMetadataStore &metadata, const OfflineScalarIndexMaterializationOptions &options);
	};

} // namespace graph::storage
