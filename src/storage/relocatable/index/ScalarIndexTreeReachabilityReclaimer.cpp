/**
 * @file ScalarIndexTreeReachabilityReclaimer.cpp
 * @brief Snapshot-safe reclamation of obsolete public scalar-index B+Trees.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/index/ScalarIndexTreeReachabilityReclaimer.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <vector>

#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateOptions(const ScalarIndexTreeReclaimOptions &options, LogSequenceNumber checkpointLsn) {
			const uint8_t invalid = static_cast<uint8_t>(options.maximum_index_definitions == 0U) |
									static_cast<uint8_t>(options.maximum_constraint_definitions == 0U) |
									static_cast<uint8_t>(options.maximum_vector_manifests == 0U) |
									static_cast<uint8_t>(options.maximum_index_roots == 0U) |
									static_cast<uint8_t>(options.maximum_tree_pages == 0U) |
									static_cast<uint8_t>(options.maximum_retirements == 0U) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable scalar Index-tree reclamation requires nonzero scan bounds and a checkpoint LSN");
			}
		}

		[[nodiscard]] bool requiresConstraintSupportTree(const ConstraintDefinition &definition) {
			return definition.kind == ConstraintDefinitionKind::CDK_UNIQUE ||
				   definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY;
		}

		void planTreeRetirement(BTreePageStore &trees, RelocatableMetadataStore &metadata,
								   const MetadataSnapshotPin &snapshot, const IndexRootLocation &root,
								   size_t maximumTreePages, std::set<PageId> &plannedPages,
								   std::vector<RetiredIndexTreePage> &retirementPages,
								   std::set<SegmentId> &affectedPools) {
			std::vector<PageId> pending = {root.root_page_id};
			std::set<PageId> visited;
			while (!pending.empty()) {
				const PageId pageId = pending.back();
				pending.pop_back();
				if (retirementPages.size() == maximumTreePages || !visited.insert(pageId).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable scalar Index-tree reclamation found an oversized or cyclic B+Tree");
				}
				const PageLocation location = metadata.findPageLocation(snapshot, pageId);
				const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
				const BTreePage page = trees.read(snapshot, pageId);
				const BTreePageHeader header = page.header();
					// Index pools are shared allocation containers. Tree ownership is
					// authenticated by the PageOwner and B+Tree headers, not the pool's
					// allocation-namespace owner.
					const uint8_t invalidPage = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
												static_cast<uint8_t>(pool.owner_id == 0U) |
											static_cast<uint8_t>(header.index_id != root.index_id) |
											static_cast<uint8_t>(header.page_id != pageId) |
											static_cast<uint8_t>(header.generation != location.generation) |
											static_cast<uint8_t>(
															static_cast<uint8_t>(pageId == root.root_page_id) &
															static_cast<uint8_t>(header.generation != root.root_generation)) |
											static_cast<uint8_t>(!page.slottedPage().ownerCommittedLsn().isValid());
				if (invalidPage != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable scalar Index-tree reclamation found a shared or mismatched B+Tree page");
				}
				if (!plannedPages.insert(pageId).second) { // ZYX_COV_EXCL_START: visited rejects duplicate descendants
					// first; different roots cannot share a page after the pool-owner/root-ID validation above.
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable scalar Index-tree reclamation found a B+Tree page shared by two roots");
				} // ZYX_COV_EXCL_STOP
				retirementPages.push_back({location, root.index_id, page.slottedPage().ownerCommittedLsn()});
				affectedPools.insert(location.segment_id);
				if (header.kind == BTreeNodeKind::BPN_INTERNAL) {
					pending.push_back(header.leftmost_child);
					for (const BTreePageEntry &entry: page.entries()) {
						pending.emplace_back(entry.value_or_child);
					}
				}
			}
		}

	} // namespace

	ScalarIndexTreeReachabilityReclaimer::ScalarIndexTreeReachabilityReclaimer(
			BTreePageStore &trees, SystemCatalog &catalog, VectorManifestStore &vectorManifests,
			RelocatableMetadataStore &metadata) :
		trees_(trees), catalog_(catalog), vector_manifests_(vectorManifests), metadata_(metadata) {
		if (!trees_.usesMetadataStore(metadata_) || !catalog_.usesMetadataStore(metadata_) ||
			!vector_manifests_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument(
					"relocatable canonical Index-tree reclaimer requires stores attached to one metadata store");
		}
	}

	ScalarIndexTreeReclaimReport ScalarIndexTreeReachabilityReclaimer::reclaim(
			const ScalarIndexTreeReclaimOptions &options, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &publishOptions) {
		validateOptions(options, checkpointLsn);
		ScalarIndexTreeReclaimReport report;
		std::vector<IndexRootLocation> retirementRoots;
		std::vector<RetiredIndexTreePage> retirementPages;
		std::set<SegmentId> affectedPools;
		std::set<PageId> plannedPages;

		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			report.scanned_generation = snapshot.superblock().generation;
			const std::vector<IndexDefinition> definitions =
					catalog_.readIndexDefinitions(snapshot, options.maximum_index_definitions);
			const std::vector<ConstraintDefinition> constraints =
					catalog_.readConstraintDefinitions(snapshot, options.maximum_constraint_definitions);
			report.index_definitions = definitions.size();
			report.constraint_definitions = constraints.size();

			std::map<uint64_t, IndexDefinitionKind> activeKinds;
			for (const IndexDefinition &definition: definitions) {
				if (!activeKinds.emplace(definition.index_id, definition.kind).second) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable scalar Index-tree reclamation found duplicate active Catalog Index IDs");
				}
			}
			std::set<uint64_t> activeConstraintSupportRoots;
			uint64_t previousConstraintId = 0U;
			for (const ConstraintDefinition &definition: constraints) {
				if (!isSystemCatalogConstraintDefinitionId(definition.constraint_id) ||
					definition.constraint_id <= previousConstraintId) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable scalar Index-tree reclamation found an invalid constraint definition namespace");
				}
				previousConstraintId = definition.constraint_id;
				const bool requiresSupport = requiresConstraintSupportTree(definition);
				const std::optional<IndexRootLocation> root =
						metadata_.findIndexRootOptional(snapshot, definition.constraint_id);
				if (requiresSupport) {
					const uint8_t invalidSupport = static_cast<uint8_t>(!root.has_value()) |
											   static_cast<uint8_t>(
															   !activeConstraintSupportRoots.insert(definition.constraint_id).second);
					if (invalidSupport != 0U) {
						fail(FormatError::FERR_INVALID_STATE,
								 "relocatable scalar Index-tree reclamation found an active constraint without one support-tree root");
					}
				} else if (root.has_value()) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable scalar Index-tree reclamation found a support-tree root for a non-unique constraint");
				}
			}
			std::map<uint64_t, IndexRootLocation> visibleVectorRoots;
			size_t visibleVectorManifestCount = 0U;
			metadata_.visitVectorManifestLocations(snapshot, [&](const VectorManifestLocation &location) {
				if (visibleVectorManifestCount == options.maximum_vector_manifests) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable scalar Index-tree reclamation exceeded its Vector Manifest scan bound");
				}
				++visibleVectorManifestCount;
				const VectorManifest manifest = vector_manifests_.read(snapshot, location.index_id).manifest();
				const uint8_t invalidManifest =
						static_cast<uint8_t>(manifest.index_id != location.index_id) |
						static_cast<uint8_t>((manifest.index_id & (UINT64_C(1) << 63U)) != 0U) |
						static_cast<uint8_t>(manifest.node_mapping_root_page_id.isValid() !=
												 manifest.node_mapping_root_generation.isValid()) |
						static_cast<uint8_t>(manifest.vector_node_mapping_index_id == 0U) |
						static_cast<uint8_t>(!VectorNodeMapping::isMappingIndexId(
												 manifest.vector_node_mapping_index_id)) |
						static_cast<uint8_t>(manifest.vector_node_mapping_root_page_id.isValid() !=
												 manifest.vector_node_mapping_root_generation.isValid());
				if (invalidManifest != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable scalar Index-tree reclamation found an invalid Vector Manifest identity or mapping root");
				}
				const IndexRootLocation root{manifest.index_id, manifest.node_mapping_root_page_id,
														 manifest.node_mapping_root_generation};
				const IndexRootLocation vectorNodeRoot{manifest.vector_node_mapping_index_id,
															 manifest.vector_node_mapping_root_page_id,
															 manifest.vector_node_mapping_root_generation};
				const uint8_t invalidRoots =
						static_cast<uint8_t>(metadata_.findIndexRoot(snapshot, manifest.index_id) != root) |
						static_cast<uint8_t>(metadata_.findIndexRoot(snapshot, manifest.vector_node_mapping_index_id) !=
														 vectorNodeRoot) |
						static_cast<uint8_t>(!visibleVectorRoots.emplace(manifest.index_id, root).second) |
						static_cast<uint8_t>(
								!visibleVectorRoots.emplace(manifest.vector_node_mapping_index_id, vectorNodeRoot).second);
				if (invalidRoots != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable scalar Index-tree reclamation found a stale or duplicate Vector Mapping root");
				}
			});

			metadata_.visitIndexRoots(snapshot, [&](const IndexRootLocation &root) {
				if (report.index_roots_scanned == options.maximum_index_roots) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable scalar Index-tree reclamation exceeded its Index Root scan bound");
				}
				++report.index_roots_scanned;
				const auto visibleVector = visibleVectorRoots.find(root.index_id);
				if (visibleVector != visibleVectorRoots.end()) {
					++report.visible_vector_roots_retained;
					return;
				}
				if (VectorNodeMapping::isMappingIndexId(root.index_id)) {
					retirementRoots.push_back(root);
					++report.dropped_index_roots_retired;
					return;
				}
				if (isSystemCatalogConstraintDefinitionId(root.index_id)) {
					if (activeConstraintSupportRoots.contains(root.index_id)) {
						++report.active_constraint_support_roots_retained;
						return;
					}
					retirementRoots.push_back(root);
					++report.orphaned_constraint_support_roots_retired;
					return;
				}
				if (root.index_id >= (UINT64_C(1) << 63U)) {
					++report.metadata_roots_retained;
					return;
				}
				const auto active = activeKinds.find(root.index_id);
				if (active != activeKinds.end()) {
					if (active->second == IndexDefinitionKind::IDK_VECTOR) {
						fail(FormatError::FERR_INVALID_STATE,
								 "relocatable scalar Index-tree reclamation found a Vector Mapping root without a visible Manifest");
					}
					++report.active_roots_retained;
					return;
				}
				if (isSystemCatalogIndexDefinitionId(root.index_id)) {
					retirementRoots.push_back(root);
					++report.dropped_index_roots_retired;
					return;
				}
				fail(FormatError::FERR_INVALID_STATE,
						"relocatable scalar Index-tree reclamation found an unknown Index root");
			});

			for (const IndexRootLocation &root: retirementRoots) {
				planTreeRetirement(trees_, metadata_, snapshot, root, options.maximum_tree_pages, plannedPages,
									  retirementPages, affectedPools);
			}
			snapshot.release();
		} catch (...) {
			snapshot.release();
			throw;
		}

		if (retirementRoots.empty()) {
			report.published_generation = report.scanned_generation;
			return report;
		}
		if (retirementPages.size() > options.maximum_retirements) {
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable scalar Index-tree reclamation exceeded its page-retirement bound");
		}
		std::ranges::sort(retirementRoots, {}, &IndexRootLocation::index_id);
		std::ranges::sort(retirementPages, {},
						  [](const RetiredIndexTreePage &page) { return page.location.page_id.value(); });
		const Superblock published = trees_.retireIndexTreePages(report.scanned_generation, retirementRoots,
			retirementPages, checkpointLsn, publishOptions);
		report.published_generation = published.generation;
		report.index_tree_pages_retired = retirementPages.size();
		for (const SegmentId segmentId: affectedPools) {
			trees_.reclaimRetiredFrames(segmentId);
		}
		return report;
	}

} // namespace graph::storage
