from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdRowRouting.cpp');s=p.read_text();idx=s.rindex('#endif');s=s[:idx]+'''TEST_F(HybridPropertyColdRowRoutingTest, MixedOwningProjectionAndExactCountsBorrowOneColdBody) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    int64_t decodes = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}}; const std::array<uint32_t, 3> tokens{1, 2, 4};
        std::array<std::optional<PropertyCollectionRootEntry>, 6> values;
        std::array<std::optional<size_t>, 2> counts;
        graph::debug::PerfTrace::ScopedObserver observer(&decodes, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.cold_block_decodes") *static_cast<int64_t *>(p) += value;
        });
        EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, values, counts));
        ASSERT_TRUE(values[0]); EXPECT_EQ(values[0]->value, PropertyValue(int64_t(100)));
        ASSERT_TRUE(values[1]); EXPECT_EQ(values[1]->value, PropertyValue("user-000000"));
        ASSERT_TRUE(values[2]); EXPECT_EQ(values[2]->value, PropertyValue()); EXPECT_FALSE(values[3]);
        ASSERT_TRUE(values[4]); EXPECT_EQ(values[4]->value, PropertyValue("user-000063")); EXPECT_FALSE(values[5]);
        EXPECT_EQ(counts[0], 3U); EXPECT_EQ(counts[1], 1U);
    }
    EXPECT_EQ(decodes, 1); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowRoutingTest, CountOnlyWindowSharesActualColdDirectoriesWithoutValues) {
    seed(true); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    int64_t decodes = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}}; std::array<std::optional<size_t>, 2> counts;
        graph::debug::PerfTrace::ScopedObserver observer(&decodes, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.cold_block_decodes") *static_cast<int64_t *>(p) += value;
        });
        EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, {}, {}, counts, values_.get()));
        EXPECT_EQ(counts[0], 3U); EXPECT_EQ(counts[1], 1U);
    }
    EXPECT_EQ(decodes, 1); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowRoutingTest, MixedWindowQuotaFailureRestoresBorrowAndNeverReplaysOutput) {
    seed(); auto pin = metadata_->openSnapshot(); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    int64_t decodes = 0; size_t calls = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}}; const std::array<uint32_t, 2> tokens{1, 2};
        const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> sink =
            [&](size_t, size_t, auto) { if (++calls == 3) throw graph::query::execution::MaterializedByteLimitExceeded(); };
        graph::debug::PerfTrace::ScopedObserver observer(&decodes, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.hybrid.cold_block_decodes") *static_cast<int64_t *>(p) += value;
        });
        EXPECT_THROW(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr, nullptr, &sink),
            graph::query::execution::MaterializedByteLimitExceeded);
        EXPECT_EQ(calls, 3U); EXPECT_EQ(decodes, 1);
        // The failed window cannot leave a dangling borrowed block in context.
        EXPECT_EQ(HybridPropertyValueReader::read(context, pin, {NODE, 64}, 2).value->value, PropertyValue("user-000063"));
        EXPECT_EQ(decodes, 2);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowRoutingTest, OwningHotOnlyProjectionDoesNotOpenTheColdWindow) {
    bad_cold_crc_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}}; const std::array<uint32_t, 1> tokens{1};
        std::array<std::optional<PropertyCollectionRootEntry>, 2> values;
        EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, values));
        ASSERT_TRUE(values[0]); EXPECT_EQ(values[0]->value, PropertyValue(int64_t(100))); EXPECT_FALSE(values[1]);
    }
    EXPECT_EQ(memory->used(), 0U);
}
''' +s[idx:];p.write_text(s)
