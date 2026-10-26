#pragma once

#include <cerrno>
#include <array>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include <filesystem>
#include <system_error>
#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/format/ScanBuildingClone.hpp"
#include "graph/storage/relocatable/format/BuildingScanMaintenance.hpp"
#include "graph/storage/relocatable/format/BuildingScanCompletion.hpp"
#include "graph/storage/relocatable/entity/BuildingScanMembership.hpp"
#include "graph/storage/relocatable/entity/ScanMembershipDirectory.hpp"
#include "graph/storage/relocatable/entity/ScanMembershipSource.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/HybridPropertyGroupConversion.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"

namespace graph::storage {
    // Offline private-image construction. Only complete() can clear BUILDING
    // after fresh integrity and actual whole-file admission; public backend
    // eligibility remains a separate lifecycle decision.
    class ScanBuildingImageBuilder final {
    public:
        struct FieldConstructionCounts {
            uint64_t examined_groups = 0, converted_groups = 0, row_fallback_groups = 0;
        };
        ScanBuildingImageBuilder(const ScanBuildingImageBuilder &) = delete;
        ScanBuildingImageBuilder &operator=(const ScanBuildingImageBuilder &) = delete;
        ~ScanBuildingImageBuilder() {
            static_cast<void>(discardIfSourceChanged());
            metadata_.reset(); target_.reset(); source_.reset();
            if (target_handle_ != INVALID_FILE_HANDLE) static_cast<void>(portable_close_rw(target_handle_));
            if (source_handle_ != INVALID_FILE_HANDLE) static_cast<void>(portable_close(source_handle_));
        }

        // Source must be an offline, stable default-format image. Exclusive
        // creation never overwrites a destination. Failure may leave an invalid
        // or BUILDING artifact; neither this method nor its result opens it as
        // a complete graph. No temporary disk image is allocated.
        [[nodiscard]] static std::unique_ptr<ScanBuildingImageBuilder> cloneWithPrimaryMembership(
            const std::filesystem::path &sourcePath, const std::filesystem::path &destinationPath,
            const std::shared_ptr<query::execution::MaterializedByteBudget> &memory,
            uint64_t maximumTotalFileBytes, uint64_t fieldPoolOwner,
            const CancellationCheckpoint *cancellation = nullptr,
            const std::shared_ptr<std::pmr::memory_resource> &rawCacheMemory = {}) {
            if (!RSS_SCAN_PATH_FORMAT_ENABLED)
                throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "scan image builder requires the isolated format");
            constexpr size_t limit = 1024U * 1024U;
            if (!memory || memory->used() > limit || memory->available() > limit - memory->used() ||
                !fieldPoolOwner || !maximumTotalFileBytes || maximumTotalFileBytes > UINT64_C(2) * 1024U * 1024U * 1024U)
                throw std::invalid_argument("scan image builder requires bounded source and whole-file accounts");
            if (rawCacheMemory && !dynamic_cast<utils::UnmanagedAllocationResource *>(rawCacheMemory.get()))
                throw std::invalid_argument("offline raw page cache requires a governed allocation owner");
            const auto check = [&] { if (cancellation) cancellation->check(); };
            check();
            auto result = std::unique_ptr<ScanBuildingImageBuilder>(new ScanBuildingImageBuilder(memory));
            result->fixed_bytes_.set(sizeof(ScanBuildingImageBuilder) + sizeof(RelocatableMetadataStore) +
                2U * sizeof(StorageIO) + 16U * 1024U);
            utils::OwnedAllocationScope owner(memory);
            const size_t sourcePathUnits = sourcePath.native().size(), targetPathUnits = destinationPath.native().size();
            // Admit path conversion/copies and later cleanup paths before their
            // standard-library allocations. Excessively long paths refuse here.
            if (sourcePathUnits > limit / 64U || targetPathUnits > limit / 64U - sourcePathUnits)
                throw query::execution::MaterializedByteLimitExceeded();
            const size_t pathCredit = 64U * (sourcePathUnits + targetPathUnits + 2U);
            result->fixed_bytes_.set(sizeof(ScanBuildingImageBuilder) + sizeof(RelocatableMetadataStore) +
                2U * sizeof(StorageIO) + 16U * 1024U + pathCredit);
            result->source_path_ = sourcePath.string(); result->target_path_ = destinationPath.string();
            result->source_handle_ = portable_open(result->source_path_.c_str(), O_RDONLY);
            if (result->source_handle_ == INVALID_FILE_HANDLE)
                throw std::system_error(errno, std::generic_category(), "cannot open offline scan source");
            result->source_ = std::make_shared<StorageIO>(nullptr, INVALID_FILE_HANDLE, result->source_handle_,
                StorageIOAccessMode::SIOAM_READ_ONLY);
            result->source_bytes_ = result->source_->fileSize();
            result->maximum_total_bytes_ = maximumTotalFileBytes;
            if (result->source_bytes_ < RSS_FILE_BOOTSTRAP_BYTES || result->source_bytes_ > maximumTotalFileBytes / 2U)
                throw StorageFileLimitExceeded();
            result->source_identity_ = nativeIdentity(result->source_handle_);
            for (size_t i = 0; i < result->source_headers_.size(); ++i)
                if (result->source_->readAt(i * RSS_SUPERBLOCK_BYTES, result->source_headers_[i].data(),
                        RSS_SUPERBLOCK_BYTES) != RSS_SUPERBLOCK_BYTES)
                    throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL, "offline source headers are truncated");
            check();
            try {
            result->target_handle_ = portable_create_rw_exclusive(result->target_path_.c_str());
            if (result->target_handle_ == INVALID_FILE_HANDLE)
                throw std::system_error(errno, std::generic_category(), "cannot exclusively create private scan image");
            result->target_identity_ = nativeIdentity(result->target_handle_);
            const uint64_t targetLimit = maximumTotalFileBytes - result->source_bytes_;
            result->target_ = std::make_shared<StorageIO>(nullptr, result->target_handle_, result->target_handle_,
                StorageIOAccessMode::SIOAM_READ_WRITE, targetLimit);
            const auto cloned = scan_candidate_detail::cloneUnverifiedBuildingImage(*result->source_, *result->target_,
                *memory, maximumTotalFileBytes, check);
            // Slot IDs are one based. Only append beyond the copied source;
            // eventual reclamation/space admission is a separate required stage.
            const uint64_t first = (result->source_bytes_ - cloned.data_region_offset) / RSS_SEGMENT_SIZE + 1U;
            const uint64_t last = (targetLimit - cloned.data_region_offset) / RSS_SEGMENT_SIZE;
            if (first > last) throw StorageFileLimitExceeded();
            result->first_slot_ = PhysicalSlotId(first); result->last_slot_ = PhysicalSlotId(last);
            result->field_pool_owner_ = fieldPoolOwner;
            // Reuse the established raw-page pool within its frozen 64 MiB
            // allowance. Source/maintenance scratch remain separate accounts;
            // no decoded values or candidate-specific cache is introduced.
            if (rawCacheMemory) result->target_->enableBufferPool(64U * 1024U * 1024U, rawCacheMemory);
            result->metadata_ = RelocatableMetadataStore::open(result->target_, memory);
            result->counts_ = populatePrimaryMembership(result->target_, *result->metadata_, memory,
                PhysicalSlotId(first), PhysicalSlotId(last), fieldPoolOwner, cancellation);
            check();
            if (result->discardIfSourceChanged())
                throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed during private construction");
            return result;
            } catch (...) { static_cast<void>(result->discardIfSourceChanged()); throw; }
        }

        [[nodiscard]] FieldConstructionCounts constructFields(const CancellationCheckpoint *cancellation = nullptr,
            const std::shared_ptr<query::execution::MaterializedByteBudget> &integrityMemory = {}) {
            if (discardIfSourceChanged())
                throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed before field construction");
            if (fields_constructed_) throw std::logic_error("private image fields were already constructed");
            try {
            ensureImageIntegrity(integrityMemory ? integrityMemory : memory_, cancellation);
            auto counts = constructFieldsForBuilding(target_, *metadata_, memory_, first_slot_, last_slot_,
                field_pool_owner_, cancellation);
            if (discardIfSourceChanged())
                throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed during field construction");
            recordKnownRoot(buildingRoot());
            fields_constructed_ = true; return counts;
            } catch (...) { static_cast<void>(discardIfSourceChanged()); throw; }
        }

        // Bounded read-only preflight permits a whole ROW-group fallback before
        // entering a write transaction. No partial field publication is caught
        // and silently reclassified as fallback.
        [[nodiscard]] static FieldConstructionCounts constructFieldsForBuilding(
            const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
            const std::shared_ptr<query::execution::MaterializedByteBudget> &memory,
            PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, uint64_t fieldPoolOwner,
            const CancellationCheckpoint *cancellation = nullptr) {
#if !ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
            static_cast<void>(io); static_cast<void>(metadata); static_cast<void>(memory);
            static_cast<void>(firstSlot); static_cast<void>(lastSlot); static_cast<void>(fieldPoolOwner);
            static_cast<void>(cancellation);
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "scan field construction requires the isolated format");
#else
            constexpr size_t limit = 1024U * 1024U;
            if (!io || !metadata.usesDataStorage(io) || !memory || memory->used() > limit ||
                memory->available() > limit - memory->used() || !firstSlot.isValid() || lastSlot < firstSlot || !fieldPoolOwner)
                throw std::invalid_argument("scan field construction requires its store and bounded placement/account");
            const std::function<void()> check = [&] { if (cancellation) cancellation->check(); };
            check();
            utils::OwnedAllocationScope owner(memory);
            utils::OwnedHeapReservation frontier(memory, sizeof(EntityPageStore) + sizeof(SlottedPagePool) +
                sizeof(EntityLabelSetStore) + sizeof(LargeValueStore) + sizeof(PropertyCollectionRootStore) +
                sizeof(BTreePageStore) + sizeof(NativeGraphWritePlanner) + 16U * 1024U);
            EntityPageStore entities(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            SlottedPagePool pages(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            EntityLabelSetStore labels(io, metadata, pages, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            LargeValueStore values(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            PropertyCollectionRootStore properties(pages, values, metadata);
            BTreePageStore indexes(pages, metadata);
            DatabaseId database;
            {
                const auto snapshot = metadata.openSnapshot();
                requireBuilding(snapshot.superblock()); database = snapshot.superblock().database_id;
                HybridPropertyReadContext source(io, pages, metadata, snapshot, cancellation);
                static_cast<void>(HybridPropertyDirectoryIntegrity::verify(source, snapshot, memory, &values));
            }
            NativeGraphWritePlannerOptions options;
            options.first_slot = firstSlot; options.last_slot = lastSlot;
            options.property_roots = {firstSlot, lastSlot, fieldPoolOwner};
            NativeGraphWritePlanner planner(metadata, entities, labels, properties, indexes, options);
            FieldConstructionCounts counts;
            uint64_t after = 0;
            for (;;) {
                check();
                std::optional<EntityPropertyKey> next;
                uint64_t group = 0;
                bool eligible = false;
                {
                    const auto snapshot = metadata.openSnapshot();
                    requireBuilding(snapshot.superblock());
                    if (snapshot.superblock().database_id != database)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "private field source identity changed");
                    const bool exhausted = metadata.visitEntityPropertyLocationsAfter(snapshot, after,
                        (std::numeric_limits<size_t>::max)(), [&](const EntityPropertyLocation &location) {
                            check(); next = location.key; return false;
                        });
                    if (!next) {
                        if (!exhausted) throw FormatException(FormatError::FERR_INVALID_STATE, "field discovery made no progress");
                        break;
                    }
                    group = hybridPropertyGroupFor(*next);
                    debug::PerfTrace::addValue("native_graph.scan_build.field_group_preflight", 1);
                    eligible = preflightFields(snapshot, *next, group, entities, properties, memory, check);
                }
                ++counts.examined_groups;
                if (eligible) {
                    auto transaction = planner.beginTransaction();
                    const auto before = transaction.baseSuperblock().generation;
                    transaction.stageHybridPropertyConversion(next->owner_kind, group, memory, cancellation, true,
                        NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, true, {}, 1U, true);
                    const auto result = transaction.commit();
                    if (!result.committed())
                        throw FormatException(FormatError::FERR_INVALID_STATE, "private field construction conflicted");
                    if (result.superblock.generation == before)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "eligible private field group was not published");
                    requireBuilding(result.superblock); ++counts.converted_groups;
                    debug::PerfTrace::addValue("native_graph.scan_build.field_groups_converted", 1);
                } else {
                    ++counts.row_fallback_groups;
                    debug::PerfTrace::addValue("native_graph.scan_build.field_groups_row_fallback", 1);
                }
                constexpr uint64_t maximumOwner = (UINT64_C(1) << 63U) - 1U;
                const auto last = (std::min)(group * 64U, maximumOwner);
                after = EntityPropertyKey{next->owner_kind, last}.directoryId();
                if (after == UINT64_MAX) break;
            }
            check(); return counts;
#endif
        }

        // One existing metadata-pool reclamation publication at most. Native
        // logical field/member roots and BUILDING authority remain unchanged.
        [[nodiscard]] MetadataPagePoolReclamation reclaimMetadataOnce(
            const std::shared_ptr<query::execution::MaterializedByteBudget> &maintenanceMemory,
            const CancellationCheckpoint *cancellation = nullptr, const MetadataPublishOptions &options = {}) {
            requireMaintenanceMemory(maintenanceMemory);
            if (discardIfSourceChanged())
                throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed before metadata reclamation");
            try {
                ensureImageIntegrity(maintenanceMemory, cancellation);
                const auto root = buildingRoot();
                if (root.checkpoint_lsn.value() == UINT64_MAX)
                    throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "private maintenance LSN overflow");
                BuildingScanMaintenanceRequest request;
                request.expected_database_id = root.database_id; request.expected_generation = root.generation;
                request.expected_descriptor = root.hybrid_property_descriptor_root;
                request.memory = maintenanceMemory; request.cancellation = cancellation;
                auto result = metadata_->reclaimOneBuildingScanMetadataPool(request,
                    LogSequenceNumber(root.checkpoint_lsn.value() + 1U), options);
                if (discardIfSourceChanged())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed during metadata reclamation");
                recordKnownRoot(result.superblock);
                return result;
            } catch (...) { static_cast<void>(discardIfSourceChanged()); throw; }
        }

        // Evacuate at most the highest registered metadata pool, using existing
        // routed-page and bootstrap refresh coordinators. Unsupported physical
        // roots remain BUILDING rather than invoking a whole-directory clone.
        [[nodiscard]] std::optional<Superblock> compactMetadataPoolOnce(
            const std::shared_ptr<query::execution::MaterializedByteBudget> &maintenanceMemory,
            const CancellationCheckpoint *cancellation = nullptr, const MetadataPublishOptions &options = {}) {
            requireMaintenanceMemory(maintenanceMemory);
            if (cancellation) cancellation->check();
            utils::OwnedAllocationScope owner(maintenanceMemory);
            utils::OwnedHeapReservation frontier(maintenanceMemory, 16U * 1024U);
            if (discardIfSourceChanged())
                throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed before metadata compaction");
            try {
                ensureImageIntegrity(maintenanceMemory, cancellation);
                Superblock root;
                std::optional<PhysicalSlotId> selected;
                {
                    auto pin = metadata_->openSnapshot(); root = pin.superblock();
                    metadata_->visitCatalogEntries(pin, [&](const CatalogEntry &entry) {
                        if (cancellation) cancellation->check();
                        if (entry.kind == SegmentKind::SGK_METADATA_PAGE_POOL && entry.state == SegmentState::SGS_SEALED &&
                            (!selected || entry.physical_slot > *selected)) selected = entry.physical_slot;
                    });
                }
                if (!selected) return {};
                BuildingScanMaintenanceRequest request;
                request.expected_database_id = root.database_id; request.expected_generation = root.generation;
                request.expected_descriptor = root.hybrid_property_descriptor_root; request.evacuate_pool = *selected;
                request.memory = maintenanceMemory; request.cancellation = cancellation;
                const auto result = metadata_->reclaimOneBuildingScanMetadataPool(request, nextLsn(root), options);
                if (discardIfSourceChanged())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed during metadata compaction");
                recordKnownRoot(result.superblock);
                if (result.superblock == root) return {};
                return result.superblock;
            } catch (...) { static_cast<void>(discardIfSourceChanged()); throw; }
        }

        struct RetiredPoolStep {
            Superblock root;
            bool pending = false, released = false, blocked = false;
        };
        // Consumes one durable retired-pool identity through the existing
        // Catalog/allocator/retirement protocol. Discovery pins drain first.
        [[nodiscard]] RetiredPoolStep releaseRetiredPoolOnce(
            const std::shared_ptr<query::execution::MaterializedByteBudget> &maintenanceMemory,
            const CancellationCheckpoint *cancellation = nullptr) {
            requireMaintenanceMemory(maintenanceMemory);
            if (discardIfSourceChanged())
                throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed before pool release");
            try {
                ensureImageIntegrity(maintenanceMemory, cancellation);
                utils::OwnedAllocationScope owner(maintenanceMemory);
                utils::OwnedHeapReservation frontier(maintenanceMemory, 16U * 1024U);
                const auto check = [&] { if (cancellation) cancellation->check(); };
                check();
                std::optional<CatalogSegmentRetirementRequest> request;
                RetireQueueDisposition disposition = RetireQueueDisposition::RQD_RETIRED_SEGMENT;
                Superblock root;
                {
                    const auto pin = metadata_->openSnapshot(); root = pin.superblock(); requireBuilding(root);
                    metadata_->visitRetireQueueEntries(pin, [&](const RetireQueueEntry &entry) {
                        check();
                        if (request) return;
                        const auto pool = metadata_->findCatalog(pin, entry.segment_id);
                        request = CatalogSegmentRetirementRequest{pool.segment_id, pool.generation};
                        disposition = entry.disposition;
                    });
                }
                if (!request) return {root, false, false, false};
                check();
                std::optional<Superblock> released;
                if (disposition == RetireQueueDisposition::RQD_RETIRED_SEGMENT)
                    released = metadata_->tryReleaseRetiredCatalogSegments(std::span(&*request, 1U), nextLsn(root));
                else if (disposition == RetireQueueDisposition::RQD_RELOCATED_SOURCE)
                    released = metadata_->tryReleaseRelocatedCatalogSource(request->segment_id, request->expected_generation, nextLsn(root));
                else throw FormatException(FormatError::FERR_INVALID_ENUM, "private image has an unknown retirement disposition");
                if (discardIfSourceChanged())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed during pool release");
                if (!released) return {root, true, false, true};
                recordKnownRoot(*released);
                return {*released, true, true, false};
            } catch (...) { static_cast<void>(discardIfSourceChanged()); throw; }
        }

        // One physical data pool is copied to a lower free slot. Unsupported
        // mixed ROW/native pools remain in place for the final whole-file gate.
        [[nodiscard]] std::optional<Superblock> compactDataPoolOnce(
            const std::shared_ptr<query::execution::MaterializedByteBudget> &maintenanceMemory,
            const CancellationCheckpoint *cancellation = nullptr) {
            requireMaintenanceMemory(maintenanceMemory);
            if (discardIfSourceChanged())
                throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed before data compaction");
            try {
                ensureImageIntegrity(maintenanceMemory, cancellation);
                utils::OwnedAllocationScope owner(maintenanceMemory);
                utils::OwnedHeapReservation frontier(maintenanceMemory, 32U * 1024U);
                const auto check = [&] { if (cancellation) cancellation->check(); };
                check();
                std::optional<CatalogEntry> selected;
                PhysicalSlotId hole;
                Superblock root;
                {
                    const auto pin = metadata_->openSnapshot(); root = pin.superblock(); requireBuilding(root);
                    uint64_t upper = UINT64_MAX;
                    for (;;) {
                        selected.reset();
                        metadata_->visitCatalogEntries(pin, [&](const CatalogEntry &entry) {
                            check();
                            const bool kind = entry.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL ||
                                entry.kind == SegmentKind::SGK_INDEX_PAGE_POOL || entry.kind == SegmentKind::SGK_ENTITY_PAGE_POOL;
                            if (kind && entry.state == SegmentState::SGS_SEALED && entry.physical_slot.value() < upper &&
                                (!selected || selected->physical_slot < entry.physical_slot)) selected = entry;
                        });
                        if (!selected) return std::nullopt;
                        hole = {};
                        for (uint64_t slot = 1U; slot < selected->physical_slot.value(); ++slot) {
                            check();
                            if (!metadata_->isPhysicalSlotAllocated(pin, PhysicalSlotId(slot))) { hole = PhysicalSlotId(slot); break; }
                        }
                        if (!hole.isValid()) return std::nullopt;
                        bool supported = root.hybrid_property_descriptor_root.isValid() ||
                            selected->kind == SegmentKind::SGK_INDEX_PAGE_POOL;
                        if (selected->kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && supported) {
                            // Discovery classifies current owner headers only.
                            // The typed relocation coordinator reauthenticates
                            // the pool control and every frame before any write.
                            const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(root.data_region_offset,
                                selected->physical_slot);
                            for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
                                check();
                                std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> bytes{};
                                if (target_->readAt(offset + uint64_t(frame) * RSS_PAGE_SIZE, bytes.data(), bytes.size()) != bytes.size()) continue;
                                std::optional<PageOwnerHeader> owner;
                                try { owner = PageOwnerHeaderCodec::decode(bytes); }
                                catch (const FormatException &) { continue; }
                                const auto route = metadata_->findPageLocationOptional(pin, owner->page_id);
                                if (route && route->segment_id == selected->segment_id && route->page_index == frame &&
                                    owner->kind != PageOwnerKind::POK_HYBRID_PROPERTY) supported = false;
                            }
                        }
                        if (selected->kind == SegmentKind::SGK_INDEX_PAGE_POOL)
                            supported = supported && metadata_->findIndexRootOptional(pin, selected->owner_id).has_value();
                        if (supported) break;
                        upper = selected->physical_slot.value();
                    }
                }
                // Prepaid child capacity is governed by the maintenance caller,
                // rather than adding an independent source allowance.
                utils::OwnedHeapReservation grant(maintenanceMemory, 1024U * 1024U);
                auto block = std::make_shared<query::execution::MaterializedByteBudget>(1024U * 1024U);
                NativeHybridPropertyPoolRelocation request{root.database_id, root.generation,
                    root.hybrid_property_descriptor_root, selected->segment_id, hole, hole, block, cancellation};
                Superblock moved;
                if (selected->kind == SegmentKind::SGK_PROPERTY_PAGE_POOL)
                    moved = metadata_->relocateHybridPropertyFieldPool(request, nextLsn(root));
                else if (selected->kind == SegmentKind::SGK_INDEX_PAGE_POOL)
                    moved = metadata_->relocateHybridPropertyIndexPool(request, nextLsn(root));
                else moved = metadata_->relocateHybridPropertyEntityPool(request, nextLsn(root));
                if (block->used()) throw std::logic_error("private pool relocation retained its prepaid block workspace");
                if (discardIfSourceChanged())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed during data compaction");
                recordKnownRoot(moved);
                return moved;
            } catch (...) { static_cast<void>(discardIfSourceChanged()); throw; }
        }

        [[nodiscard]] RelocatableTailTruncationResult truncateReleasedTailOnce(
            const std::shared_ptr<query::execution::MaterializedByteBudget> &maintenanceMemory,
            const CancellationCheckpoint *cancellation = nullptr) {
            requireMaintenanceMemory(maintenanceMemory);
            if (discardIfSourceChanged())
                throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed before tail truncation");
            try {
                ensureImageIntegrity(maintenanceMemory, cancellation);
                utils::OwnedAllocationScope owner(maintenanceMemory);
                utils::OwnedHeapReservation frontier(maintenanceMemory, 16U * 1024U);
                if (cancellation) cancellation->check();
                const auto result = metadata_->truncateReleasedTail();
                if (discardIfSourceChanged())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "offline source changed during tail truncation");
                recordKnownRoot(buildingRoot()); return result;
            } catch (...) { static_cast<void>(discardIfSourceChanged()); throw; }
        }

        // A fresh full scan is mandatory even when physical maintenance retained
        // a known generation. The certificate expires on any target I/O mutation.
        [[nodiscard]] Superblock complete(
            const std::shared_ptr<query::execution::MaterializedByteBudget> &maintenanceMemory,
            const CancellationCheckpoint *cancellation = nullptr, const MetadataPublishOptions &options = {}) {
            requireMaintenanceMemory(maintenanceMemory);
            if (cancellation) cancellation->check();
            if (completed_ || !fields_constructed_ || !source_integrity_generation_.isValid())
                throw std::logic_error("scan completion requires constructed, validated private fields");
            const auto stable = [&] {
                if (!sourceStable() || !pathHasIdentity(target_path_, target_identity_))
                    throw FormatException(FormatError::FERR_INVALID_STATE, "offline files changed before scan completion");
            };
            try {
                stable();
                const auto root = buildingRoot(); requireBuilding(root);
                const auto epoch = target_->dataMutationEpoch();
                const auto verified = RelocatableIntegrityScanner(target_, *metadata_, maintenanceMemory).scan(cancellation);
                if (verified.generation != root.generation || buildingRoot() != root ||
                    epoch == UINT64_MAX || target_->dataMutationEpoch() != epoch)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "private image changed during final validation");
                BuildingScanCompletionRequest request;
                request.database = root.database_id; request.generation = root.generation;
                request.source_integrity_generation = source_integrity_generation_;
                request.descriptor = root.hybrid_property_descriptor_root; request.io_epoch = epoch;
                request.source_bytes = source_bytes_; request.maximum_total_bytes = maximum_total_bytes_;
                request.memory = maintenanceMemory; request.cancellation = cancellation;
                request.require_stable_files = stable;
                try {
                    const auto ready = metadata_->completeBuildingScanImage(request, nextLsn(root), options);
                    stable(); completed_ = true; return ready;
                } catch (...) {
                    stable();
                    // Publication or its post-publication reconciliation can
                    // report failure after READY is durable. Preserve that
                    // independently recoverable image on every exception path.
                    const auto recovered = buildingRoot();
                    if (!(recovered.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING) &&
                        recovered.database_id == root.database_id &&
                        recovered.generation.value() == root.generation.value() + 1U &&
                        recovered.checkpoint_lsn == nextLsn(root) &&
                        recovered.hybrid_property_descriptor_root == root.hybrid_property_descriptor_root)
                        completed_ = true;
                    throw;
                }
            } catch (...) { static_cast<void>(discardIfSourceChanged()); throw; }
        }

        [[nodiscard]] Superblock buildingRoot() const {
            if (!metadata_) throw std::logic_error("private image was discarded");
            return metadata_->openSnapshot().superblock();
        }
        [[nodiscard]] scan_candidate_detail::MembershipSourceCounts membershipCounts() const { return counts_; }
        [[nodiscard]] StorageIOStats sourceStats() const { return source_->stats(); }
        [[nodiscard]] uint64_t sourceFileBytes() const { return source_bytes_; }
        [[nodiscard]] uint64_t targetFileBytes() const {
            if (!target_) throw std::logic_error("private image was discarded");
            return target_->fileSize();
        }

        [[nodiscard]] static scan_candidate_detail::MembershipSourceCounts populatePrimaryMembership(
            const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
            const std::shared_ptr<query::execution::MaterializedByteBudget> &memory,
            PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, uint64_t fieldPoolOwner,
            const CancellationCheckpoint *cancellation = nullptr) {
#if !ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
            static_cast<void>(io); static_cast<void>(metadata); static_cast<void>(memory);
            static_cast<void>(firstSlot); static_cast<void>(lastSlot); static_cast<void>(fieldPoolOwner);
            static_cast<void>(cancellation);
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                "BUILDING image membership construction requires the isolated format");
#else
            constexpr size_t limit = 1024U * 1024U;
            if (!io || !metadata.usesDataStorage(io) || !memory || memory->used() > limit ||
                memory->available() > limit - memory->used() || !firstSlot.isValid() ||
                lastSlot < firstSlot || !fieldPoolOwner)
                throw std::invalid_argument("BUILDING image construction requires its store, slots and bounded account");
            const auto check = [&] { if (cancellation) cancellation->check(); };
            check();
            utils::OwnedAllocationScope allocationOwner(memory);
            utils::OwnedHeapReservation frontier(memory, 32U * 1024U);
            DatabaseId database;
            {
                const auto snapshot = metadata.openSnapshot();
                const auto &root = snapshot.superblock();
                if (!(root.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING) ||
                    root.hybrid_property_descriptor_root.isValid())
                    throw FormatException(FormatError::FERR_INVALID_STATE,
                        "membership construction requires a fresh ROW-only BUILDING clone");
                database = root.database_id;
            }
            EntityPageStore entities(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            uint64_t after = 0;
            for (;;) {
                check();
                BuildingScanMembershipRequest request;
                bool exhausted = false;
                {
                    // Release each discovery pin before publishing a group.
                    // Keeping the initial pin for the whole build would retain
                    // every retired intermediate membership-directory version.
                    const auto snapshot = metadata.openSnapshot();
                    const auto &root = snapshot.superblock();
                    if (!(root.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING) || root.database_id != database)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "BUILDING image identity changed");
                    request.expected_database_id = database;
                    request.expected_generation = root.generation;
                    request.expected_descriptor = root.hybrid_property_descriptor_root;
                    exhausted = entities.visitNodesAfter(snapshot, after, 1U,
                        (std::numeric_limits<size_t>::max)(),
                        [&](uint64_t id, const NodeEntityRecord &) {
                            check(); request.group_id = 1U + (id - 1U) / 64U; return false;
                        });
                    if (request.group_id && root.checkpoint_lsn.value() == UINT64_MAX)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "BUILDING checkpoint LSN is exhausted");
                    if (request.group_id) {
                        request.first_slot = firstSlot; request.last_slot = lastSlot;
                        request.field_pool_owner_id = fieldPoolOwner;
                        request.memory = memory; request.cancellation = cancellation;
                    }
                }
                if (!request.group_id) {
                    if (!exhausted)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "BUILDING discovery made no progress");
                    break;
                }
                check();
                // The coordinator re-reads every primary ID and its complete
                // labels; discovery does not supply membership facts.
                const auto current = metadata.openSnapshot().superblock();
                static_cast<void>(metadata.installBuildingScanMembershipGroup(request,
                    LogSequenceNumber(current.checkpoint_lsn.value() + 1U)));
                check();
                if (request.group_id == ScanMembershipDirectoryCodec::MAX_GROUP) break;
                after = request.group_id * 64U;
            }
            const auto snapshot = metadata.openSnapshot();
            const auto &root = snapshot.superblock();
            if (!(root.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING))
                throw FormatException(FormatError::FERR_INVALID_STATE, "membership construction lost BUILDING protection");
            if (!root.hybrid_property_descriptor_root.isValid()) return {};
            MetadataPageFile pages(io, root);
            const auto descriptor = pages.readHybridPropertyDescriptor(root.hybrid_property_descriptor_root, root).descriptor;
            return {descriptor.scan_live_node_count, descriptor.scan_membership_group_count};
#endif
        }
    private:
        static void requireMaintenanceMemory(const std::shared_ptr<query::execution::MaterializedByteBudget> &memory) {
            constexpr size_t limit = 7U * 1024U * 1024U;
            if (!memory || memory->used() > limit || memory->available() > limit - memory->used())
                throw std::invalid_argument("private maintenance plus its 1 MiB source must fit 8 MiB");
        }
        [[nodiscard]] static LogSequenceNumber nextLsn(const Superblock &root) {
            if (root.checkpoint_lsn.value() == UINT64_MAX)
                throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "private image LSN overflow");
            return LogSequenceNumber(root.checkpoint_lsn.value() + 1U);
        }
        void recordKnownRoot(const Superblock &root) {
            requireBuilding(root);
            known_generation_ = root.generation; known_io_epoch_ = target_->dataMutationEpoch();
        }
        void ensureImageIntegrity(const std::shared_ptr<query::execution::MaterializedByteBudget> &memory,
            const CancellationCheckpoint *cancellation) {
            requireMaintenanceMemory(memory);
            if (cancellation) cancellation->check();
            const auto root = buildingRoot(); requireBuilding(root);
            const auto epoch = target_->dataMutationEpoch();
            if (source_integrity_generation_.isValid() && epoch != UINT64_MAX &&
                known_generation_ == root.generation && known_io_epoch_ == epoch) return;
            const auto verified = RelocatableIntegrityScanner(target_, *metadata_, memory).scan(cancellation);
            if (metadata_->openSnapshot().superblock().generation != verified.generation || target_->dataMutationEpoch() != epoch)
                throw FormatException(FormatError::FERR_INVALID_STATE, "private image changed during full integrity validation");
            if (!source_integrity_generation_.isValid()) source_integrity_generation_ = verified.generation;
            recordKnownRoot(root);
        }
        Generation known_generation_{};
        uint64_t known_io_epoch_ = UINT64_MAX;
        using FileIdentity = std::array<uint64_t, 2>;
        [[nodiscard]] static FileIdentity nativeIdentity(file_handle_t handle) {
#ifdef _WIN32
            BY_HANDLE_FILE_INFORMATION info{};
            if (!::GetFileInformationByHandle(reinterpret_cast<HANDLE>(handle), &info))
                throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), "cannot identify offline file");
            return {info.dwVolumeSerialNumber, (uint64_t(info.nFileIndexHigh) << 32U) | info.nFileIndexLow};
#else
            struct stat info{};
            if (::fstat(handle, &info) != 0)
                throw std::system_error(errno, std::generic_category(), "cannot identify offline file");
            return {static_cast<uint64_t>(info.st_dev), static_cast<uint64_t>(info.st_ino)};
#endif
        }
        [[nodiscard]] static bool pathHasIdentity(const std::string &path, const FileIdentity &expected) noexcept {
            const auto handle = portable_open(path.c_str(), O_RDONLY);
            if (handle == INVALID_FILE_HANDLE) return false;
            bool same = false;
            try { same = nativeIdentity(handle) == expected; } catch (...) {}
            static_cast<void>(portable_close(handle));
            return same;
        }
        [[nodiscard]] bool sourceStable() const noexcept {
            try {
                if (!pathHasIdentity(source_path_, source_identity_) || source_->fileSize() != source_bytes_) return false;
                SuperblockPair::Slots headers{};
                for (size_t i = 0; i < headers.size(); ++i)
                    if (source_->readAt(i * RSS_SUPERBLOCK_BYTES, headers[i].data(), RSS_SUPERBLOCK_BYTES) != RSS_SUPERBLOCK_BYTES)
                        return false;
                return headers == source_headers_;
            } catch (...) { return false; }
        }
        // Only the exclusively created file may be deleted. A replacement at
        // the target pathname belongs to its new owner and is left untouched.
        [[nodiscard]] bool discardIfSourceChanged() noexcept {
            if (completed_) return false;
            if (source_invalidated_) return true;
            if (!source_ || sourceStable()) return false;
            source_invalidated_ = true;
            const bool removeTarget = target_handle_ != INVALID_FILE_HANDLE &&
                pathHasIdentity(target_path_, target_identity_);
            metadata_.reset(); target_.reset();
            if (target_handle_ != INVALID_FILE_HANDLE) {
                static_cast<void>(portable_close(target_handle_)); target_handle_ = INVALID_FILE_HANDLE;
            }
            if (removeTarget) {
                std::error_code ignored; std::filesystem::remove(target_path_, ignored);
            }
            return true;
        }
        std::string source_path_, target_path_;
        FileIdentity source_identity_{}, target_identity_{};
        SuperblockPair::Slots source_headers_{};
        bool source_invalidated_ = false;
        friend class ScanBuildingImageBuilderTestAccess;
        static void requireBuilding(const Superblock &root) {
            if (!(root.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING))
                throw FormatException(FormatError::FERR_INVALID_STATE, "field construction requires a private BUILDING image");
        }
        static bool preflightFields(const MetadataSnapshotPin &snapshot, const EntityPropertyKey &first,
            uint64_t group, EntityPageStore &entities, PropertyCollectionRootStore &properties,
            const std::shared_ptr<query::execution::MaterializedByteBudget> &memory,
            const std::function<void()> &check) {
            bool checkpointFailed = false;
            const std::function<void()> checkpoint = [&] {
                try { check(); } catch (...) { checkpointFailed = true; throw; }
            };
            try {
                // Reserve writer-only work before decoding. A refusal here has
                // performed no writes and can safely retain the original ROW.
                utils::OwnedHeapReservation writerFrontier(memory, 64U * 1024U);
                detail::PropertyDecodeWorkspace workspace(memory, &checkpoint);
                std::array<uint64_t, 64> ids{};
                std::array<EntityPropertyKey, 64> owners{}; size_t ownerCount = 0;
                const uint64_t begin = (group - 1U) * 64U + 1U;
                constexpr uint64_t maximumOwner = (UINT64_C(1) << 63U) - 1U;
                const auto width = (std::min)(uint64_t{64}, maximumOwner - begin + 1U);
                for (size_t i = 0; i < width; ++i) ids[i] = begin + i;
                {
                    utils::OwnedHeapReservation buffers(memory);
                    const auto admit = [&](size_t bytes) { buffers.set(bytes); };
                    if (first.owner_kind == EntityPropertyOwnerKind::EPOK_NODE) {
                        const auto records = entities.readNodesSorted(snapshot, std::span(ids).first(width), admit);
                        for (size_t i = 0; i < width; ++i) if (records[i]) owners[ownerCount++] = {first.owner_kind, ids[i]};
                    } else {
                        const auto records = entities.readEdgesSorted(snapshot, std::span(ids).first(width), admit);
                        for (size_t i = 0; i < width; ++i) if (records[i]) owners[ownerCount++] = {first.owner_kind, ids[i]};
                    }
                }
                if (!ownerCount)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "ROW property group has no primary owner");
                size_t memberships = 0;
                for (const auto &key : std::span(owners).first(ownerCount)) {
                    checkpoint(); const auto count = properties.readEntityPropertyCount(snapshot, key).value_or(0);
                    if (count > HybridPropertyGroupConversion::MAX_MEMBERSHIPS - memberships) {
                        debug::PerfTrace::addValue("native_graph.scan_build.field_source_membership_refusal", 1); return false;
                    }
                    memberships += count;
                }
                auto roots = utils::OwnedVector<PropertyCollectionRoot>(utils::OwnedPmrAllocator<PropertyCollectionRoot>(memory));
                roots.reserve(ownerCount);
                utils::OwnedHeapReservation slots(memory); size_t retained = 0;
                const SelectedPropertyReadAdmission admitSlots = [&](size_t bytes) {
                    if (retained > (std::numeric_limits<size_t>::max)() - 4096U ||
                        bytes > (std::numeric_limits<size_t>::max)() - retained - 4096U)
                        throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "field preflight source slots overflow");
                    slots.set(retained + bytes + 4096U);
                };
                auto cursor = properties.openScanCursor(snapshot);
                for (const auto &key : std::span(owners).first(ownerCount)) {
                    checkpoint();
                    auto row = cursor.readEntityOptional(snapshot, key, nullptr, &admitSlots).value_or(PropertyCollectionRoot{
                        key.entity_id, key.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId, {}});
                    retained += utils::detail::standardContainerProxyBytes() + utils::detail::standardAllocatorRequestBytes(
                        row.entries.capacity() * sizeof(PropertyCollectionRootEntry), alignof(PropertyCollectionRootEntry));
                    roots.push_back(std::move(row)); slots.set(retained);
                }
                HybridPropertyGroupConversion plan(memory, roots, first.owner_kind, group,
                    snapshot.superblock().generation, RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, checkpoint);
                debug::PerfTrace::addValue("native_graph.scan_build.field_group_typed_columns", int64_t(plan.columns().size()));
                if (plan.columns().size() > 64U) {
                    debug::PerfTrace::addValue("native_graph.scan_build.field_typed_bound_refusal", 1); return false;
                }
                return true;
            } catch (const query::execution::MaterializedByteLimitExceeded &) {
                if (checkpointFailed) throw;
                debug::PerfTrace::addValue("native_graph.scan_build.field_source_workspace_refusal", 1); return false;
            } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                if (checkpointFailed) throw;
                debug::PerfTrace::addValue("native_graph.scan_build.field_source_workspace_refusal", 1); return false;
            }
        }
        explicit ScanBuildingImageBuilder(std::shared_ptr<query::execution::MaterializedByteBudget> memory)
            : memory_(std::move(memory)), fixed_bytes_(memory_.get()) {}
        std::shared_ptr<query::execution::MaterializedByteBudget> memory_;
        query::execution::MaterializedByteReservation fixed_bytes_;
        file_handle_t source_handle_ = INVALID_FILE_HANDLE, target_handle_ = INVALID_FILE_HANDLE;
        std::shared_ptr<StorageIO> source_, target_;
        std::unique_ptr<RelocatableMetadataStore> metadata_;
        scan_candidate_detail::MembershipSourceCounts counts_;
        uint64_t source_bytes_ = 0, maximum_total_bytes_ = 0;
        PhysicalSlotId first_slot_{}, last_slot_{};
        uint64_t field_pool_owner_ = 0;
        Generation source_integrity_generation_{};
        bool fields_constructed_ = false;
        bool completed_ = false;
    };
}
