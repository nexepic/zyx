from pathlib import Path
root=Path('/workspace/zyx');p=root/'include/graph/storage/relocatable/property/HybridPropertyReadContext.hpp';s=p.read_text()
a=s.index('    [[nodiscard]] Enumeration visitOwner(');b=s.index('    // A disengaged result means',a);part=s[a:b]
start=part.index('        if (group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER)');body=part[start:];assert body.endswith('    }\n')
body=body[:-len('    }\n')].replace('group->','group.').replace('*group','group')
private='''    Enumeration visitOwnerInGroup(EntityPropertyKey owner, const HybridPropertyGroupRecord &group, size_t maximumEntries,
        const std::function<bool(uint32_t, HybridPropertySource)> &visitor,
        const std::function<void(size_t)> &boundaryAdmission) const {
'''+body+'    }\n'
public=part[:start]+'''        return visitOwnerInGroup(owner, *group, maximumEntries, visitor, boundaryAdmission);
    }
    // Root presence is a metadata membership fact. A selected hot field must
    // not decompress or authenticate an unrelated cold root merely to prove
    // that its owner has fields. Exact counts still inspect the cold directory.
    [[nodiscard]] std::optional<bool> hasFields(EntityPropertyKey owner,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        const auto group = findGroup(owner, boundaryAdmission, nullptr, true, true);
        if (!group) return {};
        if (group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
            requireNoLegacySelector(owner, boundaryAdmission);
            return withColumnMembership(*group, [&](const auto &matrix) {
                const auto slot = hybridPropertySlotFor(owner);
                if (matrix.cold_rows.owners & (UINT64_C(1) << slot)) return true;
                for (const auto word : matrix.owners[slot]) if (word) return true;
                return false;
            });
        }
        return visitOwnerInGroup(owner, *group, 1,
            [](uint32_t, HybridPropertySource) { return false; }, boundaryAdmission).fields != 0;
    }
'''
s=s[:a]+public+s[b:];pos=s.index('    void requireNoLegacySelector(');s=s[:pos]+private+s[pos:];p.write_text(s)
p=root/'include/graph/storage/relocatable/property/HybridPropertyValueReader.hpp';s=p.read_text();needle='''                const auto first = context.visitOwner(owners[i], 1,
                    [](uint32_t, HybridPropertySource) { return false; }, std::ref(boundary));
                if (first.routed) hasFields = first.fields != 0;''';assert needle in s;s=s.replace(needle,'                hasFields = context.hasFields(owners[i], std::ref(boundary));',1);p.write_text(s)
p=root/'tests/src/storage/relocatable/property/test_HybridPropertyColdRowRouting.cpp';s=p.read_text();assert s.endswith('#endif\n');s=s[:-len('#endif\n')]+'''
TEST_F(HybridPropertyColdRowRoutingTest, ScalarFallbackUsesMetadataPresenceAndNeverReadsAnUnrequestedColdRoot) {
    bad_cold_crc_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); graph::utils::OwnedAllocationScope scope(memory);
    auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
    const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}};
    const std::array<uint32_t, 1> tokens{1}; size_t delivered = 0, missing = 0;
    const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
        [&](size_t row, size_t column, auto value) { EXPECT_EQ(row, 1U); EXPECT_EQ(column, 0U); EXPECT_FALSE(value); ++missing; };
    const PropertyScalarReadSink scalar = [&](size_t row, size_t column, auto type, uint64_t word) {
        EXPECT_EQ(row, 0U); EXPECT_EQ(column, 0U); EXPECT_EQ(type, graph::PropertyType::INTEGER); EXPECT_EQ(word, 100U); ++delivered;
    };
    EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr, nullptr, &owning, &scalar));
    EXPECT_EQ(delivered, 1U); EXPECT_EQ(missing, 1U);
    // Asking for exact counts really does access the cold directory, so its
    // corruption must then be discovered rather than inferred from a bitmap.
    std::array<std::optional<size_t>, 2> counts;
    EXPECT_THROW(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, counts,
        nullptr, nullptr, &owning, &scalar), FormatException);
}
#endif
''';p.write_text(s)
