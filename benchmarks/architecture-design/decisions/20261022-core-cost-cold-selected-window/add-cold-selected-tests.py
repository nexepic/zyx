from pathlib import Path
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyColdRowRouting.cpp');s=p.read_text();idx=s.rindex('#endif');s=s[:idx]+'''TEST_F(HybridPropertyColdRowRoutingTest, ColdSelectedWindowDecodesSharedBlockOnceAndPreservesOrderedMissingCells) {
    seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    size_t textCount = 0, missing = 0, last = 0; bool first = true; int64_t decodes = 0, routes = 0;
    const auto epoch = io_->dataMutationEpoch();
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        std::array<EntityPropertyKey, 64> owners{};
        for (size_t i = 0; i < owners.size(); ++i) owners[i] = {NODE, i + 1};
        const auto ordered = [&](size_t row) { if (!first) EXPECT_GT(row, last); first = false; last = row; };
        const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
            [&](size_t row, size_t column, auto value) {
                ordered(row); EXPECT_GE(row, 1U); EXPECT_LT(row, 48U); EXPECT_EQ(column, 0U); EXPECT_FALSE(value); ++missing;
            };
        const PropertyScalarReadSink scalar = [](auto, auto, auto, auto) { ADD_FAILURE() << "text emitted scalar"; };
        const PropertyTextReadSink text = [&](size_t row, size_t column, std::string_view value) {
            ordered(row); EXPECT_EQ(column, 0U); EXPECT_EQ(value, row ? "user-000063" : "user-000000"); ++textCount;
        };
        struct Work { int64_t &decodes, &routes; } work{decodes, routes};
        graph::debug::PerfTrace::ScopedObserver observer(&work, [](void *p, std::string_view key, int64_t value, bool) noexcept {
            auto &w = *static_cast<Work *>(p);
            if (key == "native_graph.hybrid.cold_block_decodes") w.decodes += value;
            if (key == "native_graph.hybrid.grouped_owner_route_checks") w.routes += value;
        });
        graph::property_decode::AllocationScope forbid(nullptr, [](void *, size_t) { throw std::logic_error("cold text must borrow"); });
        const std::array<uint32_t, 1> tokens{2};
        EXPECT_TRUE(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
            nullptr, &owning, &scalar, &text));
    }
    EXPECT_EQ(textCount, 2U); EXPECT_EQ(missing, 47U); EXPECT_EQ(decodes, 1); EXPECT_EQ(routes, 64);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
}
TEST_F(HybridPropertyColdRowRoutingTest, ColdSelectedWindowFailureNeverReplaysEarlierOutputAndRefundsItsBody) {
    seed(true); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); size_t callbacks = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}};
        const std::array<uint32_t, 1> tokens{2};
        const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning =
            [](auto, auto, auto) { ADD_FAILURE() << "text must borrow"; };
        const PropertyScalarReadSink scalar = [](auto, auto, auto, auto) { ADD_FAILURE(); };
        const PropertyTextReadSink text = [&](size_t row, auto, auto) {
            ++callbacks; if (row == 1) throw std::runtime_error("cold selected sink failed");
        };
        EXPECT_THROW(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, values_.get(),
            nullptr, &owning, &scalar, &text), std::runtime_error);
    }
    EXPECT_EQ(callbacks, 2U); EXPECT_EQ(memory->used(), 0U);
}
TEST_F(HybridPropertyColdRowRoutingTest, ColdSelectedWindowPreflightsCompetingRoutesBeforeAnyOutput) {
    competing_row_ = true; seed(); auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20); size_t callbacks = 0;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        auto context = HybridPropertyReadContextTestAccess::open(io_, *slotted_pages_, *metadata_, pin, descriptor_);
        const std::array<EntityPropertyKey, 2> owners{{{NODE, 1}, {NODE, 64}}}; const std::array<uint32_t, 1> tokens{2};
        const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> owning = [&](auto, auto, auto) { ++callbacks; };
        const PropertyScalarReadSink scalar = [&](auto, auto, auto, auto) { ++callbacks; };
        const PropertyTextReadSink text = [&](auto, auto, auto) { ++callbacks; };
        EXPECT_THROW(HybridPropertyValueReader::readSelected(context, pin, owners, tokens, {}, {}, nullptr,
            nullptr, &owning, &scalar, &text), FormatException);
    }
    EXPECT_EQ(callbacks, 0U); EXPECT_EQ(memory->used(), 0U);
}
''' +s[idx:];p.write_text(s)
