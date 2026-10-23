from pathlib import Path
p=Path('include/graph/storage/relocatable/property/HybridPropertyValueReader.hpp');s=p.read_text();idx=s.index('        const auto tokens = context.tokens(owner, tokensAdmission, boundary);');s=s[:idx]+'''#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
        // Count, admission passes and value construction borrow the same cold
        // body. A completed collection leaves the lease before escaping.
        const std::array<EntityPropertyKey, 1> selected{owner};
        std::optional<CollectionResult> windowResult;
        bool producing = false, consumed = false;
        try { consumed = context.visitColdWindowSorted(selected, {}, true, [&](size_t, size_t) {
            producing = true;
            windowResult = readCollection(context, snapshot, owner, ownerType, largeValues, admission, slotsAdmission);
        }, std::ref(boundary));
        } catch (const query::execution::MaterializedByteLimitExceeded &) {
            if (producing) throw;
        } catch (const query::execution::detail::ResourceAdmissionDenied &) {
            if (producing) throw;
        }
        boundaryCredit.set(0);
        if (consumed) {
            hybrid_property_detail::require(bool(windowResult), "cold collection window produced no result");
            return std::move(*windowResult);
        }
#endif
''' +s[idx:];p.write_text(s)
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdRowRouting.cpp');s=p.read_text();idx=s.rindex('#endif');s=s[:idx]+'''TEST_F(HybridPropertyColdRowRoutingTest, CompleteCollectionCountAdmissionAndValuesShareOneDecodedColdBody) {
    seed(true); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    int64_t decodes = 0; size_t admitted = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        graph::debug::PerfTrace::ScopedObserver observer(&decodes, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.cold_block_decodes") *static_cast<int64_t *>(p) += value;
        });
        const SelectedPropertyReadAdmission admission = [&](size_t bytes) { EXPECT_GT(bytes, 0U); ++admitted; };
        auto result = HybridPropertyValueReader::readCollection(context, pin, {NODE, 1}, graph::Node::typeId, values_.get(), &admission);
        EXPECT_TRUE(result.routed); ASSERT_TRUE(result.value); ASSERT_EQ(result.value->entries.size(), 3U);
        EXPECT_EQ(result.value->entries[0].key_token, 1U); EXPECT_EQ(result.value->entries[0].value, PropertyValue(int64_t(100)));
        EXPECT_EQ(result.value->entries[1].key_token, 2U); EXPECT_EQ(result.value->entries[1].value, PropertyValue("user-000000"));
        EXPECT_EQ(result.value->entries[2].key_token, 4U); EXPECT_EQ(result.value->entries[2].value, PropertyValue(std::string(8193, 'x')));
    }
    EXPECT_EQ(decodes, 1); EXPECT_EQ(admitted, 3U); EXPECT_EQ(memory->used(), 0U);
}
''' +s[idx:];p.write_text(s)
