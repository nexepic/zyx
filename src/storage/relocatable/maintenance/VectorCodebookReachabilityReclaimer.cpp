/**
 * @file VectorCodebookReachabilityReclaimer.cpp
 * @brief Snapshot-safe retirement of Manifests' immutable Codebook extents.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/maintenance/VectorCodebookReachabilityReclaimer.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void validateOptions(const VectorCodebookReachabilityReclaimOptions &options, LogSequenceNumber checkpointLsn) {
			const uint8_t invalid = static_cast<uint8_t>(options.maximum_vector_manifests == 0U) |
									static_cast<uint8_t>(options.maximum_vector_manifests == std::numeric_limits<size_t>::max()) |
									static_cast<uint8_t>(options.maximum_extent_locations == 0U) |
									static_cast<uint8_t>(options.maximum_extent_locations == std::numeric_limits<size_t>::max()) |
									static_cast<uint8_t>(options.maximum_catalog_entries == 0U) |
									static_cast<uint8_t>(options.maximum_catalog_entries == std::numeric_limits<size_t>::max()) |
									static_cast<uint8_t>(options.maximum_retire_queue_entries == 0U) |
									static_cast<uint8_t>(options.maximum_retire_queue_entries == std::numeric_limits<size_t>::max()) |
									static_cast<uint8_t>(options.maximum_retirements == 0U) |
									static_cast<uint8_t>(options.maximum_retirements == std::numeric_limits<size_t>::max()) |
									static_cast<uint8_t>(!checkpointLsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector Codebook reclamation requires finite nonzero bounds and a checkpoint LSN");
			}
		}

		[[nodiscard]] bool isFullSegmentExtent(const ExtentLocation &location) {
			return location.first_page == RSS_PAGE_POOL_FIRST_DATA_FRAME &&
				   location.page_count == RSS_PAGE_POOL_DATA_FRAME_COUNT;
		}

		[[nodiscard]] LogSequenceNumber nextCheckpointLsn(const Superblock &published) {
			if (published.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "relocatable Vector Codebook reclamation cannot advance the checkpoint LSN");
			}
			return LogSequenceNumber(published.checkpoint_lsn.value() + 1U);
		}

	} // namespace

	VectorCodebookReachabilityReclaimer::VectorCodebookReachabilityReclaimer(PhysicalSegmentAllocator &segments,
																			 VectorManifestStore &manifests,
																			 RelocatableMetadataStore &metadata) :
		segments_(segments), manifests_(manifests), metadata_(metadata) {
		if (!segments_.usesMetadataStore(metadata_) || !manifests_.usesMetadataStore(metadata_)) {
			throw std::invalid_argument(
					"relocatable Vector Codebook reclaimer requires stores attached to one metadata store");
		}
	}

	VectorCodebookReachabilityReclaimReport
	VectorCodebookReachabilityReclaimer::reclaim(const VectorCodebookReachabilityReclaimOptions &options,
												 LogSequenceNumber checkpointLsn,
												 const MetadataPublishOptions &publishOptions) {
		validateOptions(options, checkpointLsn);
		VectorCodebookReachabilityReclaimReport report;
		std::vector<ExtentRetirement> retirementPlan;
		std::vector<CatalogEntry> retiredCodebooks;
		uint64_t plannedCodebookRetirements = 0U;
		Superblock scanned;

		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		try {
			scanned = snapshot.superblock();
			report.scanned_generation = scanned.generation;
			std::set<uint64_t> relocatedSourceSegments;
			metadata_.visitRetireQueueEntries(snapshot, [&](const RetireQueueEntry &entry) {
				if (report.retire_queue_entries == options.maximum_retire_queue_entries) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook reclamation exceeded its Retire Queue scan bound");
				}
				++report.retire_queue_entries;
				if (entry.disposition == RetireQueueDisposition::RQD_RELOCATED_SOURCE &&
					!relocatedSourceSegments.insert(entry.segment_id.value()).second) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook reclamation found duplicate pending relocation sources");
				}
			});
			std::map<uint64_t, Generation> reachableCodebooks;
			metadata_.visitVectorManifestLocations(snapshot, [&](const VectorManifestLocation &location) {
				if (report.vector_manifests == options.maximum_vector_manifests) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook reclamation exceeded its Manifest scan bound");
				}
				++report.vector_manifests;
				const VectorManifest manifest = manifests_.read(snapshot, location.index_id).manifest();
				if (manifest.codebook_id == 0U) {
					return;
				}
				const auto [iterator, inserted] =
						reachableCodebooks.emplace(manifest.codebook_id, manifest.codebook_generation);
				if (!inserted && iterator->second != manifest.codebook_generation) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook reclamation found conflicting Manifest Codebook generations");
				}
			});

			struct ScannedCodebookExtent {
				ExtentLocation location;
				VectorCodebookExtentHeader header;
				bool has_relocated_source = false;
			};
			std::map<uint64_t, std::vector<ScannedCodebookExtent>> scannedCodebooks;
			metadata_.visitExtentLocations(snapshot, [&](const ExtentLocation &location) {
				if (report.extent_locations == options.maximum_extent_locations) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook reclamation exceeded its Extent Directory scan bound");
				}
				++report.extent_locations;
				const PhysicalSegmentContents contents = segments_.read(snapshot, location.segment_id);
				if (contents.entry.kind != SegmentKind::SGK_CODEBOOK_EXTENT) {
					return;
				}
				const VectorCodebookExtentHeader header =
						VectorCodebookExtentHeaderCodec::decode(contents.header_extension);
				VectorCodebookExtentHeaderCodec::validateBlock(header, contents.payload);
				const uint8_t invalid = static_cast<uint8_t>(contents.entry.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(contents.entry.segment_id != location.segment_id) |
										static_cast<uint8_t>(contents.entry.owner_id != header.codebook_id) |
										static_cast<uint8_t>(contents.entry.generation != location.generation) |
										static_cast<uint8_t>(header.extent_id != location.extent_id) |
										static_cast<uint8_t>(!isFullSegmentExtent(location)) |
										static_cast<uint8_t>(!metadata_.isPhysicalSlotAllocated(
												snapshot, contents.entry.physical_slot));
				if (invalid != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook reclamation found a malformed Codebook extent");
				}
				scannedCodebooks[header.codebook_id].push_back(
						{location, header, relocatedSourceSegments.contains(location.segment_id.value())});
			});

			VectorCodebookStore codebooks(segments_, metadata_);
			for (auto &[codebookId, extents]: scannedCodebooks) {
				const auto isCanonicalManifest = [codebookId](const ScannedCodebookExtent &extent) {
					return extent.header.kind == VectorCodebookExtentKind::VCEK_MANIFEST &&
						   extent.header.extent_id ==
									   VectorCodebookExtentIdCodec::manifestExtentId(codebookId);
				};
				const auto root = std::ranges::find_if(extents, isCanonicalManifest);
				if (root == extents.end()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook reclamation found data extents without one canonical manifest");
				}
				const VectorCodebookManifest immutableManifest = codebooks.manifest(snapshot, codebookId);
				codebooks.validate(snapshot, codebookId, immutableManifest.header.payload_bytes,
								   immutableManifest.entries.size());
				if (extents.size() != immutableManifest.entries.size() + 1U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook reclamation found an incomplete immutable extent set");
				}
				// `validate()` proves every Manifest-listed extent is present and valid.
				// Equal cardinality then excludes an additional unowned extent.
				const auto reachable = reachableCodebooks.find(codebookId);
				if (reachable != reachableCodebooks.end()) {
					if (reachable->second != immutableManifest.header.generation) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Vector Codebook reclamation found a Manifest pointing at a replaced Codebook");
					}
					++report.reachable_codebooks;
					report.reachable_codebook_extents += extents.size();
					continue;
				}
				if (std::ranges::any_of(
							extents, [](const ScannedCodebookExtent &extent) { return extent.has_relocated_source; })) {
					// A relocation source remains readable for an older snapshot. The
					// whole immutable Codebook remains reachable until every source drains.
					++report.relocated_codebooks_retained;
					continue;
				}
				if (extents.size() > options.maximum_retirements - retirementPlan.size()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook reclamation exceeded its extent-retirement bound");
				}
				for (const ScannedCodebookExtent &extent: extents) {
					retirementPlan.push_back({extent.location, codebookId});
				}
				++plannedCodebookRetirements;
			}
			for (const auto &[codebookId, generation]: reachableCodebooks) {
				static_cast<void>(generation);
				if (!scannedCodebooks.contains(codebookId)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook reclamation found a Manifest reference to an absent Codebook extent");
				}
			}
			metadata_.visitCatalogEntries(snapshot, [&](const CatalogEntry &entry) {
				if (report.catalog_entries == options.maximum_catalog_entries) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable Vector Codebook reclamation exceeded its Catalog scan bound");
				}
				++report.catalog_entries;
				if (entry.kind == SegmentKind::SGK_CODEBOOK_EXTENT && entry.state == SegmentState::SGS_RETIRED) {
					if (!metadata_.isPhysicalSlotAllocated(snapshot, entry.physical_slot)) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable Vector Codebook reclamation found an unallocated retired Codebook slot");
					}
					retiredCodebooks.push_back(entry);
				}
			});
			snapshot.release();
		} catch (...) {
			snapshot.release();
			throw;
		}

		std::ranges::sort(retirementPlan, {},
						  [](const ExtentRetirement &retirement) { return retirement.location.extent_id.value(); });
		std::ranges::sort(retiredCodebooks, {}, &CatalogEntry::segment_id);
		Superblock published = scanned;
		bool hasPublished = false;
		if (!retirementPlan.empty()) {
			published = metadata_.retireVectorCodebookExtents(report.scanned_generation, retirementPlan, checkpointLsn,
															  publishOptions);
			hasPublished = true;
			report.codebooks_retired = plannedCodebookRetirements;
			report.codebook_extents_retired = retirementPlan.size();
			for (const ExtentRetirement &retirement: retirementPlan) {
				CatalogEntry retired;
				retired.segment_id = retirement.location.segment_id;
				retired.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
				retired.owner_id = retirement.expected_owner_id;
				retired.generation = retirement.location.generation;
				retired.state = SegmentState::SGS_RETIRED;
				retiredCodebooks.push_back(std::move(retired));
			}
		}
		if (metadata_.activeSnapshotCount() == 0U) {
			std::ranges::sort(retiredCodebooks, {}, &CatalogEntry::segment_id);
			for (const CatalogEntry &codebook: retiredCodebooks) {
				const LogSequenceNumber publicationLsn = hasPublished ? nextCheckpointLsn(published) : checkpointLsn;
				published = metadata_.releaseRetiredCatalogSegment(codebook.segment_id, codebook.generation,
																   publicationLsn, publishOptions);
				hasPublished = true;
				++report.codebook_extents_released;
			}
		}
		report.published_generation = hasPublished ? published.generation : report.scanned_generation;
		return report;
	}

} // namespace graph::storage
