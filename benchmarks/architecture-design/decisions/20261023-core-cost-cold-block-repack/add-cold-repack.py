from pathlib import Path
h=Path('include/graph/storage/relocatable/property/HybridPropertyColdRowBlock.hpp');s=h.read_text();pos=s.index('    // All declared bounds/checksums')
s=s[:pos]+'''    // Private byte preparation only: caller authenticates source storage and
    // manifests, and atomically installs bindings/retirement. Changes replace
    // complete owner roots; empty root/count removes an owner. Empty output
    // means the last row was removed. Oversize output refuses without a prefix.
    [[nodiscard]] static utils::OwnedVector<std::byte> repack(std::span<const std::byte> source,
        EntityPropertyOwnerKind kind, uint64_t group, Generation visibleGeneration, Generation targetGeneration,
        std::span<const HybridColdRowInput> changes, std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr);
'''+s[pos:];h.write_text(s)
p=Path('src/storage/relocatable/property/HybridPropertyColdRowBlock.cpp');s=p.read_text();pos=s.rindex('\n}')
s=s[:pos]+'''
utils::OwnedVector<std::byte> HybridPropertyColdRowBlock::repack(std::span<const std::byte> source,
    EntityPropertyOwnerKind kind, uint64_t group, Generation visibleGeneration, Generation targetGeneration,
    std::span<const HybridColdRowInput> changes, std::shared_ptr<std::pmr::memory_resource> memory,
    const CancellationCheckpoint *cancellation) {
    require(targetGeneration.isValid() && targetGeneration > visibleGeneration &&
        !changes.empty() && changes.size() <= MAX_ROWS, "cold repack requires a bounded change set and next generation");
    auto decoded = decode(source, kind, group, visibleGeneration, memory, cancellation);
    // Authenticate reference identity against the ORIGINAL semantic generation,
    // including replaced/deleted rows. Re-encoding under the new generation
    // must never make an originally future reference appear legitimate.
    const auto validateReference = [&](const HybridColdRowInput &row, Generation fence) {
        if (!row.large_value_reference) return;
        utils::OwnedHeapReservation scratch(memory, row.root.size() +
            3U * utils::detail::standardContainerProxyBytes());
        const auto reference = EntityPropertyLargeValueReferenceCodec::decode(row.root);
        require(reference.value.generation <= fence && reference.value.kind == LargeValueKind::LVK_BYTES &&
            reference.value.media_type == RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE &&
            reference.value.logical_bytes >= RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES,
            "cold repack reference has invalid media, length or original generation");
    };
    std::array<HybridColdRowInput, MAX_ROWS> rows{};
    uint64_t owners = decoded.owners, changed = 0;
    for (unsigned slot = 0; slot < MAX_ROWS; ++slot) {
        check(cancellation);
        if (const auto row = decoded.row(slot)) {
            validateReference(*row, decoded.semantic_generation); rows[slot] = *row;
        }
    }
    for (const auto &row : changes) {
        check(cancellation);
        require(row.owner_slot < MAX_ROWS, "cold repack change slot is outside the group");
        const auto bit = UINT64_C(1) << row.owner_slot;
        require(!(changed & bit), "cold repack changes duplicate an owner"); changed |= bit;
        if (row.root.empty()) {
            require(!row.field_count && !row.large_value_reference && (owners & bit),
                "cold repack removal must name an existing row"); owners &= ~bit;
        } else {
            (void)validateRow(kind, group, row, memory);
            validateReference(row, targetGeneration); rows[row.owner_slot] = row; owners |= bit;
        }
    }
    if (!owners) { check(cancellation); return utils::OwnedVector<std::byte>{utils::OwnedPmrAllocator<std::byte>(memory)}; }
    std::array<HybridColdRowInput, MAX_ROWS> ordered{}; size_t count = 0;
    for (unsigned slot = 0; slot < MAX_ROWS; ++slot)
        if (owners & (UINT64_C(1) << slot)) ordered[count++] = rows[slot];
    return encode(kind, group, targetGeneration, std::span(ordered).first(count), memory, true, cancellation);
}
'''+s[pos:];p.write_text(s)
