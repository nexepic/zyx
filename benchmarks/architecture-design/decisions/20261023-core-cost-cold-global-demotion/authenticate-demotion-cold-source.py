from pathlib import Path
p=Path('/workspace/zyx/include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp');s=p.read_text();needle='''            uint64_t requiredOwners = 0; size_t emitted = 0;''';assert needle in s
s=s.replace(needle,'''            // Removing global hot authority changes what every existing cold
            // directory is allowed to contain. Authenticate all original cold
            // roots in this one64-owner group, even roots not being repacked,
            // so an old forbidden hot token cannot become valid by demotion.
            const auto originalMatrix = context.columnMembership(source);
            uint64_t inspectedOwners = 0;
            for (unsigned slot = 0; slot < 64; ++slot) {
                const auto bit = UINT64_C(1) << slot;
                if (!(originalMatrix.cold_rows.owners & bit) || (inspectedOwners & bit)) continue;
                context.withColdBlock(source, originalMatrix, originalMatrix.cold_rows.roots[slot],
                    [&](const auto &block, const auto &) {
                        for (unsigned member = 0; member < 64; ++member) if (const auto row = block.row(member)) {
                            check(); const EntityPropertyKey owner{source.owner_kind, (source.group_id - 1U) * 64U + member + 1U};
                            require(context.findGroup(owner, admit, nullptr, true, true) == source,
                                "cold demotion original cold owner Entity/ROW authority changed");
                            context.requireNoLegacySelector(owner, admit); boundary.set(0);
                            HybridPropertyColdRowInspection original(context.snapshot_, *row, owner, block.semantic_generation,
                                std::span(originalMatrix.tokens).first(originalMatrix.column_count), memory, values, cancellation);
                            static_cast<void>(original.count());
                        }
                        inspectedOwners |= block.owners;
                    });
            }
            uint64_t requiredOwners = 0; size_t emitted = 0;''',1);p.write_text(s)
