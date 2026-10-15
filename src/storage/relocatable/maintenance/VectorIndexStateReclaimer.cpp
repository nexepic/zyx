/**
 * @file VectorIndexStateReclaimer.cpp
 * @brief Ordered, crash-retryable Vector index state retirement.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/VectorIndexStateReclaimer.hpp"

#include <limits>
#include <map>
#include <stdexcept>
#include <vector>
#include "graph/utils/OwnedAssociativeContainers.hpp"
#include "graph/utils/OwnedVector.hpp"

#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateManifestOptions(const VectorManifestReachabilityReclaimOptions &options,
									 LogSequenceNumber checkpointLsn) {
			const uint8_t invalid = static_cast<uint8_t>(options.maximum_index_definitions == 0U) |
									static_cast<uint8_t>(options.maximum_vector_manifests == 0U) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector Manifest reclamation requires nonzero bounds and a checkpoint LSN");
			}
		}

		// Validate every phase before publishing the first Manifest retirement. A
		// bounded maintenance request must either start from a complete budget or
		// leave the durable dependency graph unchanged.
		void validateReclaimOptions(const VectorIndexStateReclaimOptions &options, LogSequenceNumber checkpointLsn) {
			validateManifestOptions(options.manifests, checkpointLsn);
			const uint8_t invalid = static_cast<uint8_t>(options.raw.maximum_index_definitions == 0U) |
									static_cast<uint8_t>(options.raw.maximum_vector_manifests == 0U) |
									static_cast<uint8_t>(options.raw.maximum_raw_pools == 0U) |
									static_cast<uint8_t>(options.raw.maximum_raw_pages == 0U) |
									static_cast<uint8_t>(options.raw.maximum_vector_locations == 0U) |
									static_cast<uint8_t>(options.raw.maximum_retirements == 0U) |
									static_cast<uint8_t>(options.derived.maximum_vector_manifests == 0U) |
									static_cast<uint8_t>(options.derived.maximum_vector_locations == 0U) |
									static_cast<uint8_t>(options.derived.maximum_mapping_entries == 0U) |
									static_cast<uint8_t>(options.derived.maximum_candidate_pages == 0U) |
									static_cast<uint8_t>(options.derived.maximum_retirements == 0U) |
									static_cast<uint8_t>(options.derived.maximum_mapping_roots == 0U) |
									static_cast<uint8_t>(options.derived.maximum_mapping_tree_pages == 0U) |
									static_cast<uint8_t>(options.mapping_trees.maximum_index_definitions == 0U) |
									static_cast<uint8_t>(options.mapping_trees.maximum_constraint_definitions == 0U) |
									static_cast<uint8_t>(options.mapping_trees.maximum_vector_manifests == 0U) |
									static_cast<uint8_t>(options.mapping_trees.maximum_index_roots == 0U) |
									static_cast<uint8_t>(options.mapping_trees.maximum_tree_pages == 0U) |
									static_cast<uint8_t>(options.mapping_trees.maximum_retirements == 0U) |
									static_cast<uint8_t>(options.codebooks.maximum_vector_manifests == 0U) |
									static_cast<uint8_t>(options.codebooks.maximum_extent_locations == 0U) |
									static_cast<uint8_t>(options.codebooks.maximum_catalog_entries == 0U) |
									static_cast<uint8_t>(options.codebooks.maximum_retire_queue_entries == 0U) |
									static_cast<uint8_t>(options.codebooks.maximum_retirements == 0U) |
									static_cast<uint8_t>(options.empty_pools.maximum_index_definitions == 0U) |
									static_cast<uint8_t>(options.empty_pools.maximum_catalog_entries == 0U) |
									static_cast<uint8_t>(options.empty_pools.maximum_page_locations == 0U) |
									static_cast<uint8_t>(options.empty_pools.maximum_retire_queue_entries == 0U) |
									static_cast<uint8_t>(options.empty_pools.maximum_pools == 0U);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector state reclamation requires nonzero bounds for every lifecycle phase");
			}
		}

		struct RetiredManifestPlan {
			uint64_t index_id = 0U;
			LogSequenceNumber owner_lsn{};
		};

	} // namespace

	VectorIndexStateReclaimer::VectorIndexStateReclaimer(
			PhysicalSegmentAllocator &segments, SlottedPagePool &slottedPages, BTreePageStore &trees,
			VectorPageStore &pages, VectorAdjacencyPageStore &adjacencies, VectorManifestStore &manifests,
			SystemCatalog &catalog, RelocatableMetadataStore &metadata) :
		segments_(segments), slotted_pages_(slottedPages), trees_(trees), pages_(pages), adjacencies_(adjacencies),
		manifests_(manifests), catalog_(catalog), metadata_(metadata) {
		if (!segments_.usesMetadataStore(metadata_) || !slotted_pages_.usesMetadataStore(metadata_) ||
			!trees_.usesMetadataStore(metadata_) || !pages_.usesMetadataStore(metadata_) ||
			!adjacencies_.usesMetadataStore(metadata_) || !manifests_.usesMetadataStore(metadata_) ||
			!catalog_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument(
					"relocatable canonical Vector state reclaimer requires stores attached to one metadata store");
		}
	}

	LogSequenceNumber VectorIndexStateReclaimer::nextCheckpointLsn() const {
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const LogSequenceNumber current = snapshot.superblock().checkpoint_lsn;
		snapshot.release();
		if (current.value() ==
					std::numeric_limits<uint64_t>::max()) { // ZYX_COV_EXCL_LINE: opened Superblocks are decoded before
															// state maintenance reads their LSN.
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "relocatable Vector state reclamation cannot advance the checkpoint LSN");
		}
		return LogSequenceNumber(current.value() + 1U);
	}

	VectorManifestReachabilityReclaimReport
	VectorIndexStateReclaimer::reclaimManifests(const VectorManifestReachabilityReclaimOptions &options,
												LogSequenceNumber checkpointLsn,
												const MetadataPublishOptions &publishOptions) {
		validateManifestOptions(options, checkpointLsn);
		VectorManifestReachabilityReclaimReport report;
		utils::OwnedVector<RetiredManifestPlan> retirementPlan;

		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			report.scanned_generation = snapshot.superblock().generation;
			const utils::OwnedStdVector<IndexDefinition> definitions =
					catalog_.readIndexDefinitions(snapshot, options.maximum_index_definitions);
			report.index_definitions = definitions.size();

			utils::OwnedMap<uint64_t, IndexDefinitionKind> activeKinds;
			for (const IndexDefinition &definition: definitions) {
				// The canonical System Catalog reader has already validated the
				// definition namespace, key/value agreement, and uniqueness.
				activeKinds.emplace(definition.index_id, definition.kind);
			}

			metadata_.visitVectorManifestLocations(snapshot, [&](const VectorManifestLocation &location) {
				if (report.manifests_scanned == options.maximum_vector_manifests) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Manifest reclamation exceeded its Manifest scan bound");
				}
				++report.manifests_scanned;
				// `read()` returns a page by value, so retain the Manifest by value as
				// well; a reference to the temporary page would dangle in this visitor.
				const VectorManifest manifest = manifests_.read(snapshot, location.index_id).manifest();
				const auto active = activeKinds.find(manifest.index_id);
				if (active != activeKinds.end()) {
					if (active->second != IndexDefinitionKind::IDK_VECTOR) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Vector Manifest reclamation found a Manifest owned by a non-Vector Index");
					}
					// The manifest codec validates internally paired optional fields and
					// mapping-id namespaces. Reclamation independently verifies that the
					// active generation still names the exact durable roots.
					const uint8_t invalidMapping = static_cast<uint8_t>(!manifest.node_mapping_root_page_id.isValid()) |
							static_cast<uint8_t>(!manifest.vector_node_mapping_root_page_id.isValid()) |
							static_cast<uint8_t>(metadata_.findIndexRoot(snapshot, manifest.index_id) !=
												 IndexRootLocation{manifest.index_id,
																		   manifest.node_mapping_root_page_id,
																		   manifest.node_mapping_root_generation}) |
							static_cast<uint8_t>(metadata_.findIndexRoot(snapshot,
											 manifest.vector_node_mapping_index_id) !=
											 IndexRootLocation{manifest.vector_node_mapping_index_id,
														   manifest.vector_node_mapping_root_page_id,
														   manifest.vector_node_mapping_root_generation}) |
							static_cast<uint8_t>(manifest.delta_mapping_index_id != 0U &&
											 metadata_.findIndexRoot(snapshot, manifest.delta_mapping_index_id) !=
														 IndexRootLocation{manifest.delta_mapping_index_id,
																		   manifest.delta_mapping_root_page_id,
																		   manifest.delta_mapping_root_generation});
					if (invalidMapping != 0U) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Vector Manifest reclamation found an active Manifest with an invalid Mapping root");
					}
					++report.active_manifests_retained;
					return;
				}
				if (!isSystemCatalogIndexDefinitionId(manifest.index_id)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Manifest reclamation found a Manifest outside a dropped Vector Index namespace");
				}
				retirementPlan.push_back({manifest.index_id, manifest.committed_lsn});
			});
			snapshot.release();
		} catch (...) {
			snapshot.release();
			throw;
		}

		Superblock published;
		bool hasPublished = false;
		for (const RetiredManifestPlan &manifest: retirementPlan) {
			const LogSequenceNumber publicationLsn = hasPublished ? nextCheckpointLsn() : checkpointLsn;
			published = manifests_.retire(manifest.index_id, manifest.owner_lsn, publicationLsn, publishOptions);
			hasPublished = true;
			++report.dropped_manifests_retired;
		}
		report.published_generation = hasPublished ? published.generation : report.scanned_generation;
		return report;
	}

	VectorIndexStateReclaimReport VectorIndexStateReclaimer::reclaim(const VectorIndexStateReclaimOptions &options,
																	 LogSequenceNumber checkpointLsn,
																	 const MetadataPublishOptions &publishOptions) {
		validateReclaimOptions(options, checkpointLsn);
		VectorIndexStateReclaimReport report;
		report.manifests = reclaimManifests(options.manifests, checkpointLsn, publishOptions);

			VectorRawReachabilityReclaimer raw(pages_, manifests_, catalog_, metadata_);
		report.raw = raw.reclaim(options.raw, nextCheckpointLsn(), publishOptions);

		DerivedObjectReachabilityReclaimer derived(trees_, pages_, adjacencies_, manifests_, metadata_);
		report.adjacency = derived.reclaim(options.derived, nextCheckpointLsn(), publishOptions);
		report.pq = derived.reclaimPq(options.derived, nextCheckpointLsn(), publishOptions);
		report.derived_mappings = derived.reclaimMappingTrees(options.derived, nextCheckpointLsn(), publishOptions);

			ScalarIndexTreeReachabilityReclaimer mappings(trees_, catalog_, manifests_, metadata_);
		report.node_mappings = mappings.reclaim(options.mapping_trees, nextCheckpointLsn(), publishOptions);

		VectorCodebookReachabilityReclaimer codebooks(segments_, manifests_, metadata_);
		report.codebooks = codebooks.reclaim(options.codebooks, nextCheckpointLsn(), publishOptions);

			VectorPagePoolReclaimer pools(slotted_pages_, catalog_, metadata_);
		report.empty_pools = pools.reclaim(options.empty_pools, nextCheckpointLsn(), publishOptions);
		return report;
	}

} // namespace graph::storage
