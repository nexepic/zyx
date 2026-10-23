from pathlib import Path
p=Path('include/graph/storage/relocatable/property/HybridPropertyReadContext.hpp')
s=p.read_text()
a=s.index('    template<class Visitor>\n    bool withColdOwner(')
b=s.index('    Enumeration visitRemainderOwner',a)
s=s[:a]+'''    // One accounted decoded block exists only during this synchronous window.
    // The same primitive serves field reading and reference-only maintenance.
    template<class Visitor>
    void withColdBlock(const HybridPropertyGroupRecord &group, const HybridPropertyColumnMembership &matrix,
        const SlottedSlotReference &reference, Visitor &&visitor) const {
        check();
        const auto memory = utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
            (snapshot_.readContext() ? snapshot_.readContext()->memoryOwner() : nullptr);
        require(bool(memory), "cold ROW reading requires the caller's original resource account");
        utils::OwnedAllocationScope scope(memory);
        if (before_cold_decode_) (*before_cold_decode_)();
        const auto page = fields_.read(snapshot_, reference.page_id);
        const auto block = page.view().readColdRows(reference, group, matrix, descriptor_->semantic_generation, memory, cancellation_);
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
                LargeValueStore values(source_io_, metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
                inspect(&values);
            } else inspect(largeValues);
        });
    }
''' +s[b:]
pos=s.index('    [[nodiscard]] Enumeration visitOwner(')
s=s[:pos]+'''    // Reference marking deliberately does not read or materialize the value's
    // content. The maintenance sink authenticates its actual manifest; full
    // integrity and field reads retain their stronger content validation.
    [[nodiscard]] bool visitColdReference(EntityPropertyKey owner,
        const std::function<void(const LargeValueRef &)> &visitor,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        require(bool(visitor), "cold reference reading requires a synchronous visitor");
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
''' +s[pos:]
p.write_text(s)
