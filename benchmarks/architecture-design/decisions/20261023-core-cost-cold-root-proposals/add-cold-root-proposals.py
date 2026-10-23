from pathlib import Path
p=Path('include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp');s=p.read_text();s=s.replace('#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"', '#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"\n#include "graph/storage/relocatable/property/HybridPropertyLargeFieldPreparation.hpp"',1)
s=s.replace('const LargeValueStore *values = nullptr, const CancellationCheckpoint *cancellation = nullptr) {','const LargeValueStore *values = nullptr, const CancellationCheckpoint *cancellation = nullptr,\n        std::span<const HybridPropertyLargeFieldPreparation> newRoots = {}) {',3)
s=s.replace('const CancellationCheckpoint *cancellation = nullptr) {\n        hybrid_property_detail::require(hotChanges.size()', 'const CancellationCheckpoint *cancellation = nullptr,\n        std::span<const HybridPropertyLargeFieldPreparation> newRoots = {}) {\n        hybrid_property_detail::require(hotChanges.size()',1)
s=s.replace('cancellation, std::move(result));','cancellation, std::move(result), newRoots);',1)
s=s.replace('cancellation, HybridPropertyDemotionPreparation(memory));','cancellation, HybridPropertyDemotionPreparation(memory), newRoots);',1)
s=s.replace('cancellation, std::move(prepared));','cancellation, std::move(prepared), newRoots);',1)
s=s.replace('const LargeValueStore *values, const CancellationCheckpoint *cancellation, HybridPropertyDemotionPreparation result) {','const LargeValueStore *values, const CancellationCheckpoint *cancellation, HybridPropertyDemotionPreparation result,\n        std::span<const HybridPropertyLargeFieldPreparation> newRoots) {',1)
a=s.index('    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlockImpl(')
s=s[:a]+'''    // Only immutable original proposal objects can authenticate not-yet-visible
    // roots. Copied SOURCE rows never use this path: their original block
    // generation/manifests must still be checked against the source pin.
    static bool admitPreparedColdRoot(const MetadataWriteBatch &batch, const HybridColdRowInput &row,
        EntityPropertyKey owner, std::span<const uint32_t> hotTokens,
        std::span<const HybridPropertyLargeFieldPreparation> proposals,
        const std::shared_ptr<std::pmr::memory_resource> &memory, const CancellationCheckpoint *cancellation) {
        using hybrid_property_detail::require;
        if (!row.large_value_reference || proposals.empty()) return false;
        require(proposals.size() <= 4096U, "cold root proposals exceed the original membership frontier");
        utils::OwnedHeapReservation referenceCredit(memory, row.root.size() +
            3U * utils::detail::standardContainerProxyBytes());
        const auto reference = EntityPropertyLargeValueReferenceCodec::decode(row.root);
        if (reference.value.value_id.value() < batch.current().next_large_value_id) return false;
        require(reference.owner_entity_id == owner.entity_id && reference.owner_entity_type ==
            (owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId) &&
            reference.value.value_id.value() < batch.candidate().next_large_value_id,
            "new cold root has a foreign owner or unreserved batch identity");
        for (const auto &proposal : proposals) {
            if (cancellation) cancellation->check();
            if (proposal.reference().value_id != reference.value.value_id) continue;
            require(!proposal.reused() && proposal.databaseId() == batch.current().database_id &&
                proposal.sourceGeneration() == batch.current().generation && proposal.reference() == reference.value &&
                reference.value.kind == LargeValueKind::LVK_BYTES &&
                reference.value.media_type == RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE &&
                reference.value.generation <= batch.generation(), "new cold root lost its original immutable carrier proof");
            const auto payload = proposal.payload();
            require(payload.size() >= RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES &&
                payload.size() == reference.value.logical_bytes &&
                LittleEndianCodec::readU32(payload, 12) == payload.size() - RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES,
                "new cold root proposal has an invalid payload length");
            utils::OwnedHeapReservation directoryCredit(memory);
            const SelectedPropertyReadAdmission admit = [&](size_t bytes) {
                directoryCredit.set(bytes + utils::detail::standardContainerProxyBytes());
            };
            const auto directory = EntityPropertyLargeValuePayloadCodec::decodeDirectory(payload, &admit, cancellation);
            require(directory.size() == row.field_count, "new cold root proposal has a different field count");
            for (const auto &entry : directory) {
                if (cancellation) cancellation->check();
                require(!std::binary_search(hotTokens.begin(), hotTokens.end(), entry.key_token),
                    "new cold root proposal contains a globally hot token");
                const auto data = payload.subspan(RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES);
                require(entry.offset <= data.size() && entry.length <= data.size() - entry.offset && entry.length >= 4,
                    "new cold root proposal entry exceeds its actual payload");
                const auto bytes = data.subspan(entry.offset, entry.length);
                require(LittleEndianCodec::readU32(bytes, 0) == entry.key_token, "new cold root proposal entry has a foreign token");
                size_t offset = 0; const auto value = bytes.subspan(4);
                static_cast<void>(CanonicalPropertyValueCodec::inspectNext(value, offset));
                require(offset == value.size(), "new cold root proposal value has trailing bytes");
            }
            return true;
        }
        return false;
    }
'''+s[a:]
s=s.replace('''        // This restricted path admits existing pinned manifests only. New
        // batch-local LargeValues require the shared proposal coordinator.
        for (const auto &row : changes) if (!row.root.empty()) { check(); inspect(row, batch.current().generation); }''','''        for (const auto &row : changes) if (!row.root.empty()) {
            check(); const EntityPropertyKey owner{source->owner_kind, (source->group_id - 1U) * 64U + row.owner_slot + 1U};
            if (!admitPreparedColdRoot(batch, row, owner, std::span(matrix.tokens).first(matrix.column_count),
                newRoots, memory, cancellation)) inspect(row, batch.current().generation);
        }''',1)
p.write_text(s)
