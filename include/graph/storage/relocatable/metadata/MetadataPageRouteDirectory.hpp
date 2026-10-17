/**
 * @file MetadataPageRouteDirectory.hpp
 * @brief Physical COW branch for resolving typed metadata page identities.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <span>
#include <vector>
#include "graph/utils/OwnedStdVector.hpp"

#include "graph/storage/relocatable/metadata/MetadataPageRoute.hpp"

namespace graph::storage {

	class MetadataPageFile;
	class MetadataWriteBatch;
	struct Superblock;

	class MetadataPageRouteDirectory final {
	public:
		explicit MetadataPageRouteDirectory(MetadataPageFile &pages);

		// The bootstrap branch is physical plumbing. It receives the complete
		// visible set, writes a fresh COW tree, and deliberately does not register
		// its own radix/leaf frames as logical metadata routes.
		[[nodiscard]] PhysicalPageRef buildFromCompleteRouteSet(MetadataWriteBatch &batch,
													std::span<const MetadataPageRoute> routes);
		// Before the Page Directory bootstrap header is published, the existing
		// Superblock roots provide the complete physical metadata graph. Collect it
		// once so the initial typed route branch cannot omit unchanged COW pages.
		[[nodiscard]] utils::OwnedStdVector<MetadataPageRoute>
		collectRoutesFromPhysicalDirectoryRoots(const Superblock &superblock) const;
		// Returns the exact number of metadata frames needed by a standalone
		// initial migration: route leaves, radix plumbing, and the physical header.
		// The count is used for a preflight reservation before any candidate frame
		// is allocated, so migration cannot perturb normal COW failure ordering.
		[[nodiscard]] size_t initialBootstrapFrameCount(const Superblock &superblock) const;
		// Counts the exact initial route/header frames after ordinary creation
		// metadata COW has converged. This is used by the creation transaction so
		// Catalog ownership pages are included in the reservation.
		[[nodiscard]] size_t initialBootstrapFrameCount(const MetadataWriteBatch &batch) const;
		// Counts the exact branch/header frames needed to overlay the current
		// batch's final routes on a published bootstrap. Callers preflight this
		// after all ordinary metadata COW work has completed.
		[[nodiscard]] size_t nextBootstrapFrameCount(const MetadataWriteBatch &batch) const;
		// Once the bootstrap header is published, this replaces the physical-root
		// collector. Bootstrap pages themselves are validated as traversal state,
		// but never returned as logical metadata routes.
		[[nodiscard]] std::vector<MetadataPageRoute>
		collectRoutesFromRouteBranch(const Superblock &superblock, PhysicalPageRef root) const;
		// Returns the physical plumbing frames of one fully validated route branch.
		// The bootstrap header and all metadata route targets are deliberately
		// excluded so callers can identify a pool that is safe to refresh as a
		// whole without relocating typed metadata pages.
		[[nodiscard]] std::vector<PhysicalPageRef>
		collectRouteBranchFrames(const Superblock &superblock, PhysicalPageRef root) const;
		// Combines the stable current graph with final COW pairs recorded by this
		// batch. This is the initial migration writer; later publications will
		// merge through the already-published typed route branch instead.
		[[nodiscard]] PhysicalPageRef buildInitialRouteBranch(MetadataWriteBatch &batch);
		[[nodiscard]] PhysicalPageRef buildNextRouteBranch(MetadataWriteBatch &batch,
																	PhysicalPageRef currentRoot);
		// Publishes a new physical bootstrap header whose metadata route branch
		// overlays the final route pairs collected by this batch. A valid data-root
		// argument replaces the existing data branch; an invalid reference preserves
		// it for metadata-only COW such as Catalog or Allocator updates.
		[[nodiscard]] PhysicalPageRef publishNextBootstrap(MetadataWriteBatch &batch,
																												PhysicalPageRef dataRoot = {});
		// Advances only the physical bootstrap header for an identity-only
		// publication. The existing route branch remains valid because all route
		// targets are from an earlier visible generation.
		[[nodiscard]] PhysicalPageRef publishIdentityOnlyBootstrap(MetadataWriteBatch &batch);
		// Writes the physical header and changes only the batch candidate. Callers
		// may publish it only after all page-directory readers and recovery paths
		// understand the header; this helper deliberately has no automatic caller.
		[[nodiscard]] PhysicalPageRef prepareInitialBootstrapMigration(MetadataWriteBatch &batch);
		[[nodiscard]] PhysicalPageRef find(const Superblock &superblock, PhysicalPageRef root,
														  MetadataPageId pageId) const;
		// Resolve one radix page's children without restarting the physical route
		// tree for each identity. Results preserve input order, including duplicates.
		void findMany(const Superblock &superblock, PhysicalPageRef root,
					  std::span<const MetadataPageId> pageIds, std::span<PhysicalPageRef> results) const;

	private:
		MetadataPageFile &pages_;
	};

} // namespace graph::storage
