from pathlib import Path
p=Path('include/graph/storage/relocatable/property/HybridPropertyColdRowBlock.hpp');s=p.read_text();a=s.index('    // All declared bounds/checksums');s=s[:a]+'''    // Complete private result: all blocks share the original 4KiB bound.
    // A failure destroys every prepared block; no successful prefix escapes.
    using EncodedBlocks = utils::OwnedVector<utils::OwnedVector<std::byte>>;
    [[nodiscard]] static EncodedBlocks repackBlocks(std::span<const std::byte> source,
        EntityPropertyOwnerKind kind, uint64_t group, Generation visibleGeneration, Generation targetGeneration,
        std::span<const HybridColdRowInput> changes, std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr);
'''+s[a:];p.write_text(s)
p=Path('src/storage/relocatable/property/HybridPropertyColdRowBlock.cpp');s=p.read_text();a=s.index('utils::OwnedVector<std::byte> HybridPropertyColdRowBlock::repack(');body=s[a:];z=body.index('\n}\n');body=body[:z+2]
body=body.replace('utils::OwnedVector<std::byte> HybridPropertyColdRowBlock::repack(', 'template<class Consumer>\nauto repackRows(').replace('    const CancellationCheckpoint *cancellation) {','    const CancellationCheckpoint *cancellation, Consumer &&consume) {',1).replace('MAX_ROWS','Block::MAX_ROWS').replace('auto decoded = decode(', 'auto decoded = Block::decode(')
body=body.replace('    if (!owners) { check(cancellation); return utils::OwnedVector<std::byte>{utils::OwnedPmrAllocator<std::byte>(memory)}; }','    check(cancellation);')
body=body.replace('    return encode(kind, group, targetGeneration, std::span(ordered).first(count), memory, true, cancellation);','    return consume(std::span(ordered).first(count));')
s=s[:a]+'namespace {\n'+body+'\n}\n'+'''
utils::OwnedVector<std::byte> HybridPropertyColdRowBlock::repack(std::span<const std::byte> source,
    EntityPropertyOwnerKind kind, uint64_t group, Generation visibleGeneration, Generation targetGeneration,
    std::span<const HybridColdRowInput> changes, std::shared_ptr<std::pmr::memory_resource> memory,
    const CancellationCheckpoint *cancellation) {
    return repackRows(source, kind, group, visibleGeneration, targetGeneration, changes, memory, cancellation,
        [&](auto rows) {
            if (rows.empty()) return utils::OwnedVector<std::byte>{utils::OwnedPmrAllocator<std::byte>(memory)};
            return encode(kind, group, targetGeneration, rows, memory, true, cancellation);
        });
}
HybridPropertyColdRowBlock::EncodedBlocks HybridPropertyColdRowBlock::repackBlocks(std::span<const std::byte> source,
    EntityPropertyOwnerKind kind, uint64_t group, Generation visibleGeneration, Generation targetGeneration,
    std::span<const HybridColdRowInput> changes, std::shared_ptr<std::pmr::memory_resource> memory,
    const CancellationCheckpoint *cancellation) {
    return repackRows(source, kind, group, visibleGeneration, targetGeneration, changes, memory, cancellation,
        [&](std::span<const HybridColdRowInput> rows) {
            EncodedBlocks result{utils::OwnedPmrAllocator<utils::OwnedVector<std::byte>>(memory)};
            const auto rowBytes = [&](size_t i) { return DIRECTORY_ENTRY_BYTES +
                (rows[i].large_value_reference ? 4U : 0U) + rows[i].root.size(); };
            size_t first = 0;
            while (first < rows.size()) {
                check(cancellation); size_t end = first, size = 0;
                while (end < rows.size() && rowBytes(end) <= MAX_UNCOMPRESSED_BYTES - size)
                    size += rowBytes(end++);
                if (end == first) throw std::length_error("cold ROW requires an existing whole-root LargeValue");
                const auto encodePrefix = [&] { return encode(kind, group, targetGeneration,
                    rows.subspan(first, end - first), memory, true, cancellation); };
                utils::OwnedVector<std::byte> encoded{utils::OwnedPmrAllocator<std::byte>(memory)};
                try { encoded = encodePrefix(); }
                catch (const std::length_error &) {
                    // Original conversion's RAW-safe prefix fallback. A larger
                    // single row must fit compressed or refuse as a whole.
                    end = first + 1; size = rowBytes(first);
                    while (end < rows.size() && size <= MAX_STORED_BYTES && rowBytes(end) <= MAX_STORED_BYTES - size)
                        size += rowBytes(end++);
                    encoded = encodePrefix();
                }
                result.push_back(std::move(encoded)); first = end;
            }
            check(cancellation); return result;
        });
}
}
''';p.write_text(s)
