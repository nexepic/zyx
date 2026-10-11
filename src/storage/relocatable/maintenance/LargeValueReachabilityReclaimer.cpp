/**
 * @file LargeValueReachabilityReclaimer.cpp
 * @brief Conservative native graph LargeValue reachability reclamation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/metadata/StateRecord.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

namespace graph::storage {

	namespace {

		struct ScannedValue {
			LargeValueManifest manifest;
			std::vector<PageRetirement> retirements;
			size_t manifest_page_count = 0U;
		};

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateOptions(const LargeValueReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn) {
			const uint8_t invalid = static_cast<uint8_t>(options.maximum_manifest_locations == 0U) |
									static_cast<uint8_t>(options.maximum_manifest_metadata_pages == 0U) |
									static_cast<uint8_t>(options.maximum_manifest_nodes == 0U) |
									static_cast<uint8_t>(options.maximum_owner_locations == 0U) |
									static_cast<uint8_t>(options.maximum_owner_metadata_pages == 0U) |
									static_cast<uint8_t>(options.maximum_data_pages == 0U) |
									static_cast<uint8_t>(options.maximum_retirements == 0U) |
									static_cast<uint8_t>(options.maximum_page_locations == 0U) |
									static_cast<uint8_t>(options.maximum_pools == 0U) |
									static_cast<uint8_t>(!options.first_index_slot.isValid()) |
									static_cast<uint8_t>(!options.last_index_slot.isValid()) |
									static_cast<uint8_t>(options.first_index_slot > options.last_index_slot) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue reclamation requires nonzero scan bounds, a valid "
													   "Index pool range, and a checkpoint LSN");
			}
		}

		[[nodiscard]] uint32_t ownerEntityType(EntityPropertyOwnerKind kind) {
			return kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
		}

		void validateScannerMediaType(std::string_view mediaType) {
			if (mediaType.empty() || mediaType.size() > RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "LargeValue ownership scanner has an empty or oversized media type");
			}
			for (const unsigned char character: mediaType) {
				if (character < 0x20U || character > 0x7EU) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "LargeValue ownership scanner media type must be printable ASCII metadata");
				}
			}
		}

		class ReachabilitySink final : public LargeValueOwnershipScanSink {
		public:
			ReachabilitySink(const std::map<uint64_t, ScannedValue> &reclaimableValues,
							 std::set<uint64_t> &reachableValues) :
				reclaimable_values_(reclaimableValues), reachable_values_(reachableValues) {}

			void setExpectedMediaType(std::string_view mediaType) { expected_media_type_ = mediaType; }

			void markReachable(const LargeValueRef &reference) override {
				const auto value = reclaimable_values_.find(reference.value_id.value());
				if (reference.media_type != expected_media_type_ || value == reclaimable_values_.end() ||
					value->second.manifest.reference() != reference) {
					fail(FormatError::FERR_INVALID_STATE,
						 "LargeValue ownership scanner referenced an absent, cross-media, or mismatched Manifest");
				}
				reachable_values_.insert(reference.value_id.value());
			}

		private:
			const std::map<uint64_t, ScannedValue> &reclaimable_values_;
			std::set<uint64_t> &reachable_values_;
			std::string_view expected_media_type_;
		};

	} // namespace

	EntityPropertyLargeValueOwnershipScanner::EntityPropertyLargeValueOwnershipScanner(
			SlottedPagePool &propertyPages, RelocatableMetadataStore &metadata) :
		property_pages_(propertyPages), metadata_(metadata) {}

	std::string_view EntityPropertyLargeValueOwnershipScanner::mediaType() const noexcept {
		return RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
	}

	LargeValueOwnershipScanner::Progress EntityPropertyLargeValueOwnershipScanner::scanReachableAfter(
			const MetadataSnapshotPin &snapshot, uint64_t afterLocationId, size_t maximumLocations,
			size_t maximumMetadataPages, LargeValueOwnershipScanSink &sink) const {
		Progress progress;
		progress.exhausted = metadata_.visitEntityPropertyLocationsAfter(
				snapshot, afterLocationId, maximumMetadataPages,
				[&](const EntityPropertyLocation &location) {
					if (progress.locations_scanned == maximumLocations) {
						return false;
					}
					const SlottedPage page = property_pages_.read(snapshot, location.page_id);
					const std::vector<std::byte> bytes =
							page.read({location.page_id, location.slot_id, location.slot_generation});
					if (!EntityPropertyLargeValueReferenceCodec::hasMagic(bytes)) {
						const PropertyCollectionRoot root = EntityPropertyCollectionRootCodec::decode(bytes);
						if (root.owner_entity_id != location.key.entity_id ||
							root.owner_entity_type != ownerEntityType(location.key.owner_kind)) {
							fail(FormatError::FERR_INVALID_STATE,
								 "LargeValue reclamation found an Entity Property root with mismatched ownership");
						}
					} else {
						const EntityPropertyLargeValueReference reference =
								EntityPropertyLargeValueReferenceCodec::decode(bytes);
						if (reference.owner_entity_id != location.key.entity_id ||
							reference.owner_entity_type != ownerEntityType(location.key.owner_kind) ||
							reference.value.kind != LargeValueKind::LVK_BYTES ||
							reference.value.media_type != mediaType()) {
							fail(FormatError::FERR_INVALID_STATE, "LargeValue reclamation found an Entity Property "
																  "reference with mismatched ownership or type");
						}
						sink.markReachable(reference.value);
					}
					++progress.locations_scanned;
					progress.last_scanned_location_id = location.key.directoryId();
					return true;
				},
				&progress.metadata_pages_scanned);
		return progress;
	}

	StateLargeValueOwnershipScanner::StateLargeValueOwnershipScanner(SlottedPagePool &pages, LargeValueStore &values,
																	 RelocatableMetadataStore &metadata) :
		pages_(pages), values_(values), metadata_(metadata) {}

	std::string_view StateLargeValueOwnershipScanner::mediaType() const noexcept {
		return RSS_STATE_LARGE_VALUE_MEDIA_TYPE;
	}

	LargeValueOwnershipScanner::Progress
	StateLargeValueOwnershipScanner::scanReachableAfter(const MetadataSnapshotPin &snapshot, uint64_t afterLocationId,
														size_t maximumLocations, size_t maximumMetadataPages,
														LargeValueOwnershipScanSink &sink) const {
		Progress progress;
		progress.exhausted = metadata_.visitStateLocationsAfter(
				snapshot, afterLocationId, maximumMetadataPages,
				[&](const StateLocation &location) {
					if (progress.locations_scanned == maximumLocations) {
						return false;
					}
					const SlottedPage page = pages_.read(snapshot, location.page_id);
					const StateRecord record = StateRecordCodec::decode(
							page.read({location.page_id, location.slot_id, location.slot_generation}));
					if (record.state_id != location.state_id) {
						fail(FormatError::FERR_INVALID_STATE,
							 "LargeValue reclamation found a State route that resolves to another State record");
					}
					if (record.storage_kind != StateRecordStorageKind::SRS_INLINE) {
						const LargeValueRef reference = values_.manifest(snapshot, record.large_value_id).reference();
						if (reference.kind != LargeValueKind::LVK_BYTES || reference.media_type != mediaType()) {
							fail(FormatError::FERR_INVALID_STATE,
								 "LargeValue reclamation found a State reference with an invalid content type");
						}
						sink.markReachable(reference);
					}
					++progress.locations_scanned;
					progress.last_scanned_location_id = location.state_id;
					return true;
				},
				&progress.metadata_pages_scanned);
		return progress;
	}

	LargeValueReachabilityReclaimer::LargeValueReachabilityReclaimer(
			LargeValueStore &values, SlottedPagePool &propertyPages, RelocatableMetadataStore &metadata,
			std::span<const LargeValueOwnershipScannerPtr> additionalScanners) :
		values_(values), property_pages_(propertyPages), metadata_(metadata),
		entity_property_scanner_(propertyPages, metadata), state_scanner_(propertyPages, values, metadata),
		additional_scanners_(additionalScanners.begin(), additionalScanners.end()) {
		if (!values_.usesMetadataStore(metadata_) || !property_pages_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument(
					"LargeValue reclaimer requires the value and Property page stores to share one metadata store");
		}
		const auto registerScanner = [this](const LargeValueOwnershipScanner *scanner) {
			if (scanner == nullptr) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue ownership scanner cannot be null");
			}
			const std::string mediaType(scanner->mediaType());
			validateScannerMediaType(mediaType);
			if (!scanners_.emplace(mediaType, scanner).second) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "LargeValue ownership scanners cannot register the same media type twice");
			}
		};
		registerScanner(&entity_property_scanner_);
		registerScanner(&state_scanner_);
		for (const LargeValueOwnershipScannerPtr &scanner: additional_scanners_) {
			registerScanner(scanner.get());
		}
	}

	LargeValueMaintenanceJournalAdvance LargeValueReachabilityReclaimer::advanceJournal(
			const MetadataSnapshotPin &snapshot, const LargeValueMaintenanceJournal &journal,
			const LargeValueReachabilityReclaimOptions &options) const {
		validateOptions(options, LogSequenceNumber(1U));
		if (snapshot.superblock().generation != journal.scan_generation) {
			fail(FormatError::FERR_INVALID_STATE,
				 "LargeValue maintenance journal evidence belongs to another metadata generation");
		}
		if (!additional_scanners_.empty()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "LargeValue maintenance journal cannot resume an unregistered persistent owner namespace");
		}

		LargeValueMaintenanceJournalAdvance result;
		result.journal = journal;
		switch (journal.phase) {
			case LargeValueMaintenanceJournalPhase::LVMJP_ENUMERATE_MANIFESTS: {
				size_t metadataPages = 0U;
				const bool exhausted = metadata_.visitLargeValueManifestLocationsAfter(
					snapshot, journal.manifest_after_value_id, options.maximum_manifest_metadata_pages,
					[&](const LargeValueManifestLocation &route) {
						if (result.manifest_locations_scanned == options.maximum_manifest_locations ||
							result.journal.candidate_value_ids.size() == RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_MAX_IDS) {
							return false;
						}
						++result.manifest_locations_scanned;
						result.journal.manifest_after_value_id = route.value_id.value();
						const LargeValueManifest manifest = values_.manifest(snapshot, route.value_id);
						if (scanners_.contains(manifest.media_type)) {
							result.journal.candidate_value_ids.push_back(route.value_id);
						}
						return true;
					},
					&metadataPages);
				if (result.manifest_locations_scanned == 0U && !exhausted) {
					fail(FormatError::FERR_INVALID_STATE,
						 "LargeValue maintenance Manifest scan bound cannot advance its logical cursor");
				}
				result.journal.manifest_scan_exhausted = exhausted;
				if (result.journal.candidate_value_ids.empty()) {
					result.completed_without_candidates = exhausted;
					return result;
				}
				result.journal.phase = LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES;
				return result;
			}
			case LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES:
			case LargeValueMaintenanceJournalPhase::LVMJP_SCAN_STATES:
			case LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM:
				break;
		}

		std::map<uint64_t, ScannedValue> candidates;
		for (const LargeValueId valueId: journal.candidate_value_ids) {
			const LargeValueManifest manifest = values_.manifest(snapshot, valueId);
			if (!scanners_.contains(manifest.media_type)) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue maintenance journal contains an unregistered candidate media type");
			}
			ScannedValue candidate;
			candidate.manifest = manifest;
			if (!candidates.emplace(valueId.value(), std::move(candidate)).second) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue maintenance journal contains duplicate candidate identities");
			}
		}
		std::set<uint64_t> reachable;
		for (const LargeValueId valueId: journal.reachable_value_ids) {
			if (!candidates.contains(valueId.value())) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue maintenance journal marks a non-candidate value as reachable");
			}
			reachable.insert(valueId.value());
		}
		ReachabilitySink sink(candidates, reachable);
		const auto persistReachable = [&] {
			result.journal.reachable_value_ids.clear();
			result.journal.reachable_value_ids.reserve(reachable.size());
			for (const uint64_t valueId: reachable) {
				result.journal.reachable_value_ids.emplace_back(valueId);
			}
		};

		if (journal.phase == LargeValueMaintenanceJournalPhase::LVMJP_SCAN_ENTITY_PROPERTIES) {
			sink.setExpectedMediaType(entity_property_scanner_.mediaType());
			const LargeValueOwnershipScanner::Progress progress = entity_property_scanner_.scanReachableAfter(
				snapshot, journal.entity_property_after_directory_id, options.maximum_owner_locations,
				options.maximum_owner_metadata_pages, sink);
			if (progress.locations_scanned > options.maximum_owner_locations ||
				progress.metadata_pages_scanned > options.maximum_owner_metadata_pages) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue maintenance Entity Property scanner exceeded its declared bounds");
			}
			result.owner_locations_scanned = progress.locations_scanned;
			result.owner_metadata_pages_scanned = progress.metadata_pages_scanned;
			if (!progress.exhausted && progress.locations_scanned == 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue maintenance Entity Property scan bound cannot advance its logical cursor");
			}
			result.journal.entity_property_after_directory_id = progress.last_scanned_location_id;
			if (progress.exhausted) {
				result.journal.phase = LargeValueMaintenanceJournalPhase::LVMJP_SCAN_STATES;
			}
			persistReachable();
			return result;
		}

		if (journal.phase == LargeValueMaintenanceJournalPhase::LVMJP_SCAN_STATES) {
			sink.setExpectedMediaType(state_scanner_.mediaType());
			const LargeValueOwnershipScanner::Progress progress = state_scanner_.scanReachableAfter(
				snapshot, journal.state_after_id, options.maximum_owner_locations, options.maximum_owner_metadata_pages, sink);
			if (progress.locations_scanned > options.maximum_owner_locations ||
				progress.metadata_pages_scanned > options.maximum_owner_metadata_pages) {
				fail(FormatError::FERR_INVALID_STATE, "LargeValue maintenance State scanner exceeded its declared bounds");
			}
			result.owner_locations_scanned = progress.locations_scanned;
			result.owner_metadata_pages_scanned = progress.metadata_pages_scanned;
			if (!progress.exhausted && progress.locations_scanned == 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue maintenance State scan bound cannot advance its logical cursor");
			}
			result.journal.state_after_id = progress.last_scanned_location_id;
			if (progress.exhausted) {
				result.journal.phase = LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM;
			}
			persistReachable();
			return result;
		}

		persistReachable();
		return result;
	}

	LargeValueReachabilityReclaimReport
	LargeValueReachabilityReclaimer::reclaimJournal(
			MetadataSnapshotPin &snapshot, const LargeValueMaintenanceJournal &journal,
			const LargeValueReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &publishOptions) {
		validateOptions(options, checkpointLsn);
		if (snapshot.superblock().generation != journal.scan_generation ||
			journal.phase != LargeValueMaintenanceJournalPhase::LVMJP_RECLAIM) {
			fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue maintenance reclaim requires complete journal evidence for the pinned generation");
		}
		if (!additional_scanners_.empty()) {
			fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue maintenance journal cannot reclaim without persistent evidence for every owner namespace");
		}

		LargeValueReachabilityReclaimReport report;
		report.scanned_generation = snapshot.superblock().generation;
		std::map<uint64_t, ScannedValue> reclaimableValues;
		std::set<uint64_t> reachableValues;
		std::set<uint64_t> observedPages;
		std::map<uint64_t, std::set<uint64_t>> retiredPageIdsBySegment;
		std::map<uint64_t, CatalogEntry> frameReclaimPools;
		std::map<uint64_t, CatalogEntry> sealedPoolCandidates;
		std::map<uint64_t, CatalogEntry> retiredPoolCandidates;
		try {
			for (const LargeValueId valueId: journal.candidate_value_ids) {
				const LargeValueManifest manifest = values_.manifest(snapshot, valueId);
				if (!scanners_.contains(manifest.media_type)) {
					fail(FormatError::FERR_INVALID_STATE,
							 "LargeValue maintenance journal contains an unregistered candidate media type");
				}
				if (report.manifest_locations_scanned == options.maximum_manifest_locations) {
					fail(FormatError::FERR_INVALID_STATE,
							 "LargeValue maintenance journal exceeds its Manifest Directory scan bound");
				}
				++report.manifest_locations_scanned;
				ScannedValue scanned;
				scanned.manifest = manifest;
				const std::vector<LargeValueManifest> manifestNodes = values_.manifestTree(snapshot, manifest);
				for (const LargeValueManifest &node: manifestNodes) {
					if (report.manifest_nodes_scanned == options.maximum_manifest_nodes) {
						fail(FormatError::FERR_INVALID_STATE,
								 "LargeValue maintenance journal exceeds its Manifest node bound");
					}
					++report.manifest_nodes_scanned;
					if (!observedPages.insert(node.page_id.value()).second) {
						fail(FormatError::FERR_INVALID_STATE,
								 "LargeValue maintenance journal found duplicate Manifest Page identities");
					}
					scanned.retirements.push_back(values_.planPageRetirement(
							snapshot, node.page_id, node.value_id, PageOwnerKind::POK_LARGE_VALUE_MANIFEST));
				}
				scanned.manifest_page_count = manifestNodes.size();
				for (const LargeValueManifest &node: manifestNodes) {
					if (!node.isLeaf()) {
						continue;
					}
					for (const LargeValueChunk &chunk: node.chunks) {
						for (uint16_t ordinal = 0U; ordinal < chunk.data_page_count; ++ordinal) {
							if (report.data_pages_scanned == options.maximum_data_pages) {
								fail(FormatError::FERR_INVALID_STATE,
										 "LargeValue maintenance journal exceeds its data-page bound");
							}
							const PageId pageId(chunk.first_data_page_id.value() + ordinal);
							++report.data_pages_scanned;
							if (!observedPages.insert(pageId.value()).second) {
								fail(FormatError::FERR_INVALID_STATE,
										 "LargeValue maintenance journal found duplicate Manifest or data Page identities");
							}
							scanned.retirements.push_back(values_.planPageRetirement(
									snapshot, pageId, manifest.value_id, PageOwnerKind::POK_LARGE_VALUE_DATA));
						}
					}
				}
				if (!reclaimableValues.emplace(valueId.value(), std::move(scanned)).second) {
					fail(FormatError::FERR_INVALID_STATE,
							 "LargeValue maintenance journal contains duplicate candidate identities");
				}
			}
			for (const LargeValueId valueId: journal.reachable_value_ids) {
				if (!reclaimableValues.contains(valueId.value())) {
					fail(FormatError::FERR_INVALID_STATE,
							 "LargeValue maintenance journal marks a non-candidate value as reachable");
				}
				reachableValues.insert(valueId.value());
			}

			for (const auto &[valueId, scanned]: reclaimableValues) {
				if (reachableValues.contains(valueId)) {
					continue;
				}
				for (const PageRetirement &retirement: scanned.retirements) {
					static_cast<void>(retiredPageIdsBySegment[retirement.location.segment_id.value()].insert(
							retirement.location.page_id.value()));
				}
			}
			const auto retainSealedPool = [&](const CatalogEntry &pool) {
				static_cast<void>(frameReclaimPools.emplace(pool.segment_id.value(), pool));
				if (frameReclaimPools.size() > options.maximum_pools - retiredPoolCandidates.size()) {
					fail(FormatError::FERR_INVALID_STATE, "LargeValue maintenance journal exceeded its pool bound");
				}
			};
			metadata_.visitCatalogEntries(snapshot, [&](const CatalogEntry &pool) {
				if (pool.kind != SegmentKind::SGK_LARGE_VALUE_DATA_POOL &&
					pool.kind != SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) {
					return;
				}
				const uint64_t expectedPoolOwner = pool.kind == SegmentKind::SGK_LARGE_VALUE_DATA_POOL
													   ? RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID
													   : RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
				if (pool.owner_id != expectedPoolOwner) {
					fail(FormatError::FERR_INVALID_STATE,
							 "LargeValue maintenance journal found a pool outside its shared namespace");
				}
				if (pool.state == SegmentState::SGS_RETIRED) {
					if (retiredPoolCandidates.size() == options.maximum_pools - frameReclaimPools.size()) {
						fail(FormatError::FERR_INVALID_STATE,
								 "LargeValue maintenance journal exceeded its retired-pool bound");
					}
					static_cast<void>(retiredPoolCandidates.emplace(pool.segment_id.value(), pool));
					return;
				}
				if (pool.state != SegmentState::SGS_SEALED) {
					return;
				}
				const bool hasPlannedRetirements = retiredPageIdsBySegment.contains(pool.segment_id.value());
				retainSealedPool(pool);
				if (hasPlannedRetirements) {
					static_cast<void>(sealedPoolCandidates.emplace(pool.segment_id.value(), pool));
				}
			});
			static_cast<void>(metadata_.visitPageLocationsUntil(snapshot, [&](const PageLocation &location) {
				if (report.page_locations_scanned == options.maximum_page_locations) {
					fail(FormatError::FERR_INVALID_STATE,
							 "LargeValue maintenance journal exceeded its Page Directory scan bound");
				}
				++report.page_locations_scanned;
				const auto candidate = sealedPoolCandidates.find(location.segment_id.value());
				if (candidate == sealedPoolCandidates.end()) {
					return true;
				}
				const auto planned = retiredPageIdsBySegment.find(location.segment_id.value());
				if (planned == retiredPageIdsBySegment.end() || !planned->second.contains(location.page_id.value())) {
					sealedPoolCandidates.erase(candidate);
				}
				return true;
			}));
			report.pools_scanned = frameReclaimPools.size() + retiredPoolCandidates.size();
			snapshot.release();
		} catch (...) {
			snapshot.release();
			throw;
		}

		std::vector<LargeValueId> erasedValueIds;
		std::vector<PageRetirement> retirements;
		for (const auto &[valueId, scanned]: reclaimableValues) {
			if (reachableValues.contains(valueId)) {
				++report.reachable_values;
				continue;
			}
			if (scanned.retirements.size() > options.maximum_retirements - retirements.size()) {
				fail(FormatError::FERR_INVALID_STATE,
						 "LargeValue maintenance journal exceeded its retirement bound before publication");
			}
			erasedValueIds.emplace_back(valueId);
			retirements.insert(retirements.end(), scanned.retirements.begin(), scanned.retirements.end());
			++report.retired_values;
			report.retired_manifest_pages += scanned.manifest_page_count;
			report.retired_data_pages += scanned.retirements.size() - scanned.manifest_page_count;
		}
		std::vector<CatalogEntry> sealedPools;
		sealedPools.reserve(sealedPoolCandidates.size());
		for (const auto &[segmentId, pool]: sealedPoolCandidates) {
			static_cast<void>(segmentId);
			sealedPools.push_back(pool);
		}
		uint64_t remainingPublicationIncrements = std::numeric_limits<uint64_t>::max() - checkpointLsn.value();
		bool hasReservedPublication = false;
		const auto reservePublicationLsns = [&](size_t count) {
			if (count == 0U) {
				return;
			}
			if (!hasReservedPublication) {
				hasReservedPublication = true;
				--count;
			}
			if (count > remainingPublicationIncrements) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "LargeValue maintenance journal lacks checkpoint LSNs for its publication sequence");
			}
			remainingPublicationIncrements -= static_cast<uint64_t>(count);
		};
		reservePublicationLsns(retirements.empty() ? 0U : 1U);
		reservePublicationLsns(sealedPools.size());
		reservePublicationLsns(retiredPoolCandidates.size());
		reservePublicationLsns(sealedPools.size());
		uint64_t publicationOrdinal = 0U;
		const auto takePublicationLsn = [&] { return LogSequenceNumber(checkpointLsn.value() + publicationOrdinal++); };
		Superblock published;
		bool hasPublished = false;
		if (!retirements.empty()) {
			std::ranges::sort(retirements, {},
							  [](const PageRetirement &retirement) { return retirement.location.page_id.value(); });
			published = values_.retireUnreachablePages(report.scanned_generation, erasedValueIds, retirements,
												   options.first_index_slot, options.last_index_slot,
												   takePublicationLsn(), publishOptions);
			hasPublished = true;
		}
		for (const auto &[segmentId, pool]: frameReclaimPools) {
			static_cast<void>(segmentId);
			values_.reclaimRetiredFrames(pool.segment_id);
		}
		for (const CatalogEntry &pool: sealedPools) {
			published = values_.retireEmptyPool(pool.segment_id, pool.generation, takePublicationLsn(), publishOptions);
			hasPublished = true;
			++report.empty_pools_retired;
			retiredPoolCandidates.emplace(pool.segment_id.value(), pool);
		}
		if (metadata_.activeSnapshotCount() == 0U) {
			for (const auto &[segmentId, pool]: retiredPoolCandidates) {
				static_cast<void>(segmentId);
				published = values_.releaseRetiredEmptyPool(pool.segment_id, pool.generation, takePublicationLsn(),
															publishOptions);
				hasPublished = true;
				++report.retired_pools_released;
			}
		}
		report.published_generation = hasPublished ? published.generation : report.scanned_generation;
		return report;
	}

	LargeValueReachabilityReclaimReport
	LargeValueReachabilityReclaimer::reclaim(const LargeValueReachabilityReclaimOptions &options,
											 LogSequenceNumber checkpointLsn,
											 const MetadataPublishOptions &publishOptions) {
		validateOptions(options, checkpointLsn);
		LargeValueReachabilityReclaimReport report;
		std::map<uint64_t, ScannedValue> reclaimableValues;
		std::set<uint64_t> reachableValues;
		std::set<uint64_t> observedPages;
		std::map<uint64_t, std::set<uint64_t>> retiredPageIdsBySegment;
		std::map<uint64_t, CatalogEntry> frameReclaimPools;
		std::map<uint64_t, CatalogEntry> sealedPoolCandidates;
		std::map<uint64_t, CatalogEntry> retiredPoolCandidates;

		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			report.scanned_generation = snapshot.superblock().generation;
			size_t manifestMetadataPagesScanned = 0U;
			const bool manifestsExhausted = metadata_.visitLargeValueManifestLocationsAfter(
					snapshot, 0U, options.maximum_manifest_metadata_pages,
					[&](const LargeValueManifestLocation &route) {
				if (report.manifest_locations_scanned == options.maximum_manifest_locations) {
					fail(FormatError::FERR_INVALID_STATE,
						 "LargeValue reclamation exceeded its Manifest Directory scan bound");
				}
				++report.manifest_locations_scanned;
				const LargeValueManifest manifest = values_.manifest(snapshot, route.value_id);
				ScannedValue scanned;
				scanned.manifest = manifest;
				const std::vector<LargeValueManifest> manifestNodes = values_.manifestTree(snapshot, manifest);
				for (const LargeValueManifest &node: manifestNodes) {
					if (report.manifest_nodes_scanned == options.maximum_manifest_nodes) {
						fail(FormatError::FERR_INVALID_STATE,
							 "LargeValue reclamation exceeded its Manifest node scan bound");
					}
					++report.manifest_nodes_scanned;
					scanned.retirements.push_back(values_.planPageRetirement(snapshot, node.page_id, node.value_id,
																			 PageOwnerKind::POK_LARGE_VALUE_MANIFEST));
					static_cast<void>(observedPages.insert(node.page_id.value()));
				}
				scanned.manifest_page_count = manifestNodes.size();
				for (const LargeValueManifest &node: manifestNodes) {
					if (!node.isLeaf()) {
						continue;
					}
					for (const LargeValueChunk &chunk: node.chunks) {
						for (uint16_t ordinal = 0U; ordinal < chunk.data_page_count; ++ordinal) {
							if (report.data_pages_scanned == options.maximum_data_pages) {
								// Manifest decoding already rejects overflowing PageId ranges before
								// reclamation can inspect a chunk.
								fail(FormatError::FERR_INVALID_STATE,
									 "LargeValue reclamation exceeded its data-page bound");
							}
							const PageId pageId(chunk.first_data_page_id.value() + ordinal);
							++report.data_pages_scanned;
							if (!observedPages.insert(pageId.value()).second) {
								fail(FormatError::FERR_INVALID_STATE,
									 "LargeValue reclamation found duplicate manifest or data Page identities");
							}
							scanned.retirements.push_back(values_.planPageRetirement(
									snapshot, pageId, manifest.value_id, PageOwnerKind::POK_LARGE_VALUE_DATA));
						}
					}
				}
				if (!scanners_.contains(manifest.media_type)) {
					++report.retained_unregistered_values;
					return true;
				}
				static_cast<void>(reclaimableValues.emplace(manifest.value_id.value(), std::move(scanned)));
				return true;
			},
					&manifestMetadataPagesScanned);
			if (!manifestsExhausted) {
				fail(FormatError::FERR_INVALID_STATE,
						 "LargeValue reclamation requires a complete Manifest Directory scan within its metadata-page bound");
			}

			ReachabilitySink sink(reclaimableValues, reachableValues);
			size_t ownerLocationsScanned = 0U;
			size_t ownerMetadataPagesScanned = 0U;
			for (const auto &[mediaType, scanner]: scanners_) {
				sink.setExpectedMediaType(mediaType);
				const size_t remaining = options.maximum_owner_locations - ownerLocationsScanned;
				const size_t remainingMetadataPages = options.maximum_owner_metadata_pages - ownerMetadataPagesScanned;
				const LargeValueOwnershipScanner::Progress progress =
						scanner->scanReachableAfter(snapshot, 0U, remaining, remainingMetadataPages, sink);
				if (progress.locations_scanned > remaining ||
					progress.metadata_pages_scanned > remainingMetadataPages || !progress.exhausted) {
					fail(FormatError::FERR_INVALID_STATE,
						 "LargeValue reclamation requires a complete typed owner scan within its declared bounds");
				}
				ownerLocationsScanned += progress.locations_scanned;
				ownerMetadataPagesScanned += progress.metadata_pages_scanned;
			}
			report.owner_locations_scanned = ownerLocationsScanned;
			// Track every planned retirement by physical pool. A shared data pool can
			// be retired only when the Page Directory proves that every current route
			// in that pool belongs to this complete retirement plan.
			for (const auto &[valueId, scanned]: reclaimableValues) {
				if (reachableValues.contains(valueId)) {
					continue;
				}
				for (const PageRetirement &retirement: scanned.retirements) {
					static_cast<void>(retiredPageIdsBySegment[retirement.location.segment_id.value()].insert(
							retirement.location.page_id.value()));
				}
			}
			const auto retainSealedPool = [&](const CatalogEntry &pool) {
				static_cast<void>(frameReclaimPools.emplace(pool.segment_id.value(), pool));
				if (frameReclaimPools.size() > options.maximum_pools - retiredPoolCandidates.size()) {
					fail(FormatError::FERR_INVALID_STATE, "LargeValue reclamation exceeded its pool bound");
				}
			};
			metadata_.visitCatalogEntries(snapshot, [&](const CatalogEntry &pool) {
				if (pool.kind != SegmentKind::SGK_LARGE_VALUE_DATA_POOL &&
					pool.kind != SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) {
					return;
				}
				const uint64_t expectedPoolOwner = pool.kind == SegmentKind::SGK_LARGE_VALUE_DATA_POOL
														   ? RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID
														   : RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
				if (pool.owner_id != expectedPoolOwner) {
					fail(FormatError::FERR_INVALID_STATE,
						 "LargeValue reclamation found a pool outside its shared namespace");
				}
				if (pool.state == SegmentState::SGS_RETIRED) {
					if (retiredPoolCandidates.size() == options.maximum_pools - frameReclaimPools.size()) {
						fail(FormatError::FERR_INVALID_STATE, "LargeValue reclamation exceeded its retired-pool bound");
					}
					static_cast<void>(retiredPoolCandidates.emplace(pool.segment_id.value(), pool));
					return;
				}
				if (pool.state != SegmentState::SGS_SEALED) {
					return;
				}
				const bool hasPlannedRetirements = retiredPageIdsBySegment.contains(pool.segment_id.value());
				retainSealedPool(pool);
				if (hasPlannedRetirements) {
					static_cast<void>(sealedPoolCandidates.emplace(pool.segment_id.value(), pool));
				}
			});
			// A shared pool remains sealed when one or more current routes survive.
			// The snapshot's Page Directory is authoritative; the Catalog owner is
			// only a shared namespace and must not be mistaken for value ownership.
			static_cast<void>(metadata_.visitPageLocationsUntil(snapshot, [&](const PageLocation &location) {
				if (report.page_locations_scanned == options.maximum_page_locations) {
					fail(FormatError::FERR_INVALID_STATE,
						 "LargeValue reclamation exceeded its Page Directory scan bound");
				}
				++report.page_locations_scanned;
				const auto candidate = sealedPoolCandidates.find(location.segment_id.value());
				if (candidate == sealedPoolCandidates.end()) {
					return true;
				}
				const auto planned = retiredPageIdsBySegment.find(location.segment_id.value());
				if (planned == retiredPageIdsBySegment.end() || !planned->second.contains(location.page_id.value())) {
					sealedPoolCandidates.erase(candidate);
				}
				return true;
			}));
			report.pools_scanned = frameReclaimPools.size() + retiredPoolCandidates.size();
			snapshot.release();
		} catch (...) {
			snapshot.release();
			throw;
		}

		std::vector<LargeValueId> erasedValueIds;
		std::vector<PageRetirement> retirements;
		for (const auto &[valueId, scanned]: reclaimableValues) {
			if (reachableValues.contains(valueId)) {
				++report.reachable_values;
				continue;
			}
			// Every preceding iteration proves this subtraction is safe: the
			// accumulator starts empty and is appended only after this bound passes.
			if (scanned.retirements.size() > options.maximum_retirements - retirements.size()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue reclamation exceeded its retirement bound before publication");
			}
			erasedValueIds.emplace_back(valueId);
			retirements.insert(retirements.end(), scanned.retirements.begin(), scanned.retirements.end());
			++report.retired_values;
			report.retired_manifest_pages += scanned.manifest_page_count;
			report.retired_data_pages += scanned.retirements.size() - scanned.manifest_page_count;
		}
		std::vector<CatalogEntry> sealedPools;
		sealedPools.reserve(sealedPoolCandidates.size());
		for (const auto &[segmentId, pool]: sealedPoolCandidates) {
			static_cast<void>(segmentId);
			sealedPools.push_back(pool);
		}
		// Reserve the worst-case publication sequence before mutating metadata. A
		// caller's snapshot may drain while this pass runs, so every current and
		// newly retired pool can require a later release publication.
		uint64_t remainingPublicationIncrements = std::numeric_limits<uint64_t>::max() - checkpointLsn.value();
		bool hasReservedPublication = false;
		const auto reservePublicationLsns = [&](size_t count) {
			if (count == 0U) {
				return;
			}
			if (!hasReservedPublication) {
				hasReservedPublication = true;
				--count;
			}
			if (count > remainingPublicationIncrements) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "LargeValue reclamation does not have enough checkpoint LSNs for its publication sequence");
			}
			remainingPublicationIncrements -= static_cast<uint64_t>(count);
		};
		reservePublicationLsns(retirements.empty() ? 0U : 1U);
		reservePublicationLsns(sealedPools.size());
		reservePublicationLsns(retiredPoolCandidates.size());
		reservePublicationLsns(sealedPools.size());
		uint64_t publicationOrdinal = 0U;
		const auto takePublicationLsn = [&] { return LogSequenceNumber(checkpointLsn.value() + publicationOrdinal++); };
		Superblock published;
		bool hasPublished = false;
		if (!retirements.empty()) {
			std::ranges::sort(retirements, {},
							  [](const PageRetirement &retirement) { return retirement.location.page_id.value(); });
			published = values_.retireUnreachablePages(report.scanned_generation, erasedValueIds, retirements,
													   options.first_index_slot, options.last_index_slot,
													   takePublicationLsn(), publishOptions);
			hasPublished = true;
		}

		// Reconciliation releases only retired frames whose old Page Directory
		// roots are no longer visible. It must run for every current shared pool,
		// including a pool that retained another value after this pass.
		for (const auto &[segmentId, pool]: frameReclaimPools) {
			static_cast<void>(segmentId);
			values_.reclaimRetiredFrames(pool.segment_id);
		}

		for (const CatalogEntry &pool: sealedPools) {
			published = values_.retireEmptyPool(pool.segment_id, pool.generation, takePublicationLsn(), publishOptions);
			hasPublished = true;
			++report.empty_pools_retired;
			// The pool is now in the same retired-slot protocol as pools found at
			// the beginning of this pass. Keep it in the release batch so a pass
			// without pinned snapshots does not require an unnecessary retry.
			retiredPoolCandidates.emplace(pool.segment_id.value(), pool);
		}

		if (metadata_.activeSnapshotCount() == 0U) {
			for (const auto &[segmentId, pool]: retiredPoolCandidates) {
				static_cast<void>(segmentId);
				published = values_.releaseRetiredEmptyPool(pool.segment_id, pool.generation, takePublicationLsn(),
															publishOptions);
				hasPublished = true;
				++report.retired_pools_released;
			}
		}
		report.published_generation = hasPublished ? published.generation : report.scanned_generation;
		return report;
	}

} // namespace graph::storage
