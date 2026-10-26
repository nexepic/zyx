#include "graph/storage/relocatable/entity/ScanMembershipPublication.hpp"
/**
 * @file RelocatableMetadataStoreGraphMutation.cpp
 * @brief Atomic native graph metadata publication across typed page stores.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/EntityFreeSpaceIndex.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPoolRoutes.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyConversionPreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyPoolPlacement.hpp"
#include "graph/storage/relocatable/metadata/NativeGraphDeferredIndexImages.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyImmutablePreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyContentIndexPreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyDirectoryPreparation.hpp"
#include "graph/storage/relocatable/property/HybridPropertyReadContext.hpp"
#include "graph/storage/relocatable/metadata/StateRecordStore.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueMaintenanceJournal.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/NativeVectorMutationPlanner.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/HybridPropertyLargeFieldPreparation.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/CancellationCheckpoint.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphEntityPageMaterializer.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphFinalPublication.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValuePlanner.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPageRanges.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPropertyPageMaterializer.hpp"
#include "graph/storage/relocatable/property/LargeValue.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/utils/Sha256.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] bool isEntityPoolPageOwnerKind(PageOwnerKind ownerKind) {
			return ownerKind == PageOwnerKind::POK_ENTITY || ownerKind == PageOwnerKind::POK_ENTITY_LABEL;
		}

		[[nodiscard]] bool hasEntityMapDelta(const EntityPageReplacementRequest &request) {
			return !request.inserted_nodes.empty() || !request.erased_node_ids.empty() ||
				   !request.inserted_edges.empty() || !request.erased_edge_ids.empty();
		}

		[[nodiscard]] bool hasSameNewEntityPoolIdentity(const EntityPageNewPoolBatchRequest &left,
																		const EntityPageNewPoolBatchRequest &right) {
			const CatalogSlotReservationRequest &leftReservation = left.reservation;
			const CatalogSlotReservationRequest &rightReservation = right.reservation;
			const uint8_t same = static_cast<uint8_t>(leftReservation.segment_id == rightReservation.segment_id) &
								 static_cast<uint8_t>(leftReservation.kind == rightReservation.kind) &
								 static_cast<uint8_t>(leftReservation.owner_id == rightReservation.owner_id) &
								 static_cast<uint8_t>(leftReservation.segment_generation ==
														  rightReservation.segment_generation) &
								 static_cast<uint8_t>(leftReservation.state == rightReservation.state) &
								 static_cast<uint8_t>(leftReservation.require_absent ==
														  rightReservation.require_absent) &
								 static_cast<uint8_t>(left.first_slot == right.first_slot) &
								 static_cast<uint8_t>(left.last_slot == right.last_slot);
			return same != 0U;
		}

		[[nodiscard]] bool hasSameFreshEntityPoolIdentity(const EntityPageFreshPoolDestination &left,
														  const EntityPageFreshPoolDestination &right) {
			const uint8_t same = static_cast<uint8_t>(left.segment_id == right.segment_id) &
								 static_cast<uint8_t>(left.pool_owner_id == right.pool_owner_id) &
								 static_cast<uint8_t>(left.generation == right.generation) &
								 static_cast<uint8_t>(left.first_slot == right.first_slot) &
								 static_cast<uint8_t>(left.last_slot == right.last_slot);
			return same != 0U;
		}

		[[nodiscard]] bool hasSameFreshEntityPoolIdentity(const EntityPageFreshPoolDestination &destination,
														  const EntityPageNewPoolBatchRequest &request) {
			const CatalogSlotReservationRequest &reservation = request.reservation;
			const uint8_t same = static_cast<uint8_t>(destination.segment_id == reservation.segment_id) &
								 static_cast<uint8_t>(destination.pool_owner_id == reservation.owner_id) &
								 static_cast<uint8_t>(destination.generation == reservation.segment_generation) &
								 static_cast<uint8_t>(destination.first_slot == request.first_slot) &
								 static_cast<uint8_t>(destination.last_slot == request.last_slot) &
								 static_cast<uint8_t>(reservation.kind == SegmentKind::SGK_ENTITY_PAGE_POOL) &
								 static_cast<uint8_t>(reservation.state == SegmentState::SGS_SEALED) &
								 static_cast<uint8_t>(reservation.require_absent);
			return same != 0U;
		}

		[[nodiscard]] bool hasSameCatalogEntry(const CatalogEntry &left, const CatalogEntry &right) {
			const uint8_t same = static_cast<uint8_t>(left.segment_id == right.segment_id) &
								 static_cast<uint8_t>(left.physical_slot == right.physical_slot) &
								 static_cast<uint8_t>(left.kind == right.kind) &
								 static_cast<uint8_t>(left.owner_id == right.owner_id) &
								 static_cast<uint8_t>(left.generation == right.generation) &
								 static_cast<uint8_t>(left.state == right.state) &
								 static_cast<uint8_t>(left.committed_lsn == right.committed_lsn);
			return same != 0U;
		}

		void validateStreamedEdgeLeaseRanges(const NativeGraphLogicalIdReservation &reservation,
														 uint64_t firstEdgeId, uint64_t lastEdgeId) {
			if (!reservation.node_ranges.empty() || reservation.edge_ranges.empty()) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "streamed Edge publication requires ordered Edge-only lease ranges");
			}
			uint64_t previousLast = 0U;
			bool ownsFirst = false;
			bool ownsLast = false;
			for (const NativeGraphLogicalIdLeaseRange &range: reservation.edge_ranges) {
				if (range.first_id == 0U || range.count == 0U ||
					range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "streamed Edge publication has an invalid durable ID lease range");
				}
				const uint64_t rangeLast = range.first_id + range.count - 1U;
				if ((previousLast != 0U && range.first_id <= previousLast) || range.first_id > lastEdgeId) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "streamed Edge publication has unordered or unrelated ID lease ranges");
				}
				ownsFirst |= range.contains(firstEdgeId);
				ownsLast |= range.contains(lastEdgeId);
				previousLast = rangeLast;
			}
			if (!ownsFirst || !ownsLast) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "streamed Edge publication does not own its source ID bounds");
			}
		}

		[[nodiscard]] bool streamedEdgeIdIsLeased(const NativeGraphLogicalIdReservation &reservation, uint64_t edgeId) {
			// The publication boundary validates that lease ranges are sorted and
			// non-overlapping. Find the only range that can contain this ID instead
			// of rescanning every bounded input batch for every materialized Edge.
			const auto next = std::upper_bound(
					reservation.edge_ranges.begin(), reservation.edge_ranges.end(), edgeId,
					[](uint64_t id, const NativeGraphLogicalIdLeaseRange &range) { return id < range.first_id; });
			return next != reservation.edge_ranges.begin() && std::prev(next)->contains(edgeId); // this private helper is called only for records already authenticated by NativeBulkEdgeMaterializer against this lease.
		}

		struct WriterScopedPrivateScratch {
			CatalogEntry entry;
			uint16_t header_bytes = RSS_ENVELOPE_BYTES;
			size_t payload_bytes = 0U;
		};

		// Compact bulk candidates retain contiguous logical identities, while their
		// physical slots are chosen from allocator-visible free space. Private
		// scratch must never reserve an unmanaged append-only file suffix: a failed
		// publication must leave those slots immediately reusable by the allocator.
		struct WriterScopedCompactPrivateScratch {
			CatalogEntry first_entry;
			// Logical candidate identities are reserved up front, while physical
			// slots exist only after their private payload is first materialized.
			std::map<size_t, CatalogEntry> materialized_entries;
			PhysicalSlotId first_slot;
			PhysicalSlotId last_slot;
			size_t count = 0U;
			uint16_t header_bytes = RSS_ENVELOPE_BYTES;
			std::vector<std::byte> header_extension;
		};

		struct WriterScopedPrivateScratchState {
			Superblock superblock;
			std::vector<WriterScopedPrivateScratch> candidates;
			std::optional<WriterScopedCompactPrivateScratch> compact_candidate;
			bool active = true;
			bool candidate_payloads_rewritten = false;
		};

		constexpr uint32_t PRIVATE_SCRATCH_RESERVATION_MAGIC = 0x5352595AU; // Little-endian bytes: ZYRS
		constexpr uint16_t PRIVATE_SCRATCH_RESERVATION_VERSION = 1U;
		constexpr size_t PRIVATE_SCRATCH_RESERVATION_BYTES = 16U;

		[[nodiscard]] bool isPrivateScratchReservation(std::span<const std::byte> payload) {
			return payload.size() == PRIVATE_SCRATCH_RESERVATION_BYTES &&
				   LittleEndianCodec::readU32(payload, 0U) == PRIVATE_SCRATCH_RESERVATION_MAGIC &&
				   LittleEndianCodec::readU16(payload, sizeof(uint32_t)) == PRIVATE_SCRATCH_RESERVATION_VERSION &&
				   LittleEndianCodec::readU16(payload, sizeof(uint32_t) + sizeof(uint16_t)) == 0U &&
				   LittleEndianCodec::readU64(payload, sizeof(uint32_t) + sizeof(uint16_t) * 2U) == 0U;
		}

	} // namespace

	NativeGraphPrivateSegmentSet::NativeGraphPrivateSegmentSet(SegmentId firstSegmentId, Generation generation,
															   size_t count, uint64_t ownerId) :
		first_segment_id_(firstSegmentId), generation_(generation), count_(count), owner_id_(ownerId) {
		if (!first_segment_id_.isValid() || !generation_.isValid() || count_ == 0U || owner_id_ == 0U ||
			count_ - 1U > std::numeric_limits<uint64_t>::max() - first_segment_id_.value()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph private candidate set has an invalid contiguous identity range");
		}
	}

	std::vector<std::byte> NativeGraphPrivateSegmentReservation::payload() {
		std::vector<std::byte> result(PRIVATE_SCRATCH_RESERVATION_BYTES);
		LittleEndianCodec::writeU32(result, 0U, PRIVATE_SCRATCH_RESERVATION_MAGIC);
		LittleEndianCodec::writeU16(result, sizeof(uint32_t), PRIVATE_SCRATCH_RESERVATION_VERSION);
		LittleEndianCodec::writeU16(result, sizeof(uint32_t) + sizeof(uint16_t), 0U);
		LittleEndianCodec::writeU64(result, sizeof(uint32_t) + sizeof(uint16_t) * 2U, 0U);
		return result;
	}

	NativeGraphPrivateSegmentMaterialization NativeGraphPrivateSegmentSet::at(size_t ordinal) const {
		if (ordinal >= count_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph private candidate ordinal is outside its writer-scoped set");
		}
		return {SegmentId(first_segment_id_.value() + ordinal), generation_, owner_id_};
	}

	size_t NativeGraphPrivateSegmentSet::ordinalOf(const NativeGraphPrivateSegmentMaterialization &identity) const {
		if (!identity.isValid() || identity.generation != generation_ || identity.owner_id != owner_id_ ||
			identity.segment_id.value() < first_segment_id_.value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph private candidate identity does not belong to this writer-scoped set");
		}
		const uint64_t ordinal = identity.segment_id.value() - first_segment_id_.value();
		if (ordinal >= count_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph private candidate identity is outside its writer-scoped range");
		}
		return static_cast<size_t>(ordinal);
	}

	Superblock RelocatableMetadataStore::mutateNativeGraphPagesWithLargeValues(
			std::span<const EntityPageReplacementRequest> requests,
			std::span<const EntityPageAppendBatchRequest> appendRequests,
			std::span<const EntityPageNewPoolBatchRequest> newPoolRequests,
			std::span<const NativeGraphPropertyPageReplacementRequest> propertyReplacements,
			std::span<const NativeGraphPropertyPageBatchRequest> propertyPageBatchRequests,
			std::span<const NativeGraphLargeValueMutation> largeValues,
			const NativeGraphDerivedStateMutation &derivedState, LogSequenceNumber checkpointLsn,
			const NativeGraphPageTransactionWithLargeValuesPreparer &preparer,
			const NativeGraphDerivedStatePreparer &derivedStatePreparer,
			const NativeGraphVectorBootstrapPreparer &vectorBootstrapPreparer,
			const NativeGraphVectorDeltaPreparer &vectorDeltaPreparer,
			std::span<const PageRetirement> retiredLargeValuePages, std::span<const LargeValueId> erasedLargeValueIds,
			const ExistingPoolPagesRetirementPreparer &largeValueRetirementPreparer,
			const MetadataPublishOptions &options, NativeGraphLogicalIdReservation logicalIdReservation,
			std::optional<Generation> expectedMetadataGeneration,
			const NativeGraphPrivateSegmentImagePreparer &privateSegmentImagePreparer,
			const NativeGraphPrivateSegmentPreparer &privateSegmentPreparer,
			const NativeGraphDeferredIndexPlanner &deferredIndexPlanner,
			const NativeGraphEarlyPublicationPlanner &earlyPublicationPlanner,
			const NativeGraphDeferredIndexPlansPlanner &deferredIndexPlansPlanner,
            const std::optional<NativeGraphHybridPropertyRootMutation> &hybridPropertyRootMutation,
            const std::optional<NativeLargeValueReclamationEvidence> &reclamationEvidence,
            const NativeHybridVectorPublication *hybridVectors,
            const NativeHybridTopologyPublication *hybridTopology) {
		debug::ScopedPerfTimer timer("native_graph.metadata_publish");
        if (hybridTopology && (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED || hybridVectors || reclamationEvidence || hybridPropertyRootMutation ||
            hybridTopology->metadata_ != this || hybridTopology->derived_ != &derivedState ||
            hybridTopology->field_changes_.size() > 64U || hybridTopology->owner_deletions_.size() > 64U || hybridTopology->new_property_tokens_.size() > 64U ||
            !std::ranges::is_sorted(hybridTopology->new_property_tokens_) ||
            std::ranges::adjacent_find(hybridTopology->new_property_tokens_) != hybridTopology->new_property_tokens_.end() ||
            (!hybridTopology->new_property_tokens_.empty() && hybridTopology->field_changes_.empty()) ||
            (!hybridTopology->field_changes_.empty() && !hybridTopology->fields_memory_) || expectedMetadataGeneration != std::optional(hybridTopology->source_.generation)))
            throw FormatException(FormatError::FERR_INVALID_STATE, "native topology preservation requires its same-domain source transaction");
        if (hybridTopology && hybridTopology->schema_only_ && (!requests.empty() || !appendRequests.empty() || !newPoolRequests.empty() ||
            !propertyReplacements.empty() || !propertyPageBatchRequests.empty() || !largeValues.empty() ||
            !logicalIdReservation.empty() || !retiredLargeValuePages.empty() || !erasedLargeValueIds.empty() ||
            !hybridTopology->property_owners_.empty() || !hybridTopology->field_changes_.empty() || !hybridTopology->owner_deletions_.empty() ||
            !derivedState.vector_deltas.empty()))
            throw FormatException(FormatError::FERR_INVALID_STATE, "native schema publication cannot contain graph or field mutations");
        std::optional<utils::OwnedAllocationScope> hybridAllocation;
        std::optional<utils::OwnedHeapReservation> hybridStoreCredit;
        bool deferredBuildingAdmission = false;
        if (hybridVectors) {
            const auto &fields = hybridVectors->fields_;
            const auto memory = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(fields.memory);
            constexpr size_t limit = 1024U * 1024U;
            if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED || hybridVectors->metadata_ != this || hybridVectors->derived_ != &derivedState ||
                (!hybridVectors->catalog_tokens_ && hybridVectors->semantics_.empty()) || hybridVectors->semantics_.size() > 128 || !memory ||
                hybridVectors->catalog_tokens_ != !hybridVectors->new_property_tokens_.empty() || hybridVectors->new_property_tokens_.size() > 64U ||
                !std::ranges::is_sorted(hybridVectors->new_property_tokens_) ||
                std::ranges::adjacent_find(hybridVectors->new_property_tokens_) != hybridVectors->new_property_tokens_.end() ||
                memory->used() > limit || memory->available() > limit - memory->used() ||
                (fields.replacements.empty() == fields.owner_deletions.empty()) || fields.replacements.size() > 64 || fields.owner_deletions.size() > 64 ||
                !fields.first_slot.isValid() || fields.last_slot < fields.first_slot || !fields.field_pool_owner_id ||
                !requests.empty() || !appendRequests.empty() || !newPoolRequests.empty() ||
                !propertyReplacements.empty() || !propertyPageBatchRequests.empty() || !largeValues.empty() ||
                hybridPropertyRootMutation || reclamationEvidence || !logicalIdReservation.empty() ||
                !retiredLargeValuePages.empty() || !erasedLargeValueIds.empty() || largeValueRetirementPreparer ||
                privateSegmentImagePreparer || privateSegmentPreparer || deferredIndexPlanner || earlyPublicationPlanner || deferredIndexPlansPlanner ||
                !derivedState.reused_large_values.empty() || !derivedState.private_segments.empty() ||
                derivedState.private_segment_plan || derivedState.external_private_tail_reservation ||
                (!hybridVectors->catalog_tokens_ && derivedState.vector_bootstraps.empty() && derivedState.vector_deltas.empty()) ||
                expectedMetadataGeneration != std::optional(fields.expected_generation))
                throw FormatException(FormatError::FERR_INVALID_STATE, "native fields/Vector publication requires its sealed transaction and exact bounded work");
            if (hybridVectors->catalog_tokens_ && (!fields.owner_deletions.empty() ||
                std::ranges::count(derivedState.index_tree_mutations, RSS_SYSTEM_CATALOG_INDEX_ID,
                    &NativeGraphIndexTreeMutation::index_id) != 1))
                throw FormatException(FormatError::FERR_INVALID_STATE, "native dictionary proof requires one Catalog transition and fields");
            for (const auto &tree : derivedState.index_tree_mutations) {
                const bool owned = (hybridVectors->catalog_tokens_ && tree.index_id == RSS_SYSTEM_CATALOG_INDEX_ID &&
                    tree.expected_root.has_value() && !tree.retire_root) || std::ranges::any_of(derivedState.vector_bootstraps, [&](const auto &v) {
                    return tree.index_id == v.index_id || tree.index_id == v.vector_node_mapping_index_id;
                }) || std::ranges::any_of(derivedState.vector_deltas, [&](const auto &v) {
                    return tree.index_id == v.index_id || tree.index_id == v.vector_node_mapping_index_id ||
                        tree.index_id == v.delta_mapping_index_id || tree.index_id == v.expected_delta_mapping_index_id;
                });
                if (!owned) throw FormatException(FormatError::FERR_INVALID_STATE, "native Vector proof cannot authorize unrelated derived roots");
            }
            hybridAllocation.emplace(fields.memory); hybridStoreCredit.emplace(fields.memory, 32768U);
        }
        if (hybridPropertyRootMutation) {
            const auto &request = *hybridPropertyRootMutation;
            if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED || !request.expected_database_id.isValid() ||
                (request.expected_root != PhysicalPageRef{} && !request.expected_root.isValid()) ||
                (request.mode != NativeHybridPropertyRootMode::NHPRM_REPLACE &&
                    request.mode != NativeHybridPropertyRootMode::NHPRM_CLEAR) ||
                (request.mode == NativeHybridPropertyRootMode::NHPRM_CLEAR && !request.expected_root.isValid()))
                throw FormatException(FormatError::FERR_INVALID_HEADER, "native hybrid root request is disabled or invalid");
            for (size_t i = 0; i < request.replacement_index_ids.size(); ++i) {
                const auto id = request.replacement_index_ids[i];
                if (id && request.mode == NativeHybridPropertyRootMode::NHPRM_CLEAR)
                    throw FormatException(FormatError::FERR_INVALID_HEADER, "native hybrid root clear names replacement trees");
                if (id && id != hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(i)))
                    throw FormatException(FormatError::FERR_INVALID_HEADER, "native hybrid root request names the wrong tree namespace");
                for (size_t j = 0; id && j < i; ++j)
                    if (request.replacement_index_ids[j] == id)
                        throw FormatException(FormatError::FERR_INVALID_HEADER, "native hybrid root request aliases namespaces");
            }
            if (request.conversion) {
                const auto &conversion = *request.conversion;
                if (conversion.cold_row_remainder && conversion.publish_candidate && !conversion.allocation_admission &&
                    !RSS_SCAN_PATH_FORMAT_ENABLED)
                    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                        "native cold ROW publication requires whole-database allocation admission");
                deferredBuildingAdmission = conversion.publish_candidate && !conversion.allocation_admission &&
                    (conversion.cold_row_remainder || conversion.group_count > 1U);
                if (conversion.cold_row_remainder && (!conversion.compact_column_memberships ||
                    conversion.direction != NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID))
                    throw FormatException(FormatError::FERR_INVALID_HEADER,
                        "native cold ROW conversion requires compact forward groups");
                const auto memory = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(conversion.memory);
                constexpr size_t maximumConversionBytes = 1024U * 1024U;
                if (!conversion.group_count || conversion.group_count > 8U ||
                    conversion.group_id > RSS_HYBRID_MAX_GROUP_ID - (conversion.group_count - 1U) ||
                    (conversion.group_count > 1U && (!conversion.compact_column_memberships ||
                        (!conversion.allocation_admission && !RSS_SCAN_PATH_FORMAT_ENABLED) ||
                        !conversion.publish_candidate || conversion.direction != NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID)))
                    throw FormatException(FormatError::FERR_INVALID_HEADER, "native hybrid cohort requires1..8groups and explicit gated compact forward publication");
                if (conversion.allocation_admission && (!conversion.publish_candidate ||
                    conversion.direction != NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID ||
                    !conversion.allocation_admission->maximum_admitted_peak_bytes || !conversion.allocation_admission->maximum_admitted_file_bytes ||
                    (conversion.allocation_admission->census.cancellation && conversion.allocation_admission->census.cancellation != conversion.cancellation)))
                    throw FormatException(FormatError::FERR_INVALID_HEADER, "native allocation admission requires explicit forward publication, peak limits and its conversion checkpoint");
                if (request.mode != NativeHybridPropertyRootMode::NHPRM_REPLACE ||
                    !expectedMetadataGeneration.has_value() || !hybrid_property_detail::validKind(conversion.owner_kind) ||
                    (conversion.direction != NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID &&
                        conversion.direction != NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW) ||
                    !conversion.group_id || conversion.group_id > RSS_HYBRID_MAX_GROUP_ID || !memory ||
                    !conversion.first_slot.isValid() || !conversion.last_slot.isValid() ||
                    conversion.first_slot > conversion.last_slot || !conversion.field_pool_owner_id ||
                    memory->used() > maximumConversionBytes || memory->available() > maximumConversionBytes - memory->used() ||
                    std::ranges::any_of(request.replacement_index_ids, [](uint64_t id) { return id != 0; }) ||
                    !requests.empty() || !appendRequests.empty() || !newPoolRequests.empty() ||
                    !propertyReplacements.empty() || !propertyPageBatchRequests.empty() || !largeValues.empty() ||
                    !retiredLargeValuePages.empty() || !erasedLargeValueIds.empty() || !derivedState.empty() ||
                    !logicalIdReservation.empty())
                    throw FormatException(FormatError::FERR_INVALID_HEADER,
                        "native hybrid conversion requires one fenced group and a bounded conversion account");
            }
        }
		const bool hasLargeValueRetirements = !retiredLargeValuePages.empty() || !erasedLargeValueIds.empty();
		if (!preparer ||
			(requests.empty() && appendRequests.empty() && newPoolRequests.empty() && propertyReplacements.empty() &&
			 propertyPageBatchRequests.empty() && largeValues.empty() && !hasLargeValueRetirements &&
			 derivedState.empty() && logicalIdReservation.empty() && !hybridPropertyRootMutation) ||
			!checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph page transaction requires work, a checkpoint LSN, and a preparer");
		}
		if (hasLargeValueRetirements &&
			(retiredLargeValuePages.empty() || erasedLargeValueIds.empty() || !largeValueRetirementPreparer)) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"native graph LargeValue retirement requires pages, manifest IDs, and a typed preparer");
		}
		uint64_t previousErasedLargeValueId = 0U;
		for (const LargeValueId valueId: erasedLargeValueIds) {
			if (!valueId.isValid() || valueId.value() <= previousErasedLargeValueId) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph LargeValue retirement has unordered Manifest identities");
			}
			previousErasedLargeValueId = valueId.value();
		}
		uint64_t previousRetiredLargeValuePageId = 0U;
		for (const PageRetirement &retirement: retiredLargeValuePages) {
			const PageLocation &location = retirement.location;
			const bool validOwner = retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_DATA ||
									retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
			const uint8_t invalid =
					static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.page_id.value() <= previousRetiredLargeValuePageId) |
					static_cast<uint8_t>(!location.segment_id.isValid()) |
					static_cast<uint8_t>(!location.generation.isValid()) |
					static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(retirement.expected_owner_id == 0U) | static_cast<uint8_t>(!validOwner) |
					static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(
							!std::ranges::binary_search(erasedLargeValueIds, retirement.expected_owner_id, {},
														[](LargeValueId valueId) { return valueId.value(); }));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph LargeValue retirement has an invalid or unrelated Page frame");
			}
			previousRetiredLargeValuePageId = location.page_id.value();
		}
        const bool jointNativeContent = hybridTopology && !hybridTopology->field_changes_.empty() && !largeValues.empty();
        if (jointNativeContent && (largeValues.size() > 64U || hasLargeValueRetirements ||
            std::ranges::any_of(derivedState.index_tree_mutations, [](const auto &mutation) {
                return mutation.index_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID;
            })))
            throw FormatException(FormatError::FERR_INVALID_STATE, "joint native content requires one bounded metadata-owned index plan");
		if ((!largeValues.empty() || hasLargeValueRetirements) && !jointNativeContent) {
			// LargeValue creation and retirement both alter the durable content
			// identity namespace. Publishing either without its index mutation would
			// create an orphan Manifest or a dangling content-index key.
			const auto contentIndexMutation =
					std::ranges::find(derivedState.index_tree_mutations, RSS_LARGE_VALUE_CONTENT_INDEX_ID,
									  &NativeGraphIndexTreeMutation::index_id);
			if (contentIndexMutation == derivedState.index_tree_mutations.end() || contentIndexMutation->retire_root ||
				(hasLargeValueRetirements && !contentIndexMutation->expected_root.has_value())) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph LargeValue publication requires a coherent content-index mutation");
			}
		}
		if (!derivedState.empty() && !derivedStatePreparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph derived-state publication requires a typed page preparer");
		}
		const size_t privateSegmentCount = derivedState.private_segment_plan.has_value()
												   ? derivedState.private_segment_plan->segment_count
												   : derivedState.private_segments.size();
		if (derivedState.private_segment_plan.has_value() && !derivedState.private_segments.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph private scratch request and compact plan are mutually exclusive");
		}
		if (derivedState.external_private_tail_reservation.has_value() && privateSegmentCount == 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"native graph external private tail reservation requires candidate-private materialization");
		}
		if (privateSegmentPreparer && privateSegmentCount == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph private scratch preparer requires candidate scratch segments");
		}
		if (deferredIndexPlanner && privateSegmentCount == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph deferred Index planner requires candidate scratch segments");
		}
		if (deferredIndexPlansPlanner && privateSegmentCount == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph deferred Index planners require candidate scratch segments");
		}
		if (earlyPublicationPlanner && privateSegmentCount == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph early publication planner requires candidate scratch segments");
		}
		if (privateSegmentImagePreparer && privateSegmentCount == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph private scratch image preparer requires candidate scratch segments");
		}
		const auto validatePrivateSegmentRequest = [&privateSegmentImagePreparer](
														   const NativeGraphPrivateSegmentRequest &request) {
			const uint8_t invalid =
					static_cast<uint8_t>(request.kind != SegmentKind::SGK_BULK_SCRATCH) |
					static_cast<uint8_t>(request.owner_id == 0U) | static_cast<uint8_t>(!request.first_slot.isValid()) |
					static_cast<uint8_t>(!request.last_slot.isValid()) |
					static_cast<uint8_t>(request.first_slot > request.last_slot) |
					static_cast<uint8_t>(request.header_bytes < RSS_ENVELOPE_BYTES) |
					static_cast<uint8_t>(request.header_bytes > RSS_MAX_HEADER_BYTES) |
					static_cast<uint8_t>(request.header_bytes % alignof(uint64_t) != 0U) |
					static_cast<uint8_t>(request.header_extension.size() != request.header_bytes - RSS_ENVELOPE_BYTES) |
					static_cast<uint8_t>(!privateSegmentImagePreparer && request.payload.empty()) |
					static_cast<uint8_t>(request.payload.size() > RSS_SEGMENT_SIZE - request.header_bytes);
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph private segment has an invalid kind, owner, placement, or image");
			}
		};
		for (const NativeGraphPrivateSegmentRequest &request: derivedState.private_segments) {
			validatePrivateSegmentRequest(request);
		}
		if (!derivedState.private_segments.empty()) {
			const NativeGraphPrivateSegmentRequest &first = derivedState.private_segments.front();
			for (const NativeGraphPrivateSegmentRequest &request: derivedState.private_segments) {
				if (request.owner_id != first.owner_id ||
					request.header_bytes != first.header_bytes || request.header_extension != first.header_extension) {
					throw FormatException(
							FormatError::FERR_INVALID_HEADER,
							"native graph private scratch candidates require one homogeneous identity contract");
				}
			}
			// A batch must be able to give every candidate a distinct slot before
			// materialization starts; otherwise a failed publish could alias scratch.
			std::vector<const NativeGraphPrivateSegmentRequest *> ordered;
			ordered.reserve(derivedState.private_segments.size());
			for (const NativeGraphPrivateSegmentRequest &request: derivedState.private_segments) {
				ordered.push_back(&request);
			}
			std::ranges::sort(ordered, {}, [](const auto *request) { return request->last_slot; });
			uint64_t nextSlot = 0U;
			for (const NativeGraphPrivateSegmentRequest *request: ordered) {
				const uint64_t candidate = std::max(nextSlot, request->first_slot.value());
				if (candidate > request->last_slot.value()) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph private scratch candidates exceed their bounded physical slots");
				}
				if (candidate == std::numeric_limits<uint64_t>::max()) {
					nextSlot = 0U;
					if (request != ordered.back()) {
						throw FormatException(FormatError::FERR_INVALID_HEADER,
											  "native graph private scratch candidates exceed their bounded physical slots");
					}
				} else {
					nextSlot = candidate + 1U;
				}
			}
		}
		if (derivedState.private_segment_plan.has_value()) {
			const NativeGraphPrivateSegmentPlan &plan = *derivedState.private_segment_plan;
			NativeGraphPrivateSegmentRequest representative;
			representative.kind = plan.kind;
			representative.owner_id = plan.owner_id;
			representative.first_slot = plan.first_slot;
			representative.last_slot = plan.last_slot;
			representative.header_bytes = plan.header_bytes;
			representative.header_extension = plan.header_extension;
			validatePrivateSegmentRequest(representative);
			if (plan.segment_count == 0U) { // private scratch plans are produced only after the segment-count preflight.
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph compact private scratch plan has no candidate segments");
			}
			if (plan.segment_count - 1U > plan.last_slot.value() - plan.first_slot.value()) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"native graph compact private scratch plan exceeds its bounded physical slot range");
			}
		}
		if ((!derivedState.vector_bootstraps.empty() && !vectorBootstrapPreparer) ||
			(derivedState.vector_bootstraps.empty() && vectorBootstrapPreparer)) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph Vector bootstrap publication requires an exact typed preparer");
		}
		if ((!derivedState.vector_deltas.empty() && !vectorDeltaPreparer) ||
			(derivedState.vector_deltas.empty() && vectorDeltaPreparer)) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph Vector delta publication requires an exact typed preparer");
		}
		uint64_t previousReusedLargeValueId = 0U;
		for (const LargeValueRef &reference: derivedState.reused_large_values) {
			if (!reference.value_id.isValid() || reference.value_id.value() <= previousReusedLargeValueId) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph reused LargeValue references must have unique ordered identities");
			}
			static_cast<void>(LargeValueRefCodec::encode(reference));
			previousReusedLargeValueId = reference.value_id.value();
		}
		if (!derivedState.reused_large_values.empty()) {
			// A reused Property reference has no new typed frame to authenticate it
			// during materialization. Verify its complete immutable identity before
			// taking the writer; the writer-side route fence below then rejects a
			// concurrent retirement without extending the publication critical section.
			SharedPagePoolAllocator reusablePages(io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
			MetadataSnapshotPin reuseSnapshot = openSnapshot();
			try {
				for (const LargeValueRef &reference: derivedState.reused_large_values) {
					const LargeValueManifestLocation route =
							locations_.findLargeValueManifest(reuseSnapshot.superblock(), reference.value_id);
					const SharedPagePoolContents contents = reusablePages.read(reuseSnapshot, route.manifest_page_id);
					const LargeValueManifest manifest =
							LargeValueManifestPage::open(contents.owner, contents.payload).manifest();
					if (!manifest.isRoot() || manifest.reference() != reference) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph reused LargeValue reference does not match its current Manifest");
					}
				}
				reuseSnapshot.release();
			} catch (...) {
				reuseSnapshot.release();
				throw;
			}
		}
		uint64_t previousIndexId = 0U;
		std::set<uint64_t> derivedSourcePageIds;
		std::set<uint64_t> derivedRetirementPageIds;
		for (const NativeGraphIndexTreeMutation &mutation: derivedState.index_tree_mutations) {
			const std::optional<IndexRootLocation> &expectedRoot = mutation.expected_root;
			const bool retiresRoot = mutation.retire_root;
			const uint8_t invalidRoot =
					static_cast<uint8_t>(mutation.index_id == 0U) |
					static_cast<uint8_t>(mutation.index_id <= previousIndexId) |
					static_cast<uint8_t>(expectedRoot.has_value() && (expectedRoot->index_id != mutation.index_id ||
																	  !expectedRoot->root_page_id.isValid() ||
																	  !expectedRoot->root_generation.isValid())) |
					static_cast<uint8_t>(!retiresRoot && mutation.pool_requests.empty() &&
										 mutation.new_pool_requests.empty()) |
					static_cast<uint8_t>(mutation.replacement_root.has_value() &&
										 mutation.stable_replacement_root.has_value());
			if (invalidRoot != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph derived-state mutation has an invalid or unordered Index root");
			}
			// A fresh logical root may only use fresh physical pools. This keeps
			// first publication independent of an unrelated current pool and makes
			// a failed creation completely unreachable.
			if (!retiresRoot && !expectedRoot.has_value() &&
				(!mutation.pool_requests.empty() || !mutation.replacement_root.has_value() ||
				 !mutation.replacement_root->newly_reserved || mutation.stable_replacement_root.has_value())) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"native graph derived-state root creation must publish one newly reserved Index page");
			}
			previousIndexId = mutation.index_id;

			std::set<uint64_t> mutationPoolIds;
			for (const IndexTreeMutationPoolRequest &pool: mutation.pool_requests) {
				const uint8_t invalidPool =
						static_cast<uint8_t>(!pool.segment_id.isValid()) |
						static_cast<uint8_t>(pool.expected_replacements.empty() && pool.appended_page_count == 0U &&
											 pool.expected_retirements.empty()) |
						static_cast<uint8_t>(pool.replacement_segment_id.isValid() &&
											 pool.expected_replacements.empty()) |
						static_cast<uint8_t>(pool.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(!mutationPoolIds.insert(pool.segment_id.value()).second);
				if (invalidPool != 0U) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "native graph derived-state mutation has an invalid or duplicate Index pool");
				}
				for (const PageLocation &location: pool.expected_replacements) {
					const uint8_t invalidLocation =
							static_cast<uint8_t>(!location.page_id.isValid()) |
							static_cast<uint8_t>(!derivedSourcePageIds.insert(location.page_id.value()).second) |
							static_cast<uint8_t>(derivedRetirementPageIds.contains(location.page_id.value()));
					if (invalidLocation != 0U) {
						throw FormatException(FormatError::FERR_INVALID_HEADER,
											  "native graph derived-state mutation has an invalid or duplicate Index "
											  "replacement page");
					}
				}
				for (const IndexTreePageRetirement &retirement: pool.expected_retirements) {
					const uint8_t invalidRetirement =
							static_cast<uint8_t>(!retirement.location.page_id.isValid()) |
							static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid()) |
							static_cast<uint8_t>(derivedSourcePageIds.contains(retirement.location.page_id.value())) |
							static_cast<uint8_t>(
									!derivedRetirementPageIds.insert(retirement.location.page_id.value()).second);
					if (invalidRetirement != 0U) {
						throw FormatException(FormatError::FERR_INVALID_HEADER,
											  "native graph derived-state mutation has an invalid or duplicate Index "
											  "retirement page");
					}
				}
			}
			for (const NewIndexTreePoolRequest &pool: mutation.new_pool_requests) {
				const uint8_t invalidPool =
						static_cast<uint8_t>(!pool.first_slot.isValid()) |
						static_cast<uint8_t>(!pool.last_slot.isValid()) |
						static_cast<uint8_t>(pool.first_slot > pool.last_slot) |
						static_cast<uint8_t>(pool.owner_id == 0U) |
						static_cast<uint8_t>(pool.appended_page_count == 0U) |
						static_cast<uint8_t>(pool.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
				if (invalidPool != 0U) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "native graph derived-state mutation has an invalid new Index pool request");
				}
			}
			if (mutation.replacement_root.has_value()) {
				const NativeGraphIndexTreeAppendedPageReference &reference = *mutation.replacement_root;
				const size_t poolCount =
						reference.newly_reserved ? mutation.new_pool_requests.size() : mutation.pool_requests.size();
				const size_t appendedCount = [&]() {
					if (reference.pool_ordinal >= poolCount) {
						return size_t{0U};
					}
					return reference.newly_reserved
								   ? mutation.new_pool_requests[reference.pool_ordinal].appended_page_count
								   : mutation.pool_requests[reference.pool_ordinal].appended_page_count;
				}();
				if (reference.pool_ordinal >= poolCount || reference.appended_page_ordinal >= appendedCount) {
					throw FormatException(
							FormatError::FERR_INVALID_HEADER,
							"native graph derived-state mutation has an invalid appended Index root reference");
				}
			}
			if (mutation.stable_replacement_root.has_value() &&
				(!mutation.stable_replacement_root->isValid() ||
				 derivedRetirementPageIds.contains(mutation.stable_replacement_root->value()))) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph derived-state mutation has an invalid stable Index root reference");
			}
			if (retiresRoot) {
				const bool invalidRetirement = !expectedRoot.has_value() || !mutation.new_pool_requests.empty() ||
											   mutation.replacement_root.has_value() ||
											   mutation.stable_replacement_root.has_value() ||
											   mutation.pool_requests.empty();
				bool rootRetired = false;
				for (const IndexTreeMutationPoolRequest &pool: mutation.pool_requests) {
					if (!pool.expected_replacements.empty() || pool.appended_page_count != 0U) {
						throw FormatException(FormatError::FERR_INVALID_HEADER,
											  "native graph root retirement cannot replace or append Index pages");
					}
					for (const IndexTreePageRetirement &retirement: pool.expected_retirements) {
						rootRetired = rootRetired || (expectedRoot.has_value() &&
													  retirement.location.page_id == expectedRoot->root_page_id &&
													  retirement.location.generation == expectedRoot->root_generation);
					}
				}
				if (invalidRetirement || !rootRetired) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "native graph root retirement does not contain its pinned root page");
				}
			}
		}
		uint64_t previousVectorIndexId = 0U;
		for (const NativeGraphVectorBootstrapMutation &mutation: derivedState.vector_bootstraps) {
			const bool knownMetric = mutation.metric == VectorDistanceMetric::VDM_L2 ||
									 mutation.metric == VectorDistanceMetric::VDM_COSINE ||
									 mutation.metric == VectorDistanceMetric::VDM_INNER_PRODUCT;
			const uint8_t malformed =
					static_cast<uint8_t>(mutation.index_id == 0U) |
					static_cast<uint8_t>(mutation.index_id <= previousVectorIndexId) |
					static_cast<uint8_t>(mutation.vector_node_mapping_index_id == 0U) |
					static_cast<uint8_t>(mutation.vector_node_mapping_index_id == mutation.index_id) |
					static_cast<uint8_t>(mutation.first_vector_id == 0U) |
					static_cast<uint8_t>(mutation.vector_count == 0U) |
					static_cast<uint8_t>(mutation.dimensions == 0U) | static_cast<uint8_t>(!knownMetric) |
					static_cast<uint8_t>(mutation.raw_page_count == 0U) |
					static_cast<uint8_t>(!mutation.first_slot.isValid()) |
					static_cast<uint8_t>(!mutation.last_slot.isValid()) |
					static_cast<uint8_t>(mutation.first_slot > mutation.last_slot) |
					static_cast<uint8_t>(mutation.dimensions > (std::numeric_limits<uint32_t>::max() -
																static_cast<uint32_t>(sizeof(uint64_t))) /
																	   sizeof(uint16_t));
			if (malformed != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"native graph Vector bootstrap has an invalid identity, metric, resource bound, or stride");
			}
			const uint32_t recordBytes =
					static_cast<uint32_t>(sizeof(uint64_t)) + mutation.dimensions * sizeof(uint16_t);
			const uint16_t capacity = VectorPageStore::rawSlotCapacity(recordBytes);
			// rawSlotCapacity fails closed for an unrepresentable stride, so a
			// returned capacity is always usable for the canonical page-count check.
			const uint64_t expectedPages = (mutation.vector_count - 1U) / static_cast<uint64_t>(capacity) + 1U;
			if (expectedPages != mutation.raw_page_count) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph Vector bootstrap has a noncanonical Raw-page cardinality");
			}
			const auto hasFreshRoot = [&derivedState](uint64_t indexId) {
				const auto found = std::ranges::find(derivedState.index_tree_mutations, indexId,
													 &NativeGraphIndexTreeMutation::index_id);
				return found != derivedState.index_tree_mutations.end() && !found->expected_root.has_value() &&
					   !found->retire_root && found->replacement_root.has_value() &&
					   found->replacement_root->newly_reserved;
			};
			if (!hasFreshRoot(mutation.index_id) || !hasFreshRoot(mutation.vector_node_mapping_index_id)) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph Vector bootstrap requires two first-use immutable mapping roots");
			}
			previousVectorIndexId = mutation.index_id;
		}
		std::set<uint64_t> vectorPublicationIndexIds;
		for (const NativeGraphVectorBootstrapMutation &mutation: derivedState.vector_bootstraps) {
			vectorPublicationIndexIds.insert(mutation.index_id);
		}
		previousVectorIndexId = 0U;
		for (const NativeGraphVectorDeltaMutation &mutation: derivedState.vector_deltas) {
			const bool knownMetric = mutation.metric == VectorDistanceMetric::VDM_L2 ||
									 mutation.metric == VectorDistanceMetric::VDM_COSINE ||
									 mutation.metric == VectorDistanceMetric::VDM_INNER_PRODUCT;
			const bool hasInserts = mutation.inserted_vector_count != 0U;
			const bool hasExpectedDelta = mutation.expected_delta_mapping_index_id != 0U;
			const bool publishesDelta = mutation.delta_mapping_index_id != 0U;
			const uint8_t malformed =
					static_cast<uint8_t>(mutation.index_id == 0U) |
					static_cast<uint8_t>(mutation.index_id <= previousVectorIndexId) |
					static_cast<uint8_t>(!vectorPublicationIndexIds.insert(mutation.index_id).second) |
					static_cast<uint8_t>(mutation.vector_node_mapping_index_id == 0U) |
					static_cast<uint8_t>(mutation.vector_node_mapping_index_id == mutation.index_id) |
					static_cast<uint8_t>(hasExpectedDelta != mutation.expected_delta_mapping_root.has_value()) |
					static_cast<uint8_t>(hasExpectedDelta && !VectorDeltaMapping::isMappingIndexId(
																	 mutation.expected_delta_mapping_index_id)) |
					static_cast<uint8_t>(mutation.expected_delta_mapping_root.has_value() &&
										 mutation.expected_delta_mapping_root->index_id !=
												 mutation.expected_delta_mapping_index_id) |
					static_cast<uint8_t>(mutation.expected_delta_mapping_root.has_value() &&
										 (!mutation.expected_delta_mapping_root->root_page_id.isValid() ||
										  !mutation.expected_delta_mapping_root->root_generation.isValid())) |
					static_cast<uint8_t>(publishesDelta != (mutation.delta_entry_count != 0U)) |
					static_cast<uint8_t>(publishesDelta &&
										 !VectorDeltaMapping::isMappingIndexId(mutation.delta_mapping_index_id)) |
					static_cast<uint8_t>(mutation.expected_node_mapping_root.index_id != mutation.index_id) |
					static_cast<uint8_t>(mutation.expected_vector_node_mapping_root.index_id !=
										 mutation.vector_node_mapping_index_id) |
					static_cast<uint8_t>(!mutation.expected_node_mapping_root.root_page_id.isValid()) |
					static_cast<uint8_t>(!mutation.expected_node_mapping_root.root_generation.isValid()) |
					static_cast<uint8_t>(!mutation.expected_vector_node_mapping_root.root_page_id.isValid()) |
					static_cast<uint8_t>(!mutation.expected_vector_node_mapping_root.root_generation.isValid()) |
					static_cast<uint8_t>(!mutation.expected_manifest_pool.segment_id.isValid()) |
					static_cast<uint8_t>(!mutation.expected_manifest_location.page_id.isValid()) |
					static_cast<uint8_t>(!mutation.expected_manifest_location.generation.isValid()) |
					static_cast<uint8_t>(mutation.expected_manifest.index_id != mutation.index_id) |
					static_cast<uint8_t>(mutation.expected_manifest.manifest_page_id !=
										 mutation.expected_manifest_location.page_id) |
					static_cast<uint8_t>(mutation.expected_manifest.generation !=
										 mutation.expected_manifest_location.generation) |
					static_cast<uint8_t>(!mutation.expected_manifest_owner_lsn.isValid()) |
					static_cast<uint8_t>(mutation.dimensions == 0U) | static_cast<uint8_t>(!knownMetric) |
					static_cast<uint8_t>(hasInserts && mutation.first_vector_id == 0U) |
					static_cast<uint8_t>(!hasInserts && mutation.first_vector_id != 0U) |
					static_cast<uint8_t>(hasInserts &&
										 mutation.inserted_vector_count - 1U >
												 std::numeric_limits<uint64_t>::max() - mutation.first_vector_id) |
					static_cast<uint8_t>(!mutation.first_slot.isValid()) |
					static_cast<uint8_t>(!mutation.last_slot.isValid()) |
					static_cast<uint8_t>(mutation.first_slot > mutation.last_slot);
			if (malformed != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph Vector delta has an invalid identity, fence, or resource bound");
			}
			const auto expectedMappingMutation = [&derivedState](uint64_t indexId, const IndexRootLocation &root) {
				const auto found = std::ranges::find(derivedState.index_tree_mutations, indexId,
													 &NativeGraphIndexTreeMutation::index_id);
				return found != derivedState.index_tree_mutations.end() && !found->retire_root &&
					   found->expected_root.has_value() && *found->expected_root == root;
			};
			const auto freshMappingMutation = [&derivedState](uint64_t indexId) {
				const auto found = std::ranges::find(derivedState.index_tree_mutations, indexId,
													 &NativeGraphIndexTreeMutation::index_id);
				return found != derivedState.index_tree_mutations.end() && !found->retire_root &&
					   !found->expected_root.has_value() && found->replacement_root.has_value() &&
					   found->replacement_root->newly_reserved;
			};
			const auto retirementMutation = [&derivedState](uint64_t indexId, const IndexRootLocation &root) {
				const auto found = std::ranges::find(derivedState.index_tree_mutations, indexId,
													 &NativeGraphIndexTreeMutation::index_id);
				return found != derivedState.index_tree_mutations.end() && found->retire_root &&
					   found->expected_root.has_value() && *found->expected_root == root;
			};
			if (!expectedMappingMutation(mutation.index_id, mutation.expected_node_mapping_root) ||
				!expectedMappingMutation(mutation.vector_node_mapping_index_id,
										 mutation.expected_vector_node_mapping_root) ||
				(hasExpectedDelta && publishesDelta &&
				 (!expectedMappingMutation(mutation.delta_mapping_index_id, *mutation.expected_delta_mapping_root) ||
				  mutation.delta_mapping_index_id != mutation.expected_delta_mapping_index_id)) ||
				(!hasExpectedDelta && publishesDelta && !freshMappingMutation(mutation.delta_mapping_index_id)) ||
				(hasExpectedDelta && !publishesDelta &&
				 !retirementMutation(mutation.expected_delta_mapping_index_id,
									 *mutation.expected_delta_mapping_root))) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"native graph Vector delta requires matching semantic and overlay mapping-tree transitions");
			}
			uint64_t previousErasedVectorId = 0U;
			for (const uint64_t vectorId: mutation.erased_vector_ids) {
				if (vectorId == 0U || vectorId <= previousErasedVectorId) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "native graph Vector delta has unordered erased Vector IDs");
				}
				previousErasedVectorId = vectorId;
			}
			uint64_t insertedRecords = 0U;
			uint32_t expectedPageOrdinal = 0U;
			for (const NativeGraphVectorDeltaRawPageMutation &raw: mutation.raw_pages) {
				const bool cow = raw.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL;
				const bool append = raw.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_APPEND_TO_CURRENT_POOL;
				const bool fresh = raw.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_NEW_POOL;
				const uint8_t invalidRaw = static_cast<uint8_t>(!cow && !append && !fresh) |
										   static_cast<uint8_t>(raw.page_ordinal != expectedPageOrdinal++) |
										   static_cast<uint8_t>(raw.inserted_record_count == 0U) |
										   static_cast<uint8_t>(cow && (!raw.expected_pool.segment_id.isValid() ||
																		!raw.expected_location.page_id.isValid() ||
																		!raw.expected_owner_lsn.isValid())) |
										   static_cast<uint8_t>(append && !raw.expected_pool.segment_id.isValid()) |
										   static_cast<uint8_t>(fresh && (raw.expected_pool.segment_id.isValid() ||
																		  raw.new_pool_ordinal == 0U));
				if (invalidRaw != 0U ||
					raw.inserted_record_count > std::numeric_limits<uint64_t>::max() - insertedRecords) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "native graph Vector delta has an invalid Raw-page transition");
				}
				insertedRecords += raw.inserted_record_count;
			}
			if (insertedRecords != mutation.inserted_vector_count) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"native graph Vector delta Raw-page transitions do not cover its inserted Vector IDs");
			}
			previousVectorIndexId = mutation.index_id;
		}

		// A logical-ID reservation is an in-memory capability layered over durable
		// high-water marks. Validate and consume it at this common publication
		// boundary so direct coordinator callers cannot forge an abandoned range.
		// Claim before taking the metadata writer: ID acquisition uses the same
		// lease lock before it enters the writer, which keeps the lock order stable.
		if (!logicalIdReservation.empty() && !claimNativeGraphLogicalIdReservation(logicalIdReservation)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph page transaction does not own its logical-ID lease capability");
		}
		// Construct the read-only adapter before acquiring the publication barrier:
		// its allocator attaches through openSnapshot(). Actual schema reads below
		// use the writer-fenced pin, so no stale schema is accepted.
		std::unique_ptr<SlottedPagePool> deferredSchemaPool;
		if (deferredIndexPlanner || earlyPublicationPlanner || deferredIndexPlansPlanner || !derivedState.index_images.empty() ||
            (RSS_SCAN_PATH_FORMAT_ENABLED && !derivedState.entity_free_summary_fences.empty())) {
			deferredSchemaPool = std::make_unique<SlottedPagePool>(
					io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
		}
        // Allocator adapters attach through openSnapshot(); construct them
        // before taking the writer, then use only its fenced source pin below.
        std::unique_ptr<EntityPageStore> conversionEntities;
        std::unique_ptr<SlottedPagePool> conversionPages;
        std::unique_ptr<LargeValueStore> conversionLargeValues;
        std::unique_ptr<PropertyCollectionRootStore> conversionRoots;
        std::unique_ptr<BTreePageStore> conversionIndexes;
        std::unique_ptr<EntityLabelSetStore> hybridLabels;
        if (hybridTopology || hybridVectors || (hybridPropertyRootMutation && hybridPropertyRootMutation->conversion)) {
            conversionEntities = std::make_unique<EntityPageStore>(io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            conversionPages = std::make_unique<SlottedPagePool>(io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            conversionLargeValues = std::make_unique<LargeValueStore>(io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            // The content-index allocator attaches through openSnapshot(). No
            // lazy snapshot acquisition is allowed inside the writer below.
            static_cast<void>(conversionLargeValues->contentIndex());
            conversionRoots = std::make_unique<PropertyCollectionRootStore>(*conversionPages, *conversionLargeValues, *this);
            conversionIndexes = std::make_unique<BTreePageStore>(*conversionPages, *this);
            if (hybridVectors || hybridTopology) hybridLabels = std::make_unique<EntityLabelSetStore>(io_, *this, *conversionPages, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        }
        const auto requirePrivateBuilding = [&](const Superblock &root) {
            if (!deferredBuildingAdmission) return;
            if (!RSS_SCAN_PATH_FORMAT_ENABLED || !(root.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING) ||
                !hybridPropertyRootMutation || root.database_id != hybridPropertyRootMutation->expected_database_id ||
                root.hybrid_property_descriptor_root != hybridPropertyRootMutation->expected_root ||
                expectedMetadataGeneration != std::optional(root.generation))
                throw FormatException(FormatError::FERR_INVALID_STATE,
                    "whole-file admission can be deferred only within its exact private BUILDING image");
        };
        // Check before writer recovery may change storage, then repeat under
        // the writer guard. Intermediate roots remain unavailable to public open.
        if (deferredBuildingAdmission) requirePrivateBuilding(bootstrap_.recover());
        MetadataWriteGuard writer = beginWrite(hybridTopology || hybridVectors || reclamationEvidence.has_value() || (hybridPropertyRootMutation && hybridPropertyRootMutation->conversion));
		const Superblock current = bootstrap_.recover();
        requirePrivateBuilding(current);
		// The writer fences this published generation until this call returns.
		// Reuse only immutable raw decodes through the existing bounded context;
		// StorageIO mutation epochs invalidate it and candidate generations are excluded.
		MetadataPageReadContext::Scope writerReads(
				std::make_shared<MetadataPageReadContext>(current), *io_);
        std::optional<MetadataSnapshotPin> topologySource;
        std::unique_ptr<HybridPropertyReadContext> topologyFields;
        bool preservingNativeFields = false;
        if (hybridTopology) {
            if (hybridTopology->source_ != current)
                throw FormatException(FormatError::FERR_INVALID_STATE, "native topology preservation has a stale graph fence");
            topologySource.emplace(snapshots_.pinCurrentUnderWrite(writer, current));
            topologyFields = std::make_unique<HybridPropertyReadContext>(*conversionPages, *this, *topologySource);
            preservingNativeFields = topologyFields->hasNativeGroups();
            if (preservingNativeFields) {
                if ((!hybridTopology->schema_preserved_ && !hybridTopology->schema_only_) ||
                    std::ranges::any_of(derivedState.index_tree_mutations, [](const auto &tree) { return isHybridPropertyIndexId(tree.index_id); }))
                    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native topology preservation cannot replace field roots or schema");
                if (findIndexRootOptional(*topologySource, RSS_SYSTEM_CATALOG_INDEX_ID)) {
                    SystemCatalog schema(*conversionIndexes, *this);
                    static_cast<void>(schema.readFieldMutationSchema(*topologySource, 4096, nullptr));
                }
                if (current.state_directory_root.isValid()) {
                    StateRecordStore states(*conversionPages, *this); size_t controls = 0;
                    if (!locations_.visitStatesAfter(current, 0, 64, [&](const StateLocation &location) {
                        if (++controls > 64) throw FormatException(FormatError::FERR_INVALID_STATE, "native topology State census exceeded its bound");
                        const auto record = states.read(*topologySource, location.state_id);
                        if ((record.key != RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY && record.key != RSS_NATIVE_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY) ||
                            record.storage_kind != StateRecordStorageKind::SRS_INLINE || record.large_value_id.isValid() ||
                            LargeValueMaintenanceJournalCodec::decode(record.inline_bytes).scan_generation > current.generation)
                            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native topology needs complete derived State coordination");
                        return true;
                    })) throw FormatException(FormatError::FERR_INVALID_STATE, "native topology did not complete its State census");
                }
            }
        }
		if (derivedState.external_private_tail_reservation.has_value()) {
			const PhysicalSlotRange tail = *derivedState.external_private_tail_reservation;
			const uint64_t fileSize = io_->fileSize();
			if (!tail.isValid() || fileSize < current.data_region_offset ||
				(fileSize - current.data_region_offset) % RSS_SEGMENT_SIZE != 0U ||
				tail.last.value() != (fileSize - current.data_region_offset) / RSS_SEGMENT_SIZE) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph external private tail reservation is not the current complete file suffix");
			}
		}
        uint64_t previousSummaryId = 0U;
        for (const auto &fence : derivedState.entity_free_summary_fences) {
            if (!isEntityFreeSpaceIndexId(fence.index_id) || fence.index_id <= previousSummaryId ||
                fence.expected_entity_root != (fence.index_id == RSS_NODE_FREE_SPACE_INDEX_ID ? current.node_entity_directory_root : current.edge_entity_directory_root) ||
                std::ranges::count(derivedState.index_tree_mutations, fence.index_id, &NativeGraphIndexTreeMutation::index_id) != 1)
                throw FormatException(FormatError::FERR_INVALID_STATE, "free-space summary has a stale source or no unique index mutation");
            previousSummaryId = fence.index_id;
        }
        for (const auto &tree : derivedState.index_tree_mutations)
            if (isEntityFreeSpaceIndexId(tree.index_id) && std::ranges::count(derivedState.entity_free_summary_fences,
                    tree.index_id, &NativeGraphEntityFreeSummaryFence::index_id) != 1)
                throw FormatException(FormatError::FERR_INVALID_STATE, "reserved free-space mutation lacks its entity-root fence");
		if (expectedMetadataGeneration.has_value() && current.generation != *expectedMetadataGeneration) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph metadata publication no longer matches its pinned generation");
		}
        if (reclamationEvidence) {
            const auto &proof = *reclamationEvidence;
            if (proof.source_.database_id != current.database_id || proof.source_.generation != current.generation ||
                proof.source_.hybrid_property_descriptor_root != current.hybrid_property_descriptor_root ||
                expectedMetadataGeneration != std::optional(current.generation) ||
                !std::ranges::equal(proof.erased_, erasedLargeValueIds) || !hasLargeValueRetirements ||
                !requests.empty() || !appendRequests.empty() || !newPoolRequests.empty() ||
                !propertyReplacements.empty() || !propertyPageBatchRequests.empty() || !largeValues.empty() ||
                !logicalIdReservation.empty() || hybridPropertyRootMutation ||
                privateSegmentImagePreparer || privateSegmentPreparer || deferredIndexPlanner ||
                earlyPublicationPlanner || deferredIndexPlansPlanner || vectorBootstrapPreparer || vectorDeltaPreparer ||
                derivedState.index_tree_mutations.size() != 1U ||
                derivedState.index_tree_mutations.front().index_id != RSS_LARGE_VALUE_CONTENT_INDEX_ID ||
                !derivedState.reused_large_values.empty() || !derivedState.vector_bootstraps.empty() ||
                !derivedState.vector_deltas.empty() || !derivedState.private_segments.empty() ||
                derivedState.private_segment_plan || derivedState.external_private_tail_reservation)
                throw FormatException(FormatError::FERR_INVALID_STATE,
                    "native LargeValue reclamation requires a current complete ownership proof and retirement-only work");
        }
        if (hybridPropertyRootMutation &&
            (hybridPropertyRootMutation->expected_database_id != current.database_id ||
                hybridPropertyRootMutation->expected_root != current.hybrid_property_descriptor_root))
            throw FormatException(FormatError::FERR_INVALID_STATE, "native hybrid root no longer matches its database and graph fence");
        if (hybridPropertyRootMutation && current.hybrid_property_descriptor_root.isValid())
            static_cast<void>(pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current));
		for (const LargeValueRef &reference: derivedState.reused_large_values) {
			// A prior snapshot authenticated the bytes. This writer-side fence only
			// proves that the immutable Manifest remains current before its small
			// Property reference can be made reachable.
			if (!locations_.findLargeValueManifestOptional(current, reference.value_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph reused LargeValue Manifest was retired before publication");
			}
		}
		uint64_t expectedVectorId = current.next_logical_vector_id;
		for (const NativeGraphVectorBootstrapMutation &mutation: derivedState.vector_bootstraps) {
			const uint8_t stale =
					static_cast<uint8_t>(expectedVectorId == 0U) |
					static_cast<uint8_t>(mutation.first_vector_id != expectedVectorId) |
					static_cast<uint8_t>(mutation.vector_count - 1U >
										 std::numeric_limits<uint64_t>::max() - mutation.first_vector_id) |
					static_cast<uint8_t>(locations_.findVectorManifestOptional(current, mutation.index_id).has_value());
			if (stale != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Vector bootstrap has a stale Manifest or noncontiguous Vector-ID reservation");
			}
			for (uint64_t offset = 0U; offset < mutation.vector_count; ++offset) {
				if (locations_.findVectorLocationOptional(current, mutation.first_vector_id + offset).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap reuses a published logical Vector ID");
				}
			}
			const uint64_t lastVectorId = mutation.first_vector_id + mutation.vector_count - 1U;
			expectedVectorId = lastVectorId == std::numeric_limits<uint64_t>::max() ? 0U : lastVectorId + 1U;
		}
		std::vector<const NativeGraphVectorDeltaMutation *> vectorDeltasByVectorRange;
		vectorDeltasByVectorRange.reserve(derivedState.vector_deltas.size());
		for (const NativeGraphVectorDeltaMutation &mutation: derivedState.vector_deltas) {
			vectorDeltasByVectorRange.push_back(&mutation);
		}
		std::ranges::sort(vectorDeltasByVectorRange, [](const NativeGraphVectorDeltaMutation *left,
														const NativeGraphVectorDeltaMutation *right) {
			const uint64_t leftFirst =
					left->inserted_vector_count == 0U ? std::numeric_limits<uint64_t>::max() : left->first_vector_id;
			const uint64_t rightFirst =
					right->inserted_vector_count == 0U ? std::numeric_limits<uint64_t>::max() : right->first_vector_id;
			return std::tuple<uint64_t, uint64_t>{leftFirst, left->index_id} <
				   std::tuple<uint64_t, uint64_t>{rightFirst, right->index_id};
		});
		for (const NativeGraphVectorDeltaMutation *mutation: vectorDeltasByVectorRange) {
			const std::optional<IndexRootLocation> nodeRoot =
					locations_.findIndexRootOptional(current, mutation->index_id);
			const std::optional<IndexRootLocation> reverseRoot =
					locations_.findIndexRootOptional(current, mutation->vector_node_mapping_index_id);
			const std::optional<IndexRootLocation> deltaRoot =
					mutation->expected_delta_mapping_root.has_value()
							? locations_.findIndexRootOptional(current, mutation->expected_delta_mapping_index_id)
							: std::nullopt;
			const std::optional<VectorManifestLocation> manifestDirectory =
					locations_.findVectorManifestOptional(current, mutation->index_id);
			const PageLocation manifestLocation =
					locations_.findPage(current, mutation->expected_manifest_location.page_id);
			const CatalogEntry manifestPool = catalog_.find(current, manifestLocation.segment_id);
			const uint8_t stale =
					static_cast<uint8_t>(!nodeRoot.has_value() || *nodeRoot != mutation->expected_node_mapping_root) |
					static_cast<uint8_t>(!reverseRoot.has_value() ||
										 *reverseRoot != mutation->expected_vector_node_mapping_root) |
					static_cast<uint8_t>(mutation->expected_delta_mapping_root.has_value() != deltaRoot.has_value()) |
					static_cast<uint8_t>(deltaRoot.has_value() &&
										 *deltaRoot != *mutation->expected_delta_mapping_root) |
					static_cast<uint8_t>(!manifestDirectory.has_value() ||
										 *manifestDirectory != mutation->expected_manifest) |
					static_cast<uint8_t>(manifestLocation != mutation->expected_manifest_location) |
					static_cast<uint8_t>(!hasSameCatalogEntry(manifestPool, mutation->expected_manifest_pool)) |
					static_cast<uint8_t>(manifestPool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
					static_cast<uint8_t>(manifestPool.owner_id != mutation->index_id) |
					static_cast<uint8_t>(manifestPool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, manifestPool.physical_slot));
			if (stale != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta has a stale Manifest or mapping-root fence");
			}
			for (const uint64_t vectorId: mutation->erased_vector_ids) {
				if (!locations_.findVectorLocationOptional(current, vectorId).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta erases an unreachable logical Vector ID");
				}
			}
			if (mutation->inserted_vector_count == 0U) {
				continue;
			}
			const uint8_t invalidRange =
					static_cast<uint8_t>(expectedVectorId == 0U) |
					static_cast<uint8_t>(mutation->first_vector_id != expectedVectorId) |
					static_cast<uint8_t>(mutation->inserted_vector_count - 1U >
										 std::numeric_limits<uint64_t>::max() - mutation->first_vector_id);
			if (invalidRange != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta has a noncontiguous logical Vector-ID reservation");
			}
			for (uint64_t offset = 0U; offset < mutation->inserted_vector_count; ++offset) {
				if (locations_.findVectorLocationOptional(current, mutation->first_vector_id + offset).has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta reuses a published logical Vector ID");
				}
			}
			const uint64_t lastVectorId = mutation->first_vector_id + mutation->inserted_vector_count - 1U;
			expectedVectorId = lastVectorId == std::numeric_limits<uint64_t>::max() ? 0U : lastVectorId + 1U;
		}
		uint64_t expectedLargeValueId = current.next_large_value_id;
		for (const NativeGraphLargeValueMutation &mutation: largeValues) {
			const bool validCodec = mutation.reference.codec == LargeValueCodec::LVC_RAW ||
									mutation.reference.codec == LargeValueCodec::LVC_LZ4 ||
									mutation.reference.codec == LargeValueCodec::LVC_ZSTD ||
									mutation.reference.codec == LargeValueCodec::LVC_MIXED;
			const uint8_t invalid =
					static_cast<uint8_t>(expectedLargeValueId == 0U) |
					static_cast<uint8_t>(mutation.reference.value_id.value() != expectedLargeValueId) |
					static_cast<uint8_t>(mutation.reference.generation != Generation(1U)) |
					static_cast<uint8_t>(mutation.reference.logical_bytes != mutation.logical_bytes.size()) |
					static_cast<uint8_t>(mutation.planned_manifest_page_count == 0U) |
					static_cast<uint8_t>(!validCodec) |
					static_cast<uint8_t>(mutation.logical_bytes.empty() != (mutation.planned_data_page_count == 0U)) |
					static_cast<uint8_t>(!mutation.first_data_slot.isValid()) |
					static_cast<uint8_t>(!mutation.last_data_slot.isValid()) |
					static_cast<uint8_t>(mutation.first_data_slot > mutation.last_data_slot) |
					static_cast<uint8_t>(!mutation.first_manifest_slot.isValid()) |
					static_cast<uint8_t>(!mutation.last_manifest_slot.isValid()) |
					static_cast<uint8_t>(mutation.first_manifest_slot > mutation.last_manifest_slot) |
					static_cast<uint8_t>(utils::Sha256::digest(mutation.logical_bytes) !=
										 mutation.reference.content_hash) |
					static_cast<uint8_t>(locations_.findLargeValueManifestOptional(current, mutation.reference.value_id)
												 .has_value());
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph LargeValue mutation is stale, malformed, or already reachable");
			}
			static_cast<void>(LargeValueRefCodec::encode(mutation.reference));
			if (expectedLargeValueId == std::numeric_limits<uint64_t>::max()) {
				if (&mutation != &largeValues.back()) {
					throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
										  "native graph LargeValue mutation exhausts the logical value ID space");
				}
			} else {
				++expectedLargeValueId;
			}
		}
		std::set<uint64_t> pageIds;
		std::vector<ExistingPoolPageRetirementBatch> largeValueRetirementBatches;
		if (hasLargeValueRetirements) {
			for (const LargeValueId valueId: erasedLargeValueIds) {
				const LargeValueManifestLocation manifest = locations_.findLargeValueManifest(current, valueId);
				const auto retirement = std::ranges::find_if(
						retiredLargeValuePages, [&manifest, valueId](const PageRetirement &candidate) {
							return candidate.location.page_id == manifest.manifest_page_id &&
								   candidate.expected_owner_id == valueId.value() &&
								   candidate.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
						});
				if (retirement == retiredLargeValuePages.end()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue retirement omits one current Manifest frame");
				}
			}
			std::map<uint64_t, ExistingPoolPageRetirementBatch> batchesBySegment;
			for (const PageRetirement &retirement: retiredLargeValuePages) {
				const PageLocation visibleLocation = locations_.findPage(current, retirement.location.page_id);
				const CatalogEntry pool = catalog_.find(current, retirement.location.segment_id);
				const SegmentKind expectedKind = retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_DATA
														 ? SegmentKind::SGK_LARGE_VALUE_DATA_POOL
														 : SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
				const uint64_t expectedPoolOwner = retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_DATA
														   ? RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID
														   : RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
				const uint8_t invalid =
						static_cast<uint8_t>(visibleLocation != retirement.location) |
						static_cast<uint8_t>(pool.kind != expectedKind) |
						static_cast<uint8_t>(pool.owner_id != expectedPoolOwner) |
						static_cast<uint8_t>(pool.generation != retirement.location.generation) |
						static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot)) |
						static_cast<uint8_t>(!pageIds.insert(retirement.location.page_id.value()).second);
				if (invalid != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue retirement has a stale or unavailable Page frame");
				}
				auto [iterator, inserted] = batchesBySegment.try_emplace(pool.segment_id.value());
				if (inserted) {
					iterator->second.pool = pool;
				}
				iterator->second.retirements.push_back(retirement);
			}
			largeValueRetirementBatches.reserve(batchesBySegment.size());
			for (auto &[segmentId, batch]: batchesBySegment) {
				static_cast<void>(segmentId);
				largeValueRetirementBatches.push_back(std::move(batch));
			}
		}
		std::set<uint64_t> insertedNodeIds;
		std::set<uint64_t> erasedNodeIds;
		std::set<uint64_t> insertedEdgeIds;
		std::set<uint64_t> erasedEdgeIds;
		std::vector<EntityPageLocation> insertedNodes;
		std::vector<uint64_t> erasedNodes;
		std::vector<EntityPageLocation> insertedEdges;
		std::vector<uint64_t> erasedEdges;
		std::vector<uint64_t> retiredPageIds;
		retiredPageIds.reserve(retiredLargeValuePages.size());
		for (const PageRetirement &retirement: retiredLargeValuePages) {
			retiredPageIds.push_back(retirement.location.page_id.value());
		}
		std::vector<EntityPageReplacementMaterialization> materializations;
		std::vector<std::optional<EntityPageFreshPoolDestination>> replacementFreshDestinations;
		std::vector<EntityPageAppendBatchMaterialization> appendMaterializations;
		std::vector<EntityPageNewPoolBatchMaterialization> newPoolMaterializations;
		std::vector<NativeGraphPropertyPageReplacementMaterialization> propertyMaterializations;
		std::vector<bool> propertyReplacementNeedsFreshPool;
		std::vector<NativeGraphPropertyPageBatchMaterialization> propertyPageBatchMaterializations;
		std::vector<LargeValueMaterializationResult> largeValueMaterializations;
		materializations.reserve(requests.size());
		replacementFreshDestinations.reserve(requests.size());
		appendMaterializations.reserve(appendRequests.size());
		newPoolMaterializations.reserve(newPoolRequests.size());
		propertyMaterializations.reserve(propertyReplacements.size());
		propertyReplacementNeedsFreshPool.reserve(propertyReplacements.size());
		propertyPageBatchMaterializations.reserve(propertyPageBatchRequests.size());
		largeValueMaterializations.reserve(largeValues.size());

		// Revalidate the pinned B+Tree roots and every existing page/pool fence
		// while this writer owns the current metadata generation. New pools and
		// PageIds are assigned later from the same MetadataWriteBatch as Entity
		// and Property work, so none can become visible independently.
		// Deferred bulk planners append only after their candidate scratch bytes are
		// durable. Keep the ordinary caller-owned mutations immutable and extend a
		// coordinator-owned copy only through the typed deferred protocol below.
		std::vector<NativeGraphIndexTreeMutation> effectiveIndexMutations = derivedState.index_tree_mutations;
		std::vector<std::vector<IndexTreeMutationPoolBatch>> derivedIndexBatches;
		derivedIndexBatches.reserve(derivedState.index_tree_mutations.size() + (deferredIndexPlanner ? 1U : 0U));
		for (const NativeGraphIndexTreeMutation &mutation: derivedState.index_tree_mutations) {
			const std::optional<IndexRootLocation> visibleRoot =
					locations_.findIndexRootOptional(current, mutation.index_id);
			if (mutation.expected_root.has_value()) {
				if (!visibleRoot.has_value() || *visibleRoot != *mutation.expected_root) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph derived-state mutation has a stale Index root mapping");
				}
				const PageLocation rootLocation = locations_.findPage(current, mutation.expected_root->root_page_id);
				const CatalogEntry rootPool = catalog_.find(current, rootLocation.segment_id);
				const uint8_t invalidRoot =
						static_cast<uint8_t>(rootPool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
						static_cast<uint8_t>(rootPool.owner_id == 0U) |
						static_cast<uint8_t>(rootPool.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(rootPool.generation != rootLocation.generation) |
						static_cast<uint8_t>(rootLocation.generation != mutation.expected_root->root_generation) |
						static_cast<uint8_t>(!allocator_.isAllocated(current, rootPool.physical_slot));
				if (invalidRoot != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph derived-state mutation root does not resolve to one current Index pool");
				}
			} else if (visibleRoot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph derived-state root creation found an already published Index root");
			}

			std::vector<IndexTreeMutationPoolBatch> batches;
			batches.reserve(mutation.pool_requests.size() + mutation.new_pool_requests.size());
			for (const IndexTreeMutationPoolRequest &request: mutation.pool_requests) {
				const CatalogEntry pool = catalog_.find(current, request.segment_id);
				const uint8_t invalidPool = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
											static_cast<uint8_t>(pool.owner_id == 0U) |
											static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
											static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
				if (invalidPool != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph derived-state mutation requires current owned Index pools");
				}
				CatalogEntry replacementPool = pool;
				if (request.replacement_segment_id.isValid()) {
					replacementPool = catalog_.find(current, request.replacement_segment_id);
					const uint8_t invalidReplacementPool =
							static_cast<uint8_t>(replacementPool.segment_id == pool.segment_id) |
							static_cast<uint8_t>(replacementPool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
							static_cast<uint8_t>(replacementPool.owner_id != pool.owner_id) |
							static_cast<uint8_t>(replacementPool.state != SegmentState::SGS_SEALED) |
							static_cast<uint8_t>(!allocator_.isAllocated(current, replacementPool.physical_slot));
					if (invalidReplacementPool != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph derived-state mutation has an unavailable cross-pool Index replacement destination");
					}
				}
				const auto validateSource = [this, &current, &pool, &pageIds](const PageLocation &location,
																			  const char *message) {
					const PageLocation visible = locations_.findPage(current, location.page_id);
					const uint8_t invalid = static_cast<uint8_t>(visible != location) |
											static_cast<uint8_t>(location.segment_id != pool.segment_id) |
											static_cast<uint8_t>(location.generation != pool.generation) |
											static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
											static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
											static_cast<uint8_t>(!pageIds.insert(location.page_id.value()).second);
					if (invalid != 0U) {
						throw FormatException(FormatError::FERR_INVALID_STATE, message);
					}
				};
				for (const PageLocation &location: request.expected_replacements) {
					validateSource(
							location,
							"native graph derived-state mutation has a stale or duplicate Index replacement page");
				}
				for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
					validateSource(
							retirement.location,
							"native graph derived-state mutation has a stale or duplicate Index retirement page");
				}
				batches.push_back({pool,
								   request.expected_replacements,
								   {},
								   request.expected_replacements,
								   {},
								   false,
								   request.expected_retirements,
								   0U,
								   replacementPool});
			}
			derivedIndexBatches.push_back(std::move(batches));
		}

		// Resolve all Entity IDs touched by this publication through one ordered
		// Directory walk per entity kind. This avoids a radix descent for every
		// insert/erase when a batch contains many entries from the same leaves.
		std::vector<uint64_t> validationNodeIds;
		std::vector<uint64_t> validationEdgeIds;
		const auto collectValidationIds = [&validationNodeIds, &validationEdgeIds](
				std::span<const EntityPageLocation> insertedNodes, std::span<const uint64_t> erasedNodes,
				std::span<const EntityPageLocation> insertedEdges, std::span<const uint64_t> erasedEdges) {
			for (const EntityPageLocation &location: insertedNodes) {
				if (location.entity_id != 0U) validationNodeIds.push_back(location.entity_id);
			}
			for (const uint64_t entityId: erasedNodes) {
				if (entityId != 0U) validationNodeIds.push_back(entityId);
			}
			for (const EntityPageLocation &location: insertedEdges) {
				if (location.entity_id != 0U) validationEdgeIds.push_back(location.entity_id);
			}
			for (const uint64_t entityId: erasedEdges) {
				if (entityId != 0U) validationEdgeIds.push_back(entityId);
			}
		};
		for (const EntityPageReplacementRequest &request: requests) {
			collectValidationIds(request.inserted_nodes, request.erased_node_ids, request.inserted_edges,
								request.erased_edge_ids);
		}
		for (const EntityPageAppendBatchRequest &request: appendRequests) {
			collectValidationIds(request.inserted_nodes, std::span<const uint64_t>{}, request.inserted_edges,
								std::span<const uint64_t>{});
		}
		for (const EntityPageNewPoolBatchRequest &request: newPoolRequests) {
			collectValidationIds(request.inserted_nodes, std::span<const uint64_t>{}, request.inserted_edges,
								std::span<const uint64_t>{});
		}
		const auto prepareValidationIds = [](std::vector<uint64_t> &ids) {
			std::ranges::sort(ids);
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		};
		prepareValidationIds(validationNodeIds);
		prepareValidationIds(validationEdgeIds);
		std::vector<std::optional<EntityPageLocation>> validationNodeLocations(validationNodeIds.size());
		std::vector<std::optional<EntityPageLocation>> validationEdgeLocations(validationEdgeIds.size());
		if (!validationNodeIds.empty()) {
			locations_.findEntityPageLocationsSorted(current, MetadataDirectoryType::MDT_NODE_ENTITY, validationNodeIds,
												validationNodeLocations);
		}
		if (!validationEdgeIds.empty()) {
			locations_.findEntityPageLocationsSorted(current, MetadataDirectoryType::MDT_EDGE_ENTITY, validationEdgeIds,
												validationEdgeLocations);
		}
		const auto findValidationLocation = [](uint64_t entityId, const std::vector<uint64_t> &ids,
															const std::vector<std::optional<EntityPageLocation>> &locations)
															-> std::optional<EntityPageLocation> {
			if (entityId == 0U) {
				return std::nullopt;
			}
			const auto iterator = std::ranges::lower_bound(ids, entityId);
			if (iterator == ids.end() || *iterator != entityId) {
				return std::nullopt;
			}
			return locations[static_cast<size_t>(iterator - ids.begin())];
		};
		const auto validateInserted = [&validationNodeIds, &validationEdgeIds, &validationNodeLocations,
										&validationEdgeLocations, &findValidationLocation](
										std::span<const EntityPageLocation> locations, PageId pageId, bool nodes,
										std::set<uint64_t> &seen, const char *message) {
			uint64_t previous = 0U;
			for (const EntityPageLocation &location: locations) {
				const std::optional<EntityPageLocation> existing = nodes
						? findValidationLocation(location.entity_id, validationNodeIds, validationNodeLocations)
						: findValidationLocation(location.entity_id, validationEdgeIds, validationEdgeLocations);
				const uint8_t invalid = static_cast<uint8_t>(location.entity_id == 0U) |
										static_cast<uint8_t>(location.entity_id <= previous) |
										static_cast<uint8_t>(location.page_id != pageId) |
										static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
										static_cast<uint8_t>(location.slot_generation == 0U) |
										static_cast<uint8_t>(existing.has_value()) |
										static_cast<uint8_t>(!seen.insert(location.entity_id).second);
				if (invalid != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE, message);
				}
				previous = location.entity_id;
			}
		};
		const auto validateErasures = [&validationNodeIds, &validationEdgeIds, &validationNodeLocations,
													 &validationEdgeLocations, &findValidationLocation](
															std::span<const uint64_t> entityIds, PageId pageId, bool nodes,
															std::set<uint64_t> &seen, const char *message) {
			uint64_t previous = 0U;
			for (const uint64_t entityId: entityIds) {
				const std::optional<EntityPageLocation> existing = nodes
						? findValidationLocation(entityId, validationNodeIds, validationNodeLocations)
						: findValidationLocation(entityId, validationEdgeIds, validationEdgeLocations);
				const uint8_t invalid = static_cast<uint8_t>(entityId == 0U) |
										static_cast<uint8_t>(entityId <= previous) |
										static_cast<uint8_t>(!existing.has_value()) |
										static_cast<uint8_t>(existing.has_value() && existing->page_id != pageId) |
										static_cast<uint8_t>(!seen.insert(entityId).second);
				if (invalid != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE, message);
				}
				previous = entityId;
			}
		};

		std::map<uint64_t, EntityPageFreshPoolDestination> freshReplacementPools;
		for (const EntityPageReplacementRequest &request: requests) {
			const CatalogEntry visible = catalog_.find(current, request.expected_pool.segment_id);
			const bool hasReplacementPool = request.replacement_pool.segment_id.isValid();
			const bool hasFreshReplacementPool = request.replacement_new_pool.has_value();
			const bool malformedAbsentReplacementPool =
					!hasReplacementPool && !hasFreshReplacementPool &&
					(request.replacement_pool.physical_slot.isValid() ||
					 request.replacement_pool.kind != SegmentKind::SGK_NONE ||
					 request.replacement_pool.owner_id != 0U || request.replacement_pool.generation.isValid() ||
					 request.replacement_pool.state != SegmentState::SGS_FREE ||
					 request.replacement_pool.committed_lsn.isValid());
			const CatalogEntry replacementPool =
					hasReplacementPool ? catalog_.find(current, request.replacement_pool.segment_id) : visible;
			const EntityPageFreshPoolDestination freshPool =
					hasFreshReplacementPool ? *request.replacement_new_pool : EntityPageFreshPoolDestination{};
			const uint8_t invalidFreshReplacementPool = static_cast<uint8_t>(
					hasFreshReplacementPool &&
					(!freshPool.segment_id.isValid() ||
					 freshPool.segment_id.value() < RSS_FIRST_ALLOCATABLE_SEGMENT_ID || freshPool.pool_owner_id == 0U ||
					 !freshPool.generation.isValid() || !freshPool.first_slot.isValid() ||
					 !freshPool.last_slot.isValid() || freshPool.first_slot > freshPool.last_slot ||
					 catalog_.findOptional(current, freshPool.segment_id).has_value()));
			if (invalidFreshReplacementPool != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity page batch fresh replacement pool is malformed or published");
			}
			if (hasFreshReplacementPool) {
				auto [iterator, inserted] = freshReplacementPools.emplace(freshPool.segment_id.value(), freshPool);
				if (!inserted && !hasSameFreshEntityPoolIdentity(iterator->second, freshPool)) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable Entity page batch reuses a fresh replacement Segment ID inconsistently");
				}
			}
			const PageLocation currentLocation = locations_.findPage(current, request.expected_location.page_id);
			const bool changesNodes = !request.inserted_nodes.empty() || !request.erased_node_ids.empty();
			const bool changesEdges = !request.inserted_edges.empty() || !request.erased_edge_ids.empty();
			const bool isAuxiliaryPage = request.owner_kind != PageOwnerKind::POK_ENTITY;
			const bool invalidRetirement =
					request.retire_page &&
					(isAuxiliaryPage ? hasEntityMapDelta(request)
									 : (!request.inserted_nodes.empty() || !request.inserted_edges.empty() ||
										(!request.erased_node_ids.empty() == !request.erased_edge_ids.empty())));
			const uint8_t invalid =
					static_cast<uint8_t>(visible.segment_id != request.expected_pool.segment_id) |
					static_cast<uint8_t>(visible.physical_slot != request.expected_pool.physical_slot) |
					static_cast<uint8_t>(visible.kind != request.expected_pool.kind) |
					static_cast<uint8_t>(visible.owner_id != request.expected_pool.owner_id) |
					static_cast<uint8_t>(visible.generation != request.expected_pool.generation) |
					static_cast<uint8_t>(visible.committed_lsn != request.expected_pool.committed_lsn) |
					static_cast<uint8_t>(visible.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
					static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot)) |
					static_cast<uint8_t>(malformedAbsentReplacementPool) |
					static_cast<uint8_t>(hasReplacementPool && hasFreshReplacementPool) |
					static_cast<uint8_t>(hasReplacementPool &&
										 request.replacement_pool.physical_slot != replacementPool.physical_slot) |
					static_cast<uint8_t>(hasReplacementPool && request.replacement_pool.kind != replacementPool.kind) |
					static_cast<uint8_t>(hasReplacementPool &&
										 request.replacement_pool.owner_id != replacementPool.owner_id) |
					static_cast<uint8_t>(hasReplacementPool &&
										 request.replacement_pool.generation != replacementPool.generation) |
					static_cast<uint8_t>(hasReplacementPool &&
										 request.replacement_pool.state != replacementPool.state) |
					static_cast<uint8_t>(hasReplacementPool &&
										 request.replacement_pool.committed_lsn != replacementPool.committed_lsn) |
					static_cast<uint8_t>(replacementPool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
					static_cast<uint8_t>(replacementPool.owner_id != visible.owner_id) |
					static_cast<uint8_t>(hasFreshReplacementPool && freshPool.pool_owner_id != visible.owner_id) |
					static_cast<uint8_t>(replacementPool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!replacementPool.generation.isValid()) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, replacementPool.physical_slot)) |
					static_cast<uint8_t>(currentLocation != request.expected_location) |
					static_cast<uint8_t>(request.expected_location.segment_id != visible.segment_id) |
					static_cast<uint8_t>(request.expected_location.generation != visible.generation) |
					static_cast<uint8_t>(request.expected_location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(request.expected_location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!isEntityPoolPageOwnerKind(request.owner_kind)) |
					static_cast<uint8_t>(isAuxiliaryPage && hasEntityMapDelta(request)) |
					static_cast<uint8_t>(changesNodes && changesEdges) | static_cast<uint8_t>(invalidRetirement) |
					static_cast<uint8_t>(request.retire_page && (hasReplacementPool || hasFreshReplacementPool)) |
					static_cast<uint8_t>(!pageIds.insert(request.expected_location.page_id.value()).second);
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity page batch replacement has a stale pool, duplicate page, or "
									  "invalid map delta");
			}
			validateInserted(
					request.inserted_nodes, request.expected_location.page_id, true, insertedNodeIds,
					"relocatable Entity page batch replacement has an invalid or published Node Entity Map entry");
			validateErasures(
					request.erased_node_ids, request.expected_location.page_id, true, erasedNodeIds,
					"relocatable Entity page batch replacement erases an absent or foreign Node Entity Map entry");
			validateInserted(
					request.inserted_edges, request.expected_location.page_id, false, insertedEdgeIds,
					"relocatable Entity page batch replacement has an invalid or published Edge Entity Map entry");
			validateErasures(
					request.erased_edge_ids, request.expected_location.page_id, false, erasedEdgeIds,
					"relocatable Entity page batch replacement erases an absent or foreign Edge Entity Map entry");
			insertedNodes.insert(insertedNodes.end(), request.inserted_nodes.begin(), request.inserted_nodes.end());
			erasedNodes.insert(erasedNodes.end(), request.erased_node_ids.begin(), request.erased_node_ids.end());
			insertedEdges.insert(insertedEdges.end(), request.inserted_edges.begin(), request.inserted_edges.end());
			erasedEdges.insert(erasedEdges.end(), request.erased_edge_ids.begin(), request.erased_edge_ids.end());
			materializations.push_back({replacementPool, currentLocation,
										request.retire_page
												? PageLocation{}
												: PageLocation{currentLocation.page_id, replacementPool.segment_id, 0U,
															   replacementPool.generation, 0U},
										request.retire_page, hasFreshReplacementPool, request.owner_kind});
			replacementFreshDestinations.push_back(request.replacement_new_pool);
		}
		for (const EntityPageAppendBatchRequest &request: appendRequests) {
			const CatalogEntry visible = catalog_.find(current, request.expected_pool.segment_id);
			const bool isAuxiliaryPage = request.owner_kind != PageOwnerKind::POK_ENTITY;
			const bool invalidMapShape = isAuxiliaryPage
												 ? (!request.inserted_nodes.empty() || !request.inserted_edges.empty())
												 : (request.inserted_nodes.empty() == request.inserted_edges.empty());
			const bool invalid =
					!request.page_id.isValid() || invalidMapShape || !isEntityPoolPageOwnerKind(request.owner_kind) ||
					visible.segment_id != request.expected_pool.segment_id ||
					visible.physical_slot != request.expected_pool.physical_slot ||
					visible.kind != request.expected_pool.kind || visible.owner_id != request.expected_pool.owner_id ||
					visible.generation != request.expected_pool.generation ||
					visible.committed_lsn != request.expected_pool.committed_lsn ||
					visible.kind != SegmentKind::SGK_ENTITY_PAGE_POOL || visible.state != SegmentState::SGS_SEALED ||
					!allocator_.isAllocated(current, visible.physical_slot) ||
					locations_.findPageOptional(current, request.page_id).has_value() ||
					!pageIds.insert(request.page_id.value()).second;
			if (invalid) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity page batch append has a stale pool, published Page ID, or "
									  "invalid map delta");
			}
			validateInserted(request.inserted_nodes, request.page_id, true, insertedNodeIds,
							 "relocatable Entity page batch append has an invalid or published Node Entity Map entry");
			validateInserted(request.inserted_edges, request.page_id, false, insertedEdgeIds,
							 "relocatable Entity page batch append has an invalid or published Edge Entity Map entry");
			insertedNodes.insert(insertedNodes.end(), request.inserted_nodes.begin(), request.inserted_nodes.end());
			insertedEdges.insert(insertedEdges.end(), request.inserted_edges.begin(), request.inserted_edges.end());
			appendMaterializations.push_back(
					{visible, {request.page_id, visible.segment_id, 0U, visible.generation, 0U}, request.owner_kind});
		}
		// A fresh Entity pool may start with several typed pages (for example, a
		// Node page and a LabelSet page). They share exactly one Catalog identity
		// and physical slot, while every PageId remains independently validated.
		std::map<uint64_t, size_t> firstNewPoolRequestBySegment;
		std::map<uint64_t, size_t> newPoolInitialFrameCounts;
		for (size_t requestOrdinal = 0U; requestOrdinal < newPoolRequests.size(); ++requestOrdinal) {
			const EntityPageNewPoolBatchRequest &request = newPoolRequests[requestOrdinal];
			const CatalogSlotReservationRequest &reservation = request.reservation;
			const bool isAuxiliaryPage = request.owner_kind != PageOwnerKind::POK_ENTITY;
			const bool invalidMapShape = isAuxiliaryPage
												 ? (!request.inserted_nodes.empty() || !request.inserted_edges.empty())
												 : (request.inserted_nodes.empty() == request.inserted_edges.empty());
			const auto [firstRequest, firstForSegment] =
					firstNewPoolRequestBySegment.try_emplace(reservation.segment_id.value(), requestOrdinal);
			const bool conflictingPoolIdentity =
					!firstForSegment && !hasSameNewEntityPoolIdentity(request, newPoolRequests[firstRequest->second]);
			const size_t initialFrameCount = ++newPoolInitialFrameCounts[reservation.segment_id.value()];
			const bool invalid = !reservation.segment_id.isValid() ||
								 reservation.segment_id.value() < RSS_FIRST_ALLOCATABLE_SEGMENT_ID ||
								 reservation.kind != SegmentKind::SGK_ENTITY_PAGE_POOL || reservation.owner_id == 0U ||
								 !reservation.segment_generation.isValid() ||
								 reservation.state != SegmentState::SGS_SEALED || !reservation.require_absent ||
								 !request.page_id.isValid() || !request.first_slot.isValid() ||
								 !request.last_slot.isValid() || request.first_slot > request.last_slot ||
								 invalidMapShape || !isEntityPoolPageOwnerKind(request.owner_kind) ||
								 catalog_.findOptional(current, reservation.segment_id).has_value() ||
								 conflictingPoolIdentity || initialFrameCount > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
								 locations_.findPageOptional(current, request.page_id).has_value() ||
								 !pageIds.insert(request.page_id.value()).second;
			if (invalid) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page new-pool batch has a published identity or invalid pool request");
			}
			validateInserted(
					request.inserted_nodes, request.page_id, true, insertedNodeIds,
					"relocatable Entity page new-pool batch has an invalid or published Node Entity Map entry");
			validateInserted(
					request.inserted_edges, request.page_id, false, insertedEdgeIds,
					"relocatable Entity page new-pool batch has an invalid or published Edge Entity Map entry");
			insertedNodes.insert(insertedNodes.end(), request.inserted_nodes.begin(), request.inserted_nodes.end());
			insertedEdges.insert(insertedEdges.end(), request.inserted_edges.begin(), request.inserted_edges.end());
		}
		std::set<uint64_t> deletedEntityPropertyIds;
		const auto registerDeletedEntityProperties = [&deletedEntityPropertyIds](std::span<const uint64_t> entityIds,
																				 EntityPropertyOwnerKind ownerKind) {
			for (const uint64_t entityId: entityIds) {
				const EntityPropertyKey key{ownerKind, entityId};
				if (!key.isValid() || !deletedEntityPropertyIds.insert(key.directoryId()).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph page transaction has duplicate deleted Entity Property owners");
				}
			}
		};
		registerDeletedEntityProperties(erasedNodes, EntityPropertyOwnerKind::EPOK_NODE);
		registerDeletedEntityProperties(erasedEdges, EntityPropertyOwnerKind::EPOK_EDGE);
		std::map<uint64_t, bool> existingNodePropertyOwners;
		std::map<uint64_t, bool> existingEdgePropertyOwners;
		const auto ownerSurvivesTransaction = [this, &current, &insertedNodeIds, &erasedNodeIds, &insertedEdgeIds,
																					 &erasedEdgeIds, &existingNodePropertyOwners,
																					 &existingEdgePropertyOwners](const EntityPropertyKey &key) {
			switch (key.owner_kind) {
				case EntityPropertyOwnerKind::EPOK_NODE:
					if (erasedNodeIds.contains(key.entity_id)) {
						return false;
					}
					if (insertedNodeIds.contains(key.entity_id)) {
						return true;
					}
					if (const auto iterator = existingNodePropertyOwners.find(key.entity_id);
						iterator != existingNodePropertyOwners.end()) {
						return iterator->second;
					}
					return locations_.findNodePageLocationOptional(current, key.entity_id).has_value();
				case EntityPropertyOwnerKind::EPOK_EDGE:
					if (erasedEdgeIds.contains(key.entity_id)) {
						return false;
					}
					if (insertedEdgeIds.contains(key.entity_id)) {
						return true;
					}
					if (const auto iterator = existingEdgePropertyOwners.find(key.entity_id);
						iterator != existingEdgePropertyOwners.end()) {
						return iterator->second;
					}
					return locations_.findEdgePageLocationOptional(current, key.entity_id).has_value();
			}
			return false;
		};

		// Resolve every Property owner touched by this publication through one
		// ordered Directory walk. Property replacement validation used to perform
		// a root-to-leaf lookup for each entry, which amplified COW publication
		// cost with the number of properties in the batch. Invalid keys are kept
		// out of the batch so the validation below can preserve its fail-closed
		// error path without asking the Directory to interpret an invalid ID.
		std::vector<EntityPropertyKey> propertyLookupKeys;
		const auto collectPropertyLookupKey = [&propertyLookupKeys](const EntityPropertyKey &key) {
			// Keep the legacy validation order: malformed keys fail with the
			// Directory's INVALID_HEADER before any publication-local lookup is
			// attempted. Valid keys alone enter the ordered batch.
			if (!key.isValid()) {
				static_cast<void>(key.directoryId());
			}
			propertyLookupKeys.push_back(key);
		};
		for (const NativeGraphPropertyPageReplacementRequest &request: propertyReplacements) {
			for (const EntityPropertyLocation &location: request.upserted_properties) {
				collectPropertyLookupKey(location.key);
			}
			for (const EntityPropertyKey &key: request.erased_properties) {
				collectPropertyLookupKey(key);
			}
			for (const EntityPropertyKey &key: request.relocated_properties) {
				collectPropertyLookupKey(key);
			}
		}
		for (const NativeGraphPropertyPageBatchRequest &request: propertyPageBatchRequests) {
			for (const std::vector<EntityPropertyKey> &pageKeys: request.page_property_keys) {
				for (const EntityPropertyKey &key: pageKeys) {
					collectPropertyLookupKey(key);
				}
			}
		}
		for (const uint64_t entityId: erasedNodes) {
			collectPropertyLookupKey({EntityPropertyOwnerKind::EPOK_NODE, entityId});
		}
		for (const uint64_t entityId: erasedEdges) {
			collectPropertyLookupKey({EntityPropertyOwnerKind::EPOK_EDGE, entityId});
		}
		std::vector<uint64_t> propertyNodeOwnerIds;
		std::vector<uint64_t> propertyEdgeOwnerIds;
		for (const EntityPropertyKey &key: propertyLookupKeys) {
			(key.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? propertyNodeOwnerIds : propertyEdgeOwnerIds)
					.push_back(key.entity_id);
		}
		const auto prepareOwnerLookupIds = [](std::vector<uint64_t> &ids) {
			std::ranges::sort(ids);
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		};
		prepareOwnerLookupIds(propertyNodeOwnerIds);
		prepareOwnerLookupIds(propertyEdgeOwnerIds);
		std::vector<std::optional<EntityPageLocation>> propertyNodeOwnerLocations(propertyNodeOwnerIds.size());
		std::vector<std::optional<EntityPageLocation>> propertyEdgeOwnerLocations(propertyEdgeOwnerIds.size());
		if (!propertyNodeOwnerIds.empty()) {
			locations_.findEntityPageLocationsSorted(
					current, MetadataDirectoryType::MDT_NODE_ENTITY, propertyNodeOwnerIds, propertyNodeOwnerLocations);
		}
		if (!propertyEdgeOwnerIds.empty()) {
			locations_.findEntityPageLocationsSorted(
					current, MetadataDirectoryType::MDT_EDGE_ENTITY, propertyEdgeOwnerIds, propertyEdgeOwnerLocations);
		}
		for (size_t index = 0U; index < propertyNodeOwnerIds.size(); ++index) {
			existingNodePropertyOwners.emplace(propertyNodeOwnerIds[index], propertyNodeOwnerLocations[index].has_value());
		}
		for (size_t index = 0U; index < propertyEdgeOwnerIds.size(); ++index) {
			existingEdgePropertyOwners.emplace(propertyEdgeOwnerIds[index], propertyEdgeOwnerLocations[index].has_value());
		}
		std::ranges::sort(propertyLookupKeys, {}, &EntityPropertyKey::directoryId);
		propertyLookupKeys.erase(
				std::unique(propertyLookupKeys.begin(), propertyLookupKeys.end(), [](const EntityPropertyKey &left,
																									 const EntityPropertyKey &right) {
					return left.directoryId() == right.directoryId();
				}),
				propertyLookupKeys.end());
		std::vector<std::optional<EntityPropertyLocation>> propertyLookupResults(propertyLookupKeys.size());
		if (!propertyLookupKeys.empty()) {
			locations_.findEntityPropertiesSorted(current, propertyLookupKeys, propertyLookupResults);
		}
		std::map<uint64_t, std::optional<EntityPropertyLocation>> existingPropertyLocations;
		for (size_t index = 0U; index < propertyLookupKeys.size(); ++index) {
			existingPropertyLocations.emplace(propertyLookupKeys[index].directoryId(), std::move(propertyLookupResults[index]));
		}
		const auto findExistingProperty = [&existingPropertyLocations](const EntityPropertyKey &key)
				-> std::optional<EntityPropertyLocation> {
			if (!key.isValid()) {
				return std::nullopt;
			}
			const auto iterator = existingPropertyLocations.find(key.directoryId());
			return iterator == existingPropertyLocations.end() ? std::nullopt : iterator->second;
		};

		std::set<uint64_t> touchedPropertyKeys;
		std::set<std::pair<uint64_t, uint16_t>> publishedPropertySlots;
		std::vector<EntityPropertyLocation> upsertedEntityProperties;
		std::vector<EntityPropertyKey> explicitlyErasedEntityProperties;
		std::map<uint64_t, EntityPropertyLocation> relocatedPropertySources;
		for (const NativeGraphPropertyPageReplacementRequest &request: propertyReplacements) {
			const CatalogEntry visible = catalog_.find(current, request.expected_pool.segment_id);
			const PageLocation currentLocation = locations_.findPage(current, request.expected_location.page_id);
			const bool invalidRetirement =
					request.retire_page &&
					(!request.upserted_properties.empty() ||
					 (request.erased_properties.empty() && request.relocated_properties.empty()));
			const uint8_t invalidSource =
					static_cast<uint8_t>(visible.segment_id != request.expected_pool.segment_id) |
					static_cast<uint8_t>(visible.physical_slot != request.expected_pool.physical_slot) |
					static_cast<uint8_t>(visible.kind != request.expected_pool.kind) |
					static_cast<uint8_t>(visible.owner_id != request.expected_pool.owner_id) |
					static_cast<uint8_t>(visible.generation != request.expected_pool.generation) |
					static_cast<uint8_t>(visible.committed_lsn != request.expected_pool.committed_lsn) |
					static_cast<uint8_t>(visible.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
					static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot)) |
					static_cast<uint8_t>(currentLocation != request.expected_location) |
					static_cast<uint8_t>(request.expected_location.segment_id != visible.segment_id) |
					static_cast<uint8_t>(request.expected_location.generation != visible.generation) |
					static_cast<uint8_t>(request.expected_location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(request.expected_location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(request.expected_page_owner_id == 0U) |
					static_cast<uint8_t>(!request.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(request.upserted_properties.empty() && request.erased_properties.empty() &&
										 request.relocated_properties.empty()) |
					static_cast<uint8_t>(invalidRetirement) |
					static_cast<uint8_t>(!pageIds.insert(request.expected_location.page_id.value()).second);
			if (invalidSource != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph page transaction has a stale or invalid Property page replacement source");
			}
			for (const EntityPropertyLocation &location: request.upserted_properties) {
				const std::optional<EntityPropertyLocation> existing =
						findExistingProperty(location.key);
				const uint8_t invalidLocation =
						static_cast<uint8_t>(!location.key.isValid()) |
						static_cast<uint8_t>(!ownerSurvivesTransaction(location.key)) |
						static_cast<uint8_t>(location.page_id != request.expected_location.page_id) |
						static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
						static_cast<uint8_t>(location.slot_generation == 0U) |
						static_cast<uint8_t>(existing.has_value() &&
											 existing->page_id != request.expected_location.page_id) |
						static_cast<uint8_t>(deletedEntityPropertyIds.contains(location.key.directoryId())) |
						static_cast<uint8_t>(relocatedPropertySources.contains(location.key.directoryId())) |
						static_cast<uint8_t>(!touchedPropertyKeys.insert(location.key.directoryId()).second) |
						static_cast<uint8_t>(
								!publishedPropertySlots.emplace(location.page_id.value(), location.slot_id).second);
				if (invalidLocation != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph page transaction has an invalid or conflicting Property directory upsert");
				}
				upsertedEntityProperties.push_back(location);
			}
			for (const EntityPropertyKey &key: request.erased_properties) {
				const std::optional<EntityPropertyLocation> existing =
						findExistingProperty(key);
				const uint8_t invalidErase =
						static_cast<uint8_t>(!key.isValid()) | static_cast<uint8_t>(!existing.has_value()) |
						static_cast<uint8_t>(existing.has_value() &&
											 existing->page_id != request.expected_location.page_id) |
						static_cast<uint8_t>(relocatedPropertySources.contains(key.directoryId())) |
						static_cast<uint8_t>(!touchedPropertyKeys.insert(key.directoryId()).second);
				if (invalidErase != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph page transaction has an invalid or conflicting Property directory deletion");
				}
				explicitlyErasedEntityProperties.push_back(key);
			}
			for (const EntityPropertyKey &key: request.relocated_properties) {
				const std::optional<EntityPropertyLocation> existing =
						findExistingProperty(key);
				const uint64_t directoryId = key.isValid() ? key.directoryId() : 0U;
				const bool invalidRelocation = // property relocation entries are authenticated against the current snapshot before planning.
						!key.isValid() || !ownerSurvivesTransaction(key) || !existing.has_value() ||
						(existing.has_value() && existing->page_id != request.expected_location.page_id) ||
						deletedEntityPropertyIds.contains(directoryId) || touchedPropertyKeys.contains(directoryId) ||
						relocatedPropertySources.contains(directoryId);
				if (invalidRelocation) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph page transaction has an invalid or conflicting Property relocation source");
				}
				relocatedPropertySources.emplace(directoryId, *existing);
			}
			propertyMaterializations.push_back(
					{visible, currentLocation,
					 request.retire_page
							 ? PageLocation{}
							 : PageLocation{currentLocation.page_id, visible.segment_id, 0U, visible.generation, 0U},
					 request.expected_page_owner_id, request.expected_owner_lsn, request.retire_page, false, false});
			propertyReplacementNeedsFreshPool.push_back(false);
		}
		std::set<uint64_t> matchedPropertyRelocations;
		std::map<uint64_t, size_t> propertyReplacementFramesByPool;
		std::map<uint64_t, size_t> remainingReplacementFramesByPool;
		for (size_t ordinal = 0U; ordinal < propertyReplacements.size(); ++ordinal) {
			const NativeGraphPropertyPageReplacementRequest &request = propertyReplacements[ordinal];
			if (request.retire_page) {
				continue;
			}
			const bool hasFirstFreshPoolSlot = request.first_slot.isValid();
			const bool hasLastFreshPoolSlot = request.last_slot.isValid();
			if (hasFirstFreshPoolSlot != hasLastFreshPoolSlot ||
				(hasFirstFreshPoolSlot && request.first_slot > request.last_slot)) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"native graph Property replacement has an incomplete fresh-pool reservation range");
			}
			if (!hasFirstFreshPoolSlot) {
				// The metadata-only transaction API deliberately permits a caller to
				// materialize its own destination frame. It does not promise that the
				// callback supplied a durable shared-pool image, so envelope inspection
				// here would turn a route-publication test seam into a physical I/O path.
				// Production PropertyCollectionRootStore always supplies this range and
				// therefore always uses the checked physical-capacity path below.
				continue;
			}
			const uint64_t segmentId = request.expected_pool.segment_id.value();
			auto [iterator, inserted] = remainingReplacementFramesByPool.try_emplace(
					segmentId, SharedPagePoolAllocator::availableFrameCountUnderWrite(*io_, current.data_region_offset,
																					  request.expected_pool));
			static_cast<void>(inserted);
			if (iterator->second != 0U) {
				--iterator->second;
				++propertyReplacementFramesByPool[segmentId];
			} else {
				// The old directory generation may still resolve every source frame.
				// Reserve a fresh compatible pool below rather than overwriting a
				// snapshot-visible frame or failing a normal graph mutation.
				propertyReplacementNeedsFreshPool[ordinal] = true;
			}
		}
		std::set<uint64_t> appendedPropertyPools;
		for (const NativeGraphPropertyPageBatchRequest &request: propertyPageBatchRequests) {
			const bool appendsExisting = request.kind == NativeGraphPropertyPageBatchKind::NGPBK_APPEND_EXISTING_POOL;
			const bool createsPool = request.kind == NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL;
			const bool emptyExpectedPool =
					!request.expected_pool.segment_id.isValid() && !request.expected_pool.physical_slot.isValid() &&
					request.expected_pool.kind == SegmentKind::SGK_NONE && request.expected_pool.owner_id == 0U &&
					!request.expected_pool.generation.isValid() &&
					request.expected_pool.state == SegmentState::SGS_FREE &&
					!request.expected_pool.committed_lsn.isValid();
			const uint8_t invalidRequest =
					static_cast<uint8_t>(!appendsExisting && !createsPool) |
					static_cast<uint8_t>(request.page_owner_id == 0U) |
					static_cast<uint8_t>(request.page_property_keys.empty()) |
					static_cast<uint8_t>(request.page_property_keys.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(appendsExisting &&
										 (request.first_slot.isValid() || request.last_slot.isValid())) |
					static_cast<uint8_t>(createsPool && !emptyExpectedPool) |
					static_cast<uint8_t>(createsPool && !request.first_slot.isValid()) |
					static_cast<uint8_t>(createsPool && !request.last_slot.isValid()) |
					static_cast<uint8_t>(createsPool && request.first_slot > request.last_slot);
			if (invalidRequest != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"native graph Property page batch has an invalid kind, owner, pool, slot range, or page set");
			}
			if (appendsExisting) {
				const CatalogEntry visible = catalog_.find(current, request.expected_pool.segment_id);
				const size_t replacementFrames = propertyReplacementFramesByPool[visible.segment_id.value()];
				const size_t availableFrames = SharedPagePoolAllocator::availableFrameCountUnderWrite(
						*io_, current.data_region_offset, visible);
				const bool invalidCapacity = request.page_property_keys.size() > availableFrames ||
											 replacementFrames > availableFrames - request.page_property_keys.size();
				const uint8_t invalidPool =
						static_cast<uint8_t>(!hasSameCatalogEntry(visible, request.expected_pool)) |
						static_cast<uint8_t>(visible.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
						static_cast<uint8_t>(visible.owner_id != request.page_owner_id) |
						static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot)) |
						static_cast<uint8_t>(invalidCapacity) |
						static_cast<uint8_t>(!appendedPropertyPools.insert(visible.segment_id.value()).second);
				if (invalidPool != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph Property append batch has a stale, duplicate, or capacity-exhausted pool");
				}
			}
			uint64_t previousKey = 0U;
			for (const std::vector<EntityPropertyKey> &pageKeys: request.page_property_keys) {
				if (pageKeys.empty()) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "native graph Property page batch contains an empty page");
				}
				for (const EntityPropertyKey &key: pageKeys) {
					const std::optional<EntityPropertyLocation> existing =
							findExistingProperty(key);
					const uint64_t directoryId = key.isValid() ? key.directoryId() : 0U;
					const auto relocation = relocatedPropertySources.find(directoryId);
					const bool relocatesExisting = existing.has_value();
					const bool invalidKey =
							!key.isValid() || !ownerSurvivesTransaction(key) || directoryId <= previousKey ||
							deletedEntityPropertyIds.contains(directoryId) ||
							(relocatesExisting &&
							 (relocation == relocatedPropertySources.end() || relocation->second != *existing)) ||
							(!relocatesExisting && relocation != relocatedPropertySources.end());
					if (invalidKey || !touchedPropertyKeys.insert(directoryId).second ||
						(relocatesExisting && !matchedPropertyRelocations.insert(directoryId).second)) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph Property page batch has an unmatched, unordered, or "
											  "conflicting owner key");
					}
					previousKey = directoryId;
				}
			}
		}
		if (matchedPropertyRelocations.size() != relocatedPropertySources.size()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph Property relocation is missing its destination root publication");
		}

		std::ranges::sort(insertedNodes, {}, &EntityPageLocation::entity_id);
		std::ranges::sort(erasedNodes);
		std::ranges::sort(insertedEdges, {}, &EntityPageLocation::entity_id);
		std::ranges::sort(erasedEdges);
		const auto validateLeaseRanges = [](std::span<const NativeGraphLogicalIdLeaseRange> ranges,
											uint64_t currentHighWater, const char *entityName) {
			uint64_t previousLast = 0U;
			for (const NativeGraphLogicalIdLeaseRange &range: ranges) {
				if (range.first_id == 0U || range.count == 0U ||
					range.count - 1U > std::numeric_limits<uint64_t>::max() - range.first_id) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  std::string("native graph ") + entityName +
												  " lease range is invalid or overflows the logical ID space");
				}
				const uint64_t last = range.first_id + range.count - 1U;
				if ((previousLast != 0U && range.first_id <= previousLast) ||
					(currentHighWater != 0U && last >= currentHighWater)) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							std::string("native graph ") + entityName +
									" lease range is not durably reserved by the current high-water mark");
				}
				previousLast = last;
			}
		};
		const auto validateLeasedInsertions = [](std::span<const EntityPageLocation> inserted,
												 std::span<const NativeGraphLogicalIdLeaseRange> ranges,
												 uint64_t currentHighWater, const char *entityName) {
			if (inserted.empty()) {
				return;
			}
			// Lower typed-store callers that do not use the graph planner retain the
			// original contiguous high-water reservation path. They can never insert a
			// stale ID below the current high-water mark; planner transactions always
			// provide durable ranges to support later rebases.
			if (ranges.empty()) {
				if (currentHighWater == 0U ||
					std::ranges::any_of(inserted, [currentHighWater](const EntityPageLocation &location) {
						return location.entity_id < currentHighWater;
					})) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  std::string("native graph ") + entityName +
												  " insertion lies below the current logical-ID high-water mark");
				}
				return;
			}
			if (std::ranges::any_of(inserted, [&ranges](const EntityPageLocation &location) {
					return !std::ranges::any_of(ranges, [&location](const NativeGraphLogicalIdLeaseRange &range) {
						return range.contains(location.entity_id);
					});
				})) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  std::string("native graph ") + entityName +
											  " insertion lies outside its durable logical-ID lease");
			}
		};
		validateLeaseRanges(logicalIdReservation.node_ranges, current.next_logical_node_id, "Node");
		validateLeaseRanges(logicalIdReservation.edge_ranges, current.next_logical_edge_id, "Edge");
		validateLeasedInsertions(insertedNodes, logicalIdReservation.node_ranges, current.next_logical_node_id, "Node");
		validateLeasedInsertions(insertedEdges, logicalIdReservation.edge_ranges, current.next_logical_edge_id, "Edge");
		// Entity-keyed Property roots are part of an Entity's logical lifetime,
		// not a second independently visible object. Remove their directory entries
		// with the Entity Map deletion so a new snapshot cannot resolve Properties
		// for an Entity that is already absent. The old Property-directory root
		// remains reachable through existing snapshot pins until normal reclamation.
		std::vector<EntityPropertyKey> erasedEntityProperties;
		erasedEntityProperties.reserve(erasedNodes.size() + erasedEdges.size());
		std::set<uint64_t> explicitlyErasedEntityPropertyIds;
		for (const EntityPropertyKey &key: explicitlyErasedEntityProperties) {
			if (!explicitlyErasedEntityPropertyIds.insert(key.directoryId()).second) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph page transaction has duplicate explicit Entity Property deletion");
			}
		}
		const auto collectErasedEntityProperties = [&findExistingProperty, &touchedPropertyKeys, &erasedEntityProperties,
																								&explicitlyErasedEntityPropertyIds](
														   std::span<const uint64_t> entityIds,
														   EntityPropertyOwnerKind ownerKind) {
			for (const uint64_t entityId: entityIds) {
				const EntityPropertyKey key{ownerKind, entityId};
				if (key.isValid() && findExistingProperty(key).has_value()) {
					if (explicitlyErasedEntityPropertyIds.contains(key.directoryId())) {
						// The matching Property-page COW has already claimed this
						// directory erase, so do not emit a duplicate deletion.
						continue;
					}
					if (!touchedPropertyKeys.insert(key.directoryId()).second) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph page transaction has overlapping Entity Property deletion");
					}
					erasedEntityProperties.push_back(key);
				}
			}
		};
		collectErasedEntityProperties(erasedNodes, EntityPropertyOwnerKind::EPOK_NODE);
		collectErasedEntityProperties(erasedEdges, EntityPropertyOwnerKind::EPOK_EDGE);
		std::ranges::sort(erasedEntityProperties, {}, &EntityPropertyKey::directoryId);
        // Only the sealed graph/field coordinator consumes this cold-aware
        // source view. Generic mutation group lookup stays guarded elsewhere.
        const auto topologyGroup = [&](EntityPropertyKey owner, const std::function<void(size_t)> &admission,
            SlottedSlotReference *reference = nullptr) {
            return topologyFields->findGroup(owner, admission, reference, true, true);
        };
        std::array<EntityPropertyKey, 64> pendingNativeOwners{};
        size_t pendingNativeOwnerCount = 0;
        if (preservingNativeFields) {
            utils::OwnedHeapReservation boundary(utils::OwnedAllocationScope::owner());
            const auto independent = [&](EntityPropertyKey owner) {
                if (topologyGroup(owner, [&](size_t bytes) { boundary.set(bytes); }))
                    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                        "native group owner insertion/deletion/property mutation requires its field lifecycle coordinator");
                boundary.set(0);
            };
            for (const auto &owner : hybridTopology->property_owners_) independent(owner);
            const auto inserted = [&](EntityPropertyKey owner) {
                const auto group = topologyGroup(owner, [&](size_t bytes) { boundary.set(bytes); }); boundary.set(0);
                if (!group) return;
                if (pendingNativeOwnerCount == pendingNativeOwners.size() ||
                    (group->live & (UINT64_C(1) << hybridPropertySlotFor(owner))))
                    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native creation requires one bounded absent owner set");
                pendingNativeOwners[pendingNativeOwnerCount++] = owner;
            };
            for (const auto &owner : insertedNodes) inserted({EntityPropertyOwnerKind::EPOK_NODE, owner.entity_id});
            for (const auto &owner : insertedEdges) inserted({EntityPropertyOwnerKind::EPOK_EDGE, owner.entity_id});
            std::array<EntityPropertyKey, 64> nativeOwners = pendingNativeOwners; size_t nativeOwnerCount = pendingNativeOwnerCount;
            const auto accountOwner = [&](EntityPropertyKey owner) {
                const auto selected = std::span(nativeOwners).first(nativeOwnerCount);
                if (std::ranges::find(selected, owner) != selected.end()) return;
                if (nativeOwnerCount == nativeOwners.size())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "native logical lifecycle exceeds64distinct affected owners");
                nativeOwners[nativeOwnerCount++] = owner;
            };
            for (const auto owner : hybridTopology->owner_deletions_) accountOwner(owner);
            for (const auto &cell : hybridTopology->field_changes_) {
                accountOwner(cell.owner);
                const bool pending = std::ranges::find(std::span(pendingNativeOwners).first(pendingNativeOwnerCount), cell.owner) !=
                    std::span(pendingNativeOwners).first(pendingNativeOwnerCount).end();
                const auto group = topologyGroup(cell.owner, [&](size_t bytes) { boundary.set(bytes); }); boundary.set(0);
                if (!cell.token || !group || (pending ? cell.erase : !(group->live & (UINT64_C(1) << hybridPropertySlotFor(cell.owner)))))
                    throw FormatException(FormatError::FERR_INVALID_STATE, "native graph field has no authenticated live or pending owner");
            }
            for (size_t i = 0; i < hybridTopology->owner_deletions_.size(); ++i) {
                const auto owner = hybridTopology->owner_deletions_[i];
                const auto group = topologyGroup(owner, [&](size_t bytes) { boundary.set(bytes); }); boundary.set(0);
                const auto &erased = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? erasedNodes : erasedEdges;
                if (!owner.isValid() || !std::ranges::binary_search(erased, owner.entity_id) || !group ||
                    !(group->live & (UINT64_C(1) << hybridPropertySlotFor(owner))) ||
                    std::ranges::find(hybridTopology->owner_deletions_.first(i), owner) != hybridTopology->owner_deletions_.first(i).end())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "native layout deletion has no unique live Entity removal");
            }
            const auto erased = [&](EntityPropertyKey owner) {
                if (std::ranges::find(hybridTopology->owner_deletions_, owner) == hybridTopology->owner_deletions_.end()) independent(owner);
            };
            for (const auto owner : erasedNodes) erased({EntityPropertyOwnerKind::EPOK_NODE, owner});
            for (const auto owner : erasedEdges) erased({EntityPropertyOwnerKind::EPOK_EDGE, owner});
            // Inline Property rewrites are represented in the sealed owner's
            // list even when they do not create a ROW directory entry.
            for (const auto key : touchedPropertyKeys) independent(EntityPropertyKey::fromDirectoryId(key));
        }
        const bool nativeScanWork = RSS_SCAN_PATH_FORMAT_ENABLED && hybridTopology && !hybridTopology->scan_changes_.empty();
        const bool nativeFieldWork = pendingNativeOwnerCount || (hybridTopology &&
            (!hybridTopology->field_changes_.empty() || !hybridTopology->owner_deletions_.empty()));
        // A Node-only scan transition creates its source account here. Prepay
        // it from the caller; declaration order keeps that credit until every
        // child buffer and the metadata batch have been destroyed.
        std::optional<utils::OwnedHeapReservation> scanSourceCredit;
        if (nativeScanWork && !hybridTopology->fields_memory_) {
            scanSourceCredit.emplace(utils::OwnedAllocationScope::owner(), 1024U * 1024U);
            if (!scanSourceCredit->governed())
                throw std::invalid_argument("scan graph mutation needs an admitted caller source grant");
        }
        std::shared_ptr<std::pmr::memory_resource> pendingNativeMemory = nativeFieldWork || nativeScanWork ?
            (hybridTopology->fields_memory_ ? hybridTopology->fields_memory_ :
                std::make_shared<query::execution::MaterializedByteBudget>(1024U * 1024U - (nativeScanWork ? 4096U : 0U))) : nullptr;
        MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn,
            pendingNativeMemory ? pendingNativeMemory : hybridVectors ? hybridVectors->fields_.memory : hybridPropertyRootMutation && hybridPropertyRootMutation->conversion ?
                hybridPropertyRootMutation->conversion->memory : std::shared_ptr<std::pmr::memory_resource>{});
        // Caller-assigned logical identities must precede every private image
        // allocation, even though their physical pools are materialized later.
        for (const auto pageId : pageIds) batch.observeLogicalPageId(PageId(pageId));
        for (const auto &request : newPoolRequests) batch.observeLogicalSegmentId(request.reservation.segment_id);
        for (const auto &[id, destination] : freshReplacementPools) batch.observeLogicalSegmentId(destination.segment_id);
        // ROW references were assigned from this source before field planning.
        // Reserve their exact range once; native proposals follow it, and both
        // sets feed the metadata-owned content-index preparation below.
        const auto jointRowLargeFirst = jointNativeContent ? batch.allocateLargeValueIdRange(largeValues.size()) : LargeValueId{};
        std::optional<MetadataSnapshotPin> hybridSource;
        std::unique_ptr<HybridPropertyPoolPlacement> hybridPlacement;
        std::unique_ptr<HybridPropertyPoolPlacement> streamedAdjacencyPlacement;
        std::unique_ptr<HybridPropertyPoolPlacement> streamedMembershipPlacement;
        std::optional<MetadataSnapshotPin> ordinaryIndexSource;
        std::unique_ptr<HybridPropertyPoolPlacement> ordinaryIndexPlacement;

        if (hybridVectors) {
            const auto &fields = hybridVectors->fields_;
            hybridSource.emplace(snapshots_.pinCurrentUnderWrite(writer, current));
            hybridPlacement.reset(new HybridPropertyPoolPlacement(*this, batch, writer, *hybridSource,
                fields.first_slot, fields.last_slot, fields.memory, [checkpoint = fields.cancellation] { if (checkpoint) checkpoint->check(); }));
            if (!prepareHybridPropertyColumns(batch, writer, *hybridSource, fields, fields.replacements,
                *conversionPages, *conversionIndexes, *conversionLargeValues, *conversionEntities, *hybridLabels, *hybridPlacement, hybridVectors))
                throw FormatException(FormatError::FERR_INVALID_STATE, "native Vector publication has no authoritative field transition");
        }
        std::unique_ptr<PreparedScanMembershipMutation> scanMembership;
        if (nativeScanWork) scanMembership = prepareScanMembershipMutation(batch, writer, *topologySource,
            *hybridTopology, pendingNativeMemory, *conversionEntities, *hybridLabels, *conversionIndexes);
        bool nativeFieldsPrepared = false;
        if (nativeFieldWork) {
            utils::OwnedAllocationScope pendingScope(pendingNativeMemory);
            SlottedSlotReference groupReference; utils::OwnedHeapReservation boundary(pendingNativeMemory);
            if (!topologyGroup(pendingNativeOwnerCount ? pendingNativeOwners[0] : !hybridTopology->field_changes_.empty() ?
                hybridTopology->field_changes_.front().owner : hybridTopology->owner_deletions_.front(), [&](size_t bytes) { boundary.set(bytes); }, &groupReference))
                throw FormatException(FormatError::FERR_INVALID_STATE, "native pending owner lost its source group");
            boundary.set(0);
            const auto groupPage = locations_.findPage(current, groupReference.page_id);
            const auto fieldPool = catalog_.find(current, groupPage.segment_id);
            hybridPlacement.reset(new HybridPropertyPoolPlacement(*this, batch, writer, *topologySource,
                PhysicalSlotId(1), PhysicalSlotId(UINT64_MAX), pendingNativeMemory, [] {}));
            NativeHybridPropertyColumnWrite fields{current.database_id, current.generation,
                current.hybrid_property_descriptor_root, hybridTopology->field_changes_, pendingNativeMemory, PhysicalSlotId(1), PhysicalSlotId(UINT64_MAX),
                fieldPool.owner_id, nullptr, true, hybridTopology->owner_deletions_, std::span(pendingNativeOwners).first(pendingNativeOwnerCount)};
            nativeFieldsPrepared = prepareHybridPropertyColumns(batch, writer, *topologySource, fields, fields.replacements, *conversionPages,
                *conversionIndexes, *conversionLargeValues, *conversionEntities, *hybridLabels, *hybridPlacement, nullptr, hybridTopology, jointNativeContent ? largeValues : std::span<const NativeGraphLargeValueMutation>{});
            if (!nativeFieldsPrepared && pendingNativeOwnerCount)
                throw FormatException(FormatError::FERR_INVALID_STATE, "native pending owner liveness did not prepare a transition");
        }
        if (reclamationEvidence) batch.hybrid_property_publication_ = true;
        if (hybridPropertyRootMutation && hybridPropertyRootMutation->conversion) {
            const auto &conversion = *hybridPropertyRootMutation->conversion;
            const std::function<void()> checkpoint = [&conversion] {
                if (conversion.cancellation) conversion.cancellation->check();
            };
            // Declare the decode admission before all borrowed ROW values:
            // decoded heaps and vector backing die before their credit refunds.
            utils::OwnedAllocationScope allocationScope(conversion.memory);
            std::optional<detail::PropertyDecodeWorkspace> headerWorkspace;
            headerWorkspace.emplace(conversion.memory, &checkpoint);
            auto source = snapshots_.pinCurrentUnderWrite(writer, current);
            if (conversion.direction == NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW)
                return publishHybridPropertyRows(batch, writer, source, conversion, *conversionEntities,
                    *conversionPages, *conversionIndexes, *conversionLargeValues, options);
            if (current.hybrid_property_descriptor_root.isValid()) {
                HybridPropertyReadContext existing(io_, *conversionPages, *this, source, conversion.cancellation);
                for (uint64_t offset = 0; offset < conversion.group_count; ++offset)
                    if (existing.groupRecord({conversion.owner_kind, (conversion.group_id + offset - 1U) * 64U + 1U}))
                        throw FormatException(FormatError::FERR_INVALID_STATE, "ROW conversion group is already native");
            }
            std::optional<NativeStorageAllocationCensus> allocationCensus;
            std::optional<NativeStorageAllocationFootprint> sourceCost;
            NativeStorageAllocationCensusOptions censusOptions;
            if (conversion.allocation_admission) {
                censusOptions = conversion.allocation_admission->census;
                censusOptions.cancellation = conversion.cancellation;
                allocationCensus.emplace(io_, *this, conversion.memory);
                try { sourceCost = allocationCensus->inspectCurrentUnderWrite(writer, source, censusOptions); }
                catch (const query::execution::MaterializedByteLimitExceeded &) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_unknown", 1); return current;
                }
                if (!sourceCost || !sourceCost->fullyAttributed()) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_unknown", 1); return current;
                }
                const auto &limits = *conversion.allocation_admission;
                if (sourceCost->totalAllocatedBytes() > limits.maximum_admitted_peak_bytes || sourceCost->file_bytes > limits.maximum_admitted_file_bytes) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_resource_refusal", 1); return current;
                }
            }
            struct SourceOwner { EntityPropertyKey key; size_t fields; };
            auto owners = utils::OwnedVector<SourceOwner>(utils::OwnedPmrAllocator<SourceOwner>(conversion.memory));
            owners.reserve(size_t(conversion.group_count) * 64U);
            size_t memberships = 0;
            const uint64_t firstId = (conversion.group_id - 1U) * 64U + 1U;
            for (uint64_t slot = 0; slot < uint64_t(conversion.group_count) * 64U; ++slot) {
                checkpoint();
                if (slot % 64U == 0) memberships = 0;
                const auto id = firstId + slot;
                if (id > RSS_HYBRID_MAX_ENTITY_ID) break;
                const bool node = conversion.owner_kind == EntityPropertyOwnerKind::EPOK_NODE;
                const auto location = node ? locations_.findNodePageLocationOptional(current, id) :
                    locations_.findEdgePageLocationOptional(current, id);
                if (!location) continue;
                if (node) static_cast<void>(conversionEntities->readNodeAt(source, *location));
                else static_cast<void>(conversionEntities->readEdgeAt(source, *location));
                const EntityPropertyKey key{conversion.owner_kind, id};
                // Admit the fixed work bound before decoding any values from an
                // over-wide source. Count lookup authenticates owner framing.
                const auto count = conversionRoots->readEntityPropertyCount(source, key).value_or(0);
                if (count > HybridPropertyGroupConversion::MAX_MEMBERSHIPS - memberships)
                    throw std::length_error("native hybrid conversion exceeds its membership bound");
                memberships += count;
                owners.push_back({key, count});
            }
            if (owners.empty())
                throw FormatException(FormatError::FERR_INVALID_STATE, "native hybrid conversion group has no live owners");
            // Header-only temporaries have died. Decode/classify/encode ROW
            // values in a separate lifetime: field images and immutable value
            // proposals own their bytes and borrow no input after preparation.
            // Releasing source values before index/physical planning keeps the
            // documented maximal group inside the same 1 MiB account.
            headerWorkspace.reset();
            auto largeFields = utils::OwnedVector<HybridPropertyLargeFieldPreparation>(
                utils::OwnedPmrAllocator<HybridPropertyLargeFieldPreparation>(conversion.memory));
            std::shared_ptr<HybridPropertyConversionPreparation> fields;
            for (uint64_t groupOffset = 0; groupOffset < conversion.group_count; ++groupOffset) {
            const auto groupId = conversion.group_id + groupOffset;
            auto preparedGroup = [&] {
                detail::PropertyDecodeWorkspace sourceWorkspace(conversion.memory, &checkpoint);
                // Keep decoded values admitted for the complete source lifetime,
                // but release per-owner LargeValue wire buffers after decoding.
                // Without explicit slot admission those transient bodies join
                // the retained decoder ledger once per owner.
                size_t retainedSlotBytes = 0;
                utils::OwnedHeapReservation sourceSlotCredit(conversion.memory);
                const SelectedPropertyReadAdmission admitSourceSlots = [&](size_t bytes) {
                    constexpr size_t frontierBytes = 4096U;
                    if (bytes > (std::numeric_limits<size_t>::max)() - retainedSlotBytes - frontierBytes)
                        throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "native source slot admission overflows");
                    sourceSlotCredit.set(retainedSlotBytes + bytes + frontierBytes);
                };
                // Complete the header-only bound check before materializing any
                // source values. Both passes remain within the same fenced pin.
                auto roots = utils::OwnedVector<PropertyCollectionRoot>(
                    utils::OwnedPmrAllocator<PropertyCollectionRoot>(conversion.memory));
                roots.reserve(64U);
                auto cursor = conversionRoots->openScanCursor(source, conversion.cancellation);
                for (const auto &owner : owners) {
                    if ((owner.key.entity_id - 1U) / 64U + 1U != groupId) continue;
                    checkpoint();
                    auto root = cursor.readEntityOptional(source, owner.key, nullptr, &admitSourceSlots).value_or(PropertyCollectionRoot{
                        owner.key.entity_id, conversion.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId, {}});
                    if (root.entries.size() != owner.fields)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "native hybrid source count disagrees with its ROW payload");
                    retainedSlotBytes += utils::detail::standardContainerProxyBytes() +
                        utils::detail::standardAllocatorRequestBytes(root.entries.capacity() * sizeof(PropertyCollectionRootEntry),
                            alignof(PropertyCollectionRootEntry));
                    roots.push_back(std::move(root));
                    sourceSlotCredit.set(retainedSlotBytes);
                    debug::PerfTrace::addValue("native_graph.hybrid.source_owners_decoded", 1);
                }
                HybridPropertyGroupConversion plan(conversion.memory, roots, conversion.owner_kind,
                    groupId, batch.generation(), RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, checkpoint);
                if (conversion.group_count > 1U && !conversion.cold_row_remainder && (plan.columns().empty() ||
                    std::ranges::any_of(plan.members(), [](const auto &member) { return member.source != HybridPropertySource::HPS_COLUMN; })))
                    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native hybrid cohort requires compact pure-column groups");
                debug::PerfTrace::addValue("native_graph.hybrid.source_classification_completed", 1);
                // Directory sources retain their immutable final locator records,
                // rather than borrow the ROW/planner or keep a second row copy.
                const auto retainProposal = [&](HybridPropertyLargeFieldPreparation proposal) -> const HybridPropertyLargeFieldPreparation & {
                    const HybridPropertyLargeFieldPreparation *selected = &proposal;
                    // Content created earlier in this same private batch is absent
                    // from the published index. Compare the complete carrier bytes
                    // before allocating another ID; hash equality alone is not enough.
                    // Shared existing values also need one retained proposal
                    // per complete payload, rather than one copy per owner.
                    for (const auto &previous : largeFields) {
                        checkpoint();
                        if (std::ranges::equal(previous.payload(), proposal.payload())) {
                            selected = &previous; break;
                        }
                    }
                    if (selected == &proposal) {
                        // Reserve the holder before consuming a batch-local ID.
                        if (largeFields.size() == largeFields.capacity())
                            largeFields.reserve(largeFields.empty() ? 1U : largeFields.size() * 2U);
                        if (!proposal.reused() && batch.allocateLargeValueId() != proposal.reference().value_id)
                            throw FormatException(FormatError::FERR_INVALID_STATE, "native field value lost its batch ID sequence");
                        largeFields.push_back(std::move(proposal)); selected = &largeFields.back();
                    }
                    return *selected;
                };
                const auto resolveLarge = [&](EntityPropertyKey owner, uint32_t token, const PropertyValue &value) {
                    checkpoint();
                    auto proposed = HybridPropertyLargeFieldPreparation::prepare(*this, source, *conversionLargeValues,
                        owner, token, value, LargeValueId(batch.candidate().next_large_value_id), conversion.memory,
                        {}, LargeValueExtentClass::LVEC_32K, RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES, conversion.cancellation);
                    debug::PerfTrace::addValue("native_graph.hybrid.large_field_proposals_prepared", 1);
                    const auto *selected = &retainProposal(std::move(proposed));
                    const size_t encodedBytes = RSS_LARGE_VALUE_REF_HEADER_BYTES + selected->reference().media_type.size();
                    utils::OwnedHeapReservation encodingCredit(conversion.memory,
                        utils::detail::standardContainerProxyBytes() +
                        utils::detail::standardAllocatorRequestBytes(encodedBytes, alignof(std::byte)));
                    const auto encoded = LargeValueRefCodec::encode(selected->reference());
                    utils::OwnedHeapReservation decodedCredit(conversion.memory);
                    const auto admitReference = [&](size_t bytes) { decodedCredit.set(bytes); };
                    return HybridPropertyExtensionView::encodeReference(owner, token, batch.generation(), encoded,
                        RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, *conversion.memory, std::ref(admitReference));
                };
                const auto resolveRoot = [&](EntityPropertyKey owner, std::span<const BorrowedPropertyCollectionRootEntry> entries) {
                    checkpoint();
                    const auto &proposal = retainProposal(HybridPropertyLargeFieldPreparation::prepareRootBorrowed(*this, source,
                        *conversionLargeValues, owner, entries, LargeValueId(batch.candidate().next_large_value_id),
                        conversion.memory, conversion.cancellation));
                    debug::PerfTrace::addValue("native_graph.hybrid.large_root_proposals_prepared", 1);
                    // The existing owner-keyed codec has two bounded standard
                    // buffers; charge their full supported media/proxy overlap.
                    constexpr size_t maximumReference = RSS_LARGE_VALUE_REF_HEADER_BYTES + RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES;
                    utils::OwnedHeapReservation credit(conversion.memory,
                        2U * utils::detail::standardContainerProxyBytes() +
                        utils::detail::standardAllocatorRequestBytes(maximumReference, alignof(std::byte)) +
                        utils::detail::standardAllocatorRequestBytes(RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES +
                            maximumReference, alignof(std::byte)));
                    const auto type = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
                    const auto encoded = EntityPropertyLargeValueReferenceCodec::encode({owner.entity_id, type, proposal.reference()});
                    return std::pmr::vector<std::byte>(encoded.begin(), encoded.end(), conversion.memory.get());
                };
                const HybridPropertyConversionPreparation::LargeFieldResolver largeResolver = std::ref(resolveLarge);
                const HybridPropertyConversionPreparation::WholeRootResolver rootResolver = std::ref(resolveRoot);
                return utils::shareOwned(std::unique_ptr<HybridPropertyConversionPreparation>(
                    new HybridPropertyConversionPreparation(batch, plan, checkpoint, largeResolver,
                        conversion.compact_column_memberships, conversion.cold_row_remainder, rootResolver)));
            }();
            if (!fields) fields = std::move(preparedGroup);
            else {
                const auto append = [](auto &target, auto &part) {
                    target.reserve(target.size() + part.size());
                    target.insert(target.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
                };
                append(fields->pages_, preparedGroup->pages_);
                append(fields->owners_, preparedGroup->owners_);
                append(fields->columns_, preparedGroup->columns_);
                append(fields->extensions_, preparedGroup->extensions_);
                append(fields->groups_, preparedGroup->groups_);
            }
            }
            const auto sourceFor = [fields, memory = conversion.memory, checkpoint]<class Map>(
                std::span<const typename Map::Entry> entries) -> OrderedBTreeEntrySource {
                return [fields, memory, entries, checkpoint]() -> OrderedBTreeEntryCursor {
                    utils::OwnedAllocationScope sourceScope(memory);
                    return [fields, memory, entries, checkpoint, position = size_t{0}]() mutable -> std::optional<BTreePageEntry> {
                        utils::OwnedAllocationScope cursorScope(memory);
                        checkpoint();
                        if (position == entries.size()) return {};
                        const auto &entry = entries[position++];
                        const auto record = Map::encode(entry.key, entry.value);
                        BTreePageEntry result;
                        result.key.assign(record.begin(), record.end());
                        return result;
                    };
                };
            };
            using PreparedIndexImage = HybridPropertyContentIndexPreparation::Image;
            auto indexImages = utils::makeSharedOwned<utils::OwnedVector<PreparedIndexImage>>(
                utils::OwnedPmrAllocator<PreparedIndexImage>(conversion.memory));
            const auto collectIndexImages = [indexImages, checkpoint](const CatalogEntry &pool,
                std::span<BTreePage> images, std::span<PageLocation> locations, LogSequenceNumber lsn) {
                if (images.size() != locations.size() || images.empty() || pool.committed_lsn != lsn)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "private hybrid index images lost their placement");
                for (size_t i = 0; i < images.size(); ++i) {
                    checkpoint();
                    const auto header = images[i].header();
                    if (header.index_id != pool.owner_id || header.page_id != locations[i].page_id ||
                        header.generation != pool.generation || locations[i].segment_id != pool.segment_id ||
                        locations[i].page_index != 0 || images[i].slottedPage().ownerCommittedLsn() != lsn)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "private hybrid index image changed its assigned identity");
                    locations[i].page_index = uint16_t(i + RSS_PAGE_POOL_FIRST_DATA_FRAME);
                    locations[i].free_bytes = images[i].slottedPage().stats().contiguous_free_bytes;
                    indexImages->push_back({pool, locations[i], std::move(images[i])});
                }
            };
            std::array<std::optional<BTreeInitialTreeMutationPlan>, 4> directoryPlans;
            const size_t namespaceBase = conversion.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? 0U : 4U;
            // Select real, disjoint allocator-free destinations under the same
            // writer, but do not write or mark any pool reachable before complete
            // cost/lifecycle admission. This local cursor is monotonic across
            // field and Index pools; it cannot reuse another private destination.
            auto pools = utils::OwnedVector<CatalogEntry>(utils::OwnedPmrAllocator<CatalogEntry>(conversion.memory));
            HybridPropertyPoolPlacement placement(*this, batch, writer, source, conversion.first_slot,
                conversion.last_slot, conversion.memory, checkpoint);
            // Two directly neighbouring groups provide a bounded cold-open
            // hint. The descriptor/map/body and actual Catalog route remain
            // authoritative; no database-wide pool search is needed.
            if (current.hybrid_property_descriptor_root.isValid()) {
                const auto prior = pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current).descriptor;
                if (prior.roots[namespaceBase + 3U].page_id.isValid()) {
                    const auto binding = HybridPropertyDirectoryCodec::bindRoot(prior, source,
                        static_cast<HybridPropertyRootOrdinal>(namespaceBase + 3U));
                    HybridPropertyGroupDirectoryMap groups(binding.root.index_id, *conversionIndexes, *this, binding);
                    HybridPropertyReadContext reader(io_, *conversionPages, *this, source);
                    utils::OwnedHeapReservation boundary(conversion.memory);
                    for (const auto neighbor : {conversion.group_id > 1U ? conversion.group_id - 1U : conversion.group_id,
                        conversion.group_id < RSS_HYBRID_MAX_GROUP_ID ? conversion.group_id + 1U : conversion.group_id}) {
                        if (neighbor == conversion.group_id) continue;
                        const auto locator = groups.find(source, HybridPropertyDirectoryCodec::groupKey(neighbor),
                            [&](size_t bytes) { boundary.set(bytes); }, conversion.cancellation);
                        boundary.set(0); if (!locator) continue;
                        const auto neighborLive = reader.ownerLiveness(
                            {conversion.owner_kind, (neighbor - 1U) * 64U + 1U},
                            [&](size_t bytes) { boundary.set(bytes); });
                        hybrid_property_detail::require(neighborLive.has_value(),
                            "native neighbor pool has no authenticated group");
                        boundary.set(0);
                        const auto slot = HybridPropertyDirectoryCodec::decodeLocator(*locator);
                        placement.candidate(locations_.findPage(current, slot.page_id).segment_id,
                            SegmentKind::SGK_PROPERTY_PAGE_POOL, conversion.field_pool_owner_id, true);
                    }
                }
            }
            const auto allocatePool = [&](SegmentKind kind, uint64_t owner, size_t count = RSS_PAGE_POOL_DATA_FRAME_COUNT, bool native = false) {
                const bool forceFresh = kind == SegmentKind::SGK_INDEX_PAGE_POOL && !locations_.findIndexRootOptional(current, owner);
                const auto pool = placement.allocate(kind, owner, count, native, forceFresh);
                if (std::none_of(pools.begin(), pools.end(), [&](const auto &p) { return p.segment_id == pool.segment_id; })) pools.push_back(pool);
                return pool;
            };
            struct PreparedFieldImage { CatalogEntry pool; PageLocation location; SlottedPage page; };
            auto fieldImages = utils::OwnedVector<PreparedFieldImage>(utils::OwnedPmrAllocator<PreparedFieldImage>(conversion.memory));
            fieldImages.reserve(fields->pages_.size());
            CatalogEntry fieldPool; size_t fieldPoolUsed = 0;
            for (size_t i = 0; i < fields->pages_.size(); ++i) {
                checkpoint();
                const auto selected = allocatePool(SegmentKind::SGK_PROPERTY_PAGE_POOL, conversion.field_pool_owner_id, 1U, true);
                if (selected.segment_id != fieldPool.segment_id) fieldPoolUsed = 0;
                fieldPool = selected;
                auto &image = fields->pages_[i];
                PageOwnerHeader owner;
                owner.page_id = image.pageId(); owner.owner_id = fieldPool.owner_id;
                owner.kind = PageOwnerKind::POK_HYBRID_PROPERTY; owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
                owner.generation = fieldPool.generation; owner.committed_lsn = batch.checkpointLsn();
                utils::OwnedHeapReservation referenceCredit(conversion.memory);
                const auto admitReference = [&](size_t retained) { referenceCredit.set(retained); };
                auto bound = std::move(image).bindPrivateOwner(owner, fieldPool, batch.checkpointLsn(), std::ref(admitReference));
                const PageLocation location{owner.page_id, fieldPool.segment_id,
                    uint16_t(fieldPoolUsed++ + RSS_PAGE_POOL_FIRST_DATA_FRAME),
                    fieldPool.generation, bound.stats().contiguous_free_bytes};
                fieldImages.push_back({fieldPool, location, std::move(bound)});
            }
            HybridPropertyDescriptor descriptor = current.hybrid_property_descriptor_root.isValid() ?
                pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current).descriptor : HybridPropertyDescriptor{};
            descriptor.database_id = current.database_id;
            std::array<std::optional<IndexRootLocation>, 4> directoryRoots;
            utils::OwnedVector<uint64_t> supersededDirectories{utils::OwnedPmrAllocator<uint64_t>(conversion.memory)};
            utils::OwnedVector<uint64_t> erasedDirectories{utils::OwnedPmrAllocator<uint64_t>(conversion.memory)};
            const auto prepareDirectory = [&]<class Map>(size_t ordinal, std::span<const typename Map::Entry> entries) {
                checkpoint();
                const auto index = hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(namespaceBase + ordinal));
                const auto existing = findIndexRootOptional(source, index);
                if (existing) {
                    if (!descriptor.roots[namespaceBase + ordinal].page_id.isValid())
                        throw FormatException(FormatError::FERR_INVALID_STATE,
                            "ROW conversion found an existing hybrid directory without its descriptor binding");
                    const auto binding = HybridPropertyDirectoryCodec::bindRoot(descriptor, source,
                        static_cast<HybridPropertyRootOrdinal>(namespaceBase + ordinal));
                    if (entries.empty()) return;
                    HybridPropertyDirectoryPreparation prepared(batch, source, *this, *conversionIndexes, binding,
                        entries, std::span<const typename Map::Key>{}, conversion.memory, conversion.cancellation,
                        allocatePool, checkpoint, std::type_identity<Map>{});
                    if (!prepared.root_)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "new native group lost a shared directory");
                    directoryRoots[ordinal] = *prepared.root_;
                    descriptor.roots[namespaceBase + ordinal] = {prepared.root_->root_page_id, prepared.root_->root_generation};
                    for (auto &image : prepared.images_) indexImages->push_back({image.pool, image.location, std::move(image.page)});
                    supersededDirectories.insert(supersededDirectories.end(), prepared.superseded_.begin(), prepared.superseded_.end());
                    erasedDirectories.insert(erasedDirectories.end(), prepared.erased_.begin(), prepared.erased_.end());
                    return;
                }
                if (descriptor.roots[namespaceBase + ordinal].page_id.isValid())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "native descriptor has a missing Index Root binding");
                if (entries.empty()) return; // absent is canonical, no empty root/pool
                const BulkBTreeBuildOptions options{conversion.first_slot, conversion.last_slot, index,
                    entries.size(), entries.size(), BTreeLeafValueLayout::BLVL_EMPTY};
                directoryPlans[ordinal] = BulkBTreeBuilder::planNativeTreeWithImageSink(*conversionIndexes, *this, source,
                    index, sourceFor.template operator()<Map>(entries), options, collectIndexImages);
                if (directoryPlans[ordinal]->entry_count != entries.size())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "hybrid directory plan lost a final field locator");
            };
            fields->pages_.clear(); fields->pages_.shrink_to_fit();
            prepareDirectory.template operator()<HybridPropertyOwnerMap>(0, fields->owners());
            prepareDirectory.template operator()<HybridPropertyFieldDirectoryMap>(1, fields->columns());
            prepareDirectory.template operator()<HybridPropertyFieldDirectoryMap>(2, fields->extensions());
            prepareDirectory.template operator()<HybridPropertyGroupDirectoryMap>(3, std::span<const HybridPropertyGroupDirectoryMap::Entry>(fields->groups_));

            for (size_t ordinal = 0; ordinal < directoryPlans.size(); ++ordinal) {
                if (!directoryPlans[ordinal]) continue;
                const auto &directory = *directoryPlans[ordinal];
                // Pre-admit the exact standard-vector assignment backing and
                // five nested empty proxies before constructing the API batch.
                // These temporary assignments die before this credit refunds.
                size_t assignmentBytes = utils::detail::addStandardHeapBytes(utils::detail::standardContainerProxyBytes(),
                    utils::detail::standardAllocatorRequestBytes(directory.new_pool_mutations.size() * sizeof(IndexTreeMutationPoolBatch),
                        alignof(IndexTreeMutationPoolBatch)));
                for (const auto &request : directory.new_pool_mutations) {
                    assignmentBytes = utils::detail::addStandardHeapBytes(assignmentBytes,
                        5U * utils::detail::standardContainerProxyBytes());
                    assignmentBytes = utils::detail::addStandardHeapBytes(assignmentBytes,
                        utils::detail::standardAllocatorRequestBytes(request.appended_page_count * sizeof(PageId), alignof(PageId)));
                    assignmentBytes = utils::detail::addStandardHeapBytes(assignmentBytes,
                        utils::detail::standardAllocatorRequestBytes(request.appended_page_count * sizeof(PageLocation), alignof(PageLocation)));
                }
                utils::OwnedHeapReservation assignmentCredit(conversion.memory, assignmentBytes);
                std::vector<IndexTreeMutationPoolBatch> assignments(directory.new_pool_mutations.size());
                for (size_t i = 0; i < assignments.size(); ++i) {
                    const auto &request = directory.new_pool_mutations[i]; auto &assignment = assignments[i];
                    assignment.pool = allocatePool(SegmentKind::SGK_INDEX_PAGE_POOL, directory.index_id, request.appended_page_count);
                    assignment.replacement_pool = assignment.pool; assignment.newly_reserved = true;
                    assignment.appended_page_ids.reserve(request.appended_page_count);
                    assignment.appended_locations.reserve(request.appended_page_count);
                    if (assignment.appended_page_ids.capacity() != request.appended_page_count ||
                        assignment.appended_locations.capacity() != request.appended_page_count)
                        throw std::logic_error("private hybrid tree assignments exceeded their admitted capacity");
                    for (size_t page = 0; page < request.appended_page_count; ++page) {
                        const auto id = batch.allocateLogicalPageId();
                        assignment.appended_page_ids.push_back(id);
                        assignment.appended_locations.push_back({id, assignment.pool.segment_id, 0, assignment.pool.generation, 0});
                    }
                }
                directory.preparer(assignments, batch.checkpointLsn());
                const auto &root = assignments.at(directory.root_page.pool_ordinal);
                descriptor.roots[namespaceBase + ordinal] = {
                    root.appended_locations.at(directory.root_page.appended_page_ordinal).page_id, root.pool.generation};
                directoryRoots[ordinal] = IndexRootLocation{directory.index_id,
                    root.appended_locations.at(directory.root_page.appended_page_ordinal).page_id, root.pool.generation};
            }
            descriptor.semantic_generation = batch.generation();
            // Validate the final namespace bindings and publication identity,
            // without allocating or writing a descriptor metadata frame yet.
            static_cast<void>(HybridPropertyDescriptorCodec::encode({batch.generation(), batch.checkpointLsn(), descriptor}));
            const HybridPropertyImmutablePreparation immutableImages(batch, *conversionLargeValues,
                largeFields, conversion.memory, allocatePool, checkpoint);
            // These are private preparation counts, not committed work or total
            // conversion cost. Count every whole reserved pool, including its
            // header and unused frames; final metadata/retirement is additional.
            debug::PerfTrace::addValue("native_graph.hybrid.private_large_values", int64_t(largeFields.size()));
            size_t newLargeValues = 0;
            for (const auto &value : largeFields) newLargeValues += !value.reused();
            debug::PerfTrace::addValue("native_graph.hybrid.private_new_large_values", int64_t(newLargeValues));
            debug::PerfTrace::addValue("native_graph.hybrid.private_field_frames", int64_t(fieldImages.size()));
            debug::PerfTrace::addValue("native_graph.hybrid.private_index_frames", int64_t(indexImages->size()));
            debug::PerfTrace::addValue("native_graph.hybrid.private_large_data_frames", int64_t(immutableImages.dataFrameCount()));
            debug::PerfTrace::addValue("native_graph.hybrid.private_large_manifest_frames", int64_t(immutableImages.manifestFrameCount()));
            debug::PerfTrace::addValue("native_graph.hybrid.private_data_pool_bytes", int64_t(std::count_if(placement.reservations_.begin(), placement.reservations_.end(), [](const auto &r) { return r.fresh; }) * RSS_SEGMENT_SIZE));
            checkpoint();
            // Do not publish an empty descriptor and consumed IDs as a false
            // success. Physical placement, final maps and every graph route
            // must replace this guard together with nonempty-root admission.
            if (!conversion.publish_candidate)
                throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                    "native hybrid private field preparation completed; directory plans prepared; physical images and final roots prepared; physical publication is not installed for preparation-only requests");
            const size_t contentImageStart = indexImages->size();
            HybridPropertyContentIndexPreparation content(batch, source, conversion, *this, *conversionIndexes,
                *conversionLargeValues, largeFields, allocatePool, checkpoint);
            auto contentRoot = content.root_;
            auto supersededContentPages = std::move(content.superseded_);
            auto erasedContentPages = std::move(content.erased_);
            supersededContentPages.insert(supersededContentPages.end(), supersededDirectories.begin(), supersededDirectories.end());
            erasedContentPages.insert(erasedContentPages.end(), erasedDirectories.begin(), erasedDirectories.end());
            for (auto &image : content.images_) indexImages->push_back(std::move(image));
            debug::PerfTrace::addValue("native_graph.hybrid.private_content_index_frames",
                int64_t(indexImages->size() - contentImageStart));
            checkpoint();
            // Remove converted ROW slots from CURRENT physical page images as
            // well as their owner directory. Preserve every unrelated owner and
            // stable slot locator; old pins keep their original physical frames.
            // Group authenticated slot references before decoding pages. Fully
            // removed pages retain only their IDs; keeping their 4KiB bodies
            // until publication needlessly consumes the conversion workspace.
            utils::OwnedVector<SlottedSlotReference> rowSlots{utils::OwnedPmrAllocator<SlottedSlotReference>(conversion.memory)};
            rowSlots.reserve(owners.size());
            for (const auto &owner : owners) {
                checkpoint(); const auto location = locations_.findEntityPropertyOptional(current, owner.key);
                if (location) rowSlots.push_back({location->page_id, location->slot_id, location->slot_generation});
            }
            std::sort(rowSlots.begin(), rowSlots.end(), [](const auto &left, const auto &right) {
                return left.page_id < right.page_id || (left.page_id == right.page_id && left.slot_id < right.slot_id);
            });
            utils::OwnedVector<SlottedPage> rowPages{utils::OwnedPmrAllocator<SlottedPage>(conversion.memory)};
            utils::OwnedVector<PageId> changedRowPages{utils::OwnedPmrAllocator<PageId>(conversion.memory)};
            changedRowPages.reserve(rowSlots.size());
            utils::OwnedVector<uint64_t> erasedRowPages{utils::OwnedPmrAllocator<uint64_t>(conversion.memory)};
            erasedRowPages.reserve(rowSlots.size() + erasedContentPages.size());
            erasedRowPages.insert(erasedRowPages.end(), erasedContentPages.begin(), erasedContentPages.end());
            for (size_t begin = 0; begin < rowSlots.size();) {
                checkpoint();
                auto page = conversionPages->read(source, rowSlots[begin].page_id);
                size_t end = begin;
                while (end < rowSlots.size() && rowSlots[end].page_id == page.pageId()) {
                    checkpoint();
                    if (end > begin && rowSlots[end].slot_id == rowSlots[end - 1].slot_id)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "native conversion ROW owners alias one source slot");
                    page.erase(rowSlots[end++]);
                }
                changedRowPages.push_back(page.pageId());
                if (!page.stats().live_slot_count) erasedRowPages.push_back(page.pageId().value());
                else rowPages.push_back(std::move(page));
                begin = end;
            }
            // A pool may retire only when every currently routed frame is
            // replaced or removed by this conversion. Inspect at most31 frame
            // owners per source pool; never scan the database Page Directory.
            utils::OwnedVector<CatalogEntry> retiredRowPools{utils::OwnedPmrAllocator<CatalogEntry>(conversion.memory)};
            retiredRowPools.reserve(changedRowPages.size() + supersededContentPages.size());
            for (const auto id : supersededContentPages) {
                const auto location = locations_.findPage(current, PageId(id));
                retiredRowPools.push_back(catalog_.find(current, location.segment_id));
            }
            for (const auto id : changedRowPages) {
                const auto location = locations_.findPage(current, id);
                retiredRowPools.push_back(catalog_.find(current, location.segment_id));
            }
            std::sort(retiredRowPools.begin(), retiredRowPools.end(), [](const auto &a, const auto &b) { return a.segment_id < b.segment_id; });
            retiredRowPools.erase(std::unique(retiredRowPools.begin(), retiredRowPools.end(), [](const auto &a, const auto &b) {
                return a.segment_id == b.segment_id;
            }), retiredRowPools.end());
            SharedPagePoolAllocator sourceRowPools(io_, *this, source);
            for (auto pool = retiredRowPools.begin(); pool != retiredRowPools.end();) {
                checkpoint(); bool detached = pool->state == SegmentState::SGS_SEALED &&
                    (pool->kind == SegmentKind::SGK_PROPERTY_PAGE_POOL ||
                        (pool->kind == SegmentKind::SGK_INDEX_PAGE_POOL && (pool->owner_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID || isHybridPropertyIndexId(pool->owner_id)))) &&
                    allocator_.isAllocated(current, pool->physical_slot);
                if (detached) {
                    const auto header = SharedPagePoolAllocator::readPoolHeader(*io_, current.data_region_offset, *pool);
                    for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
                        checkpoint(); const auto owner = hybrid_property_detail::currentPoolFrameOwner(*pool, header, frame,
                            [&](uint8_t index) { return sourceRowPools.readPageOwner(*pool, index); },
                            [&](PageId id) { return locations_.findPageOptional(current, id); });
                        if (owner) detached = detached && (pool->kind == SegmentKind::SGK_PROPERTY_PAGE_POOL ?
                            std::binary_search(changedRowPages.begin(), changedRowPages.end(), owner->page_id) :
                            std::find(supersededContentPages.begin(), supersededContentPages.end(), owner->page_id.value()) != supersededContentPages.end());
                    }
                }
                if (!detached || placement.destination(pool->segment_id)) { pool = retiredRowPools.erase(pool); continue; }
                batch.reserveDataSlot(pool->physical_slot); pool->state = SegmentState::SGS_RETIRED; ++pool;
            }
            utils::OwnedVector<PreparedFieldImage> rowImages{utils::OwnedPmrAllocator<PreparedFieldImage>(conversion.memory)};
            rowImages.reserve(rowPages.size());
            CatalogEntry rowPool; size_t rowFrames = RSS_PAGE_POOL_DATA_FRAME_COUNT;
            for (auto &page : rowPages) {
                checkpoint();
                if (rowFrames == RSS_PAGE_POOL_DATA_FRAME_COUNT || rowPool.owner_id != page.ownerId()) {
                    rowPool = allocatePool(SegmentKind::SGK_PROPERTY_PAGE_POOL, page.ownerId()); rowFrames = 0;
                }
                const PageOwnerHeader owner{page.pageId(), page.ownerId(), PageOwnerKind::POK_PROPERTY, 0,
                    RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, rowPool.generation, batch.checkpointLsn()};
                page.rebindOwner(owner);
                const PageLocation location{page.pageId(), rowPool.segment_id,
                    uint16_t(RSS_PAGE_POOL_FIRST_DATA_FRAME + rowFrames++), rowPool.generation, page.stats().contiguous_free_bytes};
                rowImages.push_back({rowPool, location, std::move(page)});
            }
            auto published = utils::OwnedVector<PageLocation>(utils::OwnedPmrAllocator<PageLocation>(conversion.memory));
            published.reserve(fieldImages.size() + indexImages->size() + rowImages.size() + immutableImages.frames_.size());
            for (const auto &pool : pools) {
                checkpoint();
                const auto inPool = [&](const auto &image) { return image.pool.segment_id == pool.segment_id; };
                const auto frameCount = size_t(std::count_if(fieldImages.begin(), fieldImages.end(), inPool)) +
                    size_t(std::count_if(indexImages->begin(), indexImages->end(), inPool)) +
                    size_t(std::count_if(rowImages.begin(), rowImages.end(), inPool)) +
                    size_t(std::count_if(immutableImages.frames_.begin(), immutableImages.frames_.end(), inPool));
                const bool hybridPool = std::any_of(fieldImages.begin(), fieldImages.end(), inPool);
                utils::OwnedHeapReservation payloadCredit(conversion.memory, frameCount *
                    (utils::detail::standardAllocatorRequestBytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, alignof(std::byte)) +
                        3U * utils::detail::standardContainerProxyBytes()));
                utils::OwnedVector<SharedPagePoolPreparedAppend> appends{
                    utils::OwnedPmrAllocator<SharedPagePoolPreparedAppend>(conversion.memory)};
                utils::OwnedVector<PageLocation> locations{utils::OwnedPmrAllocator<PageLocation>(conversion.memory)};
                appends.reserve(frameCount); locations.reserve(frameCount);
                const auto append = [&](const auto &prepared, PageOwnerKind kind, const SlottedPage &page) {
                    checkpoint();
                    if (prepared.pool.segment_id != pool.segment_id) return;
                    const auto bytes = page.serializeOwned(conversion.memory);
                    PageOwnerHeader owner{prepared.location.page_id, pool.owner_id, kind, 0,
                        RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
                    SharedPagePoolPreparedAppend preparedAppend;
                    preparedAppend.owner = owner; preparedAppend.free_bytes = prepared.location.free_bytes;
                    preparedAppend.payload.assign(bytes.begin(), bytes.end());
                    appends.push_back(std::move(preparedAppend));
                    auto location = prepared.location; location.page_index = 0;
                    locations.push_back(location);
                };
                if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL) {
                    for (const auto &prepared : fieldImages) append(prepared, PageOwnerKind::POK_HYBRID_PROPERTY, prepared.page);
                    for (const auto &prepared : rowImages) append(prepared, PageOwnerKind::POK_PROPERTY, prepared.page);
                } else if (pool.kind == SegmentKind::SGK_INDEX_PAGE_POOL)
                    for (const auto &prepared : *indexImages) append(prepared, PageOwnerKind::POK_INDEX, prepared.page.slottedPage());
                else if (pool.kind == SegmentKind::SGK_LARGE_VALUE_DATA_POOL || pool.kind == SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) {
                    for (const auto &frame : immutableImages.frames_) if (inPool(frame)) {
                        SharedPagePoolPreparedAppend prepared; prepared.owner = frame.owner;
                        prepared.payload.assign(frame.payload.begin(), frame.payload.end()); appends.push_back(std::move(prepared));
                        auto location = frame.location; location.page_index = 0; locations.push_back(location);
                    }
                } else throw FormatException(FormatError::FERR_INVALID_STATE, "native hybrid candidate has an unsupported pool");
                // A declined optional admission can leave private appends in
                // a Catalog-visible ROW content-index pool. Its next writer
                // must recover these even though no hybrid root was published.
                if (conversion.allocation_admission) native_pool_recovery_required_ = true;
                placement.write(pool, appends, locations, hybridPool);
                for (const auto &location : locations) published.push_back(location);
            }
            // Physical frames precede every reachable metadata root. All
            // allocator, Catalog, Page Directory, field directories and ROW
            // authority changes belong to this ONE final A/B publication.
            for (const auto &reservation : placement.reservations_) if (reservation.fresh) {
                const auto &pool = reservation.pool; allocator_.prepareSetAllocated(batch, pool.physical_slot, true);
                catalog_.prepareUpsert(batch, std::span(&pool, 1));
            }
            std::sort(published.begin(), published.end(), [](const auto &left, const auto &right) {
                return left.page_id < right.page_id;
            });
            std::sort(erasedRowPages.begin(), erasedRowPages.end());
            locations_.prepareReplacePages(batch, published, erasedRowPages);
            utils::OwnedVector<RetireQueueEntry> rowRetirements{utils::OwnedPmrAllocator<RetireQueueEntry>(conversion.memory)};
            rowRetirements.reserve(retiredRowPools.size());
            for (const auto &pool : retiredRowPools)
                rowRetirements.push_back({pool.physical_slot, pool.segment_id, batch.generation(), RetireQueueDisposition::RQD_RETIRED_SEGMENT});
            std::sort(rowRetirements.begin(), rowRetirements.end(), [](const auto &a, const auto &b) { return a.physical_slot < b.physical_slot; });
            if (!retiredRowPools.empty()) {
                catalog_.prepareUpsert(batch, retiredRowPools); retirement_.prepareQueueInsertions(batch, rowRetirements);
            }
            utils::OwnedVector<IndexRootLocation> installedRoots{utils::OwnedPmrAllocator<IndexRootLocation>(conversion.memory)};
            installedRoots.reserve(5);
            if (contentRoot) installedRoots.push_back(*contentRoot);
            for (const auto &root : directoryRoots) if (root) installedRoots.push_back(*root);
            std::sort(installedRoots.begin(), installedRoots.end(), [](const auto &a, const auto &b) { return a.index_id < b.index_id; });
            locations_.prepareReplaceIndexRoots(batch, installedRoots, {});
            if (!immutableImages.manifests_.empty()) locations_.prepareReplaceLargeValueManifests(batch, immutableImages.manifests_, {});
            utils::OwnedVector<EntityPropertyKey> erasedRows{utils::OwnedPmrAllocator<EntityPropertyKey>(conversion.memory)};
            erasedRows.reserve(owners.size());
            for (const auto &owner : owners)
                if (locations_.findEntityPropertyOptional(current, owner.key)) erasedRows.push_back(owner.key);
            if (!erasedRows.empty()) locations_.prepareReplaceEntityProperties(batch, {}, erasedRows);
            pages_.prepareHybridPropertyDescriptorImpl(batch, descriptor, true);
            if (fields->compact_) batch.enableHybridColumnMembership();
            batch.hybrid_property_publication_ = true;
            checkpoint();
            if (conversion.allocation_admission) {
                // Prepare final metadata/routes once. The private future graph
                // remains unreachable until the following A/B decision.
                prepareBatchPublication(batch); checkpoint();
                // Drain dirty private cache pages before the stable-epoch
                // census; read eviction must not become a hidden writeback.
                io_->flushStream(); checkpoint();
                std::optional<NativeStorageAllocationFootprint> prepared;
                try { prepared = allocationCensus->inspectPreparedCandidate(writer, batch, censusOptions); }
                catch (const query::execution::MaterializedByteLimitExceeded &) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_unknown", 1); return current;
                }
                if (!prepared || !prepared->fullyAttributed()) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_unknown", 1); return current;
                }
                const auto row = sourceCost->steadyAllocatedBytes(), candidate = prepared->steadyAllocatedBytes();
                const auto peak = std::max(sourceCost->totalAllocatedBytes(), prepared->totalAllocatedBytes());
                debug::PerfTrace::addValue("native_graph.hybrid.allocation_row_bytes", int64_t(row));
                debug::PerfTrace::addValue("native_graph.hybrid.allocation_candidate_bytes", int64_t(candidate));
                debug::PerfTrace::addValue("native_graph.hybrid.allocation_peak_bytes", int64_t(peak));
                debug::PerfTrace::addValue("native_graph.hybrid.allocation_costs_prepared", 1); checkpoint();
                const auto &limits = *conversion.allocation_admission;
                if (peak > limits.maximum_admitted_peak_bytes || prepared->file_bytes > limits.maximum_admitted_file_bytes) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_resource_refusal", 1); return current;
                }
                const uint64_t threshold = row / 10U * 9U + row % 10U * 9U / 10U;
                if (candidate > threshold) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_insufficient_saving", 1); return current;
                }
                debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_passed", 1);
            }
            publishBatch(batch, writer, options);
            placement.remember();
            return batch.candidate();
        }
		if (derivedState.external_private_tail_reservation.has_value()) {
			// Keep the source outside every metadata allocator decision, but do not
			// publish it as allocated: it remains unreachable on both outcomes.
			batch.reserveDataSlotRange(*derivedState.external_private_tail_reservation);
		}
		for (const EntityPageLocation &location: insertedNodes) {
			batch.observeLogicalNodeId(location.entity_id);
		}
		for (const EntityPageLocation &location: insertedEdges) {
			batch.observeLogicalEdgeId(location.entity_id);
		}
		// Reserve every private or public destination in this writer-owned set.
		// `MetadataWriteBatch` fences the physical choice for this generation even
		// though unpublished private scratch is intentionally absent from Catalog.
		std::set<uint64_t> reservedNewPoolSlots;
		// Allocation state does not change while this writer fence is held. Keep a
		// per-range cursor so a compact scratch plan does not restart an allocator
		// radix walk from its first slot for every candidate segment.
		std::map<std::pair<uint64_t, uint64_t>, std::optional<uint64_t>> nextFreeSlotByRange;
		const auto findFirstFreeNewPoolSlot = [this, &batch, &current, &derivedState, &reservedNewPoolSlots,
																		  &nextFreeSlotByRange](
																 PhysicalSlotId first, PhysicalSlotId last) {
			const auto cursor = nextFreeSlotByRange.try_emplace({first.value(), last.value()}, first.value()).first;
			if (!cursor->second.has_value()) {
				return std::optional<PhysicalSlotId>{};
			}
			PhysicalSlotId candidate(*cursor->second);
			while (candidate <= last) {
				const std::optional<PhysicalSlotId> found = allocator_.findFirstFree(current, candidate, last);
				if (!found.has_value()) {
					cursor->second.reset();
					return std::optional<PhysicalSlotId>{};
				}
				const auto advanceCursor = [&cursor, found, last] {
					if (*found == last || found->value() == std::numeric_limits<uint64_t>::max()) {
						cursor->second.reset();
					} else {
						cursor->second = found->value() + 1U;
					}
				};
				const bool isPrivateSourceSlot = derivedState.external_private_tail_reservation.has_value() &&
																 derivedState.external_private_tail_reservation->contains(*found);
				if (!isPrivateSourceSlot && !batch.isDataSlotReserved(*found) && !batch.isMetadataPoolSlot(*found) &&
					reservedNewPoolSlots.insert(found->value()).second) {
					advanceCursor();
					return found;
				}
				advanceCursor();
				if (!cursor->second.has_value()) {
					return std::optional<PhysicalSlotId>{};
				}
				candidate = PhysicalSlotId(*cursor->second);
			}
			return std::optional<PhysicalSlotId>{};
		};
		const auto reserveDistinctNewPoolSlot = [&findFirstFreeNewPoolSlot](PhysicalSlotId first, PhysicalSlotId last,
																						 const char *message) {
			const std::optional<PhysicalSlotId> found = findFirstFreeNewPoolSlot(first, last);
			if (!found.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE, message);
			}
			return *found;
		};
		std::optional<WriterScopedCompactPrivateScratch> compactPrivateScratch;
		if (derivedState.private_segment_plan.has_value()) {
			const NativeGraphPrivateSegmentPlan &plan = *derivedState.private_segment_plan;
			if (plan.segment_count > std::numeric_limits<uint64_t>::max()) {
				throw FormatException(
						FormatError::FERR_ARITHMETIC_OVERFLOW,
						"native graph compact private scratch plan exceeds its durable identity namespace");
			}
			const uint64_t count = static_cast<uint64_t>(plan.segment_count);
			CatalogEntry firstEntry;
			firstEntry.segment_id = batch.allocateLogicalSegmentIdRange(count);
			firstEntry.kind = plan.kind;
			firstEntry.owner_id = plan.owner_id;
			firstEntry.generation = Generation(1U);
			firstEntry.state = SegmentState::SGS_SEALED;
			firstEntry.committed_lsn = checkpointLsn;
			compactPrivateScratch = {firstEntry, {}, plan.first_slot, plan.last_slot, plan.segment_count, plan.header_bytes,
								 plan.header_extension};
		}
		auto privateScratchState = std::make_shared<WriterScopedPrivateScratchState>();
		privateScratchState->superblock = current;
		std::shared_ptr<NativeGraphPrivateSegmentSet> privateSegmentSet;
		if (compactPrivateScratch.has_value()) {
			privateScratchState->compact_candidate = *compactPrivateScratch;
			const WriterScopedCompactPrivateScratch &compact = *privateScratchState->compact_candidate;
			privateSegmentSet = std::make_shared<NativeGraphPrivateSegmentSet>(
					compact.first_entry.segment_id, compact.first_entry.generation, compact.count,
					compact.first_entry.owner_id);
		} else if (!derivedState.private_segments.empty()) {
			privateScratchState->candidates.reserve(privateSegmentCount);
			for (const NativeGraphPrivateSegmentRequest &request: derivedState.private_segments) {
				CatalogEntry entry;
				entry.segment_id = batch.allocateLogicalSegmentId();
				entry.physical_slot = reserveDistinctNewPoolSlot(
						request.first_slot, request.last_slot,
						"native graph private segment has no free physical slot in its bounded range");
				entry.kind = request.kind;
				entry.owner_id = request.owner_id;
				entry.generation = Generation(1U);
				entry.state = SegmentState::SGS_SEALED;
				entry.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, entry.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph private segment allocated an already-published Segment ID");
				}
				batch.reserveDataSlot(entry.physical_slot);
				privateScratchState->candidates.push_back({entry, request.header_bytes, 0U});
			}
			const CatalogEntry &first = privateScratchState->candidates.front().entry;
			privateSegmentSet = std::make_shared<NativeGraphPrivateSegmentSet>(
					first.segment_id, first.generation, privateScratchState->candidates.size(), first.owner_id);
		}
		const auto privateEntryAt = [privateScratchState](size_t ordinal) {
			if (privateScratchState->compact_candidate.has_value()) {
				const WriterScopedCompactPrivateScratch &compact = *privateScratchState->compact_candidate;
				const auto entry = compact.materialized_entries.find(ordinal);
				if (ordinal >= compact.count || entry == compact.materialized_entries.end()) { // compact private scratch materialization emits one entry for every bounded ordinal.
					throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph compact private scratch ordinal is not materialized");
				}
				return entry->second;
			}
			return privateScratchState->candidates.at(ordinal).entry;
		};
		const auto privateHeaderBytesAt = [privateScratchState](size_t ordinal) {
			if (privateScratchState->compact_candidate.has_value()) {
				const WriterScopedCompactPrivateScratch &compact = *privateScratchState->compact_candidate;
				if (ordinal >= compact.count) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph compact private scratch ordinal is out of range");
				}
				return compact.header_bytes;
			}
			return privateScratchState->candidates.at(ordinal).header_bytes;
		};
		// Every reservation is a complete self-authenticating Segment image. Reuse
		// one bounded buffer rather than allocating a 128 KiB image per candidate.
		std::vector<std::byte> privateScratchImage(RSS_SEGMENT_SIZE);
		// Legacy callers already supply one physical request per candidate. Compact
		// streamed plans materialize reservations lazily in candidateWriter below.
		for (size_t ordinal = 0U;
			 !derivedState.private_segment_plan.has_value() && ordinal < privateSegmentCount; ++ordinal) {
			const NativeGraphPrivateSegmentRequest &legacyRequest = derivedState.private_segments.at(ordinal);
			const uint16_t headerBytes = legacyRequest.header_bytes;
			const std::vector<std::byte> &headerExtension = legacyRequest.header_extension;
			const CatalogEntry entry = privateEntryAt(ordinal);
			std::vector<std::byte> payload = privateSegmentImagePreparer
															 ? privateSegmentImagePreparer(*privateSegmentSet, ordinal)
															 : legacyRequest.payload;
			if (payload.empty() || payload.size() > RSS_SEGMENT_SIZE - headerBytes) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph private scratch image preparation returned an invalid bounded payload");
			}
			std::fill(privateScratchImage.begin(), privateScratchImage.end(), std::byte{});
			SegmentEnvelope envelope;
			envelope.header_bytes = headerBytes;
			envelope.kind = entry.kind;
			envelope.state = SegmentState::SGS_SEALED;
			envelope.segment_id = entry.segment_id;
			envelope.owner_id = entry.owner_id;
			envelope.generation = entry.generation;
			envelope.committed_lsn = entry.committed_lsn;
			envelope.payload_bytes = static_cast<uint32_t>(payload.size());
			envelope.payload_crc = utils::calculateCrc(payload.data(), payload.size());
			const std::array<std::byte, RSS_ENVELOPE_BYTES> encodedEnvelope = SegmentEnvelopeCodec::encode(envelope);
			std::copy(encodedEnvelope.begin(), encodedEnvelope.end(), privateScratchImage.begin());
			std::copy(headerExtension.begin(), headerExtension.end(), privateScratchImage.begin() + RSS_ENVELOPE_BYTES);
			std::copy(payload.begin(), payload.end(), privateScratchImage.begin() + headerBytes);
			const uint64_t offset =
					PhysicalSegmentAllocator::checkedSlotOffset(current.data_region_offset, entry.physical_slot);
			io_->writePagesThrough(
					{offset, entry.generation.value()}, privateScratchImage.data(), privateScratchImage.size());
		}
		const auto materializeCompactPrivateEntry =
				[this, privateScratchState, privateSegmentSet, &batch, &reserveDistinctNewPoolSlot,
					 privateSegmentImagePreparer, checkpointLsn](size_t ordinal) {
					if (!privateScratchState->compact_candidate.has_value() || privateSegmentSet == nullptr) {
						return false;
					}
					WriterScopedCompactPrivateScratch &compact = *privateScratchState->compact_candidate;
					if (ordinal >= compact.count) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph compact private scratch ordinal is out of range");
					}
					if (compact.materialized_entries.contains(ordinal)) {
						return false;
					}
					if (!privateSegmentImagePreparer) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph compact private scratch has no reservation image preparer");
					}
					CatalogEntry entry = compact.first_entry;
					entry.segment_id = SegmentId(entry.segment_id.value() + ordinal);
					entry.physical_slot = reserveDistinctNewPoolSlot(
							compact.first_slot, compact.last_slot,
							"native graph compact private scratch plan has no allocator-visible free slot");
					batch.reserveDataSlot(entry.physical_slot);
					const std::vector<std::byte> payload = privateSegmentImagePreparer(*privateSegmentSet, ordinal);
					if (payload.empty() || payload.size() > RSS_SEGMENT_SIZE - compact.header_bytes) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph compact private scratch image preparation returned an invalid bounded payload");
					}
					std::vector<std::byte> image(RSS_SEGMENT_SIZE);
					SegmentEnvelope envelope;
					envelope.header_bytes = compact.header_bytes;
					envelope.kind = entry.kind;
					envelope.state = SegmentState::SGS_SEALED;
					envelope.segment_id = entry.segment_id;
					envelope.owner_id = entry.owner_id;
					envelope.generation = entry.generation;
					envelope.committed_lsn = checkpointLsn;
					envelope.payload_bytes = static_cast<uint32_t>(payload.size());
					envelope.payload_crc = utils::calculateCrc(payload.data(), payload.size());
					const std::array<std::byte, RSS_ENVELOPE_BYTES> encodedEnvelope = SegmentEnvelopeCodec::encode(envelope);
					std::copy(encodedEnvelope.begin(), encodedEnvelope.end(), image.begin());
					std::copy(compact.header_extension.begin(), compact.header_extension.end(),
							  image.begin() + RSS_ENVELOPE_BYTES);
					std::copy(payload.begin(), payload.end(), image.begin() + compact.header_bytes);
					const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(
							privateScratchState->superblock.data_region_offset, entry.physical_slot);
					io_->writePagesThrough({offset, entry.generation.value()}, image.data(), image.size());
					compact.materialized_entries.emplace(ordinal, entry);
					return true;
				};
		const NativeGraphPrivateSegmentWriter candidateWriter = [this, privateScratchState, privateSegmentSet,
															 privateEntryAt, privateHeaderBytesAt, materializeCompactPrivateEntry](
																	 const NativeGraphPrivateSegmentMaterialization
																			 &identity,
																	 std::vector<std::byte> payload) {
			debug::ScopedPerfTimer timer("native_graph.streamed.private_scratch_write");
			if (!privateScratchState->active || privateSegmentSet == nullptr || !identity.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch writer escaped its planning scope");
			}
			const size_t ordinal = privateSegmentSet->ordinalOf(identity);
			const bool materializedNow = materializeCompactPrivateEntry(ordinal);
			const CatalogEntry entry = privateEntryAt(ordinal);
			const uint16_t headerBytes = privateHeaderBytesAt(ordinal);
			if (payload.empty() || payload.size() > RSS_SEGMENT_SIZE - headerBytes) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph private scratch writer received an invalid bounded payload");
			}
			const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(
					privateScratchState->superblock.data_region_offset, entry.physical_slot);
			if (materializedNow) {
				// This writer just emitted the authenticated reservation under the same
				// writer fence. Avoid a redundant full-segment read before replacing it.
				const WriterScopedCompactPrivateScratch &compact = *privateScratchState->compact_candidate;
				std::vector<std::byte> image(RSS_SEGMENT_SIZE);
				SegmentEnvelope envelope;
				envelope.header_bytes = headerBytes;
				envelope.kind = entry.kind;
				envelope.state = SegmentState::SGS_SEALED;
				envelope.segment_id = entry.segment_id;
				envelope.owner_id = entry.owner_id;
				envelope.generation = entry.generation;
				envelope.committed_lsn = entry.committed_lsn;
				envelope.payload_bytes = static_cast<uint32_t>(payload.size());
				envelope.payload_crc = utils::calculateCrc(payload.data(), payload.size());
				const std::array<std::byte, RSS_ENVELOPE_BYTES> encodedEnvelope = SegmentEnvelopeCodec::encode(envelope);
				std::copy(encodedEnvelope.begin(), encodedEnvelope.end(), image.begin());
				std::copy(compact.header_extension.begin(), compact.header_extension.end(),
						  image.begin() + RSS_ENVELOPE_BYTES);
				std::copy(payload.begin(), payload.end(), image.begin() + headerBytes);
				io_->writePagesThrough({offset, entry.generation.value()}, image.data(), image.size());
				privateScratchState->candidate_payloads_rewritten = true;
				return;
			}
			std::vector<std::byte> currentImage(RSS_SEGMENT_SIZE);
			const size_t bytesRead = io_->readPages({offset, entry.generation.value()}, currentImage.data(),
					currentImage.size(), BufferPoolReadHint::BPRH_SEQUENTIAL);
			if (bytesRead != currentImage.size()) {
				throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL,
								  "native graph private scratch writer found a truncated candidate segment");
			}
			const SegmentEnvelope previous =
					SegmentEnvelopeCodec::decode(std::span<const std::byte>(currentImage.data(), RSS_ENVELOPE_BYTES));
			const uint8_t invalidPrevious =
					static_cast<uint8_t>(previous.kind != SegmentKind::SGK_BULK_SCRATCH) |
					static_cast<uint8_t>(previous.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(previous.segment_id != entry.segment_id) |
					static_cast<uint8_t>(previous.owner_id != entry.owner_id) |
					static_cast<uint8_t>(previous.generation != entry.generation) |
					static_cast<uint8_t>(previous.committed_lsn != entry.committed_lsn) |
					static_cast<uint8_t>(previous.header_bytes != headerBytes) |
					static_cast<uint8_t>(previous.payload_bytes == 0U) |
					static_cast<uint8_t>(previous.payload_bytes > RSS_SEGMENT_SIZE - headerBytes);
			if (invalidPrevious != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch writer found an invalid candidate identity");
			}
			const std::span<const std::byte> previousPayload(currentImage.data() + headerBytes, previous.payload_bytes);
			if (utils::calculateCrc(previousPayload.data(), previousPayload.size()) != previous.payload_crc ||
				!isPrivateScratchReservation(previousPayload)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch writer may replace only a reserved candidate once");
			}
			// Preserve the fixed header extension and unused bytes exactly; the
			// writer is authorized to replace only the reserved payload.
			std::vector<std::byte> image = std::move(currentImage);
			SegmentEnvelope envelope = previous;
			envelope.payload_bytes = static_cast<uint32_t>(payload.size());
			envelope.payload_crc = utils::calculateCrc(payload.data(), payload.size());
			const std::array<std::byte, RSS_ENVELOPE_BYTES> encodedEnvelope = SegmentEnvelopeCodec::encode(envelope);
			std::copy(encodedEnvelope.begin(), encodedEnvelope.end(), image.begin());
			std::copy(payload.begin(), payload.end(), image.begin() + headerBytes);
			io_->writePagesThrough({offset, entry.generation.value()}, image.data(), image.size());
			privateScratchState->candidate_payloads_rewritten = true;
		};
		const NativeGraphPrivateSegmentReader candidateReader =
			[this, privateScratchState, privateSegmentSet, privateEntryAt, privateHeaderBytesAt,
			 materializeCompactPrivateEntry](
					const NativeGraphPrivateSegmentMaterialization &identity) -> std::vector<std::byte> {
				debug::ScopedPerfTimer timer("native_graph.streamed.private_scratch_read");
			if (!privateScratchState->active) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch reader escaped its writer scope");
			}
			if (!identity.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph private scratch reader received an invalid identity");
			}
			if (privateSegmentSet == nullptr) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph private scratch reader cannot resolve a non-candidate segment");
			}
			CatalogEntry entry;
			uint16_t headerBytes = 0U;
			{
				debug::ScopedPerfTimer resolveTimer("native_graph.streamed.private_scratch_resolve");
				const size_t ordinal = privateSegmentSet->ordinalOf(identity);
				// A reader may be the first consumer of a compact candidate. Materialize
				// its authenticated reservation so the normal reservation validation below
				// remains the fail-closed result for an unwritten payload.
				materializeCompactPrivateEntry(ordinal);
				entry = privateEntryAt(ordinal);
				headerBytes = privateHeaderBytesAt(ordinal);
			}
			std::vector<std::byte> image(RSS_SEGMENT_SIZE);
			const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(
					privateScratchState->superblock.data_region_offset, entry.physical_slot);
			size_t bytesRead = 0U;
			{
				debug::ScopedPerfTimer readTimer("native_graph.streamed.private_scratch_physical_read");
				bytesRead = io_->readPages({offset, entry.generation.value()}, image.data(), image.size(),
						BufferPoolReadHint::BPRH_SEQUENTIAL);
			}
			if (bytesRead != image.size()) {
				throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL,
								  "native graph private scratch reader found a truncated candidate segment");
			}
			debug::ScopedPerfTimer validationTimer("native_graph.streamed.private_scratch_validate_copy");
			const SegmentEnvelope envelope =
					SegmentEnvelopeCodec::decode(std::span<const std::byte>(image.data(), RSS_ENVELOPE_BYTES));
			const uint8_t invalid =
					static_cast<uint8_t>(envelope.kind != SegmentKind::SGK_BULK_SCRATCH) |
					static_cast<uint8_t>(envelope.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(envelope.segment_id != entry.segment_id) |
					static_cast<uint8_t>(envelope.owner_id != entry.owner_id) |
					static_cast<uint8_t>(envelope.generation != entry.generation) |
					static_cast<uint8_t>(envelope.committed_lsn != entry.committed_lsn) |
					static_cast<uint8_t>(envelope.header_bytes != headerBytes) |
					static_cast<uint8_t>(envelope.payload_bytes == 0U) |
					static_cast<uint8_t>(envelope.payload_bytes > RSS_SEGMENT_SIZE - envelope.header_bytes);
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch candidate does not match its assigned identity");
			}
			std::vector<std::byte> payload(image.begin() + envelope.header_bytes,
										   image.begin() + envelope.header_bytes + envelope.payload_bytes);
			if (utils::calculateCrc(payload.data(), payload.size()) != envelope.payload_crc) {
				throw FormatException(FormatError::FERR_CHECKSUM_MISMATCH,
									  "native graph private scratch candidate payload checksum does not match");
			}
			if (isPrivateScratchReservation(payload)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch reader cannot expose an unfilled reservation");
			}
			return payload;
		};
		const NativeGraphPrivateSegmentFence candidateFence = [this, privateScratchState] {
			debug::ScopedPerfTimer timer("native_graph.streamed.private_scratch_fence");
			if (!privateScratchState->active) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch fence escaped its planning scope");
			}
			io_->sync();
		};
		std::vector<IndexTreeMultiPoolMutationPreparer> deferredIndexPreparers;
		std::vector<NativeGraphDeferredIndexPlan> deferredPlans;
		const NativeGraphDeferredIndexPlanSink deferIndexPlan = [&deferredPlans](NativeGraphDeferredIndexPlan plan) {
			if (!plan.preparer && !plan.images) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph early publication submitted an empty deferred Index plan");
			}
			deferredPlans.push_back(std::move(plan));
		};
		// A bulk builder reopens its input while final pages are materialized, not
		// only while it computes the layout. Keep the writer-fenced pin alive for
		// both passes; it remains private to this candidate and is destroyed before
		// the metadata writer leaves scope.
		std::optional<MetadataSnapshotPin> deferredPlanningSnapshot;
		if (earlyPublicationPlanner || deferredIndexPlanner || deferredIndexPlansPlanner) {
			// The private images are durable, but no Catalog/Allocator/Superblock
			// root reaches them. A planner can therefore make both B+Tree passes
			// before this coordinator assigns any destination Index identities.
			io_->sync();
			// Opening a regular snapshot here would attempt to acquire the reader
			// side of the same publication barrier held by `writer`. Pin the already
			// fenced durable root instead, and keep that pin confined to planning.
			deferredPlanningSnapshot.emplace(snapshots_.pinCurrentUnderWrite(writer, current));
			try {
				if (earlyPublicationPlanner) {
					debug::ScopedPerfTimer timer("native_graph.streamed.early_publication");
					earlyPublicationPlanner({*privateSegmentSet, candidateReader, candidateWriter, candidateFence,
																					 *deferredPlanningSnapshot, batch, current, checkpointLsn,
																					 deferIndexPlan});
					if (privateScratchState->candidate_payloads_rewritten) {
						io_->sync();
					}
				}
			} catch (...) {
				deferredPlanningSnapshot->release();
				throw;
			}
			if (deferredIndexPlanner) {
				NativeGraphDeferredIndexPlan plan;
				try {
					plan = deferredIndexPlanner({*privateSegmentSet, candidateReader, *deferredPlanningSnapshot});
				} catch (...) {
					deferredPlanningSnapshot->release();
					throw;
				}
				deferredPlans.push_back(std::move(plan));
			}
			if (deferredIndexPlansPlanner) {
				try {
					NativeGraphDeferredIndexPlans plans =
							deferredIndexPlansPlanner({*privateSegmentSet, candidateReader, *deferredPlanningSnapshot});
					deferredPlans.insert(deferredPlans.end(), std::make_move_iterator(plans.plans.begin()),
										 std::make_move_iterator(plans.plans.end()));
				} catch (...) {
					deferredPlanningSnapshot->release();
					throw;
				}
			}
            std::set<uint64_t> deferredIndexIds;
			for (NativeGraphDeferredIndexPlan &plan: deferredPlans) {
                if (!deferredIndexIds.insert(plan.mutation.index_id).second)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "deferred index plans repeat a root");
                if (plan.images) {
                    auto &proof = *plan.images;
                    const auto &bare = plan.mutation;
                    if (!privateSegmentSet || !privateSegmentSet->size() || (!proof.private_segments_ || !proof.private_segments_->size()))
                        throw FormatException(FormatError::FERR_INVALID_STATE, "deferred images have no scoped private source");
                    const auto first = proof.private_segments_->front(), actualFirst = privateSegmentSet->front();
                    if (proof.metadata_ != this || proof.source_generation_ != current.generation || proof.materialized_ ||
                        !proof.memory_ || !proof.reader_ || !proof.entry_count_ || !proof.page_count_ ||
                        proof.private_segments_->size() != privateSegmentSet->size() || first.segment_id != actualFirst.segment_id ||
                        first.generation != actualFirst.generation || first.owner_id != actualFirst.owner_id ||
                        locations_.findIndexRootOptional(current, proof.index_id_) != proof.source_root_ ||
                        bare.index_id != proof.index_id_ || bare.expected_root || !bare.pool_requests.empty() ||
                        !bare.new_pool_requests.empty() || bare.replacement_root || bare.stable_replacement_root || bare.retire_root ||
                        plan.preparer || std::ranges::any_of(effectiveIndexMutations, [&proof](const auto &other) {
                            return other.index_id == proof.index_id_;
                        }))
                        throw FormatException(FormatError::FERR_INVALID_STATE, "deferred images have no exact owned source receipt");
                    if (proof.index_id_ != RSS_GLOBAL_ADJACENCY_INDEX_ID) {
                        BTreePageStore schemaPages(*deferredSchemaPool, *this); SystemCatalog schema(schemaPages, *this);
                        const auto definition = schema.findIndexDefinitionById(*deferredPlanningSnapshot, proof.index_id_);
                        if (!definition || definition->kind != IndexDefinitionKind::IDK_LABEL ||
                            definition->entity_kind != IndexDefinitionEntityKind::IDEK_EDGE)
                            throw FormatException(FormatError::FERR_INVALID_STATE, "streamed receipt lost its EDGE membership schema");
                    }
                    { utils::OwnedHeapReservation leaseRead(proof.memory_, 2U * RSS_SEGMENT_SIZE);
                      static_cast<void>(proof.reader_(proof.private_segments_->front())); }
                    if (proof.incremental_) {
                        prepareStreamedAdjacencyImages(batch, writer, *deferredPlanningSnapshot, proof,
                            proof.index_id_ == RSS_GLOBAL_ADJACENCY_INDEX_ID ? streamedAdjacencyPlacement : streamedMembershipPlacement);
                        continue;
                    }
                    proof.materialized_ = true;
                    plan.mutation = std::move(proof.fresh_mutation_); plan.preparer = std::move(proof.fresh_preparer_);
                }

				const NativeGraphIndexTreeMutation &mutation = plan.mutation;
				const bool replacesRoot = mutation.replacement_root.has_value();
				const bool retiresRoot = mutation.retire_root;
				bool allowedIndexId = mutation.index_id == RSS_GLOBAL_ADJACENCY_INDEX_ID ||
											mutation.index_id == RSS_SYSTEM_CATALOG_INDEX_ID ||
											mutation.index_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID;
				if (!allowedIndexId && isSystemCatalogIndexDefinitionId(mutation.index_id) &&
						replacesRoot && mutation.expected_root.has_value()) {
					// Logical members are admitted only by their authenticated published
					// schema in this writer-fenced snapshot, never by an arbitrary ID.
					BTreePageStore schemaPages(*deferredSchemaPool, *this);
					SystemCatalog schema(schemaPages, *this);
					const auto definition = schema.findIndexDefinitionById(*deferredPlanningSnapshot, mutation.index_id);
					allowedIndexId = definition.has_value() && definition->kind == IndexDefinitionKind::IDK_LABEL &&
							definition->entity_kind == IndexDefinitionEntityKind::IDEK_EDGE;
				}
				if (!plan.preparer || !allowedIndexId || // effective index mutations are assembled only from authenticated preparers and allowed IDs.
					std::ranges::any_of(effectiveIndexMutations,
										[&mutation](const NativeGraphIndexTreeMutation &existing) {
											return existing.index_id == mutation.index_id;
										}) ||
					(replacesRoot == retiresRoot) ||
					(replacesRoot &&
					 (mutation.new_pool_requests.empty() || !mutation.replacement_root->newly_reserved ||
					  mutation.stable_replacement_root.has_value())) ||
					(retiresRoot && (!mutation.expected_root.has_value() || !mutation.new_pool_requests.empty() ||
									 mutation.stable_replacement_root.has_value() || mutation.pool_requests.empty()))) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph deferred Index planner returned an invalid root transition");
				}
				const std::optional<IndexRootLocation> visibleRoot =
						locations_.findIndexRootOptional(current, mutation.index_id);
				if ((mutation.expected_root.has_value() &&
					 (!visibleRoot.has_value() || *visibleRoot != *mutation.expected_root)) ||
					(!mutation.expected_root.has_value() && visibleRoot.has_value())) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph deferred Index planner lost its current root fence");
				}
				if (replacesRoot) {
					const NativeGraphIndexTreeAppendedPageReference &root = *mutation.replacement_root;
					if (root.pool_ordinal >= mutation.new_pool_requests.size() ||
						root.appended_page_ordinal >=
								mutation.new_pool_requests[root.pool_ordinal].appended_page_count) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph deferred Index planner returned an invalid fresh root reference");
					}
				}
				std::vector<IndexTreeMutationPoolBatch> batches;
				batches.reserve(mutation.pool_requests.size() + mutation.new_pool_requests.size());
				bool retiredExpectedRoot = false;
				std::set<uint64_t> sourcePoolIds;
				for (const IndexTreeMutationPoolRequest &request: mutation.pool_requests) {
					if (!request.segment_id.isValid() ||
						(request.expected_replacements.empty() && request.appended_page_count == 0U &&
							 request.expected_retirements.empty()) ||
						(request.replacement_segment_id.isValid() && request.expected_replacements.empty()) ||
						request.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
						!sourcePoolIds.insert(request.segment_id.value()).second) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph deferred Index planner has an invalid source pool request");
					}
					const CatalogEntry pool = catalog_.find(current, request.segment_id);
					if (pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL || pool.state != SegmentState::SGS_SEALED ||
						pool.owner_id == 0U || !allocator_.isAllocated(current, pool.physical_slot)) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph deferred Index planner references an unavailable source pool");
					}
					CatalogEntry replacementPool = pool;
					if (request.replacement_segment_id.isValid()) {
						replacementPool = catalog_.find(current, request.replacement_segment_id);
						if (replacementPool.segment_id == pool.segment_id ||
							replacementPool.kind != SegmentKind::SGK_INDEX_PAGE_POOL ||
							replacementPool.owner_id != pool.owner_id || replacementPool.state != SegmentState::SGS_SEALED ||
							!allocator_.isAllocated(current, replacementPool.physical_slot)) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"native graph deferred Index planner references an unavailable cross-pool replacement destination");
						}
					}
					for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
						const PageLocation visible = locations_.findPage(current, retirement.location.page_id);
						if (visible != retirement.location || visible.segment_id != pool.segment_id ||
							visible.generation != pool.generation || !retirement.expected_owner_lsn.isValid() ||
							!pageIds.insert(visible.page_id.value()).second) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"native graph deferred Index planner references a stale retirement page");
						}
						retiredExpectedRoot =
								retiredExpectedRoot ||
								(mutation.expected_root.has_value() &&
								 retirement.location.page_id == mutation.expected_root->root_page_id &&
								 retirement.location.generation == mutation.expected_root->root_generation);
					}
					batches.push_back({pool,
								   request.expected_replacements,
								   {},
								   request.expected_replacements,
								   {},
								   false,
								   request.expected_retirements,
								   0U,
								   replacementPool});
				}
				if (mutation.expected_root.has_value() && !retiredExpectedRoot) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph deferred Index replacement did not retire its fenced root");
				}
				for (const NewIndexTreePoolRequest &request: mutation.new_pool_requests) {
					if (!request.first_slot.isValid() || !request.last_slot.isValid() ||
						request.first_slot > request.last_slot || request.owner_id != mutation.index_id ||
						request.appended_page_count == 0U ||
						request.appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph deferred Index planner has an invalid fresh pool request");
					}
				}
				effectiveIndexMutations.push_back(std::move(plan.mutation));
				derivedIndexBatches.push_back(std::move(batches));
				deferredIndexPreparers.push_back(std::move(plan.preparer));
			}
		}
            std::set<uint64_t> ordinaryImageIds;
            if (!derivedState.index_images.empty()) {
                const auto &first = derivedState.index_images.front();
                if (!first || !first->memory_)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "ordinary images require an owned source");
                ordinaryIndexSource.emplace(snapshots_.pinCurrentUnderWrite(writer, current));
                ordinaryIndexPlacement.reset(new HybridPropertyPoolPlacement(*this, batch, writer, *ordinaryIndexSource,
                    first->first_slot_, first->last_slot_, first->memory_, [] {}, true));
                // The regular frame planner has already budgeted these pools.
                // Reserve no destinations from them for private image work.
                const auto exclude = [&](SegmentId id) {
                    if (!id.isValid() || ordinaryIndexPlacement->contains(id)) return;
                    const auto pool = catalog_.find(current, id);
                    if (pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "ordinary competing plan lost its Index pool");
                    ordinaryIndexPlacement->reservations_.push_back({pool, 0, 0, 0, false, false, false});
                };
                for (const auto &mutation : effectiveIndexMutations) for (const auto &request : mutation.pool_requests) {
                    exclude(request.segment_id); exclude(request.replacement_segment_id);
                }
            }
            for (const auto &owned : derivedState.index_images) {
                if (!owned || !ordinaryImageIds.insert(owned->index_id_).second)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "ordinary image receipts repeat an index");
                auto &proof = *owned;
                if (proof.metadata_ != this || proof.source_generation_ != current.generation || proof.materialized_ ||
                    proof.private_segments_ || proof.reader_ || !proof.memory_ || !proof.incremental_ ||
                    (proof.index_id_ != RSS_GLOBAL_ADJACENCY_INDEX_ID && !isSystemCatalogIndexDefinitionId(proof.index_id_)) ||
                    locations_.findIndexRootOptional(current, proof.index_id_) != proof.source_root_ ||
                    std::ranges::any_of(effectiveIndexMutations, [&proof](const auto &other) {return other.index_id == proof.index_id_;}) ||
                    std::ranges::any_of(deferredPlans, [&proof](const auto &other) {return other.mutation.index_id == proof.index_id_;}))
                    throw FormatException(FormatError::FERR_INVALID_STATE, "ordinary index images lost their exact owned source");
                if (proof.index_id_ != RSS_GLOBAL_ADJACENCY_INDEX_ID) {
                    BTreePageStore schemaPages(*deferredSchemaPool, *this); SystemCatalog schema(schemaPages, *this);
                    const auto definition = schema.findIndexDefinitionById(*ordinaryIndexSource, proof.index_id_);
                    if (!definition || definition->kind != IndexDefinitionKind::IDK_LABEL ||
                        definition->entity_kind != IndexDefinitionEntityKind::IDEK_EDGE)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "ordinary image receipt lost its EDGE membership schema");
                }
                prepareStreamedAdjacencyImages(batch, writer, *ordinaryIndexSource, proof, ordinaryIndexPlacement);
                debug::PerfTrace::addValue(proof.index_id_ == RSS_GLOBAL_ADJACENCY_INDEX_ID ?
                    "native_graph.write.ordinary_adjacency.prepared_images" : "native_graph.write.ordinary_membership.prepared_images", int64_t(proof.images_.size()));
            }
		std::vector<CatalogEntry> newPoolEntries;
		newPoolEntries.reserve(newPoolRequests.size() + freshReplacementPools.size());
		std::map<uint64_t, size_t> newPoolEntryBySegment;
		for (const EntityPageNewPoolBatchRequest &request: newPoolRequests) {
			const auto fresh = freshReplacementPools.find(request.reservation.segment_id.value());
			if (fresh != freshReplacementPools.end() && !hasSameFreshEntityPoolIdentity(fresh->second, request)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page transaction conflicts with its fresh COW-pool reservation");
			}
		}
		for (const EntityPageNewPoolBatchRequest &request: newPoolRequests) {
			const auto existing = newPoolEntryBySegment.find(request.reservation.segment_id.value());
			if (existing != newPoolEntryBySegment.end()) {
				const CatalogEntry &entry = newPoolEntries[existing->second];
				newPoolMaterializations.push_back(
						{entry, {request.page_id, entry.segment_id, 0U, entry.generation, 0U}, request.owner_kind});
				continue;
			}

			const std::optional<PhysicalSlotId> physicalSlot =
					findFirstFreeNewPoolSlot(request.first_slot, request.last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page new-pool batch has no free physical slot in its request range");
			}
			CatalogEntry entry;
			entry.segment_id = request.reservation.segment_id;
			entry.physical_slot = *physicalSlot;
			entry.kind = request.reservation.kind;
			entry.owner_id = request.reservation.owner_id;
			entry.generation = request.reservation.segment_generation;
			entry.state = request.reservation.state;
			entry.committed_lsn = checkpointLsn;
			batch.observeLogicalSegmentId(entry.segment_id);
			batch.reserveDataSlot(entry.physical_slot);
			newPoolEntryBySegment.emplace(entry.segment_id.value(), newPoolEntries.size());
			newPoolEntries.push_back(entry);
			newPoolMaterializations.push_back(
					{entry, {request.page_id, entry.segment_id, 0U, entry.generation, 0U}, request.owner_kind});
		}
		for (const auto &[segmentId, destination]: freshReplacementPools) {
			if (newPoolEntryBySegment.contains(segmentId)) {
				continue;
			}
			const std::optional<PhysicalSlotId> physicalSlot =
					findFirstFreeNewPoolSlot(destination.first_slot, destination.last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity page fresh COW destination has no free physical slot");
			}
			CatalogEntry entry;
			entry.segment_id = destination.segment_id;
			entry.physical_slot = *physicalSlot;
			entry.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
			entry.owner_id = destination.pool_owner_id;
			entry.generation = destination.generation;
			entry.state = SegmentState::SGS_SEALED;
			entry.committed_lsn = checkpointLsn;
			batch.observeLogicalSegmentId(entry.segment_id);
			batch.reserveDataSlot(entry.physical_slot);
			newPoolEntryBySegment.emplace(segmentId, newPoolEntries.size());
			newPoolEntries.push_back(entry);
		}
		for (size_t ordinal = 0U; ordinal < replacementFreshDestinations.size(); ++ordinal) {
			if (!replacementFreshDestinations[ordinal].has_value()) {
				continue;
			}
			const EntityPageFreshPoolDestination &destination = *replacementFreshDestinations[ordinal];
			const auto entryOrdinal = newPoolEntryBySegment.find(destination.segment_id.value());
			if (entryOrdinal == newPoolEntryBySegment.end()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Entity page fresh COW destination was not materialized");
			}
			const CatalogEntry &entry = newPoolEntries[entryOrdinal->second];
			EntityPageReplacementMaterialization &materialization = materializations[ordinal];
			materialization.pool = entry;
			materialization.replacement = {materialization.expected_location.page_id, entry.segment_id, 0U,
										   entry.generation, 0U};
		}
		std::vector<CatalogEntry> freshPropertyReplacementPoolEntries;
		freshPropertyReplacementPoolEntries.reserve(propertyReplacements.size());
		struct FreshPropertyReplacementPool {
			CatalogEntry entry;
			size_t prepared_frames = 0U;
		};
		std::map<uint64_t, FreshPropertyReplacementPool> freshPropertyReplacementPools;
		for (size_t ordinal = 0U; ordinal < propertyReplacements.size(); ++ordinal) {
			if (!propertyReplacementNeedsFreshPool[ordinal]) {
				continue;
			}
			const NativeGraphPropertyPageReplacementRequest &request = propertyReplacements[ordinal];
			const bool invalidReservation = !request.first_slot.isValid() || !request.last_slot.isValid() ||
											request.first_slot > request.last_slot;
			if (invalidReservation) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Property COW exhausted a pinned pool without a fresh-pool reservation range");
			}
			FreshPropertyReplacementPool *destination = nullptr;
			auto existing = freshPropertyReplacementPools.find(request.expected_page_owner_id);
			if (existing != freshPropertyReplacementPools.end() &&
				existing->second.prepared_frames < RSS_PAGE_POOL_DATA_FRAME_COUNT) {
				destination = &existing->second;
			} else {
				const std::optional<PhysicalSlotId> physicalSlot =
						findFirstFreeNewPoolSlot(request.first_slot, request.last_slot);
				if (!physicalSlot.has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Property COW has no free physical slot for a fresh pool");
				}
				CatalogEntry entry;
				entry.segment_id = batch.allocateLogicalSegmentId();
				entry.physical_slot = *physicalSlot;
				entry.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
				entry.owner_id = request.expected_page_owner_id;
				entry.generation = Generation(1U);
				entry.state = SegmentState::SGS_SEALED;
				entry.committed_lsn = checkpointLsn;
				batch.reserveDataSlot(entry.physical_slot);
				freshPropertyReplacementPoolEntries.push_back(entry);
				auto [replacement, inserted] = freshPropertyReplacementPools.insert_or_assign(
						request.expected_page_owner_id, FreshPropertyReplacementPool{entry});
				static_cast<void>(inserted);
				destination = &replacement->second;
			}
			const bool initializesPool = destination->prepared_frames++ == 0U;
			propertyMaterializations[ordinal].pool = destination->entry;
			propertyMaterializations[ordinal].replacement = {
					propertyMaterializations[ordinal].expected_location.page_id, destination->entry.segment_id, 0U,
					destination->entry.generation, 0U};
			propertyMaterializations[ordinal].destination_is_new_pool = true;
			propertyMaterializations[ordinal].destination_initializes_new_pool = initializesPool;
		}
		std::vector<CatalogEntry> newPropertyPoolEntries;
		newPropertyPoolEntries.reserve(propertyPageBatchRequests.size());
		for (const NativeGraphPropertyPageBatchRequest &request: propertyPageBatchRequests) {
			CatalogEntry entry = request.expected_pool;
			if (request.kind == NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL) {
				const std::optional<PhysicalSlotId> physicalSlot =
						findFirstFreeNewPoolSlot(request.first_slot, request.last_slot);
				if (!physicalSlot.has_value()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph Property pool creation has no free physical slot in its request range");
				}
				entry.segment_id = batch.allocateLogicalSegmentId();
				entry.physical_slot = *physicalSlot;
				entry.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
				entry.owner_id = request.page_owner_id;
				entry.generation = Generation(1U);
				entry.state = SegmentState::SGS_SEALED;
				entry.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, entry.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph Property pool creation allocated a published logical Segment ID");
				}
				batch.reserveDataSlot(entry.physical_slot);
				newPropertyPoolEntries.push_back(entry);
			}

			std::vector<NativeGraphPropertyPageInitialMaterialization> initialPages;
			initialPages.reserve(request.page_property_keys.size());
			for (const std::vector<EntityPropertyKey> &pageKeys: request.page_property_keys) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId).has_value() || // logical Page IDs come from the monotonic batch allocator and are checked before materialization.
					!pageIds.insert(pageId.value()).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Property page batch allocated a published logical Page ID");
				}
				std::vector<EntityPropertyLocation> properties;
				properties.reserve(pageKeys.size());
				for (const EntityPropertyKey &key: pageKeys) {
					properties.push_back({key, pageId, std::numeric_limits<uint16_t>::max(), 0U});
				}
				initialPages.push_back({{pageId, entry.segment_id, 0U, entry.generation, 0U}, std::move(properties)});
			}
			propertyPageBatchMaterializations.push_back(
					{request.kind, entry, request.page_owner_id, std::move(initialPages)});
		}
		struct SharedLargeValuePoolCapacity {
			CatalogEntry pool;
			size_t available_frames = 0U;
			size_t prepared_frames = 0U;
			bool is_new_pool = false;
		};
		std::vector<SharedLargeValuePoolCapacity> sharedLargeValueDataPools;
		std::vector<SharedLargeValuePoolCapacity> sharedLargeValueManifestPools;
		if (!largeValues.empty()) {
			// Publication owns the writer lock, so a regular snapshot acquisition would
			// self-deadlock. This pin reads the already fenced root without reacquiring
			// the reader lock; the header checks below remain fail-closed.
			MetadataSnapshotPin poolSnapshot = snapshots_.pinCurrentUnderWrite(writer, current);
			for (const CatalogEntry &pool: findCurrentLargeValueDataPagePools(poolSnapshot)) {
				if (!allocator_.isAllocated(current, pool.physical_slot)) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue capacity query found an unallocated pool slot");
				}
				sharedLargeValueDataPools.push_back({pool, SharedPagePoolAllocator::availableFrameCountUnderWrite(
																   *io_, current.data_region_offset, pool)});
			}
			for (const CatalogEntry &pool: findCurrentLargeValueManifestPagePools(poolSnapshot)) {
				if (!allocator_.isAllocated(current, pool.physical_slot)) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph LargeValue Manifest capacity query found an unallocated pool slot");
				}
				sharedLargeValueManifestPools.push_back({pool, SharedPagePoolAllocator::availableFrameCountUnderWrite(
																	   *io_, current.data_region_offset, pool)});
			}
			poolSnapshot.release();
		}
		std::vector<CatalogEntry> newLargeValuePoolEntries;
		const auto allocateSharedLargeValuePages =
				[this, &batch, &current, &pageIds](std::vector<SharedLargeValuePoolCapacity> &candidates,
												   size_t &remainingPages,
												   std::vector<LargeValueDataMaterializationPoolBatch> &destination,
												   const char *duplicatePageMessage) {
					for (SharedLargeValuePoolCapacity &candidate: candidates) {
						if (remainingPages == 0U) {
							break;
						}
						if (candidate.available_frames == 0U) {
							continue;
						}
						const size_t pages = std::min(remainingPages, candidate.available_frames);
						const size_t preparedPrefix = candidate.is_new_pool ? candidate.prepared_frames : 0U;
						LargeValueDataMaterializationPoolBatch poolBatch{
								candidate.pool, {}, {}, candidate.is_new_pool, preparedPrefix};
						poolBatch.page_ids.reserve(pages);
						poolBatch.locations.reserve(pages);
						for (size_t pageOrdinal = 0U; pageOrdinal < pages; ++pageOrdinal) {
							const PageId pageId = batch.allocateLogicalPageId();
						if (locations_.findPageOptional(current, pageId).has_value() || // logical Page IDs come from the monotonic batch allocator and are checked before materialization.
								!pageIds.insert(pageId.value()).second) {
								throw FormatException(FormatError::FERR_INVALID_STATE, duplicatePageMessage);
							}
							poolBatch.page_ids.push_back(pageId);
							poolBatch.locations.push_back(
									{pageId, candidate.pool.segment_id, 0U, candidate.pool.generation, 0U});
						}
						destination.push_back(std::move(poolBatch));
						candidate.available_frames -= pages;
						if (candidate.is_new_pool) {
							candidate.prepared_frames += pages;
						}
						remainingPages -= pages;
					}
				};
		for (const NativeGraphLargeValueMutation &mutation: largeValues) {
			LargeValueMaterializationResult materialization;
			materialization.value_id = jointNativeContent ?
                LargeValueId(jointRowLargeFirst.value() + largeValueMaterializations.size()) : batch.allocateLargeValueId();
			if (materialization.value_id != mutation.reference.value_id) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph LargeValue allocation does not match its pinned logical value ID");
			}
			size_t remainingDataPages = mutation.planned_data_page_count;
			materialization.data_pools.reserve((remainingDataPages + RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U) /
											   RSS_PAGE_POOL_DATA_FRAME_COUNT);
			allocateSharedLargeValuePages(
					sharedLargeValueDataPools, remainingDataPages, materialization.data_pools,
					"native graph LargeValue allocation reused a published shared-pool data Page ID");
			while (remainingDataPages != 0U) {
				CatalogEntry pool;
				pool.segment_id = batch.allocateLogicalSegmentId();
				pool.physical_slot = reserveDistinctNewPoolSlot(
						mutation.first_data_slot, mutation.last_data_slot,
						"native graph LargeValue materialization has no free data-pool slot");
				pool.kind = SegmentKind::SGK_LARGE_VALUE_DATA_POOL;
				pool.owner_id = RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID;
				pool.generation = mutation.reference.generation;
				pool.state = SegmentState::SGS_SEALED;
				pool.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, pool.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue allocation reused a published logical Segment ID");
				}
				batch.reserveDataSlot(pool.physical_slot);
				newLargeValuePoolEntries.push_back(pool);
				sharedLargeValueDataPools.push_back({pool, RSS_PAGE_POOL_DATA_FRAME_COUNT, 0U, true});
				allocateSharedLargeValuePages(
						sharedLargeValueDataPools, remainingDataPages, materialization.data_pools,
						"native graph LargeValue allocation reused a published logical data Page ID");
			}
			size_t remainingManifestPages = mutation.planned_manifest_page_count;
			materialization.manifest_pools.reserve((remainingManifestPages + RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U) /
												   RSS_PAGE_POOL_DATA_FRAME_COUNT);
			allocateSharedLargeValuePages(
					sharedLargeValueManifestPools, remainingManifestPages, materialization.manifest_pools,
					"native graph LargeValue allocation reused a published shared-pool Manifest Page ID");
			while (remainingManifestPages != 0U) {
				CatalogEntry pool;
				pool.segment_id = batch.allocateLogicalSegmentId();
				pool.physical_slot = reserveDistinctNewPoolSlot(
						mutation.first_manifest_slot, mutation.last_manifest_slot,
						"native graph LargeValue materialization has no free manifest-pool slot");
				pool.kind = SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL;
				pool.owner_id = RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID;
				pool.generation = mutation.reference.generation;
				pool.state = SegmentState::SGS_SEALED;
				pool.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, pool.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue allocation reused a published manifest Segment ID");
				}
				batch.reserveDataSlot(pool.physical_slot);
				newLargeValuePoolEntries.push_back(pool);
				sharedLargeValueManifestPools.push_back({pool, RSS_PAGE_POOL_DATA_FRAME_COUNT, 0U, true});
				allocateSharedLargeValuePages(sharedLargeValueManifestPools, remainingManifestPages,
											  materialization.manifest_pools,
											  "native graph LargeValue allocation reused a published manifest Page ID");
			}
			materialization.manifest_pool = materialization.manifest_pools.front().pool;
			materialization.manifest_page_id = materialization.manifest_pools.front().page_ids.front();
			materialization.manifest_location = materialization.manifest_pools.front().locations.front();
			largeValueMaterializations.push_back(std::move(materialization));
		}
		// Allocate every fresh derived Index pool from this same candidate batch.
		// Its logical Segment/Page IDs and physical slot reservation therefore have
		// exactly the same visibility boundary as the Node, Edge, and Property work.
		std::vector<CatalogEntry> newDerivedIndexPoolEntries;
		for (size_t mutationOrdinal = 0U; mutationOrdinal < effectiveIndexMutations.size(); ++mutationOrdinal) {
			const NativeGraphIndexTreeMutation &mutation = effectiveIndexMutations[mutationOrdinal];
			std::vector<IndexTreeMutationPoolBatch> &batches = derivedIndexBatches[mutationOrdinal];
			for (const NewIndexTreePoolRequest &request: mutation.new_pool_requests) {
				const std::optional<PhysicalSlotId> physicalSlot =
						findFirstFreeNewPoolSlot(request.first_slot, request.last_slot);
				if (!physicalSlot.has_value()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph derived-state mutation has no free physical slot for a new Index pool");
				}
				const SegmentId segmentId = batch.allocateLogicalSegmentId();
				if (catalog_.findOptional(current, segmentId).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph derived-state mutation allocated a published Index Segment ID");
				}
				CatalogEntry pool;
				pool.segment_id = segmentId;
				pool.physical_slot = *physicalSlot;
				pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
				pool.owner_id = request.owner_id;
				pool.generation = Generation(1U);
				pool.state = SegmentState::SGS_SEALED;
				pool.committed_lsn = checkpointLsn;
				batch.reserveDataSlot(pool.physical_slot);

				IndexTreeMutationPoolBatch materialization;
				materialization.pool = pool;
				materialization.newly_reserved = true;
				materialization.appended_page_ids.reserve(request.appended_page_count);
				materialization.appended_locations.reserve(request.appended_page_count);
				for (size_t pageOrdinal = 0U; pageOrdinal < request.appended_page_count; ++pageOrdinal) {
					const PageId pageId = batch.allocateLogicalPageId();
					if (locations_.findPageOptional(current, pageId).has_value() || // logical Page IDs come from the monotonic batch allocator and are checked before materialization.
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph derived-state mutation allocated a published Index Page ID");
					}
					materialization.appended_page_ids.push_back(pageId);
					materialization.appended_locations.push_back({pageId, pool.segment_id, 0U, pool.generation, 0U});
				}
				newDerivedIndexPoolEntries.push_back(pool);
				batches.push_back(std::move(materialization));
			}
		}
		std::vector<NativeGraphVectorBootstrapMaterialization> vectorBootstrapMaterializations;
		std::vector<CatalogEntry> newVectorBootstrapPoolEntries;
		vectorBootstrapMaterializations.reserve(derivedState.vector_bootstraps.size());
		const auto resolveFreshDerivedRoot = [&effectiveIndexMutations, &derivedIndexBatches](uint64_t indexId) {
			const auto mutation =
					std::ranges::find(effectiveIndexMutations, indexId, &NativeGraphIndexTreeMutation::index_id);
			if (mutation == effectiveIndexMutations.end() || !mutation->replacement_root.has_value() || // Vector bootstrap preflight creates its matching freshly reserved mapping-root mutation before this private replay resolver runs.
				!mutation->replacement_root->newly_reserved) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap lost a freshly allocated mapping root");
			}
			const size_t mutationOrdinal = static_cast<size_t>(mutation - effectiveIndexMutations.begin());
			const NativeGraphIndexTreeAppendedPageReference &reference = *mutation->replacement_root;
			const size_t poolOrdinal = mutation->pool_requests.size() + reference.pool_ordinal;
			if (mutationOrdinal >= derivedIndexBatches.size() || // the replay batches are materialized one-for-one from the authenticated effective Index mutations.
				poolOrdinal >= derivedIndexBatches[mutationOrdinal].size() ||
				reference.appended_page_ordinal >=
						derivedIndexBatches[mutationOrdinal][poolOrdinal].appended_locations.size()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Vector bootstrap has an invalid freshly allocated mapping-root location");
			}
			const PageLocation &location = derivedIndexBatches[mutationOrdinal][poolOrdinal]
												   .appended_locations[reference.appended_page_ordinal];
			return IndexRootLocation{indexId, location.page_id, location.generation};
		};
		for (const NativeGraphVectorBootstrapMutation &mutation: derivedState.vector_bootstraps) {
			NativeGraphVectorBootstrapMaterialization materialization;
			materialization.mutation = mutation;
			materialization.node_mapping_root = resolveFreshDerivedRoot(mutation.index_id);
			materialization.vector_node_mapping_root = resolveFreshDerivedRoot(mutation.vector_node_mapping_index_id);
			for (uint64_t offset = 0U; offset < mutation.vector_count; ++offset) {
				if (batch.allocateLogicalVectorId() != mutation.first_vector_id + offset) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap lost its reserved logical Vector-ID range");
				}
			}
			const size_t rawPoolCount =
					(static_cast<size_t>(mutation.raw_page_count) - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
			materialization.raw_pools.reserve(rawPoolCount);
			materialization.raw_pages.reserve(mutation.raw_page_count);
			for (size_t poolOrdinal = 0U; poolOrdinal < rawPoolCount; ++poolOrdinal) {
				CatalogEntry pool;
				pool.segment_id = batch.allocateLogicalSegmentId();
				pool.physical_slot = reserveDistinctNewPoolSlot(
						mutation.first_slot, mutation.last_slot,
						"native graph Vector bootstrap has no free physical slot for a Raw pool");
				pool.kind = SegmentKind::SGK_VECTOR_RAW_POOL;
				pool.owner_id = mutation.index_id;
				pool.generation = Generation(1U);
				pool.state = SegmentState::SGS_SEALED;
				pool.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, pool.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap allocated a published Raw-pool Segment ID");
				}
				batch.reserveDataSlot(pool.physical_slot);
				materialization.raw_pools.push_back(pool);
				newVectorBootstrapPoolEntries.push_back(pool);
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < mutation.raw_page_count; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
						if (locations_.findPageOptional(current, pageId).has_value() || // vector delta Page IDs are allocated once by the authenticated batch.
					!pageIds.insert(pageId.value()).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap allocated a published Raw Page ID");
				}
				const CatalogEntry &pool = materialization.raw_pools[pageOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT];
				materialization.raw_pages.push_back({pageId, pool.segment_id, 0U, pool.generation, 0U});
			}
			materialization.manifest_pool.segment_id = batch.allocateLogicalSegmentId();
			materialization.manifest_pool.physical_slot = reserveDistinctNewPoolSlot(
					mutation.first_slot, mutation.last_slot,
					"native graph Vector bootstrap has no free physical slot for a Manifest pool");
			materialization.manifest_pool.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
			materialization.manifest_pool.owner_id = mutation.index_id;
			materialization.manifest_pool.generation = Generation(1U);
			materialization.manifest_pool.state = SegmentState::SGS_SEALED;
			materialization.manifest_pool.committed_lsn = checkpointLsn;
			if (catalog_.findOptional(current, materialization.manifest_pool.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap allocated a published Manifest-pool Segment ID");
			}
			batch.reserveDataSlot(materialization.manifest_pool.physical_slot);
			materialization.manifest_page.page_id = batch.allocateLogicalPageId();
			materialization.manifest_page.segment_id = materialization.manifest_pool.segment_id;
			materialization.manifest_page.generation = materialization.manifest_pool.generation;
			if (locations_.findPageOptional(current, materialization.manifest_page.page_id).has_value() || // the manifest Page ID is allocated once by the authenticated vector plan.
				!pageIds.insert(materialization.manifest_page.page_id.value()).second) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap allocated a published Manifest Page ID");
			}
			materialization.manifest_location = {mutation.index_id, materialization.manifest_page.page_id,
												 materialization.manifest_page.generation};
			newVectorBootstrapPoolEntries.push_back(materialization.manifest_pool);
			vectorBootstrapMaterializations.push_back(std::move(materialization));
		}
		for (const NativeGraphVectorDeltaMutation *mutation: vectorDeltasByVectorRange) {
			for (uint64_t offset = 0U; offset < mutation->inserted_vector_count; ++offset) {
				if (batch.allocateLogicalVectorId() != mutation->first_vector_id + offset) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta lost its reserved logical Vector-ID range");
				}
			}
		}
		std::vector<NativeGraphVectorDeltaMaterialization> vectorDeltaMaterializations;
		std::vector<CatalogEntry> newVectorDeltaPoolEntries;
		vectorDeltaMaterializations.reserve(derivedState.vector_deltas.size());
		for (const NativeGraphVectorDeltaMutation &mutation: derivedState.vector_deltas) {
			NativeGraphVectorDeltaMaterialization materialization;
			materialization.mutation = mutation;
			std::map<uint32_t, size_t> rawNewPoolOrdinals;
			for (const NativeGraphVectorDeltaRawPageMutation &rawMutation: mutation.raw_pages) {
				NativeGraphVectorDeltaRawPageMaterialization raw;
				raw.mutation = rawMutation;
				if (rawMutation.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL) {
					raw.pool = rawMutation.expected_pool;
					raw.location = rawMutation.expected_location;
				} else if (rawMutation.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_APPEND_TO_CURRENT_POOL) {
					raw.pool = rawMutation.expected_pool;
					const PageId pageId = batch.allocateLogicalPageId();
						if (locations_.findPageOptional(current, pageId).has_value() || // vector delta Page IDs are allocated once by the authenticated batch.
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph Vector delta allocated a published Raw Page ID");
					}
					raw.location = {pageId, raw.pool.segment_id, 0U, raw.pool.generation, 0U};
				} else {
					auto [position, inserted] = rawNewPoolOrdinals.try_emplace(rawMutation.new_pool_ordinal,
																			   materialization.raw_new_pools.size());
					if (inserted) {
						CatalogEntry pool;
						pool.segment_id = batch.allocateLogicalSegmentId();
						pool.physical_slot = reserveDistinctNewPoolSlot(
								mutation.first_slot, mutation.last_slot,
								"native graph Vector delta has no free physical slot for a Raw pool");
						pool.kind = SegmentKind::SGK_VECTOR_RAW_POOL;
						pool.owner_id = mutation.index_id;
						pool.generation = Generation(1U);
						pool.state = SegmentState::SGS_SEALED;
						pool.committed_lsn = checkpointLsn;
						if (catalog_.findOptional(current, pool.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"native graph Vector delta allocated a published Raw-pool Segment ID");
						}
						batch.reserveDataSlot(pool.physical_slot);
						newVectorDeltaPoolEntries.push_back(pool);
						materialization.raw_new_pools.push_back(pool);
					}
					raw.pool = materialization.raw_new_pools[position->second];
					const PageId pageId = batch.allocateLogicalPageId();
						if (locations_.findPageOptional(current, pageId).has_value() || // appended Index Page IDs are allocated once by the authenticated batch.
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph Vector delta allocated a published fresh Raw Page ID");
					}
					raw.location = {pageId, raw.pool.segment_id, 0U, raw.pool.generation, 0U};
				}
				materialization.raw_pages.push_back(std::move(raw));
			}
			if (mutation.manifest_in_new_pool) {
				materialization.manifest_new_pool.segment_id = batch.allocateLogicalSegmentId();
				materialization.manifest_new_pool.physical_slot = reserveDistinctNewPoolSlot(
						mutation.first_slot, mutation.last_slot,
						"native graph Vector delta has no free physical slot for a Manifest pool");
				materialization.manifest_new_pool.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
				materialization.manifest_new_pool.owner_id = mutation.index_id;
				materialization.manifest_new_pool.generation = Generation(1U);
				materialization.manifest_new_pool.state = SegmentState::SGS_SEALED;
				materialization.manifest_new_pool.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, materialization.manifest_new_pool.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta allocated a published Manifest-pool Segment ID");
				}
				batch.reserveDataSlot(materialization.manifest_new_pool.physical_slot);
				newVectorDeltaPoolEntries.push_back(materialization.manifest_new_pool);
			}
			const CatalogEntry &manifestPool =
					mutation.manifest_in_new_pool ? materialization.manifest_new_pool : mutation.expected_manifest_pool;
			materialization.manifest_replacement = {mutation.expected_manifest_location.page_id,
													manifestPool.segment_id, 0U, manifestPool.generation, 0U};
			materialization.manifest_location = {mutation.index_id, materialization.manifest_replacement.page_id,
												 materialization.manifest_replacement.generation};
			vectorDeltaMaterializations.push_back(std::move(materialization));
		}
		std::vector<CatalogEntry> allNewPoolEntries;
		allNewPoolEntries.reserve(newPoolEntries.size() + freshPropertyReplacementPoolEntries.size() +
								  newPropertyPoolEntries.size() + newLargeValuePoolEntries.size() +
								  newDerivedIndexPoolEntries.size() + newVectorBootstrapPoolEntries.size() +
								  newVectorDeltaPoolEntries.size());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newPoolEntries.begin(), newPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), freshPropertyReplacementPoolEntries.begin(),
								 freshPropertyReplacementPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newPropertyPoolEntries.begin(), newPropertyPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newLargeValuePoolEntries.begin(),
								 newLargeValuePoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newDerivedIndexPoolEntries.begin(),
								 newDerivedIndexPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newVectorBootstrapPoolEntries.begin(),
								 newVectorBootstrapPoolEntries.end());
		allNewPoolEntries.insert(allNewPoolEntries.end(), newVectorDeltaPoolEntries.begin(),
								 newVectorDeltaPoolEntries.end());
		for (const CatalogEntry &entry: allNewPoolEntries) {
			// Early streamed coordinators may have prepared a fresh pool under the
			// same MetadataWriteBatch. Its allocator transition is already part of
			// the candidate root, so never replay that transition.
			if (!allocator_.isAllocated(batch.candidate(), entry.physical_slot)) {
				allocator_.prepareSetAllocated(batch, entry.physical_slot, true);
			}
		}
		if (!allNewPoolEntries.empty()) {
			std::ranges::sort(allNewPoolEntries, {}, &CatalogEntry::segment_id);
			catalog_.prepareUpsert(batch, allNewPoolEntries);
		}
		for (size_t mutationOrdinal = 0U; mutationOrdinal < effectiveIndexMutations.size(); ++mutationOrdinal) {
			const NativeGraphIndexTreeMutation &mutation = effectiveIndexMutations[mutationOrdinal];
			std::vector<IndexTreeMutationPoolBatch> &batches = derivedIndexBatches[mutationOrdinal];
			for (size_t poolOrdinal = 0U; poolOrdinal < mutation.pool_requests.size(); ++poolOrdinal) {
				IndexTreeMutationPoolBatch &pool = batches[poolOrdinal];
				const size_t appendedCount = mutation.pool_requests[poolOrdinal].appended_page_count;
				pool.appended_page_ids.reserve(appendedCount);
				pool.appended_locations.reserve(appendedCount);
				for (size_t pageOrdinal = 0U; pageOrdinal < appendedCount; ++pageOrdinal) {
					const PageId pageId = batch.allocateLogicalPageId();
					if (locations_.findPageOptional(current, pageId).has_value() || // appended Index Page IDs are allocated once by the authenticated batch.
						!pageIds.insert(pageId.value()).second) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph derived-state mutation allocated a published appended Index Page ID");
					}
					pool.appended_page_ids.push_back(pageId);
					pool.appended_locations.push_back({pageId, pool.pool.segment_id, 0U, pool.pool.generation, 0U});
				}
			}
		}

		const std::vector<EntityPageReplacementMaterialization> plannedMaterializations = materializations;
		const std::vector<EntityPageAppendBatchMaterialization> plannedAppendMaterializations = appendMaterializations;
		const std::vector<EntityPageNewPoolBatchMaterialization> plannedNewPoolMaterializations =
				newPoolMaterializations;
		const std::vector<NativeGraphPropertyPageReplacementMaterialization> plannedPropertyMaterializations =
				propertyMaterializations;
		const std::vector<NativeGraphPropertyPageBatchMaterialization> plannedPropertyPageBatchMaterializations =
				propertyPageBatchMaterializations;
		const std::vector<LargeValueMaterializationResult> plannedLargeValueMaterializations =
				largeValueMaterializations;
		const std::vector<std::vector<IndexTreeMutationPoolBatch>> plannedDerivedIndexBatches = derivedIndexBatches;
		const std::vector<NativeGraphVectorBootstrapMaterialization> plannedVectorBootstrapMaterializations =
				vectorBootstrapMaterializations;
		if (hasLargeValueRetirements) {
			largeValueRetirementPreparer(largeValueRetirementBatches);
		}
		{
			debug::ScopedPerfTimer preparationTimer("native_graph.mutation.prepare_materializations");
			preparer(materializations, appendMaterializations, newPoolMaterializations, propertyMaterializations,
					 propertyPageBatchMaterializations, largeValueMaterializations);
		}
		if (!vectorBootstrapMaterializations.empty()) {
			vectorBootstrapPreparer(vectorBootstrapMaterializations);
		}
		try {
			if (privateSegmentSet != nullptr && privateSegmentPreparer) {
				privateSegmentPreparer(*privateSegmentSet, candidateReader);
			}
			if (!derivedState.empty()) {
				const size_t ordinaryCount = derivedState.index_tree_mutations.size();
				std::vector<std::vector<IndexTreeMutationPoolBatch>> ordinaryBatches(
						std::make_move_iterator(derivedIndexBatches.begin()),
						std::make_move_iterator(derivedIndexBatches.begin() + static_cast<ptrdiff_t>(ordinaryCount)));
				derivedStatePreparer(ordinaryBatches);
				std::move(ordinaryBatches.begin(), ordinaryBatches.end(), derivedIndexBatches.begin());
			}
			const size_t deferredOffset = derivedState.index_tree_mutations.size();
			if (derivedIndexBatches.size() != deferredOffset + deferredIndexPreparers.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph deferred Index planner changed its materialization partition");
			}
			for (size_t ordinal = 0U; ordinal < deferredIndexPreparers.size(); ++ordinal) {
				deferredIndexPreparers[ordinal](derivedIndexBatches[deferredOffset + ordinal]);
			}
			if (deferredPlanningSnapshot.has_value()) {
				deferredPlanningSnapshot->release();
			}
			privateScratchState->active = false;
		} catch (...) {
			if (deferredPlanningSnapshot.has_value()) {
				deferredPlanningSnapshot->release();
			}
			privateScratchState->active = false;
			throw;
		}
		if (privateSegmentSet != nullptr) {
			const size_t materializedCandidateCount = privateScratchState->compact_candidate.has_value()
																? privateScratchState->compact_candidate->materialized_entries.size()
																: privateSegmentCount;
			if (materializedCandidateCount > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "native graph private scratch materialization count exceeds PerfTrace range");
			}
			debug::PerfTrace::addValue("native_graph.streamed.scratch.candidate_segments",
									static_cast<int64_t>(materializedCandidateCount));
			debug::ScopedPerfTimer cleanupTimer("native_graph.mutation.private_scratch_cleanup");
			// Candidate scratch is only a writer-scoped transport. Authenticate every
			// frame before clearing it so allocator-free slots never retain a route-like
			// envelope after the final generation becomes visible.
			std::vector<std::byte> privateScratchCleanupImage(RSS_SEGMENT_SIZE);
			const auto validateCleanupEntry = [&](size_t ordinal, const CatalogEntry &entry) {
				const uint16_t headerBytes = privateHeaderBytesAt(ordinal);
				const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(
						current.data_region_offset, entry.physical_slot);
				const size_t bytesRead = io_->readPages(
						{offset, entry.generation.value()}, privateScratchCleanupImage.data(), privateScratchCleanupImage.size(),
						BufferPoolReadHint::BPRH_SEQUENTIAL);
				if (bytesRead != privateScratchCleanupImage.size()) {
					throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL,
										  "native graph private scratch cleanup found a truncated candidate segment");
				}
				const SegmentEnvelope envelope =
						SegmentEnvelopeCodec::decode(
								std::span<const std::byte>(privateScratchCleanupImage.data(), RSS_ENVELOPE_BYTES));
				const uint8_t invalid = static_cast<uint8_t>(envelope.kind != SegmentKind::SGK_BULK_SCRATCH) |
										static_cast<uint8_t>(envelope.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(envelope.segment_id != entry.segment_id) |
										static_cast<uint8_t>(envelope.owner_id != entry.owner_id) |
										static_cast<uint8_t>(envelope.generation != entry.generation) |
										static_cast<uint8_t>(envelope.committed_lsn != entry.committed_lsn) |
										static_cast<uint8_t>(envelope.header_bytes != headerBytes) |
										static_cast<uint8_t>(envelope.payload_bytes == 0U) |
										static_cast<uint8_t>(envelope.payload_bytes > RSS_SEGMENT_SIZE - headerBytes);
				if (invalid != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch cleanup found a foreign or invalid candidate segment");
				}
			};
			if (privateScratchState->compact_candidate.has_value()) {
				for (const auto &[ordinal, entry]: privateScratchState->compact_candidate->materialized_entries) {
					validateCleanupEntry(ordinal, entry);
				}
			} else {
				for (size_t ordinal = 0U; ordinal < privateSegmentCount; ++ordinal) {
					validateCleanupEntry(ordinal, privateEntryAt(ordinal));
				}
			}
			const std::array<std::byte, RSS_SEGMENT_SIZE> zeros{};
			const auto clearCleanupEntry = [&](const CatalogEntry &entry) {
				const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(
						current.data_region_offset, entry.physical_slot);
				io_->writePagesThrough({offset, entry.generation.value()}, zeros.data(), zeros.size());
			};
			if (privateScratchState->compact_candidate.has_value()) {
				for (const auto &[ordinal, entry]: privateScratchState->compact_candidate->materialized_entries) {
					static_cast<void>(ordinal);
					clearCleanupEntry(entry);
				}
			} else {
				for (size_t ordinal = 0U; ordinal < privateSegmentCount; ++ordinal) {
					clearCleanupEntry(privateEntryAt(ordinal));
				}
			}
		}
		debug::ScopedPerfTimer materializationTimer("native_graph.mutation.finalize_materializations");
		std::set<std::pair<uint64_t, uint8_t>> destinationFrames;
        // Private images participate in the same final destination validation
        // as every regular Entity/Property/Index materializer.
        const auto registerImageFrames = [&](const NativeGraphDeferredIndexImages &proof) {
            for (const auto &page : proof.installed_pages_)
                if (!destinationFrames.emplace(page.segment_id.value(), page.page_index).second)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "index image destinations overlap");
        };
        for (const auto &proof : derivedState.index_images) registerImageFrames(*proof);
        for (const auto &plan : deferredPlans) if (plan.images) registerImageFrames(*plan.images);
        const auto validateImagePools = [&](const auto &placement) {
            if (!placement) return;
            for (const auto &reservation : placement->reservations_) if (reservation.prepared) {
                const auto found = std::ranges::find(allNewPoolEntries, reservation.pool.segment_id, &CatalogEntry::segment_id);
                if (found != allNewPoolEntries.end() && !hasSameCatalogEntry(*found, reservation.pool))
                    throw FormatException(FormatError::FERR_INVALID_STATE, "index images conflict with another pool identity");
            }
        };
        validateImagePools(ordinaryIndexPlacement); validateImagePools(streamedAdjacencyPlacement);
        validateImagePools(streamedMembershipPlacement);

		for (size_t ordinal = 0U; ordinal < materializations.size(); ++ordinal) {
			const EntityPageReplacementMaterialization &materialization = materializations[ordinal];
			const EntityPageReplacementMaterialization &planned = plannedMaterializations[ordinal];
			const uint8_t invalid =
					static_cast<uint8_t>(materialization.pool.segment_id != planned.pool.segment_id) |
					static_cast<uint8_t>(materialization.pool.physical_slot != planned.pool.physical_slot) |
					static_cast<uint8_t>(materialization.pool.kind != planned.pool.kind) |
					static_cast<uint8_t>(materialization.pool.owner_id != planned.pool.owner_id) |
					static_cast<uint8_t>(materialization.pool.generation != planned.pool.generation) |
					static_cast<uint8_t>(materialization.pool.state != planned.pool.state) |
					static_cast<uint8_t>(materialization.pool.committed_lsn != planned.pool.committed_lsn) |
					static_cast<uint8_t>(materialization.expected_location != planned.expected_location) |
					static_cast<uint8_t>(materialization.retire_page != planned.retire_page) |
					static_cast<uint8_t>(materialization.destination_is_new_pool != planned.destination_is_new_pool) |
					static_cast<uint8_t>(materialization.owner_kind != planned.owner_kind) |
					static_cast<uint8_t>(materialization.pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
					static_cast<uint8_t>(materialization.pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!isEntityPoolPageOwnerKind(materialization.owner_kind));
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page batch replacement callback returned an invalid destination frame");
			}
			if (materialization.retire_page) {
				const uint8_t invalidRetirement =
						static_cast<uint8_t>(materialization.replacement.page_id.isValid()) |
						static_cast<uint8_t>(materialization.replacement.segment_id.isValid()) |
						static_cast<uint8_t>(materialization.replacement.page_index != 0U) |
						static_cast<uint8_t>(materialization.replacement.generation.isValid()) |
						static_cast<uint8_t>(materialization.replacement.free_bytes != 0U);
				if (invalidRetirement != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable Entity page retirement callback returned a replacement frame");
				}
				retiredPageIds.push_back(materialization.expected_location.page_id.value());
				continue;
			}
			const uint8_t invalidReplacement =
					static_cast<uint8_t>(materialization.replacement.page_id !=
										 materialization.expected_location.page_id) |
					static_cast<uint8_t>(materialization.replacement.segment_id != materialization.pool.segment_id) |
					static_cast<uint8_t>(materialization.replacement.generation != materialization.pool.generation) |
					static_cast<uint8_t>(materialization.replacement.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(materialization.replacement.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(
							materialization.replacement.segment_id == materialization.expected_location.segment_id &&
							materialization.replacement.page_index == materialization.expected_location.page_index) |
					static_cast<uint8_t>(!destinationFrames
												  .emplace(materialization.pool.segment_id.value(),
														   materialization.replacement.page_index)
												  .second);
			if (invalidReplacement != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page batch replacement callback returned an invalid destination frame");
			}
		}
		for (size_t ordinal = 0U; ordinal < appendMaterializations.size(); ++ordinal) {
			const EntityPageAppendBatchMaterialization &materialization = appendMaterializations[ordinal];
			const EntityPageAppendBatchMaterialization &planned = plannedAppendMaterializations[ordinal];
			const uint8_t invalid =
					static_cast<uint8_t>(materialization.pool.segment_id != planned.pool.segment_id) |
					static_cast<uint8_t>(materialization.pool.physical_slot != planned.pool.physical_slot) |
					static_cast<uint8_t>(materialization.pool.kind != planned.pool.kind) |
					static_cast<uint8_t>(materialization.pool.owner_id != planned.pool.owner_id) |
					static_cast<uint8_t>(materialization.pool.generation != planned.pool.generation) |
					static_cast<uint8_t>(materialization.pool.state != planned.pool.state) |
					static_cast<uint8_t>(materialization.pool.committed_lsn != planned.pool.committed_lsn) |
					static_cast<uint8_t>(materialization.owner_kind != planned.owner_kind) |
					static_cast<uint8_t>(materialization.appended_location.page_id !=
										 planned.appended_location.page_id) |
					static_cast<uint8_t>(materialization.appended_location.segment_id !=
										 materialization.pool.segment_id) |
					static_cast<uint8_t>(materialization.appended_location.generation !=
										 materialization.pool.generation) |
					static_cast<uint8_t>(materialization.appended_location.page_index <
										 RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(materialization.appended_location.page_index >
										 RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!isEntityPoolPageOwnerKind(materialization.owner_kind)) |
					static_cast<uint8_t>(!destinationFrames
												  .emplace(materialization.pool.segment_id.value(),
														   materialization.appended_location.page_index)
												  .second);
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page batch append callback returned an invalid destination frame");
			}
		}
		for (size_t ordinal = 0U; ordinal < newPoolMaterializations.size(); ++ordinal) {
			const EntityPageNewPoolBatchMaterialization &materialization = newPoolMaterializations[ordinal];
			const EntityPageNewPoolBatchMaterialization &planned = plannedNewPoolMaterializations[ordinal];
			const uint8_t invalid =
					static_cast<uint8_t>(materialization.pool.segment_id != planned.pool.segment_id) |
					static_cast<uint8_t>(materialization.pool.physical_slot != planned.pool.physical_slot) |
					static_cast<uint8_t>(materialization.pool.kind != planned.pool.kind) |
					static_cast<uint8_t>(materialization.pool.owner_id != planned.pool.owner_id) |
					static_cast<uint8_t>(materialization.pool.generation != planned.pool.generation) |
					static_cast<uint8_t>(materialization.pool.state != planned.pool.state) |
					static_cast<uint8_t>(materialization.pool.committed_lsn != planned.pool.committed_lsn) |
					static_cast<uint8_t>(materialization.owner_kind != planned.owner_kind) |
					static_cast<uint8_t>(materialization.initial_location.page_id != planned.initial_location.page_id) |
					static_cast<uint8_t>(materialization.initial_location.segment_id !=
										 materialization.pool.segment_id) |
					static_cast<uint8_t>(materialization.initial_location.generation !=
										 materialization.pool.generation) |
					static_cast<uint8_t>(materialization.initial_location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(materialization.initial_location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!isEntityPoolPageOwnerKind(materialization.owner_kind)) |
					static_cast<uint8_t>(!destinationFrames
												  .emplace(materialization.pool.segment_id.value(),
														   materialization.initial_location.page_index)
												  .second);
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page new-pool callback returned an invalid initial page frame");
			}
		}
		// The typed Entity writer owns payload construction, but the metadata
		// coordinator must authenticate every assigned frame before making its
		// Page Directory route reachable. This remains proportional to this
		// publication rather than scanning the complete Page Directory.
		std::map<uint64_t, std::pair<CatalogEntry, std::vector<SharedPagePoolPreparedPublication>>>
				entityPublicationFrames;
		const auto registerEntityPublication = [&entityPublicationFrames, checkpointLsn](const CatalogEntry &pool,
																						 const PageLocation &location,
																						 PageOwnerKind ownerKind) {
			auto [iterator, inserted] = entityPublicationFrames.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.first = pool;
			} else if (!hasSameCatalogEntry(iterator->second.first, pool)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Entity materialization reused a Segment ID with conflicting pool identity");
			}
			iterator->second.second.push_back({location, pool.owner_id, ownerKind, checkpointLsn});
		};
		for (const EntityPageReplacementMaterialization &materialization: materializations) {
			if (!materialization.retire_page) {
				registerEntityPublication(materialization.pool, materialization.replacement,
										  materialization.owner_kind);
			}
		}
		for (const EntityPageAppendBatchMaterialization &materialization: appendMaterializations) {
			registerEntityPublication(materialization.pool, materialization.appended_location,
									  materialization.owner_kind);
		}
		for (const EntityPageNewPoolBatchMaterialization &materialization: newPoolMaterializations) {
			registerEntityPublication(materialization.pool, materialization.initial_location,
									  materialization.owner_kind);
		}
		for (const auto &[segmentId, publication]: entityPublicationFrames) {
			static_cast<void>(segmentId);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(*io_, current.data_region_offset,
																			publication.first, publication.second);
		}
		for (size_t ordinal = 0U; ordinal < propertyMaterializations.size(); ++ordinal) {
			const NativeGraphPropertyPageReplacementMaterialization &materialization =
					propertyMaterializations[ordinal];
			const NativeGraphPropertyPageReplacementMaterialization &planned = plannedPropertyMaterializations[ordinal];
			const bool invalidReplacement =
					materialization.replacement.page_id != materialization.expected_location.page_id ||
					materialization.replacement.segment_id != materialization.pool.segment_id ||
					materialization.replacement.generation != materialization.pool.generation ||
					materialization.replacement.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME ||
					materialization.replacement.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
					(materialization.replacement.segment_id == materialization.expected_location.segment_id &&
					 materialization.replacement.page_index == materialization.expected_location.page_index) ||
					!destinationFrames
							 .emplace(materialization.pool.segment_id.value(), materialization.replacement.page_index)
							 .second;
			const uint8_t invalid =
					static_cast<uint8_t>(materialization.pool.segment_id != planned.pool.segment_id) |
					static_cast<uint8_t>(materialization.pool.physical_slot != planned.pool.physical_slot) |
					static_cast<uint8_t>(materialization.pool.kind != planned.pool.kind) |
					static_cast<uint8_t>(materialization.pool.owner_id != planned.pool.owner_id) |
					static_cast<uint8_t>(materialization.pool.generation != planned.pool.generation) |
					static_cast<uint8_t>(materialization.pool.state != planned.pool.state) |
					static_cast<uint8_t>(materialization.pool.committed_lsn != planned.pool.committed_lsn) |
					static_cast<uint8_t>(materialization.expected_location != planned.expected_location) |
					static_cast<uint8_t>(materialization.page_owner_id != planned.page_owner_id) |
					static_cast<uint8_t>(materialization.expected_owner_lsn != planned.expected_owner_lsn) |
					static_cast<uint8_t>(materialization.retire_page != planned.retire_page) |
					static_cast<uint8_t>(materialization.destination_is_new_pool != planned.destination_is_new_pool) |
					static_cast<uint8_t>(materialization.destination_initializes_new_pool !=
										 planned.destination_initializes_new_pool) |
					static_cast<uint8_t>(materialization.pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
					static_cast<uint8_t>(materialization.pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(materialization.page_owner_id == 0U) |
					static_cast<uint8_t>(!materialization.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(!materialization.retire_page && invalidReplacement);
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph page transaction callback returned an invalid Property page frame");
			}
			if (materialization.retire_page) {
				const uint8_t invalidRetirement =
						static_cast<uint8_t>(materialization.replacement.page_id.isValid()) |
						static_cast<uint8_t>(materialization.replacement.segment_id.isValid()) |
						static_cast<uint8_t>(materialization.replacement.page_index != 0U) |
						static_cast<uint8_t>(materialization.replacement.generation.isValid()) |
						static_cast<uint8_t>(materialization.replacement.free_bytes != 0U);
				if (invalidRetirement != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph Property page retirement callback returned a replacement frame");
				}
				retiredPageIds.push_back(materialization.expected_location.page_id.value());
			}
		}
		for (size_t ordinal = 0U; ordinal < propertyPageBatchMaterializations.size(); ++ordinal) {
			const NativeGraphPropertyPageBatchMaterialization &materialization =
					propertyPageBatchMaterializations[ordinal];
			const NativeGraphPropertyPageBatchMaterialization &planned =
					plannedPropertyPageBatchMaterializations[ordinal];
			const uint8_t invalidPool =
					static_cast<uint8_t>(materialization.kind != planned.kind) |
					static_cast<uint8_t>(!hasSameCatalogEntry(materialization.pool, planned.pool)) |
					static_cast<uint8_t>(materialization.pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
					static_cast<uint8_t>(materialization.pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(materialization.page_owner_id != planned.page_owner_id) |
					static_cast<uint8_t>(materialization.page_owner_id == 0U) |
					static_cast<uint8_t>(materialization.pages.size() != planned.pages.size()) |
					static_cast<uint8_t>(materialization.pages.empty()) |
					static_cast<uint8_t>(materialization.pages.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph page transaction callback changed a Property page-batch identity");
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < materialization.pages.size(); ++pageOrdinal) {
				const NativeGraphPropertyPageInitialMaterialization &page = materialization.pages[pageOrdinal];
				const NativeGraphPropertyPageInitialMaterialization &expectedPage = planned.pages[pageOrdinal];
				const uint8_t invalidPage =
						static_cast<uint8_t>(page.location.page_id != expectedPage.location.page_id) |
						static_cast<uint8_t>(page.location.segment_id != materialization.pool.segment_id) |
						static_cast<uint8_t>(page.location.generation != materialization.pool.generation) |
						static_cast<uint8_t>(page.location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(page.location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(page.location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
						static_cast<uint8_t>(page.upserted_properties.size() !=
											 expectedPage.upserted_properties.size()) |
						static_cast<uint8_t>(
								!destinationFrames
										 .emplace(materialization.pool.segment_id.value(), page.location.page_index)
										 .second);
				if (invalidPage != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph page transaction callback returned an invalid Property page frame");
				}
				for (size_t propertyOrdinal = 0U; propertyOrdinal < page.upserted_properties.size();
					 ++propertyOrdinal) {
					const EntityPropertyLocation &property = page.upserted_properties[propertyOrdinal];
					const EntityPropertyLocation &expected = expectedPage.upserted_properties[propertyOrdinal];
					const uint8_t invalidProperty =
							static_cast<uint8_t>(property.key != expected.key) |
							static_cast<uint8_t>(property.page_id != page.location.page_id) |
							static_cast<uint8_t>(property.slot_id == std::numeric_limits<uint16_t>::max()) |
							static_cast<uint8_t>(property.slot_generation == 0U) |
							static_cast<uint8_t>(
									!publishedPropertySlots.emplace(property.page_id.value(), property.slot_id).second);
					if (invalidProperty != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph Property page callback returned an invalid directory location");
					}
					upsertedEntityProperties.push_back(property);
				}
			}
		}
		// Property pages are owner-keyed slotted payloads. Authenticate their
		// physical headers after the typed preparer has encoded them and before
		// the owner-keyed Property Directory can publish a route to either page.
		std::map<uint64_t, std::pair<CatalogEntry, std::vector<SharedPagePoolPreparedPublication>>>
				propertyPublicationFrames;
		const auto registerPropertyPublication = [&propertyPublicationFrames,
												  checkpointLsn](const CatalogEntry &pool, const PageLocation &location,
																 uint64_t pageOwnerId) {
			auto [iterator, inserted] = propertyPublicationFrames.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.first = pool;
			} else if (!hasSameCatalogEntry(iterator->second.first, pool)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Property materialization reused a Segment ID with conflicting pool identity");
			}
			iterator->second.second.push_back({location, pageOwnerId, PageOwnerKind::POK_PROPERTY, checkpointLsn});
		};
		for (const NativeGraphPropertyPageReplacementMaterialization &materialization: propertyMaterializations) {
			if (!materialization.retire_page) {
				registerPropertyPublication(materialization.pool, materialization.replacement,
											materialization.page_owner_id);
			}
		}
		for (const NativeGraphPropertyPageBatchMaterialization &materialization: propertyPageBatchMaterializations) {
			for (const NativeGraphPropertyPageInitialMaterialization &page: materialization.pages) {
				registerPropertyPublication(materialization.pool, page.location, materialization.page_owner_id);
			}
		}
		for (const auto &[segmentId, publication]: propertyPublicationFrames) {
			static_cast<void>(segmentId);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(*io_, current.data_region_offset,
																			publication.first, publication.second);
		}
		std::vector<PageLocation> publishedLargeValuePages;
		std::vector<LargeValueManifestLocation> publishedLargeValueManifests;
		std::map<uint64_t, std::pair<CatalogEntry, std::vector<SharedPagePoolPreparedPublication>>>
				largeValuePublicationFrames;
		const auto registerLargeValuePublication = [&largeValuePublicationFrames, checkpointLsn](
														   const CatalogEntry &pool, const PageLocation &location,
														   uint64_t valueId, PageOwnerKind ownerKind) {
			auto [iterator, inserted] = largeValuePublicationFrames.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.first = pool;
			} else if (!hasSameCatalogEntry(iterator->second.first, pool)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph LargeValue materialization reused a Segment ID with conflicting pool identity");
			}
			iterator->second.second.push_back({location, valueId, ownerKind, checkpointLsn});
		};
		for (size_t ordinal = 0U; ordinal < largeValueMaterializations.size(); ++ordinal) {
			const LargeValueMaterializationResult &materialization = largeValueMaterializations[ordinal];
			const LargeValueMaterializationResult &planned = plannedLargeValueMaterializations[ordinal];
			const NativeGraphLargeValueMutation &mutation = largeValues[ordinal];
			const bool hasManifestPool = !materialization.manifest_pools.empty();
			const uint8_t invalidManifest =
					static_cast<uint8_t>(materialization.value_id != planned.value_id) |
					static_cast<uint8_t>(materialization.value_id != mutation.reference.value_id) |
					static_cast<uint8_t>(materialization.data_pools.size() != planned.data_pools.size()) |
					static_cast<uint8_t>(materialization.manifest_pools.size() != planned.manifest_pools.size()) |
					static_cast<uint8_t>(!hasSameCatalogEntry(materialization.manifest_pool, planned.manifest_pool)) |
					static_cast<uint8_t>(materialization.manifest_page_id != planned.manifest_page_id) |
					static_cast<uint8_t>(materialization.manifest_location.page_id != planned.manifest_page_id) |
					static_cast<uint8_t>(materialization.manifest_location.segment_id !=
										 materialization.manifest_pool.segment_id) |
					static_cast<uint8_t>(materialization.manifest_location.generation !=
										 mutation.reference.generation) |
					static_cast<uint8_t>(materialization.manifest_location.page_index <
										 RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(materialization.manifest_location.page_index >
										 RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(materialization.manifest_location.free_bytes != 0U) |
					static_cast<uint8_t>(!hasManifestPool);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph LargeValue preparer changed its manifest identity or frame");
			}
			const LargeValueManifestMaterializationPoolBatch &firstManifestPool =
					materialization.manifest_pools.front();
			const uint8_t invalidManifestRoot =
					static_cast<uint8_t>(!hasSameCatalogEntry(materialization.manifest_pool, firstManifestPool.pool)) |
					static_cast<uint8_t>(firstManifestPool.page_ids.empty()) |
					static_cast<uint8_t>(firstManifestPool.locations.empty()) |
					static_cast<uint8_t>(firstManifestPool.page_ids.front() != materialization.manifest_page_id) |
					static_cast<uint8_t>(firstManifestPool.locations.front() != materialization.manifest_location);
			if (invalidManifestRoot != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph LargeValue preparer changed its manifest root frame");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < materialization.data_pools.size(); ++poolOrdinal) {
				const LargeValueDataMaterializationPoolBatch &pool = materialization.data_pools[poolOrdinal];
				const LargeValueDataMaterializationPoolBatch &expected = planned.data_pools[poolOrdinal];
				const uint8_t invalidPool =
						static_cast<uint8_t>(!hasSameCatalogEntry(pool.pool, expected.pool)) |
						static_cast<uint8_t>(pool.page_ids != expected.page_ids) |
						static_cast<uint8_t>(pool.is_new_pool != expected.is_new_pool) |
						static_cast<uint8_t>(pool.prepared_prefix_count != expected.prepared_prefix_count) |
						static_cast<uint8_t>(!pool.is_new_pool && pool.prepared_prefix_count != 0U) |
						static_cast<uint8_t>(pool.page_ids.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(
								pool.is_new_pool &&
								(pool.prepared_prefix_count > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
								 pool.page_ids.size() >
										 RSS_PAGE_POOL_DATA_FRAME_COUNT -
												 std::min(pool.prepared_prefix_count,
														  static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT)))) |
						static_cast<uint8_t>(pool.locations.size() != pool.page_ids.size());
				if (invalidPool != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue preparer changed a data-pool identity or Page IDs");
				}
				for (size_t pageOrdinal = 0U; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
					const PageLocation &location = pool.locations[pageOrdinal];
					const uint8_t invalidLocation =
							static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
							static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
							static_cast<uint8_t>(location.generation != pool.pool.generation) |
							static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
							static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
							static_cast<uint8_t>(location.free_bytes != 0U) |
							static_cast<uint8_t>(pool.is_new_pool &&
												 location.page_index != pool.prepared_prefix_count + pageOrdinal + 1U) |
							static_cast<uint8_t>(
									!destinationFrames.emplace(location.segment_id.value(), location.page_index)
											 .second);
					if (invalidLocation != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph LargeValue preparer returned an invalid or duplicate data frame");
					}
					registerLargeValuePublication(pool.pool, location, mutation.reference.value_id.value(),
												  PageOwnerKind::POK_LARGE_VALUE_DATA);
					publishedLargeValuePages.push_back(location);
				}
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < materialization.manifest_pools.size(); ++poolOrdinal) {
				const LargeValueManifestMaterializationPoolBatch &pool = materialization.manifest_pools[poolOrdinal];
				const LargeValueManifestMaterializationPoolBatch &expected = planned.manifest_pools[poolOrdinal];
				const uint8_t invalidPool =
						static_cast<uint8_t>(!hasSameCatalogEntry(pool.pool, expected.pool)) |
						static_cast<uint8_t>(pool.page_ids != expected.page_ids) |
						static_cast<uint8_t>(pool.is_new_pool != expected.is_new_pool) |
						static_cast<uint8_t>(pool.prepared_prefix_count != expected.prepared_prefix_count) |
						static_cast<uint8_t>(!pool.is_new_pool && pool.prepared_prefix_count != 0U) |
						static_cast<uint8_t>(pool.page_ids.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(
								pool.is_new_pool &&
								(pool.prepared_prefix_count > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
								 pool.page_ids.size() >
										 RSS_PAGE_POOL_DATA_FRAME_COUNT -
												 std::min(pool.prepared_prefix_count,
														  static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT)))) |
						static_cast<uint8_t>(pool.locations.size() != pool.page_ids.size());
				if (invalidPool != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph LargeValue preparer changed a manifest-pool identity or Page IDs");
				}
				for (size_t pageOrdinal = 0U; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
					const PageLocation &location = pool.locations[pageOrdinal];
					const uint8_t invalidLocation =
							static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
							static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
							static_cast<uint8_t>(location.generation != pool.pool.generation) |
							static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
							static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
							static_cast<uint8_t>(location.free_bytes != 0U) |
							static_cast<uint8_t>(pool.is_new_pool &&
												 location.page_index != pool.prepared_prefix_count + pageOrdinal + 1U) |
							static_cast<uint8_t>(
									!destinationFrames.emplace(location.segment_id.value(), location.page_index)
											 .second);
					if (invalidLocation != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph LargeValue preparer returned an invalid or duplicate manifest frame");
					}
					registerLargeValuePublication(pool.pool, location, mutation.reference.value_id.value(),
												  PageOwnerKind::POK_LARGE_VALUE_MANIFEST);
					publishedLargeValuePages.push_back(location);
				}
			}
			publishedLargeValueManifests.push_back({mutation.reference.value_id, materialization.manifest_page_id,
													materialization.manifest_pool.generation});
		}
		for (const auto &[segmentId, publication]: largeValuePublicationFrames) {
			static_cast<void>(segmentId);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(*io_, current.data_region_offset,
																			publication.first, publication.second);
		}

		std::vector<PageLocation> publishedDerivedIndexPages;
		std::vector<IndexRootLocation> publishedDerivedIndexRoots;
		std::vector<uint64_t> erasedDerivedIndexRootIds;
		std::map<uint64_t, std::pair<CatalogEntry, std::vector<SharedPagePoolPreparedPublication>>>
				derivedIndexPublicationFrames;
		const auto registerDerivedIndexPublication = [&derivedIndexPublicationFrames,
													  checkpointLsn](const CatalogEntry &pool,
																	 const PageLocation &location, uint64_t indexId) {
			auto [iterator, inserted] = derivedIndexPublicationFrames.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.first = pool;
			} else if (!hasSameCatalogEntry(iterator->second.first, pool)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Index materialization reused a Segment ID with conflicting pool identity");
			}
			iterator->second.second.push_back({location, indexId, PageOwnerKind::POK_INDEX, checkpointLsn});
		};
		publishedDerivedIndexRoots.reserve(effectiveIndexMutations.size());
		erasedDerivedIndexRootIds.reserve(effectiveIndexMutations.size());
		for (size_t mutationOrdinal = 0U; mutationOrdinal < effectiveIndexMutations.size(); ++mutationOrdinal) {
			const NativeGraphIndexTreeMutation &mutation = effectiveIndexMutations[mutationOrdinal];
			const std::vector<IndexTreeMutationPoolBatch> &planned = plannedDerivedIndexBatches[mutationOrdinal];
			const std::vector<IndexTreeMutationPoolBatch> &materialized = derivedIndexBatches[mutationOrdinal];
			if (materialized.size() != planned.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph derived-state preparer changed its Index pool batch cardinality");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < materialized.size(); ++poolOrdinal) {
				const IndexTreeMutationPoolBatch &expected = planned[poolOrdinal];
				const IndexTreeMutationPoolBatch &actual = materialized[poolOrdinal];
				const bool invalidShape = // actual and expected pool records are emitted from the same authenticated materialization plan.
						actual.pool.segment_id != expected.pool.segment_id ||
						actual.pool.physical_slot != expected.pool.physical_slot ||
						actual.pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL ||
						actual.pool.kind != expected.pool.kind || actual.pool.owner_id != expected.pool.owner_id ||
						actual.pool.generation != expected.pool.generation ||
						actual.pool.state != SegmentState::SGS_SEALED || actual.pool.state != expected.pool.state ||
						actual.pool.committed_lsn != expected.pool.committed_lsn ||
						actual.replacement_pool.segment_id != expected.replacement_pool.segment_id ||
						actual.replacement_pool.physical_slot != expected.replacement_pool.physical_slot ||
						actual.replacement_pool.kind != expected.replacement_pool.kind ||
						actual.replacement_pool.owner_id != expected.replacement_pool.owner_id ||
						actual.replacement_pool.generation != expected.replacement_pool.generation ||
						actual.replacement_pool.state != expected.replacement_pool.state ||
						actual.replacement_pool.committed_lsn != expected.replacement_pool.committed_lsn ||
						actual.expected_replacements != expected.expected_replacements ||
						actual.appended_page_ids != expected.appended_page_ids ||
						actual.expected_retirements != expected.expected_retirements ||
						actual.newly_reserved != expected.newly_reserved ||
						actual.prepared_new_pool_prefix_count != expected.prepared_new_pool_prefix_count ||
						actual.replacement_locations.size() != expected.replacement_locations.size() ||
						actual.appended_locations.size() != expected.appended_locations.size();
				if (invalidShape) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph derived-state preparer changed an Index pool identity or page shape");
				}
				const auto validatePublishedLocation =
						[&destinationFrames, &publishedDerivedIndexPages, &mutation,
						 &registerDerivedIndexPublication](const PageLocation &location, PageId expectedPageId,
														 const CatalogEntry &expectedPool) {
							const uint8_t invalid =
									static_cast<uint8_t>(location.page_id != expectedPageId) |
									static_cast<uint8_t>(location.segment_id != expectedPool.segment_id) |
									static_cast<uint8_t>(location.generation != expectedPool.generation) |
									static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
									static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
									static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
									static_cast<uint8_t>(
											!destinationFrames.emplace(location.segment_id.value(), location.page_index)
													 .second);
							if (invalid != 0U) {
								throw FormatException(
										FormatError::FERR_INVALID_STATE,
										"native graph derived-state preparer returned an invalid Index page frame");
							}
							publishedDerivedIndexPages.push_back(location);
							registerDerivedIndexPublication(expectedPool, location, mutation.index_id);
						};
				for (size_t pageOrdinal = 0U; pageOrdinal < actual.replacement_locations.size(); ++pageOrdinal) {
					validatePublishedLocation(actual.replacement_locations[pageOrdinal],
											  actual.expected_replacements[pageOrdinal].page_id,
											  actual.replacement_pool.segment_id.isValid() ? actual.replacement_pool : actual.pool);
				}
				for (size_t pageOrdinal = 0U; pageOrdinal < actual.appended_locations.size(); ++pageOrdinal) {
					validatePublishedLocation(actual.appended_locations[pageOrdinal],
											  actual.appended_page_ids[pageOrdinal], actual.pool);
				}
				for (const IndexTreePageRetirement &retirement: actual.expected_retirements) {
					retiredPageIds.push_back(retirement.location.page_id.value());
				}
			}
			if (mutation.retire_root) {
				erasedDerivedIndexRootIds.push_back(mutation.index_id);
				continue;
			}

			IndexRootLocation publishedRoot{mutation.index_id, {}, {}};
			if (mutation.expected_root.has_value()) {
				publishedRoot = *mutation.expected_root;
			}
			if (mutation.replacement_root.has_value()) {
				const NativeGraphIndexTreeAppendedPageReference &reference = *mutation.replacement_root;
				const size_t poolOrdinal = reference.newly_reserved
												   ? mutation.pool_requests.size() + reference.pool_ordinal
												   : reference.pool_ordinal;
				const IndexTreeMutationPoolBatch &pool = materialized[poolOrdinal];
				const PageLocation &location = pool.appended_locations[reference.appended_page_ordinal];
				publishedRoot.root_page_id = location.page_id;
				publishedRoot.root_generation = location.generation;
			} else if (mutation.stable_replacement_root.has_value()) {
				const PageId pageId = *mutation.stable_replacement_root;
				const auto published = std::ranges::find(publishedDerivedIndexPages, pageId, &PageLocation::page_id);
				const PageLocation location = published == publishedDerivedIndexPages.end()
													  ? locations_.findPage(current, pageId)
													  : *published;
				const auto newPool =
						std::ranges::find(newDerivedIndexPoolEntries, location.segment_id, &CatalogEntry::segment_id);
				const CatalogEntry &pool = newPool == newDerivedIndexPoolEntries.end()
												   ? catalog_.find(current, location.segment_id)
												   : *newPool;
				const uint8_t invalidStableRoot =
						static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
						static_cast<uint8_t>(pool.owner_id == 0U) |
						static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(pool.generation != location.generation) |
						static_cast<uint8_t>(newPool == newDerivedIndexPoolEntries.end() &&
											 !allocator_.isAllocated(current, pool.physical_slot));
				if (invalidStableRoot != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph derived-state mutation resolved an invalid stable Index root");
				}
				publishedRoot.root_page_id = pageId;
				publishedRoot.root_generation = location.generation;
			}
			publishedDerivedIndexRoots.push_back(publishedRoot);
		}
		for (NativeGraphVectorDeltaMaterialization &materialization: vectorDeltaMaterializations) {
			const auto nodeRoot = std::ranges::find(publishedDerivedIndexRoots, materialization.mutation.index_id,
													&IndexRootLocation::index_id);
			const auto reverseRoot =
					std::ranges::find(publishedDerivedIndexRoots, materialization.mutation.vector_node_mapping_index_id,
									  &IndexRootLocation::index_id);
			const auto deltaRoot = materialization.mutation.delta_mapping_index_id == 0U
										   ? publishedDerivedIndexRoots.end()
										   : std::ranges::find(publishedDerivedIndexRoots,
															   materialization.mutation.delta_mapping_index_id,
															   &IndexRootLocation::index_id);
			if (nodeRoot == publishedDerivedIndexRoots.end() || reverseRoot == publishedDerivedIndexRoots.end() || // vector root identities are authenticated before publication replay.
				(materialization.mutation.delta_mapping_index_id != 0U &&
				 deltaRoot == publishedDerivedIndexRoots.end())) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Vector delta lost a published semantic or overlay mapping-tree root");
			}
			materialization.node_mapping_root = *nodeRoot;
			materialization.vector_node_mapping_root = *reverseRoot;
			if (deltaRoot != publishedDerivedIndexRoots.end()) {
				materialization.delta_mapping_root = *deltaRoot;
			}
		}
		const std::vector<NativeGraphVectorDeltaMaterialization> plannedVectorDeltaMaterializations =
				vectorDeltaMaterializations;
		if (!vectorDeltaMaterializations.empty()) {
			vectorDeltaPreparer(vectorDeltaMaterializations);
		}

		std::vector<PageLocation> publishedVectorBootstrapPages;
		std::vector<VectorLocation> publishedVectorBootstrapLocations;
		std::vector<VectorManifestLocation> publishedVectorBootstrapManifests;
		std::map<uint64_t, std::pair<CatalogEntry, std::vector<SharedPagePoolPreparedPublication>>>
				vectorPublicationFrames;
		const auto registerVectorPublication = [&vectorPublicationFrames,
												checkpointLsn](const CatalogEntry &pool, const PageLocation &location,
															   uint64_t indexId, PageOwnerKind ownerKind) {
			const bool expectedPoolKind =
					(ownerKind == PageOwnerKind::POK_VECTOR_RAW && pool.kind == SegmentKind::SGK_VECTOR_RAW_POOL) ||
					(ownerKind == PageOwnerKind::POK_VECTOR_MANIFEST &&
					 pool.kind == SegmentKind::SGK_VECTOR_MANIFEST_POOL);
			if (!expectedPoolKind || pool.owner_id != indexId) { // vector materialization supplies a pool kind and owner paired by preflight.
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector materialization has an incompatible pool or owner identity");
			}
			auto [iterator, inserted] = vectorPublicationFrames.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.first = pool;
			} else if (!hasSameCatalogEntry(iterator->second.first, pool)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Vector materialization reused a Segment ID with conflicting pool identity");
			}
			iterator->second.second.push_back({location, indexId, ownerKind, checkpointLsn});
		};
		std::set<std::pair<uint64_t, uint16_t>> publishedVectorBootstrapSlots;
		for (size_t mutationOrdinal = 0U; mutationOrdinal < vectorBootstrapMaterializations.size(); ++mutationOrdinal) {
			const NativeGraphVectorBootstrapMaterialization &actual = vectorBootstrapMaterializations[mutationOrdinal];
			const NativeGraphVectorBootstrapMaterialization &planned =
					plannedVectorBootstrapMaterializations[mutationOrdinal];
			const NativeGraphVectorBootstrapMutation &mutation = planned.mutation;
			const uint8_t invalidIdentity =
					static_cast<uint8_t>(actual.mutation.index_id != mutation.index_id) |
					static_cast<uint8_t>(actual.mutation.vector_node_mapping_index_id !=
										 mutation.vector_node_mapping_index_id) |
					static_cast<uint8_t>(actual.mutation.first_vector_id != mutation.first_vector_id) |
					static_cast<uint8_t>(actual.mutation.vector_count != mutation.vector_count) |
					static_cast<uint8_t>(actual.mutation.dimensions != mutation.dimensions) |
					static_cast<uint8_t>(actual.mutation.metric != mutation.metric) |
					static_cast<uint8_t>(actual.node_mapping_root != planned.node_mapping_root) |
					static_cast<uint8_t>(actual.vector_node_mapping_root != planned.vector_node_mapping_root) |
					static_cast<uint8_t>(actual.raw_pools.size() != planned.raw_pools.size()) |
					static_cast<uint8_t>(actual.raw_pages.size() != planned.raw_pages.size()) |
					static_cast<uint8_t>(actual.vector_locations.size() != mutation.vector_count) |
					static_cast<uint8_t>(actual.manifest_location != planned.manifest_location);
			if (invalidIdentity != 0U || !hasSameCatalogEntry(actual.manifest_pool, planned.manifest_pool)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap preparer changed a metadata-owned identity");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < actual.raw_pools.size(); ++poolOrdinal) {
				if (!hasSameCatalogEntry(actual.raw_pools[poolOrdinal], planned.raw_pools[poolOrdinal])) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap preparer changed a Raw-pool identity");
				}
			}
			std::set<uint64_t> rawPageIds;
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.raw_pages.size(); ++pageOrdinal) {
				const PageLocation &location = actual.raw_pages[pageOrdinal];
				const PageLocation &expected = planned.raw_pages[pageOrdinal];
				const CatalogEntry &pool = actual.raw_pools[pageOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != expected.page_id) |
						static_cast<uint8_t>(location.segment_id != pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
						static_cast<uint8_t>(!rawPageIds.insert(location.page_id.value()).second) |
						static_cast<uint8_t>(
								!destinationFrames.emplace(location.segment_id.value(), location.page_index).second);
				if (invalidLocation != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap preparer returned an invalid Raw page frame");
				}
				publishedVectorBootstrapPages.push_back(location);
				registerVectorPublication(pool, location, mutation.index_id, PageOwnerKind::POK_VECTOR_RAW);
			}
			const PageLocation &manifest = actual.manifest_page;
			const uint8_t invalidManifest =
					static_cast<uint8_t>(manifest.page_id != planned.manifest_page.page_id) |
					static_cast<uint8_t>(manifest.segment_id != actual.manifest_pool.segment_id) |
					static_cast<uint8_t>(manifest.generation != actual.manifest_pool.generation) |
					static_cast<uint8_t>(manifest.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(manifest.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(manifest.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
					static_cast<uint8_t>(
							!destinationFrames.emplace(manifest.segment_id.value(), manifest.page_index).second);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap preparer returned an invalid Manifest page frame");
			}
			registerVectorPublication(actual.manifest_pool, manifest, mutation.index_id,
									  PageOwnerKind::POK_VECTOR_MANIFEST);
			for (size_t vectorOrdinal = 0U; vectorOrdinal < actual.vector_locations.size(); ++vectorOrdinal) {
				const VectorLocation &location = actual.vector_locations[vectorOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.vector_id != mutation.first_vector_id + vectorOrdinal) |
						static_cast<uint8_t>(location.slot_generation == 0U) |
						static_cast<uint8_t>(!rawPageIds.contains(location.page_id.value())) |
						static_cast<uint8_t>(
								!publishedVectorBootstrapSlots.emplace(location.page_id.value(), location.slot_id)
										 .second);
				if (invalidLocation != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap preparer returned an invalid Vector location");
				}
				publishedVectorBootstrapLocations.push_back(location);
			}
			publishedVectorBootstrapPages.push_back(manifest);
			publishedVectorBootstrapManifests.push_back(actual.manifest_location);
		}

		std::vector<PageLocation> publishedVectorDeltaPages;
		std::vector<VectorLocation> publishedVectorDeltaLocations;
		std::vector<uint64_t> erasedVectorDeltaIds;
		std::vector<VectorManifestLocation> publishedVectorDeltaManifests;
		std::set<std::pair<uint64_t, uint16_t>> publishedVectorDeltaSlots;
		for (size_t mutationOrdinal = 0U; mutationOrdinal < vectorDeltaMaterializations.size(); ++mutationOrdinal) {
			const NativeGraphVectorDeltaMaterialization &actual = vectorDeltaMaterializations[mutationOrdinal];
			const NativeGraphVectorDeltaMaterialization &planned = plannedVectorDeltaMaterializations[mutationOrdinal];
			const NativeGraphVectorDeltaMutation &mutation = planned.mutation;
			const uint8_t invalidIdentity =
					static_cast<uint8_t>(actual.mutation.index_id != mutation.index_id) |
					static_cast<uint8_t>(actual.mutation.vector_node_mapping_index_id !=
										 mutation.vector_node_mapping_index_id) |
					static_cast<uint8_t>(actual.mutation.expected_delta_mapping_index_id !=
										 mutation.expected_delta_mapping_index_id) |
					static_cast<uint8_t>(actual.mutation.delta_mapping_index_id != mutation.delta_mapping_index_id) |
					static_cast<uint8_t>(actual.mutation.expected_delta_mapping_root !=
										 mutation.expected_delta_mapping_root) |
					static_cast<uint8_t>(actual.mutation.delta_entry_count != mutation.delta_entry_count) |
					static_cast<uint8_t>(actual.mutation.first_vector_id != mutation.first_vector_id) |
					static_cast<uint8_t>(actual.mutation.inserted_vector_count != mutation.inserted_vector_count) |
					static_cast<uint8_t>(actual.mutation.erased_vector_ids != mutation.erased_vector_ids) |
					static_cast<uint8_t>(actual.node_mapping_root != planned.node_mapping_root) |
					static_cast<uint8_t>(actual.vector_node_mapping_root != planned.vector_node_mapping_root) |
					static_cast<uint8_t>(actual.delta_mapping_root != planned.delta_mapping_root) |
					static_cast<uint8_t>(actual.raw_new_pools.size() != planned.raw_new_pools.size()) |
					static_cast<uint8_t>(actual.raw_pages.size() != planned.raw_pages.size()) |
					static_cast<uint8_t>(actual.vector_locations.size() != mutation.inserted_vector_count) |
					static_cast<uint8_t>(actual.manifest_location.index_id != mutation.index_id) |
					static_cast<uint8_t>(actual.manifest_location.manifest_page_id !=
										 actual.manifest_replacement.page_id) |
					static_cast<uint8_t>(actual.manifest_location.generation != actual.manifest_replacement.generation);
			if (invalidIdentity != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta preparer changed a metadata-owned identity");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < actual.raw_new_pools.size(); ++poolOrdinal) {
				if (!hasSameCatalogEntry(actual.raw_new_pools[poolOrdinal], planned.raw_new_pools[poolOrdinal])) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta preparer changed a fresh Raw-pool identity");
				}
			}
			if (mutation.manifest_in_new_pool &&
				!hasSameCatalogEntry(actual.manifest_new_pool, planned.manifest_new_pool)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta preparer changed a fresh Manifest-pool identity");
			}
			std::set<uint64_t> rawPageIds;
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.raw_pages.size(); ++pageOrdinal) {
				const NativeGraphVectorDeltaRawPageMaterialization &raw = actual.raw_pages[pageOrdinal];
				const NativeGraphVectorDeltaRawPageMaterialization &expected = planned.raw_pages[pageOrdinal];
				const bool cow = expected.mutation.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL;
				const uint8_t invalidRaw =
						static_cast<uint8_t>(raw.mutation.placement != expected.mutation.placement) |
						static_cast<uint8_t>(raw.mutation.page_ordinal != expected.mutation.page_ordinal) |
						static_cast<uint8_t>(raw.mutation.inserted_record_count !=
											 expected.mutation.inserted_record_count) |
						static_cast<uint8_t>(!hasSameCatalogEntry(raw.pool, expected.pool)) |
						static_cast<uint8_t>(raw.location.page_id != expected.location.page_id) |
						static_cast<uint8_t>(raw.location.segment_id != raw.pool.segment_id) |
						static_cast<uint8_t>(raw.location.generation != raw.pool.generation) |
						static_cast<uint8_t>(raw.location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(raw.location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(cow && raw.location.page_index == expected.location.page_index) |
						static_cast<uint8_t>(!rawPageIds.insert(raw.location.page_id.value()).second) |
						static_cast<uint8_t>(
								!destinationFrames.emplace(raw.location.segment_id.value(), raw.location.page_index)
										 .second);
				if (invalidRaw != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta preparer returned an invalid Raw page frame");
				}
				publishedVectorDeltaPages.push_back(raw.location);
				registerVectorPublication(raw.pool, raw.location, mutation.index_id, PageOwnerKind::POK_VECTOR_RAW);
			}
			const PageLocation &manifest = actual.manifest_replacement;
			const uint8_t invalidManifest =
					static_cast<uint8_t>(manifest.page_id != planned.manifest_replacement.page_id) |
					static_cast<uint8_t>(manifest.segment_id != planned.manifest_replacement.segment_id) |
					static_cast<uint8_t>(manifest.generation != planned.manifest_replacement.generation) |
					static_cast<uint8_t>(manifest.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(manifest.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(manifest.segment_id == mutation.expected_manifest_location.segment_id &&
										 manifest.page_index == mutation.expected_manifest_location.page_index) |
					static_cast<uint8_t>(
							!destinationFrames.emplace(manifest.segment_id.value(), manifest.page_index).second);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta preparer returned an invalid Manifest frame");
			}
			const CatalogEntry &manifestPool =
					mutation.manifest_in_new_pool ? actual.manifest_new_pool : mutation.expected_manifest_pool;
			registerVectorPublication(manifestPool, manifest, mutation.index_id, PageOwnerKind::POK_VECTOR_MANIFEST);
			for (size_t vectorOrdinal = 0U; vectorOrdinal < actual.vector_locations.size(); ++vectorOrdinal) {
				const VectorLocation &location = actual.vector_locations[vectorOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.vector_id != mutation.first_vector_id + vectorOrdinal) |
						static_cast<uint8_t>(location.slot_generation == 0U) |
						static_cast<uint8_t>(!rawPageIds.contains(location.page_id.value())) |
						static_cast<uint8_t>(
								!publishedVectorDeltaSlots.emplace(location.page_id.value(), location.slot_id).second);
				if (invalidLocation != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta preparer returned an invalid Vector Location");
				}
				publishedVectorDeltaLocations.push_back(location);
			}
			publishedVectorDeltaPages.push_back(manifest);
			publishedVectorDeltaManifests.push_back(actual.manifest_location);
			erasedVectorDeltaIds.insert(erasedVectorDeltaIds.end(), mutation.erased_vector_ids.begin(),
										mutation.erased_vector_ids.end());
		}

		// Run bounded physical authentication only after every coordinator-owned
		// logical identity and cross-type shape check has completed.
		for (const auto &[segmentId, publication]: derivedIndexPublicationFrames) {
			static_cast<void>(segmentId);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(*io_, current.data_region_offset,
																			publication.first, publication.second);
		}
		for (const auto &[segmentId, publication]: vectorPublicationFrames) {
			static_cast<void>(segmentId);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(*io_, current.data_region_offset,
																			publication.first, publication.second);
		}

		std::vector<PageLocation> publishedPages;
		size_t propertyPageCount = 0U;
		for (const NativeGraphPropertyPageBatchMaterialization &materialization: propertyPageBatchMaterializations) {
			propertyPageCount += materialization.pages.size();
		}
		publishedPages.reserve(materializations.size() + appendMaterializations.size() + newPoolMaterializations.size() +
							   propertyMaterializations.size() + propertyPageCount +
							   publishedLargeValuePages.size() + publishedDerivedIndexPages.size() +
							   publishedVectorBootstrapPages.size() + publishedVectorDeltaPages.size());
		for (const EntityPageReplacementMaterialization &materialization: materializations) {
			if (!materialization.retire_page) {
				publishedPages.push_back(materialization.replacement);
			}
		}
		for (const EntityPageAppendBatchMaterialization &materialization: appendMaterializations) {
			publishedPages.push_back(materialization.appended_location);
		}
		for (const EntityPageNewPoolBatchMaterialization &materialization: newPoolMaterializations) {
			publishedPages.push_back(materialization.initial_location);
		}
		for (const NativeGraphPropertyPageReplacementMaterialization &materialization: propertyMaterializations) {
			if (!materialization.retire_page) {
				publishedPages.push_back(materialization.replacement);
			}
		}
		for (const NativeGraphPropertyPageBatchMaterialization &materialization: propertyPageBatchMaterializations) {
			for (const NativeGraphPropertyPageInitialMaterialization &page: materialization.pages) {
				publishedPages.push_back(page.location);
			}
		}
		publishedPages.insert(publishedPages.end(), publishedLargeValuePages.begin(), publishedLargeValuePages.end());
		publishedPages.insert(publishedPages.end(), publishedDerivedIndexPages.begin(),
							  publishedDerivedIndexPages.end());
		publishedPages.insert(publishedPages.end(), publishedVectorBootstrapPages.begin(),
							  publishedVectorBootstrapPages.end());
		publishedPages.insert(publishedPages.end(), publishedVectorDeltaPages.begin(), publishedVectorDeltaPages.end());
		std::ranges::sort(publishedPages, {}, &PageLocation::page_id);
		std::ranges::sort(retiredPageIds);
		const bool duplicatePublishedPage =
				std::ranges::adjacent_find(publishedPages, {}, &PageLocation::page_id) != publishedPages.end();
		const bool duplicateRetiredPage = std::ranges::adjacent_find(retiredPageIds) != retiredPageIds.end();
		const bool publishedAndRetired =
				std::ranges::any_of(publishedPages, [&retiredPageIds](const PageLocation &page) {
					return std::ranges::binary_search(retiredPageIds, page.page_id.value());
				});
		if (duplicatePublishedPage || duplicateRetiredPage || publishedAndRetired) { // publication page sets are canonicalized before this coordinator check.
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph publication has duplicate or contradictory Page Directory mutations");
		}
		// A transaction may publish only a durable logical-ID reservation after a
		// provisional graph result is cancelled. That advances the Superblock
		// high-water marks, but it must not manufacture an empty Page Directory
		// mutation merely to satisfy the generic graph publication path.
		if (!publishedPages.empty() || !retiredPageIds.empty()) {
			if (retiredPageIds.empty()) {
				locations_.prepareUpsertPages(batch, publishedPages);
			} else {
				locations_.prepareReplacePages(batch, publishedPages, retiredPageIds);
			}
		}
		if (!insertedNodes.empty() || !erasedNodes.empty()) {
			locations_.prepareReplaceNodePageLocations(batch, insertedNodes, erasedNodes);
		}
		if (!insertedEdges.empty() || !erasedEdges.empty()) {
			locations_.prepareReplaceEdgePageLocations(batch, insertedEdges, erasedEdges);
		}
        // A record kind has one published directory root per database generation.
        // Verify that the marker's semantic generation names the actual result,
        // rather than guessing a future physical root address in the planner.
        for (const auto &fence : derivedState.entity_free_summary_fences) {
            const auto root = fence.index_id == RSS_NODE_FREE_SPACE_INDEX_ID ?
                batch.candidate().node_entity_directory_root : batch.candidate().edge_entity_directory_root;
            if ((root.isValid() && root.generation != fence.target_directory_generation) || (!root.isValid() && fence.free_pages)) {
                bool retainedStale = false;
                if (RSS_SCAN_PATH_FORMAT_ENABLED && root.isValid()) {
                    auto source = snapshots_.pinCurrentUnderWrite(writer, current);
                    BTreePageStore sourceTrees(*deferredSchemaPool, *this);
                    const auto kind = fence.index_id == RSS_NODE_FREE_SPACE_INDEX_ID ?
                        EntityPageRecordKind::EPRK_NODE : EntityPageRecordKind::EPRK_EDGE;
                    const auto proof = EntityFreeSpaceIndex::readProof(*this, sourceTrees, source, kind);
                    retainedStale = proof && !proof->fresh &&
                        proof->directory_generation == fence.target_directory_generation.value();
                }
                // A stale source marker may stay stale while changed entries
                // are maintained. It never becomes insertion authority merely
                // because the physical directory was copied elsewhere.
                if (!retainedStale)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "free-space marker does not name the resulting entity directory");
            }
        }
		std::vector<EntityPropertyKey> erasedProperties;
		erasedProperties.reserve(explicitlyErasedEntityProperties.size() + erasedEntityProperties.size());
		erasedProperties.insert(erasedProperties.end(), explicitlyErasedEntityProperties.begin(),
								explicitlyErasedEntityProperties.end());
		erasedProperties.insert(erasedProperties.end(), erasedEntityProperties.begin(), erasedEntityProperties.end());
		std::ranges::sort(upsertedEntityProperties, {},
						  [](const EntityPropertyLocation &location) { return location.key.directoryId(); });
		std::ranges::sort(erasedProperties, {}, &EntityPropertyKey::directoryId);
		// Validate duplicate writes before filtering unchanged routing identities.
		// The replacement page and source fences have already been authenticated;
		// values live behind PageDirectory, not in EntityPropertyDirectory.
		if (std::ranges::adjacent_find(upsertedEntityProperties, {},
				[](const EntityPropertyLocation &location) { return location.key.directoryId(); }) !=
				upsertedEntityProperties.end()) {
			throw FormatException(FormatError::FERR_INVALID_STATE, "duplicate Entity Property publication");
		}
		std::erase_if(upsertedEntityProperties, [&](const EntityPropertyLocation &location) {
			const auto existing = findExistingProperty(location.key);
			if (!existing || *existing != location) return false;
			debug::PerfTrace::addValue("native_graph.commit.unchanged_property_locations", 1);
			return true;
		});
		if (!upsertedEntityProperties.empty() || !erasedProperties.empty()) {
			locations_.prepareReplaceEntityProperties(batch, upsertedEntityProperties, erasedProperties);
		}
		// Several independent native derived-state plans can publish in one graph
		// generation. The Location Directory is a canonical ordered map, so make
		// that aggregate deterministic here rather than relying on planner order.
		std::ranges::sort(publishedDerivedIndexRoots, {}, &IndexRootLocation::index_id);
		std::ranges::sort(erasedDerivedIndexRootIds);
		const bool duplicatePublishedDerivedIndexRoot =
				std::ranges::adjacent_find(publishedDerivedIndexRoots, {}, &IndexRootLocation::index_id) !=
				publishedDerivedIndexRoots.end();
		const bool duplicateErasedDerivedIndexRoot =
				std::ranges::adjacent_find(erasedDerivedIndexRootIds) != erasedDerivedIndexRootIds.end();
		const bool publishedAndErasedDerivedIndexRoot = std::ranges::any_of(
				publishedDerivedIndexRoots, [&erasedDerivedIndexRootIds](const IndexRootLocation &root) {
					return std::ranges::binary_search(erasedDerivedIndexRootIds, root.index_id);
				});
		if (duplicatePublishedDerivedIndexRoot || duplicateErasedDerivedIndexRoot ||
			publishedAndErasedDerivedIndexRoot) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"native graph publication has duplicate or contradictory Index Root Directory mutations");
		}
		if (!publishedDerivedIndexRoots.empty() || !erasedDerivedIndexRootIds.empty()) {
			locations_.prepareReplaceIndexRoots(batch, publishedDerivedIndexRoots, erasedDerivedIndexRootIds);
		}
		std::vector<VectorLocation> publishedVectorLocations;
		publishedVectorLocations.reserve(publishedVectorBootstrapLocations.size() +
										 publishedVectorDeltaLocations.size());
		publishedVectorLocations.insert(publishedVectorLocations.end(), publishedVectorBootstrapLocations.begin(),
										publishedVectorBootstrapLocations.end());
		publishedVectorLocations.insert(publishedVectorLocations.end(), publishedVectorDeltaLocations.begin(),
										publishedVectorDeltaLocations.end());
		std::ranges::sort(publishedVectorLocations, {}, &VectorLocation::vector_id);
		std::ranges::sort(erasedVectorDeltaIds);
		if (std::ranges::adjacent_find(publishedVectorLocations, {}, &VectorLocation::vector_id) !=
					publishedVectorLocations.end() ||
			std::ranges::adjacent_find(erasedVectorDeltaIds) != erasedVectorDeltaIds.end() ||
			std::ranges::any_of(publishedVectorLocations, [&erasedVectorDeltaIds](const VectorLocation &location) {
				return std::ranges::binary_search(erasedVectorDeltaIds, location.vector_id);
			})) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"native graph Vector publication has duplicate or contradictory Vector Location mutations");
		}
		if (!publishedVectorLocations.empty() || !erasedVectorDeltaIds.empty()) {
			locations_.prepareReplaceVectorLocations(batch, publishedVectorLocations, erasedVectorDeltaIds);
		}
		std::vector<VectorManifestLocation> publishedVectorManifests;
		publishedVectorManifests.reserve(publishedVectorBootstrapManifests.size() +
										 publishedVectorDeltaManifests.size());
		publishedVectorManifests.insert(publishedVectorManifests.end(), publishedVectorBootstrapManifests.begin(),
										publishedVectorBootstrapManifests.end());
		publishedVectorManifests.insert(publishedVectorManifests.end(), publishedVectorDeltaManifests.begin(),
										publishedVectorDeltaManifests.end());
		std::ranges::sort(publishedVectorManifests, {}, &VectorManifestLocation::index_id);
		if (std::ranges::adjacent_find(publishedVectorManifests, {}, &VectorManifestLocation::index_id) !=
			publishedVectorManifests.end()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph Vector publication has duplicate Manifest mutations");
		}
		if (!publishedVectorManifests.empty()) {
			locations_.prepareReplaceVectorManifests(batch, publishedVectorManifests, {});
		}
		if (!publishedLargeValueManifests.empty() || !erasedLargeValueIds.empty()) {
			locations_.prepareReplaceLargeValueManifests(batch, publishedLargeValueManifests, erasedLargeValueIds);
		}
        if (preservingNativeFields && !nativeFieldsPrepared) {
            pages_.prepareHybridPropertyDescriptorRelocation(batch, current);
            batch.hybrid_property_publication_ = true;
        }
        if (scanMembership) {
            scanMembership->borrowed_placement_ = hybridPlacement.get();
            materializeScanMembershipMutation(batch, writer, *topologySource, *scanMembership, *conversionIndexes);
        }
        if (hybridPropertyRootMutation) {
            const auto &request = *hybridPropertyRootMutation;
            if (request.mode == NativeHybridPropertyRootMode::NHPRM_CLEAR) {
                batch.setHybridPropertyDescriptorRoot({});
            } else {
                HybridPropertyDescriptor descriptor{current.database_id, batch.generation(), {}};
                static_assert(RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT ==
                    std::tuple_size<decltype(request.replacement_index_ids)>::value);
                for (size_t i = 0; i < descriptor.roots.size(); ++i) {
                    const auto id = request.replacement_index_ids[i];
                    if (!id) continue;
                    if (std::ranges::binary_search(erasedDerivedIndexRootIds, id))
                        throw FormatException(FormatError::FERR_INVALID_STATE, "native hybrid descriptor names a retired tree");
                    const auto found = std::ranges::find(publishedDerivedIndexRoots, id, &IndexRootLocation::index_id);
                    const auto root = found != publishedDerivedIndexRoots.end() ? std::optional(*found) :
                        locations_.findIndexRootOptional(current, id);
                    if (!root)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "native hybrid descriptor names an absent tree");
                    descriptor.roots[i] = {root->root_page_id, root->root_generation};
                }
                // Preparation remains fail-closed for nonempty field routing.
                // This late position uses final roots and actual batch identity,
                // while preserving the existing common durability fences.
                pages_.prepareHybridPropertyDescriptor(batch, descriptor);
            }
        }
		{
			debug::ScopedPerfTimer publishTimer("native_graph.mutation.batch_publish");
            if (hybridVectors && hybridVectors->fields_.cancellation) hybridVectors->fields_.cancellation->check();
			publishBatch(batch, writer, options);
		}
        if (hybridPlacement) hybridPlacement->remember();
        if (scanMembership && scanMembership->placement_) scanMembership->placement_->remember();
        if (ordinaryIndexPlacement) ordinaryIndexPlacement->remember();
        if (streamedAdjacencyPlacement) streamedAdjacencyPlacement->remember();
        if (streamedMembershipPlacement) streamedMembershipPlacement->remember();
        for (const auto &plan : deferredPlans) if (plan.images) {
            auto root = plan.images->installed_root_;
            if (!root) {
                const auto found = std::ranges::find(publishedDerivedIndexRoots, plan.images->index_id_,
                    &IndexRootLocation::index_id);
                if (found != publishedDerivedIndexRoots.end()) root = *found;
            }
            if (root) {
                const auto dataRoot = batch.candidate().usesPageDirectoryBootstrap() ?
                    batch.pendingBootstrapDataRoot().value_or(plan.images->source_data_root_) : batch.candidate().page_directory_root;
                std::lock_guard lock(streamed_adjacency_shape_mutex_);
                auto &hint = plan.images->index_id_ == RSS_GLOBAL_ADJACENCY_INDEX_ID ? streamed_adjacency_shape_ : streamed_membership_shape_;
                hint = {batch.generation(), plan.images->entry_count_, plan.images->page_count_, dataRoot, root};
            }
        }
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::publishStreamedEdgeGraph(
			const NativeStreamedGraphFinalPublicationRequest &request, SystemCatalog &catalog,
			BTreePageStore &adjacencyPages, LargeValueStore *largeValueStore, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &options) {
		const NativeStreamedGraphPublicationPlan &plan = request.publication_plan;
		const NativeGraphPrivateSegmentPlan &privatePlan = request.private_candidate_plan;
		const bool hasLargeValues = plan.large_value_count != 0U;
		const bool invalidLargeValuePlacement =
				hasLargeValues &&
				(!request.first_large_value_data_output_slot.isValid() ||
				 !request.last_large_value_data_output_slot.isValid() ||
				 request.last_large_value_data_output_slot < request.first_large_value_data_output_slot ||
				 !request.first_large_value_manifest_output_slot.isValid() ||
				 !request.last_large_value_manifest_output_slot.isValid() ||
				 request.last_large_value_manifest_output_slot < request.first_large_value_manifest_output_slot);
		if (!request.expected_metadata_generation.isValid() || !request.durable_source.isValidFor(*this) || plan.record_count == 0U ||
			plan.first_edge_id == 0U || plan.last_edge_id < plan.first_edge_id ||
			plan.edge_entity_page_count == 0U || invalidLargeValuePlacement ||
			request.entity_pool_owner_id == 0U || request.property_pool_owner_id == 0U ||
			!request.first_entity_output_slot.isValid() || !request.last_entity_output_slot.isValid() ||
			request.last_entity_output_slot < request.first_entity_output_slot || !request.first_property_output_slot.isValid() ||
			!request.last_property_output_slot.isValid() || request.last_property_output_slot < request.first_property_output_slot ||
			privatePlan.kind != SegmentKind::SGK_BULK_SCRATCH || privatePlan.owner_id == 0U ||
			!privatePlan.first_slot.isValid() || !privatePlan.last_slot.isValid() || privatePlan.last_slot < privatePlan.first_slot ||
		privatePlan.segment_count == 0U || privatePlan.segment_count != request.candidate_layout.candidate_count ||
		privatePlan.header_bytes < RSS_ENVELOPE_BYTES ||
			!checkpointLsn.isValid() || !catalog.usesMetadataStore(*this) || !adjacencyPages.usesMetadataStore(*this) ||
			(largeValueStore != nullptr && !largeValueStore->usesMetadataStore(*this)) ||
			(hasLargeValues &&
			 (largeValueStore == nullptr || request.preparation.large_value_store != largeValueStore))) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "streamed Edge final publication requires one complete bounded metadata domain");
		}
		validateStreamedEdgeLeaseRanges(request.edge_id_reservation, plan.first_edge_id, plan.last_edge_id);

		NativeGraphDerivedStateMutation derivedState;
		derivedState.private_segment_plan = privatePlan;
		derivedState.external_private_tail_reservation = request.private_source_tail;
		const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
		const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
		const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
		const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
		const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
		const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
		const NativeGraphPageTransactionWithLargeValuesPreparer noPageMaterializations =
				[](std::span<EntityPageReplacementMaterialization> entities,
				   std::span<EntityPageAppendBatchMaterialization> appends,
				   std::span<EntityPageNewPoolBatchMaterialization> pools,
				   std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
				   std::span<NativeGraphPropertyPageBatchMaterialization> propertyPools,
				   std::span<LargeValueMaterializationResult> values) {
					const uint8_t unexpectedWork = static_cast<uint8_t>(!entities.empty()) |
														 static_cast<uint8_t>(!appends.empty()) |
														 static_cast<uint8_t>(!pools.empty()) |
														 static_cast<uint8_t>(!properties.empty()) |
														 static_cast<uint8_t>(!propertyPools.empty()) |
														 static_cast<uint8_t>(!values.empty());
					if (unexpectedWork != 0U) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
										  "streamed Edge coordinator unexpectedly received vector-backed page work");
					}
				};
		const NativeGraphDerivedStatePreparer noOrdinaryIndexMaterializations =
				[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
					if (!batches.empty()) { // streamed final publication has no ordinary Index materialization batches.
						throw FormatException(FormatError::FERR_INVALID_STATE,
										  "streamed Edge coordinator unexpectedly received ordinary Index work");
					}
				};

		const NativeGraphEarlyPublicationPlanner earlyPlanner =
				[this, &request, &catalog, &adjacencyPages, largeValueStore, checkpointLsn](
						const NativeGraphEarlyPublicationPlanningContext &context) {
					const NativeBulkEdgeRecordSource source = request.durable_source.open(context.current_snapshot);
					NativeStreamedGraphPrivatePreparationCoordinator::prepareDeferredPublication(
							source, request.edge_id_reservation.edge_ranges, request.publication_plan,
							request.candidate_layout, context.private_segments, catalog, context.current_snapshot,
							request.preparation, context.private_segment_reader, context.private_segment_writer,
							context.private_segment_fence,
							[this, &request, &catalog, &adjacencyPages, largeValueStore, checkpointLsn, &context](
									const NativeStreamedGraphPrivatePreparation &prepared,
									const NativeStreamedGraphDeferredPublication &publication) {
								if (!prepared.publication_shape.has_value() || // preparation and publication shape are generated atomically from one authenticated plan.
									(prepared.large_values.has_value() != (request.publication_plan.large_value_count != 0U))) {
									throw FormatException(FormatError::FERR_INVALID_STATE,
											  "streamed Edge final coordinator observed an incomplete LargeValue preparation");
								}
								const NativeStreamedGraphPublicationShape &shape = *prepared.publication_shape;
								// Private preparation measures this scalar shape against the sealed,
								// writer-fenced source. Reusing it avoids a duplicate full replay
								// before final page identities are allocated.
								const NativeStreamedGraphInlinePropertyPageCount properties{
										shape.property_page_count, shape.property_root_count};
								struct StreamedLargeValuePoolCapacity {
									CatalogEntry pool;
									size_t available_frames = 0U;
									size_t prepared_frames = 0U;
									bool is_new_pool = false;
								};
								std::vector<StreamedLargeValuePoolCapacity> largeValueDataPools;
								std::vector<StreamedLargeValuePoolCapacity> largeValueManifestPools;
								if (prepared.large_values.has_value()) {
									const auto loadSharedPools = [&](const std::vector<CatalogEntry> &source,
																						 std::vector<StreamedLargeValuePoolCapacity> &destination,
																						 const char *message) {
										destination.reserve(source.size());
										for (const CatalogEntry &pool: source) {
							if (!allocator_.isAllocated(context.current_superblock, pool.physical_slot)) { // coordinator reservations are validated against the allocator before page preparation.
												throw FormatException(FormatError::FERR_INVALID_STATE, message);
											}
											destination.push_back({
													pool,
													SharedPagePoolAllocator::availableFrameCountUnderWrite(
															*io_, context.current_superblock.data_region_offset, pool)});
										}
									};
									loadSharedPools(findCurrentLargeValueDataPagePools(context.current_snapshot), largeValueDataPools,
													"streamed Edge final coordinator found an unavailable LargeValue data pool");
									loadSharedPools(findCurrentLargeValueManifestPagePools(context.current_snapshot), largeValueManifestPools,
													"streamed Edge final coordinator found an unavailable LargeValue Manifest pool");
								}
					if (shape.entity_page_count != request.publication_plan.edge_entity_page_count || // immutable candidate layout and publication plan are derived together.
										shape.property_page_count != properties.page_count ||
										shape.property_root_count != properties.property_count ||
									(prepared.large_values.has_value() &&
									 prepared.large_values->value_plan.reference_count != request.publication_plan.large_value_count) ||
									shape.large_value_count !=
											(prepared.large_values.has_value()
													 ? prepared.large_values->value_plan.value_count
													 : size_t{0U}) ||
										shape.total_public_page_count < shape.entity_page_count ||
										shape.property_page_count > shape.total_public_page_count - shape.entity_page_count ||
										shape.large_value_data_page_count >
												shape.total_public_page_count - shape.entity_page_count - shape.property_page_count ||
										shape.large_value_manifest_page_count !=
												shape.total_public_page_count - shape.entity_page_count - shape.property_page_count -
														shape.large_value_data_page_count) {
									throw FormatException(FormatError::FERR_INVALID_STATE,
												  "streamed Edge final coordinator observed a divergent Entity or Property shape");
								}
								const size_t entityPoolCount =
										(shape.entity_page_count - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
								const size_t propertyPoolCount = properties.page_count == 0U
																					 ? 0U
																					 : (properties.page_count - 1U) / RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U;
								const auto requiredFreshPoolCount = [](size_t pageCount,
																						 const std::vector<StreamedLargeValuePoolCapacity> &pools) {
									size_t remaining = pageCount;
									for (const StreamedLargeValuePoolCapacity &pool: pools) {
										const size_t consumed = std::min(remaining, pool.available_frames);
										remaining -= consumed;
										if (remaining == 0U) {
											return size_t{0U};
										}
									}
									return remaining / RSS_PAGE_POOL_DATA_FRAME_COUNT +
											static_cast<size_t>(remaining % RSS_PAGE_POOL_DATA_FRAME_COUNT != 0U);
								};
								const size_t largeValueDataPoolCount = requiredFreshPoolCount(
										shape.large_value_data_page_count, largeValueDataPools);
								const size_t largeValueManifestPoolCount = requiredFreshPoolCount(
										shape.large_value_manifest_page_count, largeValueManifestPools);
					if (entityPoolCount > std::numeric_limits<size_t>::max() - propertyPoolCount || // bounded candidate layout counts cannot overflow host size arithmetic.
									propertyPoolCount > std::numeric_limits<size_t>::max() - largeValueDataPoolCount ||
									entityPoolCount + propertyPoolCount >
											std::numeric_limits<size_t>::max() - largeValueDataPoolCount ||
									entityPoolCount + propertyPoolCount + largeValueDataPoolCount >
											std::numeric_limits<size_t>::max() - largeValueManifestPoolCount ||
									shape.total_public_page_count > std::numeric_limits<uint64_t>::max()) {
									throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
												  "streamed Edge final coordinator public placement overflows");
								}
									const NativeStreamedGraphPageRanges pages = NativeStreamedGraphPageRanges::derive(
											context.metadata_batch.allocateLogicalPageIdRange(
													static_cast<uint64_t>(shape.total_public_page_count)),
											shape);
								auto entityPageSession = locations_.beginAppendPageLocations(
											context.metadata_batch, pages.first_entity_page.value(),
											PageId(pages.first_entity_page.value() + shape.entity_page_count - 1U).value(),
											shape.entity_page_count);
								std::optional<LocationDirectoryAppendSession> propertyPageSession;
								std::optional<LocationDirectoryAppendSession> largeValuePageSession;
									const LargeValueId firstValueId = prepared.large_values.has_value()
																				 ? context.metadata_batch.allocateLargeValueIdRange(
															static_cast<uint64_t>(prepared.large_values->value_plan.reference_count))
																				 : LargeValueId{};
					if (prepared.large_values.has_value() && // LargeValue preparation and final replay share the same first durable identity.
										firstValueId != prepared.large_values->first_value_id) {
										throw FormatException(FormatError::FERR_INVALID_STATE,
														  "streamed Edge final coordinator LargeValue ID reservation diverged from private planning");
									}
								const size_t totalPoolCount =
										entityPoolCount + propertyPoolCount + largeValueDataPoolCount + largeValueManifestPoolCount;
								const SegmentId firstSegment = context.metadata_batch.allocateLogicalSegmentIdRange(
										static_cast<uint64_t>(totalPoolCount));
								auto catalogSession = catalog_.beginAppend(
										context.metadata_batch, firstSegment,
										SegmentId(firstSegment.value() + totalPoolCount - 1U), totalPoolCount);
									auto edgeSession = locations_.beginAppendEdgePageLocations(
										context.metadata_batch, request.publication_plan.first_edge_id,
										request.publication_plan.last_edge_id, request.publication_plan.record_count);
								std::optional<LocationDirectoryAppendSession> propertySession;
									if (properties.page_count != 0U) {
										propertySession.emplace(locations_.beginAppendEdgePropertyLocations(
											context.metadata_batch, request.publication_plan.first_edge_id,
												request.publication_plan.last_edge_id, properties.property_count));
									}
									std::optional<LocationDirectoryAppendSession> largeValueManifestSession;
									if (prepared.large_values.has_value()) {
										const uint64_t referenceCount =
												static_cast<uint64_t>(prepared.large_values->value_plan.reference_count);
										largeValueManifestSession.emplace(locations_.beginAppendLargeValueManifestLocations(
												context.metadata_batch, firstValueId,
												LargeValueId(firstValueId.value() + referenceCount - 1U),
												prepared.large_values->value_plan.value_count));
									}
									const NativeBulkRunPayloadReader privatePayloadReader =
											[privateSegments = context.private_segments,
											 privateSegmentReader = context.private_segment_reader](
													const NativeBulkRunFragmentLocation &location) {
												const NativeGraphPrivateSegmentMaterialization identity{
														location.segment_id, location.generation, location.transaction_id};
												try {
													static_cast<void>(privateSegments.ordinalOf(identity));
												} catch (const FormatException &) {
													throw FormatException(FormatError::FERR_INVALID_STATE,
																  "streamed Edge final coordinator received a foreign LargeValue map run");
												}
												return privateSegmentReader(identity);
											};
									const auto canonicalReferences = [&]() {
										return NativeBulkRunStore::orderedSourceFromPayloadReader(
												prepared.large_values->reference_map_run,
												request.preparation.large_value_content_options.maximum_manifest_depth,
												privatePayloadReader);
									};
								PhysicalSlotId entityOutputCursor = request.first_entity_output_slot;
								PhysicalSlotId propertyOutputCursor = request.first_property_output_slot;
								PhysicalSlotId largeValueDataOutputCursor = request.first_large_value_data_output_slot;
								PhysicalSlotId largeValueManifestOutputCursor = request.first_large_value_manifest_output_slot;
								const auto reserveOutputPool = [&](PhysicalSlotId &cursor, PhysicalSlotId last, const char *message) {
									for (;;) {
										const std::optional<PhysicalSlotId> slot = allocator_.findFirstFree(
												context.metadata_batch.candidate(), cursor, last);
										if (!slot.has_value()) {
											throw FormatException(FormatError::FERR_INVALID_STATE, message);
										}
										if (!context.metadata_batch.isDataSlotReserved(*slot) &&
											!context.metadata_batch.isMetadataPoolSlot(*slot)) {
											context.metadata_batch.reserveDataSlot(*slot);
											allocator_.prepareSetAllocated(context.metadata_batch, *slot, true);
											// The batch records this reservation before the next lookup, so
											// advancing is safe and avoids repeatedly rescanning prior pools.
											if (*slot != last) {
												cursor = PhysicalSlotId(slot->value() + 1U);
											}
											return *slot;
										}
										if (*slot == last || slot->value() == std::numeric_limits<uint64_t>::max()) { // bounded slot ranges are preflighted before replay iteration.
											throw FormatException(FormatError::FERR_INVALID_STATE, message);
										}
										cursor = PhysicalSlotId(slot->value() + 1U);
									}
								};
								const auto materialize = [&](bool entity) {
									CatalogEntry activePool;
									size_t activeOrdinal = std::numeric_limits<size_t>::max();
									if (entity) {
										// Keep at most one physical pool in memory. Preparing all of its
										// frames together avoids evicting and rewriting the pool control
										// page once per Entity page while preserving the same validation
										// boundary before directory reachability changes.
										std::vector<NativeStreamedGraphEntityPage> pendingPages;
										pendingPages.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
										const auto flush = [&] {
											if (pendingPages.empty()) {
												return;
											}
											debug::ScopedPerfTimer flushTimer("native_graph.streamed.final.entity_pool_write");
											std::vector<SharedPagePoolPreparedAppend> preparedPages;
											std::vector<PageLocation> locations;
											std::vector<SharedPagePoolPreparedPublication> publications;
											preparedPages.reserve(pendingPages.size());
											locations.reserve(pendingPages.size());
											publications.reserve(pendingPages.size());
											for (const NativeStreamedGraphEntityPage &page: pendingPages) {
												locations.push_back({page.owner.page_id, activePool.segment_id, 0U,
																	 page.owner.generation, 0U});
												preparedPages.push_back({page.owner, page.payload, 0U});
											}
											SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(
													*io_, context.current_superblock.data_region_offset, activePool, preparedPages,
													locations, checkpointLsn, 0U);
											for (const PageLocation &location: locations) {
												publications.push_back(
														{location, activePool.owner_id, PageOwnerKind::POK_ENTITY, checkpointLsn});
											}
											SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(
													*io_, context.current_superblock.data_region_offset, activePool, publications);
											debug::ScopedPerfTimer directoryTimer(
													"native_graph.streamed.final.entity_directory_append");
											for (size_t index = 0U; index < pendingPages.size(); ++index) {
												{
													debug::ScopedPerfTimer pageDirectoryTimer(
															"native_graph.streamed.final.page_directory_append");
													entityPageSession.append(locations[index]);
												}
												{
													debug::ScopedPerfTimer edgeDirectoryTimer(
															"native_graph.streamed.final.edge_directory_append");
													{
														debug::ScopedPerfTimer leaseTimer(
																	"native_graph.streamed.final.edge_lease_validation");
													for (const EntityPageLocation &edge: pendingPages[index].edge_locations) {
														if (!streamedEdgeIdIsLeased(request.edge_id_reservation, edge.entity_id)) { // NativeBulkEdgeMaterializer validates every Edge ID against the same authenticated lease before this final page callback.
																throw FormatException(FormatError::FERR_INVALID_STATE,
																			  "streamed Edge final coordinator materialized an unleased Edge ID");
															}
														}
													}
													{
														debug::ScopedPerfTimer appendTimer(
																	"native_graph.streamed.final.edge_location_append");
														for (const EntityPageLocation &edge: pendingPages[index].edge_locations) {
															edgeSession.append(edge);
														}
													}
												}
											}
											pendingPages.clear();
										};
										NativeStreamedGraphEntityPageMaterializer::materialize(
											publication,
											[&](size_t ordinal) {
												const size_t poolOrdinal = ordinal / RSS_PAGE_POOL_DATA_FRAME_COUNT;
												if (poolOrdinal != activeOrdinal) {
													flush();
													activePool = {SegmentId(firstSegment.value() + poolOrdinal),
														reserveOutputPool(entityOutputCursor, request.last_entity_output_slot,
																"streamed Edge final coordinator has no free Entity pool slot"),
														SegmentKind::SGK_ENTITY_PAGE_POOL, request.entity_pool_owner_id, Generation(1U),
														SegmentState::SGS_SEALED, checkpointLsn};
													catalogSession.append(activePool);
													activeOrdinal = poolOrdinal;
												}
													return NativeStreamedGraphEntityPageAssignment{{pages.entityPageAt(ordinal), activePool.owner_id,
													PageOwnerKind::POK_ENTITY, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
													activePool.generation, checkpointLsn}};
											},
											[&](NativeStreamedGraphEntityPage &&page) {
												if (pendingPages.size() == RSS_PAGE_POOL_DATA_FRAME_COUNT) { // streamed publication shape preflight bounds each materializer to the fixed pool frame budget.
													throw FormatException(FormatError::FERR_INVALID_STATE,
																  "streamed Edge Entity pool materializer exceeded its fixed page budget");
												}
												pendingPages.push_back(std::move(page));
											});
										flush();
									} else {
										std::vector<NativeStreamedGraphPropertyPage> pendingPages;
										pendingPages.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
										const auto flush = [&] {
											if (pendingPages.empty()) {
												return;
											}
											std::vector<SharedPagePoolPreparedAppend> preparedPages;
											std::vector<PageLocation> locations;
											std::vector<SharedPagePoolPreparedPublication> publications;
											preparedPages.reserve(pendingPages.size());
											locations.reserve(pendingPages.size());
											publications.reserve(pendingPages.size());
											for (const NativeStreamedGraphPropertyPage &page: pendingPages) {
												locations.push_back({page.owner.page_id, activePool.segment_id, 0U,
																	 page.owner.generation, page.free_bytes});
												preparedPages.push_back({page.owner, page.payload, page.free_bytes});
											}
											SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(
													*io_, context.current_superblock.data_region_offset, activePool, preparedPages,
													locations, checkpointLsn, 0U);
											for (const PageLocation &location: locations) {
												publications.push_back(
														{location, activePool.owner_id, PageOwnerKind::POK_PROPERTY, checkpointLsn});
											}
											SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(
													*io_, context.current_superblock.data_region_offset, activePool, publications);
											for (size_t index = 0U; index < pendingPages.size(); ++index) {
												propertyPageSession->append(locations[index]);
													for (const EntityPropertyLocation &property: pendingPages[index].property_locations) {
												if (!streamedEdgeIdIsLeased(request.edge_id_reservation, property.key.entity_id)) { // NativeBulkEdgeMaterializer validates the Property owner from the same authenticated Edge record before this callback.
														throw FormatException(FormatError::FERR_INVALID_STATE,
																  "streamed Edge final coordinator materialized an unleased Property owner");
													}
													propertySession->append(property);
												}
											}
											pendingPages.clear();
										};
											const auto assignPropertyPage = [&](size_t ordinal) {
													const size_t poolOrdinal = ordinal / RSS_PAGE_POOL_DATA_FRAME_COUNT;
											if (poolOrdinal != activeOrdinal) {
													flush();
													activePool = {SegmentId(firstSegment.value() + entityPoolCount + poolOrdinal),
														reserveOutputPool(propertyOutputCursor, request.last_property_output_slot,
																"streamed Edge final coordinator has no free Property pool slot"),
														SegmentKind::SGK_PROPERTY_PAGE_POOL, request.property_pool_owner_id, Generation(1U),
														SegmentState::SGS_SEALED, checkpointLsn};
													catalogSession.append(activePool);
													activeOrdinal = poolOrdinal;
												}
													return NativeStreamedGraphPropertyPageAssignment{{pages.propertyPageAt(ordinal), activePool.owner_id,
														PageOwnerKind::POK_PROPERTY, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
														activePool.generation, checkpointLsn}};
											};
											const auto consumePropertyPage = [&](NativeStreamedGraphPropertyPage &&page) {
													if (pendingPages.size() == RSS_PAGE_POOL_DATA_FRAME_COUNT) { // streamed publication shape preflight bounds each materializer to the fixed pool frame budget.
														throw FormatException(FormatError::FERR_INVALID_STATE,
																  "streamed Edge Property pool materializer exceeded its fixed page budget");
													}
													pendingPages.push_back(std::move(page));
											};
											if (prepared.large_values.has_value()) {
												NativeStreamedGraphLargeValuePlanner::materializePropertyPages(
														publication, *largeValueStore, firstValueId, prepared.large_values->value_plan,
														request.preparation.large_value_options, assignPropertyPage, consumePropertyPage,
														canonicalReferences());
											} else {
												NativeStreamedGraphPropertyPageMaterializer::materializeInlinePages(
														publication, assignPropertyPage, consumePropertyPage);
											}
											flush();
										}
								};
								{
									debug::ScopedPerfTimer timer("native_graph.streamed.final.entity_pages");
									materialize(true);
								}
								entityPageSession.finish();
									if (properties.page_count != 0U) {
										propertyPageSession.emplace(locations_.beginAppendPageLocations(
												context.metadata_batch, pages.first_property_page.value(),
												PageId(pages.first_property_page.value() + shape.property_page_count - 1U).value(),
												shape.property_page_count));
										debug::ScopedPerfTimer timer("native_graph.streamed.final.property_pages");
										materialize(false);
										propertyPageSession->finish();
									}
									if (prepared.large_values.has_value()) {
									const size_t largeValuePageCount =
											shape.large_value_data_page_count + shape.large_value_manifest_page_count;
									largeValuePageSession.emplace(locations_.beginAppendPageLocations(
											context.metadata_batch, pages.first_large_value_page.value(),
											PageId(pages.first_large_value_page.value() + largeValuePageCount - 1U).value(),
											largeValuePageCount));
									const auto addFreshPools = [&](std::vector<StreamedLargeValuePoolCapacity> &pools,
											size_t count, size_t segmentOffset, PhysicalSlotId &cursor,
											PhysicalSlotId lastSlot, SegmentKind kind, uint64_t ownerId,
																	const char *message) {
										pools.reserve(pools.size() + count);
										for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
											CatalogEntry pool{SegmentId(firstSegment.value() + segmentOffset + ordinal),
													  reserveOutputPool(cursor, lastSlot, message), kind, ownerId,
																  Generation(1U), SegmentState::SGS_SEALED, checkpointLsn};
												catalogSession.append(pool);
												pools.push_back({pool, RSS_PAGE_POOL_DATA_FRAME_COUNT, 0U, true});
											}
										};
									addFreshPools(largeValueDataPools,
											largeValueDataPoolCount, entityPoolCount + propertyPoolCount,
											largeValueDataOutputCursor, request.last_large_value_data_output_slot,
												SegmentKind::SGK_LARGE_VALUE_DATA_POOL, RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID,
												"streamed Edge final coordinator has no free LargeValue data pool slot");
									addFreshPools(largeValueManifestPools,
											largeValueManifestPoolCount, entityPoolCount + propertyPoolCount + largeValueDataPoolCount,
											largeValueManifestOutputCursor, request.last_large_value_manifest_output_slot,
												SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL, RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID,
												"streamed Edge final coordinator has no free LargeValue Manifest pool slot");

										size_t dataPoolCursor = 0U;
										size_t manifestPoolCursor = 0U;
										StreamedLargeValuePoolCapacity *pendingDataPool = nullptr;
										StreamedLargeValuePoolCapacity *pendingManifestPool = nullptr;
										const auto nextLargeValuePool = [](std::vector<StreamedLargeValuePoolCapacity> &pools,
																			size_t &cursor, const char *message)
												-> StreamedLargeValuePoolCapacity & {
											while (cursor < pools.size() && pools[cursor].available_frames == 0U) {
												++cursor;
											}
											if (cursor == pools.size()) { // authenticated page counts and pool capacities are matched before replay enters this helper.
												throw FormatException(FormatError::FERR_INVALID_STATE, message);
											}
											return pools[cursor];
										};

										size_t largeValuePageOrdinal = 0U;
										NativeStreamedGraphLargeValuePlanner::materializeLargeValues(
												publication, *largeValueStore, firstValueId, prepared.large_values->value_plan,
												request.preparation.large_value_options, [&](NativeStreamedGraphLargeValueItem &&item) {
											const size_t itemPageCount = item.planned_data_page_count + item.planned_manifest_page_count;
											if (itemPageCount > shape.large_value_data_page_count + shape.large_value_manifest_page_count - // the immutable publication shape is produced from the same item plan before replay.
														largeValuePageOrdinal) {
														throw FormatException(FormatError::FERR_INVALID_STATE,
																  "streamed Edge final coordinator LargeValue item exceeded its reserved pages");
													}
											const PageId rootManifestPage =
													pages.largeValuePageAt(largeValuePageOrdinal + item.planned_data_page_count);
													std::optional<PageLocation> rootManifestLocation;
													largeValueStore->materializeStreamedValue(
														{item.stored_reference.value, item.logical_bytes, request.preparation.large_value_options.compression,
														 request.preparation.large_value_options.extent_class,
														 pages.largeValuePageAt(largeValuePageOrdinal), item.planned_data_page_count, rootManifestPage,
														 item.planned_manifest_page_count},
														checkpointLsn,
														[&](PageId pageId, PageOwnerKind kind) {
															const bool data = kind == PageOwnerKind::POK_LARGE_VALUE_DATA;
													if ((kind != PageOwnerKind::POK_LARGE_VALUE_DATA && // authenticated LargeValue replay emits only data or Manifest owner kinds in the reserved page range.
																 kind != PageOwnerKind::POK_LARGE_VALUE_MANIFEST) ||
																pageId < pages.first_large_value_page ||
																pageId.value() - pages.first_large_value_page.value() >=
																		shape.large_value_data_page_count + shape.large_value_manifest_page_count) {
																throw FormatException(
																		FormatError::FERR_INVALID_STATE,
																		"streamed Edge final coordinator received a LargeValue frame outside "
																		"its reserved range");
															}
															StreamedLargeValuePoolCapacity &target =
																	data ? nextLargeValuePool(
																				   largeValueDataPools, dataPoolCursor,
																				   "streamed Edge final coordinator exhausted its LargeValue data "
																				   "pool placement")
																		 : nextLargeValuePool(
																				   largeValueManifestPools, manifestPoolCursor,
																				   "streamed Edge final coordinator exhausted its LargeValue Manifest "
																				   "pool placement");
															StreamedLargeValuePoolCapacity *&pending =
																	data ? pendingDataPool : pendingManifestPool;
													if (pending != nullptr) { // each replay item assigns one pending pool and consumes it exactly once.
																throw FormatException(
																		FormatError::FERR_INVALID_STATE,
																		"streamed Edge LargeValue owner replay did not consume its prior "
																		"pool assignment");
															}
															pending = &target;
															return PageOwnerHeader{pageId, item.stored_reference.value.value_id.value(), kind, 0U,
																RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, target.pool.generation, checkpointLsn};
														},
														[&](const PageOwnerHeader &owner, std::vector<std::byte> payload, PageOwnerKind kind) {
															const bool data = kind == PageOwnerKind::POK_LARGE_VALUE_DATA;
															StreamedLargeValuePoolCapacity *&pending =
																	data ? pendingDataPool : pendingManifestPool;
													if ((kind != PageOwnerKind::POK_LARGE_VALUE_DATA && // authenticated LargeValue replay callback is paired with the assignment callback above.
																 kind != PageOwnerKind::POK_LARGE_VALUE_MANIFEST) ||
												 pending == nullptr || owner.kind != kind || // replay assignment and owner callbacks are paired by one authenticated plan.
																owner.generation != pending->pool.generation ||
																pending->available_frames == 0U) {
																throw FormatException(
																		FormatError::FERR_INVALID_STATE,
																		"streamed Edge LargeValue frame replay diverged from its authenticated "
																		"pool assignment");
															}
															StreamedLargeValuePoolCapacity &target = *pending;
															pending = nullptr;
															PageLocation location{
																	owner.page_id, target.pool.segment_id, 0U, target.pool.generation, 0U};
															const SharedPagePoolPreparedAppend page{owner, std::move(payload), 0U};
															if (target.is_new_pool) {
																SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(
																		*io_, context.current_superblock.data_region_offset, target.pool,
																		std::span(&page, 1U), std::span(&location, 1U), checkpointLsn,
																		target.prepared_frames);
																++target.prepared_frames;
															} else {
																SharedPagePoolAllocator::prepareAppendPagesUnderWrite(
																		*io_, context.current_superblock.data_region_offset, target.pool,
																		std::span(&page, 1U), std::span(&location, 1U), checkpointLsn);
															}
															--target.available_frames;
															const SharedPagePoolPreparedPublication publication{
																	location, owner.owner_id, kind, checkpointLsn};
															SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(
																	*io_, context.current_superblock.data_region_offset, target.pool,
																	std::span(&publication, 1U));
																			largeValuePageSession->append(location);
															if (owner.page_id == rootManifestPage) {
																rootManifestLocation = location;
															}
														});
													if (!rootManifestLocation.has_value()) { // every planned LargeValue item emits exactly one authenticated Manifest root.
														throw FormatException(FormatError::FERR_INVALID_STATE,
																  "streamed Edge final coordinator did not materialize its LargeValue Manifest root");
													}
													largeValueManifestSession->append(
															{item.stored_reference.value.value_id, rootManifestLocation->page_id,
															 item.stored_reference.value.generation});
											largeValuePageOrdinal += itemPageCount;
										}, canonicalReferences());
										if (largeValuePageOrdinal != // replay consumes the exact page count used to construct the publication shape.
											shape.large_value_data_page_count + shape.large_value_manifest_page_count) {
											throw FormatException(FormatError::FERR_INVALID_STATE,
														  "streamed Edge final coordinator LargeValue replay diverged from its publication shape");
										}
										if (pendingDataPool != nullptr || pendingManifestPool != nullptr) { // the authenticated replay callback clears each pending pool assignment exactly once.
											throw FormatException(FormatError::FERR_INVALID_STATE,
																  "streamed Edge LargeValue replay ended with an unconsumed pool assignment");
										}
									}
									{
										debug::ScopedPerfTimer timer("native_graph.streamed.final.directory_publish");
										if (largeValuePageSession.has_value()) {
											largeValuePageSession->finish();
										}
										catalogSession.finish();
										edgeSession.finish();
										if (propertySession.has_value()) {
											propertySession->finish();
										}
										if (largeValueManifestSession.has_value()) {
											largeValueManifestSession->finish();
										}
									}
									debug::ScopedPerfTimer indexTimer("native_graph.streamed.final.index_plan");
									NativeGraphDeferredIndexPlans indexPlans = NativeStreamedGraphPublicationIndexPlanner::plan(
											catalog, adjacencyPages, largeValueStore, *this, prepared, request.index_planning,
											{context.private_segments, context.private_segment_reader, context.current_snapshot}, checkpointLsn, &publication);
								for (NativeGraphDeferredIndexPlan &indexPlan: indexPlans.plans) {
									context.deferred_index_plan_sink(std::move(indexPlan));
								}
							});
				};
		return mutateNativeGraphPagesWithLargeValues(
				noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools, noLargeValues,
				derivedState, checkpointLsn, noPageMaterializations, noOrdinaryIndexMaterializations, {}, {}, {}, {}, {},
				options, request.edge_id_reservation, request.expected_metadata_generation,
				[](const NativeGraphPrivateSegmentSet &, size_t) { return NativeGraphPrivateSegmentReservation::payload(); },
				{}, {}, earlyPlanner);
	}

} // namespace graph::storage
