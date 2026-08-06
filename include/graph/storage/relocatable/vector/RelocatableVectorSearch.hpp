/**
 * @file RelocatableVectorSearch.hpp
 * @brief Generation-checked exact and derived Vector search for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <vector>

#include "graph/storage/LogicalGraphVectorSearch.hpp"
#include "graph/storage/relocatable/schema/IndexDefinition.hpp"
#include "graph/storage/relocatable/vector/VectorManifestPage.hpp"

namespace graph::storage {

	class BTreePageStore;
	class MetadataSnapshotPin;
	class RelocatableMetadataStore;
	class VectorAdjacencyPageStore;
	class VectorCodebookStore;
	class VectorPageStore;

	// Keeps physical Vector validation and candidate traversal behind the
	// format-specific boundary. The logical query layer sees only Node IDs and
	// exact scores, never Page, slot, Codebook, or derived mapping identities.
	class RelocatableVectorSearch final {
	public:
		[[nodiscard]] static LogicalGraphVectorSearchResult
		search(const MetadataSnapshotPin &snapshot, const IndexDefinition &definition, const VectorManifest &manifest,
			   const LogicalGraphVectorSearchRequest &request, BTreePageStore &trees, VectorPageStore &vectors,
			   VectorAdjacencyPageStore &adjacencies, VectorCodebookStore &codebooks, RelocatableMetadataStore &metadata);
	};

} // namespace graph::storage
