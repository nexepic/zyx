/**
 * @file HybridPropertyValueReader.hpp
 * @brief Owning-value compatibility boundary for native hybrid field reads.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <bit>
#include <memory_resource>
#include <vector>
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
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
        // Count, admission passes and value construction borrow the same cold
        // body. A completed collection leaves the lease before escaping.
        const std::array<EntityPropertyKey, 1> selected{owner};
        std::optional<CollectionResult> windowResult;
        bool producing = false, consumed = false;
        try { consumed = context.visitColdWindowSorted(selected, {}, true, [&](size_t, size_t) {
            producing = true;
            windowResult = readCollection(context, snapshot, owner, ownerType, largeValues, admission, slotsAdmission);
        }, std::ref(boundary));
        } catch (const query::execution::MaterializedByteLimitExceeded &) {
            if (producing) throw;
        } catch (const query::execution::detail::ResourceAdmissionDenied &) {
            if (producing) throw;
        }
        boundaryCredit.set(0);
        if (consumed) {
            hybrid_property_detail::require(bool(windowResult), "cold collection window produced no result");
            return std::move(*windowResult);
        }
#endif
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
                case HybridPropertyScalarKind::HPSK_SHORT_TEXT7: return PropertyValue(HybridPropertyShortText::decode(cell.word));
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
        const PropertyScalarReadSink *scalars = nullptr, const PropertyTextReadSink *texts = nullptr,
        const PropertyColumnBatchReadSinks *batches = nullptr) {
        hybrid_property_detail::require(context.usesSnapshot(snapshot), "hybrid selected batch requires its original pin");
        hybrid_property_detail::require((tokens.empty() || owners.size() <= (std::numeric_limits<size_t>::max)() / tokens.size()) &&
            (sink ? (bool(*sink) && values.empty()) : values.size() == owners.size() * tokens.size()) && (counts.empty() || counts.size() == owners.size()),
            "hybrid selected batch requires rectangular outputs");
        hybrid_property_detail::require(!scalars || (sink && bool(*scalars)), "hybrid scalar cells require an owning fallback sink");
        hybrid_property_detail::require(!batches || (sink && scalars && texts && batches->scalars && batches->texts),
            "hybrid runs require typed and owning fallbacks");
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
        const auto batchWindow = context.borrowColumnBatchWindow(
            batches && owners.size() <= 512U && tokens.size() <= HybridPropertyColumnMembership::MAX_COLUMNS);
        utils::OwnedHeapReservation boundaryCredit(memory), referenceCredit(memory);
        const auto boundary = [&](size_t bytes) { boundaryCredit.set(bytes); };
        bool allHybrid = true;
        [[maybe_unused]] size_t attemptedGroupEnd=0;
        for (size_t i = 0; i < owners.size(); ++i) {
            context.checkpoint();
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
            if (!(sink && scalars && texts && tokens.size() == 1 && counts.empty())) {
                size_t end = i + 1;
                while (end < owners.size() && owners[end].owner_kind == owners[i].owner_kind &&
                    hybridPropertyGroupFor(owners[end]) == hybridPropertyGroupFor(owners[i])) ++end;
                bool producing = false, consumed = false;
                try { consumed = context.visitColdWindowSorted(owners.subspan(i, end - i), tokens, !counts.empty(),
                    [&](size_t first, size_t last) {
                        const auto offset = i + first, length = last - first;
                        const auto owning = [&](size_t row, size_t column, auto value) {
                            producing = true; (*sink)(offset + row, column, std::move(value));
                        };
                        const auto scalar = [&](size_t row, size_t column, auto type, uint64_t word) {
                            producing = true; (*scalars)(offset + row, column, type, word);
                        };
                        const auto text = [&](size_t row, size_t column, std::string_view value) {
                            producing = true; (*texts)(offset + row, column, value);
                        };
                        const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owningSink = std::ref(owning);
                        const PropertyScalarReadSink scalarSink = std::ref(scalar);
                        const PropertyTextReadSink textSink = std::ref(text);
                        // The stack lease prevents recursive window creation.
                        // Borrow the existing output rectangle and original account.
                        const auto output = sink ? values : values.subspan(offset * tokens.size(), length * tokens.size());
                        const auto outputCounts = counts.empty() ? counts : counts.subspan(offset, length);
                        if (!sink || !counts.empty()) producing = true;
                        const bool complete = readSelected(context, snapshot, owners.subspan(offset, length), tokens,
                            output, outputCounts, largeValues, admission, sink ? &owningSink : nullptr,
                            scalars ? &scalarSink : nullptr, texts ? &textSink : nullptr, nullptr);
                        hybrid_property_detail::require(complete, "cold window changed native routing");
                        // Owning rectangles/counts are also observable output.
                        producing = true;
                    }, std::ref(boundary));
                } catch (const query::execution::MaterializedByteLimitExceeded &) {
                    if (producing) throw;
                } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                    if (producing) throw;
                }
                boundaryCredit.set(0);
                if (consumed) { i = end - 1; continue; }
            }
            if (sink && scalars && texts && tokens.size() == 1 && counts.empty()) {
                size_t end = i + 1;
                while (end < owners.size() && owners[end].owner_kind == owners[i].owner_kind &&
                    hybridPropertyGroupFor(owners[end]) == hybridPropertyGroupFor(owners[i])) ++end;
                // No decline/retry after a callback: exceptions propagate with
                // their existing ordinal and release the sole decoded body.
                bool consumed = false, producing = false;
                try { consumed = context.visitColdOwnersSorted(owners.subspan(i, end - i), tokens.front(),
                    [&](size_t relative, std::optional<std::span<const std::byte>> bytes) {
                        producing = true;
                        const auto ordinal = i + relative;
                        if (!bytes) { (*sink)(ordinal, 0, std::nullopt); return; }
                        if (admission) (*admission)(sizeof(uint32_t) + bytes->size());
                        const auto primitive = CanonicalPropertyValueCodec::tryBorrowPrimitive(*bytes);
                        context.checkpoint();
                        if (primitive) {
                            if (primitive->type == PropertyType::STRING) (*texts)(ordinal, 0, primitive->text);
                            else (*scalars)(ordinal, 0, primitive->type, primitive->bits);
                        } else {
                            size_t offset = 0;
                            auto value = CanonicalPropertyValueCodec::decodeNext(*bytes, offset);
                            hybrid_property_detail::require(offset == bytes->size(), "cold grouped value has trailing bytes");
                            (*sink)(ordinal, 0, PropertyCollectionRootEntry{tokens.front(), std::move(value)});
                        }
                    }, std::ref(boundary), largeValues);
                } catch (const query::execution::MaterializedByteLimitExceeded &) {
                    if (producing) throw;
                } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                    if (producing) throw;
                }
                boundaryCredit.set(0);
                if (consumed) { i = end - 1; continue; }
            }
            if(batches && !tokens.empty() && tokens.size() <= HybridPropertyColumnMembership::MAX_COLUMNS &&
                counts.empty() && i>=attemptedGroupEnd) {
                size_t end=i+1;
                while(end<owners.size() && owners[end].owner_kind==owners[i].owner_kind &&
                    hybridPropertyGroupFor(owners[end])==hybridPropertyGroupFor(owners[i]))++end;
                attemptedGroupEnd=end;
                bool consumed=false,producing=false;
                const auto emitColumn = [&](const HybridPropertyColumnRecord &column, size_t outputColumn) {
                        producing=true; // callback failures may never replay this group
                        std::array<size_t,64> ordinals{};std::array<uint64_t,64> words{};
                        std::array<std::array<char,HybridPropertyShortText::MAX_BYTES>,64> buffers{};
                        std::array<std::string_view,64> text{};
                        const auto typeAt=[&](size_t row){
                            const auto bit=UINT64_C(1)<<hybridPropertySlotFor(owners[row]);
                            if(column.nulls&bit)return PropertyType::NULL_TYPE;
                            switch(column.identity.scalar_kind) {
                                case HybridPropertyScalarKind::HPSK_INT64:return PropertyType::INTEGER;
                                case HybridPropertyScalarKind::HPSK_DOUBLE:return PropertyType::DOUBLE;
                                case HybridPropertyScalarKind::HPSK_BOOL:return PropertyType::BOOLEAN;
                                case HybridPropertyScalarKind::HPSK_SHORT_TEXT7:return PropertyType::STRING;
                            }
                            throw FormatException(FormatError::FERR_INVALID_ENUM,"unknown grouped column kind");
                        };
                        size_t first=i;
                        while(first<end) {
                            const auto bit=UINT64_C(1)<<hybridPropertySlotFor(owners[first]);
                            if(!(column.presence&bit)) {context.checkpoint();(*sink)(first++,outputColumn,std::nullopt);continue;}
                            const auto type=typeAt(first);size_t count=0, admittedBytes=0;
                            while(first+count<end) {
                                const auto row=first+count;const auto slot=hybridPropertySlotFor(owners[row]);
                                if(!(column.presence&(UINT64_C(1)<<slot)) || typeAt(row)!=type)break;
                                if((count%(type==PropertyType::STRING?4U:16U))==0)context.checkpoint();
                                ordinals[count]=row;words[count]=column.words[slot];
                                size_t payload=type==PropertyType::NULL_TYPE?0U:type==PropertyType::BOOLEAN?1U:8U;
                                if(type==PropertyType::STRING) {text[count]=HybridPropertyShortText::copy(words[count],buffers[count]);payload=text[count].size();}
                                // At most 64 fixed/short-text cells: this sum is bounded
                                // by the persisted group format, with no heap allocation.
                                admittedBytes += sizeof(uint32_t)+CanonicalPropertyValueCodec::HEADER_BYTES+payload;
                                ++count;
                            }
                            // Reserve the complete run before exposing any value. The
                            // callback may cancel/reenter, so recheck the source boundary.
                            if(admission) {
                                (*admission)(admittedBytes);
                                debug::PerfTrace::addValue("native_graph.hybrid.typed_run_admissions", 1);
                                debug::PerfTrace::addValue("native_graph.hybrid.typed_run_admitted_bytes", static_cast<int64_t>(admittedBytes));
                            }
                            context.checkpoint();
                            if(type==PropertyType::STRING)(*batches).texts(std::span(ordinals).first(count),outputColumn,std::span(text).first(count));
                            else (*batches).scalars(std::span(ordinals).first(count),outputColumn,type,std::span(words).first(count));
                            context.checkpoint();first+=count;
                        }
                };
                try {
                    if (tokens.size() == 1U) {
                        consumed = context.visitColumnOwnersSorted(owners.subspan(i,end-i),tokens.front(),
                            [&](const auto &,const auto &column,const auto &,const auto &){emitColumn(column,0U);},
                            std::ref(boundary));
                    } else {
                        // All requested columns must be admitted/authenticated
                        // before any callback. A mixed/cold field can then
                        // decline the whole group without replaying a prefix.
                        std::pmr::vector<HybridPropertyColumnRecord> columns(
                            memory ? memory.get() : std::pmr::get_default_resource());
                        columns.reserve(tokens.size());
                        consumed = true;
                        for (const auto token : tokens) {
                            if (!context.visitColumnOwnersSorted(owners.subspan(i,end-i),token,
                                [&](const auto &,const auto &column,const auto &,const auto &){columns.push_back(column);},
                                std::ref(boundary))) { consumed=false; break; }
                        }
                        if (consumed) {
                            hybrid_property_detail::require(columns.size()==tokens.size(),
                                "grouped field preflight changed its bounded column count");
                            for (size_t column=0;column<columns.size();++column) emitColumn(columns[column],column);
                            debug::PerfTrace::addValue("native_graph.hybrid.grouped_multi_column_batches",1);
                        }
                    }
                } catch(const query::execution::MaterializedByteLimitExceeded &) {
                    if(producing)throw;
                    debug::PerfTrace::addValue("native_graph.hybrid.grouped_column_declined_admission",1);
                } catch(const query::execution::detail::ResourceAdmissionDenied &) {
                    if(producing)throw;
                    debug::PerfTrace::addValue("native_graph.hybrid.grouped_column_declined_admission",1);
                }
                boundaryCredit.set(0);
                if(consumed) {i=end-1;continue;}
            }
#endif
            std::optional<bool> hasFields;
            if (counts.empty()) {
                // Selected-only consumers need root presence, not a full field
                // count. Stop after the first actual membership; unrequested
                // fields remain the responsibility of their reads/integrity.
                hasFields = context.hasFields(owners[i], std::ref(boundary));
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
                        if(!cell.is_null && cell.kind==HybridPropertyScalarKind::HPSK_SHORT_TEXT7) {
                            if(!texts)return; // legacy consumers retain the owning boundary
                            if(admission)(*admission)(sizeof(uint32_t)+CanonicalPropertyValueCodec::HEADER_BYTES+(cell.word>>56U));
                            context.checkpoint();
                            std::array<char,HybridPropertyShortText::MAX_BYTES> buffer{};
                            // Never queue this stack view: consume it synchronously.
                            (*texts)(i,column,HybridPropertyShortText::copy(cell.word,buffer));
                            emitted=true;return;
                        }
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
                    const auto encoded = [&](std::span<const std::byte> bytes) {
                        const auto primitive = CanonicalPropertyValueCodec::tryBorrowPrimitive(bytes);
                        if (!primitive || (primitive->type == PropertyType::STRING && !texts)) return;
                        if (admission) (*admission)(sizeof(uint32_t) + bytes.size());
                        context.checkpoint();
                        if (primitive->type == PropertyType::STRING) (*texts)(i, column, primitive->text);
                        else (*scalars)(i, column, primitive->type, primitive->bits);
                        emitted = true;
                    };
                    const auto route = context.visitField(owners[i], tokens[column], std::ref(scalar),
                        [](const HybridPropertyExtensionView &) {}, std::ref(boundary), std::ref(reference),
                        {}, std::ref(encoded), largeValues);
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
        const auto encoded = [&](std::span<const std::byte> bytes) {
            boundaryCredit.set(0);
            present = !requireNonNull || !CanonicalPropertyValueCodec::isNullHeader(bytes, bytes.size());
        };
        const auto route = context.visitField(owner, token, std::ref(scalar), std::ref(extension),
            std::ref(boundary), std::ref(reference), {}, std::ref(encoded), largeValues);
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
                    case HybridPropertyScalarKind::HPSK_SHORT_TEXT7: value = PropertyValue(HybridPropertyShortText::decode(cell.word)); break;
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
        const auto encoded = [&](std::span<const std::byte> bytes) {
            boundaryCredit.set(0);
            if (admission) (*admission)(sizeof(uint32_t) + bytes.size());
            if (mode == ReadMode::HRM_ADMISSION_ONLY) return;
            size_t offset = 0;
            auto value = CanonicalPropertyValueCodec::decodeNext(bytes, offset);
            hybrid_property_detail::require(offset == bytes.size(), "cold ROW selected value has trailing bytes");
            result.value = PropertyCollectionRootEntry{token, std::move(value)};
        };
        result.route = context.visitField(owner, token, std::ref(scalar), std::ref(extension),
            std::ref(boundary), std::ref(reference), {}, std::ref(encoded), largeValues);
        return result;
    }
};
} // namespace graph::storage
