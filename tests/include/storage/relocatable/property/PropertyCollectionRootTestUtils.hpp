/**
 * @file PropertyCollectionRootTestUtils.hpp
 * @brief Test-only helpers for publishing owner-keyed Property roots.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <limits>
#include <span>

#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"

namespace graph::storage::test {

	inline EntityPageBatchResult publishPropertyRoots(
			EntityPageStore &entities, PropertyCollectionRootStore &properties,
			RelocatableMetadataStore &metadata, std::span<const PropertyCollectionRoot> roots,
			const PropertyCollectionRootStoreOptions &options) {
		MetadataSnapshotPin snapshot = metadata.openSnapshot();
		if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
			throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
								  "Property-root test publication cannot advance the checkpoint LSN");
		}
		const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
		const std::vector<EntityPropertyRootMutationPlan> plans =
				properties.planEntityRootReplacements(snapshot, roots, options);
		EntityPageBatchResult result = entities.applyNativeGraphBatchTransaction(
				snapshot, {}, {}, {}, plans, properties, options, checkpointLsn);
		snapshot.release();
		return result;
	}

	inline EntityPageBatchResult publishPropertyRoot(
			EntityPageStore &entities, PropertyCollectionRootStore &properties,
			RelocatableMetadataStore &metadata, const PropertyCollectionRoot &root,
			const PropertyCollectionRootStoreOptions &options) {
		return publishPropertyRoots(entities, properties, metadata, std::span(&root, 1U), options);
	}

} // namespace graph::storage::test
