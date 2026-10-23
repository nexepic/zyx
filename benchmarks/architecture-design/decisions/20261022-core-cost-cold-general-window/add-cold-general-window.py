from pathlib import Path
p=Path('include/graph/storage/relocatable/property/HybridPropertyReadContext.hpp');s=p.read_text();idx=s.index('    // Borrow one decoded cold body')
s=s[:idx]+'''    // The borrowed body is owned by this call's stack, not by a cache. The
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
        const auto group = findGroup(owners.front(), admission, nullptr, false, true);
        if (!group || group->mode != HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) return false;
        return withColumnMembership(*group, [&](const auto &matrix) {
            bool needed = exactCounts;
            for (const auto token : tokens) needed |= !matrix.ordinal(token).has_value();
            if (!needed) return false;
            uint64_t selected = 0;
            for (const auto owner : owners) selected |= UINT64_C(1) << hybridPropertySlotFor(owner);
            if (!(selected & matrix.cold_rows.owners)) return false;
            constexpr size_t scratch = 64U * (sizeof(uint64_t) + sizeof(std::optional<EntityPageLocation>) +
                sizeof(std::optional<EntityPropertyLocation>));
            const auto admit = [&](size_t bytes) {
                require(bytes <= (std::numeric_limits<size_t>::max)() - scratch, "cold window scratch overflow");
                if (admission) admission(bytes + scratch);
            };
            admit(0);
            std::array<uint64_t, 64> ids{};
            std::array<std::optional<EntityPageLocation>, 64> entities{};
            std::array<std::optional<EntityPropertyLocation>, 64> rows{};
            for (size_t i = 0; i < owners.size(); ++i) ids[i] = owners[i].entity_id;
            if (kind == EntityPropertyOwnerKind::EPOK_NODE)
                metadata_.findNodePageLocationsSorted(snapshot_, std::span(ids).first(owners.size()),
                    std::span(entities).first(owners.size()), cancellation_);
            else metadata_.findEdgePageLocationsSorted(snapshot_, std::span(ids).first(owners.size()),
                std::span(entities).first(owners.size()), cancellation_);
            metadata_.findEntityPropertyLocationsSorted(snapshot_, owners, std::span(rows).first(owners.size()), cancellation_);
            for (size_t i = 0; i < owners.size(); ++i) {
                check(); require(bool(entities[i]) == isLive(owners[i], *group) && !rows[i],
                    "cold window owner has invalid Entity Map or competing ROW routing");
                requireNoLegacySelector(owners[i], admit);
            }
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
        });
    }
''' +s[idx:]
idx=s.index('    // One accounted decoded block')
s=s[:idx]+'''    struct ColdWindowView {
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
''' +s[idx:]
needle='''        utils::OwnedAllocationScope scope(memory);
        if (before_cold_decode_) (*before_cold_decode_)();'''
replace='''        utils::OwnedAllocationScope scope(memory);
        if (cold_window_ && cold_window_->block && cold_window_->reference == reference &&
            cold_window_->block->owner_kind == group.owner_kind && cold_window_->block->group_id == group.group_id) {
            visitor(*cold_window_->block, memory); check(); return;
        }
        if (before_cold_decode_) (*before_cold_decode_)();'''
assert needle in s;s=s.replace(needle,replace,1)
s=s.replace('    mutable const std::function<void()> *before_cold_decode_ = nullptr;', '    mutable const std::function<void()> *before_cold_decode_ = nullptr;\n    mutable const ColdWindowView *cold_window_ = nullptr; // borrowed only within a synchronous stack lease')
p.write_text(s)
p=Path('include/graph/storage/relocatable/property/HybridPropertyValueReader.hpp');s=p.read_text();idx=s.index('            if (sink && scalars && texts && tokens.size() == 1')
s=s[:idx]+'''            if (!(sink && scalars && texts && tokens.size() == 1 && counts.empty())) {
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
                        PropertyColumnBatchReadSinks runSinks;
                        const auto scalarRun = [&](auto rows, size_t column, auto type, auto words) {
                            std::array<size_t, 64> translated{};
                            for (size_t j = 0; j < rows.size(); ++j) translated[j] = offset + rows[j];
                            producing = true; batches->scalars(std::span(translated).first(rows.size()), column, type, words);
                        };
                        const auto textRun = [&](auto rows, size_t column, auto words) {
                            std::array<size_t, 64> translated{};
                            for (size_t j = 0; j < rows.size(); ++j) translated[j] = offset + rows[j];
                            producing = true; batches->texts(std::span(translated).first(rows.size()), column, words);
                        };
                        if (batches) { runSinks.scalars = std::ref(scalarRun); runSinks.texts = std::ref(textRun); }
                        // The stack lease prevents recursive window creation.
                        // Borrow the existing output rectangle and original account.
                        const auto output = sink ? values : values.subspan(offset * tokens.size(), length * tokens.size());
                        const auto outputCounts = counts.empty() ? counts : counts.subspan(offset, length);
                        const bool complete = readSelected(context, snapshot, owners.subspan(offset, length), tokens,
                            output, outputCounts, largeValues, admission, sink ? &owningSink : nullptr,
                            scalars ? &scalarSink : nullptr, texts ? &textSink : nullptr, batches ? &runSinks : nullptr);
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
''' +s[idx:];p.write_text(s)
