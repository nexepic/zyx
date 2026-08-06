/**
 * @file MetadataDirectoryEvacuation.hpp
 * @brief Fail-closed copy-on-write cloning for bootstrap metadata directories.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

namespace graph::storage {

	// Rewrites every reachable metadata directory into fresh COW pages and
	// updates the matching roots in `batch`. The caller owns publication and
	// source-pool retirement so no partially cloned root can become visible.
	void cloneMetadataDirectoryRootsForEvacuation(MetadataPageFile &pages, MetadataWriteBatch &batch,
														const Superblock &current);

} // namespace graph::storage
