/**
 * @file CatalogSlotRelocation.hpp
 * @brief Shared contracts for atomic Catalog-slot relocation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <functional>

#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/Catalog.hpp"

namespace graph::storage {

	struct CatalogSlotRelocation {
		Superblock superblock;
		CatalogEntry source;
		CatalogEntry destination;
	};

	// Runs under the metadata writer after the destination slot and all candidate
	// metadata roots have been staged, but before their Superblock publication.
	// The callback must durably copy and validate source bytes at the destination.
	using CatalogSlotRelocationPreparer = std::function<void(const CatalogEntry &, const CatalogEntry &)>;

} // namespace graph::storage
