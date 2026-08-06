/**
 * @file VectorPagePoolReclaimer.cpp
 * @brief Bounded empty-pool retirement after Vector page/root reclamation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/VectorPagePoolReclaimer.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateOptions(const VectorPagePoolReclaimOptions &options, LogSequenceNumber checkpointLsn) {
			const uint8_t invalid = static_cast<uint8_t>(options.maximum_index_definitions == 0U) |
									static_cast<uint8_t>(options.maximum_catalog_entries == 0U) |
									static_cast<uint8_t>(options.maximum_page_locations == 0U) |
									static_cast<uint8_t>(options.maximum_retire_queue_entries == 0U) |
									static_cast<uint8_t>(options.maximum_pools == 0U) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector page-pool reclamation requires nonzero bounds and a checkpoint LSN");
			}
		}

		[[nodiscard]] bool isCandidateKind(SegmentKind kind) {
			return kind == SegmentKind::SGK_INDEX_PAGE_POOL || kind == SegmentKind::SGK_VECTOR_MANIFEST_POOL ||
				   kind == SegmentKind::SGK_VECTOR_PQ_POOL || kind == SegmentKind::SGK_VECTOR_ADJACENCY_POOL;
		}

		[[nodiscard]] LogSequenceNumber nextCheckpointLsn(const Superblock &published) {
			if (!published.checkpoint_lsn
						 .isValid() || // ZYX_COV_EXCL_LINE: published Superblocks are decoded before use.
				published.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "relocatable Vector page-pool reclamation cannot advance the checkpoint LSN");
			}
			return LogSequenceNumber(published.checkpoint_lsn.value() + 1U);
		}

	} // namespace

	VectorPagePoolReclaimer::VectorPagePoolReclaimer(SlottedPagePool &pages, SystemCatalog &catalog,
			RelocatableMetadataStore &metadata) : pages_(pages), catalog_(catalog), metadata_(metadata) {
		if (!pages_.usesMetadataStore(metadata_) || !catalog_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument(
					"relocatable canonical Vector page-pool reclaimer requires stores attached to one metadata store");
		}
	}

	VectorPagePoolReclaimReport VectorPagePoolReclaimer::reclaim(const VectorPagePoolReclaimOptions &options,
																 LogSequenceNumber checkpointLsn,
																 const MetadataPublishOptions &publishOptions) {
		validateOptions(options, checkpointLsn);
		VectorPagePoolReclaimReport report;
		std::vector<CatalogEntry> currentEmptyPools;
		std::vector<CatalogEntry> retiredPools;
		Superblock scanned;

		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			scanned = snapshot.superblock();
			report.scanned_generation = scanned.generation;
			std::set<uint64_t> relocatedSourceSegments;
			metadata_.visitRetireQueueEntries(snapshot, [&](const RetireQueueEntry &entry) {
				if (report.retire_queue_entries_scanned == options.maximum_retire_queue_entries) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector page-pool reclamation exceeded its Retire Queue scan bound");
				}
				++report.retire_queue_entries_scanned;
				if (entry.disposition == RetireQueueDisposition::RQD_RELOCATED_SOURCE &&
					!relocatedSourceSegments.insert(entry.segment_id.value()).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector page-pool reclamation found duplicate pending relocation sources");
				}
			});
			const std::vector<IndexDefinition> definitions =
					catalog_.readIndexDefinitions(snapshot, options.maximum_index_definitions);
			std::map<uint64_t, IndexDefinitionKind> activeKinds;
			for (const IndexDefinition &definition: definitions) {
				if (!isSystemCatalogIndexDefinitionId(definition.index_id)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector page-pool reclamation found an invalid Catalog Index namespace");
				}
				if (!activeKinds.emplace(definition.index_id, definition.kind)
							 .second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector page-pool reclamation found duplicate active public Index IDs");
				}
			}
			report.index_definitions = definitions.size();

			std::set<uint64_t> nonemptySegments;
			static_cast<void>(metadata_.visitPageLocationsUntil(snapshot, [&](const PageLocation &location) {
				if (report.page_locations_scanned == options.maximum_page_locations) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector page-pool reclamation exceeded its Page Directory scan bound");
				}
				++report.page_locations_scanned;
				nonemptySegments.insert(location.segment_id.value());
				return true;
			}));
			const auto isVectorOwnedPool = [&](const CatalogEntry &pool) {
				if (pool.kind == SegmentKind::SGK_INDEX_PAGE_POOL &&
					VectorPqMapping::isDerivedMappingIndexId(pool.owner_id)) {
					return true;
				}
				if (pool.owner_id == 0U || (pool.owner_id & (UINT64_C(1) << 63U)) != 0U) {
					if (pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Vector page-pool reclamation found a Vector pool with a reserved owner identity");
					}
					return false;
				}
				const auto active = activeKinds.find(pool.owner_id);
				if (active != activeKinds.end()) {
					if (active->second == IndexDefinitionKind::IDK_VECTOR) {
						return true;
					}
					if (pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Vector page-pool reclamation found a Vector pool owned by a non-Vector Index");
					}
					return false;
				}
				if (isSystemCatalogIndexDefinitionId(pool.owner_id)) {
					return true;
				}
				if (pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector page-pool reclamation found a Vector pool outside a dropped Vector namespace");
				}
				return false;
			};

			metadata_.visitCatalogEntries(snapshot, [&](const CatalogEntry &pool) {
				if (report.catalog_entries_scanned == options.maximum_catalog_entries) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector page-pool reclamation exceeded its Catalog scan bound");
				}
				++report.catalog_entries_scanned;
				if (!isCandidateKind(pool.kind)) {
					return;
				}
				if (!metadata_.isPhysicalSlotAllocated(snapshot, pool.physical_slot)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector page-pool reclamation found an unallocated typed pool slot");
				}
				if (pool.state == SegmentState::SGS_OBSOLETE) {
					return;
				}
				if (pool.state != SegmentState::SGS_SEALED &&
					pool.state != SegmentState::SGS_RETIRED) { // ZYX_COV_EXCL_LINE: CatalogPage permits only sealed,
															   // retired, or obsolete entries.
					return;
				}
				if (!isVectorOwnedPool(pool)) {
					return;
				}
				if (relocatedSourceSegments.contains(pool.segment_id.value())) {
					++report.relocated_pools_retained;
					return;
				}
				if (pool.state == SegmentState::SGS_RETIRED) {
					if (retiredPools.size() + currentEmptyPools.size() == options.maximum_pools) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Vector page-pool reclamation exceeded its retired-pool bound");
					}
					retiredPools.push_back(pool);
					return;
				}
				if (nonemptySegments.contains(pool.segment_id.value())) {
					++report.nonempty_pools_retained;
					return;
				}
				if (currentEmptyPools.size() + retiredPools.size() == options.maximum_pools) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector page-pool reclamation exceeded its empty-pool bound");
				}
				currentEmptyPools.push_back(pool);
			});
			snapshot.release();
		} catch (...) {
			snapshot.release();
			throw;
		}

		std::ranges::sort(currentEmptyPools, {}, &CatalogEntry::segment_id);
		std::ranges::sort(retiredPools, {}, &CatalogEntry::segment_id);
		Superblock published = scanned;
		bool hasPublished = false;
		for (const CatalogEntry &pool: currentEmptyPools) {
			const LogSequenceNumber publicationLsn = hasPublished ? nextCheckpointLsn(published) : checkpointLsn;
			published = pages_.retireEmptyPool(pool.segment_id, pool.generation, publicationLsn, publishOptions);
			hasPublished = true;
			++report.empty_pools_retired;
			retiredPools.push_back(pool);
		}
		if (metadata_.activeSnapshotCount() == 0U) {
			std::ranges::sort(retiredPools, {}, &CatalogEntry::segment_id);
			for (const CatalogEntry &pool: retiredPools) {
				const LogSequenceNumber publicationLsn = hasPublished ? nextCheckpointLsn(published) : checkpointLsn;
				published = pages_.releaseRetiredEmptyPool(pool.segment_id, pool.generation, publicationLsn,
														   publishOptions);
				hasPublished = true;
				++report.retired_pools_released;
			}
		}
		report.published_generation = hasPublished ? published.generation : report.scanned_generation;
		return report;
	}

} // namespace graph::storage
