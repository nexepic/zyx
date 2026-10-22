/**
 * @file HybridPropertyValueReader.hpp
 * @brief Owning-value compatibility boundary for native hybrid field reads.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <bit>
#include "graph/storage/PropertyScalarReadSink.hpp"
#include "graph/storage/detail/ScopedPropertyBuffers.hpp"
#include "graph/core/PropertyDecodeAllocationScope.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/storage/relocatable/property/HybridPropertyReadContext.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/HybridPropertyLargeFieldInspection.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"

namespace graph::storage {
// Only compatibility consumers materialize PropertyValue here. Typed batches
// consume ReadContext's ScalarCell directly. Output heap admission remains in
// the caller's PropertyDecodeAllocationScope until the owning values die;
// local reservations cover only temporary boundaries/reference/read buffers.
class HybridPropertyValueReader final {
public:
    enum class ReadMode : uint8_t { HRM_VALUE = 1, HRM_ADMISSION_ONLY };
    struct Result {
        HybridPropertyReadContext::FieldRoute route = HybridPropertyReadContext::FieldRoute::HFR_ROW;
        std::optional<PropertyCollectionRootEntry> value;
    };
    struct CollectionResult {
        bool routed = false;
        std::optional<PropertyCollectionRoot> value;
    };
    // Keep the bounded token list separate from the returned entry slots. Every
    // field is then authenticated through the same pinned point-read boundary.
    [[nodiscard]] static CollectionResult readCollection(const HybridPropertyReadContext &context,
        const MetadataSnapshotPin &snapshot, EntityPropertyKey owner, uint32_t ownerType,
        LargeValueStore *largeValues = nullptr, const SelectedPropertyReadAdmission *admission = nullptr,
        const SelectedPropertyReadAdmission *slotsAdmission = nullptr) {
        hybrid_property_detail::require(context.usesSnapshot(snapshot), "hybrid collection requires its original pin");
        const auto memory = utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
            (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr);
        utils::OwnedHeapReservation tokenCredit(memory), boundaryCredit(memory);
        const auto tokensAdmission = [&](size_t bytes) { tokenCredit.set(bytes); };
        const auto boundary = [&](size_t bytes) { boundaryCredit.set(bytes); };
        const auto tokens = context.tokens(owner, tokensAdmission, boundary);
        if (!tokens) return {};
        if (tokens->empty()) return {true, std::nullopt};
        context.enableFullEntityWindow(tokens->size());
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
        std::array<HybridPropertyFieldView::ScalarCell, HybridPropertyColumnMembership::MAX_COLUMNS> cells{};
        const bool fullColumns = context.fullColumnCells(owner, *tokens, cells, std::ref(boundary));
        boundaryCredit.set(0);
        const auto scalarValue = [](const HybridPropertyFieldView::ScalarCell &cell) {
            if (cell.is_null) return PropertyValue{};
            switch (cell.kind) {
                case HybridPropertyScalarKind::HPSK_INT64: return PropertyValue(std::bit_cast<int64_t>(cell.word));
                case HybridPropertyScalarKind::HPSK_DOUBLE: return PropertyValue(std::bit_cast<double>(cell.word));
                case HybridPropertyScalarKind::HPSK_BOOL: return PropertyValue(bool(cell.word));
                default: throw FormatException(FormatError::FERR_INVALID_ENUM, "unknown hybrid scalar kind");
            }
        };
        if (fullColumns && admission) for (size_t i = 0; i < tokens->size(); ++i) {
            context.checkpoint();
            (*admission)(sizeof(uint32_t) + CanonicalPropertyValueCodec::encodedSize(scalarValue(cells[i])));
        }
#endif
        // Match ROW full-root semantics: admit every encoded entry before
        // allocating any decoded variable-sized value or returned entry slots.
        if (admission
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
            && !fullColumns
#endif
        ) for (const auto token : *tokens) {
            auto field = read(context, snapshot, owner, token, largeValues, admission, ReadMode::HRM_ADMISSION_ONLY);
            hybrid_property_detail::require(field.route == HybridPropertyReadContext::FieldRoute::HFR_FOUND,
                "hybrid admission membership has no field");
        }
        PropertyCollectionRoot root{owner.entity_id, ownerType, {}};
        if (tokens->size() > (std::numeric_limits<size_t>::max)() / sizeof(PropertyCollectionRootEntry))
            throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "hybrid collection slot capacity overflowed");
        if (slotsAdmission) (*slotsAdmission)(tokens->size() * sizeof(PropertyCollectionRootEntry));
        else detail::admitScopedPropertyVectorGrowth(root.entries, tokens->size());
        root.entries.reserve(tokens->size());
        if (slotsAdmission) (*slotsAdmission)(root.entries.capacity() * sizeof(PropertyCollectionRootEntry));
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
        size_t ordinal = 0;
#endif
        for (const auto token : *tokens) {
            context.checkpoint();
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
            if (fullColumns) {
                root.entries.push_back({token, scalarValue(cells[ordinal++])});
                continue;
            }
#endif
            auto field = read(context, snapshot, owner, token, largeValues);
            hybrid_property_detail::require(field.route == HybridPropertyReadContext::FieldRoute::HFR_FOUND && field.value,
                "hybrid membership has no field within its pinned collection");
            root.entries.push_back(std::move(*field.value));
        }
        return {true, std::move(root)};
    }
    // Writes only hybrid owners. ROW outputs stay untouched so the caller's
    // existing ordered metadata merge can fill them without an extra mask or
    // copying the owner array. True allows an all-hybrid batch to skip that merge.
    [[nodiscard]] static bool readSelected(const HybridPropertyReadContext &context,
        const MetadataSnapshotPin &snapshot, std::span<const EntityPropertyKey> owners,
        std::span<const uint32_t> tokens, std::span<std::optional<PropertyCollectionRootEntry>> values,
        std::span<std::optional<size_t>> counts = {}, LargeValueStore *largeValues = nullptr,
        const SelectedPropertyReadAdmission *admission = nullptr,
        const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> *sink = nullptr,
        const PropertyScalarReadSink *scalars = nullptr) {
        hybrid_property_detail::require(context.usesSnapshot(snapshot), "hybrid selected batch requires its original pin");
        hybrid_property_detail::require((tokens.empty() || owners.size() <= (std::numeric_limits<size_t>::max)() / tokens.size()) &&
            (sink ? (bool(*sink) && values.empty()) : values.size() == owners.size() * tokens.size()) && (counts.empty() || counts.size() == owners.size()),
            "hybrid selected batch requires rectangular outputs");
        hybrid_property_detail::require(!scalars || (sink && bool(*scalars)), "hybrid scalar cells require an owning fallback sink");
        uint64_t previousOwner = 0;
        for (const auto owner : owners) {
            hybrid_property_detail::require(owner.isValid() && owner.directoryId() > previousOwner,
                "hybrid selected owners must be strictly ordered");
            previousOwner = owner.directoryId();
        }
        uint32_t previousToken = 0;
        for (const auto token : tokens) {
            hybrid_property_detail::require(token && token > previousToken, "hybrid selected tokens must be strictly ordered");
            previousToken = token;
        }
        const auto memory = utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
            (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr);
        utils::OwnedHeapReservation boundaryCredit(memory), referenceCredit(memory);
        const auto boundary = [&](size_t bytes) { boundaryCredit.set(bytes); };
        bool allHybrid = true;
        for (size_t i = 0; i < owners.size(); ++i) {
            context.checkpoint();
            std::optional<bool> hasFields;
            if (counts.empty()) {
                // Selected-only consumers need root presence, not a full field
                // count. Stop after the first actual membership; unrequested
                // fields remain the responsibility of their reads/integrity.
                const auto first = context.visitOwner(owners[i], 1,
                    [](uint32_t, HybridPropertySource) { return false; }, std::ref(boundary));
                if (first.routed) hasFields = first.fields != 0;
            } else {
                const auto count = context.count(owners[i], std::ref(boundary));
                if (count) { counts[i] = *count; hasFields = *count != 0; }
            }
            boundaryCredit.set(0);
            if (!hasFields.has_value()) { allHybrid = false; continue; }
            for (size_t column = 0; column < tokens.size(); ++column) {
                if (scalars) {
                    bool emitted = false;
                    const auto scalar = [&](const HybridPropertyFieldView::ScalarCell &cell) {
                        boundaryCredit.set(0);
                        PropertyType type = PropertyType::NULL_TYPE;
                        size_t payloadBytes = 0;
                        if (!cell.is_null) switch (cell.kind) {
                            case HybridPropertyScalarKind::HPSK_INT64: type = PropertyType::INTEGER; payloadBytes = 8; break;
                            case HybridPropertyScalarKind::HPSK_DOUBLE: type = PropertyType::DOUBLE; payloadBytes = 8; break;
                            case HybridPropertyScalarKind::HPSK_BOOL: type = PropertyType::BOOLEAN; payloadBytes = 1; break;
                            default: throw FormatException(FormatError::FERR_INVALID_ENUM, "unknown hybrid scalar kind");
                        }
                        if (admission) (*admission)(sizeof(uint32_t) + CanonicalPropertyValueCodec::HEADER_BYTES + payloadBytes);
                        context.checkpoint();
                        (*scalars)(i, column, type, cell.is_null ? 0U : cell.word);
                        emitted = true;
                    };
                    const auto reference = [&](size_t bytes) { boundaryCredit.set(0); referenceCredit.set(bytes); };
                    const auto route = context.visitField(owners[i], tokens[column], std::ref(scalar),
                        [](const HybridPropertyExtensionView &) {}, std::ref(boundary), std::ref(reference));
                    boundaryCredit.set(0); referenceCredit.set(0);
                    hybrid_property_detail::require(route != HybridPropertyReadContext::FieldRoute::HFR_ROW,
                        "hybrid scalar owner changed routing within its pinned batch");
                    if (emitted) continue;
                    if (route == HybridPropertyReadContext::FieldRoute::HFR_MISSING) {
                        context.checkpoint();
                        if (*hasFields) (*sink)(i, column, std::nullopt);
                        continue;
                    }
                    // Extensions retain the existing authenticated owning-value
                    // decoder. No unrelated field is materialized for a column.
                }
                auto field = read(context, snapshot, owners[i], tokens[column], largeValues, admission);
                hybrid_property_detail::require(field.route != HybridPropertyReadContext::FieldRoute::HFR_ROW,
                    "hybrid selected owner changed routing within its pinned batch");
                if (sink) {
                    context.checkpoint();
                    if (*hasFields) (*sink)(i, column, std::move(field.value));
                } else values[i * tokens.size() + column] = std::move(field.value);
            }
        }
        return allHybrid;
    }
    // A disengaged result means ROW routing. Presence alone authenticates the
    // membership and extension reference; non-NULL inspection additionally
    // follows/authenticates a LargeValue, without materializing its value tree.
    [[nodiscard]] static std::optional<bool> exists(const HybridPropertyReadContext &context,
        const MetadataSnapshotPin &snapshot, EntityPropertyKey owner, uint32_t token,
        bool requireNonNull = false, LargeValueStore *largeValues = nullptr) {
        hybrid_property_detail::require(context.usesSnapshot(snapshot), "hybrid presence read requires its original pin");
        const auto memory = utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
            (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr);
        utils::OwnedHeapReservation boundaryCredit(memory), referenceCredit(memory);
        const auto boundary = [&](size_t bytes) { boundaryCredit.set(bytes); };
        const auto reference = [&](size_t bytes) { boundaryCredit.set(0); referenceCredit.set(bytes); };
        bool present = false;
        const auto scalar = [&](const HybridPropertyFieldView::ScalarCell &cell) {
            present = !requireNonNull || !cell.is_null;
        };
        const auto extension = [&](const HybridPropertyExtensionView &field) {
            boundaryCredit.set(0);
            if (!requireNonNull) { present = true; return; }
            if (field.mode() == HybridPropertyExtensionMode::HPEM_INLINE) {
                present = !CanonicalPropertyValueCodec::isNullHeader(field.payload(), field.payload().size());
                return;
            }
            if (!largeValues)
                throw FormatException(FormatError::FERR_INVALID_STATE, "hybrid non-NULL read requires its immutable value store");
            HybridPropertyLargeFieldInspection inspection(context, snapshot, field, *largeValues, memory);
            present = !inspection.isNull();
        };
        const auto route = context.visitField(owner, token, std::ref(scalar), std::ref(extension),
            std::ref(boundary), std::ref(reference));
        if (route == HybridPropertyReadContext::FieldRoute::HFR_ROW) return {};
        return route == HybridPropertyReadContext::FieldRoute::HFR_FOUND && present;
    }
    [[nodiscard]] static Result read(const HybridPropertyReadContext &context,
        const MetadataSnapshotPin &snapshot, EntityPropertyKey owner, uint32_t token,
        LargeValueStore *largeValues = nullptr, const SelectedPropertyReadAdmission *admission = nullptr, ReadMode mode = ReadMode::HRM_VALUE) {
        hybrid_property_detail::require(context.usesSnapshot(snapshot), "hybrid value decode requires the context's original live pin");
        const auto memory = utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
            (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr);
        utils::OwnedHeapReservation boundaryCredit(memory), referenceCredit(memory);
        const auto boundary = [&](size_t bytes) { boundaryCredit.set(bytes); };
        const auto reference = [&](size_t bytes) { boundaryCredit.set(0); referenceCredit.set(bytes); };
        Result result;
        const auto scalar = [&](const HybridPropertyFieldView::ScalarCell &cell) {
            boundaryCredit.set(0);
            PropertyValue value;
            if (!cell.is_null) {
                switch (cell.kind) {
                    case HybridPropertyScalarKind::HPSK_INT64: value = PropertyValue(std::bit_cast<int64_t>(cell.word)); break;
                    case HybridPropertyScalarKind::HPSK_DOUBLE: value = PropertyValue(std::bit_cast<double>(cell.word)); break;
                    case HybridPropertyScalarKind::HPSK_BOOL: value = PropertyValue(bool(cell.word)); break;
                    default: throw FormatException(FormatError::FERR_INVALID_ENUM, "unknown hybrid scalar kind");
                }
            }
            if (admission) (*admission)(sizeof(uint32_t) + CanonicalPropertyValueCodec::encodedSize(value));
            if (mode == ReadMode::HRM_VALUE) result.value = PropertyCollectionRootEntry{token, std::move(value)};
        };
        const auto extension = [&](const HybridPropertyExtensionView &field) {
            boundaryCredit.set(0);
            if (field.mode() == HybridPropertyExtensionMode::HPEM_INLINE) {
                if (admission) (*admission)(sizeof(uint32_t) + field.payload().size());
                size_t offset = 0;
                if (mode == ReadMode::HRM_ADMISSION_ONLY) {
                    static_cast<void>(CanonicalPropertyValueCodec::inspectNext(field.payload(), offset));
                    hybrid_property_detail::require(offset == field.payload().size(), "hybrid inline field has trailing bytes");
                    return;
                }
                auto value = CanonicalPropertyValueCodec::decodeNext(field.payload(), offset);
                hybrid_property_detail::require(offset == field.payload().size(), "hybrid inline field has trailing bytes");
                if (mode == ReadMode::HRM_VALUE) result.value = PropertyCollectionRootEntry{token, std::move(value)};
                return;
            }
            if (!largeValues)
                throw FormatException(FormatError::FERR_INVALID_STATE, "hybrid large field requires its immutable value store");
            hybrid_property_detail::require(bool(field.reference()), "hybrid large field has no immutable reference");
            if (mode == ReadMode::HRM_ADMISSION_ONLY) {
                HybridPropertyLargeFieldInspection inspection(context, snapshot, field, *largeValues, memory);
                const auto shape = inspection.inspect();
                if (admission) (*admission)(sizeof(uint32_t) + shape.encoded_bytes);
                return;
            }
            utils::OwnedHeapReservation readCredit(memory), directoryCredit(memory);
            const auto admitRead = [&](size_t bytes) { readCredit.set(bytes); };
            const auto payload = largeValues->read(snapshot, *field.reference(), std::ref(admitRead));
            const auto admitValue = [&](const CanonicalPropertyValueCodec::DecodeShape &shape) {
                if (admission) (*admission)(sizeof(uint32_t) + shape.encoded_bytes);
                if (auto *scope = property_decode::AllocationScope::current()) scope->admit(shape.maximum_heap_request_bytes);
            };
            const auto admitDirectory = [&](size_t bytes) { directoryCredit.set(bytes); };
            const auto checkpoint = [&] {
                context.checkpoint();
                if (auto *scope = property_decode::AllocationScope::current()) scope->checkpoint();
            };
            result.value = field.decodeLargePayload(payload, std::ref(admitValue), std::ref(admitDirectory), std::ref(checkpoint));
        };
        result.route = context.visitField(owner, token, std::ref(scalar), std::ref(extension),
            std::ref(boundary), std::ref(reference));
        return result;
    }
};
} // namespace graph::storage
