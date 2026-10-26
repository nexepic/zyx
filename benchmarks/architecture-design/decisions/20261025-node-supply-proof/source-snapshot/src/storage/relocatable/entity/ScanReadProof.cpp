/**
 * @file ScanReadProof.cpp
 * @brief Complete-root verification and bounded visitor-scoped scan authority.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "graph/storage/relocatable/entity/ScanReadProof.hpp"
#include "graph/storage/relocatable/metadata/LocationDirectory.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDescriptor.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include <algorithm>
#include <optional>

namespace graph::storage {
struct VerifiedScanRoot::State final {
    std::shared_ptr<StorageIO> io;
    const RelocatableMetadataStore *metadata = nullptr;
    Superblock root;
    std::optional<HybridPropertyDescriptor> descriptor;
    uint64_t io_epoch = UINT64_MAX;
};

class ScanBatchReadScope::Suspension final {
public:
    Suspension() : previous_(active_) { active_ = nullptr; }
    ~Suspension() { active_ = previous_; }
    Suspension(const Suspension &) = delete;
    Suspension &operator=(const Suspension &) = delete;
private:
    const ScanBatchReadScope *previous_;
};

VerifiedScanRoot VerifiedScanRoot::verifyComplete(std::shared_ptr<StorageIO> io,
    RelocatableMetadataStore &metadata,
    const std::shared_ptr<query::execution::MaterializedByteBudget> &workspace,
    const std::shared_ptr<std::pmr::memory_resource> &controlOwner,
    const CancellationCheckpoint *cancellation) {
#if !ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
    static_cast<void>(io); static_cast<void>(metadata); static_cast<void>(workspace);
    static_cast<void>(controlOwner); static_cast<void>(cancellation);
    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "verified scans require their isolated format");
#else
    constexpr size_t limit = 7U * 1024U * 1024U;
    if (cancellation) cancellation->check();
    if (!io || !metadata.usesDataStorage(io) || !workspace || workspace->used() > limit ||
        workspace->available() > limit - workspace->used() || !controlOwner)
        throw std::invalid_argument("verified scan root requires its store and bounded verification/control accounts");
    utils::OwnedHeapReservation controlCredit(controlOwner, sizeof(State) + 256U);
    if (!controlCredit.governed())
        throw std::invalid_argument("verified scan root requires an admitted control owner");
    utils::OwnedAllocationScope allocation(workspace);
    utils::OwnedHeapReservation frontier(workspace, 32U * 1024U);
    const ScanBatchReadScope::Suspension noInheritedAuthority;
    auto pin = metadata.openSnapshot();
    const auto root = pin.superblock();
    if (root.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING)
        throw FormatException(FormatError::FERR_INVALID_STATE, "BUILDING cannot grant complete scan read authority");
    const uint64_t epoch = io->dataMutationEpoch();
    if (epoch == UINT64_MAX)
        throw FormatException(FormatError::FERR_INVALID_STATE, "saturated I/O epoch cannot grant scan authority");
    std::optional<HybridPropertyDescriptor> descriptor;
    if (root.hybrid_property_descriptor_root.isValid()) {
        MetadataPageFile pages(io, root);
        descriptor = pages.readHybridPropertyDescriptor(root.hybrid_property_descriptor_root, root).descriptor;
    }
    if (!descriptor)
        throw FormatException(FormatError::FERR_INVALID_STATE, "complete scan authority requires its persistent descriptor");
    // This authority only covers Node membership, labels and native field
    // supply. Authenticate that complete supply chain using the same bounded
    // directory verifier as full integrity. It merges every primary Node and
    // complete label set with stored membership and validates all referenced
    // native fields. ROW relationships never receive this authority and retain
    // their ordinary read validation. Whole-graph integrity remains mandatory
    // for offline completion and available explicitly/on-open by option.
    SlottedPagePool pools(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    LargeValueStore values(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    HybridPropertyReadContext fields(io, pools, metadata, pin, cancellation);
    static_cast<void>(HybridPropertyDirectoryIntegrity::verify(fields, pin, workspace, &values));
    if (cancellation) cancellation->check();
    if (metadata.openSnapshot().superblock() != root ||
        io->dataMutationEpoch() != epoch)
        throw FormatException(FormatError::FERR_INVALID_STATE, "scan root changed during complete verification");
    auto state = std::allocate_shared<State>(utils::OwnedPmrAllocator<State>(controlOwner));
    state->io = std::move(io); state->metadata = &metadata; state->root = root;
    state->descriptor = descriptor; state->io_epoch = epoch;
    return VerifiedScanRoot(std::move(state));
#endif
}

bool VerifiedScanRoot::matches(const RelocatableMetadataStore &metadata, const StorageIO &io,
    const MetadataSnapshotPin &snapshot, const HybridPropertyDescriptor *descriptor) const {
    return RSS_SCAN_PATH_FORMAT_ENABLED && state_ && state_->metadata == &metadata && state_->io.get() == &io &&
        metadata.ownsSnapshot(snapshot) && snapshot.superblock() == state_->root &&
        state_->io_epoch != UINT64_MAX && io.dataMutationEpoch() == state_->io_epoch &&
        (!descriptor || (state_->descriptor && *state_->descriptor == *descriptor));
}

ScanBatchReadGrant::ScanBatchReadGrant(const VerifiedScanRoot &root, const MetadataSnapshotPin &snapshot,
    std::span<const uint64_t> selected, std::shared_ptr<query::execution::MaterializedByteBudget> sourceWorkspace)
    : frame_(sourceWorkspace, sizeof(ScanBatchReadGrant)), root_(root), snapshot_(snapshot),
      workspace_(std::move(sourceWorkspace)), count_(selected.size()) {
    if (!workspace_ || count_ > MAXIMUM_ROWS || !root_.state_ ||
        !root_.matches(*root_.state_->metadata, *root_.state_->io, snapshot_))
        throw FormatException(FormatError::FERR_INVALID_STATE, "scan grant lost its verified root or row bound");
    uint64_t previous = 0;
    for (size_t i = 0; i < selected.size(); ++i) {
        if (!selected[i] || selected[i] <= previous)
            throw std::invalid_argument("scan grant requires ordered unique selected IDs");
        selected_[i] = previous = selected[i];
    }
}

thread_local const ScanBatchReadScope *ScanBatchReadScope::active_ = nullptr;

ScanBatchReadScope::ScanBatchReadScope(const ScanBatchReadGrant &grant, std::span<const uint64_t> requested)
    : frame_(grant.workspace_, sizeof(ScanBatchReadScope)), grant_(grant), count_(requested.size()) {
    if (count_ > grant_.count_)
        throw std::invalid_argument("scan read exceeds its certified batch");
    uint64_t previous = 0;
    const auto end = grant_.selected_.begin() + grant_.count_;
    for (size_t i = 0; i < requested.size(); ++i) {
        if (!requested[i] || requested[i] <= previous ||
            !std::binary_search(grant_.selected_.begin(), end, requested[i]))
            throw std::invalid_argument("scan read has foreign, repeated or unordered IDs");
        requested_[i] = previous = requested[i];
    }
    dense_ = count_ && requested_[count_ - 1U] - requested_[0] == count_ - 1U;
    previous_ = active_; active_ = this;
}

ScanBatchReadScope::~ScanBatchReadScope() { active_ = previous_; }

void ScanBatchReadGrant::read(std::span<const uint64_t> requested, const std::function<void()> &consumer) const {
    if (!consumer) throw std::invalid_argument("scan read requires a synchronous consumer");
    const auto check = [&] {
        if (!root_.matches(*root_.state_->metadata, *root_.state_->io, snapshot_))
            throw FormatException(FormatError::FERR_INVALID_STATE, "scan read lost its original verified snapshot");
    };
    check();
    const ScanBatchReadScope scope(*this, requested);
    consumer(); check();
}

bool ScanBatchReadScope::covers(const RelocatableMetadataStore &metadata, const StorageIO &io,
    const MetadataSnapshotPin &snapshot, const HybridPropertyDescriptor &descriptor,
    std::span<const EntityPropertyKey> owners) {
    const auto *scope = active_;
    if (!scope || owners.empty() || owners.size() > scope->count_ ||
        !scope->grant_.root_.matches(metadata, io, snapshot, &descriptor) ||
        (&snapshot != &scope->grant_.snapshot_ &&
            !scope->grant_.root_.matches(metadata, io, scope->grant_.snapshot_, &descriptor))) return false;
    const auto end = scope->requested_.begin() + scope->count_;
    uint64_t previous = 0;
    for (const auto owner : owners) {
        if (owner.owner_kind != EntityPropertyOwnerKind::EPOK_NODE || !owner.entity_id || owner.entity_id <= previous ||
            (scope->dense_ ? owner.entity_id < scope->requested_[0] || owner.entity_id > scope->requested_[scope->count_ - 1U] :
                !std::binary_search(scope->requested_.begin(), end, owner.entity_id))) return false;
        previous = owner.entity_id;
    }
    return true;
}
} // namespace graph::storage
