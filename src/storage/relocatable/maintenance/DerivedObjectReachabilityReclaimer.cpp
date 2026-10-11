/**
 * @file DerivedObjectReachabilityReclaimer.cpp
 * @brief Conservative immutable-derived-data reachability reclamation.
 */

#include "graph/storage/relocatable/maintenance/DerivedObjectReachabilityReclaimer.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateOptions(const DerivedObjectReachabilityReclaimOptions &options,
							 LogSequenceNumber checkpointLsn) {
			constexpr size_t unbounded = std::numeric_limits<size_t>::max();
			const uint8_t invalid = static_cast<uint8_t>(options.maximum_vector_manifests == 0U) |
									static_cast<uint8_t>(options.maximum_vector_manifests == unbounded) |
									static_cast<uint8_t>(options.maximum_vector_locations == 0U) |
									static_cast<uint8_t>(options.maximum_vector_locations == unbounded) |
									static_cast<uint8_t>(options.maximum_mapping_entries == 0U) |
									static_cast<uint8_t>(options.maximum_mapping_entries == unbounded) |
									static_cast<uint8_t>(options.maximum_candidate_pages == 0U) |
									static_cast<uint8_t>(options.maximum_candidate_pages == unbounded) |
									static_cast<uint8_t>(options.maximum_retirements == 0U) |
									static_cast<uint8_t>(options.maximum_retirements == unbounded) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable derived-object reclamation requires finite nonzero scan bounds and a checkpoint LSN");
			}
		}

		[[nodiscard]] bool hasAdjacencyMapping(const VectorManifest &manifest) {
			return manifest.adjacency_mapping_index_id != 0U;
		}

		[[nodiscard]] std::set<uint64_t>
		baseVectorIds(const MetadataSnapshotPin &snapshot, const VectorManifest &manifest,
					  const std::set<uint64_t> &rawVectorIds, BTreePageStore &trees,
					  RelocatableMetadataStore &metadata, size_t maximumEntries) {
			std::set<uint64_t> result = rawVectorIds;
			if (manifest.delta_mapping_index_id == 0U) {
				return result;
			}
			VectorDeltaMapping::validateMappingIndexId(manifest.delta_mapping_index_id);
			const IndexRootLocation expected{manifest.delta_mapping_index_id, manifest.delta_mapping_root_page_id,
											manifest.delta_mapping_root_generation};
			if (metadata.findIndexRoot(snapshot, manifest.delta_mapping_index_id) != expected ||
				manifest.delta_entry_count > maximumEntries) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable derived-object reclamation found an invalid or oversized Vector delta root");
			}
			const std::vector<VectorDeltaEntry> entries =
					VectorDeltaMapping::scan(snapshot, trees, metadata, manifest.delta_mapping_index_id, maximumEntries);
			if (entries.size() != manifest.delta_entry_count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable derived-object reclamation found a Vector delta count mismatch");
			}
			for (const VectorDeltaEntry &entry: entries) {
				const bool hasCurrentRawLocation = rawVectorIds.contains(entry.vector_id);
			VectorDeltaMapping::validateCurrentRawMembership(entry, hasCurrentRawLocation);
			if (entry.operation == VectorDeltaOperation::VDO_INSERT) {
				static_cast<void>(result.erase(entry.vector_id));
			} else {
				static_cast<void>(result.insert(entry.vector_id));
				}
			}
			return result;
		}

		void validateMappingTreeOptions(const DerivedObjectReachabilityReclaimOptions &options,
										LogSequenceNumber checkpointLsn) {
			validateOptions(options, checkpointLsn);
			if (options.maximum_mapping_roots == 0U ||
				options.maximum_mapping_roots == std::numeric_limits<size_t>::max() ||
				options.maximum_mapping_tree_pages == 0U ||
				options.maximum_mapping_tree_pages == std::numeric_limits<size_t>::max()) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable mapping-tree reclamation requires finite nonzero root and page scan bounds");
			}
		}

		[[nodiscard]] bool isDerivedMappingRoot(uint64_t indexId) {
			return VectorPqMapping::isDerivedMappingIndexId(indexId);
		}

	} // namespace

	DerivedObjectReachabilityReclaimer::DerivedObjectReachabilityReclaimer(
			BTreePageStore &trees, VectorPageStore &vectors, VectorAdjacencyPageStore &adjacencies,
			VectorManifestStore &manifests, RelocatableMetadataStore &metadata) :
		trees_(trees), vectors_(vectors), adjacencies_(adjacencies), manifests_(manifests), metadata_(metadata) {
		if (!trees_.usesMetadataStore(metadata_) || !vectors_.usesMetadataStore(metadata_) ||
			!adjacencies_.usesMetadataStore(metadata_) || !manifests_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument(
					"relocatable derived-object reclaimer requires page stores attached to one metadata store");
		}
	}

	DerivedObjectReachabilityReclaimReport DerivedObjectReachabilityReclaimer::reclaim(
			const DerivedObjectReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &publishOptions) {
		validateOptions(options, checkpointLsn);
		DerivedObjectReachabilityReclaimReport report;
		std::vector<DerivedVectorAdjacencyPage> retirementPlan;
		std::set<SegmentId> affectedPools;

		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			report.scanned_generation = snapshot.superblock().generation;
			std::map<uint64_t, std::set<uint64_t>> rawVectorIdsByIndex;
			std::map<uint64_t, std::unique_ptr<VectorPage>> rawPages;
			metadata_.visitVectorLocations(snapshot, [&](const VectorLocation &location) {
				if (report.vector_locations == options.maximum_vector_locations) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable derived-object reclamation exceeded its Vector Location scan bound");
				}
				++report.vector_locations;
				auto page = rawPages.find(location.page_id.value());
				if (page == rawPages.end()) {
					auto inserted = rawPages.emplace(location.page_id.value(),
														std::make_unique<VectorPage>(vectors_.read(snapshot, location.page_id)));
					page = inserted.first;
				}
				const VectorPageHeader header = page->second->header();
				if (header.kind != VectorStorageKind::VSK_RAW) {
					return;
				}
				const std::vector<std::byte> record = page->second->read(location.slot_id, location.slot_generation);
				const uint8_t invalidRecord = static_cast<uint8_t>(header.page_id != location.page_id) |
													 static_cast<uint8_t>(record.size() < sizeof(uint64_t)) |
													 static_cast<uint8_t>(LittleEndianCodec::readU64(record, 0) != location.vector_id);
				if (invalidRecord != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable derived-object reclamation found a raw Vector Location with mismatched record identity");
				}
				// Vector Location Directory keys are unique by codec construction; retain
				// the IDs only to compare the Manifest mapping with that authority.
				rawVectorIdsByIndex[header.index_id].insert(location.vector_id);
			});

			std::set<PageId> reachableAdjacencyPages;
			metadata_.visitVectorManifestLocations(snapshot, [&](const VectorManifestLocation &manifestLocation) {
				if (report.vector_manifests == options.maximum_vector_manifests) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable derived-object reclamation exceeded its Vector Manifest scan bound");
				}
				++report.vector_manifests;
				const VectorManifest manifest = manifests_.read(snapshot, manifestLocation.index_id).manifest();
				const std::set<uint64_t> baseIds = baseVectorIds(snapshot, manifest, rawVectorIdsByIndex[manifest.index_id],
																			 trees_, metadata_, options.maximum_mapping_entries);
				if (!hasAdjacencyMapping(manifest)) {
					return;
				}
				VectorAdjacencyMapping::validateMappingIndexId(manifest.adjacency_mapping_index_id);
				const IndexRootLocation expectedRoot{manifest.adjacency_mapping_index_id,
														manifest.adjacency_mapping_root_page_id,
														manifest.adjacency_mapping_root_generation};
				if (metadata_.findIndexRoot(snapshot, manifest.adjacency_mapping_index_id) != expectedRoot) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable derived-object reclamation found a Manifest adjacency root outside the Index Root Directory");
				}
				if (baseIds.size() > options.maximum_mapping_entries) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable derived-object reclamation adjacency mapping exceeds its configured scan bound");
				}
				const std::vector<BTreePageEntry> mappings = BTreeIndex(manifest.adjacency_mapping_index_id, trees_, metadata_)
																					.scan(snapshot, std::nullopt, baseIds.size() + 1U);
				if (mappings.size() != baseIds.size()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable derived-object reclamation found an incomplete or overfull adjacency mapping");
				}
				auto vectorId = baseIds.begin();
				for (const BTreePageEntry &mapping: mappings) {
					const PageId pageId(mapping.value_or_child);
					const uint8_t invalidMapping = static_cast<uint8_t>(!pageId.isValid()) |
															static_cast<uint8_t>(mapping.key != VectorAdjacencyMapping::key(*vectorId));
					if (invalidMapping != 0U) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable derived-object reclamation found a noncanonical adjacency mapping entry");
					}
					const PageLocation location = metadata_.findPageLocation(snapshot, pageId);
					const CatalogEntry pool = metadata_.findCatalog(snapshot, location.segment_id);
					if (pool.kind != SegmentKind::SGK_VECTOR_ADJACENCY_POOL || pool.owner_id != manifest.index_id) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable derived-object reclamation found an adjacency mapping outside its index-owned pool");
					}
					const std::optional<DerivedVectorAdjacencyPage> derived = adjacencies_.inspectDerivedPage(snapshot, pageId);
					if (!derived.has_value() || derived->index_id != manifest.index_id) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable derived-object reclamation found a legacy or mismatched adjacency mapping target");
					}
					// `readRecord` validates the requested Vector ID against the page-local
					// record and fails closed before it can return a mismatched result.
					static_cast<void>(adjacencies_.readRecord(snapshot, pageId, *vectorId));
					reachableAdjacencyPages.insert(pageId);
					++vectorId;
				}
			});
			report.reachable_adjacency_pages = reachableAdjacencyPages.size();

			size_t candidatePages = 0U;
			for (const CatalogEntry &pool: metadata_.findCurrentVectorAdjacencyPagePools(snapshot)) {
				for (const PageLocation &location: metadata_.findPageLocationsForSegment(snapshot, pool.segment_id)) {
					if (candidatePages == options.maximum_candidate_pages) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable derived-object reclamation exceeded its adjacency-page candidate scan bound");
					}
					++candidatePages;
					const std::optional<DerivedVectorAdjacencyPage> derived =
							adjacencies_.inspectDerivedPage(snapshot, location.page_id);
					if (!derived.has_value()) {
						++report.non_derived_adjacency_pages_retained;
						continue;
					}
					if (!reachableAdjacencyPages.contains(location.page_id)) {
						if (retirementPlan.size() == options.maximum_retirements) {
							fail(FormatError::FERR_INVALID_STATE,
								 "relocatable derived-object reclamation exceeded its page-retirement bound");
						}
						retirementPlan.push_back(*derived);
						affectedPools.insert(location.segment_id);
					}
				}
			}
			snapshot.release();
		} catch (...) {
			snapshot.release();
			throw;
		}

		if (retirementPlan.empty()) {
			report.published_generation = report.scanned_generation;
			return report;
		}
		std::ranges::sort(retirementPlan, {},
						  [](const DerivedVectorAdjacencyPage &page) { return page.location.page_id.value(); });
		const Superblock published =
				adjacencies_.retireDerivedPages(report.scanned_generation, retirementPlan, checkpointLsn, publishOptions);
		report.published_generation = published.generation;
		report.derived_adjacency_pages_retired = retirementPlan.size();
		for (const SegmentId segmentId: affectedPools) {
			adjacencies_.reclaimRetiredFrames(segmentId);
		}
		return report;
	}

	DerivedObjectReachabilityReclaimReport DerivedObjectReachabilityReclaimer::reclaimPq(
			const DerivedObjectReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &publishOptions) {
		validateOptions(options, checkpointLsn);
		DerivedObjectReachabilityReclaimReport report;
		std::vector<DerivedVectorPqPage> retirementPlan;
		std::set<SegmentId> affectedPools;

		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			report.scanned_generation = snapshot.superblock().generation;
			std::map<uint64_t, std::set<uint64_t>> rawVectorIdsByIndex;
			std::map<uint64_t, std::unique_ptr<VectorPage>> pages;
			metadata_.visitVectorLocations(snapshot, [&](const VectorLocation &location) {
				if (report.vector_locations == options.maximum_vector_locations) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable PQ reclamation exceeded its Vector Location scan bound");
				}
				++report.vector_locations;
				auto page = pages.find(location.page_id.value());
				if (page == pages.end()) {
					auto inserted = pages.emplace(location.page_id.value(),
														std::make_unique<VectorPage>(vectors_.read(snapshot, location.page_id)));
					page = inserted.first;
				}
				const VectorPageHeader header = page->second->header();
				if (header.kind != VectorStorageKind::VSK_RAW) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable PQ reclamation found a Vector Location with an unknown storage kind");
				}
				const std::vector<std::byte> record = page->second->read(location.slot_id, location.slot_generation);
				if (header.page_id != location.page_id || record.size() < sizeof(uint64_t) ||
					LittleEndianCodec::readU64(record, 0) != location.vector_id) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable PQ reclamation found a raw Vector Location with mismatched record identity");
				}
				rawVectorIdsByIndex[header.index_id].insert(location.vector_id);
			});

			std::set<PageId> reachablePqPages;
			metadata_.visitVectorManifestLocations(snapshot, [&](const VectorManifestLocation &manifestLocation) {
				if (report.vector_manifests == options.maximum_vector_manifests) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable PQ reclamation exceeded its Vector Manifest scan bound");
				}
				++report.vector_manifests;
				const VectorManifest manifest = manifests_.read(snapshot, manifestLocation.index_id).manifest();
				const std::set<uint64_t> baseIds = baseVectorIds(snapshot, manifest, rawVectorIdsByIndex[manifest.index_id],
																			 trees_, metadata_, options.maximum_mapping_entries);
				if (manifest.pq_mapping_index_id == 0U) {
					return;
				}
				VectorPqMapping::validateMappingIndexId(manifest.pq_mapping_index_id);
				const IndexRootLocation expectedRoot{manifest.pq_mapping_index_id, manifest.pq_mapping_root_page_id,
														manifest.pq_mapping_root_generation};
				if (metadata_.findIndexRoot(snapshot, manifest.pq_mapping_index_id) != expectedRoot) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable PQ reclamation found a Manifest mapping root outside the Index Root Directory");
				}
				if (baseIds.size() > options.maximum_mapping_entries) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable PQ reclamation mapping exceeds its configured scan bound");
				}
				const std::vector<BTreePageEntry> mappings =
						BTreeIndex(manifest.pq_mapping_index_id, trees_, metadata_)
										.scan(snapshot, std::nullopt, baseIds.size() + 1U);
				if (mappings.size() != baseIds.size()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable PQ reclamation found an incomplete or overfull mapping");
				}
				auto vectorId = baseIds.begin();
				for (const BTreePageEntry &mapping: mappings) {
					const PageId pageId(mapping.value_or_child);
					if (!pageId.isValid() || mapping.key != VectorPqMapping::key(*vectorId)) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable PQ reclamation found a noncanonical or legacy mapping target");
					}
					const std::optional<DerivedVectorPqPage> derived = vectors_.inspectDerivedPqPage(snapshot, pageId);
					if (!derived.has_value() || derived->index_id != manifest.index_id ||
						!vectors_.read(snapshot, pageId).findVectorId(*vectorId).has_value()) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable PQ reclamation found a mismatched mapping target or Vector record");
					}
					reachablePqPages.insert(pageId);
					++vectorId;
				}
			});
			report.reachable_pq_pages = reachablePqPages.size();

			size_t candidatePages = 0U;
			for (const CatalogEntry &pool: metadata_.findCurrentVectorPqPagePools(snapshot)) {
				for (const PageLocation &location: metadata_.findPageLocationsForSegment(snapshot, pool.segment_id)) {
					if (candidatePages == options.maximum_candidate_pages) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable PQ reclamation exceeded its page candidate scan bound");
					}
					++candidatePages;
					const std::optional<DerivedVectorPqPage> derived = vectors_.inspectDerivedPqPage(snapshot, location.page_id);
					if (!derived.has_value()) {
						continue;
					}
					if (!reachablePqPages.contains(location.page_id)) {
						if (retirementPlan.size() == options.maximum_retirements) {
							fail(FormatError::FERR_INVALID_STATE,
								 "relocatable PQ reclamation exceeded its page-retirement bound");
						}
						retirementPlan.push_back(*derived);
						affectedPools.insert(location.segment_id);
					}
				}
			}
			snapshot.release();
		} catch (...) {
			snapshot.release();
			throw;
		}

		if (retirementPlan.empty()) {
			report.published_generation = report.scanned_generation;
			return report;
		}
		std::ranges::sort(retirementPlan, {},
						  [](const DerivedVectorPqPage &page) { return page.location.page_id.value(); });
		const Superblock published =
				vectors_.retireDerivedPqPages(report.scanned_generation, retirementPlan, checkpointLsn, publishOptions);
		report.published_generation = published.generation;
		report.derived_pq_pages_retired = retirementPlan.size();
		for (const SegmentId segmentId: affectedPools) {
			vectors_.reclaimRetiredFrames(segmentId);
		}
		return report;
	}

	DerivedObjectReachabilityReclaimReport DerivedObjectReachabilityReclaimer::reclaimMappingTrees(
			const DerivedObjectReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &publishOptions) {
		validateMappingTreeOptions(options, checkpointLsn);
		DerivedObjectReachabilityReclaimReport report;
		std::map<uint64_t, IndexRootLocation> visibleRoots;
		std::vector<IndexRootLocation> retirementRoots;
		std::vector<RetiredIndexTreePage> retirementPages;
		std::set<SegmentId> affectedPools;
		std::set<PageId> plannedPages;

		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			report.scanned_generation = snapshot.superblock().generation;
			metadata_.visitVectorManifestLocations(snapshot, [&](const VectorManifestLocation &manifestLocation) {
				if (report.vector_manifests == options.maximum_vector_manifests) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable mapping-tree reclamation exceeded its Vector Manifest scan bound");
				}
				++report.vector_manifests;
				const VectorManifest manifest = manifests_.read(snapshot, manifestLocation.index_id).manifest();
				const auto recordVisibleRoot = [&](uint64_t indexId, PageId pageId, Generation generation) {
					if (indexId == 0U) {
						return;
					}
					if (!isDerivedMappingRoot(indexId)) { // ZYX_COV_EXCL_LINE: VectorManifestPage decoding already
						// rejects a nonzero mapping field that lacks its exact typed prefix.
						fail(FormatError::FERR_INVALID_STATE,
								 "relocatable mapping-tree reclamation found a Manifest mapping outside a typed namespace");
					}
					const IndexRootLocation expected{indexId, pageId, generation};
					if (metadata_.findIndexRoot(snapshot, indexId) != expected ||
						!visibleRoots.emplace(indexId, expected).second) {
						fail(FormatError::FERR_INVALID_STATE,
								 "relocatable mapping-tree reclamation found a duplicate or stale Manifest mapping root");
					}
				};
				recordVisibleRoot(manifest.pq_mapping_index_id, manifest.pq_mapping_root_page_id,
							  manifest.pq_mapping_root_generation);
				recordVisibleRoot(manifest.adjacency_mapping_index_id, manifest.adjacency_mapping_root_page_id,
							  manifest.adjacency_mapping_root_generation);
				recordVisibleRoot(manifest.delta_mapping_index_id, manifest.delta_mapping_root_page_id,
							  manifest.delta_mapping_root_generation);
			});

			metadata_.visitIndexRoots(snapshot, [&](const IndexRootLocation &root) {
				if (report.mapping_roots_scanned == options.maximum_mapping_roots) {
					fail(FormatError::FERR_INVALID_STATE,
							 "relocatable mapping-tree reclamation exceeded its Index Root scan bound");
				}
				++report.mapping_roots_scanned;
				if (!isDerivedMappingRoot(root.index_id)) {
					++report.non_derived_mapping_roots_retained;
					return;
				}
				const auto visible = visibleRoots.find(root.index_id);
				if (visible != visibleRoots.end()) {
					if (visible->second != root) { // ZYX_COV_EXCL_LINE: the triple was read from this immutable
						// snapshot and checked against the same Index Root Directory immediately above.
						fail(FormatError::FERR_INVALID_STATE,
								 "relocatable mapping-tree reclamation found a replaced visible mapping root");
					}
					++report.reachable_mapping_roots;
					return;
				}
				retirementRoots.push_back(root);
			});

			for (const IndexRootLocation &root: retirementRoots) {
				std::vector<PageId> pending{root.root_page_id};
				std::set<PageId> visited;
				while (!pending.empty()) {
					const PageId pageId = pending.back();
					pending.pop_back();
					if (retirementPages.size() == options.maximum_mapping_tree_pages || !visited.insert(pageId).second) {
						fail(FormatError::FERR_INVALID_STATE,
								 "relocatable mapping-tree reclamation found an oversized or cyclic mapping tree");
					}
					const PageLocation location = metadata_.findPageLocation(snapshot, pageId);
					const CatalogEntry pool = metadata_.findCatalog(snapshot, location.segment_id);
					const BTreePage page = trees_.read(snapshot, pageId);
					const BTreePageHeader header = page.header();
					// The Index pool owner identifies a shared allocation namespace; the
					// B+Tree page/header owner is the authoritative mapping-tree identity.
					const uint8_t invalidPage = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
												static_cast<uint8_t>(pool.owner_id == 0U) |
											static_cast<uint8_t>(header.index_id != root.index_id) |
											static_cast<uint8_t>(header.page_id != pageId) |
											static_cast<uint8_t>(header.generation != location.generation) |
											static_cast<uint8_t>(!page.slottedPage().ownerCommittedLsn().isValid());
					if (invalidPage != 0U) {
						fail(FormatError::FERR_INVALID_STATE,
								 "relocatable mapping-tree reclamation found a shared or mismatched mapping page");
					}
					if (!plannedPages.insert(pageId).second) { // ZYX_COV_EXCL_LINE: a page can only be revisited
						// within one tree before this point, where the local `visited` set rejects its cycle.
						fail(FormatError::FERR_INVALID_STATE,
								 "relocatable mapping-tree reclamation found a shared mapping page");
					}
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
				 "relocatable mapping-tree reclamation exceeded its page-retirement bound");
		}
		std::ranges::sort(retirementRoots, {}, &IndexRootLocation::index_id);
		std::ranges::sort(retirementPages, {},
						  [](const RetiredIndexTreePage &page) { return page.location.page_id.value(); });
		const Superblock published = trees_.retireIndexTreePages(report.scanned_generation, retirementRoots,
				retirementPages, checkpointLsn, publishOptions);
		report.published_generation = published.generation;
		report.derived_mapping_roots_retired = retirementRoots.size();
		report.derived_mapping_tree_pages_retired = retirementPages.size();
		for (const SegmentId segmentId: affectedPools) {
			trees_.reclaimRetiredFrames(segmentId);
		}
		return report;
	}

} // namespace graph::storage
