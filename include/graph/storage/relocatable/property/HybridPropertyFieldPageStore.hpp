/**
 * @file HybridPropertyFieldPageStore.hpp
 * @brief Snapshot-authenticated physical reads for isolated hybrid fields.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <variant>
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldView.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"

namespace graph::storage {
class HybridPropertyFieldPageStore;
// Own the actual BufferPool pin or charged fallback bytes for the entire view
// lifetime. Moving the holder preserves its backing allocation; copying and
// extracting a view from a temporary holder are prohibited.
class HybridPropertyFieldPage final {
public:
    HybridPropertyFieldPage(const HybridPropertyFieldPage &) = delete;
    HybridPropertyFieldPage &operator=(const HybridPropertyFieldPage &) = delete;
    HybridPropertyFieldPage(HybridPropertyFieldPage &&) noexcept = default;
    HybridPropertyFieldPage &operator=(HybridPropertyFieldPage &&) noexcept = default;
    [[nodiscard]] const HybridPropertyFieldView &view() const & { return view_; }
    const HybridPropertyFieldView &view() const && = delete;
    [[nodiscard]] bool isPinned() const { return std::holds_alternative<SharedPagePoolPinnedView>(backing_); }
private:
    friend class HybridPropertyFieldPageStore;
    friend class HybridPropertyDirectoryIntegrity;
    using Backing = std::variant<SharedPagePoolPinnedView, SharedPagePoolContents>;
    // Bounded physical-page inspection; records borrow only the callback's
    // charged read buffer. There is no root/publication or reclamation grant.
    template<class Visitor>
    void visitRecords(const std::shared_ptr<std::pmr::memory_resource> &memory, Visitor &&visitor) const {
        utils::OwnedAllocationScope scope(memory);
        std::visit([&](const auto &stored) {
            const auto bytes = [&]() -> std::span<const std::byte> {
                if constexpr (requires { stored.payload(); }) return stored.payload();
                else return stored.payload;
            }();
            const auto page = SlottedPage::open(stored.owner, bytes, memory);
            hybrid_property_detail::require(page.stats().contiguous_free_bytes == stored.location.free_bytes,
                "native field page has stale free-space metadata");
            const auto references = page.liveSlotsOwned(memory);
            for (const auto &reference : references) {
                const auto record = page.readOwned(reference, memory);
                visitor(reference, std::span<const std::byte>(record));
            }
        }, backing_);
    }

    [[nodiscard]] HybridPropertyPageImage clonePrivate(Generation next,
        std::shared_ptr<std::pmr::memory_resource> memory) const {
        return std::visit([&](const auto &stored) {
            const auto bytes = [&]() -> std::span<const std::byte> {
                if constexpr (requires { stored.payload(); }) return stored.payload();
                else return stored.payload;
            }();
            return HybridPropertyPageImage::open(bytes, stored.owner.page_id, stored.owner.generation,
                next, memory);
        }, backing_);
    }

    template<class Contents>
    HybridPropertyFieldPage(Contents contents, const Superblock &visible)
        : backing_(std::move(contents)), view_(std::visit([&](const auto &stored) {
            hybrid_property_detail::require(stored.owner.owner_id == stored.pool.owner_id &&
                stored.pool.committed_lsn.isValid() && stored.pool.committed_lsn <= stored.owner.committed_lsn,
                "hybrid field frame belongs to another pool owner");
            const auto payload = [&]() -> std::span<const std::byte> {
                if constexpr (requires { stored.payload(); }) return stored.payload();
                else return stored.payload;
            }();
            return HybridPropertyFieldView::open(stored.owner, payload, visible.generation, visible.checkpoint_lsn);
        }, backing_)) {}
    Backing backing_;
    HybridPropertyFieldView view_;
};

// Physical reader only: the graph reader must additionally authenticate the
// descriptor namespace/root and typed locator. Ordinary shared ROW readers and
// every standalone writer retain their existing hybrid-kind rejection.
class HybridPropertyFieldPageStore final {
public:
    HybridPropertyFieldPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata)
        : pages_(std::move(io), metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY), metadata_(metadata) {}
    HybridPropertyFieldPageStore(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
        const MetadataSnapshotPin &snapshot) : pages_(std::move(io), metadata, snapshot), metadata_(metadata) {}
    [[nodiscard]] HybridPropertyFieldPage read(const MetadataSnapshotPin &snapshot, PageId page,
        BufferPoolReadHint hint = BufferPoolReadHint::BPRH_NORMAL) const {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "hybrid field page reading is disabled");
        hybrid_property_detail::require(metadata_.ownsSnapshot(snapshot) && page.isValid(),
            "hybrid field page read requires its own live snapshot and a logical page");
        const auto location = metadata_.findPageLocation(snapshot, page);
        const auto pool = metadata_.findCatalog(snapshot, location.segment_id);
        hybrid_property_detail::require(pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL && pool.owner_id,
            "hybrid field location does not reference a property pool");
        if (auto pinned = pages_.readPinnedKnownLocation(snapshot, pool, location, hint, true))
            return HybridPropertyFieldPage(std::move(*pinned), snapshot.superblock());
        return HybridPropertyFieldPage(pages_.readKnownLocation(snapshot, pool, location, hint, true), snapshot.superblock());
    }
    // Copies only an already snapshot-authenticated native field frame. The
    // caller must still fence its logical field locators and publish the COW
    // image with allocator/maps/descriptor/retirement in one metadata batch.
    [[nodiscard]] HybridPropertyPageImage cloneForMutation(const MetadataSnapshotPin &snapshot, PageId page,
        Generation next, std::shared_ptr<std::pmr::memory_resource> memory) const {
        hybrid_property_detail::require(metadata_.ownsSnapshot(snapshot) && bool(memory) &&
            snapshot.superblock().generation.value() < (std::numeric_limits<uint64_t>::max)() &&
            next.value() == snapshot.superblock().generation.value() + 1,
            "hybrid field clone requires its original pin, next generation and account");
        utils::OwnedAllocationScope scope(memory);
        const auto source = read(snapshot, page);
        return source.clonePrivate(next, std::move(memory));
    }
private:
    SharedPagePoolAllocator pages_;
    RelocatableMetadataStore &metadata_;
};
} // namespace graph::storage
