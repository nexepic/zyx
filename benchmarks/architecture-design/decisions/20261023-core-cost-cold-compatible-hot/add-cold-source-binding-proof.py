from pathlib import Path
p=Path('include/graph/storage/relocatable/property/HybridPropertyPageImage.hpp');s=p.read_text().replace('#pragma once','#pragma once\n#include <bitset>',1).replace('namespace graph::storage {','namespace graph::storage {\nclass HybridPropertyFieldPage;',1)
key='''        return HybridPropertyPageImage(page_.clone(), visibleGeneration, std::move(memory));''';assert key in s;s=s.replace(key,'''        auto result = HybridPropertyPageImage(page_.clone(), visibleGeneration, std::move(memory));
        result.source_slots_ = source_slots_;
        return result;''',1)
s=s.replace('return page_.insert(', 'return appendRecord(')
s=s.replace('page_.overwrite(reference, bytes);', 'overwriteRecord(reference, bytes);')
s=s.replace('page_.overwrite(reference, replacement.encode());', 'overwriteRecord(reference, replacement.encode());')
s=s.replace('page_.overwrite(reference, replacement);', 'overwriteRecord(reference, replacement);')
key='''        for (const auto &reference : references) validatePrivateRecord(readRecord(reference), referenceAdmission);''';assert key in s;s=s.replace(key,'''        for (const auto &reference : references) {
            const auto bytes = readRecord(reference);
            if (reference.slot_id < source_slots_.size() && source_slots_.test(reference.slot_id) &&
                bytes.size() >= sizeof(uint32_t) && LittleEndianCodec::readU32(bytes, 0) == HybridPropertyColdRowBlock::MAGIC) {
                // Page CRC/layout/owner were authenticated by FieldPageStore.
                // This untouched source record is copied, not read as a ROW.
                // New/changed records and ordinary open() never have this proof.
                debug::PerfTrace::addValue("native_graph.hybrid.unchanged_cold_bindings", 1);
                continue;
            }
            validatePrivateRecord(bytes, referenceAdmission);
        }''',1)
s=s.replace('    void erase(const SlottedSlotReference &reference) { page_.erase(reference); }', '    void erase(const SlottedSlotReference &reference) { forgetSourceSlot(reference); page_.erase(reference); }',1)
idx=s.index('    static void require(bool value, const char *message)');s=s[:idx]+'''    friend class HybridPropertyFieldPage;
    // Only a real snapshot-authenticated FieldPage can mint this transient
    // proof. Future slots are cleared by appendRecord before they can bind.
    void trustSnapshotClone() { source_slots_.set(); }
    void forgetSourceSlot(const SlottedSlotReference &reference) {
        if (reference.slot_id < source_slots_.size()) source_slots_.reset(reference.slot_id);
    }
    [[nodiscard]] SlottedSlotReference appendRecord(std::span<const std::byte> bytes) {
        const auto reference = page_.insert(bytes); forgetSourceSlot(reference); return reference;
    }
    void overwriteRecord(const SlottedSlotReference &reference, std::span<const std::byte> bytes) {
        forgetSourceSlot(reference); // fail closed even if a private overwrite refuses
        page_.overwrite(reference, bytes);
    }
''' +s[idx:]
s=s.replace('    SlottedPage page_;', '''    SlottedPage page_;
    // Fixed per-page COW provenance, included in the existing holder charge.
    // Not serialized, not a read cache, and never a publication capability.
    std::bitset<RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES / RSS_SLOTTED_PAGE_SLOT_BYTES> source_slots_{};''',1);p.write_text(s)
p=Path('include/graph/storage/relocatable/property/HybridPropertyFieldPageStore.hpp');s=p.read_text();key='''            return HybridPropertyPageImage::open(bytes, stored.owner.page_id, stored.owner.generation,
                next, memory);''';assert key in s;s=s.replace(key,'''            auto result = HybridPropertyPageImage::open(bytes, stored.owner.page_id, stored.owner.generation,
                next, memory);
            result.trustSnapshotClone();
            return result;''',1);p.write_text(s)
