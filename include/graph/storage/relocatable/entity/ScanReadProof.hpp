#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <span>
#include <utility>
#include "graph/utils/OwnedHeapReservation.hpp"

namespace graph::query::execution { class MaterializedByteBudget; }
namespace graph::storage {
    class StorageIO;
    class RelocatableMetadataStore;
    class MetadataSnapshotPin;
    class CancellationCheckpoint;
    struct HybridPropertyDescriptor;
    struct EntityPropertyKey;
    class CertifiedScanMembership;
    class ScanBatchReadGrant;
    class ScanBatchReadScope;

    // Semantic read authority only. Verification cannot publish an image,
    // authorize adoption, or bypass the producer's whole-file admission.
    class VerifiedScanRoot final {
    public:
        [[nodiscard]] static VerifiedScanRoot verifyComplete(std::shared_ptr<StorageIO> io,
            RelocatableMetadataStore &metadata,
            const std::shared_ptr<query::execution::MaterializedByteBudget> &workspace,
            const std::shared_ptr<std::pmr::memory_resource> &controlOwner,
            const CancellationCheckpoint *cancellation = nullptr);
        VerifiedScanRoot(const VerifiedScanRoot &) = default;
        VerifiedScanRoot(VerifiedScanRoot &&) noexcept = default;
        VerifiedScanRoot &operator=(const VerifiedScanRoot &) = default;
        VerifiedScanRoot &operator=(VerifiedScanRoot &&) noexcept = default;
    private:
        friend class CertifiedScanMembership;
        friend class ScanBatchReadGrant;
        friend class ScanBatchReadScope;
        friend class RelocatableStorageBackend;
        struct State;
        explicit VerifiedScanRoot(std::shared_ptr<const State> state) : state_(std::move(state)) {}
        [[nodiscard]] bool matches(const RelocatableMetadataStore &metadata, const StorageIO &io,
            const MetadataSnapshotPin &snapshot, const HybridPropertyDescriptor *descriptor = nullptr) const;
        std::shared_ptr<const State> state_;
    };

    // Borrowed exclusively by the producer's batch visitor. IDs are copied into
    // bounded private storage so a consumer cannot mutate a borrowed ID buffer
    // into a different proof. Only read() activates authority, synchronously.
    class ScanBatchReadGrant final {
    public:
        static constexpr size_t MAXIMUM_ROWS = 512U;
        ScanBatchReadGrant(const ScanBatchReadGrant &) = delete;
        ScanBatchReadGrant &operator=(const ScanBatchReadGrant &) = delete;
        void read(std::span<const uint64_t> requested, const std::function<void()> &consumer) const;
    private:
        friend class CertifiedScanMembership;
        friend class ScanBatchReadScope;
        ScanBatchReadGrant(const VerifiedScanRoot &root, const MetadataSnapshotPin &snapshot,
            std::span<const uint64_t> selected,
            std::shared_ptr<query::execution::MaterializedByteBudget> sourceWorkspace);
        utils::OwnedHeapReservation frame_;
        const VerifiedScanRoot root_;
        const MetadataSnapshotPin &snapshot_;
        std::shared_ptr<query::execution::MaterializedByteBudget> workspace_;
        std::array<uint64_t, MAXIMUM_ROWS> selected_{};
        size_t count_ = 0;
    };

    class ScanBatchReadScope final {
    public:
        // No raw record, caller boolean or cache entry can activate this check.
        [[nodiscard]] static bool covers(const RelocatableMetadataStore &metadata, const StorageIO &io,
            const MetadataSnapshotPin &snapshot, const HybridPropertyDescriptor &descriptor,
            std::span<const EntityPropertyKey> owners);
        ScanBatchReadScope(const ScanBatchReadScope &) = delete;
        ScanBatchReadScope &operator=(const ScanBatchReadScope &) = delete;
    private:
        friend class ScanBatchReadGrant;
        friend class VerifiedScanRoot;
        class Suspension;
        ScanBatchReadScope(const ScanBatchReadGrant &grant, std::span<const uint64_t> requested);
        ~ScanBatchReadScope();
        utils::OwnedHeapReservation frame_;
        const ScanBatchReadGrant &grant_;
        std::array<uint64_t, ScanBatchReadGrant::MAXIMUM_ROWS> requested_{};
        size_t count_ = 0;
        bool dense_ = false;
        const ScanBatchReadScope *previous_ = nullptr;
        static thread_local const ScanBatchReadScope *active_;
    };
}
