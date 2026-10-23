from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyConversionPreparation.cpp');s=p.read_text();key='''class HybridPropertyConversionPreparationTestAccess final {
public:''';s=s.replace(key,key+'''
    static auto prepareCold(MetadataWriteBatch &batch, const HybridPropertyGroupConversion &plan,
        const std::function<void()> &checkpoint = {}) {
        return std::unique_ptr<HybridPropertyConversionPreparation>(new HybridPropertyConversionPreparation(
            batch, plan, checkpoint, {}, true, true));
    }''',1);idx=s.rindex('#endif');s=s[:idx]+'''TEST(HybridPropertyConversionPreparationTest, ColdPreparationBindsWholeRowsAndHotLocatorsWithoutPerFieldDirectories) {
    NoPhysicalAllocator allocator; const auto current = sourceRoot();
    MetadataWriteBatch batch(allocator, current, LogSequenceNumber(9));
    const auto roots = inputRows(3, true); auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        Plan plan(memory, roots, NODE, 1, batch.generation(), 3952);
        const auto prepared = Access::prepareCold(batch, plan);
        EXPECT_TRUE(prepared->owners().empty()); EXPECT_TRUE(prepared->columns().empty()); EXPECT_TRUE(prepared->extensions().empty());
        const auto groupSlot = HybridPropertyDirectoryCodec::decodeLocator(prepared->group().value);
        const auto &groupPage = imageFor(*prepared, groupSlot.page_id);
        const auto group = groupPage.readGroup(groupSlot, NODE, 1, batch.generation());
        EXPECT_EQ(group.mode, HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER); EXPECT_EQ(group.memberships, 192U); EXPECT_EQ(group.columns, 2U);
        const auto matrix = groupPage.readColumnMembership(group.column_membership, group, batch.generation());
        EXPECT_EQ(matrix.cold_fields, 64U); EXPECT_EQ(matrix.cold_rows.owners, UINT64_MAX);
        size_t coldRows = 0, blocks = 0; uint64_t remaining = matrix.cold_rows.owners;
        while (remaining) {
            const auto slot = unsigned(std::countr_zero(remaining)); const auto reference = matrix.cold_rows.roots[slot];
            const auto bytes = imageFor(*prepared, reference.page_id).readRecord(reference);
            const auto block = HybridPropertyFieldView::decodeColdRows(bytes, reference, group, matrix,
                batch.generation(), batch.generation(), memory);
            remaining &= ~block.owners; ++blocks;
            for (unsigned row = 0; row < 64; ++row) if (const auto value = block.row(row)) {
                EXPECT_FALSE(value->large_value_reference); const auto decoded = EntityPropertyCollectionRootCodec::decode(value->root);
                EXPECT_EQ(decoded.owner_entity_id, row + 1U); ASSERT_EQ(decoded.entries.size(), 1U);
                EXPECT_EQ(decoded.entries[0].key_token, 3U); EXPECT_EQ(decoded.entries[0].value, PropertyValue(std::string(100, 'x'))); ++coldRows;
            }
        }
        EXPECT_EQ(coldRows, 64U); EXPECT_GT(blocks, 1U);
        for (const auto &column : plan.columns()) {
            const auto reference = matrix.columnLocator(column.identity.token);
            EXPECT_EQ(imageFor(*prepared, reference.page_id).readColumn(reference, column.identity, group, batch.generation()), column);
        }
        EXPECT_EQ(group.column_membership.page_id, groupSlot.page_id); EXPECT_NE(group.column_membership.slot_id, groupSlot.slot_id);
        EXPECT_EQ(batch.current(), current); EXPECT_EQ(batch.allocatedPageCount(), 0U); EXPECT_EQ(batch.createdPoolCount(), 0U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(roots, inputRows(3, true));
}
TEST(HybridPropertyConversionPreparationTest, ColdOnlyAndPropertylessPreparationKeepNativeOwnerIdentityWithoutSelectors) {
    for (const bool propertyless : {false, true}) {
        NoPhysicalAllocator allocator; MetadataWriteBatch batch(allocator, sourceRoot(), LogSequenceNumber(9));
        auto roots = inputRows(1, true); if (propertyless) for (auto &root : roots) root.entries.clear();
        auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
        {
            Plan plan(memory, roots, NODE, 1, batch.generation(), 3952); const auto prepared = Access::prepareCold(batch, plan);
            const auto ref = HybridPropertyDirectoryCodec::decodeLocator(prepared->group().value);
            const auto &page = imageFor(*prepared, ref.page_id); const auto group = page.readGroup(ref, NODE, 1, batch.generation());
            const auto matrix = page.readColumnMembership(group.column_membership, group, batch.generation());
            EXPECT_EQ(group.mode, HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER); EXPECT_EQ(group.columns, 0U);
            EXPECT_EQ(matrix.cold_fields, propertyless ? 0U : 64U); EXPECT_EQ(matrix.cold_rows.owners, propertyless ? 0U : UINT64_MAX);
            EXPECT_TRUE(prepared->owners().empty()); EXPECT_TRUE(prepared->extensions().empty());
        }
        EXPECT_EQ(memory->used(), 0U);
    }
}
TEST(HybridPropertyConversionPreparationTest, OversizedColdRowsRequireSameBatchResolverAndNeverPublishIndependentValues) {
    NoPhysicalAllocator allocator; const auto current = sourceRoot(); MetadataWriteBatch batch(allocator, current, LogSequenceNumber(9));
    auto roots = inputRows(1, true); for (auto &root : roots) root.entries[0].value = PropertyValue(std::string(8193, 'x'));
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        Plan plan(memory, roots, NODE, 1, batch.generation(), 3952); const auto retained = memory->used();
        EXPECT_THROW(Access::prepareCold(batch, plan), std::length_error);
        EXPECT_EQ(batch.current(), current); EXPECT_EQ(batch.candidate().next_logical_page_id, 100U);
        EXPECT_EQ(batch.allocatedPageCount(), 0U); EXPECT_EQ(batch.createdPoolCount(), 0U); EXPECT_EQ(memory->used(), retained);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyConversionPreparationTest, ColdPreparationCancellationAfterPrivateIdsRefundsAndRetriesSameSource) {
    NoPhysicalAllocator allocator; const auto current = sourceRoot(); const auto roots = inputRows(3, true);
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    {
        Plan plan(memory, roots, NODE, 1, Generation(9), 3952); const auto retained = memory->used();
        MetadataWriteBatch batch(allocator, current, LogSequenceNumber(9));
        EXPECT_THROW(Access::prepareCold(batch, plan, [&] {
            if (batch.candidate().next_logical_page_id > current.next_logical_page_id) throw std::runtime_error("cancel after private identity");
        }), std::runtime_error);
        EXPECT_EQ(batch.current(), current); EXPECT_EQ(memory->used(), retained);
        MetadataWriteBatch retry(allocator, current, LogSequenceNumber(9)); const auto prepared = Access::prepareCold(retry, plan);
        EXPECT_EQ(prepared->pages()[0].pageId(), PageId(100));
    }
    EXPECT_EQ(memory->used(), 0U);
}
''' +s[idx:];s=s.replace('#include "graph/query/execution/MaterializedByteBudget.hpp"', '#include "graph/query/execution/MaterializedByteBudget.hpp"\n#include "graph/storage/relocatable/property/HybridPropertyFieldView.hpp"');p.write_text(s)
for profile in ['hybrid','default']:
 s=Path('/tmp/build-cold-reachability-'+profile+'-modules.py').read_text().replace("units=[", "units=['property/test_PropertyCollectionRootBorrowed', 'property/test_PropertyCollectionRoot', ")
 Path('/tmp/build-cold-preparation-'+profile+'-modules.py').write_text(s)
