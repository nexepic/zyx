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
        std::vector<NativeGraphEntityFreeSummaryFence> effectiveSummaryFences = derivedState.entity_free_summary_fences;
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
#include "graph_mutation/FieldPlanningAndPrivateReservations.inc"
#include "graph_mutation/DerivedIndexMaterialization.inc"
#include "graph_mutation/PublicationAndRetirement.inc"

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

        // EntityPageStore attaches through openSnapshot and must be constructed
        // before the metadata writer is held. Only a stale existing summary
        // needs an authoritative-page rebuild.
        std::unique_ptr<EntityPageStore> staleSummaryPages;
        {
            auto before = openSnapshot();
            const auto proof = EntityFreeSpaceIndex::readProof(
                *this, adjacencyPages, before, EntityPageRecordKind::EPRK_EDGE);
            if (proof && !proof->fresh) {
                before.release();
                staleSummaryPages = std::make_unique<EntityPageStore>(io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            }
        }
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
				[this, &request, &catalog, &adjacencyPages, &staleSummaryPages, largeValueStore, checkpointLsn](
						const NativeGraphEarlyPublicationPlanningContext &context) {
					const NativeBulkEdgeRecordSource source = request.durable_source.open(context.current_snapshot);
					NativeStreamedGraphPrivatePreparationCoordinator::prepareDeferredPublication(
							source, request.edge_id_reservation.edge_ranges, request.publication_plan,
							request.candidate_layout, context.private_segments, catalog, context.current_snapshot,
							request.preparation, context.private_segment_reader, context.private_segment_writer,
							context.private_segment_fence,
							[this, &request, &catalog, &adjacencyPages, &staleSummaryPages, largeValueStore, checkpointLsn, &context](
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
                                // Streamed pages are full except the final page. Keep an
                                // existing edge free-space tree consistent with this same
                                // writer-fenced publication; a missing legacy tree remains absent.
                                const auto summary = EntityFreeSpaceIndex::readProof(
                                        *this, adjacencyPages, context.current_snapshot,
                                        EntityPageRecordKind::EPRK_EDGE);
                                if (summary) {
                                    const size_t capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_EDGE);
                                    const size_t used = request.publication_plan.record_count % capacity;
                                    uint64_t freePages = summary->free_pages;
                                    std::vector<BTreeScalarIndexOperation> operations;
                                    utils::OwnedHeapReservation operationCredit(utils::OwnedAllocationScope::owner(), 0U);
                                    const auto append = [&operations, &operationCredit](BTreeScalarIndexOperationKind kind,
                                                                                       const auto &key, uint64_t value) {
                                        if (operationCredit.bytes() > std::numeric_limits<size_t>::max() - 256U)
                                            throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
                                                                  "streamed Edge summary workspace overflowed");
                                        operationCredit.set(operationCredit.bytes() + 256U);
                                        operations.push_back({kind, {key.begin(), key.end()}, value});
                                    };
                                    const auto add = [&append](const auto &key, uint64_t value) {
                                        append(BTreeScalarIndexOperationKind::BSIOP_UPSERT, key, value);
                                    };
                                    if (!summary->fresh) {
                                        if (!staleSummaryPages || summary->free_pages > std::numeric_limits<size_t>::max() - 3U)
                                            throw FormatException(FormatError::FERR_INVALID_STATE,
                                                                  "stale Edge summary rebuild has no bounded page reader");
                                        BTreeIndex oldSummary(RSS_EDGE_FREE_SPACE_INDEX_ID, adjacencyPages, *this);
                                        auto cursor = oldSummary.openRangeCursor(context.current_snapshot, std::nullopt,
                                            size_t(summary->free_pages) + 3U, BufferPoolReadHint::BPRH_NORMAL);
                                        size_t visited = 0U;
                                        while (const auto entry = cursor.next()) {
                                            append(BTreeScalarIndexOperationKind::BSIOP_ERASE, entry->key, 0U);
                                            ++visited;
                                        }
                                        if (visited != summary->free_pages + 2U)
                                            throw FormatException(FormatError::FERR_INVALID_STATE,
                                                                  "stale Edge summary has inconsistent entries");
                                        std::set<uint64_t, std::less<uint64_t>, utils::OwnedPmrAllocator<uint64_t>> pagesSeen;
                                        freePages = 0U;
                                        visitEdgePageLocations(context.current_snapshot, [&](const EntityPageLocation &location) {
                                            if (!pagesSeen.insert(location.page_id.value()).second) return;
                                            const uint16_t remaining = staleSummaryPages->freeSlotCount(
                                                context.current_snapshot, location.page_id, EntityPageRecordKind::EPRK_EDGE);
                                            if (remaining) {
                                                add(EntityFreeSpaceIndex::entryKey(location.page_id, remaining,
                                                                                   EntityPageRecordKind::EPRK_EDGE), 1U);
                                                ++freePages;
                                            }
                                        });
                                    }
                                    if (used != 0U) {
                                        if (freePages == std::numeric_limits<uint64_t>::max())
                                            throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
                                                                  "streamed Edge free-page count overflowed");
                                        const PageId lastPage(pages.first_entity_page.value() + shape.entity_page_count - 1U);
                                        add(EntityFreeSpaceIndex::entryKey(lastPage, uint16_t(capacity - used),
                                                                           EntityPageRecordKind::EPRK_EDGE), 1U);
                                        ++freePages;
                                    }
                                    const uint64_t generation = context.current_superblock.generation.value();
                                    if (generation >= std::numeric_limits<uint64_t>::max() - 1U ||
                                        freePages == std::numeric_limits<uint64_t>::max())
                                        throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
                                                              "streamed Edge summary marker overflowed");
                                    add(EntityFreeSpaceIndex::generationKey(), generation + 2U);
                                    add(EntityFreeSpaceIndex::countKey(), freePages + 1U);
                                    auto plan = adjacencyPages.planScalarIndexTransactionMutation(
                                            context.current_snapshot, RSS_EDGE_FREE_SPACE_INDEX_ID,
                                            operations, true, true);
                                    NativeGraphDeferredIndexPlan deferred;
                                    deferred.mutation.index_id = RSS_EDGE_FREE_SPACE_INDEX_ID;
                                    deferred.mutation.expected_root = plan.expected_root;
                                    deferred.mutation.pool_requests = plan.pool_requests;
                                    const auto addPool = [&](const BTreeNewIndexTreePoolMutation &pool) {
                                        deferred.mutation.new_pool_requests.push_back({
                                            request.index_planning.adjacency.first_slot,
                                            request.index_planning.adjacency.last_slot,
                                            pool.owner_id, pool.appended_page_count});
                                    };
                                    if (plan.new_pool_mutation) addPool(*plan.new_pool_mutation);
                                    for (const auto &pool : plan.additional_new_pool_mutations) addPool(pool);
                                    if (!plan.replacement_root ||
                                        plan.replacement_root->pool_ordinal < plan.pool_requests.size())
                                        throw FormatException(FormatError::FERR_INVALID_STATE,
                                            "streamed Edge summary requires a fresh replacement root");
                                    deferred.mutation.replacement_root = NativeGraphIndexTreeAppendedPageReference{
                                        plan.replacement_root->pool_ordinal - plan.pool_requests.size(),
                                        plan.replacement_root->appended_page_ordinal, true};
                                    deferred.preparer = [preparer = std::move(plan.preparer), checkpointLsn](auto &batches) {
                                        preparer(batches, checkpointLsn);
                                    };
                                    deferred.entity_free_summary_fence = NativeGraphEntityFreeSummaryFence{
                                        RSS_EDGE_FREE_SPACE_INDEX_ID, context.current_superblock.edge_entity_directory_root,
                                        Generation(generation + 1U), freePages};
                                    indexPlans.plans.push_back(std::move(deferred));
                                }
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
