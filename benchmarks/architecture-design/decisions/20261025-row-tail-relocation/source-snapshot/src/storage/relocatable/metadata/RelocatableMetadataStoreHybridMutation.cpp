#include "graph/storage/relocatable/entity/ScanMembershipPublication.hpp"
/**
 * @file RelocatableMetadataStoreHybridMutation.cpp
 * @brief Atomic field mutation and physical maintenance for the isolated native property candidate.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#include "graph/storage/relocatable/property/HybridPropertyPoolRoutes.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include "graph/storage/relocatable/property/HybridPropertyLargeFieldInspection.hpp"
#include "graph/storage/relocatable/property/HybridPropertyValueReader.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyRowPreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyImmutablePreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyContentIndexPreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyDirectoryPreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyPoolPlacement.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyColumnWritePreparation.hpp"
#include "graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/storage/relocatable/schema/ConstraintMutationValidator.hpp"
#include "graph/storage/relocatable/entity/NativeScalarIndexMutationPlanner.hpp"
#include "graph/storage/relocatable/entity/NativeVectorMutationPlanner.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"
#include "graph/utils/OwnedAssociativeContainers.hpp"
#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSet.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityFreeSpaceIndex.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueMaintenanceJournal.hpp"
#include <algorithm>
#include <bit>
#include <optional>

namespace graph::storage {
Superblock RelocatableMetadataStore::mutateHybridPropertyColumns(const NativeHybridPropertyColumnWrite &request,
    LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
    if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
        throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native column publication is disabled");
    const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
    const auto check = [&] { if (request.cancellation) request.cancellation->check(); };
    require(request.memory && request.pending_owner_additions.empty() && (request.replacements.empty() != request.owner_deletions.empty()) &&
        request.replacements.size() <= RSS_HYBRID_GROUP_CAPACITY && request.owner_deletions.size() <= RSS_HYBRID_GROUP_CAPACITY &&
        (request.owner_deletions.empty() || request.field_lifecycle) &&
        request.first_slot.isValid() && request.last_slot >= request.first_slot && request.field_pool_owner_id,
        "native column publication requires a bounded replacement set and destination range");
    const auto account = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(request.memory);
    constexpr size_t maximumBytes = 1024U * 1024U;
    require(account && account->used() <= maximumBytes && account->available() <= maximumBytes - account->used(),
        "native column publication requires an account of at most one MiB");
    check(); utils::OwnedAllocationScope scope(request.memory);
    // Cross-authority copies acquire the request's budget before retaining any
    // PropertyValue tree; delta plans may safely borrow these immutable cells.
    utils::OwnedVector<NativeHybridPropertyColumnReplacement> replacements{
        utils::OwnedPmrAllocator<NativeHybridPropertyColumnReplacement>(request.memory)};
    replacements.reserve(request.replacements.size());
    for (const auto &cell : request.replacements) {
        check();
        if (cell.erase) { replacements.push_back({cell.owner, cell.token, std::monostate{}, true}); continue; }
        PropertyValue scratch;
        require(request.field_lifecycle || CanonicalPropertyValueCodec::encodedSize(cell.propertyValue(scratch)) <=
            RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES - RSS_HYBRID_EXTENSION_HEADER_BYTES,
            "oversized native cells require explicit field lifecycle");
        replacements.push_back(cell);
    }
    // Pay the two standard unique_ptr store objects and their bounded empty
    // container/proxy frontier; retained data buffers have separate admission.
    utils::OwnedHeapReservation sourceStoreCredit(request.memory, 16U * 1024U);
    LargeValueStore sourceValues(io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    SlottedPagePool sourcePages(io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    BTreePageStore sourceTrees(sourcePages, *this);
    EntityPageStore sourceEntities(io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    EntityLabelSetStore sourceLabels(io_, *this, sourcePages, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    auto writer = beginWrite(true); const auto current = bootstrap_.recover();
    require(current.database_id == request.expected_database_id && current.generation == request.expected_generation &&
        current.hybrid_property_descriptor_root.isValid() && current.hybrid_property_descriptor_root == request.expected_descriptor,
        "native column publication has a stale or foreign source fence");
    auto source = snapshots_.pinCurrentUnderWrite(writer, current);
    MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn, request.memory);
    HybridPropertyPoolPlacement placement(*this, batch, writer, source, request.first_slot, request.last_slot, request.memory, check);
    if (!prepareHybridPropertyColumns(batch, writer, source, request, replacements,
        sourcePages, sourceTrees, sourceValues, sourceEntities, sourceLabels, placement)) return current;
    check(); publishBatch(batch, writer, options); placement.remember(); return batch.candidate();
}

bool RelocatableMetadataStore::prepareHybridPropertyColumns(MetadataWriteBatch &batch,
    const MetadataWriteGuard &writer, const MetadataSnapshotPin &source,
    const NativeHybridPropertyColumnWrite &request, std::span<const NativeHybridPropertyColumnReplacement> replacements,
    SlottedPagePool &sourcePages, BTreePageStore &sourceTrees, LargeValueStore &sourceValues,
    EntityPageStore &sourceEntities, EntityLabelSetStore &sourceLabels, HybridPropertyPoolPlacement &placement,
    const NativeHybridVectorPublication *hybridVectors, const NativeHybridTopologyPublication *hybridTopology,
    std::span<const NativeGraphLargeValueMutation> jointValues,
    HybridPropertyReadContext *preparationSource) {
    const auto &current = source.superblock();
    const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
    const auto check = [&] { if (request.cancellation) request.cancellation->check(); };
    require(ownsSnapshot(source) && batch.current() == current &&
        current.database_id == request.expected_database_id && current.generation == request.expected_generation &&
        current.hybrid_property_descriptor_root == request.expected_descriptor &&
        batch.candidate().hybrid_property_descriptor_root == current.hybrid_property_descriptor_root,
        "native field preparation requires one owned source and unpublished descriptor");
    if (hybridTopology || !request.pending_owner_additions.empty())
        require(hybridTopology && !hybridVectors && hybridTopology->metadata_ == this && hybridTopology->source_ == current &&
            hybridTopology->schema_preserved_ && request.field_lifecycle &&
            request.owner_deletions.data() == hybridTopology->owner_deletions_.data() &&
            request.owner_deletions.size() == hybridTopology->owner_deletions_.size() &&
            replacements.data() == hybridTopology->field_changes_.data() && replacements.size() == hybridTopology->field_changes_.size() &&
            replacements.size() <= 64U &&
            request.pending_owner_additions.size() <= 64U,
            "native pending owner liveness requires its sealed new-owner graph publication");
    utils::OwnedAllocationScope allocation(request.memory);
    size_t verifiedVectorSemantics = 0;
    utils::OwnedStdVector<IndexDefinition> schemaDefinitions;
    utils::OwnedStdVector<ConstraintDefinition> constraintDefinitions;
    const bool hasCatalog = bool(locations_.findIndexRootOptional(current, RSS_SYSTEM_CATALOG_INDEX_ID));
    if (hasCatalog) {
        // A field-only change preserves owner counts, labels, relationship
        // types and their indexes. Prove the schema/control ranges before
        // preparing scalar/composite and constraint support deltas below.
        // Embedding deltas and general topology writes need their own coordinators.
        SystemCatalog schema(sourceTrees, *this);
        auto schemaState = schema.readFieldMutationSchema(source, 4096, request.cancellation);
        schemaDefinitions = std::move(schemaState.indexes);
        constraintDefinitions = std::move(schemaState.constraints);
    }
    // A private laboratory source can exercise the original coordinator up to
    // its physical-write boundary. It grants no publication authority and must
    // retain this exact pin/store/file; normal callers always use durableSource.
    HybridPropertyReadContext durableSource(io_, sourcePages, *this, source, request.cancellation);
    auto &context = preparationSource ? *preparationSource : durableSource;
    require(!preparationSource || (context.usesSnapshot(source) && &context.metadata_ == this &&
        context.source_io_ == io_ && !hybridVectors && !hybridTopology && jointValues.empty() &&
        request.pending_owner_additions.empty()),
        "native private field preparation has a foreign source or unsupported compound work");
    const auto sourceGroupFor = [&](EntityPropertyKey owner, const std::function<void(size_t)> &admission,
        SlottedSlotReference *reference = nullptr) {
        // This coordinator consumes complete cold field/liveness plans. The
        // generic groupRecord guard remains for unadapted mutation consumers.
        return context.findGroup(owner, admission, reference, true, true);
    };
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
    // The field planner and private page-image preparation share the same
    // authenticated source pin and pool reader. Reopening either boundary
    // repeats descriptor/pool authentication without granting new authority.
    auto &fields = context.fields_;
    require(bool(context.descriptor_), "native field preparation requires its authenticated descriptor");
#else
    HybridPropertyFieldPageStore fields(io_, *this, source);
#endif
    utils::OwnedVector<HybridPropertyColumnDelta> deltas{utils::OwnedPmrAllocator<HybridPropertyColumnDelta>(request.memory)};
    utils::OwnedVector<const HybridPropertyColumnDelta *> changes{utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(request.memory)};
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
    auto descriptor = *context.descriptor_;
#else
    auto descriptor = pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current).descriptor;
#endif
    std::unique_ptr<PreparedScanMembershipMutation> scanDeletion;
    if constexpr (RSS_SCAN_PATH_FORMAT_ENABLED) {
        if (!hybridTopology && !request.owner_deletions.empty()) {
            utils::OwnedVector<scan_candidate_detail::ScanNodeChange> nodes{
                utils::OwnedPmrAllocator<scan_candidate_detail::ScanNodeChange>(request.memory)};
            for (const auto owner : request.owner_deletions)
                if (owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE)
                    nodes.push_back({owner.entity_id, scan_candidate_detail::ScanNodeChangeKind::SNCK_DELETE, {}});
            if (!nodes.empty()) {
                std::sort(nodes.begin(), nodes.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
                NativeGraphDerivedStateMutation emptyDerived;
                NativeHybridTopologyPublication proof(*this, sourceEntities, current, {}, true,
                    hybridVectors ? *hybridVectors->derived_ : emptyDerived, {}, request.memory, {},
                    request.owner_deletions, false, nodes, request.field_pool_owner_id);
                scanDeletion = prepareScanMembershipMutation(batch, writer, source, proof, request.memory,
                    sourceEntities, sourceLabels, sourceTrees);
            }
        }
    }
    struct InlineChange { const NativeHybridPropertyColumnReplacement *replacement; SlottedSlotReference reference; bool insert = false; };
    utils::OwnedVector<InlineChange> inlineChanges{utils::OwnedPmrAllocator<InlineChange>(request.memory)};
    inlineChanges.reserve(replacements.size());
    utils::OwnedVector<const NativeHybridPropertyColumnReplacement *> coldChanges{
        utils::OwnedPmrAllocator<const NativeHybridPropertyColumnReplacement *>(request.memory)};

    std::optional<HybridPropertyGroupRecord> sourceGroup;
    bool multipleGroups = false;
    SlottedSlotReference groupReference;
    utils::OwnedHeapReservation boundary(request.memory), referenceCredit(request.memory);
    const auto admit = [&](size_t bytes) { boundary.set(bytes); };
    const auto admitReference = [&](size_t bytes) { referenceCredit.set(bytes); };
    if (!request.pending_owner_additions.empty() || !replacements.empty()) {
        const auto owner = !request.pending_owner_additions.empty() ? request.pending_owner_additions.front() : replacements.front().owner;
        sourceGroup = sourceGroupFor(owner, std::ref(admit), &groupReference);
        boundary.set(0); require(bool(sourceGroup), "native field work has no installed source group");
    }
    SystemCatalog propertyTokens(sourceTrees, *this);
    const auto registeredToken = [&](uint32_t token) {
        return !hasCatalog || bool(propertyTokens.findPropertyName(source, token));
    };
    const auto allocatedToken = [&](uint32_t token) { return
        (hybridVectors && std::ranges::binary_search(hybridVectors->new_property_tokens_, token)) ||
        (hybridTopology && std::ranges::binary_search(hybridTopology->new_property_tokens_, token)); };
    if (hasCatalog) {
        const auto newTokens = hybridVectors ? hybridVectors->new_property_tokens_ : hybridTopology ?
            hybridTopology->new_property_tokens_ : std::span<const uint32_t>{};
        for (const auto token : newTokens) {
            check(); require(token && !propertyTokens.findPropertyName(source, token),
                "native new property token already has a source Catalog identity");
        }
        for (const auto &cell : replacements) {
            check(); const bool allocated = allocatedToken(cell.token);
            require(allocated || registeredToken(cell.token),
                "native field token is missing its Catalog identity and sealed allocation");
            if (allocated) {
                require(context.visitField(cell.owner, cell.token, [](const auto &) {}, [](const auto &) {},
                    std::ref(admit), std::ref(admitReference)) == HybridPropertyReadContext::FieldRoute::HFR_MISSING,
                    "native new property token collides with existing field membership");
                boundary.set(0); referenceCredit.set(0);
            }
        }
    }
    struct EntityImage { EntityPage page; uint64_t owner_id; uint16_t source_free = 0; };
    utils::OwnedHeapReservation entityCredit(request.memory);
    utils::OwnedVector<EntityImage> entityImages{utils::OwnedPmrAllocator<EntityImage>(request.memory)};
    struct LabelImage { SlottedPage page; uint64_t owner_id; };
    utils::OwnedVector<LabelImage> labelImages{utils::OwnedPmrAllocator<LabelImage>(request.memory)};
    utils::OwnedVector<uint64_t> erasedNodes{utils::OwnedPmrAllocator<uint64_t>(request.memory)};
    utils::OwnedVector<uint64_t> erasedEdges{utils::OwnedPmrAllocator<uint64_t>(request.memory)};
    using AdjacencySet = FixedBTreeMap<RSS_GLOBAL_ADJACENCY_KEY_BYTES, 0>;
    utils::OwnedVector<AdjacencySet::Key> erasedAdjacency{utils::OwnedPmrAllocator<AdjacencySet::Key>(request.memory)};
    // Unknown State may carry property-derived evidence even for a SET.
    // Apply the same conservative lifecycle fence to every mutating entry.
    if (current.state_directory_root.isValid()) {
        // The canonical inline maintenance journal contains value IDs and
        // logical scan cursors, not Node/Edge ownership. A graph write
        // advances metadata generation and makes that evidence stale.
        utils::OwnedHeapReservation stateFrontier(request.memory, 16U * 1024U);
        StateRecordStore states(sourcePages, *this); size_t controls = 0;
        require(locations_.visitStatesAfter(current, 0, 64, [&](const StateLocation &location) {
            check(); require(++controls <= 64, "native field publication exceeded its State control census");
            const auto record = states.read(source, location.state_id);
            require((record.key == RSS_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY || record.key == RSS_NATIVE_LARGE_VALUE_MAINTENANCE_JOURNAL_KEY) &&
                record.storage_kind == StateRecordStorageKind::SRS_INLINE && !record.large_value_id.isValid(),
                "native field publication needs the complete derived State lifecycle");
            const auto journal = LargeValueMaintenanceJournalCodec::decode(record.inline_bytes);
            require(journal.scan_generation <= current.generation, "native field publication found future journal evidence");
            return true;
        }), "native field publication did not complete its bounded State control census");
    }
    if (!request.owner_deletions.empty() && !hybridTopology) {
        const size_t nodeCount = size_t(std::ranges::count_if(request.owner_deletions, [](const auto owner) { return owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE; }));
        const size_t edgeCount = request.owner_deletions.size() - nodeCount;
        const auto adjacencyRoot = locations_.findIndexRootOptional(current, RSS_GLOBAL_ADJACENCY_INDEX_ID);
        if (nodeCount && !adjacencyRoot) {
            bool hasEdge = false;
            require(locations_.visitEdgePageLocationsAfter(current, 0, 1, [&](const auto &) { hasEdge = true; return false; }) && !hasEdge,
                "native Node deletion cannot prove isolation without the global adjacency root");
        }
        if (edgeCount) require(bool(adjacencyRoot), "native Edge deletion has no global adjacency root");
        SharedPagePoolAllocator entityPools(io_, *this, source);
        entityImages.reserve(request.owner_deletions.size());
        erasedNodes.reserve(nodeCount); erasedEdges.reserve(edgeCount); erasedAdjacency.reserve(edgeCount * 2U);
        size_t fieldsSeen = 0;
        for (const auto owner : request.owner_deletions) {
            check(); require(owner.isValid(), "native owner deletion has an invalid owner namespace or ID");
            const bool nodes = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE;
            if (nodes) {
                const auto record = sourceEntities.readNode(source, owner.entity_id);
                if (adjacencyRoot) {
                    AdjacencySet adjacency(RSS_GLOBAL_ADJACENCY_INDEX_ID, sourceTrees, *this,
                        BTreeRootReadBinding{current.database_id, current.generation, *adjacencyRoot});
                    for (const auto direction : {GlobalAdjacencyDirection::GAD_OUT, GlobalAdjacencyDirection::GAD_IN}) {
                        const auto lower = AdjacencyKeyCodec::lowerBoundFixed(direction, owner.entity_id);
                        require(adjacency.visitPrefix(source, std::span<const std::byte>(lower).first(9), request.owner_deletions.size() + 1U,
                            [&](const auto &entry) {
                                check(); const auto key = AdjacencyKeyCodec::decode(entry.key);
                                require(std::ranges::find(request.owner_deletions, EntityPropertyKey{EntityPropertyOwnerKind::EPOK_EDGE, key.edge_id}) != request.owner_deletions.end(),
                                    "native Node deletion has an incident Edge outside its atomic deletion batch");
                                const auto edge = sourceEntities.readEdge(source, key.edge_id);
                                require(key.direction == direction && key.node_id == owner.entity_id &&
                                    (direction == GlobalAdjacencyDirection::GAD_OUT ? edge.source_node_id : edge.target_node_id) == owner.entity_id,
                                    "native Node deletion found a stale incident adjacency key");
                                return true;
                            }, std::ref(admit), request.cancellation), "native Node deletion exceeded its bounded incident Edge proof");
                        boundary.set(0);
                    }
                }
                utils::OwnedHeapReservation labelCredit(request.memory, 3U * utils::detail::standardContainerProxyBytes());
                const auto admitLabels = [&](size_t bytes) {
                    labelCredit.set(3U * utils::detail::standardContainerProxyBytes() +
                        2U * utils::detail::standardAllocatorRequestBytes(bytes, alignof(uint64_t)));
                };
                const auto labels = sourceLabels.readNodeLabels(source, record, std::ref(admitLabels));
                static_cast<void>(labels);
                if (!record.label_overflow.isEmpty()) {
                    const auto reference = record.label_overflow;
                    auto image = std::find_if(labelImages.begin(), labelImages.end(),
                        [&](const auto &candidate) { return candidate.page.pageId() == reference.page_id; });
                    if (image == labelImages.end()) {
                        utils::OwnedHeapReservation pageCredit(request.memory,
                            utils::detail::standardAllocatorRequestBytes(RSS_PAGE_SIZE, alignof(std::byte)) +
                            utils::detail::standardContainerProxyBytes());
                        const auto contents = entityPools.read(source, reference.page_id);
                        require(contents.pool.kind == SegmentKind::SGK_ENTITY_PAGE_POOL &&
                            contents.owner.kind == PageOwnerKind::POK_ENTITY_LABEL,
                            "native labeled owner deletion has a foreign LabelSet page");
                        auto page = SlottedPage::open(contents.owner, contents.payload, request.memory);
                        require(page.stats().contiguous_free_bytes == contents.location.free_bytes,
                            "native labeled owner deletion found a stale LabelSet free-byte hint");
                        labelImages.push_back({std::move(page), contents.owner.owner_id}); image = std::prev(labelImages.end());
                    }
                    image->page.erase({reference.page_id, reference.slot_id, reference.slot_generation});
                }
            } else {
                const auto edge = sourceEntities.readEdge(source, owner.entity_id);
                static_cast<void>(sourceEntities.readNode(source, edge.source_node_id));
                static_cast<void>(sourceEntities.readNode(source, edge.target_node_id));
                AdjacencySet adjacency(RSS_GLOBAL_ADJACENCY_INDEX_ID, sourceTrees, *this,
                    BTreeRootReadBinding{current.database_id, current.generation, *adjacencyRoot});
                for (const auto direction : {GlobalAdjacencyDirection::GAD_OUT, GlobalAdjacencyDirection::GAD_IN}) {
                    auto key = AdjacencyKeyCodec::lowerBoundFixed(direction,
                        direction == GlobalAdjacencyDirection::GAD_OUT ? edge.source_node_id : edge.target_node_id);
                    for (unsigned i = 0; i < 8; ++i) key[9 + i] = std::byte((owner.entity_id >> (56U - i * 8U)) & 255U);
                    require(bool(adjacency.find(source, key, std::ref(admit), request.cancellation)),
                        "native Edge deletion is missing an authenticated direction"); boundary.set(0); erasedAdjacency.push_back(key);
                }
            }
            const auto membership = context.visitOwner(owner, 4097, [&](uint32_t token, HybridPropertySource route) {
                check(); require(fieldsSeen < 4096, "native owner deletion exceeded its bounded field work"); ++fieldsSeen;
                require(registeredToken(token), "native owner deletion found a field without a Catalog identity");
                if (route == HybridPropertySource::HPS_EXTENSION)
                    static_cast<void>(context.visitField(owner, token, [](const auto &) {}, [&](const HybridPropertyExtensionView &value) {
                        if (value.reference()) {
                            HybridPropertyLargeFieldInspection inspection(context, source, value, sourceValues, request.memory);
                            static_cast<void>(inspection.inspect());
                        }
                    }, std::ref(admit), std::ref(admitReference)));
                referenceCredit.set(0); return true;
            }, std::ref(admit)); boundary.set(0);
            require(membership.routed && membership.owner_live && membership.complete,
                "native owner deletion has no complete live group membership");
            const auto location = nodes ? locations_.findNodePageLocation(current, owner.entity_id) : locations_.findEdgePageLocation(current, owner.entity_id);
            auto image = std::find_if(entityImages.begin(), entityImages.end(), [&](const auto &value) { return value.page.header().page_id == location.page_id; });
            if (image == entityImages.end()) {
                entityCredit.set((entityImages.size() + 1U) * (utils::detail::standardAllocatorRequestBytes(
                    RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, alignof(std::byte)) + utils::detail::standardContainerProxyBytes()));
                const auto contents = entityPools.read(source, location.page_id);
                require(contents.owner.kind == PageOwnerKind::POK_ENTITY, "native owner deletion has a foreign Entity page");
                entityImages.push_back({EntityPage::open(contents.owner, contents.payload), contents.owner.owner_id}); image = std::prev(entityImages.end());
                image->source_free=uint16_t(image->page.header().slot_capacity-image->page.header().live_count);
            }
            image->page.erase(location.slot_id, location.slot_generation, owner.entity_id);
            if (nodes) erasedNodes.push_back(owner.entity_id); else erasedEdges.push_back(owner.entity_id);
        }
        std::sort(erasedNodes.begin(), erasedNodes.end()); std::sort(erasedEdges.begin(), erasedEdges.end());
        std::sort(erasedAdjacency.begin(), erasedAdjacency.end());
    }
    if (!request.owner_deletions.empty()) {
        sourceGroup = sourceGroupFor(request.owner_deletions.front(), std::ref(admit), &groupReference);
        boundary.set(0); require(bool(sourceGroup), "native owner deletion has no installed source group");
    }
    deltas.reserve(replacements.size()); changes.reserve(replacements.size());
    for (const auto &replacement : replacements) {
        check(); require(request.field_lifecycle || !replacement.erase, "compatible column writes cannot remove memberships");
        require(std::ranges::find(request.owner_deletions, replacement.owner) == request.owner_deletions.end(),
            "native field change targets an owner erased by the same batch");
        if (request.field_lifecycle) {
            require(replacement.owner.isValid() && replacement.token, "native field mutation has an invalid key");
            for (const auto &previous : replacements) {
                if (&previous == &replacement) break;
                require(previous.owner != replacement.owner || previous.token != replacement.token,
                    "native field mutation repeats an owner/token");
            }
            const auto group = sourceGroupFor(replacement.owner, std::ref(admit)); boundary.set(0);
            require(bool(group), "native field mutation has no source group");
            if (sourceGroup && *group != *sourceGroup) {
                multipleGroups = true;
            }
            if (!sourceGroup || *group != *sourceGroup) {
                sourceGroup = *group;
                const size_t base = replacement.owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? 0U : 4U;
                const auto binding = HybridPropertyDirectoryCodec::bindRoot(descriptor, source,
                    static_cast<HybridPropertyRootOrdinal>(base + 3));
                HybridPropertyGroupDirectoryMap map(binding.root.index_id, sourceTrees, *this, binding);
                const auto locator = map.find(source, HybridPropertyDirectoryCodec::groupKey(group->group_id),
                    std::ref(admit), request.cancellation); boundary.set(0);
                require(bool(locator), "native field mutation lost its group locator");
                groupReference = HybridPropertyDirectoryCodec::decodeLocator(*locator);
            }
        }
        PropertyValue scalar;
        const auto &value = replacement.propertyValue(scalar);
        if (std::ranges::find(request.pending_owner_additions, replacement.owner) != request.pending_owner_additions.end()) {
            if (sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
                !context.columnMembership(*sourceGroup).ordinal(replacement.token)) {
                require(!replacement.erase, "pending cold fields cannot erase an absent source value");
                coldChanges.push_back(&replacement); continue;
            }
            require(!replacement.erase && std::ranges::find(request.pending_owner_additions, replacement.owner) != request.pending_owner_additions.end(),
                "native creation field does not name a declared pending owner");
            auto insertion = HybridPropertyColumnDelta::planPendingOwnerColumnMember(context, source, replacement.owner,
                replacement.token, batch.generation(), request.memory, value,
                sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER);
            if (insertion) deltas.push_back(std::move(*insertion));
            else inlineChanges.push_back({&replacement, {}, true});
            continue;
        }
        if (sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
            const auto page = fields.read(source, sourceGroup->column_membership.page_id);
            const auto matrix = page.view().readColumnMembership(sourceGroup->column_membership, *sourceGroup,
                descriptor.semantic_generation);
            if (!std::binary_search(matrix.tokens.begin(), matrix.tokens.begin() + matrix.column_count, replacement.token)) {
                require(request.field_lifecycle, "cold fields require explicit lifecycle coordination");
                coldChanges.push_back(&replacement); continue;
            }
        }
        auto delta = HybridPropertyColumnDelta::planExistingColumn(context, source, replacement.owner,
            replacement.token, batch.generation(), request.memory, replacement.erase ? nullptr : &value, false,
            RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, bool(preparationSource) ||
                sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER);
        if (delta) { deltas.push_back(std::move(*delta)); continue; }
        require(request.field_lifecycle, "native column replacement does not name an existing column member");
        const auto route = context.visitField(replacement.owner, replacement.token, [](const auto &) {}, [](const auto &) {},
            std::ref(admit), std::ref(admitReference)); boundary.set(0); referenceCredit.set(0);
        if (route == HybridPropertyReadContext::FieldRoute::HFR_MISSING) {
            require(bool(sourceGroup->live & (UINT64_C(1) << hybridPropertySlotFor(replacement.owner))),
                "native field insertion requires a live owner");
            if (replacement.erase) continue;
            auto insertion = HybridPropertyColumnDelta::planMissingColumnMember(context, source, replacement.owner,
                replacement.token, batch.generation(), request.memory, value, bool(preparationSource));
            if (insertion) deltas.push_back(std::move(*insertion));
            else inlineChanges.push_back({&replacement, {}, true});
            continue;
        }
        require(route == HybridPropertyReadContext::FieldRoute::HFR_FOUND, "native field mutation has no native source");
        const size_t base = replacement.owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? 0U : 4U;
        const auto binding = HybridPropertyDirectoryCodec::bindRoot(descriptor, source,
            static_cast<HybridPropertyRootOrdinal>(base + 2));
        HybridPropertyFieldDirectoryMap map(binding.root.index_id, sourceTrees, *this, binding);
        const auto locator = map.find(source, HybridPropertyDirectoryCodec::ownerKey(replacement.owner, replacement.token),
            std::ref(admit), request.cancellation); boundary.set(0);
        require(bool(locator), "native field mutation does not name an existing extension");
        const auto ref = HybridPropertyDirectoryCodec::decodeLocator(*locator);
        bool unchanged = false;
        {
            const auto page = fields.read(source, ref.page_id);
            const auto extension = page.view().readExtension(ref, replacement.owner, replacement.token, *sourceGroup,
                descriptor.semantic_generation, std::ref(admitReference));
            if (extension.reference()) {
                HybridPropertyLargeFieldInspection inspection(context, source, extension, sourceValues, request.memory);
                static_cast<void>(inspection.inspect());
                constexpr size_t carrierFraming = RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + 3U * sizeof(uint32_t);
                if (!replacement.erase && extension.reference()->logical_bytes ==
                    carrierFraming + CanonicalPropertyValueCodec::encodedSize(value)) {
                    const std::function<void()> checkpoint = std::ref(check);
                    detail::PropertyDecodeWorkspace valuesCredit(request.memory, &checkpoint);
                    const auto old = HybridPropertyValueReader::read(context, source, replacement.owner, replacement.token, &sourceValues);
                    require(old.value.has_value(), "native LargeValue comparison lost its selected field");
                    unchanged = old.value->value == value;
                }
            } else if (!replacement.erase) {
                utils::OwnedHeapReservation valueCredit(request.memory);
                const auto oldValue = extension.decodeInline([&](const auto &shape) { valueCredit.set(shape.maximum_heap_request_bytes); }, check);
                unchanged = oldValue == value;
            }
        }
        referenceCredit.set(0);
        if (unchanged) continue;
        inlineChanges.push_back({&replacement, ref});
    }
    struct ColdSourceGroup { HybridPropertyGroupRecord source; SlottedSlotReference reference; };
    utils::OwnedVector<ColdSourceGroup> coldSourceGroups{utils::OwnedPmrAllocator<ColdSourceGroup>(request.memory)};
    const auto collectGroup = [&](EntityPropertyKey owner) {
        SlottedSlotReference reference;
        const auto group = sourceGroupFor(owner, std::ref(admit), &reference); boundary.set(0);
        require(bool(group), "native batch lost an authenticated source group");
        const auto found = std::ranges::find_if(coldSourceGroups, [&](const auto &entry) {
            return entry.source.owner_kind == group->owner_kind && entry.source.group_id == group->group_id;
        });
        if (found == coldSourceGroups.end()) coldSourceGroups.push_back({*group, reference});
        else require(found->source == *group && found->reference == reference, "native batch source group fences disagree");
    };
    for (const auto &cell : replacements) collectGroup(cell.owner);
    for (const auto owner : request.owner_deletions) collectGroup(owner);
    for (const auto owner : request.pending_owner_additions) collectGroup(owner);
    const bool anyColdGroup = std::ranges::any_of(coldSourceGroups, [](const auto &entry) {
        return entry.source.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER;
    });
    require(!anyColdGroup || std::ranges::all_of(coldSourceGroups, [](const auto &entry) {
        return entry.source.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER;
    }), "native batch cannot mix unadapted and cold group lifecycles");
    multipleGroups = coldSourceGroups.size() > 1;
    const bool coldDeletion = anyColdGroup && !request.owner_deletions.empty();
    if (coldDeletion) {
        // Fold removals immediately per column: at most64 full column plans,
        // never4096 simultaneous source-based images or borrowed temporaries.
        for (const auto owner : request.owner_deletions) {
            const auto group = sourceGroupFor(owner, std::ref(admit)); boundary.set(0);
            require(bool(group), "cold owner deletion lost its source group");
            const auto matrix = context.columnMembership(*group);
            for (unsigned i = 0; i < matrix.column_count; ++i) {
                const auto token = matrix.tokens[i]; if (!matrix.contains(owner, token)) continue;
                auto removal = HybridPropertyColumnDelta::planExistingColumn(context, source, owner, token,
                    batch.generation(), request.memory, nullptr, false, RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, true);
                require(bool(removal), "cold owner deletion lost its original column member");
                const auto prior = std::find_if(deltas.begin(), deltas.end(), [&](const auto &plan) {
                    return plan.columnReference() == removal->columnReference();
                });
                if (prior == deltas.end()) deltas.push_back(std::move(*removal));
                else {
                    const std::array<const HybridPropertyColumnDelta *, 2> input{&*prior, &*removal};
                    *prior = HybridPropertyColumnDelta::mergeExistingColumnChanges(input, request.memory,
                        RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, request.cancellation, true,
                        !request.pending_owner_additions.empty());
                }
            }
        }
    }
    for (const auto &delta : deltas) changes.push_back(&delta);
    utils::OwnedVector<HybridPropertyLargeFieldPreparation> largeFields{
        utils::OwnedPmrAllocator<HybridPropertyLargeFieldPreparation>(request.memory)};
    const auto resolveLarge = [&](EntityPropertyKey owner, uint32_t token, const PropertyValue &value) {
        check(); auto proposal = HybridPropertyLargeFieldPreparation::prepare(*this, source, sourceValues, owner, token,
            value, LargeValueId(batch.candidate().next_large_value_id), request.memory, {}, LargeValueExtentClass::LVEC_32K,
            RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES, request.cancellation);
        const HybridPropertyLargeFieldPreparation *selected = &proposal;
        if (!proposal.reused()) {
            for (const auto &previous : largeFields) {
                check(); if (std::ranges::equal(previous.payload(), proposal.payload())) { selected = &previous; break; }
            }
            if (selected == &proposal) {
                if (largeFields.size() == largeFields.capacity()) largeFields.reserve(largeFields.empty() ? 1U : largeFields.size() * 2U);
                require(batch.allocateLargeValueId() == proposal.reference().value_id,
                    "native field value lost its batch-local identity sequence");
                largeFields.push_back(std::move(proposal)); selected = &largeFields.back();
            }
        }
        const auto encodedBytes = RSS_LARGE_VALUE_REF_HEADER_BYTES + selected->reference().media_type.size();
        utils::OwnedHeapReservation encodingCredit(request.memory, utils::detail::standardContainerProxyBytes() +
            utils::detail::standardAllocatorRequestBytes(encodedBytes, alignof(std::byte)));
        const auto encoded = LargeValueRefCodec::encode(selected->reference());
        utils::OwnedHeapReservation decodedCredit(request.memory);
        return HybridPropertyExtensionView::encodeReference(owner, token, batch.generation(), encoded,
            RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, *request.memory, [&](size_t bytes) { decodedCredit.set(bytes); });
    };
    const HybridPropertyDemotionPreparation::LargeFieldResolver largeResolver = std::ref(resolveLarge);
    utils::OwnedVector<HybridPropertyColumnDelta> merged{utils::OwnedPmrAllocator<HybridPropertyColumnDelta>(request.memory)};
    bool coldPlansMerged = false;
    utils::OwnedVector<const HybridPropertyColumnDelta *> demotions{
        utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(request.memory)};
    const bool coldPending = anyColdGroup &&
        !request.pending_owner_additions.empty();
    if (sourceGroup && sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
        request.field_lifecycle) {
        std::sort(changes.begin(), changes.end(), [](auto *left, auto *right) {
            return left->columnReference() < right->columnReference();
        });
        merged.reserve(changes.size());
        for (size_t begin = 0; begin < changes.size();) {
            size_t end = begin + 1;
            while (end < changes.size() && changes[end]->columnReference() == changes[begin]->columnReference()) ++end;
            merged.push_back(HybridPropertyColumnDelta::mergeExistingColumnChanges(
                std::span(changes).subspan(begin, end - begin), request.memory,
                RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, request.cancellation, true, coldPending));
            begin = end;
        }
        changes.clear();
        for (const auto &delta : merged) {
            changes.push_back(&delta);
            if (delta.action() == HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD) demotions.push_back(&delta);
        }
        coldPlansMerged = true;
    }
    std::optional<HybridPropertyColumnWritePreparation> compatible;
    std::optional<HybridPropertyDemotionPreparation> lifecycle;
    const bool onlyCompatibleColumns = multipleGroups && request.pending_owner_additions.empty() &&
        request.owner_deletions.empty() && inlineChanges.empty() && std::ranges::all_of(changes, [](const auto *change) {
            const auto effects = change->effects();
            return change->action() != HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD &&
                !effects.memberships && !effects.columns && !effects.clear_live && !effects.set_live;
        });
    if (!coldChanges.empty() || !demotions.empty() || coldDeletion || coldPending) {
        require(anyColdGroup && inlineChanges.empty(), "cold field preparation requires authenticated cold source groups");
        if (!coldPlansMerged) {
        std::sort(changes.begin(), changes.end(), [](auto *left, auto *right) {
            return left->columnReference() < right->columnReference();
        });
        merged.reserve(changes.size());
        for (size_t begin = 0; begin < changes.size();) {
            size_t end = begin + 1;
            while (end < changes.size() && changes[end]->columnReference() == changes[begin]->columnReference()) ++end;
            merged.push_back(HybridPropertyColumnDelta::mergeExistingColumnChanges(
                std::span(changes).subspan(begin, end - begin), request.memory,
                RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, request.cancellation));
            begin = end;
        }
        changes.clear(); for (const auto &delta : merged) changes.push_back(&delta);
        }
        require(changes.size() <= 64U, "cold batch exceeds its original column-plan bound");
        utils::OwnedVector<const HybridPropertyColumnDelta *> compatibleHot{
            utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(request.memory)};
        for (const auto *plan : changes)
            if (plan->action() != HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD) compatibleHot.push_back(plan);
        if (coldPending) lifecycle.emplace(HybridPropertyDemotionPreparation::preparePendingOwners(batch, context,
            source, fields, compatibleHot, request.pending_owner_additions, request.memory, request.cancellation, {}, true));
        else if (!compatibleHot.empty()) lifecycle.emplace(HybridPropertyDemotionPreparation(batch, source, fields,
            compatibleHot, request.memory, request.cancellation, {}, false, {}, true));
        else { lifecycle.emplace(HybridPropertyDemotionPreparation(request.memory)); lifecycle->group_ = *sourceGroup; }
        utils::OwnedVector<EntityPropertyKey> affectedOwners{utils::OwnedPmrAllocator<EntityPropertyKey>(request.memory)};
        const auto affect = [&](EntityPropertyKey owner) {
            if (std::ranges::find(affectedOwners, owner) == affectedOwners.end()) {
                require(affectedOwners.size() < 64U, "cold batch exceeds64 actual changed or migrated owners");
                affectedOwners.push_back(owner);
            }
        };
        for (const auto &cell : replacements) affect(cell.owner);
        size_t emittedMemberships = 0;
        for (const auto &work : coldSourceGroups) {
        const auto sourceGroup = std::optional(work.source);
        const auto sameGroup = [&](EntityPropertyKey owner) { return owner.owner_kind == work.source.owner_kind &&
            hybridPropertyGroupFor(owner) == work.source.group_id; };
        const auto matrix = context.columnMembership(work.source);
        utils::OwnedVector<const HybridPropertyColumnDelta *> groupDemotions{
            utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(request.memory)};
        for (const auto *plan : demotions) if (sameGroup(plan->owner())) {
            groupDemotions.push_back(plan);
            for (const auto &cell : plan->demotedCells()) if (cell.kind != HybridPropertyColumnDelta::CellKind::HCC_MISSING)
                require(++emittedMemberships <= 4096U, "cold batch exceeds4096 emitted demotion memberships");
        }
        utils::OwnedVector<EntityPropertyKey> groupDeleted{utils::OwnedPmrAllocator<EntityPropertyKey>(request.memory)},
            groupPending{utils::OwnedPmrAllocator<EntityPropertyKey>(request.memory)};
        for (const auto owner : request.owner_deletions) if (sameGroup(owner)) groupDeleted.push_back(owner);
        for (const auto owner : request.pending_owner_additions) if (sameGroup(owner)) groupPending.push_back(owner);
        auto finalHot = matrix;
        uint64_t coldOwners = 0, structuralOwners = 0;
        for (const auto *cell : coldChanges) if (sameGroup(cell->owner)) coldOwners |= UINT64_C(1) << hybridPropertySlotFor(cell->owner);
        for (const auto *plan : groupDemotions) {
            finalHot.eraseColumn(plan->sourceColumn().identity.token);
            structuralOwners |= plan->sourceColumn().presence;
            for (unsigned slot = 0; slot < 64; ++slot)
                if (plan->demotedCells()[slot].kind != HybridPropertyColumnDelta::CellKind::HCC_MISSING)
                    structuralOwners |= UINT64_C(1) << slot;
        }
        uint64_t deletedOwners = 0;
        for (const auto owner : groupDeleted)
            deletedOwners |= UINT64_C(1) << hybridPropertySlotFor(owner);
        uint64_t pendingOwners = 0;
        for (const auto owner : groupPending)
            pendingOwners |= UINT64_C(1) << hybridPropertySlotFor(owner);
        coldOwners |= structuralOwners | deletedOwners | pendingOwners;
        for (unsigned slot = 0; slot < 64; ++slot) if (coldOwners & (UINT64_C(1) << slot))
            affect({work.source.owner_kind, (work.source.group_id - 1U) * 64U + slot + 1U});
        struct PreparedRow { EntityPropertyKey owner; SlottedSlotReference reference; HybridColdRowInput row; };
        utils::OwnedVector<std::pmr::vector<std::byte>> roots{
            utils::OwnedPmrAllocator<std::pmr::vector<std::byte>>(request.memory)};
        utils::OwnedVector<PreparedRow> rows{utils::OwnedPmrAllocator<PreparedRow>(request.memory)};
        roots.reserve(64); rows.reserve(64);
        const std::function<void()> checkpoint = std::ref(check);
        const uint32_t ownerType = sourceGroup->owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
        for (unsigned slot = 0; slot < 64; ++slot) {
            const auto ownerBit = UINT64_C(1) << slot;
            if (!(coldOwners & ownerBit)) continue;
            check(); const EntityPropertyKey owner{sourceGroup->owner_kind, (sourceGroup->group_id - 1U) * 64U + slot + 1U};
            utils::OwnedHeapReservation decodeCredit(request.memory);
            struct DecodeOwner { utils::OwnedHeapReservation &credit; const std::function<void()> &check; } decodeOwner{decodeCredit, checkpoint};
            property_decode::AllocationScope decoding(&decodeOwner, [](void *p, size_t bytes) {
                auto &credit = static_cast<DecodeOwner *>(p)->credit;
                credit.set(utils::detail::addStandardHeapBytes(credit.bytes(), bytes));
            }, [](void *p) { static_cast<DecodeOwner *>(p)->check(); });
            auto collection = HybridPropertyValueReader::readCollection(context, source, owner, ownerType, &sourceValues);
            require(collection.routed, "cold native field lost its authenticated collection");
            utils::OwnedVector<BorrowedPropertyCollectionRootEntry> entries{
                utils::OwnedPmrAllocator<BorrowedPropertyCollectionRootEntry>(request.memory)};
            if (collection.value) for (const auto &entry : collection.value->entries) {
                if (!std::binary_search(finalHot.tokens.begin(), finalHot.tokens.begin() + finalHot.column_count, entry.key_token))
                    entries.push_back({entry.key_token, &entry.value});
            }
            utils::OwnedVector<PropertyValue> scalars{utils::OwnedPmrAllocator<PropertyValue>(request.memory)};
            scalars.reserve(replacements.size()); bool changed = bool(structuralOwners & ownerBit);
            for (const auto &cell : replacements) {
                if (cell.owner != owner || std::binary_search(finalHot.tokens.begin(),
                    finalHot.tokens.begin() + finalHot.column_count, cell.token)) continue;
                check();
                auto found = std::lower_bound(entries.begin(), entries.end(), cell.token,
                    [](const auto &entry, uint32_t token) { return entry.key_token < token; });
                if (cell.erase) {
                    if (found != entries.end() && found->key_token == cell.token) { entries.erase(found); changed = true; }
                    continue;
                }
                scalars.emplace_back(); const auto &value = cell.propertyValue(scalars.back());
                if (found != entries.end() && found->key_token == cell.token) {
                    if (*found->value == value) continue;
                    found->value = &value;
                } else entries.insert(found, {cell.token, &value});
                changed = true;
            }
            if (deletedOwners & ownerBit) { entries.clear(); changed = true; }
            if (!changed || (entries.empty() && !(matrix.cold_rows.owners & ownerBit))) continue;
            require(entries.size() <= UINT32_MAX, "cold field count overflows its original envelope");
            std::pmr::vector<std::byte> encoded(request.memory.get()); bool large = false;
            if (!entries.empty()) {
                const auto bytes = EntityPropertyCollectionRootCodec::encodedSizeBorrowed(owner.entity_id, ownerType, entries, checkpoint);
                large = bytes > HybridPropertyColdRowBlock::MAX_UNCOMPRESSED_BYTES - HybridPropertyColdRowBlock::DIRECTORY_ENTRY_BYTES;
                if (!large) {
                    encoded = EntityPropertyCollectionRootCodec::encodeBorrowed(owner.entity_id, ownerType, entries, *request.memory, checkpoint);
                    if (bytes > HybridPropertyColdRowBlock::MAX_STORED_BYTES - HybridPropertyColdRowBlock::DIRECTORY_ENTRY_BYTES) {
                        const std::array<HybridColdRowInput, 1> probe{{{uint8_t(hybridPropertySlotFor(owner)),
                            uint32_t(entries.size()), false, encoded}}};
                        try { static_cast<void>(HybridPropertyColdRowBlock::encode(owner.owner_kind, sourceGroup->group_id,
                            batch.generation(), probe, request.memory)); }
                        catch (const std::length_error &) { large = true; }
                    }
                }
                if (large) {
                    auto proposal = HybridPropertyLargeFieldPreparation::prepareRootBorrowed(*this, source, sourceValues,
                        owner, entries, LargeValueId(batch.candidate().next_large_value_id), request.memory, request.cancellation);
                    const HybridPropertyLargeFieldPreparation *selected = &proposal;
                    if (!proposal.reused()) {
                        for (const auto &previous : largeFields) if (std::ranges::equal(previous.payload(), proposal.payload())) {
                            selected = &previous; break;
                        }
                        if (selected == &proposal) {
                            require(batch.allocateLargeValueId() == proposal.reference().value_id,
                                "cold native root lost its batch-local identity sequence");
                            largeFields.push_back(std::move(proposal)); selected = &largeFields.back();
                        }
                    }
                    constexpr size_t maximumReference = RSS_LARGE_VALUE_REF_HEADER_BYTES + RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES;
                    utils::OwnedHeapReservation referenceCredit(request.memory,
                        2U * utils::detail::standardContainerProxyBytes() +
                        utils::detail::standardAllocatorRequestBytes(maximumReference, alignof(std::byte)) +
                        utils::detail::standardAllocatorRequestBytes(RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES +
                            maximumReference, alignof(std::byte)));
                    const EntityPropertyLargeValueReference reference{owner.entity_id, ownerType, selected->reference()};
                    const auto bytes = EntityPropertyLargeValueReferenceCodec::encode(reference);
                    encoded.assign(bytes.begin(), bytes.end());
                }
            }
            roots.push_back(std::move(encoded));
            rows.push_back({owner, matrix.cold_rows.roots[hybridPropertySlotFor(owner)],
                {uint8_t(hybridPropertySlotFor(owner)), uint32_t(entries.size()), large, roots.back()}});
        }
        std::sort(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
            return left.reference < right.reference || (left.reference == right.reference && left.owner.entity_id < right.owner.entity_id);
        });
        utils::OwnedVector<HybridColdRowInput> inputs{utils::OwnedPmrAllocator<HybridColdRowInput>(request.memory)};
        utils::OwnedVector<HybridPropertyDemotionPreparation::ColdBlockChange> blocks{
            utils::OwnedPmrAllocator<HybridPropertyDemotionPreparation::ColdBlockChange>(request.memory)};
        inputs.reserve(rows.size()); blocks.reserve(rows.size());
        for (size_t begin = 0; begin < rows.size();) {
            size_t end = begin + 1; while (end < rows.size() && rows[end].reference == rows[begin].reference) ++end;
            const auto offset = inputs.size();
            for (size_t i = begin; i < end; ++i) inputs.push_back(rows[i].row);
            blocks.push_back({rows[begin].owner, rows[begin].reference, std::span(inputs).subspan(offset, end - begin)});
            begin = end;
        }
        if (!blocks.empty() || !groupDemotions.empty() || !groupDeleted.empty() || !groupPending.empty()) {
            HybridPropertyDemotionPreparation local(request.memory);
            local.pages_ = std::move(lifecycle->pages_);
            const auto image = std::ranges::find_if(local.pages_, [&](const auto &page) { return page.pageId() == work.reference.page_id; });
            local.group_ = image == local.pages_.end() ? work.source : image->readGroup(work.reference,
                work.source.owner_kind, work.source.group_id, batch.generation());
            local = HybridPropertyDemotionPreparation::prepareColdBlocksImpl(batch, context, blocks, request.memory,
                {}, &sourceValues, request.cancellation, largeFields, groupDemotions, groupDeleted, groupPending, std::move(local));
            lifecycle->pages_ = std::move(local.pages_);
            auto &upserts = lifecycle->groupsFor(work.source.owner_kind);
            upserts.insert(upserts.end(), local.groups_.begin(), local.groups_.end());
            auto &erased = lifecycle->erasedGroupsFor(work.source.owner_kind);
            erased.insert(erased.end(), local.erased_groups_.begin(), local.erased_groups_.end());
            if (lifecycle->group_.owner_kind == work.source.owner_kind && lifecycle->group_.group_id == work.source.group_id) {
                lifecycle->group_ = local.group_; lifecycle->group_erased_ = local.group_erased_;
            }
        }
        }
        for (const auto kind : {EntityPropertyOwnerKind::EPOK_NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
            auto &upserts = lifecycle->groupsFor(kind); auto &erased = lifecycle->erasedGroupsFor(kind);
            std::ranges::sort(upserts, {}, &HybridPropertyGroupDirectoryMap::Entry::key); std::ranges::sort(erased);
        }
    } else if (onlyCompatibleColumns) {
        // This existing preparer authenticates all source groups/columns and
        // clones each shared page once. Derived keys below still cover the
        // complete original replacement span in this single metadata batch.
        compatible.emplace(source, fields, changes, request.memory, request.cancellation);
    } else if (!request.owner_deletions.empty()) {
        lifecycle.emplace(HybridPropertyDemotionPreparation::prepareOwnerDeletion(batch, context, source, fields,
            request.owner_deletions, request.memory, request.cancellation, changes, largeResolver, request.pending_owner_additions));
    } else if (!request.pending_owner_additions.empty()) {
        if (!coldPlansMerged) {
        std::sort(changes.begin(), changes.end(), [](auto *a, auto *b) { return a->columnReference() < b->columnReference(); });
        merged.reserve(changes.size());
        for (size_t begin = 0; begin < changes.size();) {
            size_t end = begin + 1;
            while (end < changes.size() && changes[end]->columnReference() == changes[begin]->columnReference()) ++end;
            merged.push_back(HybridPropertyColumnDelta::mergeExistingColumnChanges(
                std::span(changes).subspan(begin, end - begin), request.memory,
                RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, request.cancellation));
            begin = end;
        }
        changes.clear(); for (const auto &delta : merged) changes.push_back(&delta);
        }
        lifecycle.emplace(HybridPropertyDemotionPreparation::preparePendingOwners(batch, context, source, fields,
            changes, request.pending_owner_additions, request.memory, request.cancellation, largeResolver));
    } else if (request.field_lifecycle) {
        if (!coldPlansMerged) {
        std::sort(changes.begin(), changes.end(), [](auto *a, auto *b) { return a->columnReference() < b->columnReference(); });
        merged.reserve(changes.size());
        for (size_t begin = 0; begin < changes.size();) {
            size_t end = begin + 1;
            while (end < changes.size() && changes[end]->columnReference() == changes[begin]->columnReference()) ++end;
            merged.push_back(HybridPropertyColumnDelta::mergeExistingColumnChanges(
                std::span(changes).subspan(begin, end - begin), request.memory,
                RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, request.cancellation));
            begin = end;
        }
        changes.clear(); for (const auto &delta : merged) changes.push_back(&delta);
        }
        if (!changes.empty()) {
            if (multipleGroups) lifecycle.emplace(HybridPropertyDemotionPreparation::prepareGroups(
                batch, source, fields, changes, request.memory, request.cancellation, largeResolver));
            else lifecycle.emplace(batch, source, fields, changes, request.memory, request.cancellation, largeResolver);
        }
        else {
            lifecycle.emplace(HybridPropertyDemotionPreparation(request.memory));
            lifecycle->group_ = *sourceGroup;
        }
    } else compatible.emplace(source, fields, changes, request.memory, request.cancellation);
    if (!inlineChanges.empty()) {
            require(bool(lifecycle), "native extensions require lifecycle preparation");
            auto &images = lifecycle->pages_;
            const auto image = [&](PageId id) -> HybridPropertyPageImage & {
                for (auto &page : images) if (page.pageId() == id) return page;
                images.push_back(fields.cloneForMutation(source, id, batch.generation(), request.memory));
                return images.back();
            };
            for (const auto &change : inlineChanges) {
                check(); const auto &replacement = *change.replacement;
                SlottedSlotReference ownerGroupReference;
                const auto ownerGroup = context.groupRecord(replacement.owner, std::ref(admit), &ownerGroupReference);
                boundary.set(0); require(bool(ownerGroup), "native extension lost its authenticated source group");
                auto finalGroup = image(ownerGroupReference.page_id).readGroup(ownerGroupReference, ownerGroup->owner_kind,
                    ownerGroup->group_id, batch.generation());
                uint64_t expectedLive = ownerGroup->live;
                for (const auto owner : request.pending_owner_additions)
                    if (owner.owner_kind == ownerGroup->owner_kind && hybridPropertyGroupFor(owner) == ownerGroup->group_id)
                        expectedLive |= UINT64_C(1) << hybridPropertySlotFor(owner);
                for (const auto owner : request.owner_deletions)
                    if (owner.owner_kind == ownerGroup->owner_kind && hybridPropertyGroupFor(owner) == ownerGroup->group_id)
                        expectedLive &= ~(UINT64_C(1) << hybridPropertySlotFor(owner));
                require(finalGroup.live == expectedLive && (finalGroup.semantic_generation == ownerGroup->semantic_generation ||
                    finalGroup.semantic_generation == batch.generation()), "native extension found an inconsistent private group");
                const int64_t memberships = int64_t(finalGroup.memberships) + (change.insert ? 1 : replacement.erase ? -1 : 0);
                require(memberships >= 0 && memberships <= UINT32_MAX, "native extension mutation overflows memberships");
                finalGroup.memberships = uint32_t(memberships); finalGroup.semantic_generation = batch.generation(); finalGroup.validate();
                image(ownerGroupReference.page_id).replaceGroup(ownerGroupReference, finalGroup, batch.generation());
                if (finalGroup.owner_kind == lifecycle->group_.owner_kind && finalGroup.group_id == lifecycle->group_.group_id)
                    lifecycle->group_ = finalGroup;
                const auto key = HybridPropertyDirectoryCodec::ownerKey(replacement.owner, replacement.token);
                if (!change.insert) {
                    auto &oldImage = image(change.reference.page_id);
                    // Keep the record bytes alive while the authenticated view borrows them.
                    const auto bytes = oldImage.readRecord(change.reference);
                    static_cast<void>(HybridPropertyExtensionView::open(bytes, replacement.owner, replacement.token,
                        ownerGroup->semantic_generation, descriptor.semantic_generation, current.generation, std::ref(admitReference)));
                    referenceCredit.set(0);
                    if (replacement.erase) oldImage.erase(change.reference);
                } else lifecycle->ownersFor(replacement.owner.owner_kind).push_back({key, HybridPropertyDirectoryCodec::sourceValue(HybridPropertySource::HPS_EXTENSION)});
                if (replacement.erase) {
                    lifecycle->erasedOwnersFor(replacement.owner.owner_kind).push_back(key); lifecycle->erasedExtensionsFor(replacement.owner.owner_kind).push_back(key); continue;
                }
                PropertyValue scalar;
                const auto &value = replacement.propertyValue(scalar);
                const auto size = CanonicalPropertyValueCodec::encodedSize(value);
                const auto encoded = size > RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES - RSS_HYBRID_EXTENSION_HEADER_BYTES ?
                    resolveLarge(replacement.owner, replacement.token, value) :
                    HybridPropertyExtensionView::encodeInline(replacement.owner, replacement.token,
                        batch.generation(), value, RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, *request.memory);
                if (!change.insert) {
                    auto &oldImage = image(change.reference.page_id);
                    if (encoded.size() <= oldImage.readRecord(change.reference).size()) {
                        oldImage.replaceExtension(change.reference, encoded, replacement.owner, replacement.token,
                            finalGroup, batch.generation(), std::ref(admitReference));
                        referenceCredit.set(0);
                        continue;
                    }
                    oldImage.erase(change.reference);
                }
                std::optional<SlottedSlotReference> destination;
                for (auto &page : images) {
                    check(); const auto stats = page.stats();
                    const auto needed = encoded.size() + (stats.reusable_free_slot_count ? 0U : RSS_SLOTTED_PAGE_SLOT_BYTES);
                    if (stats.contiguous_free_bytes + stats.reclaimable_bytes < needed) continue;
                    if (stats.contiguous_free_bytes < needed) page.compact();
                    destination = page.appendExtension(encoded, replacement.owner, replacement.token, finalGroup, batch.generation(), std::ref(admitReference));
                    referenceCredit.set(0); break;
                }
                if (!destination) {
                    images.push_back(HybridPropertyPageImage::create(batch.allocateLogicalPageId(), Generation(1), batch.generation(), request.memory));
                    destination = images.back().appendExtension(encoded, replacement.owner, replacement.token, finalGroup, batch.generation(), std::ref(admitReference));
                    referenceCredit.set(0);
                }
                lifecycle->extensionsFor(replacement.owner.owner_kind).push_back({key, HybridPropertyDirectoryCodec::locatorValue(*destination)});
            }
            const auto entryLess = [](const auto &a, const auto &b) { return a.key < b.key; };
            for (const auto kind : {EntityPropertyOwnerKind::EPOK_NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
                auto &owners = lifecycle->ownersFor(kind); auto &extensions = lifecycle->extensionsFor(kind);
                auto &erasedOwners = lifecycle->erasedOwnersFor(kind); auto &erasedExtensions = lifecycle->erasedExtensionsFor(kind);
                std::sort(owners.begin(), owners.end(), entryLess); std::sort(extensions.begin(), extensions.end(), entryLess);
                std::sort(erasedOwners.begin(), erasedOwners.end()); std::sort(erasedExtensions.begin(), erasedExtensions.end());
            }
    }
    auto &fieldImages = lifecycle ? lifecycle->pages_ : compatible->pages_;
    if (fieldImages.empty() && jointValues.empty()) { check(); return false; }
    utils::OwnedVector<BTreePage> indexImages{utils::OwnedPmrAllocator<BTreePage>(request.memory)};
    utils::OwnedVector<uint64_t> erasedPages{utils::OwnedPmrAllocator<uint64_t>(request.memory)};
    utils::OwnedVector<IndexRootLocation> changedRoots{utils::OwnedPmrAllocator<IndexRootLocation>(request.memory)};
    utils::OwnedVector<uint64_t> erasedRoots{utils::OwnedPmrAllocator<uint64_t>(request.memory)};
    if (lifecycle) for (const auto kind : {EntityPropertyOwnerKind::EPOK_NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
        const bool erasedAnyGroup = !lifecycle->erasedGroups(kind).empty();
        if (lifecycle->owners(kind).empty() && lifecycle->erasedOwners(kind).empty() && lifecycle->extensions(kind).empty() &&
            lifecycle->erasedColumns(kind).empty() && lifecycle->erasedExtensions(kind).empty() &&
            lifecycle->groups(kind).empty() && !erasedAnyGroup &&
            !(kind == EntityPropertyOwnerKind::EPOK_EDGE && !erasedAdjacency.empty())) continue;
        const size_t base = kind == EntityPropertyOwnerKind::EPOK_NODE ? 0U : 4U;
        const auto prepareDirectory = [&]<class Map>(size_t role, std::span<const typename Map::Entry> upserts,
            std::span<const typename Map::Key> erases, bool adjacency = false) {
            const auto globalRoot = adjacency ? locations_.findIndexRootOptional(current, RSS_GLOBAL_ADJACENCY_INDEX_ID) : std::nullopt;
            const HybridPropertyTreeRoot sourceRoot = adjacency ? (globalRoot ? HybridPropertyTreeRoot{globalRoot->root_page_id, globalRoot->root_generation} : HybridPropertyTreeRoot{}) : descriptor.roots[base + role];
            if (upserts.empty() && erases.empty()) return;
            check(); const auto ordinal = static_cast<HybridPropertyRootOrdinal>(base + role);
            const auto index = adjacency ? RSS_GLOBAL_ADJACENCY_INDEX_ID : hybridPropertyIndexId(ordinal);
            const auto binding = [&] {
                if (adjacency) { require(bool(globalRoot), "native Edge deletion lost the global adjacency namespace"); return BTreeRootReadBinding{current.database_id, current.generation, *globalRoot}; }
                return HybridPropertyDirectoryCodec::bindRoot(descriptor, source, ordinal);
            };
            if (erasedAnyGroup && !adjacency && sourceRoot.page_id.isValid() && upserts.empty()) {
                Map map(index, sourceTrees, *this, binding()); utils::OwnedHeapReservation censusBoundary(request.memory);
                size_t position = 0; bool exclusive = true;
                const bool exhausted = map.visitAll(source, erases.size() + 1U, [&](const auto &entry) {
                    check();
                    if (position == erases.size() || entry.key != erases[position]) { exclusive = false; return false; }
                    ++position; return true;
                }, [&](size_t bytes) { censusBoundary.set(bytes); }, request.cancellation);
                censusBoundary.set(0);
                if (exclusive && exhausted && position == erases.size()) {
                    utils::OwnedHeapReservation frontier(request.memory, 256U * 1024U);
                    const auto plan = sourceTrees.planTreeRetirement(source, index, 128U, request.cancellation);
                    for (const auto &pool : plan.pool_requests)
                        for (const auto &page : pool.expected_retirements) erasedPages.push_back(page.location.page_id.value());
                    descriptor.roots[base + role] = {}; erasedRoots.push_back(index); return;
                }
            }
            const auto appendId = [&](IndexTreeMutationPoolBatch &assignment) {
                const auto id = batch.allocateLogicalPageId(); assignment.appended_page_ids.push_back(id);
                assignment.appended_locations.push_back({id, {}, 0, Generation(1), 0});
            };
            const auto unplaced = [&] {
                IndexTreeMutationPoolBatch assignment; assignment.newly_reserved = true;
                assignment.pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL; assignment.pool.owner_id = index;
                assignment.pool.generation = Generation(1); assignment.pool.state = SegmentState::SGS_SEALED;
                assignment.pool.committed_lsn = batch.checkpointLsn(); assignment.replacement_pool = assignment.pool;
                return assignment;
            };
            if (!sourceRoot.page_id.isValid()) {
                require(!adjacency && erases.empty(), "native map erasure has no source namespace");
                OrderedBTreeEntrySource entries = [upserts]() -> OrderedBTreeEntryCursor {
                    return [upserts, position = size_t(0)]() mutable -> std::optional<BTreePageEntry> {
                        if (position == upserts.size()) return {};
                        const auto &entry = upserts[position++]; const auto key = Map::encode(entry.key, entry.value);
                        return BTreePageEntry{utils::OwnedStdVector<std::byte>(key.begin(), key.end()), 0};
                    };
                };
                const BulkBTreeBuildOptions build{request.first_slot, request.last_slot, index, upserts.size(), upserts.size(), BTreeLeafValueLayout::BLVL_EMPTY};
                auto plan = BulkBTreeBuilder::planNativeTreeWithImageSink(sourceTrees, *this, source, index, entries, build,
                    [&](const CatalogEntry &, std::span<BTreePage> images, std::span<PageLocation>, LogSequenceNumber) {
                        for (auto &image : images) { check(); indexImages.push_back(std::move(image)); }
                    });
                utils::OwnedHeapReservation assignmentsCredit(request.memory, 16384U + plan.new_pool_mutations.size() * 8192U);
                std::vector<IndexTreeMutationPoolBatch> assignments; assignments.reserve(plan.new_pool_mutations.size());
                for (const auto &pool : plan.new_pool_mutations) {
                    auto assignment = unplaced(); assignment.appended_page_ids.reserve(pool.appended_page_count);
                    assignment.appended_locations.reserve(pool.appended_page_count);
                    for (size_t i = 0; i < pool.appended_page_count; ++i) appendId(assignment);
                    assignments.push_back(std::move(assignment));
                }
                plan.preparer(assignments, batch.checkpointLsn());
                const auto root = assignments.at(plan.root_page.pool_ordinal).appended_page_ids.at(plan.root_page.appended_page_ordinal);
                descriptor.roots[base + role] = {root, Generation(1)}; changedRoots.push_back({index, root, Generation(1)});
                return;
            }
            Map map(index, sourceTrees, *this, binding());
            const size_t maximumOperations = upserts.size() * 2U + erases.size();
            utils::OwnedHeapReservation operationsCredit(request.memory, 16384U + maximumOperations *
                (sizeof(BTreeScalarIndexOperation) + 64U + 2U * utils::detail::standardContainerProxyBytes()));
            std::vector<BTreeScalarIndexOperation> operations; operations.reserve(maximumOperations);
            const auto operation = [&](BTreeScalarIndexOperationKind action, const typename Map::Key &key, const typename Map::Value &value) {
                const auto record = Map::encode(key, value);
                operations.push_back({action, std::vector<std::byte>(record.begin(), record.end()), 0});
            };
            utils::OwnedHeapReservation boundary(request.memory);
            const auto admit = [&](size_t bytes) { boundary.set(bytes); };
            for (const auto &key : erases) {
                check(); const auto old = map.find(source, key, std::ref(admit), request.cancellation); boundary.set(0);
                require(bool(old), "native map erasure has no exact source prefix");
                operation(BTreeScalarIndexOperationKind::BSIOP_ERASE, key, *old);
            }
            for (const auto &entry : upserts) {
                check(); const auto old = map.find(source, entry.key, std::ref(admit), request.cancellation); boundary.set(0);
                if (old == std::optional(entry.value)) continue;
                if (old) operation(BTreeScalarIndexOperationKind::BSIOP_ERASE, entry.key, *old);
                operation(BTreeScalarIndexOperationKind::BSIOP_UPSERT, entry.key, entry.value);
            }
            if (operations.empty()) return;
            const auto start = indexImages.size();
            auto plan = sourceTrees.planHybridMapImages(source, index, operations, request.memory,
                [&](BTreePage image) { check(); indexImages.push_back(std::move(image)); }, request.cancellation);
            utils::OwnedHeapReservation assignmentsCredit(request.memory, 16384U +
                (plan.pool_requests.size() + plan.additional_new_pool_mutations.size() + size_t(bool(plan.new_pool_mutation))) * 8192U);
            std::vector<IndexTreeMutationPoolBatch> assignments;
            assignments.reserve(plan.pool_requests.size() + plan.additional_new_pool_mutations.size() + size_t(bool(plan.new_pool_mutation)));
            for (const auto &pool : plan.pool_requests) {
                auto assignment = IndexTreeMutationPoolBatch{}; assignment.pool = catalog_.find(current, pool.segment_id);
                assignment.replacement_pool = assignment.pool; assignment.expected_replacements = pool.expected_replacements;
                assignment.expected_retirements = pool.expected_retirements;
                require(pool.appended_page_count == 0 && !pool.replacement_segment_id.isValid(), "native private map plan selected physical source capacity");
                for (const auto &retirement : pool.expected_retirements) erasedPages.push_back(retirement.location.page_id.value());
                assignments.push_back(std::move(assignment));
            }
            const auto appendPool = [&](const BTreeNewIndexTreePoolMutation &pool) {
                auto assignment = unplaced(); assignment.appended_page_ids.reserve(pool.appended_page_count);
                assignment.appended_locations.reserve(pool.appended_page_count);
                for (size_t i = 0; i < pool.appended_page_count; ++i) appendId(assignment);
                assignments.push_back(std::move(assignment));
            };
            if (plan.new_pool_mutation) appendPool(*plan.new_pool_mutation);
            for (const auto &pool : plan.additional_new_pool_mutations) appendPool(pool);
            plan.preparer(assignments, batch.checkpointLsn());
            if (plan.final_tree_empty) {
                for (const auto &pool : plan.pool_requests)
                    for (const auto &location : pool.expected_replacements) erasedPages.push_back(location.page_id.value());
                indexImages.erase(indexImages.begin() + start, indexImages.end());
                if (!adjacency) descriptor.roots[base + role] = {}; erasedRoots.push_back(index); return;
            }
            PageId root = plan.stable_replacement_root.value_or(plan.expected_root.root_page_id);
            if (plan.replacement_root) root = assignments.at(plan.replacement_root->pool_ordinal).appended_page_ids.at(plan.replacement_root->appended_page_ordinal);
            Generation generation = sourceRoot.generation;
            for (size_t i = start; i < indexImages.size(); ++i)
                if (indexImages[i].header().page_id == root) generation = Generation(1);
            if (!adjacency) descriptor.roots[base + role] = {root, generation}; changedRoots.push_back({index, root, generation});
        };
        prepareDirectory.template operator()<HybridPropertyFieldDirectoryMap>(1, {}, lifecycle->erasedColumns(kind));
        prepareDirectory.template operator()<HybridPropertyOwnerMap>(0, lifecycle->owners(kind), lifecycle->erasedOwners(kind));
        prepareDirectory.template operator()<HybridPropertyFieldDirectoryMap>(2, lifecycle->extensions(kind), lifecycle->erasedExtensions(kind));
        prepareDirectory.template operator()<HybridPropertyGroupDirectoryMap>(3, lifecycle->groups(kind), lifecycle->erasedGroups(kind));
        if (kind == EntityPropertyOwnerKind::EPOK_EDGE && !erasedAdjacency.empty()) prepareDirectory.template operator()<AdjacencySet>(0, {}, erasedAdjacency, true);
    }
    // Prepare derived scalar keys from this fenced source and final cells.
    // Old selected values live for one owner only; unchanged/new values are
    // borrowed, so large owning payloads are not cloned into a second root.
    // The sealed logical graph writer already prepares all new-owner derived
    // transitions. Re-reading an absent source Entity here would be invalid;
    // this branch must not prepare competing scalar/constraint/count roots.
    if (!hybridTopology && request.pending_owner_additions.empty() && (!entityImages.empty() || (hasCatalog && !request.owner_deletions.empty()) || !constraintDefinitions.empty() || std::ranges::any_of(schemaDefinitions, [](const auto &definition) {
            return definition.kind == IndexDefinitionKind::IDK_PROPERTY || definition.kind == IndexDefinitionKind::IDK_COMPOSITE ||
                definition.kind == IndexDefinitionKind::IDK_VECTOR;
        }))) {
        SystemCatalog schema(sourceTrees, *this);
        for (const auto &definition : schemaDefinitions)
            if (definition.kind == IndexDefinitionKind::IDK_PROPERTY || definition.kind == IndexDefinitionKind::IDK_COMPOSITE ||
                (!request.owner_deletions.empty() && definition.kind == IndexDefinitionKind::IDK_LABEL))
                require(bool(findIndexRootOptional(source, definition.index_id)), "native field schema has a scalar definition without an active root");
        // Vector mappings and manifests form one publication even for an
        // unrelated field write. Never preserve a partially published index.
        for (const auto &definition : schemaDefinitions) if (definition.kind == IndexDefinitionKind::IDK_VECTOR) {
            require(definition.vector_dimensions <= RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS,
                "native field publication requires bounded Vector dimensions");
            require(bool(findIndexRootOptional(source, definition.index_id)) ==
                bool(findVectorManifestOptional(source, definition.index_id)),
                "native field publication found an incomplete Vector mapping/Manifest pair");
        }
        NativeScalarIndexMutationPlanner scalar(*this, sourceTrees, std::move(schemaDefinitions),
            [&](const MetadataSnapshotPin &pin, std::string_view name) { return schema.findPropertyToken(pin, name); });
        struct ScalarChange {
            uint64_t index, owner; bool erase;
            utils::OwnedVector<std::byte> key;
        };
        utils::OwnedVector<ScalarChange> operations{utils::OwnedPmrAllocator<ScalarChange>(request.memory)};
        utils::OwnedVector<EntityPropertyKey> owners{utils::OwnedPmrAllocator<EntityPropertyKey>(request.memory)};
        for (const auto &cell : replacements)
            if (std::find(owners.begin(), owners.end(), cell.owner) == owners.end()) owners.push_back(cell.owner);
        for (const auto owner : request.owner_deletions) owners.push_back(owner);
        const auto deleted = [&](EntityPropertyKey owner) {
            return std::ranges::find(request.owner_deletions, owner) != request.owner_deletions.end();
        };
        utils::OwnedMap<std::pair<SystemCatalogSchemaTokenKind, uint64_t>, uint64_t> tokenDecrements;
        PropertyCollectionRootStore roots(sourcePages, sourceValues, *this);
        const std::function<void()> decodeCheck = check;
        for (const auto owner : owners) {
            check(); detail::PropertyDecodeWorkspace decoded(request.memory, &decodeCheck);
            utils::OwnedMap<uint32_t, std::optional<PropertyCollectionRootEntry>> oldValues;
            std::array<PropertyValue, RSS_HYBRID_GROUP_CAPACITY> scratch;
            utils::OwnedHeapReservation labelsCredit(request.memory);
            std::vector<uint64_t> labels; uint64_t edgeType = 0;
            if (owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE) {
                const auto record = sourceEntities.readNode(source, owner.entity_id);
                labels = sourceLabels.readNodeLabels(source, record, [&](size_t bytes) { labelsCredit.set(bytes); });
            } else {
                const auto record = sourceEntities.readEdge(source, owner.entity_id);
                edgeType = record.type_id;
            }
            const auto oldValue = [&](uint32_t token) -> const PropertyValue * {
                check(); auto found = oldValues.find(token);
                if (found == oldValues.end()) {
                    // Derived indexes and constraints must read the same
                    // authenticated Native source as field preparation. A ROW
                    // owner in a joint publication retains its original route.
                    auto selected = HybridPropertyValueReader::read(context, source, owner, token, &sourceValues);
                    auto previous = selected.route == HybridPropertyReadContext::FieldRoute::HFR_ROW ?
                        roots.readEntityPropertyOptional(source, owner, token) : std::move(selected.value);
                    found = oldValues.emplace(token, std::move(previous)).first;
                }
                return found->second ? &found->second->value : nullptr;
            };
            const auto newValue = [&](uint32_t token) -> const PropertyValue * {
                check(); if (deleted(owner)) return nullptr;
                for (size_t i = 0; i < replacements.size(); ++i) if (replacements[i].owner == owner && replacements[i].token == token)
                    return replacements[i].erase ? nullptr : &replacements[i].propertyValue(scratch[i]);
                return oldValue(token);
            };
            // Property-only writes keep labels fixed. Prove that every scoped
            // Vector field is unchanged before preserving its mapping/manifest.
            // A real embedding delta still requires the atomic Vector writer.
            for (const auto &definition : scalar.definitions()) {
                check(); if (definition.kind != IndexDefinitionKind::IDK_VECTOR) continue;
                const auto token = schema.findPropertyToken(source, definition.property_names.front());
                require(bool(token), "native Vector definition has no indexed Property-key Token");
                if (owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE && deleted(owner) &&
                    findIndexRootOptional(source, definition.index_id)) {
                    // Membership can be stale even when the authoritative field
                    // or label is absent. Do not orphan a persisted mapping by
                    // treating the missing property as an empty Vector index.
                    utils::OwnedHeapReservation keyCredit(request.memory, 1024U);
                    const auto mapped = BTreeIndex(definition.index_id, sourceTrees, *this).find(source,
                        VectorSemanticCodec::nodeMappingKey(owner.entity_id));
                    const bool removing = hybridVectors && std::ranges::any_of(hybridVectors->semantics_, [&](const auto &v) {
                        return v.index_id == definition.index_id && v.node_id == owner.entity_id && v.kind == NativeVectorMutationKind::NVMK_ERASE;
                    });
                    require(!mapped || removing, "native owner deletion requires coordinated removal of its Vector mapping");
                }
                if (owner.owner_kind != EntityPropertyOwnerKind::EPOK_NODE ||
                    std::ranges::find(labels, definition.label_token_id) == labels.end()) continue;
                const bool changed = deleted(owner) || std::ranges::any_of(replacements, [&](const auto &cell) {
                    return cell.owner == owner && cell.token == *token;
                });
                if (!changed) continue;
                const auto *previous = oldValue(*token);
                const auto *final = newValue(*token);
                if ((!previous && !final) || (previous && final && *previous == *final)) continue;
                // Reuse ordinary-write BFloat16/metric semantics without owning
                // Property roots. Different logical values can encode the same
                // embedding and safely preserve all Vector state.
                utils::OwnedHeapReservation vectorCredit(request.memory,
                    16384U + size_t(definition.vector_dimensions) * 4U);
                NativeVectorMutationPlanner vector({definition},
                    [&](const MetadataSnapshotPin &pin, std::string_view name) { return schema.findPropertyToken(pin, name); },
                    {1, 2});
                const auto semantics = vector.planNodeValues(source, owner.entity_id, labels, oldValue,
                    deleted(owner) ? std::span<const uint64_t>{} : std::span<const uint64_t>(labels), newValue);
                if (!hybridVectors) require(semantics.empty(), "native embedding changes require atomic Vector delta coordination");
                else {
                    size_t matched = 0;
                    for (const auto &expected : hybridVectors->semantics_) if (expected.index_id == definition.index_id && expected.node_id == owner.entity_id) {
                        require(matched < semantics.size() && expected == semantics[matched], "native Vector bytes disagree with the final authoritative fields");
                        ++matched;
                    }
                    require(matched == semantics.size(), "native Vector proof omits an authoritative field transition");
                    verifiedVectorSemantics += matched;
                }
            }
            for (const auto &definition : constraintDefinitions) {
                check(); if (deleted(owner)) continue;
                const bool node = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE;
                if ((definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE) != node ||
                    (node ? std::ranges::find(labels, definition.schema_token_id) == labels.end() :
                        edgeType != definition.schema_token_id)) continue;
                std::array<const PropertyValue *, RSS_CONSTRAINT_DEFINITION_MAX_PROPERTIES> values{};
                size_t count = 0;
                for (const auto &name : definition.property_names) {
                    check(); const auto token = schema.findPropertyToken(source, name);
                    values[count++] = token ? newValue(*token) : nullptr;
                }
                ConstraintMutationValidator::validatePropertyValues(definition, std::span(values.data(), count));
            }
            scalar.visitPropertyDeltaValues(source, owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ?
                IndexDefinitionEntityKind::IDEK_NODE : IndexDefinitionEntityKind::IDEK_EDGE,
                owner.entity_id, labels, oldValue, deleted(owner) ? std::span<const uint64_t>{} : std::span<const uint64_t>(labels), newValue,
                [&](uint64_t index, std::span<const std::byte> key, uint64_t id, bool erase) {
                    check(); require(operations.size() < 8192U, "native scalar deltas exceed their operation bound");
                    const auto previous = BTreeIndex(index, sourceTrees, *this).find(source, key);
                    require(erase ? previous == std::optional(id) : !previous,
                        "native scalar index key disagrees with its authoritative source field");
                    operations.push_back({index, id, erase,
                        utils::OwnedVector<std::byte>(key.begin(), key.end(), utils::OwnedPmrAllocator<std::byte>(request.memory))});
                });
            if (deleted(owner)) {
                const bool node = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE;
                const auto schemaKind = node ? SystemCatalogSchemaTokenKind::SCSTK_LABEL : SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
                const auto tokens = node ? std::span<const uint64_t>(labels) : std::span<const uint64_t>(&edgeType, 1);
                for (const auto token : tokens) ++tokenDecrements[{schemaKind, token}];
                require(tokenDecrements.size() <= 4096U, "native deletion exceeds its affected schema-token bound");
                for (const auto &definition : scalar.definitions()) {
                    if (definition.kind != IndexDefinitionKind::IDK_LABEL ||
                        (definition.entity_kind == IndexDefinitionEntityKind::IDEK_NODE) != node) continue;
                    for (const auto token : tokens) {
                        if (!node && !token) continue;
                        require(token <= uint64_t((std::numeric_limits<int64_t>::max)()), "schema Token exceeds its scalar index representation");
                        const auto key = IndexKeyCodec::encodeEntryKey(PropertyValue(int64_t(token)), owner.entity_id);
                        require(BTreeIndex(definition.index_id, sourceTrees, *this).find(source, key) == std::optional(owner.entity_id),
                            "native deletion found a missing label/type index entry");
                        require(operations.size() < 8192U, "native deletion exceeds its derived operation bound");
                        operations.push_back({definition.index_id, owner.entity_id, true,
                            utils::OwnedVector<std::byte>(key.begin(), key.end(), utils::OwnedPmrAllocator<std::byte>(request.memory))});
                    }
                }
            }
        }
        // Uniqueness compares complete canonical bytes after digest routing.
        // Retain final transitions for one constraint only, so swaps in the
        // same batch see final values rather than transient source conflicts.
        using CanonicalKey = std::pmr::vector<std::byte>;
        const auto canonicalFor = [&](const ConstraintDefinition &definition, uint64_t id, bool final)
            -> std::optional<CanonicalKey> {
            check(); detail::PropertyDecodeWorkspace decoded(request.memory, &decodeCheck);
            const auto kind = definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE ?
                EntityPropertyOwnerKind::EPOK_NODE : EntityPropertyOwnerKind::EPOK_EDGE;
            const EntityPropertyKey owner{kind, id};
            if (final && deleted(owner)) return std::nullopt;
            if (kind == EntityPropertyOwnerKind::EPOK_NODE) {
                utils::OwnedHeapReservation labelCredit(request.memory);
                const auto labels = sourceLabels.readNodeLabels(source, sourceEntities.readNode(source, id),
                    [&](size_t bytes) { labelCredit.set(bytes); });
                if (std::ranges::find(labels, definition.schema_token_id) == labels.end()) return std::nullopt;
            } else if (sourceEntities.readEdge(source, id).type_id != definition.schema_token_id) return std::nullopt;
            CanonicalKey key(request.memory.get());
            std::array<std::optional<PropertyCollectionRootEntry>, RSS_CONSTRAINT_DEFINITION_MAX_PROPERTIES> old;
            std::array<PropertyValue, RSS_CONSTRAINT_DEFINITION_MAX_PROPERTIES> scratch;
            for (size_t i = 0; i < definition.property_names.size(); ++i) {
                check(); const auto token = schema.findPropertyToken(source, definition.property_names[i]);
                const PropertyValue *value = nullptr; bool replacement = false;
                if (token && final) for (const auto &cell : replacements) if (cell.owner == owner && cell.token == *token) {
                    replacement = true; if (!cell.erase) value = &cell.propertyValue(scratch[i]); break;
                }
                if (token && !replacement) {
                    auto selected = HybridPropertyValueReader::read(context, source, owner, *token, &sourceValues);
                    old[i] = selected.route == HybridPropertyReadContext::FieldRoute::HFR_ROW ?
                        roots.readEntityPropertyOptional(source, owner, *token) : std::move(selected.value);
                    if (old[i]) value = &old[i]->value;
                }
                if (!value || value->getType() == PropertyType::NULL_TYPE) return std::nullopt;
                CanonicalPropertyValueCodec::append(key, *value);
            }
            return key;
        };
        for (const auto &definition : constraintDefinitions) {
            if (definition.kind != ConstraintDefinitionKind::CDK_UNIQUE && definition.kind != ConstraintDefinitionKind::CDK_NODE_KEY) continue;
            check(); const auto index = definition.constraint_id;
            require(bool(findIndexRootOptional(source, index)), "native uniqueness constraint has no active support root");
            struct Transition { uint64_t owner; std::optional<CanonicalKey> old_key, new_key; };
            utils::OwnedVector<Transition> transitions{utils::OwnedPmrAllocator<Transition>(request.memory)};
            for (const auto &owner : owners) {
                if ((owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE) !=
                    (definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE)) continue;
                transitions.push_back({owner.entity_id, canonicalFor(definition, owner.entity_id, false),
                    canonicalFor(definition, owner.entity_id, true)});
            }
            utils::OwnedHeapReservation supportFrontier(request.memory, 16U * 1024U), keyCredit(request.memory, 1024U), supportBoundary(request.memory);
            BTreeIndex support(index, sourceTrees, *this);
            for (const auto &transition : transitions) {
                check();
                if (transition.old_key) {
                    const auto key = ConstraintSupportTree::entryKey(*transition.old_key, transition.owner);
                    require(support.find(source, key) == std::optional(transition.owner), "native constraint source support entry is missing");
                }
                if (!transition.new_key || transition.old_key == transition.new_key) continue;
                for (const auto &other : transitions) if (other.owner != transition.owner && other.new_key == transition.new_key)
                    throw std::runtime_error("Constraint violation: '" + definition.name + "' has a duplicate key");
                const auto lower = ConstraintSupportTree::lowerBound(*transition.new_key);
                auto cursor = support.openRangeCursor(source, std::span<const std::byte>(lower), 65U,
                    BufferPoolReadHint::BPRH_SEQUENTIAL, false, request.cancellation);
                size_t collisions = 0;
                while (auto candidate = cursor.next([&](size_t bytes) { supportBoundary.set(bytes); })) {
                    check(); if (!ConstraintSupportTree::matchesDigest(candidate->key, *transition.new_key)) break;
                    require(++collisions <= 64U, "native constraint support collision bound exceeded");
                    const auto id = ConstraintSupportTree::entryEntityId(candidate->key);
                    require(candidate->value_or_child == id, "native support entry has a mismatched owner");
                    const auto sourceKey = canonicalFor(definition, id, false);
                    require(sourceKey && ConstraintSupportTree::entryKey(*sourceKey, id) == candidate->key,
                        "native constraint support entry is stale");
                    const auto changed = std::ranges::find(transitions, id, &Transition::owner);
                    const auto &finalKey = changed == transitions.end() ? sourceKey : changed->new_key;
                    if (id != transition.owner && finalKey == transition.new_key)
                        throw std::runtime_error("Constraint violation: '" + definition.name + "' has a duplicate key");
                }
            }
            for (const auto &transition : transitions) {
                if (transition.old_key == transition.new_key) continue;
                const auto oldKey = transition.old_key ? ConstraintSupportTree::entryKey(*transition.old_key, transition.owner) : std::vector<std::byte>{};
                const auto newKey = transition.new_key ? ConstraintSupportTree::entryKey(*transition.new_key, transition.owner) : std::vector<std::byte>{};
                if (oldKey == newKey) continue; // A digest collision changes only the authoritative field.
                if (!newKey.empty()) require(!support.find(source, newKey), "native constraint has a ghost target support entry");
                for (bool erase : {true, false}) {
                    const auto &key = erase ? oldKey : newKey; if (key.empty()) continue;
                    require(operations.size() < 8192U, "native derived deltas exceed their operation bound");
                    operations.push_back({index, transition.owner, erase,
                        utils::OwnedVector<std::byte>(key.begin(), key.end(), utils::OwnedPmrAllocator<std::byte>(request.memory))});
                }
            }
        }
        if (!request.owner_deletions.empty()) {
            const auto appendCount = [&](const auto &key, uint64_t count) {
                require(operations.size() < 8192U, "native deletion exceeds its combined derived operation bound");
                operations.push_back({RSS_SYSTEM_CATALOG_INDEX_ID, SystemCatalogGraphCardinalityCodec::encodeCount(count), false,
                    utils::OwnedVector<std::byte>(key.begin(), key.end(), utils::OwnedPmrAllocator<std::byte>(request.memory))});
            };
            if (const auto statistics = schema.readGraphCardinalityStatistics(source)) {
                require(statistics->node_count >= erasedNodes.size() && statistics->edge_count >= erasedEdges.size(),
                    "native deletion would underflow graph cardinality");
                if (!erasedNodes.empty()) appendCount(SystemCatalogGraphCardinalityCodec::nodeCountKeyOwned(), statistics->node_count - erasedNodes.size());
                if (!erasedEdges.empty()) appendCount(SystemCatalogGraphCardinalityCodec::edgeCountKeyOwned(), statistics->edge_count - erasedEdges.size());
            }
            for (const auto &[token, decrement] : tokenDecrements) {
                check(); if (const auto count = schema.readTokenCardinality(source, token.first, token.second)) {
                    require(*count >= decrement, "native deletion would underflow token cardinality");
                    appendCount(SystemCatalogTokenCardinalityCodec::countKeyOwned(token.first, token.second), *count - decrement);
                }
            }
        }
        // This coordinator prepares Entity deletions outside the ordinary graph
        // page planner. Keep its existing derived free-space summaries coherent
        // from the authenticated source/final page images, in the same commit.
        for(const auto kind : {EntityPageRecordKind::EPRK_NODE,EntityPageRecordKind::EPRK_EDGE}) {
            if(std::ranges::none_of(entityImages,[&](const auto &image){return image.page.header().record_kind==kind;}))continue;
            const auto proof=EntityFreeSpaceIndex::readProof(*this,sourceTrees,source,kind);
            if(!proof)continue; // old files retain explicit rebuild/fallback semantics
            const auto index=EntityFreeSpaceIndex::indexId(kind);
            BTreeIndex summary(index,sourceTrees,*this);
            uint64_t count=proof->free_pages;
            const auto appendSummary=[&](const auto &key,bool erase,uint64_t value=1U){
                check();require(operations.size()<8192U,"native free-space deltas exceed their operation bound");
                operations.push_back({index,value,erase,utils::OwnedVector<std::byte>(key.begin(),key.end(),
                    utils::OwnedPmrAllocator<std::byte>(request.memory))});
            };
            for(const auto &image:entityImages) {
                const auto &header=image.page.header();if(header.record_kind!=kind)continue;
                if(image.source_free) {
                    const auto key=EntityFreeSpaceIndex::entryKey(header.page_id,image.source_free,kind);
                    require(count && summary.find(source,key)==std::optional<uint64_t>(1U),
                        "native deletion free-space summary disagrees with its source page");
                    appendSummary(key,true);--count;
                }
                const auto free=uint16_t(header.slot_capacity-header.live_count);
                if(free && header.live_count) {
                    require(count<UINT64_MAX-1U,"native free-space count marker overflow");
                    appendSummary(EntityFreeSpaceIndex::entryKey(header.page_id,free,kind),false);++count;
                }
            }
            // An already stale proof stays stale; no unrelated branch census or
            // hidden rebuild may be inferred from validating the changed pages.
            const auto generation=proof->fresh?batch.generation().value():proof->directory_generation;
            require(generation<UINT64_MAX,"native free-space generation marker overflow");
            appendSummary(EntityFreeSpaceIndex::generationKey(),false,generation+1U);
            appendSummary(EntityFreeSpaceIndex::countKey(),false,count+1U);
        }
        std::sort(operations.begin(), operations.end(), [](const auto &a, const auto &b) {
            if (a.index != b.index) return a.index < b.index;
            return a.erase != b.erase ? a.erase > b.erase : a.owner < b.owner;
        });
        const auto start = indexImages.size(); size_t constraintImages = 0, catalogImages = 0, freeSummaryImages = 0;
        for (size_t first = 0; first < operations.size();) {
            size_t last = first + 1; while (last < operations.size() && operations[last].index == operations[first].index) ++last;
            const auto index = operations[first].index; const auto groupStart = indexImages.size();
            const auto definition = std::ranges::find(scalar.definitions(), index, &IndexDefinition::index_id);
            const auto constraint = std::ranges::find(constraintDefinitions, index, &ConstraintDefinition::constraint_id);
            require(index == RSS_SYSTEM_CATALOG_INDEX_ID || isEntityFreeSpaceIndexId(index) || definition != scalar.definitions().end() || constraint != constraintDefinitions.end(),
                "native derived preparation lost its authenticated definition");
            utils::OwnedHeapReservation wireCredit(request.memory, 16384U);
            const std::function<void(const std::function<void(const BTreeScalarIndexOperation &)> &)> operationSource =
                [&](const std::function<void(const BTreeScalarIndexOperation &)> &emit) {
                    for (size_t i = first; i < last; ++i) {
                        check(); const auto &change = operations[i];
                        emit({change.erase ? BTreeScalarIndexOperationKind::BSIOP_ERASE : BTreeScalarIndexOperationKind::BSIOP_UPSERT,
                            std::vector<std::byte>(change.key.begin(), change.key.end()), change.erase ? 0U : change.owner});
                    }
                };
            const std::function<void(BTreePage)> imageSink = [&](BTreePage page) { check(); indexImages.push_back(std::move(page)); };
            auto plan = (index == RSS_SYSTEM_CATALOG_INDEX_ID || isEntityFreeSpaceIndexId(index)) ?
                sourceTrees.planNativeDerivedIndexImages(source, index, last - first, operationSource, request.memory, imageSink, request.cancellation) :
                definition != scalar.definitions().end() ?
                sourceTrees.planNativeScalarIndexImages(source, *definition, last - first, operationSource, request.memory, imageSink, request.cancellation) :
                sourceTrees.planNativeConstraintImages(source, *constraint, last - first, operationSource, request.memory, imageSink, request.cancellation);
            utils::OwnedHeapReservation assignmentsCredit(request.memory, 16384U +
                (plan.pool_requests.size() + plan.additional_new_pool_mutations.size() + size_t(bool(plan.new_pool_mutation))) * 8192U);
            std::vector<IndexTreeMutationPoolBatch> assignments;
            assignments.reserve(plan.pool_requests.size() + plan.additional_new_pool_mutations.size() + size_t(bool(plan.new_pool_mutation)));
            for (const auto &pool : plan.pool_requests) {
                IndexTreeMutationPoolBatch assigned; assigned.pool = catalog_.find(current, pool.segment_id);
                assigned.replacement_pool = assigned.pool; assigned.expected_replacements = pool.expected_replacements;
                assigned.expected_retirements = pool.expected_retirements;
                require(!pool.appended_page_count && !pool.replacement_segment_id.isValid(), "native scalar preparation selected source physical capacity");
                for (const auto &retirement : pool.expected_retirements) erasedPages.push_back(retirement.location.page_id.value());
                assignments.push_back(std::move(assigned));
            }
            const auto append = [&](const BTreeNewIndexTreePoolMutation &pool) {
                IndexTreeMutationPoolBatch assigned; assigned.newly_reserved = true;
                assigned.pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL; assigned.pool.owner_id = index;
                assigned.pool.generation = Generation(1); assigned.pool.state = SegmentState::SGS_SEALED;
                assigned.pool.committed_lsn = batch.checkpointLsn(); assigned.replacement_pool = assigned.pool;
                assigned.appended_page_ids.reserve(pool.appended_page_count); assigned.appended_locations.reserve(pool.appended_page_count);
                for (size_t i = 0; i < pool.appended_page_count; ++i) {
                    const auto id = batch.allocateLogicalPageId(); assigned.appended_page_ids.push_back(id);
                    assigned.appended_locations.push_back({id, {}, 0, Generation(1), 0});
                }
                assignments.push_back(std::move(assigned));
            };
            if (plan.new_pool_mutation) append(*plan.new_pool_mutation);
            for (const auto &pool : plan.additional_new_pool_mutations) append(pool);
            plan.preparer(assignments, batch.checkpointLsn());
            auto root = plan.stable_replacement_root.value_or(plan.expected_root.root_page_id);
            if (plan.replacement_root) root = assignments.at(plan.replacement_root->pool_ordinal).appended_page_ids.at(plan.replacement_root->appended_page_ordinal);
            auto generation = plan.expected_root.root_generation;
            for (size_t i = start; i < indexImages.size(); ++i) if (indexImages[i].header().page_id == root) generation = Generation(1);
            // An active schema definition keeps its empty root after the last
            // key is removed, unlike the canonical absent hybrid directories.
            changedRoots.push_back({index, root, generation});
            if (constraint != constraintDefinitions.end()) constraintImages += indexImages.size() - groupStart;
            if (index == RSS_SYSTEM_CATALOG_INDEX_ID) catalogImages += indexImages.size() - groupStart;
            if (isEntityFreeSpaceIndexId(index)) freeSummaryImages += indexImages.size() - groupStart;
            first = last;
        }
        debug::PerfTrace::addValue("native_graph.hybrid.private_scalar_index_images", int64_t(indexImages.size() - start - constraintImages - catalogImages - freeSummaryImages));
        debug::PerfTrace::addValue("native_graph.hybrid.private_constraint_index_images", int64_t(constraintImages));
        debug::PerfTrace::addValue("native_graph.hybrid.private_catalog_index_images", int64_t(catalogImages));
        debug::PerfTrace::addValue("native_graph.hybrid.private_entity_free_summary_index_images", int64_t(freeSummaryImages));
    }
    debug::PerfTrace::addValue("native_graph.hybrid.private_map_images", int64_t(indexImages.size()));
    check();
    require(!hybridVectors || verifiedVectorSemantics == hybridVectors->semantics_.size(),
        "native Vector proof has an unrelated or unverified semantic transition");
    utils::OwnedVector<CatalogEntry> newPools{utils::OwnedPmrAllocator<CatalogEntry>(request.memory)};
    const auto addSourceCandidate = [&](PageId id, SegmentKind kind, uint64_t owner, bool native) {
        const auto route = locations_.findPageOptional(current, id);
        if (route) placement.candidate(route->segment_id, kind, owner, native);
    };
    for (const auto &image : fieldImages) addSourceCandidate(image.pageId(), SegmentKind::SGK_PROPERTY_PAGE_POOL, request.field_pool_owner_id, true);
    for (const auto &image : entityImages) addSourceCandidate(image.page.header().page_id, SegmentKind::SGK_ENTITY_PAGE_POOL, image.owner_id, false);
    for (const auto &image : labelImages) addSourceCandidate(image.page.pageId(), SegmentKind::SGK_ENTITY_PAGE_POOL, image.owner_id, false);
    const auto allocatePool = [&](SegmentKind kind, uint64_t owner, size_t count = RSS_PAGE_POOL_DATA_FRAME_COUNT, bool native = false) {
        const bool forceFresh = kind == SegmentKind::SGK_INDEX_PAGE_POOL && !locations_.findIndexRootOptional(current, owner);
        const auto pool = placement.allocate(kind, owner, count, native, forceFresh);
        if (std::none_of(newPools.begin(), newPools.end(), [&](const auto &p) { return p.segment_id == pool.segment_id; })) newPools.push_back(pool);
        return pool;
    };
    const NativeGraphHybridPropertyConversion largeConversion{sourceGroup->owner_kind, sourceGroup->group_id,
        request.memory, request.cancellation, request.first_slot, request.last_slot, request.field_pool_owner_id};
    HybridPropertyImmutablePreparation immutableImages(batch, sourceValues, largeFields, request.memory, allocatePool, check);
    HybridPropertyContentIndexPreparation content(batch, source, largeConversion, *this, sourceTrees, sourceValues, largeFields, allocatePool, check, jointValues);
    if (content.root_) changedRoots.push_back(*content.root_);
    erasedPages.insert(erasedPages.end(), content.erased_.begin(), content.erased_.end());
    debug::PerfTrace::addValue("native_graph.hybrid.private_new_field_values", int64_t(largeFields.size()));
    check();
    utils::OwnedVector<uint64_t> superseded{utils::OwnedPmrAllocator<uint64_t>(request.memory)};
    superseded.reserve(fieldImages.size() + indexImages.size() + entityImages.size() + labelImages.size() + erasedPages.size());
    for (const auto &image : fieldImages) superseded.push_back(image.pageId().value());
    for (const auto &image : indexImages) superseded.push_back(image.header().page_id.value());
    for (const auto &image : entityImages) superseded.push_back(image.page.header().page_id.value());
    for (const auto &image : labelImages) superseded.push_back(image.page.pageId().value());
    superseded.insert(superseded.end(), erasedPages.begin(), erasedPages.end());
    superseded.insert(superseded.end(), content.superseded_.begin(), content.superseded_.end());
    std::sort(superseded.begin(), superseded.end()); superseded.erase(std::unique(superseded.begin(), superseded.end()), superseded.end());
    utils::OwnedVector<CatalogEntry> oldPools{utils::OwnedPmrAllocator<CatalogEntry>(request.memory)};
    oldPools.reserve(superseded.size());
    for (const auto id : superseded) {
        const auto location = locations_.findPageOptional(current, PageId(id));
        if (location) oldPools.push_back(catalog_.find(current, location->segment_id));
    }
    std::sort(oldPools.begin(), oldPools.end(), [](const auto &a, const auto &b) { return a.segment_id < b.segment_id; });
    oldPools.erase(std::unique(oldPools.begin(), oldPools.end(), [](const auto &a, const auto &b) { return a.segment_id == b.segment_id; }), oldPools.end());
    SharedPagePoolAllocator sourcePools(io_, *this, source);
    for (const auto &image : fieldImages)
        if (!image.liveSlotCount() && locations_.findPageOptional(current, image.pageId())) erasedPages.push_back(image.pageId().value());
    fieldImages.erase(std::remove_if(fieldImages.begin(), fieldImages.end(), [](const auto &image) { return !image.liveSlotCount(); }), fieldImages.end());
    for (const auto &image : entityImages) if (!image.page.header().live_count) erasedPages.push_back(image.page.header().page_id.value());
    entityImages.erase(std::remove_if(entityImages.begin(), entityImages.end(), [](const auto &image) { return !image.page.header().live_count; }), entityImages.end());
    std::sort(entityImages.begin(), entityImages.end(), [](const auto &a, const auto &b) {
        return a.owner_id != b.owner_id ? a.owner_id < b.owner_id : a.page.header().page_id < b.page.header().page_id;
    });
    for (const auto &image : labelImages) if (!image.page.stats().live_slot_count) erasedPages.push_back(image.page.pageId().value());
    labelImages.erase(std::remove_if(labelImages.begin(), labelImages.end(), [](const auto &image) {
        return !image.page.stats().live_slot_count;
    }), labelImages.end());
    std::sort(labelImages.begin(), labelImages.end(), [](const auto &a, const auto &b) {
        return a.owner_id != b.owner_id ? a.owner_id < b.owner_id : a.page.pageId() < b.page.pageId();
    });
    utils::OwnedVector<PageLocation> published{utils::OwnedPmrAllocator<PageLocation>(request.memory)};
    published.reserve(fieldImages.size() + indexImages.size() + entityImages.size() + labelImages.size() + content.images_.size() + immutableImages.frames_.size());
    struct PlacementRun { CatalogEntry pool; size_t offset, count; };
    const auto planImages = [&]<class Images>(const Images &images, bool native) {
        utils::OwnedVector<PlacementRun> runs{utils::OwnedPmrAllocator<PlacementRun>(request.memory)};
        constexpr bool fixedEntity = std::is_same_v<typename Images::value_type, EntityImage>;
        constexpr bool labelSet = std::is_same_v<typename Images::value_type, LabelImage>;
        for (size_t offset = 0; offset < images.size();) {
            check(); const auto ownerId = [&] {
                if constexpr (std::is_same_v<typename Images::value_type, BTreePage>) return images[offset].header().index_id;
                else if constexpr (fixedEntity || labelSet) return images[offset].owner_id;
                else return request.field_pool_owner_id;
            }();
            size_t count = std::min(size_t(RSS_PAGE_POOL_DATA_FRAME_COUNT), images.size() - offset);
            if constexpr (std::is_same_v<typename Images::value_type, BTreePage>)
                for (size_t i = 1; i < count; ++i) if (images[offset + i].header().index_id != ownerId) { count = i; break; }
            if constexpr (fixedEntity || labelSet)
                for (size_t i = 1; i < count; ++i) if (images[offset + i].owner_id != ownerId) { count = i; break; }
            const auto pool = allocatePool((fixedEntity || labelSet) ? SegmentKind::SGK_ENTITY_PAGE_POOL :
                native ? SegmentKind::SGK_PROPERTY_PAGE_POOL : SegmentKind::SGK_INDEX_PAGE_POOL, ownerId, count, native);
            runs.push_back({pool, offset, count}); offset += count;
        }
        return runs;
    };
    auto fieldRuns = planImages(fieldImages, true); auto indexRuns = planImages(indexImages, false);
    auto entityRuns = planImages(entityImages, false); auto labelRuns = planImages(labelImages, false);
    // Virtual dirty images initially use epoch 1. Bind every changed root to
    // its actual destination epoch before publishing either root authority.
    for (const auto &run : indexRuns) for (size_t i = 0; i < run.count; ++i) {
        const auto id = indexImages[run.offset + i].header().page_id;
        for (auto &root : changedRoots) if (root.root_page_id == id) {
            root.root_generation = run.pool.generation;
            if (isHybridPropertyIndexId(root.index_id)) {
                const size_t ordinal = root.index_id - RSS_HYBRID_PROPERTY_INDEX_ID_FIRST;
                require(ordinal < descriptor.roots.size(), "native topology index namespace is unavailable in this format");
                descriptor.roots[ordinal] = {id, run.pool.generation};
            }
        }
    }
    for (auto pool = oldPools.begin(); pool != oldPools.end();) {
        check(); bool detached = pool->state == SegmentState::SGS_SEALED &&
            (pool->kind == SegmentKind::SGK_PROPERTY_PAGE_POOL || pool->kind == SegmentKind::SGK_INDEX_PAGE_POOL || pool->kind == SegmentKind::SGK_ENTITY_PAGE_POOL) && allocator_.isAllocated(current, pool->physical_slot);
        if (detached) {
            const auto header = SharedPagePoolAllocator::readPoolHeader(*io_, current.data_region_offset, *pool);
            for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
                check(); const auto owner = hybrid_property_detail::currentPoolFrameOwner(*pool, header, frame,
                    [&](uint8_t index) { return sourcePools.readPageOwner(*pool, index); },
                    [&](PageId id) { return locations_.findPageOptional(current, id); });
                if (owner) detached = detached && std::binary_search(superseded.begin(), superseded.end(), owner->page_id.value());
            }
        }
        if (!detached || placement.destination(pool->segment_id)) { pool = oldPools.erase(pool); continue; }
        batch.reserveDataSlot(pool->physical_slot); pool->state = SegmentState::SGS_RETIRED; ++pool;
    }
    if (preparationSource) {
        debug::PerfTrace::addValue("native_graph.hybrid.private_field_coordinator_frames", int64_t(fieldImages.size()));
        debug::PerfTrace::addValue("native_graph.hybrid.private_field_coordinator_directories", int64_t(indexImages.size()));
        check();
        throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
            "native field coordinator preparation completed; private source cannot publish");
    }
    const auto writeImages = [&]<class Images>(Images &images, bool native, std::span<const PlacementRun> runs) {
        constexpr bool fixedEntity = std::is_same_v<typename Images::value_type, EntityImage>;
        constexpr bool labelSet = std::is_same_v<typename Images::value_type, LabelImage>;
        for (const auto &run : runs) {
            check(); const auto &pool = run.pool; const auto offset = run.offset; const auto count = run.count;
            utils::OwnedHeapReservation payloadCredit(request.memory, count *
                (utils::detail::standardAllocatorRequestBytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, alignof(std::byte)) + 3U * utils::detail::standardContainerProxyBytes()));
            utils::OwnedVector<SharedPagePoolPreparedAppend> appends{utils::OwnedPmrAllocator<SharedPagePoolPreparedAppend>(request.memory)};
            utils::OwnedVector<PageLocation> locations{utils::OwnedPmrAllocator<PageLocation>(request.memory)};
            appends.reserve(count); locations.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                check(); auto &image = images[offset + i];
                const auto id = [&] { if constexpr (std::is_same_v<typename Images::value_type, BTreePage>) return image.header().page_id; else if constexpr (fixedEntity) return image.page.header().page_id; else if constexpr (labelSet) return image.page.pageId(); else return image.pageId(); }();
                const PageOwnerHeader owner{id, pool.owner_id, fixedEntity ? PageOwnerKind::POK_ENTITY : labelSet ? PageOwnerKind::POK_ENTITY_LABEL :
                    native ? PageOwnerKind::POK_HYBRID_PROPERTY : PageOwnerKind::POK_INDEX,
                    0, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
                utils::OwnedHeapReservation referenceCredit(request.memory);
                utils::OwnedHeapReservation entityCopies(request.memory);
                if constexpr (std::is_same_v<typename Images::value_type, EntityImage>) entityCopies.set(2U *
                    (utils::detail::standardAllocatorRequestBytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, alignof(std::byte)) + utils::detail::standardContainerProxyBytes()));
                auto page = [&] {
                    if constexpr (fixedEntity) return image.page.rebindOwner(owner);
                    else if constexpr (labelSet) { auto page = std::move(image.page); page.rebindOwner(owner); return page; }
                    else if constexpr (std::is_same_v<typename Images::value_type, BTreePage>) return image.rebindOwner(owner);
                    else return std::move(image).bindPrivateOwner(owner, pool, batch.checkpointLsn(), [&](size_t bytes) { referenceCredit.set(bytes); });
                }();
                const auto bytes = [&] {
                    if constexpr (std::is_same_v<typename Images::value_type, EntityImage>) return page.serialize();
                    else if constexpr (std::is_same_v<typename Images::value_type, BTreePage>) return page.slottedPage().serializeOwned(request.memory);
                    else return page.serializeOwned(request.memory);
                }();
                const auto freeBytes = [&]() -> uint32_t {
                    if constexpr (std::is_same_v<typename Images::value_type, EntityImage>) return 0;
                    else if constexpr (std::is_same_v<typename Images::value_type, BTreePage>) return page.slottedPage().stats().contiguous_free_bytes;
                    else return page.stats().contiguous_free_bytes;
                }();
                SharedPagePoolPreparedAppend append; append.owner = owner; append.free_bytes = freeBytes;
                append.payload.assign(bytes.begin(), bytes.end()); appends.push_back(std::move(append));
                locations.push_back({id, pool.segment_id, 0, pool.generation, freeBytes});
            }
            check(); placement.write(pool, appends, locations, native);
            for (const auto &location : locations) published.push_back(location);
        }
    };
    writeImages(fieldImages, true, fieldRuns); writeImages(indexImages, false, indexRuns);
    writeImages(entityImages, false, entityRuns); writeImages(labelImages, false, labelRuns);
    for (const auto &pool : newPools) {
        const auto inPool = [&](const auto &image) { return image.pool.segment_id == pool.segment_id; };
        const auto count = size_t(std::count_if(content.images_.begin(), content.images_.end(), inPool)) +
            size_t(std::count_if(immutableImages.frames_.begin(), immutableImages.frames_.end(), inPool));
        if (!count) continue; // field/entity/directory images were written above
        check(); utils::OwnedHeapReservation payloadCredit(request.memory, count *
            (utils::detail::standardAllocatorRequestBytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, alignof(std::byte)) + 3U * utils::detail::standardContainerProxyBytes()));
        utils::OwnedVector<SharedPagePoolPreparedAppend> appends{utils::OwnedPmrAllocator<SharedPagePoolPreparedAppend>(request.memory)};
        utils::OwnedVector<PageLocation> locations{utils::OwnedPmrAllocator<PageLocation>(request.memory)};
        appends.reserve(count); locations.reserve(count);
        for (const auto &image : content.images_) if (inPool(image)) {
            check(); const auto bytes = image.page.slottedPage().serializeOwned(request.memory);
            SharedPagePoolPreparedAppend append;
            append.owner = {image.location.page_id, pool.owner_id, PageOwnerKind::POK_INDEX, 0,
                RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
            append.payload.assign(bytes.begin(), bytes.end()); append.free_bytes = image.location.free_bytes;
            appends.push_back(std::move(append)); auto location = image.location; location.page_index = 0; locations.push_back(location);
        }
        for (const auto &frame : immutableImages.frames_) if (inPool(frame)) {
            check(); SharedPagePoolPreparedAppend append; append.owner = frame.owner;
            append.payload.assign(frame.payload.begin(), frame.payload.end()); appends.push_back(std::move(append));
            auto location = frame.location; location.page_index = 0; locations.push_back(location);
        }
        placement.write(pool, appends, locations, false);
        for (const auto &location : locations) published.push_back(location);
    }

    for (const auto &reservation : placement.reservations_) if (reservation.fresh) {
        const auto &pool = reservation.pool; allocator_.prepareSetAllocated(batch, pool.physical_slot, true);
        catalog_.prepareUpsert(batch, std::span(&pool, 1));
    }
    std::sort(published.begin(), published.end(), [](const auto &a, const auto &b) { return a.page_id < b.page_id; });
    std::sort(erasedPages.begin(), erasedPages.end()); erasedPages.erase(std::unique(erasedPages.begin(), erasedPages.end()), erasedPages.end());
    locations_.prepareReplacePages(batch, published, erasedPages);
    if (!immutableImages.manifests_.empty()) locations_.prepareReplaceLargeValueManifests(batch, immutableImages.manifests_, {});
    if (!erasedNodes.empty()) locations_.prepareReplaceNodePageLocations(batch, {}, erasedNodes);
    if (!erasedEdges.empty()) locations_.prepareReplaceEdgePageLocations(batch, {}, erasedEdges);
    std::sort(changedRoots.begin(), changedRoots.end(), [](const auto &a, const auto &b) { return a.index_id < b.index_id; });
    std::sort(erasedRoots.begin(), erasedRoots.end());
    if (!changedRoots.empty() || !erasedRoots.empty()) locations_.prepareReplaceIndexRoots(batch, changedRoots, erasedRoots);
    utils::OwnedVector<RetireQueueEntry> retirements{utils::OwnedPmrAllocator<RetireQueueEntry>(request.memory)};
    retirements.reserve(oldPools.size());
    for (const auto &pool : oldPools) retirements.push_back({pool.physical_slot, pool.segment_id, batch.generation(), RetireQueueDisposition::RQD_RETIRED_SEGMENT});
    std::sort(retirements.begin(), retirements.end(), [](const auto &a, const auto &b) { return a.physical_slot < b.physical_slot; });
    if (!oldPools.empty()) { catalog_.prepareUpsert(batch, oldPools); retirement_.prepareQueueInsertions(batch, retirements); }
    descriptor.semantic_generation = batch.generation(); pages_.prepareHybridPropertyDescriptorImpl(batch, descriptor, true);
    if (scanDeletion) {
        scanDeletion->borrowed_placement_ = &placement;
        materializeScanMembershipMutation(batch, writer, source, *scanDeletion, sourceTrees);
    }
    // Its pool hints are optional; this private plan dies before the caller's
    // A/B publication, while all durable reservations belong to the batch.
    batch.hybrid_property_publication_ = true; check(); return true;
}

Superblock RelocatableMetadataStore::relocateHybridPropertyFieldPool(const NativeHybridPropertyPoolRelocation &request,
    LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
    if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
        throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native field relocation is disabled");
    const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
    const auto check = [&] { if (request.cancellation) request.cancellation->check(); };
    const auto account = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(request.memory);
    constexpr size_t maximumBytes = 1024U * 1024U;
    require(account && account->used() <= maximumBytes && account->available() <= maximumBytes - account->used() &&
        request.source_segment.isValid() && request.first_slot.isValid() && request.last_slot >= request.first_slot,
        "native field relocation requires a bounded account and exact pool/destination identities");
    check(); utils::OwnedAllocationScope scope(request.memory);
    auto writer = beginWrite(true); const auto current = bootstrap_.recover();
    require(current.database_id == request.expected_database_id && current.generation == request.expected_generation &&
        current.hybrid_property_descriptor_root.isValid() && current.hybrid_property_descriptor_root == request.expected_descriptor,
        "native field relocation has a stale or foreign source fence");
    const auto sourcePool = catalog_.find(current, request.source_segment);
    require(sourcePool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && sourcePool.state == SegmentState::SGS_SEALED &&
        allocator_.isAllocated(current, sourcePool.physical_slot), "native field relocation requires a sealed allocated property pool");
    auto source = snapshots_.pinCurrentUnderWrite(writer, current);
    MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn, request.memory);
    batch.reserveDataSlot(sourcePool.physical_slot);
    HybridPropertyFieldPageStore fields(io_, *this, source);
    SharedPagePoolAllocator sourcePools(io_, *this, source);
    const auto header = SharedPagePoolAllocator::readPoolHeader(*io_, current.data_region_offset, sourcePool);
    struct Image { PageOwnerKind kind; std::variant<HybridPropertyPageImage, SlottedPage> page; };
    utils::OwnedVector<Image> images{utils::OwnedPmrAllocator<Image>(request.memory)};
    images.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
    for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
        check(); const auto currentOwner = hybrid_property_detail::currentPoolFrameOwner(sourcePool, header, frame,
            [&](uint8_t index) { return sourcePools.readPageOwner(sourcePool, index); },
            [&](PageId id) { return locations_.findPageOptional(current, id); });
        if (!currentOwner) continue; const auto owner = *currentOwner;
        require(!owner.flags && (owner.kind == PageOwnerKind::POK_HYBRID_PROPERTY ||
            (RSS_SCAN_PATH_FORMAT_ENABLED && owner.kind == PageOwnerKind::POK_PROPERTY)),
            "field relocation found an unsupported current page owner");
        if (owner.kind == PageOwnerKind::POK_HYBRID_PROPERTY)
            images.push_back({owner.kind, fields.cloneForMutation(source, owner.page_id, batch.generation(), request.memory)});
        else {
            // Physical ROW relocation keeps every logical PageId and slot
            // identity. Authenticate the owner, payload CRC and complete slotted
            // layout before rebinding only its physical owner generation/LSN.
            const auto contents = sourcePools.read(source, owner.page_id);
            auto page = SlottedPage::open(contents.owner, contents.payload, request.memory);
            require(page.stats().contiguous_free_bytes == contents.location.free_bytes,
                "ROW field relocation has a stale free-byte hint");
            images.push_back({owner.kind, std::move(page)});
        }
    }
    debug::PerfTrace::addValue("native_graph.hybrid.relocation_private_field_frames", int64_t(images.size())); check();
    utils::OwnedVector<PageLocation> published{utils::OwnedPmrAllocator<PageLocation>(request.memory)};
    published.reserve(images.size());
    std::optional<CatalogEntry> destination;
    if (!images.empty()) {
        std::optional<uint64_t> nextSlot = request.first_slot.value();
        while (nextSlot) {
            const auto free = allocator_.findFirstFree(current, PhysicalSlotId(*nextSlot), request.last_slot); if (!free) break;
            nextSlot = *free == request.last_slot ? std::nullopt : std::optional(free->value() + 1U);
            if (batch.isDataSlotReserved(*free) || batch.isMetadataPoolSlot(*free)) continue;
            destination = CatalogEntry{batch.allocateLogicalSegmentId(), *free, SegmentKind::SGK_PROPERTY_PAGE_POOL,
                sourcePool.owner_id, Generation(1), SegmentState::SGS_SEALED, batch.checkpointLsn()};
            batch.reserveDataSlot(*free); break;
        }
        require(bool(destination), "native field relocation has no free destination pool");
        const auto pool = *destination;
        utils::OwnedHeapReservation payloadCredit(request.memory, images.size() *
            (utils::detail::standardAllocatorRequestBytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, alignof(std::byte)) +
                3U * utils::detail::standardContainerProxyBytes()));
        utils::OwnedVector<SharedPagePoolPreparedAppend> appends{utils::OwnedPmrAllocator<SharedPagePoolPreparedAppend>(request.memory)};
        appends.reserve(images.size());
        for (auto &image : images) {
            check();
            const auto id = std::visit([](const auto &page) { return page.pageId(); }, image.page);
            const PageOwnerHeader owner{id, pool.owner_id, image.kind,
                0, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
            utils::OwnedHeapReservation referenceCredit(request.memory);
            auto page = std::visit([&](auto &value) -> SlottedPage {
                if constexpr (std::is_same_v<std::decay_t<decltype(value)>, HybridPropertyPageImage>)
                    return std::move(value).bindPrivateOwner(owner, pool, batch.checkpointLsn(),
                        [&](size_t bytes) { referenceCredit.set(bytes); });
                else { value.rebindOwner(owner); return std::move(value); }
            }, image.page);
            const auto bytes = page.serializeOwned(request.memory);
            SharedPagePoolPreparedAppend append; append.owner = owner; append.free_bytes = page.stats().contiguous_free_bytes;
            append.payload.assign(bytes.begin(), bytes.end()); appends.push_back(std::move(append));
            published.push_back({id, pool.segment_id, 0, pool.generation, page.stats().contiguous_free_bytes});
        }
        check();
        const bool uniform = std::all_of(appends.begin(), appends.end(), [&](const auto &append) {
            return append.owner.kind == appends.front().owner.kind;
        });
        if (uniform) {
            SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(*io_, current.data_region_offset, pool,
                appends, published, batch.checkpointLsn(), 0,
                appends.front().owner.kind == PageOwnerKind::POK_HYBRID_PROPERTY);
        } else {
            // Each typed suffix authenticates the already prepared prefix, so
            // mixed pools preserve the ordinary owner-kind checks.
            for (size_t prefix = 0; prefix < appends.size(); ++prefix) {
                SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(*io_, current.data_region_offset, pool,
                    std::span(appends).subspan(prefix, 1), std::span(published).subspan(prefix, 1),
                    batch.checkpointLsn(), prefix, appends[prefix].owner.kind == PageOwnerKind::POK_HYBRID_PROPERTY);
                check();
            }
        }
        check();
        allocator_.prepareSetAllocated(batch, pool.physical_slot, true);
        catalog_.prepareUpsert(batch, std::span(&pool, 1));
    }
    // Every frame was checked against its exact current Page Directory route.
    // All current frames now have fresh destinations; stale frames stay behind
    // the old pool's pin-protected retirement without changing their bytes.
    auto retired = sourcePool; retired.state = SegmentState::SGS_RETIRED;
    catalog_.prepareUpsert(batch, std::span(&retired, 1));
    const RetireQueueEntry retirement{sourcePool.physical_slot, sourcePool.segment_id, batch.generation(), RetireQueueDisposition::RQD_RETIRED_SEGMENT};
    retirement_.prepareQueueInsertions(batch, std::span(&retirement, 1));
    std::sort(published.begin(), published.end(), [](const auto &a, const auto &b) { return a.page_id < b.page_id; });
    if (!published.empty()) locations_.prepareReplacePages(batch, published, {});
    // No logical field or directory root changed, so retain the exact semantic
    // descriptor. Only this metadata-owned physical coordinator has admission.
    batch.hybrid_property_publication_ = true; check(); publishBatch(batch, writer, options); return batch.candidate();
}

Superblock RelocatableMetadataStore::relocateHybridPropertyIndexPool(const NativeHybridPropertyPoolRelocation &request,
    LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
    if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
        throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native index relocation is disabled");
    const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
    const auto check = [&] { if (request.cancellation) request.cancellation->check(); };
    const auto account = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(request.memory);
    constexpr size_t maximumBytes = 1024U * 1024U;
    require(account && account->used() <= maximumBytes && account->available() <= maximumBytes - account->used() &&
        request.source_segment.isValid() && request.first_slot.isValid() && request.last_slot >= request.first_slot,
        "native index relocation requires a bounded account and exact pool/destination identities");
    check(); utils::OwnedAllocationScope scope(request.memory);
    SlottedPagePool sourcePages(io_, *this, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    BTreePageStore sourceTrees(sourcePages, *this);
    auto writer = beginWrite(true); const auto current = bootstrap_.recover();
    // Private scan construction also compacts registered ordinary trees, such
    // as global adjacency. Ordinary roots retain the existing namespace guard.
    const bool privateBuilding = RSS_SCAN_PATH_FORMAT_ENABLED && (current.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING);
    require(current.database_id == request.expected_database_id && current.generation == request.expected_generation &&
        (current.hybrid_property_descriptor_root.isValid() || privateBuilding) &&
        current.hybrid_property_descriptor_root == request.expected_descriptor,
        "native index relocation has a stale or foreign source fence");
    const auto sourcePool = catalog_.find(current, request.source_segment);
    const bool nativeNamespace = isHybridPropertyIndexId(sourcePool.owner_id);
    require(sourcePool.kind == SegmentKind::SGK_INDEX_PAGE_POOL && (nativeNamespace || privateBuilding) &&
        sourcePool.state == SegmentState::SGS_SEALED && sourcePool.generation.value() < UINT64_MAX &&
        allocator_.isAllocated(current, sourcePool.physical_slot), "native index relocation requires a sealed allocated native directory pool");
    HybridPropertyDescriptor descriptor;
    size_t ordinal = 0U;
    const auto expectedRoot = locations_.findIndexRootOptional(current, sourcePool.owner_id);
    require(bool(expectedRoot), "native index relocation has no registered root");
    if (nativeNamespace) {
        require(current.hybrid_property_descriptor_root.isValid(), "native index relocation has no descriptor");
        descriptor = pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current).descriptor;
        ordinal = sourcePool.owner_id - RSS_HYBRID_PROPERTY_INDEX_ID_FIRST;
        require(ordinal < descriptor.roots.size(), "native index relocation namespace is unavailable in this format");
        require(expectedRoot->root_page_id == descriptor.roots[ordinal].page_id &&
            expectedRoot->root_generation == descriptor.roots[ordinal].generation,
            "native index relocation has no matching descriptor namespace root");
    }
    auto source = snapshots_.pinCurrentUnderWrite(writer, current);
    MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn, request.memory);
    batch.reserveDataSlot(sourcePool.physical_slot);
    SharedPagePoolAllocator sourcePools(io_, *this, source);
    const auto header = SharedPagePoolAllocator::readPoolHeader(*io_, current.data_region_offset, sourcePool);
    utils::OwnedVector<BTreePage> images{utils::OwnedPmrAllocator<BTreePage>(request.memory)};
    images.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
    for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
        check(); const auto currentOwner = hybrid_property_detail::currentPoolFrameOwner(sourcePool, header, frame,
            [&](uint8_t index) { return sourcePools.readPageOwner(sourcePool, index); },
            [&](PageId id) { return locations_.findPageOptional(current, id); });
        if (!currentOwner) continue; const auto owner = *currentOwner;
        require(owner.kind == PageOwnerKind::POK_INDEX && owner.owner_id == sourcePool.owner_id && !owner.flags,
            "native index relocation cannot move a foreign or retired current frame");
        auto page = sourceTrees.read(source, owner.page_id);
        require(page.header().index_id == sourcePool.owner_id && page.header().generation == sourcePool.generation,
            "native index relocation has a mismatched page namespace or physical epoch");
        images.push_back(std::move(page));
    }
    debug::PerfTrace::addValue("native_graph.hybrid.relocation_private_index_frames", int64_t(images.size())); check();
    utils::OwnedVector<PageLocation> published{utils::OwnedPmrAllocator<PageLocation>(request.memory)};
    published.reserve(images.size());
    std::optional<CatalogEntry> destination;
    if (!images.empty()) {
        std::optional<uint64_t> nextSlot = request.first_slot.value();
        while (nextSlot) {
            const auto free = allocator_.findFirstFree(current, PhysicalSlotId(*nextSlot), request.last_slot); if (!free) break;
            nextSlot = *free == request.last_slot ? std::nullopt : std::optional(free->value() + 1U);
            if (batch.isDataSlotReserved(*free) || batch.isMetadataPoolSlot(*free)) continue;
            destination = CatalogEntry{batch.allocateLogicalSegmentId(), *free, SegmentKind::SGK_INDEX_PAGE_POOL,
                sourcePool.owner_id, Generation(sourcePool.generation.value() + 1U), SegmentState::SGS_SEALED, batch.checkpointLsn()};
            batch.reserveDataSlot(*free); break;
        }
        require(bool(destination), "native index relocation has no free destination pool");
        const auto pool = *destination;
        utils::OwnedHeapReservation payloadCredit(request.memory, images.size() *
            (utils::detail::standardAllocatorRequestBytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, alignof(std::byte)) +
                3U * utils::detail::standardContainerProxyBytes()));
        utils::OwnedVector<SharedPagePoolPreparedAppend> appends{utils::OwnedPmrAllocator<SharedPagePoolPreparedAppend>(request.memory)};
        appends.reserve(images.size());
        for (auto &image : images) {
            check(); const auto id = image.header().page_id;
            const PageOwnerHeader owner{id, pool.owner_id, PageOwnerKind::POK_INDEX,
                0, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
            const auto page = image.rebindOwner(owner);
            const auto bytes = page.slottedPage().serializeOwned(request.memory);
            SharedPagePoolPreparedAppend append; append.owner = owner; append.free_bytes = page.slottedPage().stats().contiguous_free_bytes;
            append.payload.assign(bytes.begin(), bytes.end()); appends.push_back(std::move(append));
            published.push_back({id, pool.segment_id, 0, pool.generation, page.slottedPage().stats().contiguous_free_bytes});
        }
        check(); SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(*io_, current.data_region_offset, pool,
            appends, published, batch.checkpointLsn(), 0, false); check();
        allocator_.prepareSetAllocated(batch, pool.physical_slot, true);
        catalog_.prepareUpsert(batch, std::span(&pool, 1));
    }
    // Every frame was checked against its exact current Page Directory route.
    // All current frames now have fresh destinations; stale frames stay behind
    // the old pool's pin-protected retirement without changing their bytes.
    auto retired = sourcePool; retired.state = SegmentState::SGS_RETIRED;
    catalog_.prepareUpsert(batch, std::span(&retired, 1));
    const RetireQueueEntry retirement{sourcePool.physical_slot, sourcePool.segment_id, batch.generation(), RetireQueueDisposition::RQD_RETIRED_SEGMENT};
    retirement_.prepareQueueInsertions(batch, std::span(&retirement, 1));
    std::sort(published.begin(), published.end(), [](const auto &a, const auto &b) { return a.page_id < b.page_id; });
    if (!published.empty()) locations_.prepareReplacePages(batch, published, {});
    if (destination && std::any_of(published.begin(), published.end(), [&](const auto &location) { return location.page_id == expectedRoot->root_page_id; })) {
        const IndexRootLocation replacement{sourcePool.owner_id, expectedRoot->root_page_id, destination->generation};
        locations_.prepareReplaceIndexRoots(batch, std::span(&replacement, 1), {});
        if (nativeNamespace) {
            descriptor.roots[ordinal] = {replacement.root_page_id, replacement.root_generation};
            // The native namespace also carries the new physical root epoch.
            descriptor.semantic_generation = batch.generation();
            pages_.prepareHybridPropertyDescriptorImpl(batch, descriptor, true);
        }
    }
    batch.hybrid_property_publication_ = true; check(); publishBatch(batch, writer, options); return batch.candidate();
}

Superblock RelocatableMetadataStore::relocateHybridPropertyEntityPool(const NativeHybridPropertyPoolRelocation &request,
    LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
    if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
        throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native Entity relocation is disabled");
    const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
    const auto check = [&] { if (request.cancellation) request.cancellation->check(); };
    const auto account = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(request.memory);
    constexpr size_t maximumBytes = 1024U * 1024U;
    require(account && account->used() <= maximumBytes && account->available() <= maximumBytes - account->used() &&
        request.source_segment.isValid() && request.first_slot.isValid() && request.last_slot >= request.first_slot,
        "native Entity relocation requires a bounded account and pool/destination identities");
    check(); utils::OwnedAllocationScope scope(request.memory);
    utils::OwnedHeapReservation scratch(request.memory, 16U * 1024U);
    auto writer = beginWrite(true); const auto current = bootstrap_.recover();
    require(current.database_id == request.expected_database_id && current.generation == request.expected_generation &&
        current.hybrid_property_descriptor_root.isValid() && current.hybrid_property_descriptor_root == request.expected_descriptor,
        "native Entity relocation has a stale or foreign source fence");
    const auto sourcePool = catalog_.find(current, request.source_segment);
    require(sourcePool.kind == SegmentKind::SGK_ENTITY_PAGE_POOL && sourcePool.state == SegmentState::SGS_SEALED &&
        sourcePool.generation.value() < UINT64_MAX && allocator_.isAllocated(current, sourcePool.physical_slot),
        "native Entity relocation requires a sealed allocated Entity pool with a next physical epoch");
    auto source = snapshots_.pinCurrentUnderWrite(writer, current);
    MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn, request.memory); batch.reserveDataSlot(sourcePool.physical_slot);
    SharedPagePoolAllocator sourcePools(io_, *this, source);
    const auto header = SharedPagePoolAllocator::readPoolHeader(*io_, current.data_region_offset, sourcePool);
    struct Image { PageId id; PageOwnerKind kind; std::variant<EntityPage, SlottedPage> page; };
    utils::OwnedHeapReservation entityCredit(request.memory); size_t fixedPages = 0;
    utils::OwnedVector<Image> images{utils::OwnedPmrAllocator<Image>(request.memory)}; images.reserve(RSS_PAGE_POOL_DATA_FRAME_COUNT);
    for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
        check(); const auto owner = hybrid_property_detail::currentPoolFrameOwner(sourcePool, header, frame,
            [&](uint8_t index) { return sourcePools.readPageOwner(sourcePool, index); },
            [&](PageId id) { return locations_.findPageOptional(current, id); });
        if (!owner) continue;
        require(owner->owner_id == sourcePool.owner_id &&
            (owner->kind == PageOwnerKind::POK_ENTITY || owner->kind == PageOwnerKind::POK_ENTITY_LABEL),
            "native Entity relocation found a current foreign page owner");
        const auto contents = sourcePools.read(source, owner->page_id);
        if (owner->kind == PageOwnerKind::POK_ENTITY) {
            entityCredit.set(++fixedPages * (utils::detail::standardAllocatorRequestBytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
                alignof(std::byte)) + utils::detail::standardContainerProxyBytes()));
            require(contents.location.free_bytes == 0, "fixed Entity relocation has a nonzero free-byte hint");
            images.push_back({owner->page_id, owner->kind, EntityPage::open(contents.owner, contents.payload)});
        } else {
            auto page = SlottedPage::open(contents.owner, contents.payload, request.memory);
            require(page.stats().contiguous_free_bytes == contents.location.free_bytes, "LabelSet relocation has a stale free-byte hint");
            const auto slots = page.liveSlotsOwned(request.memory);
            for (const auto &slot : slots) {
                check(); const auto bytes = page.readOwned(slot, request.memory); utils::OwnedHeapReservation decodedCredit(request.memory);
                const auto labels = EntityLabelSetCodec::decode(bytes, [&](size_t size) {
                    decodedCredit.set(utils::detail::standardAllocatorRequestBytes(size, alignof(uint64_t)) +
                        3U * utils::detail::standardContainerProxyBytes());
                });
                static_cast<void>(labels);
            }
            images.push_back({owner->page_id, owner->kind, std::move(page)});
        }
    }
    debug::PerfTrace::addValue("native_graph.hybrid.relocation_private_entity_frames", int64_t(images.size())); check();
    utils::OwnedVector<PageLocation> published{utils::OwnedPmrAllocator<PageLocation>(request.memory)}; published.reserve(images.size());
    if (!images.empty()) {
        std::optional<CatalogEntry> destination; std::optional<uint64_t> nextSlot = request.first_slot.value();
        while (nextSlot) {
            const auto free = allocator_.findFirstFree(current, PhysicalSlotId(*nextSlot), request.last_slot); if (!free) break;
            nextSlot = *free == request.last_slot ? std::nullopt : std::optional(free->value() + 1U);
            if (batch.isDataSlotReserved(*free) || batch.isMetadataPoolSlot(*free)) continue;
            destination = CatalogEntry{batch.allocateLogicalSegmentId(), *free, SegmentKind::SGK_ENTITY_PAGE_POOL,
                sourcePool.owner_id, Generation(sourcePool.generation.value() + 1U), SegmentState::SGS_SEALED, batch.checkpointLsn()};
            batch.reserveDataSlot(*free); break;
        }
        require(bool(destination), "native Entity relocation has no free destination pool"); const auto pool = *destination;
        utils::OwnedHeapReservation payloadCredit(request.memory, images.size() *
            (utils::detail::standardAllocatorRequestBytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, alignof(std::byte)) +
                3U * utils::detail::standardContainerProxyBytes()));
        utils::OwnedVector<SharedPagePoolPreparedAppend> appends{utils::OwnedPmrAllocator<SharedPagePoolPreparedAppend>(request.memory)}; appends.reserve(images.size());
        for (auto &image : images) {
            check(); const PageOwnerHeader owner{image.id, pool.owner_id, image.kind, 0, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
            utils::OwnedHeapReservation copies(request.memory);
            const auto bytes = std::visit([&](auto &page) -> utils::OwnedVector<std::byte> {
                if constexpr (std::is_same_v<std::decay_t<decltype(page)>, EntityPage>) {
                    copies.set(2U * (utils::detail::standardAllocatorRequestBytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
                        alignof(std::byte)) + utils::detail::standardContainerProxyBytes()));
                    const auto rebound = page.rebindOwner(owner); const auto raw = rebound.serialize();
                    utils::OwnedVector<std::byte> encoded{utils::OwnedPmrAllocator<std::byte>(request.memory)};
                    encoded.assign(raw.begin(), raw.end()); return encoded;
                } else { page.rebindOwner(owner); return page.serializeOwned(request.memory); }
            }, image.page);
            const auto freeBytes = std::visit([](const auto &page) -> uint32_t {
                if constexpr (std::is_same_v<std::decay_t<decltype(page)>, EntityPage>) return 0;
                else return page.stats().contiguous_free_bytes;
            }, image.page);
            SharedPagePoolPreparedAppend append; append.owner = owner; append.free_bytes = freeBytes;
            append.payload.assign(bytes.begin(), bytes.end()); appends.push_back(std::move(append));
            published.push_back({image.id, pool.segment_id, 0, pool.generation, freeBytes});
        }
        check(); SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(*io_, current.data_region_offset, pool,
            appends, published, batch.checkpointLsn(), 0, false); check();
        allocator_.prepareSetAllocated(batch, pool.physical_slot, true); catalog_.prepareUpsert(batch, std::span(&pool, 1));
    }
    auto retired = sourcePool; retired.state = SegmentState::SGS_RETIRED; catalog_.prepareUpsert(batch, std::span(&retired, 1));
    const RetireQueueEntry retirement{sourcePool.physical_slot, sourcePool.segment_id, batch.generation(), RetireQueueDisposition::RQD_RETIRED_SEGMENT};
    retirement_.prepareQueueInsertions(batch, std::span(&retirement, 1));
    std::sort(published.begin(), published.end(), [](const auto &a, const auto &b) { return a.page_id < b.page_id; });
    if (!published.empty()) locations_.prepareReplacePages(batch, published, {});
    batch.hybrid_property_publication_ = true; check(); publishBatch(batch, writer, options); return batch.candidate();
}

Superblock RelocatableMetadataStore::publishHybridPropertyRows(MetadataWriteBatch &batch,
    const MetadataWriteGuard &writer, const MetadataSnapshotPin &source,
    const NativeGraphHybridPropertyConversion &conversion, EntityPageStore &entities,
    SlottedPagePool &sourcePages, BTreePageStore &sourceTrees, LargeValueStore &largeValues,
    const MetadataPublishOptions &options, HybridPropertyReadContext *preparationSource) {
    const auto require = [](bool valid, const char *message) { hybrid_property_detail::require(valid, message); };
    const auto &current = batch.current();
    require(RSS_HYBRID_PROPERTY_FORMAT_ENABLED && source.superblock() == current && snapshots_.owns(source) &&
        current.hybrid_property_descriptor_root.isValid() && entities.usesMetadataStore(*this) &&
        sourcePages.usesMetadataStore(*this) && sourceTrees.usesMetadataStore(*this) && largeValues.usesMetadataStore(*this),
        "inverse native conversion requires its exact writer source and typed stores");
    utils::OwnedAllocationScope scope(conversion.memory);
    const auto check = [&] { if (conversion.cancellation) conversion.cancellation->check(); };
    check();
    auto descriptor = pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current).descriptor;
    const size_t base = conversion.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? 0U : 4U;
    HybridPropertyReadContext durableSource(io_, sourcePages, *this, source, conversion.cancellation);
    auto &context = preparationSource ? *preparationSource : durableSource;
    require(!preparationSource || (context.usesSnapshot(source) && &context.metadata_ == this &&
        context.source_io_ == io_ && !conversion.publish_candidate && context.descriptor_),
        "private inverse requires its exact source pin/file and cannot publish");
    if (preparationSource) descriptor = *context.descriptor_;
    // The complete projection and source census below authenticate cold inverse
    // ownership. Generic groupRecord remains guarded for unadapted consumers;
    // a private candidate still unconditionally refuses before physical I/O.
    HybridPropertyRowPreparation projection(context, source, *this, entities, conversion.owner_kind,
        conversion.group_id, conversion.memory, &largeValues, conversion.cancellation);
    const auto group = projection.sourceGroup();
    HybridPropertyFieldPageStore sourceFields(io_, *this, source);
    utils::OwnedHeapReservation boundary(conversion.memory);
    const auto admit = [&](size_t bytes) { boundary.set(bytes); };
    utils::OwnedVector<HybridPropertyOwnerMap::Key> ownerKeys{utils::OwnedPmrAllocator<HybridPropertyOwnerMap::Key>(conversion.memory)};
    utils::OwnedVector<HybridPropertyFieldDirectoryMap::Key> columnKeys{utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Key>(conversion.memory)};
    utils::OwnedVector<HybridPropertyFieldDirectoryMap::Key> extensionKeys{utils::OwnedPmrAllocator<HybridPropertyFieldDirectoryMap::Key>(conversion.memory)};
    utils::OwnedVector<SlottedSlotReference> references{utils::OwnedPmrAllocator<SlottedSlotReference>(conversion.memory)};
    if (!group.usesColumnMembership()) ownerKeys.reserve(group.memberships);
    columnKeys.reserve(group.columns);
    references.reserve(group.columns + 2U + (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER ? 64U : 0U));
    std::optional<HybridPropertyColumnMembership> sourceMatrix;
    std::array<const PropertyCollectionRoot *, 64> projected{};
    for (const auto &row : projection.rows()) projected[hybridPropertySlotFor({conversion.owner_kind, row.owner_entity_id})] = &row;
    const auto groupBinding = HybridPropertyDirectoryCodec::bindRoot(descriptor, source,
        static_cast<HybridPropertyRootOrdinal>(base + 3));
    HybridPropertyGroupDirectoryMap groups(groupBinding.root.index_id, sourceTrees, *this, groupBinding);
    const auto groupKey = HybridPropertyDirectoryCodec::groupKey(conversion.group_id);
    const auto sourceGroup = groups.find(source, groupKey, std::ref(admit), conversion.cancellation); boundary.set(0);
    require(bool(sourceGroup), "inverse selected group has no exact directory locator");
    references.push_back(HybridPropertyDirectoryCodec::decodeLocator(*sourceGroup));
    if (group.usesColumnMembership()) {
        const auto matrixPage = sourceFields.read(source, group.column_membership.page_id);
        sourceMatrix = matrixPage.view().readColumnMembership(group.column_membership, group, descriptor.semantic_generation);
        references.push_back(group.column_membership);
    }
    const auto selectedSource = [&](EntityPropertyKey owner, uint32_t token,
        const std::function<void(size_t)> &admission) -> std::optional<HybridPropertySource> {
        if (group.mode != HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER)
            return context.selector(group, owner, token, admission);
        require(sourceMatrix && owner.owner_kind == group.owner_kind && hybridPropertyGroupFor(owner) == group.group_id,
            "cold inverse selection escaped its authenticated group");
        const auto *row = projected[hybridPropertySlotFor(owner)];
        if (!row) return {};
        const auto field = std::lower_bound(row->entries.begin(), row->entries.end(), token,
            [](const auto &entry, uint32_t selected) { return entry.key_token < selected; });
        if (field == row->entries.end() || field->key_token != token) return {};
        if (sourceMatrix->ordinal(token)) return sourceMatrix->contains(owner, token) ?
            std::optional(HybridPropertySource::HPS_COLUMN) : std::nullopt;
        return (sourceMatrix->cold_rows.owners & (UINT64_C(1) << hybridPropertySlotFor(owner))) ?
            std::optional(HybridPropertySource::HPS_COLD_ROW) : std::nullopt;
    };
    if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER)
        for (unsigned slot = 0; slot < 64; ++slot) {
            const EntityPropertyKey owner{conversion.owner_kind, (conversion.group_id - 1U) * 64U + slot + 1U};
            if (!owner.isValid()) continue;
            context.requireNoLegacySelector(owner, admit); boundary.set(0);
        }
    size_t actualMemberships = 0;
    for (const auto &row : projection.rows()) for (const auto &field : row.entries) {
        const EntityPropertyKey owner{conversion.owner_kind, row.owner_entity_id};
        const auto selected = selectedSource(owner, field.key_token, std::ref(admit)); boundary.set(0);
        require(bool(selected), "inverse projected field has no source selector");
        ++actualMemberships;
        if (!group.usesColumnMembership() || *selected == HybridPropertySource::HPS_EXTENSION)
            ownerKeys.push_back(HybridPropertyDirectoryCodec::ownerKey(owner, field.key_token));
    }
    require(actualMemberships == group.memberships, "inverse selected group lost actual memberships");
    uint64_t bodyMemberships = 0;
    size_t directColumnCount = 0;
    const auto selector = [&](EntityPropertyKey owner, uint32_t token, HybridPropertySource expected) {
        utils::OwnedHeapReservation selectorBoundary(conversion.memory);
        const auto value = selectedSource(owner, token, [&](size_t bytes) { selectorBoundary.set(bytes); });
        require(value && *value == expected, "inverse selected body has no exact authoritative selector");
    };
    if (group.usesColumnMembership()) {
        const auto matrixPage = sourceFields.read(source, group.column_membership.page_id);
        const auto matrix = matrixPage.view().readColumnMembership(group.column_membership, group, descriptor.semantic_generation);
        for (size_t i = 0; i < matrix.column_count; ++i) {
            check(); const auto reference = matrix.columnLocator(matrix.tokens[i]);
            const auto page = sourceFields.read(source, reference.page_id);
            const auto column = page.view().readColumn(reference, group, matrix.tokens[i], descriptor.semantic_generation);
            require(column.presence == matrix.presence(matrix.tokens[i]), "inverse compact column contradicts membership presence");
            for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot)
                if (column.presence & (UINT64_C(1) << slot)) selector({conversion.owner_kind,
                    (conversion.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + slot + 1U}, matrix.tokens[i], HybridPropertySource::HPS_COLUMN);
            bodyMemberships += std::popcount(column.presence); ++directColumnCount; references.push_back(reference);
        }
    }
    if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
        uint64_t seen = 0;
        for (unsigned slot = 0; slot < 64; ++slot) {
            const auto bit = UINT64_C(1) << slot;
            if (!(sourceMatrix->cold_rows.owners & bit) || (seen & bit)) continue;
            const auto reference = sourceMatrix->cold_rows.roots[slot];
            context.withColdBlock(group, *sourceMatrix, reference, [&](const auto &block, const auto &) {
                seen |= block.owners; bodyMemberships += block.fields;
            });
            references.push_back(reference);
        }
        require(seen == sourceMatrix->cold_rows.owners, "inverse cold source census lost actual owners");
    }
    if (descriptor.roots[base + 1].page_id.isValid()) {
        const auto binding = HybridPropertyDirectoryCodec::bindRoot(descriptor, source, static_cast<HybridPropertyRootOrdinal>(base + 1));
        HybridPropertyFieldDirectoryMap map(binding.root.index_id, sourceTrees, *this, binding);
        require(map.visitPrefix(source, groupKey, group.columns + 1U, [&](const auto &entry) {
            check(); require(!group.usesColumnMembership(),
                "inverse compact group has a competing column directory");
            require(columnKeys.size() < group.columns, "inverse selected group has excess columns");
            const auto field = HybridPropertyDirectoryCodec::decodeColumnKey(entry.key);
            require(field.group == conversion.group_id, "inverse column escaped its group prefix");
            const auto reference = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
            const auto page = sourceFields.read(source, reference.page_id);
            const auto column = page.view().readColumn(reference, group, field.token, descriptor.semantic_generation);
            for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot)
                if (column.presence & (UINT64_C(1) << slot)) selector({conversion.owner_kind,
                    (conversion.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + slot + 1U}, field.token, HybridPropertySource::HPS_COLUMN);
            bodyMemberships += std::popcount(column.presence);
            columnKeys.push_back(entry.key); references.push_back(reference); return true;
        }, std::ref(admit), conversion.cancellation), "inverse selected column scan was incomplete"); boundary.set(0);
    }
    require(columnKeys.size() + directColumnCount == group.columns, "inverse selected column census differs from group counts");
    if (descriptor.roots[base + 2].page_id.isValid()) {
        const auto binding = HybridPropertyDirectoryCodec::bindRoot(descriptor, source, static_cast<HybridPropertyRootOrdinal>(base + 2));
        HybridPropertyFieldDirectoryMap map(binding.root.index_id, sourceTrees, *this, binding);
        for (uint64_t slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot) {
            const EntityPropertyKey owner{conversion.owner_kind, (conversion.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + slot + 1U};
            if (!owner.isValid()) continue;
            const auto prefix = HybridPropertyDirectoryCodec::ownerKey(owner, 1);
            require(map.visitPrefix(source, std::span(prefix).first(8), group.memberships + 1U, [&](const auto &entry) {
                check(); require(extensionKeys.size() < group.memberships, "inverse selected group has excess extensions");
                const auto field = HybridPropertyDirectoryCodec::decodeOwnerKey(entry.key, conversion.owner_kind);
                require(field.owner == owner, "inverse extension escaped its owner prefix");
                selector(owner, field.token, HybridPropertySource::HPS_EXTENSION);
                extensionKeys.push_back(entry.key); references.push_back(HybridPropertyDirectoryCodec::decodeLocator(entry.value));
                ++bodyMemberships; return true;
            }, std::ref(admit), conversion.cancellation), "inverse selected extension scan was incomplete"); boundary.set(0);
        }
    }
    require(bodyMemberships == group.memberships, "inverse body membership census differs from actual selectors");
    utils::OwnedVector<PageId> fieldIds{utils::OwnedPmrAllocator<PageId>(conversion.memory)};
    for (const auto &reference : references) fieldIds.push_back(reference.page_id);
    std::sort(fieldIds.begin(), fieldIds.end()); fieldIds.erase(std::unique(fieldIds.begin(), fieldIds.end()), fieldIds.end());
    // A converted cohort can pack records from several groups into one page.
    // Authenticate the complete source page, then remove only this group's
    // slots in a private COW image. Other groups retain their logical locators
    // and old snapshots retain the original physical page.
    utils::OwnedVector<HybridPropertyPageImage> retainedFields{
        utils::OwnedPmrAllocator<HybridPropertyPageImage>(conversion.memory)};
    utils::OwnedVector<uint64_t> retiredIds{utils::OwnedPmrAllocator<uint64_t>(conversion.memory)};
    utils::OwnedVector<uint64_t> supersededIds{utils::OwnedPmrAllocator<uint64_t>(conversion.memory)};
    for (const auto id : fieldIds) {
        check();
        const auto records = HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, source, id, conversion.memory, &largeValues);
        const auto selected = size_t(std::count_if(references.begin(), references.end(),
            [&](const auto &ref) { return ref.page_id == id; }));
        require(records >= selected, "inverse selected page has fewer records than its selected locators");
        // Erasing also proves distinct, live slot generations; a duplicate
        // locator must fail rather than making a page look fully detached.
        auto image = sourceFields.cloneForMutation(source, id, batch.generation(), conversion.memory);
        for (const auto &reference : references) if (reference.page_id == id) {
            check(); image.erase(reference);
        }
        require(image.liveSlotCount() == records - selected,
            "inverse selected page census disagrees with private slot removal");
        supersededIds.push_back(id.value());
        if (image.liveSlotCount()) retainedFields.push_back(std::move(image));
        else retiredIds.push_back(id.value());
    }
    utils::OwnedVector<uint64_t> erasedRoots{utils::OwnedPmrAllocator<uint64_t>(conversion.memory)};
    utils::OwnedVector<IndexRootLocation> changedRoots{utils::OwnedPmrAllocator<IndexRootLocation>(conversion.memory)};
    utils::OwnedVector<HybridPropertyDirectoryPreparation::Image> directoryImages{
        utils::OwnedPmrAllocator<HybridPropertyDirectoryPreparation::Image>(conversion.memory)};
    struct RowImage { CatalogEntry pool; PageLocation location; SlottedPage page; };
    utils::OwnedVector<CatalogEntry> newPools{utils::OwnedPmrAllocator<CatalogEntry>(conversion.memory)};
    utils::OwnedVector<RowImage> images{utils::OwnedPmrAllocator<RowImage>(conversion.memory)};
    utils::OwnedVector<EntityPropertyLocation> properties{utils::OwnedPmrAllocator<EntityPropertyLocation>(conversion.memory)};
    properties.reserve(projection.rows().size()); images.reserve(projection.rows().size()); newPools.reserve(projection.rows().size());
    std::optional<uint64_t> nextSlot = conversion.first_slot.value();
    const auto newPool = [&](SegmentKind kind, uint64_t ownerId, size_t = RSS_PAGE_POOL_DATA_FRAME_COUNT) {
        while (nextSlot) {
            check(); const auto free = allocator_.findFirstFree(current, PhysicalSlotId(*nextSlot), conversion.last_slot);
            if (!free) break;
            nextSlot = *free == conversion.last_slot ? std::nullopt : std::optional(free->value() + 1U);
            if (batch.isDataSlotReserved(*free) || batch.isMetadataPoolSlot(*free)) continue;
            CatalogEntry pool{batch.allocateLogicalSegmentId(), *free, kind,
                ownerId, Generation(1), SegmentState::SGS_SEALED, batch.checkpointLsn()};
            batch.reserveDataSlot(pool.physical_slot); newPools.push_back(pool); return pool;
        }
        throw FormatException(FormatError::FERR_INVALID_STATE, "inverse native conversion has no free destination pool");
    };
    const auto prepareDirectory = [&]<class Map>(size_t role, std::span<const typename Map::Key> erases) {
        if (erases.empty()) return;
        const auto binding = HybridPropertyDirectoryCodec::bindRoot(descriptor, source, static_cast<HybridPropertyRootOrdinal>(base + role));
        HybridPropertyDirectoryPreparation prepared(batch, source, *this, sourceTrees, binding,
            std::span<const typename Map::Entry>{}, erases, conversion.memory, conversion.cancellation,
            newPool, check, std::type_identity<Map>{});
        if (prepared.root_) {
            descriptor.roots[base + role] = {prepared.root_->root_page_id, prepared.root_->root_generation};
            changedRoots.push_back(*prepared.root_);
        } else { descriptor.roots[base + role] = {}; erasedRoots.push_back(binding.root.index_id); }
        for (auto &image : prepared.images_) directoryImages.push_back(std::move(image));
        retiredIds.insert(retiredIds.end(), prepared.erased_.begin(), prepared.erased_.end());
        supersededIds.insert(supersededIds.end(), prepared.superseded_.begin(), prepared.superseded_.end());
    };
    prepareDirectory.template operator()<HybridPropertyOwnerMap>(0, ownerKeys);
    prepareDirectory.template operator()<HybridPropertyFieldDirectoryMap>(1, columnKeys);
    prepareDirectory.template operator()<HybridPropertyFieldDirectoryMap>(2, extensionKeys);
    prepareDirectory.template operator()<HybridPropertyGroupDirectoryMap>(3, std::span(&groupKey, 1));
    descriptor.semantic_generation = batch.generation();
    std::sort(supersededIds.begin(), supersededIds.end());
    require(std::adjacent_find(supersededIds.begin(), supersededIds.end()) == supersededIds.end(),
        "inverse native directory and field pages overlap");
    std::sort(retiredIds.begin(), retiredIds.end());
    utils::OwnedVector<CatalogEntry> retiredPools{utils::OwnedPmrAllocator<CatalogEntry>(conversion.memory)};
    for (const auto id : supersededIds) {
        const auto location = locations_.findPage(current, PageId(id)); retiredPools.push_back(catalog_.find(current, location.segment_id));
    }
    std::sort(retiredPools.begin(), retiredPools.end(), [](const auto &a, const auto &b) { return a.segment_id < b.segment_id; });
    retiredPools.erase(std::unique(retiredPools.begin(), retiredPools.end(), [](const auto &a, const auto &b) { return a.segment_id == b.segment_id; }), retiredPools.end());
    SharedPagePoolAllocator sourcePools(io_, *this, source);
    for (auto pool = retiredPools.begin(); pool != retiredPools.end();) {
        check(); require(pool->state == SegmentState::SGS_SEALED && allocator_.isAllocated(current, pool->physical_slot) &&
            (pool->kind == SegmentKind::SGK_PROPERTY_PAGE_POOL ||
                (pool->kind == SegmentKind::SGK_INDEX_PAGE_POOL && isHybridPropertyIndexId(pool->owner_id))),
            "inverse native retirement has an incompatible source pool");
        bool detached = true;
        const auto header = SharedPagePoolAllocator::readPoolHeader(*io_, current.data_region_offset, *pool);
        for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
            check(); const auto owner = hybrid_property_detail::currentPoolFrameOwner(*pool, header, frame,
                [&](uint8_t index) { return sourcePools.readPageOwner(*pool, index); },
                [&](PageId id) { return locations_.findPageOptional(current, id); });
            if (owner) detached = detached && std::binary_search(supersededIds.begin(), supersededIds.end(), owner->page_id.value());
        }
        if (!detached) { pool = retiredPools.erase(pool); continue; }
        batch.reserveDataSlot(pool->physical_slot); pool->state = SegmentState::SGS_RETIRED; ++pool;
    }
    // Pay the existing canonical encoder's bounded standard-vector frontier.
    // Reject roots lacking a supported ROW carrier before writing any frame.
    utils::OwnedVector<HybridPropertyLargeFieldPreparation> largeRows{
        utils::OwnedPmrAllocator<HybridPropertyLargeFieldPreparation>(conversion.memory)};
    utils::OwnedHeapReservation encodingScratch(conversion.memory, 64U * 1024U);
    for (const auto &row : projection.rows()) {
        check(); if (row.entries.empty()) continue;
        size_t encodedSize = RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES;
        for (const auto &entry : row.entries) {
            check(); encodedSize = utils::detail::addStandardHeapBytes(encodedSize,
                utils::detail::addStandardHeapBytes(8U, CanonicalPropertyValueCodec::encodedSize(entry.value)));
        }
        utils::OwnedVector<std::byte> encoded{utils::OwnedPmrAllocator<std::byte>(conversion.memory)};
        if (encodedSize <= RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES) {
            const auto bytes = EntityPropertyCollectionRootCodec::encode(row);
            require(bytes.size() == encodedSize, "inverse ROW encoder size disagrees with preflight");
            encoded.assign(bytes.begin(), bytes.end());
        } else if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
            const EntityPropertyKey owner{conversion.owner_kind, row.owner_entity_id};
            const auto slot = hybridPropertySlotFor(owner);
            const bool hasHot = std::ranges::any_of(sourceMatrix->owners[slot], [](uint64_t word) { return word != 0; });
            if (!hasHot && (sourceMatrix->cold_rows.owners & (UINT64_C(1) << slot)))
                context.withColdBlock(group, *sourceMatrix, sourceMatrix->cold_rows.roots[slot],
                    [&](const auto &block, const auto &) {
                        const auto cold = block.row(slot);
                        require(cold && cold->field_count == row.entries.size(), "inverse complete cold root lost its actual fields");
                        if (!cold->large_value_reference) return;
                        HybridPropertyColdRowInspection inspection(source, *cold, owner, block.semantic_generation,
                            std::span(sourceMatrix->tokens).first(sourceMatrix->column_count), conversion.memory,
                            &largeValues, conversion.cancellation);
                        require(bool(inspection.reference()), "inverse complete cold carrier has no authenticated reference");
                        const auto bytes = EntityPropertyLargeValueReferenceCodec::encode(*inspection.reference());
                        encoded.assign(bytes.begin(), bytes.end());
                    });
        } else if (row.entries.size() == 1) {
            utils::OwnedHeapReservation referenceCredit(conversion.memory);
            const auto referenceAdmission = [&](size_t bytes) { referenceCredit.set(bytes); };
            const auto extension = [&](const HybridPropertyExtensionView &value) {
                if (!value.reference()) return;
                const EntityPropertyLargeValueReference reference{row.owner_entity_id, row.owner_entity_type, *value.reference()};
                const auto bytes = EntityPropertyLargeValueReferenceCodec::encode(reference);
                encoded.assign(bytes.begin(), bytes.end());
            };
            static_cast<void>(context.visitField({conversion.owner_kind, row.owner_entity_id}, row.entries[0].key_token,
                [](const auto &) {}, std::ref(extension), std::ref(admit), std::ref(referenceAdmission)));
            boundary.set(0);
        }
        if (encoded.empty()) {
            auto proposal = HybridPropertyLargeFieldPreparation::prepareRoot(*this, source, largeValues,
                {conversion.owner_kind, row.owner_entity_id}, row, LargeValueId(batch.candidate().next_large_value_id),
                conversion.memory, conversion.cancellation);
            const HybridPropertyLargeFieldPreparation *selected = &proposal;
            if (!proposal.reused()) {
                for (const auto &previous : largeRows) {
                    check(); if (std::ranges::equal(previous.payload(), proposal.payload())) { selected = &previous; break; }
                }
                if (selected == &proposal) {
                    if (largeRows.size() == largeRows.capacity()) largeRows.reserve(largeRows.empty() ? 1U : largeRows.size() * 2U);
                    require(batch.allocateLargeValueId() == proposal.reference().value_id,
                        "inverse ROW carrier lost its same-batch identity sequence");
                    largeRows.push_back(std::move(proposal)); selected = &largeRows.back();
                }
            }
            const EntityPropertyLargeValueReference reference{row.owner_entity_id, row.owner_entity_type, selected->reference()};
            const auto bytes = EntityPropertyLargeValueReferenceCodec::encode(reference);
            encoded.assign(bytes.begin(), bytes.end());
        }
        require(!encoded.empty() && encoded.size() <= RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES,
            "inverse ROW carrier exceeds its native slot capacity");
        if (images.empty() || images.back().page.stats().contiguous_free_bytes < encoded.size() + RSS_SLOTTED_PAGE_SLOT_BYTES) {
            if (newPools.empty() || images.size() % RSS_PAGE_POOL_DATA_FRAME_COUNT == 0) static_cast<void>(newPool(SegmentKind::SGK_PROPERTY_PAGE_POOL, conversion.field_pool_owner_id));
            const auto pool = newPools.back(); const auto pageId = batch.allocateLogicalPageId();
            const PageOwnerHeader owner{pageId, pool.owner_id, PageOwnerKind::POK_PROPERTY, 0,
                RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
            auto page = SlottedPage::create(owner);
            images.push_back({pool, {pageId, pool.segment_id, uint16_t(RSS_PAGE_POOL_FIRST_DATA_FRAME + images.size() % RSS_PAGE_POOL_DATA_FRAME_COUNT),
                pool.generation, 0}, std::move(page)});
        }
        auto &image = images.back(); const auto slot = image.page.insert(encoded);
        image.location.free_bytes = image.page.stats().contiguous_free_bytes;
        properties.push_back({{conversion.owner_kind, row.owner_entity_id}, slot.page_id, slot.slot_id, slot.generation});
    }
    // Allocate preserved hybrid frames only after ROW packing, so the two
    // layouts never share a packing cursor or an owner-kind header.
    std::optional<CatalogEntry> retainedPool;
    size_t retainedPoolFrames = 0;
    for (auto &field : retainedFields) {
        check();
        if (!retainedPool || retainedPoolFrames == RSS_PAGE_POOL_DATA_FRAME_COUNT) {
            retainedPool = newPool(SegmentKind::SGK_PROPERTY_PAGE_POOL, conversion.field_pool_owner_id);
            retainedPoolFrames = 0;
        }
        const auto pool = *retainedPool;
        const auto id = field.pageId();
        const PageOwnerHeader owner{id, pool.owner_id, PageOwnerKind::POK_HYBRID_PROPERTY, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
        auto page = std::move(field).bindPrivateOwner(owner, pool, batch.checkpointLsn());
        const auto freeBytes = page.stats().contiguous_free_bytes;
        images.push_back({pool, {id, pool.segment_id, uint16_t(RSS_PAGE_POOL_FIRST_DATA_FRAME + retainedPoolFrames++),
            pool.generation, freeBytes}, std::move(page)});
    }
    encodingScratch.set(0);
    HybridPropertyImmutablePreparation immutableImages(batch, largeValues, largeRows, conversion.memory, newPool, check);
    HybridPropertyContentIndexPreparation content(batch, source, conversion, *this, sourceTrees, largeValues, largeRows, newPool, check);
    std::sort(content.superseded_.begin(), content.superseded_.end());
    utils::OwnedVector<CatalogEntry> contentPools{utils::OwnedPmrAllocator<CatalogEntry>(conversion.memory)};
    for (const auto id : content.superseded_) {
        const auto route = locations_.findPage(current, PageId(id)); contentPools.push_back(catalog_.find(current, route.segment_id));
    }
    std::sort(contentPools.begin(), contentPools.end(), [](const auto &a, const auto &b) { return a.segment_id < b.segment_id; });
    contentPools.erase(std::unique(contentPools.begin(), contentPools.end(), [](const auto &a, const auto &b) {
        return a.segment_id == b.segment_id;
    }), contentPools.end());
    for (auto &pool : contentPools) {
        check(); require(pool.kind == SegmentKind::SGK_INDEX_PAGE_POOL && pool.owner_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID &&
            pool.state == SegmentState::SGS_SEALED && allocator_.isAllocated(current, pool.physical_slot),
            "inverse content COW has a foreign source pool");
        const auto header = SharedPagePoolAllocator::readPoolHeader(*io_, current.data_region_offset, pool); bool detached = true;
        for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
            check(); const auto owner = hybrid_property_detail::currentPoolFrameOwner(pool, header, frame,
                [&](uint8_t index) { return sourcePools.readPageOwner(pool, index); },
                [&](PageId id) { return locations_.findPageOptional(current, id); });
            if (owner) detached = detached && std::binary_search(content.superseded_.begin(), content.superseded_.end(), owner->page_id.value());
        }
        if (detached) {
            batch.reserveDataSlot(pool.physical_slot); pool.state = SegmentState::SGS_RETIRED; retiredPools.push_back(pool);
        }
    }
    std::sort(retiredPools.begin(), retiredPools.end(), [](const auto &a, const auto &b) { return a.segment_id < b.segment_id; });
    retiredIds.insert(retiredIds.end(), content.erased_.begin(), content.erased_.end());
    std::sort(retiredIds.begin(), retiredIds.end());
    retiredIds.erase(std::unique(retiredIds.begin(), retiredIds.end()), retiredIds.end());
    debug::PerfTrace::addValue("native_graph.hybrid.inverse_private_new_large_values", int64_t(largeRows.size()));
    debug::PerfTrace::addValue("native_graph.hybrid.inverse_private_row_frames", int64_t(images.size()));
    check();
    if (!conversion.publish_candidate)
        throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "inverse native ROW preparation completed; publication was not requested");
    utils::OwnedVector<PageLocation> published{utils::OwnedPmrAllocator<PageLocation>(conversion.memory)};
    published.reserve(images.size() + directoryImages.size() + content.images_.size() + immutableImages.frames_.size());
    for (const auto &pool : newPools) {
        check(); const auto inPool = [&](const auto &image) { return image.pool.segment_id == pool.segment_id; };
        const auto count = size_t(std::count_if(images.begin(), images.end(), inPool)) +
            size_t(std::count_if(content.images_.begin(), content.images_.end(), inPool)) +
            size_t(std::count_if(directoryImages.begin(), directoryImages.end(), inPool)) +
            size_t(std::count_if(immutableImages.frames_.begin(), immutableImages.frames_.end(), inPool));
        utils::OwnedHeapReservation payloadCredit(conversion.memory, count *
            (utils::detail::standardAllocatorRequestBytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, alignof(std::byte)) +
                3U * utils::detail::standardContainerProxyBytes()));
        utils::OwnedVector<SharedPagePoolPreparedAppend> appends{utils::OwnedPmrAllocator<SharedPagePoolPreparedAppend>(conversion.memory)};
        utils::OwnedVector<PageLocation> locations{utils::OwnedPmrAllocator<PageLocation>(conversion.memory)};
        appends.reserve(count); locations.reserve(count);
        for (const auto &image : images) {
            if (image.pool.segment_id != pool.segment_id) continue;
            check(); const auto bytes = image.page.serializeOwned(conversion.memory);
            SharedPagePoolPreparedAppend append;
            append.owner = {image.location.page_id, pool.owner_id, image.page.ownerKind(), 0,
                RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
            append.payload.assign(bytes.begin(), bytes.end()); append.free_bytes = image.location.free_bytes;
            appends.push_back(std::move(append)); auto location = image.location; location.page_index = 0; locations.push_back(location);
        }
        const auto emitIndex = [&](const auto &image) {
            if (!inPool(image)) return;
            check(); const auto bytes = image.page.slottedPage().serializeOwned(conversion.memory);
            SharedPagePoolPreparedAppend append;
            append.owner = {image.location.page_id, pool.owner_id, PageOwnerKind::POK_INDEX, 0,
                RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
            append.payload.assign(bytes.begin(), bytes.end()); append.free_bytes = image.location.free_bytes;
            appends.push_back(std::move(append)); auto location = image.location; location.page_index = 0; locations.push_back(location);
        };
        for (const auto &image : directoryImages) emitIndex(image);
        for (const auto &image : content.images_) emitIndex(image);
        for (const auto &frame : immutableImages.frames_) {
            if (!inPool(frame)) continue;
            check(); SharedPagePoolPreparedAppend append; append.owner = frame.owner;
            append.payload.assign(frame.payload.begin(), frame.payload.end()); appends.push_back(std::move(append));
            auto location = frame.location; location.page_index = 0; locations.push_back(location);
        }
        // Retained unrelated groups use their dedicated Hybrid pool cursor,
        // separate from the new ordinary ROW frames. Authenticate that complete
        // pool as Hybrid; passing the ordinary writer rejects its real owners.
        const bool native = !appends.empty() && appends.front().owner.kind == PageOwnerKind::POK_HYBRID_PROPERTY;
        require(!native || (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL &&
            std::ranges::all_of(appends, [](const auto &append) { return append.owner.kind == PageOwnerKind::POK_HYBRID_PROPERTY; })),
            "inverse retained Hybrid pool contains unrelated frame kinds");
        SharedPagePoolAllocator::prepareNewPoolPagesUnderWrite(*io_, current.data_region_offset, pool, appends, locations,
            batch.checkpointLsn(), 0, native);
        for (const auto &location : locations) published.push_back(location);
    }
    for (const auto &pool : newPools) allocator_.prepareSetAllocated(batch, pool.physical_slot, true);
    if (!newPools.empty()) catalog_.prepareUpsert(batch, newPools);
    std::sort(published.begin(), published.end(), [](const auto &a, const auto &b) { return a.page_id < b.page_id; });
    locations_.prepareReplacePages(batch, published, retiredIds);
    if (content.root_) changedRoots.push_back(*content.root_);
    std::sort(changedRoots.begin(), changedRoots.end(), [](const auto &a, const auto &b) { return a.index_id < b.index_id; });
    locations_.prepareReplaceIndexRoots(batch, changedRoots, erasedRoots);
    if (!immutableImages.manifests_.empty()) locations_.prepareReplaceLargeValueManifests(batch, immutableImages.manifests_, {});
    if (!properties.empty()) locations_.prepareReplaceEntityProperties(batch, properties, {});
    utils::OwnedVector<RetireQueueEntry> retirements{utils::OwnedPmrAllocator<RetireQueueEntry>(conversion.memory)};
    retirements.reserve(retiredPools.size());
    for (const auto &pool : retiredPools)
        retirements.push_back({pool.physical_slot, pool.segment_id, batch.generation(), RetireQueueDisposition::RQD_RETIRED_SEGMENT});
    std::sort(retirements.begin(), retirements.end(), [](const auto &left, const auto &right) { return left.physical_slot < right.physical_slot; });
    catalog_.prepareUpsert(batch, retiredPools); retirement_.prepareQueueInsertions(batch, retirements);
    if (std::ranges::any_of(descriptor.roots, [](const auto &root) { return root.page_id.isValid(); }))
        pages_.prepareHybridPropertyDescriptorImpl(batch, descriptor, true);
    else batch.setHybridPropertyDescriptorRoot({});
    batch.hybrid_property_publication_ = true;
    check(); publishBatch(batch, writer, options); return batch.candidate();
}
} // namespace graph::storage
