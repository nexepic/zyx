/**
 * @file MetadataDirectoryEvacuation.hpp
 * @brief Fail-closed copy-on-write cloning for bootstrap metadata directories.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <optional>

#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

namespace graph::storage {

	// Rewrites every reachable metadata directory into fresh COW pages and
	// updates the matching roots in `batch`. The caller owns publication and
	// source-pool retirement so no partially cloned root can become visible.
	void cloneMetadataDirectoryRootsForEvacuation(MetadataPageFile &pages, MetadataWriteBatch &batch,
																				const Superblock &current);

	// Relocates only the route targets owned by one metadata pool. A missing
	// route target, physical bootstrap anchor, or unrouteable physical child
	// returns no value so callers can retain the conservative fallback.
	[[nodiscard]] bool canRelocateRoutedMetadataPoolPages(MetadataPageFile &pages, const Superblock &current,
															 PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames);
	[[nodiscard]] std::optional<uint32_t>
	relocateRoutedMetadataPoolPages(MetadataPageFile &pages, MetadataWriteBatch &batch, const Superblock &current,
																					 PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames);

	// A mixed source is eligible only when every allocated frame is a current
	// typed route target, validated route-branch plumbing, or the current
	// bootstrap header. It relocates targets and rebuilds the physical plumbing
	// through one common publication; any other frame remains on fallback.
	[[nodiscard]] bool canEvacuateMixedRoutedMetadataPool(MetadataPageFile &pages, const Superblock &current,
																					 PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames);
	[[nodiscard]] std::optional<uint32_t>
	evacuateMixedRoutedMetadataPool(MetadataPageFile &pages, MetadataWriteBatch &batch, const Superblock &current,
																				 PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames);

	// Refreshes only a source pool containing the complete physical route-branch
	// plumbing. The refreshed branch and bootstrap header are published by the
	// common batch path; any other pool remains on the conservative clone path.
	[[nodiscard]] bool canRefreshMetadataRouteBranchForEvacuation(MetadataPageFile &pages,
																 const Superblock &current,
																 PhysicalSlotId sourcePoolSlot,
																 uint32_t sourceAllocatedFrames);
	[[nodiscard]] std::optional<uint32_t>
	refreshMetadataRouteBranchForEvacuation(MetadataPageFile &pages, MetadataWriteBatch &batch,
														  const Superblock &current, PhysicalSlotId sourcePoolSlot,
														  uint32_t sourceAllocatedFrames);

	// A source pool holding only the currently published bootstrap header can be
	// retired by rebuilding that header through the common route publication.
	[[nodiscard]] bool canRefreshBootstrapHeaderForEvacuation(MetadataPageFile &pages, const Superblock &current,
																  PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames);
	[[nodiscard]] std::optional<uint32_t>
	refreshBootstrapHeaderForEvacuation(MetadataPageFile &pages, MetadataWriteBatch &batch, const Superblock &current,
														 PhysicalSlotId sourcePoolSlot, uint32_t sourceAllocatedFrames);

} // namespace graph::storage
