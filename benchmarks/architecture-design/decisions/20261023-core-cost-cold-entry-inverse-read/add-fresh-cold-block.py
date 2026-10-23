from pathlib import Path
p=Path('src/storage/relocatable/property/HybridPropertyColdRowBlock.cpp');s=p.read_text();s=s.replace('    require(targetGeneration.isValid() && targetGeneration > visibleGeneration &&\n        !changes.empty()', '    require(memory && hybrid_property_detail::validKind(kind) && visibleGeneration.isValid() &&\n        targetGeneration.isValid() && targetGeneration > visibleGeneration &&\n        !changes.empty()',1)
s=s.replace('    auto decoded = Block::decode(source, kind, group, visibleGeneration, memory, cancellation);','''    hybrid_property_detail::validateGroup(group, 1);
    auto decoded = source.empty() ? Block::Decoded(memory) : Block::decode(source, kind, group, visibleGeneration, memory, cancellation);
    if (source.empty()) {
        decoded.owner_kind = kind; decoded.group_id = group; decoded.semantic_generation = visibleGeneration;
    }''',1);p.write_text(s)
p=Path('include/graph/storage/relocatable/property/HybridPropertyColdRowBlock.hpp');s=p.read_text();s=s.replace('    // complete owner roots; empty root/count removes an owner. Empty output','    // complete owner roots; empty source prepares a fresh block without granting\n    // storage authority. Empty root/count removes an existing owner. Empty output',1);p.write_text(s)
p=Path('include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp');s=p.read_text()
s=s.replace('block.anchor.isValid() && block.reference.isValid() && !block.rows.empty()', 'block.anchor.isValid() && (block.reference.isValid() || block.reference == SlottedSlotReference{}) && !block.rows.empty()',1)
s=s.replace('require(memory && !changes.empty() && changes.size() <= 64 && context.snapshot_.superblock() == batch.current(),','require(memory && !changes.empty() && changes.size() <= 64 &&\n            (coldReference.isValid() || coldReference == SlottedSlotReference{}) && context.snapshot_.superblock() == batch.current(),',1)
s=s.replace('(void)imageFor(groupReference.page_id); (void)imageFor(coldReference.page_id);','(void)imageFor(groupReference.page_id);\n        if (coldReference.isValid()) (void)imageFor(coldReference.page_id);',1)
s=s.replace('require(affected != 0, "cold block source reference has no actual members");','require(coldReference.isValid() ? affected != 0 : affected == 0, "cold block source reference has no actual members");',1)
s=s.replace('        context.withColdBlock(*source, matrixSource, coldReference,', '        if (coldReference.isValid()) context.withColdBlock(*source, matrixSource, coldReference,',1)
s=s.replace('        const auto sourceBytes = imageFor(coldReference.page_id).readRecord(coldReference);','''        utils::OwnedVector<std::byte> sourceBytes{utils::OwnedPmrAllocator<std::byte>(memory)};
        if (coldReference.isValid()) sourceBytes = imageFor(coldReference.page_id).readRecord(coldReference);''',1)
s=s.replace('const bool keepSlot = blocks.size() == 1 && blocks.front().size() <=','const bool keepSlot = coldReference.isValid() && blocks.size() == 1 && blocks.front().size() <=',1)
s=s.replace('if (!keepSlot) imageFor(coldReference.page_id).erase(coldReference);','if (!keepSlot && coldReference.isValid()) imageFor(coldReference.page_id).erase(coldReference);',1)
p.write_text(s)
