/**
 * @file HybridPropertyReadContext.hpp
 * @brief Pinned descriptor, directory and field access for the hybrid candidate.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/utils/OwnedPmrObject.hpp"
#include "graph/storage/relocatable/entity/ScanReadProof.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include <algorithm>
#include <bit>
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include <type_traits>
#include <limits>
#include <vector>
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectory.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldPageStore.hpp"
#include "graph/storage/relocatable/property/HybridPropertyColdRowInspection.hpp"
#include "graph/utils/StandardContainerHeapBytes.hpp"

namespace graph::storage {
class HybridPropertyReadContextTestAccess;
class HybridPropertyDirectoryIntegrity;
// Snapshot-local, with borrowed caller-owned page store and pin. Normal opening
// reads the actual physical descriptor root in the isolated native format.
// Decoded metadata stays within this source and its original resource owner.
// Callbacks borrow fields only during invocation; outputs must own their values.
class HybridPropertyReadContext final {
    // One synchronous bounded batch may decode several records from one
    // authenticated physical frame. The lease below never outlives its caller.
    struct FieldPageCursor {
        std::optional<HybridPropertyFieldPage> page;
        PageId id{};
        uint64_t epoch = UINT64_MAX;
        bool sequential = false;
    };
    struct BatchReadWindow {
        static constexpr size_t GROUPS = 8U;
        // Group, membership and column records often alternate between pages.
        // Three bounded frames avoid evicting each role on every lookup.
        std::array<FieldPageCursor, 3> fields;
        size_t next_field = 0;
        void releaseFields() { for (auto &field : fields) field.page.reset(); }
        std::array<HybridPropertyGroupDirectoryMap::Entry, GROUPS> groups{};
        HybridPropertyGroupDirectoryMap::Key first{};
        EntityPropertyOwnerKind kind = EntityPropertyOwnerKind::EPOK_NODE;
        uint64_t epoch = UINT64_MAX;
        size_t count = 0;
        bool valid = false;
        bool busy = false;
        // Index outlives its cursor; neither owns a per-call admission callback.
        std::optional<BTreeIndex> directory;
        std::optional<BTreeIndex::RangeCursor> continuation;
        std::optional<HybridPropertyGroupDirectoryMap::Entry> pending;
        std::optional<HybridPropertyGroupDirectoryMap::Key> last;
        void releaseDirectory() {
            continuation.reset(); directory.reset(); pending.reset(); last.reset();
        }
    };
    mutable BatchReadWindow *batch_read_window_ = nullptr;
    template<class Visitor>
    auto withFieldPage(PageId id, FieldPageCursor *cursor, Visitor &&visitor) const
        -> std::invoke_result_t<Visitor &, const HybridPropertyFieldPage &> {
        check();
        if (batch_read_window_) {
            auto &window = *batch_read_window_;
            cursor = nullptr;
            for (auto &field : window.fields) {
                if (field.page && field.id == id) { cursor = &field; break; }
            }
            if (!cursor) cursor = &window.fields[window.next_field++ % window.fields.size()];
        }
        if (!cursor) {
            const auto page = fields_.read(snapshot_, id);
            return visitor(page);
        }
        const auto epoch = membership_io_->dataMutationEpoch();
        const bool sequential = StorageIO::SequentialReadScope::activeFor(*membership_io_);
        if (!cursor->page || cursor->id != id || epoch == UINT64_MAX || cursor->epoch != epoch ||
            cursor->sequential != sequential) {
            cursor->page.reset();
            const auto load = [&] { cursor->page.emplace(fields_.read(snapshot_, id)); };
            try { load(); }
            catch (const query::execution::MaterializedByteLimitExceeded &) {
                if (!batch_read_window_) throw;
                // Optional reuse may not make a formerly readable page fail.
                // Drop retained frames and retry with the original single-page footprint.
                batch_read_window_->releaseFields(); load();
            } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                if (!batch_read_window_) throw;
                batch_read_window_->releaseFields(); load();
            }
            cursor->id = id; cursor->sequential = sequential;
            cursor->epoch = membership_io_->dataMutationEpoch() == epoch ? epoch : UINT64_MAX;
        } else debug::PerfTrace::addValue("native_graph.hybrid.column_window_page_reuses", 1);
        check();
        return visitor(std::as_const(*cursor->page));
    }
public:
    enum class FieldRoute : uint8_t { HFR_ROW = 1, HFR_MISSING, HFR_FOUND };
    struct Enumeration { bool routed = false; bool owner_live = false; bool complete = true; size_t fields = 0; };
    using ScalarVisitor = std::function<void(const HybridPropertyFieldView::ScalarCell &)>;
    using EncodedVisitor = std::function<void(std::span<const std::byte>)>;
    using ExtensionVisitor = std::function<void(const HybridPropertyExtensionView &)>;
    using ColumnVisitor = std::function<void(const HybridPropertyGroupRecord &, const HybridPropertyColumnRecord &,
        const SlottedSlotReference &, const SlottedSlotReference &)>;
    // A cursor lends its existing ROW representation to this synchronous read.
    // No callback or bytes escape the lease; nested calls restore their parent.
    class ColdRowRepresentationLease final {
        const HybridPropertyReadContext &context_;
        const std::function<void()> *previous_;
    public:
        ColdRowRepresentationLease(const HybridPropertyReadContext &context, const std::function<void()> *release)
            : context_(context), previous_(context.before_cold_decode_) { context.before_cold_decode_ = release; }
        ~ColdRowRepresentationLease() { context_.before_cold_decode_ = previous_; }
        ColdRowRepresentationLease(const ColdRowRepresentationLease &) = delete;
        ColdRowRepresentationLease &operator=(const ColdRowRepresentationLease &) = delete;
    };
    [[nodiscard]] ColdRowRepresentationLease borrowColdRowRepresentation(const std::function<void()> *release) const {
        return ColdRowRepresentationLease(*this, release);
    }
    // This is a short metadata/page view, not entity or label authority. The
    // producer's existing proof is still required for directory read-ahead.
    class ColumnBatchLease final {
        friend class HybridPropertyReadContext;
        utils::OwnedHeapReservation frame_;
        const HybridPropertyReadContext &context_;
        BatchReadWindow window_;
        BatchReadWindow *previous_ = nullptr;
        bool installed_ = false, reused_ = false;
        ColumnBatchLease(const HybridPropertyReadContext &context, bool enabled, bool query = false)
            : frame_(context.membership_memory_owner_), context_(context) {
            if (!enabled || !context.membership_memory_owner_) return;
            previous_ = context_.batch_read_window_;
            if (previous_ && !previous_->busy && !query) {
                previous_->busy = true; reused_ = true; return;
            }
            // A rejected nested lease must not accidentally mutate the active
            // parent's optional state. Restore the parent even on this fallback.
            if (previous_) {
                previous_->releaseFields(); context_.batch_read_window_ = nullptr; installed_ = true;
            }
            try { frame_.set(sizeof(ColumnBatchLease)); }
            catch (const query::execution::MaterializedByteLimitExceeded &) { return; }
            catch (const query::execution::detail::ResourceAdmissionDenied &) { return; }
            window_.busy = !query;
            context_.batch_read_window_ = &window_; installed_ = true;
        }
    public:
        ColumnBatchLease(const ColumnBatchLease &) = delete;
        ColumnBatchLease &operator=(const ColumnBatchLease &) = delete;
        ~ColumnBatchLease() {
            if (reused_) previous_->busy = false;
            else if (installed_) context_.batch_read_window_ = previous_;
        }
    };
    [[nodiscard]] ColumnBatchLease borrowColumnBatchWindow(bool enabled) const {
        return ColumnBatchLease(*this, enabled);
    }
    [[nodiscard]] ColumnBatchLease borrowQueryWindow() const {
        return ColumnBatchLease(*this, true, true);
    }
    HybridPropertyReadContext(std::shared_ptr<StorageIO> io, SlottedPagePool &pages,
        RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
        const CancellationCheckpoint *cancellation = nullptr)
        : HybridPropertyReadContext(io, pages, metadata, snapshot, load(io, metadata, snapshot), cancellation) {}
    // Typed maintenance can borrow the already authenticated page store's file
    // without acquiring raw I/O authority or constructing a second allocator.
    HybridPropertyReadContext(SlottedPagePool &pages, RelocatableMetadataStore &metadata,
        const MetadataSnapshotPin &snapshot, const CancellationCheckpoint *cancellation = nullptr)
        : HybridPropertyReadContext(pages.pool_.io_, pages, metadata, snapshot, cancellation) {}
    HybridPropertyReadContext(const HybridPropertyReadContext &) = delete;
    HybridPropertyReadContext &operator=(const HybridPropertyReadContext &) = delete;
    HybridPropertyReadContext(HybridPropertyReadContext &&) noexcept = default;

    [[nodiscard]] bool usesSnapshot(const MetadataSnapshotPin &snapshot) const {
        return &snapshot == &snapshot_ && metadata_.ownsSnapshot(snapshot) && snapshot.superblock().generation == generation_;
    }
    void checkpoint() const { check(); }
    // A full-entity scan may retain one bounded group worth of columns, rather
    // than thrashing the small selected-field window for each successive owner.
    // It uses the same cache/account, fills only requested fields, and remains
    // optional under pressure. There is no second ROW image or prefetch.
    void enableFullEntityWindow(size_t actualFields) const {
        check();
        const auto desired = (std::min)((std::max)(actualFields, size_t{8}), HybridPropertyColumnMembership::MAX_COLUMNS);
        if (desired <= full_entity_columns_) return;
        full_entity_columns_ = desired;
        if (membership_cache_) {
            try { membership_cache_->columns.resize(desired); }
            catch (const query::execution::MaterializedByteLimitExceeded &) {}
            catch (const query::execution::detail::ResourceAdmissionDenied &) {}
        }
    }
    [[nodiscard]] bool hasNativeGroups() const {
        check(); return hasRoot(EntityPropertyOwnerKind::EPOK_NODE, 3) || hasRoot(EntityPropertyOwnerKind::EPOK_EDGE, 3);
    }
    // Read-only group/Entity membership probe. A disengaged result follows
    // ordinary ROW routing; false names an absent pending slot in a real group.
    // This authenticates Entity/competing ROW authority without visiting fields
    // and grants no mutation or new-owner publication authority.
    [[nodiscard]] std::optional<bool> ownerLiveness(EntityPropertyKey owner,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        const auto group = findGroup(owner, boundaryAdmission, nullptr, true, true);
        if (!group) return {};
        return isLive(owner, *group);
    }
    // Fixed-size authenticated source for bounded inverse conversion. This
    // grants no write authority; the caller must retain/fence the same pin.
    [[nodiscard]] std::optional<HybridPropertyGroupRecord> groupRecord(EntityPropertyKey owner,
        const std::function<void(size_t)> &boundaryAdmission = {}, SlottedSlotReference *reference = nullptr) const {
        return findGroup(owner, boundaryAdmission, reference);
    }
    // Next explicit-selector owner for immutable-value maintenance. Compact
    // column-only owners have primitive values and need no LargeValue visit.
    [[nodiscard]] std::optional<EntityPropertyKey> nextOwnerAfter(EntityPropertyOwnerKind kind, uint64_t afterOwnerId,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        check(); require(hybrid_property_detail::validKind(kind) && afterOwnerId <= RSS_HYBRID_MAX_ENTITY_ID,
            "hybrid owner seek has an invalid namespace or cursor");
        if (!hasRoot(kind, 0)) return {};
        HybridPropertyOwnerMap::Key first{};
        if (afterOwnerId == RSS_HYBRID_MAX_ENTITY_ID) first[0] = std::byte{0x80};
        else if (afterOwnerId) {
            first = HybridPropertyDirectoryCodec::ownerKey({kind, afterOwnerId + 1U}, 1);
            std::fill(first.begin() + 8, first.end(), std::byte{0});
        }
        std::optional<EntityPropertyKey> result;
        const auto map = ownerMap(kind);
        static_cast<void>(map.visitFrom(snapshot_, first, 1, [&](const auto &entry) {
            const auto field = HybridPropertyDirectoryCodec::decodeOwnerKey(entry.key, kind);
            static_cast<void>(HybridPropertyDirectoryCodec::decodeSource(entry.value));
            require(field.owner.entity_id > afterOwnerId, "hybrid owner seek did not advance");
            result = field.owner; return false;
        }, boundaryAdmission, cancellation_));
        return result;
    }
    enum class OwnershipStepKind : uint8_t { HOSK_LEGACY_OWNER, HOSK_COLD_OWNER, HOSK_BOUNDARY };
    struct OwnershipStep { EntityPropertyKey owner; OwnershipStepKind kind; };
    // Reference-free ranges still consume one work unit and advance a stable
    // logical cursor. Never walk arbitrary numbers of empty groups under one
    // owner budget, and never use a boundary to skip an ordinary ROW owner.
    [[nodiscard]] std::optional<OwnershipStep> nextOwnershipStepAfter(EntityPropertyOwnerKind kind, uint64_t after,
        const std::function<void(size_t)> &admission = {}) const {
        const auto legacy = nextOwnerAfter(kind, after, admission);
        std::optional<OwnershipStep> result;
        if (legacy) result = OwnershipStep{*legacy, OwnershipStepKind::HOSK_LEGACY_OWNER};
        if (after == RSS_HYBRID_MAX_ENTITY_ID || !hasRoot(kind, 3)) return result;
        if (admission) admission(RSS_PAGE_SIZE); // charge even a single-leaf cursor with no sibling key
        const auto binding = bind(kind, 3);
        HybridPropertyGroupDirectoryMap groups(binding.root.index_id, trees_, metadata_, binding);
        const auto first = HybridPropertyDirectoryCodec::groupKey(hybridPropertyGroupFor({kind, after + 1U}));
        static_cast<void>(groups.visitFrom(snapshot_, first, 2, [&](const auto &entry) {
            check();
            const auto id = HybridPropertyDirectoryCodec::decodeGroupKey(entry.key);
            const auto reference = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
            const auto page = fields_.read(snapshot_, reference.page_id);
            const auto group = page.view().readGroup(reference, kind, id, descriptor_->semantic_generation);
            require(group.live != 0, "ownership group has no live boundary");
            const uint64_t baseId = (id - 1U) * 64U;
            const uint64_t last = baseId + 64U - std::countl_zero(group.live);
            if (last <= after) return true;
            // findGroup authenticates format, actual Entity Map and competing
            // ROW at the range boundary; selected reference owners are checked
            // again before their reference is passed to the maintenance sink.
            const auto authenticated = findGroup({kind, last}, admission, nullptr, true, true);
            require(authenticated && *authenticated == group, "ownership group changed within its snapshot");
            if (!group.usesColumnMembership())
                require(!group.memberships || hasRoot(kind, 0), "legacy ownership group has no selector directory");
            OwnershipStep candidate{{kind, last}, OwnershipStepKind::HOSK_BOUNDARY};
            if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
                withColumnMembership(group, [&](const auto &matrix) {
                    uint64_t remaining = matrix.cold_rows.owners;
                    while (remaining) {
                        check(); const auto slot = unsigned(std::countr_zero(remaining));
                        const auto root = matrix.cold_rows.roots[slot];
                        if (admission) admission(RSS_PAGE_SIZE);
                        withColdBlock(group, matrix, root, [&](const auto &block, const auto &) {
                            remaining &= ~block.owners;
                            for (unsigned i = 0; i < 64; ++i) {
                                const auto row = block.row(i);
                                const auto ownerId = baseId + i + 1U;
                                if (row && row->large_value_reference && ownerId > after &&
                                    (candidate.kind == OwnershipStepKind::HOSK_BOUNDARY || ownerId < candidate.owner.entity_id))
                                    candidate = {{kind, ownerId}, OwnershipStepKind::HOSK_COLD_OWNER};
                            }
                        });
                    }
                });
            }
            if (!result || candidate.owner.entity_id < result->owner.entity_id) result = candidate;
            return false;
        }, admission, cancellation_));
        return result;
    }
    // Reference marking deliberately does not read or materialize the value's
    // content. The maintenance sink authenticates its actual manifest; full
    // integrity and field reads retain their stronger content validation.
    [[nodiscard]] bool visitColdReference(EntityPropertyKey owner,
        const std::function<void(const LargeValueRef &)> &visitor,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        require(bool(visitor), "cold reference reading requires a synchronous visitor");
        if (boundaryAdmission) boundaryAdmission(RSS_PAGE_SIZE);
        const auto group = findGroup(owner, boundaryAdmission, nullptr, true, true);
        require(group && group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER && isLive(owner, *group),
            "cold reference reading requires its live ROW-remainder group");
        requireNoLegacySelector(owner, boundaryAdmission);
        bool found = false;
        withColumnMembership(*group, [&](const auto &matrix) {
            withColdRow(owner, *group, matrix, [&](const auto &row, Generation generation, const auto &memory) {
                if (!row.large_value_reference) return;
                utils::OwnedHeapReservation credit(memory, row.root.size());
                const auto reference = EntityPropertyLargeValueReferenceCodec::decode(row.root);
                const auto type = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
                require(reference.owner_entity_id == owner.entity_id && reference.owner_entity_type == type &&
                    reference.value.kind == LargeValueKind::LVK_BYTES &&
                    reference.value.media_type == RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE &&
                    reference.value.generation <= generation &&
                    reference.value.logical_bytes >= RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES,
                    "cold reference has a foreign owner, media or future generation");
                check(); visitor(reference.value); check(); found = true;
            });
        });
        return found;
    }
    [[nodiscard]] Enumeration visitOwner(EntityPropertyKey owner, size_t maximumEntries,
        const std::function<bool(uint32_t, HybridPropertySource)> &visitor,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        require(maximumEntries && visitor, "hybrid enumeration requires a positive bound and visitor");
        const auto group = findGroup(owner, boundaryAdmission, nullptr, true, true);
        if (!group) return {};
        return visitOwnerInGroup(owner, *group, maximumEntries, visitor, boundaryAdmission);
    }
    // Root presence is a metadata membership fact. A selected hot field must
    // not decompress or authenticate an unrelated cold root merely to prove
    // that its owner has fields. Exact counts still inspect the cold directory.
    [[nodiscard]] std::optional<bool> hasFields(EntityPropertyKey owner,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        const auto group = findGroup(owner, boundaryAdmission, nullptr, true, true);
        if (!group) return {};
        if (group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
            requireNoLegacySelector(owner, boundaryAdmission);
            return withColumnMembership(*group, [&](const auto &matrix) {
                const auto slot = hybridPropertySlotFor(owner);
                if (matrix.cold_rows.owners & (UINT64_C(1) << slot)) return true;
                for (const auto word : matrix.owners[slot]) if (word) return true;
                return false;
            });
        }
        return visitOwnerInGroup(owner, *group, 1,
            [](uint32_t, HybridPropertySource) { return false; }, boundaryAdmission).fields != 0;
    }
    // A disengaged result means this owner still follows ROW routing. Counts
    // and tokens authenticate memberships, not unvisited field/LargeValue bodies.
    [[nodiscard]] std::optional<size_t> count(EntityPropertyKey owner,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        const auto result = visitOwner(owner, (std::numeric_limits<size_t>::max)() - 1U,
            [](uint32_t, HybridPropertySource) { return true; }, boundaryAdmission);
        if (!result.routed) return {};
        if (!result.complete)
            throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "hybrid membership count exceeded its native bound");
        return result.fields;
    }
    [[nodiscard]] std::optional<std::vector<uint32_t>> tokens(EntityPropertyKey owner,
        const SelectedPropertyReadAdmission &outputAdmission,
        const SelectedPropertyReadAdmission &transientAdmission) const {
        require(outputAdmission && transientAdmission, "hybrid token enumeration requires retained output and transient admission");
        const auto size = count(owner, transientAdmission);
        transientAdmission(0);
        if (!size) return {};
        if (*size > (std::numeric_limits<size_t>::max)() / sizeof(uint32_t))
            throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "hybrid token output capacity overflowed");
        outputAdmission(*size * sizeof(uint32_t));
        std::vector<uint32_t> result;
        result.reserve(*size); outputAdmission(result.capacity() * sizeof(uint32_t));
        const auto append = [&](uint32_t token, HybridPropertySource) { result.push_back(token); return true; };
        const auto enumeration = visitOwner(owner, *size ? *size : 1U, std::ref(append), transientAdmission);
        require(enumeration.routed && enumeration.complete && enumeration.fields == *size,
            "hybrid owner membership changed within its pinned token enumeration");
        transientAdmission(0);
        return result;
    }
    [[nodiscard]] FieldRoute visitField(EntityPropertyKey owner, uint32_t token,
        const ScalarVisitor &scalar, const ExtensionVisitor &extension,
        const std::function<void(size_t)> &boundaryAdmission = {},
        const HybridPropertyExtensionView::ReferenceAdmission &referenceAdmission = {},
        const ColumnVisitor &columnVisitor = {}, const EncodedVisitor &encoded = {}, LargeValueStore *largeValues = nullptr) const {
        return visitFieldImpl(owner, token, scalar, extension, boundaryAdmission, referenceAdmission,
            columnVisitor, encoded, largeValues, false);
    }
private:
    [[nodiscard]] FieldRoute visitFieldImpl(EntityPropertyKey owner, uint32_t token,
        const ScalarVisitor &scalar, const ExtensionVisitor &extension,
        const std::function<void(size_t)> &boundaryAdmission = {},
        const HybridPropertyExtensionView::ReferenceAdmission &referenceAdmission = {},
        const ColumnVisitor &columnVisitor = {}, const EncodedVisitor &encoded = {}, LargeValueStore *largeValues = nullptr, bool coldColumnPlan = false) const {
        require(token && scalar && extension, "hybrid field read requires a token and typed visitors");
        SlottedSlotReference groupReference;
        const auto group = findGroup(owner, boundaryAdmission, &groupReference, true, true);
        if (!group) return FieldRoute::HFR_ROW;
        if (group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
            if (columnVisitor && !coldColumnPlan) throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                "cold ROW column mutation lifecycle is not integrated yet");
            return visitRemainderField(owner, token, *group, groupReference, scalar, encoded,
                boundaryAdmission, columnVisitor, largeValues);
        }
        const auto key = HybridPropertyDirectoryCodec::ownerKey(owner, token);
        const auto selected = selector(*group, owner, token, boundaryAdmission);
        if (!selected) return FieldRoute::HFR_MISSING;
        require(isLive(owner, *group) && group->memberships,
            "hybrid field membership belongs to a dead or empty owner group");
        const auto source = *selected;
        const size_t role = source == HybridPropertySource::HPS_COLUMN ? 1U : 2U;
        require((role == 1 && group->mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) ||
            hasRoot(owner.owner_kind, role), "hybrid member source directory is absent");
        if (role == 1) {
            if (!columnVisitor) if (const auto *cached = cachedColumn(*group, token)) {
                const auto slot = hybridPropertySlotFor(owner); const auto bit = UINT64_C(1) << slot;
                require(bool(cached->column.presence & bit), "hybrid column membership has no present value");
                // Copy the selected bits before calling user code. Reentrancy
                // may resize/evict the cache; no borrowed column escapes.
                const HybridPropertyFieldView::ScalarCell cell{cached->column.identity.scalar_kind,
                    true, bool(cached->column.nulls & bit), cached->column.words[slot]};
                debug::PerfTrace::addValue("native_graph.hybrid.column_cache_hits", 1);
                scalar(cell); return FieldRoute::HFR_FOUND;
            }
            const auto selectedColumn = columnRecord(*group, token, boundaryAdmission);
            const auto &column = selectedColumn.column;
            const auto slot = hybridPropertySlotFor(owner); const auto bit = UINT64_C(1) << slot;
            require(bool(column.presence & bit), "hybrid column membership has no present value");
            if (columnVisitor) columnVisitor(*group, column, groupReference, selectedColumn.reference);
            scalar({column.identity.scalar_kind, true, bool(column.nulls & bit), column.words[slot]});
        } else {
            const auto binding = bind(owner.owner_kind, role);
            HybridPropertyFieldDirectoryMap fields(binding.root.index_id, trees_, metadata_, binding);
            const auto locator = fields.find(snapshot_, key, boundaryAdmission, cancellation_);
            require(bool(locator), "hybrid field membership has no source record");
            const auto reference = HybridPropertyDirectoryCodec::decodeLocator(*locator);
            const auto page = fields_.read(snapshot_, reference.page_id);
            check();
            const auto value = page.view().readExtension(reference, owner, token, *group,
                descriptor_->semantic_generation, referenceAdmission);
            extension(value);
        }
        check();
        return FieldRoute::HFR_FOUND;
    }
private:
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
    friend class HybridPropertyValueReader;
    // The borrowed body is owned by this call's stack, not by a cache. The
    // original point/count readers run synchronously inside each ordered run.
    [[nodiscard]] bool visitColdWindowSorted(std::span<const EntityPropertyKey> owners, std::span<const uint32_t> tokens,
        bool exactCounts, const std::function<void(size_t, size_t)> &visitor,
        const std::function<void(size_t)> &admission) const {
        if (cold_window_) return false;
        check(); require(!owners.empty() && owners.size() <= 64 && visitor, "invalid cold window request");
        const auto kind = owners.front().owner_kind; const auto id = hybridPropertyGroupFor(owners.front());
        uint64_t previous = 0;
        for (const auto owner : owners) {
            require(owner.isValid() && owner.owner_kind == kind && hybridPropertyGroupFor(owner) == id &&
                owner.entity_id > previous, "cold window owners must be ordered, unique and in one group");
            previous = owner.entity_id;
        }
        const auto pageAdmission = [&](size_t bytes) {
            require(bytes <= SIZE_MAX - sizeof(FieldPageCursor), "cold window page scratch overflow");
            if (admission) admission(bytes + sizeof(FieldPageCursor));
        };
        pageAdmission(0);
        FieldPageCursor pageCursor;
        const auto group = findGroup(owners.front(), pageAdmission, nullptr, false, true, &pageCursor);
        if (!group || group->mode != HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) return false;
        return withColumnMembership(*group, [&](const auto &sourceMatrix) {
            bool needed = exactCounts;
            for (const auto token : tokens) needed |= !sourceMatrix.ordinal(token).has_value();
            if (!needed) return false;
            uint64_t selected = 0;
            for (const auto owner : owners) selected |= UINT64_C(1) << hybridPropertySlotFor(owner);
            if (!(selected & sourceMatrix.cold_rows.owners)) return false;
            constexpr size_t scratch = 64U * (sizeof(uint64_t) + sizeof(std::optional<EntityPageLocation>) +
                sizeof(std::optional<EntityPropertyLocation>)) + sizeof(HybridPropertyColumnMembership) + sizeof(FieldPageCursor);
            const auto admit = [&](size_t bytes) {
                require(bytes <= (std::numeric_limits<size_t>::max)() - scratch, "cold window scratch overflow");
                if (admission) admission(bytes + scratch);
            };
            admit(0);
            // A nested consumer may reuse the existing membership cache. Keep
            // this fixed-size, accounted boundary stable across callbacks.
            const auto matrix = sourceMatrix;
            pageCursor.page.reset(); // the owning matrix is enough for cold decoding
            static_cast<void>(authenticateOwnerRoutes(*group, owners));
            for (const auto owner : owners) { check(); requireNoLegacySelector(owner, admit); }
            const auto hasCold = [&](size_t i) { return bool(matrix.cold_rows.owners & (UINT64_C(1) << hybridPropertySlotFor(owners[i]))); };
            size_t first = 0;
            while (first < owners.size()) {
                if (!hasCold(first)) {
                    size_t end = first + 1;
                    while (end < owners.size() && !hasCold(end)) ++end;
                    ColdWindowLease lease(*this, nullptr, {}); visitor(first, end); first = end; continue;
                }
                const auto root = matrix.cold_rows.roots[hybridPropertySlotFor(owners[first])];
                size_t end = first + 1;
                while (end < owners.size() && (!hasCold(end) || matrix.cold_rows.roots[hybridPropertySlotFor(owners[end])] == root)) ++end;
                withColdBlock(*group, matrix, root, [&](const auto &block, const auto &) {
                    ColdWindowLease lease(*this, &block, root); visitor(first, end);
                });
                first = end;
            }
            return true;
        }, &pageCursor);
    }
    // Borrow one decoded cold body for a consecutive same-reference run in
    // the existing <=64-owner window. Output ordinals stay ordered; no decoded
    // body or callback is retained in the context and no second cache exists.
    [[nodiscard]] bool visitColdOwnersSorted(std::span<const EntityPropertyKey> owners, uint32_t token,
        const std::function<void(size_t, std::optional<std::span<const std::byte>>)> &visitor,
        const std::function<void(size_t)> &admission, LargeValueStore *largeValues) const {
        check(); require(!owners.empty() && owners.size() <= 64 && token && visitor, "invalid cold grouped request");
        const auto kind = owners.front().owner_kind; const auto groupId = hybridPropertyGroupFor(owners.front());
        uint64_t previous = 0;
        for (const auto owner : owners) {
            require(owner.isValid() && owner.owner_kind == kind && hybridPropertyGroupFor(owner) == groupId &&
                owner.entity_id > previous, "cold grouped owners must be ordered, unique and in one group");
            previous = owner.entity_id;
        }
        constexpr size_t scratch = 64U * (sizeof(uint64_t) + sizeof(std::optional<EntityPageLocation>) +
            sizeof(std::optional<EntityPropertyLocation>)) + sizeof(FieldPageCursor);
        const auto admit = [&](size_t bytes) {
            require(bytes <= (std::numeric_limits<size_t>::max)() - scratch, "cold grouped scratch overflow");
            if (admission) admission(bytes + scratch);
        };
        admit(0);
        FieldPageCursor pageCursor;
        const auto group = findGroup(owners.front(), admit, nullptr, false, true, &pageCursor);
        if (!group || group->mode != HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) return false;
        return withColumnMembership(*group, [&](const auto &matrix) {
            if (matrix.ordinal(token)) return false; // globally hot absence never falls through to cold
            pageCursor.page.reset();
            static_cast<void>(authenticateOwnerRoutes(*group, owners));
            for (const auto owner : owners) { check(); requireNoLegacySelector(owner, admit); }
            debug::PerfTrace::addValue("native_graph.hybrid.grouped_owner_route_checks", int64_t(owners.size()));
            const auto hasCold = [&](size_t i) { return bool(matrix.cold_rows.owners & (UINT64_C(1) << hybridPropertySlotFor(owners[i]))); };
            const auto missing = [&](size_t i) {
                bool present = hasCold(i);
                for (const auto word : matrix.owners[hybridPropertySlotFor(owners[i])]) present |= word != 0;
                if (present) { check(); visitor(i, std::nullopt); }
            };
            size_t first = 0;
            while (first < owners.size()) {
                if (!hasCold(first)) { missing(first++); continue; }
                const auto root = matrix.cold_rows.roots[hybridPropertySlotFor(owners[first])];
                size_t end = first + 1;
                while (end < owners.size() && (!hasCold(end) ||
                    matrix.cold_rows.roots[hybridPropertySlotFor(owners[end])] == root)) ++end;
                withColdBlock(*group, matrix, root, [&](const auto &block, const auto &memory) {
                    for (size_t i = first; i < end; ++i) {
                        if (!hasCold(i)) { missing(i); continue; }
                        const auto row = block.row(hybridPropertySlotFor(owners[i]));
                        require(bool(row), "cold grouped membership has no row");
                        const auto inspect = [&](LargeValueStore *values) {
                            HybridPropertyColdRowInspection source(snapshot_, *row, owners[i], block.semantic_generation,
                                std::span(matrix.tokens).first(matrix.column_count), memory, values, cancellation_);
                            const std::array<uint32_t, 1> selected{token};
                            source.visitSelected(selected, [&](size_t, auto bytes) { check(); visitor(i, bytes); });
                        };
                        if (row->large_value_reference && !largeValues) {
                            utils::OwnedHeapReservation credit(memory,
                                utils::detail::standardAllocatorRequestBytes(sizeof(SlottedPagePool), alignof(SlottedPagePool)) +
                                utils::detail::standardAllocatorRequestBytes(sizeof(BTreePageStore), alignof(BTreePageStore)));
                            LargeValueStore values(source_io_, metadata_, snapshot_);
                            inspect(&values);
                        } else inspect(largeValues);
                    }
                });
                first = end;
            }
            return true;
        }, &pageCursor);
    }
    // One compact field is authenticated for a sorted same-group request.
    // No output or authority is granted until primary-map and competing-ROW
    // checks complete. Declining this group leaves the ordinary path available.
    [[nodiscard]] bool visitColumnOwnersSorted(std::span<const EntityPropertyKey> owners,uint32_t token,
        const ColumnVisitor &visitor,const std::function<void(size_t)> &admission) const {
        check();require(!owners.empty() && owners.size()<=64 && token && visitor,"invalid grouped column request");
        const auto kind=owners.front().owner_kind;const auto groupId=hybridPropertyGroupFor(owners.front());
        uint64_t previous=0;
        for(const auto owner:owners) {
            require(owner.isValid() && owner.owner_kind==kind && hybridPropertyGroupFor(owner)==groupId &&
                owner.entity_id>previous,"column group requires ordered unique same-group owners");
            previous=owner.entity_id;
        }
        constexpr size_t scratchBytes=64U*(sizeof(uint64_t)+sizeof(std::optional<EntityPageLocation>)+
            sizeof(std::optional<EntityPropertyLocation>)+sizeof(size_t)+sizeof(uint64_t)+
            sizeof(std::string_view)+HybridPropertyShortText::MAX_BYTES)+sizeof(HybridPropertyColumnRecord)+sizeof(FieldPageCursor);
        const auto admit=[&](size_t bytes){
            require(bytes<=(std::numeric_limits<size_t>::max)()-scratchBytes,"column group scratch overflow");
            if(admission)admission(scratchBytes+bytes);
        };
        admit(0);
        FieldPageCursor pageCursor;
        SlottedSlotReference groupReference;
        const auto group=findGroup(owners.front(),admit,&groupReference,false,true,&pageCursor);
        if(!group || !group->usesColumnMembership())return false;
        std::optional<uint64_t> presence;
        withColumnMembership(*group,[&](const auto &matrix){if(matrix.ordinal(token))presence=matrix.presence(token);}, &pageCursor);
        if(!presence)return false;
        const bool certified = authenticateOwnerRoutes(*group, owners);
        if (!certified) {
            debug::PerfTrace::addValue("native_graph.hybrid.grouped_owner_route_batches", 1);
            debug::PerfTrace::addValue("native_graph.hybrid.grouped_owner_route_checks", int64_t(owners.size()));
        }
        if(hasRoot(kind,0)) {
            // Compact selectors retain explicit entries only for extensions.
            // Scan a bounded selected-owner interval, never the complete tree.
            bool finished=false,extension=false;size_t selected=0;
            const auto maximum=(std::min)(size_t(group->memberships)+1U,owners.size()*8U+1U);
            const auto first=HybridPropertyDirectoryCodec::ownerKey(owners.front(),
                group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER ? 1U : token);
            const bool complete=ownerMap(kind).visitFrom(snapshot_,first,maximum,[&](const auto &entry){
                check();const auto field=HybridPropertyDirectoryCodec::decodeOwnerKey(entry.key,kind);
                if(field.owner.entity_id>owners.back().entity_id){finished=true;return false;}
                while(selected<owners.size() && owners[selected].entity_id<field.owner.entity_id)++selected;
                if(selected<owners.size() && owners[selected]==field.owner)
                    require(group->mode != HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER,
                        "ROW-remainder owner has a competing explicit selector");
                if(selected<owners.size() && owners[selected]==field.owner && field.token==token) {
                    const auto bit=UINT64_C(1)<<hybridPropertySlotFor(field.owner);
                    require(!(*presence&bit) && HybridPropertyDirectoryCodec::decodeSource(entry.value)==HybridPropertySource::HPS_EXTENSION,
                        "compact field has two authorities or a legacy selector");
                    extension=true;
                }
                return true;
            },admit,cancellation_);
            if((!complete && !finished) || extension)return false;
        }
        const auto selected=columnRecord(*group,token,admit,&pageCursor);
        require(selected.column.presence==*presence,"column presence disagrees with group membership");
        check();visitor(*group,selected.column,groupReference,selected.reference);check();
        debug::PerfTrace::addValue("native_graph.hybrid.grouped_column_batches",1);
        return true;
    }
    // The token list comes from this context's authenticated membership walk.
    // Only a pure compact owner containing every group column can use a single
    // bounded directory walk. Mixed/sparse owners retain the point boundary.
    [[nodiscard]] bool fullColumnCells(EntityPropertyKey owner, std::span<const uint32_t> tokens,
        std::span<HybridPropertyFieldView::ScalarCell> cells,
        const std::function<void(size_t)> &admission) const {
        check();
        if (tokens.empty() || tokens.size() > HybridPropertyColumnMembership::MAX_COLUMNS ||
            StorageIO::SequentialReadScope::activeFor(*membership_io_)) return false;
        const auto epoch = membership_io_->dataMutationEpoch();
        if (epoch == (std::numeric_limits<uint64_t>::max)()) return false;
        const auto group = findGroup(owner, admission, nullptr, true, true);
        if (!group || group->mode != HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP ||
            hasRoot(owner.owner_kind, 0) || hasRoot(owner.owner_kind, 2) || group->columns != tokens.size()) return false;
        require(cells.size() >= tokens.size() && isLive(owner, *group), "invalid full-column owner output");
        const auto slot = hybridPropertySlotFor(owner);
        const auto bit = UINT64_C(1) << slot;
        const auto copy = [&](size_t i, const HybridPropertyColumnRecord &column) {
            require(bool(column.presence & bit), "full-column membership has no present value");
            cells[i] = {column.identity.scalar_kind, true, bool(column.nulls & bit), column.words[slot]};
        };
        size_t cached = 0;
        for (; cached < tokens.size(); ++cached) {
            const auto *column = cachedColumn(*group, tokens[cached]);
            if (!column) break;
            copy(cached, column->column);
        }
        if (cached != tokens.size()) {
            debug::PerfTrace::addValue("native_graph.hybrid.full_column_locator_batches", 1);
            withColumnMembership(*group, [&](const auto &matrix) {
                require(matrix.column_count == tokens.size(), "full-column membership changed column count");
                std::optional<HybridPropertyFieldPage> page;
                PageId pageId{};
                for (size_t i = 0; i < tokens.size(); ++i) {
                    check();
                    require(matrix.tokens[i] == tokens[i], "full-column tokens disagree with membership");
                    const auto reference = matrix.columnLocator(tokens[i]);
                    if (!page || pageId != reference.page_id) {
                        page.reset(); page.emplace(fields_.read(snapshot_, reference.page_id)); pageId = reference.page_id;
                    }
                    const AuthenticatedColumn value{page->view().readColumn(reference, *group, tokens[i],
                        descriptor_->semantic_generation), reference};
                    require(value.column.presence == matrix.presence(tokens[i]), "full column disagrees with membership presence");
                    copy(i, value.column);
                    debug::PerfTrace::addValue("native_graph.hybrid.column_decodes", 1);
                    debug::PerfTrace::addValue("native_graph.hybrid.direct_column_locator_reads", 1);
                    prepareReadCache();
                    if (membership_cache_) {
                        auto &destination = membership_cache_->columns[membership_cache_->column_cursor++ % membership_cache_->columns.size()];
                        destination.group = *group; destination.value = value;
                        destination.io_epoch = epoch; destination.valid = true;
                    }
                }
            });
        }
        check();
        const bool unchanged = membership_io_->dataMutationEpoch() == epoch;
        if (unchanged) debug::PerfTrace::addValue("native_graph.hybrid.full_column_batches", 1);
        return unchanged;
    }
#endif
    friend class HybridPropertyReadContextTestAccess;
    friend class HybridPropertyDirectoryIntegrity;
    friend class HybridPropertyDemotionPreparation;
    friend class HybridPropertyRowPreparation;
    friend class HybridPropertyColumnDelta;
    friend class RelocatableMetadataStore;
    HybridPropertyReadContext(std::shared_ptr<StorageIO> io, SlottedPagePool &pages,
        RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
        std::optional<HybridPropertyDescriptor> descriptor, const CancellationCheckpoint *cancellation)
        : snapshot_(snapshot), metadata_(metadata), generation_(snapshot.superblock().generation),
          trees_(pages, metadata), membership_io_(io.get()), source_io_(io), fields_(std::move(io), metadata, snapshot),
          descriptor_(std::move(descriptor)), cancellation_(cancellation),
          membership_memory_owner_(utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
              (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr)) {
        require(metadata_.ownsSnapshot(snapshot_), "hybrid read context requires its own live pin");
        if (descriptor_) require(descriptor_->database_id == snapshot_.superblock().database_id &&
            descriptor_->semantic_generation.isValid() && descriptor_->semantic_generation <= snapshot_.superblock().generation,
            "hybrid read descriptor is foreign or not visible");
    }
    static std::optional<HybridPropertyDescriptor> load(const std::shared_ptr<StorageIO> &io,
        RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot) {
        require(metadata.ownsSnapshot(snapshot) && metadata.usesDataStorage(io), "hybrid descriptor read uses a foreign pin or file");
        if (!snapshot.superblock().hybrid_property_descriptor_root.isValid()) return {};
        MetadataPageFile pages(io, snapshot.superblock());
        return pages.readHybridPropertyDescriptor(snapshot.superblock().hybrid_property_descriptor_root,
            snapshot.superblock()).descriptor;
    }
    void check() const {
        require(metadata_.ownsSnapshot(snapshot_) && snapshot_.superblock().generation == generation_,
            "hybrid read context requires its original live metadata generation");
        if (cancellation_) cancellation_->check();
    }
    static void require(bool value, const char *message) { hybrid_property_detail::require(value, message); }
    static size_t base(EntityPropertyOwnerKind kind) {
        require(hybrid_property_detail::validKind(kind), "hybrid owner namespace is invalid");
        return kind == EntityPropertyOwnerKind::EPOK_NODE ? 0U : 4U;
    }
    bool hasRoot(EntityPropertyOwnerKind kind, size_t role) const {
        return descriptor_ && descriptor_->roots[base(kind) + role].page_id.isValid();
    }
    BTreeRootReadBinding bind(EntityPropertyOwnerKind kind, size_t role) const {
        return HybridPropertyDirectoryCodec::bindRoot(*descriptor_, snapshot_,
            static_cast<HybridPropertyRootOrdinal>(base(kind) + role));
    }
    HybridPropertyOwnerMap ownerMap(EntityPropertyOwnerKind kind) const {
        const auto binding = bind(kind, 0);
        return HybridPropertyOwnerMap(binding.root.index_id, trees_, metadata_, binding);
    }
    static bool isLive(EntityPropertyKey owner, const HybridPropertyGroupRecord &group) {
        return group.live & (UINT64_C(1) << hybridPropertySlotFor(owner));
    }
    [[nodiscard]] bool certifiedOwners(std::span<const EntityPropertyKey> owners) const {
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
        return descriptor_ && ScanBatchReadScope::covers(metadata_, *source_io_, snapshot_, *descriptor_, owners);
#else
        static_cast<void>(owners); return false;
#endif
    }
    [[nodiscard]] bool authenticateOwnerRoutes(const HybridPropertyGroupRecord &group,
        std::span<const EntityPropertyKey> owners) const {
        require(!owners.empty() && owners.size() <= 64U, "hybrid owner authentication exceeds one group");
        if (certifiedOwners(owners)) {
            for (const auto owner : owners) { check(); require(isLive(owner, group), "certified Node is absent from its field group"); }
            debug::PerfTrace::addValue("native_graph.hybrid.certified_owner_route_checks_saved", int64_t(owners.size()));
            return true;
        }
        std::array<uint64_t, 64> ids{};
        std::array<std::optional<EntityPageLocation>, 64> entities{};
        std::array<std::optional<EntityPropertyLocation>, 64> rows{};
        for (size_t i = 0; i < owners.size(); ++i) ids[i] = owners[i].entity_id;
        if (group.owner_kind == EntityPropertyOwnerKind::EPOK_NODE)
            metadata_.findNodePageLocationsSorted(snapshot_, std::span(ids).first(owners.size()),
                std::span(entities).first(owners.size()), cancellation_);
        else metadata_.findEdgePageLocationsSorted(snapshot_, std::span(ids).first(owners.size()),
            std::span(entities).first(owners.size()), cancellation_);
        metadata_.findEntityPropertyLocationsSorted(snapshot_, owners, std::span(rows).first(owners.size()), cancellation_);
        for (size_t i = 0; i < owners.size(); ++i) {
            check(); require(bool(entities[i]) == isLive(owners[i], group) && !rows[i],
                "hybrid grouped owner has invalid Entity Map or competing ROW routing");
        }
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
        debug::PerfTrace::addValue("native_graph.hybrid.owner_route_checks_performed", int64_t(owners.size()));
#endif
        return false;
    }
    // One bounded continuation for certified monotonic windows. Seeking a
    // sparse jump never walks more than eight unrelated records before restart.
    void refillGroupDirectory(BatchReadWindow &window, const HybridPropertyGroupDirectoryMap::Key &key,
        EntityPropertyOwnerKind kind, uint64_t epoch) const {
        utils::OwnedAllocationScope owner(membership_memory_owner_);
        const auto start = [&] {
            window.releaseDirectory();
            const auto binding = bind(kind, 3);
            window.directory.emplace(binding.root.index_id, trees_, metadata_, BTreeLeafValueLayout::BLVL_EMPTY, binding);
            size_t limit = (std::numeric_limits<size_t>::max)() - 1U;
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
            limit = size_t((std::min)(descriptor_->scan_membership_group_count,
                uint64_t((std::numeric_limits<size_t>::max)() - 2U))) + 2U;
#endif
            window.continuation.emplace(window.directory->openFixedKeyRangeCursor(snapshot_,
                std::span<const std::byte>(key), limit, HybridPropertyGroupDirectoryMap::Record{}.size(),
                BufferPoolReadHint::BPRH_NORMAL, false, cancellation_));
            debug::PerfTrace::addValue("native_graph.hybrid.group_directory_cursor_restarts", 1);
        };
        try {
            if (!window.continuation || window.kind != kind || window.epoch != epoch ||
                (window.last && key <= *window.last)) start();
            const auto nextRecord = [&]() -> std::optional<HybridPropertyGroupDirectoryMap::Entry> {
                if (window.continuation->fixedKeyWidth()) {
                    const auto raw = window.continuation->nextFixedKey();
                    return raw ? std::optional(HybridPropertyGroupDirectoryMap::decode(*raw)) : std::nullopt;
                }
                const auto raw = window.continuation->next();
                return raw ? std::optional(HybridPropertyGroupDirectoryMap::decode(*raw)) : std::nullopt;
            };
            const auto take = [&]() -> std::optional<HybridPropertyGroupDirectoryMap::Entry> {
                check();
                auto first = window.pending;
                if (!first) {
                    first = nextRecord();
                    if (!first) return {};
                }
                // The cursor owns separator credit in the original account.
                // Do not supply an admission closure that dies with this batch.
                const auto next = nextRecord();
                require(!next || next->key != first->key, "group directory has duplicate logical ownership");
                window.pending = next; window.last = first->key;
                return first;
            };
            auto entry = take(); size_t skipped = 0;
            while (entry && entry->key < key && skipped < BatchReadWindow::GROUPS) {
                ++skipped; entry = take();
            }
            if (entry && entry->key < key) { start(); entry = take(); }
            require(!entry || entry->key >= key, "group directory continuation precedes its lower bound");
            while (entry && window.count < BatchReadWindow::GROUPS) {
                window.groups[window.count++] = *entry;
                if (window.count < BatchReadWindow::GROUPS) entry = take();
            }
            window.first = key; window.kind = kind; window.epoch = epoch;
            window.valid = membership_io_->dataMutationEpoch() == epoch;
            if (!window.valid) window.releaseDirectory();
        } catch (...) {
            window.valid = false; window.count = 0; window.releaseDirectory(); throw;
        }
    }
    std::optional<HybridPropertyGroupRecord> findGroup(EntityPropertyKey owner,
        const std::function<void(size_t)> &boundaryAdmission, SlottedSlotReference *source = nullptr,
        bool authenticateOwner = true, bool allowRemainderRead = false, FieldPageCursor *pageCursor = nullptr) const {
        check(); require(owner.isValid(), "hybrid group lookup requires a valid owner");
        if (!hasRoot(owner.owner_kind, 3)) {
            require(!hasRoot(owner.owner_kind, 0) && !hasRoot(owner.owner_kind, 1) && !hasRoot(owner.owner_kind, 2),
                "hybrid descriptor has field directories without a group directory");
            return {};
        }
        const auto epoch = membership_io_->dataMutationEpoch();
        const bool cacheable = epoch != (std::numeric_limits<uint64_t>::max)() &&
            !StorageIO::SequentialReadScope::activeFor(*membership_io_);
        const auto groupId = hybridPropertyGroupFor(owner);
        const auto ownerBit = UINT64_C(1) << hybridPropertySlotFor(owner);
        std::optional<HybridPropertyGroupRecord> decoded;
        SlottedSlotReference reference;
        uint64_t authenticatedOwners = 0;
        if (cacheable && membership_cache_) for (const auto &entry : membership_cache_->groups)
            if (entry.valid && entry.io_epoch == epoch && entry.group.owner_kind == owner.owner_kind && entry.group.group_id == groupId) {
                decoded = entry.group; reference = entry.reference; authenticatedOwners = entry.authenticated_owners;
                debug::PerfTrace::addValue("native_graph.hybrid.group_cache_hits", 1); break;
            }
        if (!decoded) {
            const auto binding = bind(owner.owner_kind, 3);
            HybridPropertyGroupDirectoryMap groups(binding.root.index_id, trees_, metadata_, binding);
            const auto key = HybridPropertyDirectoryCodec::groupKey(groupId);
            const auto locator = [&]() -> std::optional<HybridPropertyGroupDirectoryMap::Value> {
                const std::array<EntityPropertyKey, 1> selected{owner};
                if (!batch_read_window_ || !cacheable || !certifiedOwners(selected))
                    return groups.find(snapshot_, key, boundaryAdmission, cancellation_);
                auto &window = *batch_read_window_;
                const auto cached = [&]() -> std::optional<HybridPropertyGroupDirectoryMap::Value> {
                    for (size_t i = 0; i < window.count; ++i) {
                        if (window.groups[i].key == key) return window.groups[i].value;
                        if (window.groups[i].key > key) break;
                    }
                    return {};
                };
                if (window.valid && window.epoch == epoch && window.kind == owner.owner_kind &&
                    window.count && key >= window.first && key <= window.groups[window.count - 1U].key) {
                    debug::PerfTrace::addValue("native_graph.hybrid.group_directory_window_hits", 1);
                    return cached();
                }
                // At most eight records plus the existing duplicate lookahead;
                // a sparse/random request never walks an unrelated long gap.
                window.valid = false; window.count = 0;
                refillGroupDirectory(window, key, owner.owner_kind, epoch);
                debug::PerfTrace::addValue("native_graph.hybrid.group_directory_window_refills", 1);
                if (!window.valid) return groups.find(snapshot_, key, boundaryAdmission, cancellation_);
                return cached();
            }();
            if (!locator) return {};
            reference = HybridPropertyDirectoryCodec::decodeLocator(*locator);
            decoded = withFieldPage(reference.page_id, pageCursor, [&](const auto &page) {
                return page.view().readGroup(reference, owner.owner_kind, groupId, descriptor_->semantic_generation);
            });
            require(!decoded->usesColumnMembership() ||
                (snapshot_.superblock().flags & RSS_SUPERBLOCK_FLAG_HYBRID_COLUMN_MEMBERSHIP),
                "compact group is missing its native format feature");
            debug::PerfTrace::addValue("native_graph.hybrid.group_decodes", 1);
        }
        if (authenticateOwner && !(authenticatedOwners & ownerBit)) {
            const std::array<EntityPropertyKey, 1> selected{owner};
            if (certifiedOwners(selected)) {
                require(isLive(owner, *decoded), "certified Node is absent from its field group");
                debug::PerfTrace::addValue("native_graph.hybrid.certified_owner_route_checks_saved", 1);
                // Visitor authority must not escape into the ordinary read cache.
            } else {
                const auto entity = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ?
                    metadata_.findNodePageLocationOptional(snapshot_, owner.entity_id) :
                    metadata_.findEdgePageLocationOptional(snapshot_, owner.entity_id);
                require(bool(entity) == isLive(owner, *decoded), "hybrid group liveness disagrees with the Entity Map");
                require(!metadata_.findEntityPropertyLocationOptional(snapshot_, owner), "hybrid group owner still has a competing ROW root");
                authenticatedOwners |= ownerBit;
                debug::PerfTrace::addValue("native_graph.hybrid.owner_route_checks", 1);
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
                debug::PerfTrace::addValue("native_graph.hybrid.owner_route_checks_performed", 1);
#endif
            }
        }
        if (cacheable) {
            prepareReadCache();
            if (membership_cache_) {
                auto *destination = &membership_cache_->groups[membership_cache_->group_cursor % membership_cache_->groups.size()];
                bool found = false;
                for (auto &entry : membership_cache_->groups)
                    if (entry.valid && entry.io_epoch == epoch && entry.group == *decoded) { destination = &entry; found = true; break; }
                if (!found) ++membership_cache_->group_cursor;
                destination->group = *decoded; destination->reference = reference;
                destination->authenticated_owners = authenticatedOwners; destination->io_epoch = epoch; destination->valid = true;
            }
        }
        if (decoded->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER && !allowRemainderRead)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                "native cold ROW mutation lifecycle is not integrated yet");
        if (source) *source = reference;
        return decoded;
    }
    [[nodiscard]] std::optional<HybridPropertySource> selector(const HybridPropertyGroupRecord &group,
        EntityPropertyKey owner, uint32_t token, const std::function<void(size_t)> &admission = {}) const {
        check(); require(owner.owner_kind == group.owner_kind && hybridPropertyGroupFor(owner) == group.group_id,
            "hybrid selector has a foreign group or owner");
        const auto key = HybridPropertyDirectoryCodec::ownerKey(owner, token);
        std::optional<HybridPropertyOwnerMap::Value> explicitSource;
        if (hasRoot(owner.owner_kind, 0)) explicitSource = ownerMap(owner.owner_kind).find(snapshot_, key, admission, cancellation_);
        if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) {
            const bool column = withColumnMembership(group, [&](const auto &matrix) { return matrix.contains(owner, token); });
            if (explicitSource) require(!column && HybridPropertyDirectoryCodec::decodeSource(*explicitSource) == HybridPropertySource::HPS_EXTENSION,
                "compact field has two authorities or a legacy column selector");
            if (column) return HybridPropertySource::HPS_COLUMN;
        } else if (!hasRoot(owner.owner_kind, 0))
            require(!group.memberships, "hybrid group memberships have no owner directory");
        if (!explicitSource) return {};
        return HybridPropertyDirectoryCodec::decodeSource(*explicitSource);
    }
    struct AuthenticatedColumn {
        HybridPropertyColumnRecord column;
        SlottedSlotReference reference;
    };
    struct MembershipCache final : utils::OwnedPmrObject {
        struct GroupEntry {
            HybridPropertyGroupRecord group;
            SlottedSlotReference reference;
            uint64_t authenticated_owners = 0;
            uint64_t io_epoch = 0;
            bool valid = false;
        };
        std::array<GroupEntry, 2> groups{};
        size_t group_cursor = 0;
        struct Entry {
            HybridPropertyGroupRecord group;
            HybridPropertyColumnMembership value;
            bool valid = false;
            uint64_t io_epoch = 0;
        };
        std::array<Entry, 2> entries{};
        size_t cursor = 0;
        struct ColumnEntry {
            HybridPropertyGroupRecord group;
            AuthenticatedColumn value;
            uint64_t io_epoch = 0;
            bool valid = false;
        };
        explicit MembershipCache(std::pmr::memory_resource *memory, size_t count) : columns(memory) { columns.resize(count); }
        std::pmr::vector<ColumnEntry> columns;
        size_t column_cursor = 0;
        size_t column_lookup_cursor = 0;
    };
    void prepareReadCache() const {
        if (membership_cache_ || membership_cache_disabled_) return;
        if (!membership_memory_owner_) membership_cache_disabled_ = true;
        else try { membership_cache_.reset(new (membership_memory_owner_) MembershipCache(membership_memory_owner_.get(),
            full_entity_columns_)); }
        catch (const query::execution::MaterializedByteLimitExceeded &) { membership_cache_disabled_ = true; }
        catch (const query::execution::detail::ResourceAdmissionDenied &) { membership_cache_disabled_ = true; }
    }
    [[nodiscard]] const AuthenticatedColumn *cachedColumn(const HybridPropertyGroupRecord &group, uint32_t token) const {
        check();
        const auto epoch = membership_io_->dataMutationEpoch();
        if (epoch == (std::numeric_limits<uint64_t>::max)() || StorageIO::SequentialReadScope::activeFor(*membership_io_) ||
            !membership_cache_) return nullptr;
        // Full collections visit their actual tokens in the same order for
        // successive owners. Continue after the previous hit so a retained
        // F-field window does not incur F linear probes for every field.
        size_t ordinal = membership_cache_->column_lookup_cursor;
        for (size_t checked = 0; checked < membership_cache_->columns.size(); ++checked) {
            const auto &entry = membership_cache_->columns[ordinal];
            if (++ordinal == membership_cache_->columns.size()) ordinal = 0;
            if (entry.valid && entry.io_epoch == epoch && entry.group == group && entry.value.column.identity.token == token) {
                membership_cache_->column_lookup_cursor = ordinal;
                return &entry.value;
            }
        }
        return nullptr;
    }
    Enumeration visitOwnerInGroup(EntityPropertyKey owner, const HybridPropertyGroupRecord &group, size_t maximumEntries,
        const std::function<bool(uint32_t, HybridPropertySource)> &visitor,
        const std::function<void(size_t)> &boundaryAdmission) const {
        if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER)
            return visitRemainderOwner(owner, group, maximumEntries, visitor, boundaryAdmission);
        const bool live = isLive(owner, group);
        Enumeration result{true, live, true, 0};
        const auto checked = [&](uint32_t token, HybridPropertySource source) {
            check();
            if (result.fields == maximumEntries) return false;
            require(live && result.fields < group.memberships,
                "hybrid owner membership contradicts group liveness or count");
            require((source == HybridPropertySource::HPS_COLUMN &&
                group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) ||
                hasRoot(owner.owner_kind, source == HybridPropertySource::HPS_COLUMN ? 1U : 2U),
                "hybrid owner membership has no source directory");
            ++result.fields; return visitor(token, source);
        };
        if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) {
            std::array<uint32_t, HybridPropertyColumnMembership::MAX_COLUMNS> columns{}; size_t count = 0, position = 0;
            withColumnMembership(group, [&](const auto &matrix) {
                require(matrix.visitOwner(owner, columns.size(), [&](uint32_t token) { columns[count++] = token; return true; }),
                    "compact owner exceeds its bounded column tokens");
            });
            const auto remaining = [&] {
                while (position < count)
                    if (!checked(columns[position++], HybridPropertySource::HPS_COLUMN)) return false;
                return true;
            };
            if (hasRoot(owner.owner_kind, 0)) {
                const auto map = ownerMap(owner.owner_kind);
                const auto extension = [&](uint32_t token, HybridPropertySource source) {
                    require(source == HybridPropertySource::HPS_EXTENSION, "compact owner has a repeated legacy column selector");
                    while (position < count && columns[position] < token)
                        if (!checked(columns[position++], HybridPropertySource::HPS_COLUMN)) return false;
                    require(position == count || columns[position] != token, "compact owner has two field authorities");
                    return checked(token, source);
                };
                result.complete = HybridPropertyDirectoryCodec::visitOwner(map, snapshot_, owner, maximumEntries,
                    std::ref(extension), boundaryAdmission, cancellation_);
            }
            if (result.complete) result.complete = remaining();
        } else if (hasRoot(owner.owner_kind, 0)) {
            const auto map = ownerMap(owner.owner_kind);
            result.complete = HybridPropertyDirectoryCodec::visitOwner(map, snapshot_, owner, maximumEntries,
                std::ref(checked), boundaryAdmission, cancellation_);
        } else require(group.memberships == 0, "hybrid group memberships have no owner directory");
        return result;
    }
    void requireNoLegacySelector(EntityPropertyKey owner, const std::function<void(size_t)> &admission) const {
        if (!hasRoot(owner.owner_kind, 0)) return;
        const auto map = ownerMap(owner.owner_kind);
        static_cast<void>(HybridPropertyDirectoryCodec::visitOwner(map, snapshot_, owner, 1,
            [](uint32_t, HybridPropertySource) -> bool {
                require(false, "ROW-remainder owner has a competing explicit selector"); return false;
            }, admission, cancellation_));
    }
    struct ColdWindowView {
        const HybridPropertyColdRowBlock::Decoded *block;
        SlottedSlotReference reference;
    };
    class ColdWindowLease final {
        const HybridPropertyReadContext &context_;
        ColdWindowView view_;
        const ColdWindowView *previous_;
    public:
        ColdWindowLease(const HybridPropertyReadContext &context, const HybridPropertyColdRowBlock::Decoded *block,
            SlottedSlotReference reference) : context_(context), view_{block, reference}, previous_(context.cold_window_) {
            context_.cold_window_ = &view_;
        }
        ~ColdWindowLease() { context_.cold_window_ = previous_; }
        ColdWindowLease(const ColdWindowLease &) = delete;
        ColdWindowLease &operator=(const ColdWindowLease &) = delete;
    };
    // One accounted decoded block exists only during this synchronous window.
    // The same primitive serves field reading and reference-only maintenance.
    template<class Visitor>
    void withColdBlock(const HybridPropertyGroupRecord &group, const HybridPropertyColumnMembership &matrix,
        const SlottedSlotReference &reference, Visitor &&visitor) const {
        check();
        if (batch_read_window_) batch_read_window_->releaseFields();
        const auto memory = utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
            (snapshot_.readContext() ? snapshot_.readContext()->memoryOwner() : nullptr);
        require(bool(memory), "cold ROW reading requires the caller's original resource account");
        utils::OwnedAllocationScope scope(memory);
        if (cold_window_ && cold_window_->block && cold_window_->reference == reference &&
            cold_window_->block->owner_kind == group.owner_kind && cold_window_->block->group_id == group.group_id) {
            visitor(*cold_window_->block, memory); check(); return;
        }
        if (before_cold_decode_) (*before_cold_decode_)();
        // Decoded owns the accounted bytes. Release the physical page pin
        // before consumers read hot fields or immutable-value manifests.
        const auto block = [&] {
            const auto page = fields_.read(snapshot_, reference.page_id);
            return page.view().readColdRows(reference, group, matrix, descriptor_->semantic_generation, memory, cancellation_);
        }();
        debug::PerfTrace::addValue("native_graph.hybrid.cold_block_decodes", 1);
        visitor(block, memory);
        check();
    }
    template<class Visitor>
    bool withColdRow(EntityPropertyKey owner, const HybridPropertyGroupRecord &group,
        const HybridPropertyColumnMembership &matrix, Visitor &&visitor) const {
        const auto slot = hybridPropertySlotFor(owner);
        if (!(matrix.cold_rows.owners & (UINT64_C(1) << slot))) return false;
        withColdBlock(group, matrix, matrix.cold_rows.roots[slot], [&](const auto &block, const auto &memory) {
            const auto row = block.row(slot);
            require(bool(row), "cold ROW membership has no source row");
            visitor(*row, block.semantic_generation, memory);
        });
        return true;
    }
    template<class Visitor>
    bool withColdOwner(EntityPropertyKey owner, const HybridPropertyGroupRecord &group,
        const HybridPropertyColumnMembership &matrix, Visitor &&visitor, LargeValueStore *largeValues = nullptr) const {
        return withColdRow(owner, group, matrix, [&](const auto &row, Generation generation, const auto &memory) {
            const auto inspect = [&](LargeValueStore *values) {
                HybridPropertyColdRowInspection source(snapshot_, row, owner, generation,
                    std::span(matrix.tokens).first(matrix.column_count), memory, values, cancellation_);
                visitor(source);
            };
            if (row.large_value_reference && !largeValues) {
                utils::OwnedHeapReservation storeCredit(memory,
                    utils::detail::standardAllocatorRequestBytes(sizeof(SlottedPagePool), alignof(SlottedPagePool)) +
                    utils::detail::standardAllocatorRequestBytes(sizeof(BTreePageStore), alignof(BTreePageStore)));
                LargeValueStore values(source_io_, metadata_, snapshot_);
                inspect(&values);
            } else inspect(largeValues);
        });
    }
    Enumeration visitRemainderOwner(EntityPropertyKey owner, const HybridPropertyGroupRecord &group, size_t maximumEntries,
        const std::function<bool(uint32_t, HybridPropertySource)> &visitor,
        const std::function<void(size_t)> &admission) const {
        requireNoLegacySelector(owner, admission);
        Enumeration result{true, isLive(owner, group), true, 0};
        withColumnMembership(group, [&](const auto &matrix) {
            std::array<uint32_t, HybridPropertyColumnMembership::MAX_COLUMNS> hot{}; size_t count = 0, position = 0;
            require(matrix.visitOwner(owner, hot.size(), [&](uint32_t token) { hot[count++] = token; return true; }),
                "ROW-remainder owner exceeds its bounded hot columns");
            const auto emit = [&](uint32_t token, HybridPropertySource source) {
                check();
                if (result.fields == maximumEntries) return false;
                require(result.owner_live && result.fields < group.memberships, "cold ROW owner is dead or exceeds its group count");
                ++result.fields; return visitor(token, source);
            };
            withColdOwner(owner, group, matrix, [&](auto &source) {
                for (size_t i = 0; result.complete && i < source.count(); ++i) {
                    const auto token = source.token(i);
                    while (position < count && hot[position] < token)
                        if (!emit(hot[position++], HybridPropertySource::HPS_COLUMN)) { result.complete = false; break; }
                    if (result.complete && !emit(token, HybridPropertySource::HPS_COLD_ROW)) result.complete = false;
                }
            });
            while (result.complete && position < count)
                if (!emit(hot[position++], HybridPropertySource::HPS_COLUMN)) result.complete = false;
        });
        return result;
    }
    FieldRoute visitRemainderField(EntityPropertyKey owner, uint32_t token, const HybridPropertyGroupRecord &group,
        const SlottedSlotReference &groupReference, const ScalarVisitor &scalar, const EncodedVisitor &encoded,
        const std::function<void(size_t)> &admission, const ColumnVisitor &columnVisitor, LargeValueStore *largeValues) const {
        requireNoLegacySelector(owner, admission);
        return withColumnMembership(group, [&](const auto &matrix) {
            if (matrix.ordinal(token)) {
                if (!matrix.contains(owner, token)) return FieldRoute::HFR_MISSING;
                const auto selected = columnRecord(group, token, admission);
                const auto slot = hybridPropertySlotFor(owner); const auto bit = UINT64_C(1) << slot;
                require(selected.column.presence == matrix.presence(token), "ROW-remainder column disagrees with its membership");
                const HybridPropertyFieldView::ScalarCell cell{selected.column.identity.scalar_kind,
                    true, bool(selected.column.nulls & bit), selected.column.words[slot]};
                if (columnVisitor) columnVisitor(group, selected.column, groupReference, selected.reference);
                check(); scalar(cell); return FieldRoute::HFR_FOUND;
            }
            // Private hot planning grants no cold-field authority and must not
            // decode a non-hot source merely to decline this write route.
            if (columnVisitor) return FieldRoute::HFR_MISSING;
            bool found = false;
            withColdOwner(owner, group, matrix, [&](auto &source) {
                const std::array<uint32_t, 1> tokens{token};
                const auto sink = [&](size_t, std::optional<std::span<const std::byte>> bytes) {
                    check(); found = bool(bytes); if (bytes && encoded) encoded(*bytes);
                };
                source.visitSelected(tokens, std::ref(sink));
            }, largeValues);
            return found ? FieldRoute::HFR_FOUND : FieldRoute::HFR_MISSING;
        });
    }
    [[nodiscard]] AuthenticatedColumn columnRecord(const HybridPropertyGroupRecord &group, uint32_t token,
        const std::function<void(size_t)> &admission, FieldPageCursor *pageCursor = nullptr) const {
        check();
        const auto epoch = membership_io_->dataMutationEpoch();
        const bool cacheable = epoch != (std::numeric_limits<uint64_t>::max)() &&
            !StorageIO::SequentialReadScope::activeFor(*membership_io_);
        // Column callbacks retain an owning record across reentrant eviction.
        if (const auto *cached = cachedColumn(group, token)) {
            const auto value = *cached;
            debug::PerfTrace::addValue("native_graph.hybrid.column_cache_hits", 1);
            return value;
        }
        const auto reference = [&] {
            if (group.usesColumnMembership()) {
                debug::PerfTrace::addValue("native_graph.hybrid.direct_column_locator_reads", 1);
                return withColumnMembership(group, [&](const auto &matrix) { return matrix.columnLocator(token); }, pageCursor);
            }
            const auto binding = bind(group.owner_kind, 1);
            HybridPropertyFieldDirectoryMap directory(binding.root.index_id, trees_, metadata_, binding);
            const auto locator = directory.find(snapshot_, HybridPropertyDirectoryCodec::columnKey(group.group_id, token),
                admission, cancellation_);
            require(bool(locator), "hybrid field membership has no source record");
            return HybridPropertyDirectoryCodec::decodeLocator(*locator);
        }();
        const auto value = withFieldPage(reference.page_id, pageCursor, [&](const auto &page) {
            check();
            return AuthenticatedColumn{page.view().readColumn(reference, group, token, descriptor_->semantic_generation), reference};
        });
        debug::PerfTrace::addValue("native_graph.hybrid.column_decodes", 1);
        if (cacheable) {
            prepareReadCache();
            if (membership_cache_) {
                auto &entry = membership_cache_->columns[membership_cache_->column_cursor++ % membership_cache_->columns.size()];
                entry.group = group; entry.value = value; entry.io_epoch = epoch; entry.valid = true;
            }
        }
        return value;
    }
    template<class Visitor>
    auto withColumnMembership(const HybridPropertyGroupRecord &group, Visitor &&visitor, FieldPageCursor *pageCursor = nullptr) const
        -> std::invoke_result_t<Visitor &, const HybridPropertyColumnMembership &> {
        check(); require(group.usesColumnMembership() &&
            (snapshot_.superblock().flags & RSS_SUPERBLOCK_FLAG_HYBRID_COLUMN_MEMBERSHIP),
            "compact membership has no admitted source feature");
        const auto epoch = membership_io_->dataMutationEpoch();
        const bool cacheable = epoch != (std::numeric_limits<uint64_t>::max)() &&
            !StorageIO::SequentialReadScope::activeFor(*membership_io_);
        if (cacheable && membership_cache_) for (const auto &entry : membership_cache_->entries)
            if (entry.valid && entry.io_epoch == epoch && entry.group == group) {
                debug::PerfTrace::addValue("native_graph.hybrid.column_membership_cache_hits", 1);
                return visitor(std::as_const(entry.value));
            }
        const auto decoded = withFieldPage(group.column_membership.page_id, pageCursor, [&](const auto &page) {
            return page.view().readColumnMembership(group.column_membership, group, descriptor_->semantic_generation);
        });
        require(group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER ||
            hasRoot(group.owner_kind, 0) || decoded.memberships() == group.memberships,
            "compact group claims extensions without an owner directory");
        debug::PerfTrace::addValue("native_graph.hybrid.column_membership_decodes", 1);
        if (!cacheable) return visitor(std::as_const(decoded));
        prepareReadCache();
        if (!membership_cache_) return visitor(std::as_const(decoded));
        auto &entry = membership_cache_->entries[membership_cache_->cursor++ % membership_cache_->entries.size()];
        entry.group = group; entry.value = decoded; entry.valid = true; entry.io_epoch = epoch;
        return visitor(std::as_const(entry.value));
    }
    [[nodiscard]] HybridPropertyColumnMembership columnMembership(const HybridPropertyGroupRecord &group) const {
        return withColumnMembership(group, [](const auto &value) { return value; });
    }
    const MetadataSnapshotPin &snapshot_;
    RelocatableMetadataStore &metadata_;
    Generation generation_;
    mutable BTreePageStore trees_;
    const StorageIO *membership_io_ = nullptr; // owned by fields_, never escaped
    std::shared_ptr<StorageIO> source_io_;
    HybridPropertyFieldPageStore fields_;
    std::optional<HybridPropertyDescriptor> descriptor_;
    const CancellationCheckpoint *cancellation_ = nullptr;
    // Capture at context construction, never charge a later caller's temporary
    // integrity/maintenance account for metadata retained by this context.
    std::shared_ptr<std::pmr::memory_resource> membership_memory_owner_;
    mutable std::unique_ptr<MembershipCache> membership_cache_;
    mutable bool membership_cache_disabled_ = false;
    mutable size_t full_entity_columns_ = 8U;
    mutable const std::function<void()> *before_cold_decode_ = nullptr;
    mutable const ColdWindowView *cold_window_ = nullptr; // borrowed only within a synchronous stack lease
};
} // namespace graph::storage
