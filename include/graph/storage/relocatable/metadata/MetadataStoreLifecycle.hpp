/**
 * @file MetadataStoreLifecycle.hpp
 * @brief Bootstrap lifecycle for a relocatable single-file metadata store.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <memory>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/Superblock.hpp"

namespace graph::storage {

	// Initializes the immutable bootstrap region and the generation-one allocator
	// state. It accepts only an empty file, so legacy formats can never be
	// silently reinterpreted as the clean single-file layout.
	void initializeRelocatableMetadataFile(const std::shared_ptr<StorageIO> &io, const Superblock &initial);

	// Validates the on-disk format before recovering the active A/B Superblock.
	// Callers construct typed stores only from this returned, verified snapshot.
	[[nodiscard]] Superblock recoverRelocatableMetadataSuperblock(const std::shared_ptr<StorageIO> &io);

} // namespace graph::storage
