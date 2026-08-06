/**
 * @file VectorRawReachabilityReclaimer.cpp
 * @brief Conservative reclamation of logically unreachable Raw Vector pools.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/VectorRawReachabilityReclaimer.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateOptions(const VectorRawReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn) {
			const uint8_t invalid = static_cast<uint8_t>(options.maximum_index_definitions == 0U) |
									static_cast<uint8_t>(options.maximum_vector_manifests == 0U) |
									static_cast<uint8_t>(options.maximum_raw_pools == 0U) |
									static_cast<uint8_t>(options.maximum_raw_pages == 0U) |
									static_cast<uint8_t>(options.maximum_vector_locations == 0U) |
									static_cast<uint8_t>(options.maximum_retirements == 0U) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Raw Vector reclamation requires nonzero bounds and a checkpoint LSN");
			}
		}

		struct RawPageObservation {
			PageLocation location;
			uint64_t index_id = 0U;
			LogSequenceNumber owner_lsn{};
			uint16_t live_count = 0U;
			bool retire = false;
			std::vector<uint64_t> vector_ids;
		};

		[[nodiscard]] LogSequenceNumber nextCheckpointLsn(const Superblock &published) {
			if (!published.checkpoint_lsn
						 .isValid() || // ZYX_COV_EXCL_LINE: published Superblocks are decoded before use.
				published.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "relocatable Raw Vector reclamation cannot advance the checkpoint LSN");
			}
			return LogSequenceNumber(published.checkpoint_lsn.value() + 1U);
		}

	} // namespace

	VectorRawReachabilityReclaimer::VectorRawReachabilityReclaimer(
			VectorPageStore &pages, VectorManifestStore &manifests, SystemCatalog &catalog,
			RelocatableMetadataStore &metadata) :
		pages_(pages), manifests_(manifests), catalog_(catalog), metadata_(metadata) {
		if (!pages_.usesMetadataStore(metadata_) || !manifests_.usesMetadataStore(metadata_) ||
			!catalog_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument(
					"relocatable canonical Raw Vector reclaimer requires stores attached to one metadata store");
		}
	}

	VectorRawReachabilityReclaimReport
	VectorRawReachabilityReclaimer::reclaim(const VectorRawReachabilityReclaimOptions &options,
											LogSequenceNumber checkpointLsn,
											const MetadataPublishOptions &publishOptions) {
		validateOptions(options, checkpointLsn);
		VectorRawReachabilityReclaimReport report;
		std::vector<RetiredRawVectorPage> retirementPages;
		std::vector<uint64_t> erasedVectorIds;
		std::vector<CatalogEntry> candidatePools;
		std::vector<CatalogEntry> retiredPools;
		Superblock scanned;

		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			scanned = snapshot.superblock();
			report.scanned_generation = scanned.generation;
			const std::vector<IndexDefinition> definitions =
					catalog_.readIndexDefinitions(snapshot, options.maximum_index_definitions);
			std::map<uint64_t, IndexDefinitionKind> activeKinds;
			for (const IndexDefinition &definition: definitions) {
				if (!isSystemCatalogIndexDefinitionId(definition.index_id) ||
					!activeKinds.emplace(definition.index_id, definition.kind).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Raw Vector reclamation found an invalid or duplicate Catalog Index namespace");
				}
			}
			report.index_definitions = definitions.size();

			std::map<uint64_t, bool> manifestHasNodeMapping;
			metadata_.visitVectorManifestLocations(snapshot, [&](const VectorManifestLocation &location) {
				if (report.vector_manifests == options.maximum_vector_manifests) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Raw Vector reclamation exceeded its Vector Manifest scan bound");
				}
				++report.vector_manifests;
				const VectorManifest manifest = manifests_.read(snapshot, location.index_id).manifest();
				const uint8_t invalidManifest = static_cast<uint8_t>(manifest.index_id != location.index_id) |
												static_cast<uint8_t>(manifest.index_id == 0U) |
												static_cast<uint8_t>((manifest.index_id & (UINT64_C(1) << 63U)) != 0U) |
												static_cast<uint8_t>(manifest.node_mapping_root_page_id.isValid() !=
																 manifest.node_mapping_root_generation.isValid()) |
												static_cast<uint8_t>(manifest.vector_node_mapping_index_id == 0U) |
							static_cast<uint8_t>(!VectorNodeMapping::isMappingIndexId(
											 manifest.vector_node_mapping_index_id)) |
							static_cast<uint8_t>(manifest.vector_node_mapping_root_page_id.isValid() !=
											 manifest.vector_node_mapping_root_generation.isValid()) |
							static_cast<uint8_t>(manifest.delta_mapping_index_id != 0U &&
									 !VectorDeltaMapping::isMappingIndexId(manifest.delta_mapping_index_id)) |
							static_cast<uint8_t>((manifest.delta_mapping_index_id != 0U) !=
									 manifest.delta_mapping_root_page_id.isValid()) |
							static_cast<uint8_t>((manifest.delta_mapping_index_id != 0U) !=
									 manifest.delta_mapping_root_generation.isValid()) |
							static_cast<uint8_t>((manifest.delta_mapping_index_id != 0U) !=
									 (manifest.delta_entry_count != 0U));
				if (invalidManifest != 0U || // ZYX_COV_EXCL_LINE: VectorManifestStore::read rejects a frame that
											 // disagrees with its directory entry.
					!manifestHasNodeMapping
							 .emplace(manifest.index_id, manifest.node_mapping_root_page_id.isValid())
							 // ZYX_COV_EXCL_LINE: the Manifest Directory is keyed by the one stable Index ID.
							 .second) {
					fail(FormatError::FERR_INVALID_STATE, // ZYX_COV_EXCL_LINE: the store rejects frame/directory
														  // mismatches. ZYX_COV_EXCL_LINE
						 "relocatable Raw Vector reclamation found an invalid or duplicate Vector Manifest identity");
				}
				const auto active = activeKinds.find(manifest.index_id);
				if (active != activeKinds.end()) {
					const IndexRootLocation expectedRoot{manifest.index_id, manifest.node_mapping_root_page_id,
																	 manifest.node_mapping_root_generation};
					const IndexRootLocation expectedVectorNodeRoot{manifest.vector_node_mapping_index_id,
																	manifest.vector_node_mapping_root_page_id,
																	manifest.vector_node_mapping_root_generation};
					const IndexRootLocation expectedDeltaRoot{manifest.delta_mapping_index_id,
																 manifest.delta_mapping_root_page_id,
																 manifest.delta_mapping_root_generation};
					if (active->second != IndexDefinitionKind::IDK_VECTOR ||
						metadata_.findIndexRoot(snapshot, manifest.index_id) != expectedRoot ||
						metadata_.findIndexRoot(snapshot, manifest.vector_node_mapping_index_id) != expectedVectorNodeRoot ||
						(manifest.delta_mapping_index_id != 0U &&
						 metadata_.findIndexRoot(snapshot, manifest.delta_mapping_index_id) != expectedDeltaRoot)) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Raw Vector reclamation found an active Manifest with an invalid Mapping root");
					}
					return;
				}
				if (!isSystemCatalogIndexDefinitionId(manifest.index_id)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Raw Vector reclamation found a Manifest outside a dropped Vector Index namespace");
				}
			});

			const std::vector<CatalogEntry> currentPools = metadata_.findCurrentVectorRawPagePools(snapshot);
			retiredPools = metadata_.findRetiredVectorRawPagePools(snapshot);
			if (currentPools.size() > options.maximum_raw_pools ||
				retiredPools.size() > options.maximum_raw_pools - currentPools.size()) {
				fail(FormatError::FERR_INVALID_STATE, "relocatable Raw Vector reclamation exceeded its Raw pool scan bound");
			}
			report.raw_pools_scanned = currentPools.size() + retiredPools.size();
			std::map<uint64_t, CatalogEntry> currentPoolsBySegment;
			std::set<uint64_t> candidateSegments;
			for (const CatalogEntry &pool: currentPools) {
				const auto [currentPool, inserted] = currentPoolsBySegment.emplace(pool.segment_id.value(), pool);
				static_cast<void>(currentPool);
				if (!inserted) { // ZYX_COV_EXCL_LINE: CatalogTree exposes one current entry per logical Segment ID.
					fail(FormatError::FERR_INVALID_STATE, // ZYX_COV_EXCL_LINE: CatalogTree has unique Segment IDs.
						 "relocatable Raw Vector reclamation found a malformed current Raw pool identity");
				}
				if (pool.owner_id == 0U || (pool.owner_id & (UINT64_C(1) << 63U)) != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Raw Vector reclamation found a malformed current Raw pool identity");
				}
				const auto active = activeKinds.find(pool.owner_id);
				if (active != activeKinds.end()) {
					const auto manifest = manifestHasNodeMapping.find(pool.owner_id);
					if (active->second != IndexDefinitionKind::IDK_VECTOR || manifest == manifestHasNodeMapping.end() ||
						!manifest->second) { // ZYX_COV_EXCL_LINE: active Manifests require a Mapping root.
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Raw Vector reclamation found an active Raw pool without a Vector Manifest mapping");
					}
					++report.active_raw_pools_retained;
					continue;
				}
				if (!isSystemCatalogIndexDefinitionId(pool.owner_id)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Raw Vector reclamation found a pool owned by an active non-Vector or unknown Index");
				}
				if (manifestHasNodeMapping.contains(pool.owner_id)) {
					++report.stale_manifest_raw_pools_retained;
					continue;
				}
				candidateSegments.insert(pool.segment_id.value());
				candidatePools.push_back(pool);
			}

			std::map<uint64_t, RawPageObservation> rawPages;
			static_cast<void>(metadata_.visitPageLocationsUntil(snapshot, [&](const PageLocation &location) {
				const auto pool = currentPoolsBySegment.find(location.segment_id.value());
				if (pool == currentPoolsBySegment.end()) {
					return true;
				}
				if (report.raw_pages_scanned == options.maximum_raw_pages) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Raw Vector reclamation exceeded its Raw page scan bound");
				}
				++report.raw_pages_scanned;
				const VectorPage page = pages_.read(snapshot, location.page_id);
				const VectorPageHeader header = page.header();
				if (header.kind != VectorStorageKind::VSK_RAW) { // ZYX_COV_EXCL_LINE: VectorPageStore reads only Raw
																 // frames from a Raw pool.
					fail(FormatError::FERR_INVALID_STATE, // ZYX_COV_EXCL_LINE: page-pool type is checked before
														  // decoding. ZYX_COV_EXCL_LINE
						 "relocatable Raw Vector reclamation found a malformed or duplicate Raw page mapping");
				}
				if (header.index_id != pool->second.owner_id) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Raw Vector reclamation found a malformed or duplicate Raw page mapping");
				}
				if (header.page_id != location.page_id) { // ZYX_COV_EXCL_LINE: PageOwnerHeader and Page Directory both
														  // bind the logical Page ID.
					fail(FormatError::FERR_INVALID_STATE, // ZYX_COV_EXCL_LINE: directory and owner header bind Page ID.
						 "relocatable Raw Vector reclamation found a malformed or duplicate Raw page mapping");
				}
				if (header.generation != location.generation) { // ZYX_COV_EXCL_LINE: VectorPageStore validates the
																// owner generation against the Page Directory.
					fail(FormatError::FERR_INVALID_STATE, // ZYX_COV_EXCL_LINE: VectorPageStore checks the generation.
						 "relocatable Raw Vector reclamation found a malformed or duplicate Raw page mapping");
				}
				if (!header.committed_lsn.isValid()) { // ZYX_COV_EXCL_LINE: VectorPageHeaderCodec rejects an invalid
													   // publication LSN.
					fail(FormatError::FERR_INVALID_STATE, // ZYX_COV_EXCL_LINE: the header codec validates publication
														  // LSN. ZYX_COV_EXCL_LINE
						 "relocatable Raw Vector reclamation found a malformed or duplicate Raw page mapping");
				}
				const RawPageObservation observation{location,
													 header.index_id,
													 header.committed_lsn,
													 header.live_count,
													 candidateSegments.contains(location.segment_id.value()),
													 {}};
				if (!rawPages.emplace(location.page_id.value(), observation)
							 .second) { // ZYX_COV_EXCL_LINE: Page Directory traversal yields one record per logical
										// Page ID.
					fail(FormatError::FERR_INVALID_STATE, // ZYX_COV_EXCL_LINE: the Page Directory has unique Page IDs.
						 "relocatable Raw Vector reclamation found a malformed or duplicate Raw page mapping");
				}
				return true;
			}));

			std::set<uint64_t> seenVectorIds;
			metadata_.visitVectorLocations(snapshot, [&](const VectorLocation &location) {
				if (report.vector_locations_scanned == options.maximum_vector_locations) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Raw Vector reclamation exceeded its Vector Location scan bound");
				}
				++report.vector_locations_scanned;
				if (!seenVectorIds.insert(location.vector_id).second) { // ZYX_COV_EXCL_LINE: Vector Location Directory
																		// is keyed by one stable Vector ID.
					fail(FormatError::FERR_INVALID_STATE, // ZYX_COV_EXCL_LINE: the directory has unique Vector IDs.
						 "relocatable Raw Vector reclamation found a duplicate Vector Location identity");
				}
				const auto rawPage = rawPages.find(location.page_id.value());
				if (rawPage == rawPages.end()) { // ZYX_COV_EXCL_LINE: a current Raw Vector Location must resolve
												 // through the current Raw pool directory.
					const VectorPage page = pages_.read(
							snapshot, location.page_id); // ZYX_COV_EXCL_LINE: current
														 // locations are directory-backed. ZYX_COV_EXCL_LINE
					if (page.header().kind ==
						VectorStorageKind::VSK_RAW) { // ZYX_COV_EXCL_LINE: raw pages resolve via their pool.
						fail(FormatError::FERR_INVALID_STATE, // ZYX_COV_EXCL_LINE: the page store rejects this
															  // mismatch. ZYX_COV_EXCL_LINE
							 "relocatable Raw Vector reclamation found a Raw Vector Location outside a current Raw pool");
					}
					return;
				}
				const std::vector<std::byte> record =
						pages_.read(snapshot, location.page_id).read(location.slot_id, location.slot_generation);
				if (record.size() < sizeof(uint64_t) || LittleEndianCodec::readU64(record, 0U) != location.vector_id) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Raw Vector reclamation found a Vector Location with mismatched record identity");
				}
				rawPage->second.vector_ids.push_back(location.vector_id);
			});

			for (auto &[pageId, page]: rawPages) {
				static_cast<void>(pageId);
				std::ranges::sort(page.vector_ids);
				if (std::adjacent_find(page.vector_ids.begin(), page.vector_ids.end()) != page.vector_ids.end() ||
					// ZYX_COV_EXCL_LINE: Vector Location Directory uniqueness prevents duplicate vector IDs per page.
					page.vector_ids.size() != page.live_count) {
					fail(FormatError::FERR_INVALID_STATE, // ZYX_COV_EXCL_LINE: the uniqueness check above prevents
														  // duplicates.
						 "relocatable Raw Vector reclamation found a page whose live slots and Vector Locations disagree");
				}
				if (!page.retire) {
					continue;
				}
				if (retirementPages.size() == options.maximum_retirements) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Raw Vector reclamation exceeded its page-retirement bound");
				}
				retirementPages.push_back({page.location, page.index_id, page.owner_lsn});
				erasedVectorIds.insert(erasedVectorIds.end(), page.vector_ids.begin(), page.vector_ids.end());
			}
			snapshot.release();
		} catch (...) {
			snapshot.release();
			throw;
		}

		std::ranges::sort(retirementPages, {},
						  [](const RetiredRawVectorPage &page) { return page.location.page_id.value(); });
		std::ranges::sort(erasedVectorIds);
		if (std::adjacent_find(erasedVectorIds.begin(), erasedVectorIds.end()) !=
			erasedVectorIds.end()) { // ZYX_COV_EXCL_LINE: Vector Location Directory uniqueness is preserved while each
									 // Raw page is scanned once.
			fail(FormatError::FERR_INVALID_STATE, // ZYX_COV_EXCL_LINE: each source page is visited once.
				 "relocatable Raw Vector reclamation found a Vector ID shared by multiple Raw pages");
		}

		Superblock published = scanned;
		bool hasPublished = false;
		if (!retirementPages.empty()) {
			published = pages_.retireRawPages(report.scanned_generation, retirementPages, erasedVectorIds,
											  checkpointLsn, publishOptions);
			hasPublished = true;
			report.raw_pages_retired = retirementPages.size();
			report.vector_locations_erased = erasedVectorIds.size();
		}
		for (const CatalogEntry &pool: candidatePools) {
			const LogSequenceNumber publicationLsn = hasPublished ? nextCheckpointLsn(published) : checkpointLsn;
			published = pages_.retireEmptyRawPool(pool.segment_id, pool.generation, publicationLsn, publishOptions);
			hasPublished = true;
			++report.raw_pools_retired;
			retiredPools.push_back(pool);
		}
		if (metadata_.activeSnapshotCount() == 0U) {
			std::ranges::sort(retiredPools, {}, &CatalogEntry::segment_id);
			for (const CatalogEntry &pool: retiredPools) {
				const LogSequenceNumber publicationLsn = hasPublished ? nextCheckpointLsn(published) : checkpointLsn;
				published =
						pages_.releaseRetiredRawPool(pool.segment_id, pool.generation, publicationLsn, publishOptions);
				hasPublished = true;
				++report.raw_pools_released;
			}
		}
		report.published_generation = published.generation;
		return report;
	}

} // namespace graph::storage
