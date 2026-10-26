#include "BuildingScanMembershipFixture.hpp"

static_assert(!std::is_default_constructible_v<BuildingScanMembershipRequest>);
static_assert(!std::is_default_constructible_v<BuildingScanMaintenanceRequest>);
static_assert(!std::is_default_constructible_v<BuildingScanCompletionRequest>);

TEST(ScanMembershipDirectoryTest, UsesTheCompleteNodeDomainAndBigEndianOrder) {
    using Codec = ScanMembershipDirectoryCodec;
    const auto last = Codec::MAX_GROUP;
    const std::array<uint64_t, 6> groups{1, 255, 256, 65536, last - 1, last};
    for (const auto group : groups) EXPECT_EQ(Codec::decodeKey(Codec::key(group)), group);
    for (size_t i = 1; i < groups.size(); ++i) EXPECT_LT(Codec::key(groups[i - 1]), Codec::key(groups[i]));
    EXPECT_THROW(static_cast<void>(Codec::key(0)), FormatException);
    EXPECT_THROW(static_cast<void>(Codec::key(last + 1)), FormatException);
    ScanMembershipDirectoryMap::Key zero{};
    EXPECT_THROW(static_cast<void>(Codec::decodeKey(zero)), FormatException);
}

#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT

TEST_F(BuildingScanMembershipTest, PublishesDirectoryAndCountsTogetherAndPreservesOldPinsAndReopen) {
    insert(1, 0, {}); insert(64, 1, {7}); insert(65, 2, {7, 11});
    auto original = metadata->openSnapshot();
    auto first = request(1); const auto firstRoot = install(first);
    EXPECT_NE(firstRoot.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
    EXPECT_FALSE(original.superblock().hybrid_property_descriptor_root.isValid());
    EXPECT_EQ(firstRoot.entity_property_directory_root, original.superblock().entity_property_directory_root);
    auto old = metadata->openSnapshot(); const auto oldDesc = descriptor(*metadata, old);
    EXPECT_EQ(oldDesc.scan_live_node_count, 2U); EXPECT_EQ(oldDesc.scan_membership_group_count, 1U);
    const auto one = read(*metadata, old, oldDesc, 1);
    ASSERT_TRUE(one); EXPECT_EQ(one->live, UINT64_C(1) | (UINT64_C(1) << 63));
    ASSERT_EQ(one->label_count, 1U); EXPECT_EQ(one->labels[0].token, 7U);
    EXPECT_EQ(one->labels[0].members, UINT64_C(1) << 63);
    auto second = request(2); const auto secondRoot = install(second);
    EXPECT_NE(secondRoot.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
    EXPECT_FALSE(read(*metadata, old, oldDesc, 2));
    auto reopened = RelocatableMetadataStore::openReadOnly(io); auto pin = reopened->openSnapshot();
    const auto current = descriptor(*reopened, pin);
    EXPECT_EQ(current.scan_live_node_count, 3U); EXPECT_EQ(current.scan_membership_group_count, 2U);
    ASSERT_TRUE(read(*reopened, pin, current, 1));
    const auto two = read(*reopened, pin, current, 2); ASSERT_TRUE(two);
    EXPECT_EQ(two->live, 1U); EXPECT_EQ(two->label_count, 2U);
    EXPECT_EQ(first.memory->used(), 0U); EXPECT_EQ(second.memory->used(), 0U);
}

TEST_F(BuildingScanMembershipTest, StaleDuplicateEmptyAndCompleteTargetsRefuseBeforeWrites) {
    insert(1, 0, {7});
    auto stale = request(1); stale.expected_generation = Generation(stale.expected_generation.value() + 1);
    io->resetStats(); EXPECT_THROW(static_cast<void>(install(stale)), FormatException); EXPECT_EQ(io->stats().write_calls, 0U);
    auto empty = request(2);
    io->resetStats(); EXPECT_THROW(static_cast<void>(install(empty)), FormatException); EXPECT_EQ(io->stats().write_calls, 0U);
    auto valid = request(1); static_cast<void>(install(valid));
    auto duplicate = request(1); const auto before = metadata->openSnapshot().superblock();
    io->resetStats(); EXPECT_THROW(static_cast<void>(install(duplicate)), FormatException); EXPECT_EQ(io->stats().write_calls, 0U);
    EXPECT_EQ(metadata->openSnapshot().superblock(), before);
    initialize(false); auto complete = request(1);
    io->resetStats(); EXPECT_THROW(static_cast<void>(install(complete)), FormatException); EXPECT_EQ(io->stats().write_calls, 0U);
}

TEST_F(BuildingScanMembershipTest, ValidChecksumsCannotForgePrimaryLabelFallbackForAnOrdinaryGroup) {
    insert(1, 0, {}); auto input = request(1); static_cast<void>(install(input));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    ASSERT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata, memory).scan()));
    auto pin = metadata->openSnapshot(); const auto desc = descriptor(*metadata, pin);
    BTreePageStore trees(*pages, *metadata);
    ScanMembershipDirectoryMap map(hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_SCAN), trees,
        *metadata, ScanMembershipDirectoryCodec::bind(desc, pin));
    const auto value = map.find(pin, ScanMembershipDirectoryCodec::key(1)); ASSERT_TRUE(value);
    const auto slot = HybridPropertyDirectoryCodec::decodeLocator(*value);
    const auto location = metadata->findPageLocation(pin, slot.page_id);
    const auto pool = metadata->findCatalog(pin, location.segment_id);
    const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(pin.superblock().data_region_offset, pool.physical_slot) +
        uint64_t(location.page_index) * RSS_PAGE_SIZE;
    std::array<std::byte, RSS_PAGE_SIZE> frame{};
    ASSERT_EQ(io->readAt(offset, frame.data(), frame.size()), frame.size());
    {
        auto image = HybridPropertyPageImage::open(std::span(frame).subspan(RSS_PAGE_OWNER_HEADER_BYTES),
            slot.page_id, location.generation, pin.superblock().generation, memory);
        auto record = image.readScanMembership(slot, pin.superblock().database_id, 1, desc.semantic_generation);
        ASSERT_FALSE(record.labels_from_primary); ASSERT_EQ(record.label_count, 0U);
        record.labels_from_primary = true;
        image.replaceScanMembership(slot, record, desc.semantic_generation);
        const auto bytes = image.serialize();
        std::copy(bytes.begin(), bytes.end(), frame.begin() + RSS_PAGE_OWNER_HEADER_BYTES);
    }
    io->writeAt(offset, frame.data(), frame.size());
    const auto raw = read(*metadata, pin, desc, 1); ASSERT_TRUE(raw); EXPECT_TRUE(raw->labels_from_primary);
    EXPECT_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata, memory).scan()), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(BuildingScanMembershipTest, OversizedCompleteLabelsPublishExplicitFallbackAndTinyMemoryStillRefusesBeforeIo) {
    insert(1, 0, {1, 2, 3});
    auto source = metadata->openSnapshot();
    std::vector<uint64_t> tokens; for (uint64_t token = 1; token <= 65; ++token) tokens.push_back(token);
    EntityLabelSetWritePlan plan; plan.append_page = EntityLabelSetAppendRequest{SegmentId(31), PageId(102)};
    static_cast<void>(labels->updateNodeLabels(source, 1, tokens, plan, LogSequenceNumber(3))); source.release();
    auto input = request(1);
    EXPECT_NO_THROW(static_cast<void>(install(input)));
    {
        auto pin = metadata->openSnapshot();
        MetadataPageFile file(io, pin.superblock());
        const auto descriptor = file.readHybridPropertyDescriptor(pin.superblock().hybrid_property_descriptor_root,
            pin.superblock()).descriptor;
        BTreePageStore trees(*pages, *metadata);
        ScanMembershipDirectoryMap map(hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_SCAN), trees,
            *metadata, ScanMembershipDirectoryCodec::bind(descriptor, pin));
        const auto value = map.find(pin, ScanMembershipDirectoryCodec::key(1)); ASSERT_TRUE(value);
        const auto slot = HybridPropertyDirectoryCodec::decodeLocator(*value);
        HybridPropertyFieldPageStore fields(io, *metadata);
        const auto page = fields.read(pin, slot.page_id);
        const auto membership = page.view().readScanMembership(slot, pin.superblock().database_id, 1,
            descriptor.semantic_generation);
        EXPECT_TRUE(membership.labels_from_primary); EXPECT_EQ(membership.label_count, 0U); EXPECT_EQ(membership.live, 1U);
    }
    EXPECT_EQ(input.memory->used(), 0U);
    const auto before = metadata->openSnapshot().superblock(); input = request(2);
    input.memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1);
    io->resetStats();
    EXPECT_THROW(static_cast<void>(metadata->installBuildingScanMembershipGroup(input,
        LogSequenceNumber(before.checkpoint_lsn.value() + 1))), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(io->stats().read_calls, 0U); EXPECT_EQ(io->stats().write_calls, 0U);
    EXPECT_EQ(metadata->openSnapshot().superblock(), before); EXPECT_EQ(input.memory->used(), 0U);
}

TEST_F(BuildingScanMembershipTest, CancellationAfterPrivateDataWritesKeepsTheOldBuildingRoot) {
    insert(1, 0, {7});
    struct CancelOnWrite final : CancellationCheckpoint {
        const StorageIO &io;
        explicit CancelOnWrite(const StorageIO &value) : io(value) {}
        void check() const override { if (io.stats().write_calls) throw std::runtime_error("cancel after private write"); }
    } cancel(*io);
    auto input = request(1); input.cancellation = &cancel;
    const auto before = metadata->openSnapshot().superblock(); io->resetStats();
    EXPECT_THROW(static_cast<void>(install(input)), std::runtime_error);
    EXPECT_GT(io->stats().write_calls, 0U); EXPECT_EQ(metadata->openSnapshot().superblock(), before);
    auto reopened = RelocatableMetadataStore::openReadOnly(io);
    EXPECT_EQ(reopened->openSnapshot().superblock(), before); EXPECT_EQ(input.memory->used(), 0U);
}

TEST_F(BuildingScanMembershipTest, PartialRootWriteRecoversNoMembershipAndCompleteWriteRecoversTheWholeGroup) {
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,
            BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        initialize(true); insert(1, 0, {7}); auto input = request(1);
        const auto before = metadata->openSnapshot().superblock();
        MetadataPublishOptions options; options.bootstrap_failpoint = failpoint;
        EXPECT_THROW(static_cast<void>(install(input, options)), BootstrapPublishInterrupted);
        auto reopened = RelocatableMetadataStore::openReadOnly(io); auto pin = reopened->openSnapshot();
        EXPECT_NE(pin.superblock().flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
        if (failpoint == BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE) EXPECT_EQ(pin.superblock(), before);
        else {
            const auto desc = descriptor(*reopened, pin);
            EXPECT_EQ(desc.scan_live_node_count, 1U); EXPECT_EQ(desc.scan_membership_group_count, 1U);
            const auto member = read(*reopened, pin, desc, 1); ASSERT_TRUE(member); EXPECT_EQ(member->live, 1U);
        }
        EXPECT_EQ(input.memory->used(), 0U);
    }
}

TEST_F(BuildingScanMembershipTest, CompleteIntegrityRequiresEveryPrimaryNodeAndChecksCountsAndPhysicalOwnership) {
    insert(1, 0, {}); insert(65, 1, {7});
    auto raw = metadata->openSnapshot();
    EXPECT_THROW(static_cast<void>(verify(raw)), FormatException); raw.release();
    static_cast<void>(install(request(1)));
    auto partial = metadata->openSnapshot();
    EXPECT_THROW(static_cast<void>(verify(partial)), FormatException); partial.release();
    static_cast<void>(install(request(2)));
    auto complete = metadata->openSnapshot();
    const auto report = verify(complete);
    EXPECT_EQ(report.scan_groups, 2U); EXPECT_EQ(report.scan_nodes, 2U);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata, memory).scan()));
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(BuildingScanMembershipTest, ValidChecksumsCannotCertifyIncorrectCompleteLabelMembership) {
    insert(1, 0, {7}); static_cast<void>(install(request(1)));
    auto pin = metadata->openSnapshot();
    EXPECT_NO_THROW(static_cast<void>(verify(pin)));
    static_cast<void>(replaceValidMembershipBytes(pin, 1, [](auto &value) { value.labels[0].token = 8; }));
    EXPECT_THROW(static_cast<void>(verify(pin)), FormatException);
}

TEST_F(BuildingScanMembershipTest, ReverseOwnershipRefusesAValidRecordOwnedByAnotherDirectoryGroup) {
    insert(1, 0, {7}); static_cast<void>(install(request(1)));
    auto pin = metadata->openSnapshot();
    const auto pageId = replaceValidMembershipBytes(pin, 1, [](auto &value) { value.group_id = 2; });
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    SlottedPagePool source(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    HybridPropertyReadContext context(io, source, *metadata, pin);
    EXPECT_THROW(static_cast<void>(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, pin, pageId, memory)), FormatException);
    EXPECT_THROW(static_cast<void>(verify(pin)), FormatException);
}

TEST_F(BuildingScanMembershipTest, IntegritySourceWorkspaceRemainsChargedToTheCaller) {
    insert(1, 0, {7}); static_cast<void>(install(request(1)));
    auto pin = metadata->openSnapshot();
    SlottedPagePool source(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    HybridPropertyReadContext context(io, source, *metadata, pin);
    auto small = std::make_shared<graph::query::execution::MaterializedByteBudget>(128U * 1024U);
    EXPECT_THROW(static_cast<void>(HybridPropertyDirectoryIntegrity::verify(context, pin, small)),
        graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(small->used(), 0U);
    EXPECT_NO_THROW(static_cast<void>(verify(pin)));
}

TEST_F(BuildingScanMembershipTest, UnknownBoundedAllocatorCannotBeReplacedByAnUnchargedSourceAccount) {
    insert(1, 0, {7}); static_cast<void>(install(request(1)));
    auto pin = metadata->openSnapshot();
    SlottedPagePool source(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    HybridPropertyReadContext context(io, source, *metadata, pin);
    std::array<std::byte, 128U * 1024U> bytes{};
    std::pmr::monotonic_buffer_resource bounded(bytes.data(), bytes.size(), std::pmr::null_memory_resource());
    auto owner = std::shared_ptr<std::pmr::memory_resource>(&bounded, [](auto *) {});
    io->resetStats();
    EXPECT_THROW(static_cast<void>(HybridPropertyDirectoryIntegrity::verify(context, pin, owner)), std::invalid_argument);
    EXPECT_EQ(io->stats().read_calls, 0U); EXPECT_EQ(io->stats().write_calls, 0U);
    EXPECT_NO_THROW(static_cast<void>(verify(pin)));
}
TEST_F(BuildingScanMembershipTest, BuilderDiscoversAllLogicalGroupsWithoutRetainingIntermediateRootPins) {
    insert(129, 0, {11}); insert(1, 1, {7}); insert(64, 2, {}); insert(65, 3, {7});
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    const auto counts = ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, memory,
        PhysicalSlotId(70), PhysicalSlotId(127), 989);
    EXPECT_EQ(counts.nodes, 4U); EXPECT_EQ(counts.groups, 3U);
    EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
    auto snapshot = metadata->openSnapshot();
    EXPECT_NE(snapshot.superblock().flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
    const auto desc = descriptor(*metadata, snapshot);
    ASSERT_TRUE(read(*metadata, snapshot, desc, 1));
    EXPECT_EQ(read(*metadata, snapshot, desc, 1)->live, 1U | (UINT64_C(1) << 63));
    EXPECT_EQ(verify(snapshot).scan_nodes, 4U);
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()));
    snapshot.release();
    EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, memory,
        PhysicalSlotId(70), PhysicalSlotId(127), 989)), FormatException);
}

TEST_F(BuildingScanMembershipTest, BuilderRefusesCompletedImagesAndPreIoBudgetDenial) {
    auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(1U);
    io->resetStats();
    EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, tiny,
        PhysicalSlotId(70), PhysicalSlotId(127), 989)), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(io->stats().read_calls, 0U); EXPECT_EQ(io->stats().write_calls, 0U);
    initialize(false);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    const auto before = metadata->openSnapshot().superblock();
    io->resetStats();
    EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, memory,
        PhysicalSlotId(70), PhysicalSlotId(127), 989)), FormatException);
    EXPECT_EQ(io->stats().write_calls, 0U);
    EXPECT_EQ(metadata->openSnapshot().superblock(), before);
}

TEST_F(BuildingScanMembershipTest, BuilderCancellationAfterOnePublicationLeavesOnlyAnIncompleteBuildingImage) {
    insert(1, 0, {7}); insert(65, 1, {11});
    class CancelAfterPublication final : public CancellationCheckpoint {
        const StorageIO &io_;
    public:
        explicit CancelAfterPublication(const StorageIO &io) : io_(io) {}
        void check() const override {
            if (io_.stats().superblock_sync_calls) throw std::runtime_error("cancelled after one private group");
        }
    } cancellation(*io);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    io->resetStats();
    EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, memory,
        PhysicalSlotId(70), PhysicalSlotId(127), 989, &cancellation)), std::runtime_error);
    EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
    auto snapshot = metadata->openSnapshot();
    EXPECT_NE(snapshot.superblock().flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
    const auto desc = descriptor(*metadata, snapshot);
    EXPECT_EQ(desc.scan_membership_group_count, 1U); EXPECT_EQ(desc.scan_live_node_count, 1U);
    EXPECT_THROW(static_cast<void>(verify(snapshot)), FormatException);
    const auto writes = io->stats().write_calls;
    EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, memory,
        PhysicalSlotId(70), PhysicalSlotId(127), 989)), FormatException);
    EXPECT_EQ(io->stats().write_calls, writes);
}

TEST_F(BuildingScanMembershipTest, OfflineRawPageCacheUsesGovernedCreditAndRefundsAfterClose) {
    insert(1, 0, {7}); const auto legacy = makeLegacyRowSource();
    const std::filesystem::path target(path.string() + ".scan");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    auto cache = std::make_shared<graph::query::execution::MaterializedByteBudget>(64U * 1024U * 1024U);
    auto unknown = std::make_shared<std::pmr::monotonic_buffer_resource>();
    EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
        UINT64_C(64) * 1024U * 1024U, 989, nullptr, unknown)), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(target)); EXPECT_EQ(memory->used(), 0U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989, nullptr, cache);
        EXPECT_GT(cache->used(), 0U); EXPECT_LE(cache->used(), 64U * 1024U * 1024U);
        EXPECT_EQ(builder->membershipCounts().nodes, 1U); EXPECT_EQ(builder->sourceStats().write_calls, 0U);
    }
    EXPECT_EQ(cache->used(), 0U); EXPECT_EQ(memory->used(), 0U);
    std::ifstream input(path, std::ios::binary);
    std::vector<std::byte> unchanged(legacy.size());
    input.read(reinterpret_cast<char *>(unchanged.data()), static_cast<std::streamsize>(unchanged.size()));
    EXPECT_EQ(unchanged, legacy);
    std::filesystem::remove(target);
}

TEST_F(BuildingScanMembershipTest, OfflineBuilderClonesRealPrimaryPagesFromReadOnlyLegacySource) {
    insert(65, 0, {11}); insert(1, 1, {7}); insert(64, 2, {});
    const auto sourceRoot = metadata->openSnapshot().superblock();
    const auto legacy = makeLegacyRowSource();
    const std::filesystem::path target(path.string() + ".scan");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989);
        const auto root = builder->buildingRoot();
        EXPECT_EQ(root.database_id, sourceRoot.database_id);
        EXPECT_NE(root.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
        EXPECT_EQ(builder->membershipCounts().nodes, 3U); EXPECT_EQ(builder->membershipCounts().groups, 2U);
        EXPECT_EQ(builder->sourceStats().write_calls, 0U);
        const auto beforeRetry = builder->buildingRoot();
        const auto retained = memory->used();
        EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989)), std::system_error);
        EXPECT_EQ(builder->buildingRoot(), beforeRetry); EXPECT_EQ(memory->used(), retained);
        EXPECT_LE(builder->sourceFileBytes() + builder->targetFileBytes(), UINT64_C(64) * 1024U * 1024U);
        auto maintenanceMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
        const auto maintained = builder->reclaimMetadataOnce(maintenanceMemory);
        EXPECT_LE(maintained.reclaimed_pool_slots, 1U);
        EXPECT_NE(maintained.superblock.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
        EXPECT_EQ(maintained.superblock.hybrid_property_descriptor_root, beforeRetry.hybrid_property_descriptor_root);
        EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(ScanBuildingImageBuilderTestAccess::io(*builder),
            ScanBuildingImageBuilderTestAccess::metadata(*builder)).scan()));
    }
    EXPECT_EQ(memory->used(), 0U);
    const std::filesystem::path tight(path.string() + ".tight");
    EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, tight, memory,
        2U * legacy.size(), 989)), StorageFileLimitExceeded);
    EXPECT_EQ(memory->used(), 0U);
    ASSERT_TRUE(std::filesystem::exists(tight)); EXPECT_LE(std::filesystem::file_size(tight), legacy.size());
    { std::ifstream partial(tight, std::ios::binary);
      std::array<std::byte, RSS_SUPERBLOCK_BYTES> bytes{};
      partial.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
      ASSERT_TRUE(partial.good());
      EXPECT_NE(SuperblockCodec::decode(bytes).flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U); }
    std::ifstream input(path, std::ios::binary);
    std::vector<std::byte> unchanged(legacy.size());
    input.read(reinterpret_cast<char *>(unchanged.data()), static_cast<std::streamsize>(unchanged.size()));
    EXPECT_EQ(unchanged, legacy);
    std::error_code ignored; std::filesystem::remove(target, ignored); std::filesystem::remove(tight, ignored);
}

TEST_F(BuildingScanMembershipTest, PrivateFieldConstructionDefersWholeFileGateAndPreservesMembershipAndFallbackValues) {
    LargeValueStore values(io, *metadata);
    PropertyCollectionRootStore properties(*pages, values, *metadata);
    BTreePageStore indexes(*pages, *metadata);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(127);
    options.entity_pool_owner_id = 901;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(127), 989};
    NativeGraphWritePlanner planner(*metadata, *entities, *labels, properties, indexes, options);
    std::vector<PropertyCollectionRoot> expected;
    {
        auto transaction = planner.beginTransaction();
        for (size_t i = 0; i < 64U; ++i) {
            const auto id = transaction.createNode({});
            PropertyCollectionRoot row{id, graph::Node::typeId,
                {{1, graph::PropertyValue(int64_t(id))}, {2, graph::PropertyValue("shared")},
                 {3, graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(int64_t{7})})}}};
            if (i % 2U == 0) row.entries.push_back({4, graph::PropertyValue()});
            expected.push_back(row); transaction.replaceNodeProperty(std::move(row));
        }
        for (size_t i = 0; i < 64U; ++i) {
            const auto id = transaction.createEdge(1U, 2U, 1U);
            PropertyCollectionRoot row{id, graph::Edge::typeId, {{5, graph::PropertyValue(int64_t(id * 10U))}}};
            expected.push_back(row); transaction.replaceEdgeProperty(std::move(row));
        }
        ASSERT_TRUE(transaction.commit().committed());
    }
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    const auto counts = ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, memory,
        PhysicalSlotId(40), PhysicalSlotId(127), 989);
    ASSERT_EQ(counts.nodes, 64U);
    auto old = metadata->openSnapshot(); const auto before = descriptor(*metadata, old);
    const auto fields = ScanBuildingImageBuilder::constructFieldsForBuilding(io, *metadata, memory,
        PhysicalSlotId(40), PhysicalSlotId(127), 989);
    EXPECT_EQ(fields.examined_groups, 2U); EXPECT_EQ(fields.converted_groups, 1U);
    EXPECT_EQ(fields.row_fallback_groups, 1U);
    auto current = metadata->openSnapshot(); const auto after = descriptor(*metadata, current);
    EXPECT_NE(current.superblock().flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
    EXPECT_EQ(after.scan_live_node_count, before.scan_live_node_count);
    EXPECT_EQ(after.scan_membership_group_count, before.scan_membership_group_count);
    EXPECT_EQ(after.roots[8], before.roots[8]);
    EXPECT_TRUE(after.roots[static_cast<size_t>(HybridPropertyRootOrdinal::HPRO_NODE_GROUP)].page_id.isValid());
    EXPECT_FALSE(after.roots[static_cast<size_t>(HybridPropertyRootOrdinal::HPRO_EDGE_GROUP)].page_id.isValid());
    graph::utils::OwnedAllocationScope readerAccount(memory);
    for (const auto &row : expected) {
        const EntityPropertyKey key{row.owner_entity_type == graph::Node::typeId ?
            EntityPropertyOwnerKind::EPOK_NODE : EntityPropertyOwnerKind::EPOK_EDGE, row.owner_entity_id};
        EXPECT_EQ(properties.readEntityOptional(current, key), std::optional(row));
        EXPECT_EQ(properties.readEntityOptional(old, key), std::optional(row));
    }
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata).scan()));
    old.release(); current.release();
    auto reopened = RelocatableMetadataStore::openReadOnly(io);
    auto pin = reopened->openSnapshot();
    EXPECT_EQ(descriptor(*reopened, pin).roots[8], before.roots[8]);
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *reopened).scan()));
}

TEST_F(BuildingScanMembershipTest, OversizedFieldSourceKeepsTheWholeRowGroupWithoutPublishingAPrefix) {
    LargeValueStore values(io, *metadata);
    PropertyCollectionRootStore properties(*pages, values, *metadata);
    BTreePageStore indexes(*pages, *metadata);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(127);
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(127), 989};
    NativeGraphWritePlanner planner(*metadata, *entities, *labels, properties, indexes, options);
    const std::string large(2U * 1024U * 1024U, 'v');
    uint64_t id = 0;
    {
        auto tx = planner.beginTransaction(); id = tx.createNode({});
        tx.replaceNodeProperty({id, graph::Node::typeId, {{1, graph::PropertyValue(large)}}});
        ASSERT_TRUE(tx.commit().committed());
    }
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, memory,
        PhysicalSlotId(40), PhysicalSlotId(127), 989));
    const auto before = metadata->openSnapshot().superblock(); io->resetStats();
    const auto result = ScanBuildingImageBuilder::constructFieldsForBuilding(io, *metadata, memory,
        PhysicalSlotId(40), PhysicalSlotId(127), 989);
    EXPECT_EQ(result.examined_groups, 1U); EXPECT_EQ(result.converted_groups, 0U);
    EXPECT_EQ(result.row_fallback_groups, 1U); EXPECT_EQ(io->stats().write_calls, 0U);
    EXPECT_EQ(metadata->openSnapshot().superblock(), before);
    EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
    auto pin = metadata->openSnapshot();
    auto outputMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
    graph::storage::detail::PropertyDecodeWorkspace output(outputMemory);
    const auto row = properties.readEntityOptional(pin, {EntityPropertyOwnerKind::EPOK_NODE, id});
    ASSERT_TRUE(row); ASSERT_EQ(row->entries.size(), 1U);
    EXPECT_EQ(row->entries[0].value, graph::PropertyValue(large));
    EXPECT_NE(pin.superblock().flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
}

TEST_F(BuildingScanMembershipTest, CancellationWithAnAdmissionExceptionIsNotMisclassifiedAsRowFallback) {
    LargeValueStore values(io, *metadata);
    PropertyCollectionRootStore properties(*pages, values, *metadata);
    BTreePageStore indexes(*pages, *metadata);
    NativeGraphWritePlannerOptions options;
    options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(127);
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(127), 989};
    NativeGraphWritePlanner planner(*metadata, *entities, *labels, properties, indexes, options);
    { auto tx = planner.beginTransaction(); const auto id = tx.createNode({});
      tx.replaceNodeProperty({id, graph::Node::typeId, {{1, graph::PropertyValue(int64_t{7})}}});
      ASSERT_TRUE(tx.commit().committed()); }
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, memory,
        PhysicalSlotId(40), PhysicalSlotId(127), 989));
    bool preflight = false;
    graph::debug::PerfTrace::ScopedObserver observer(&preflight,
        [](void *p, std::string_view key, int64_t, bool) noexcept {
            if (key == "native_graph.scan_build.field_group_preflight") *static_cast<bool *>(p) = true;
        });
    class Cancel final : public CancellationCheckpoint {
        const bool &preflight_;
    public:
        explicit Cancel(const bool &preflight) : preflight_(preflight) {}
        void check() const override {
            if (preflight_) throw graph::query::execution::MaterializedByteLimitExceeded();
        }
    } cancellation(preflight);
    const auto before = metadata->openSnapshot().superblock(); io->resetStats();
    EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::constructFieldsForBuilding(io, *metadata, memory,
        PhysicalSlotId(40), PhysicalSlotId(127), 989, &cancellation)), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_TRUE(preflight); EXPECT_EQ(io->stats().write_calls, 0U);
    EXPECT_EQ(metadata->openSnapshot().superblock(), before); EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
}

TEST_F(BuildingScanMembershipTest, RealLegacyCloneBuildsNodeFieldsAndPreservesEdgeRowsBeforeCompletePublication) {
    std::vector<PropertyCollectionRoot> expected;
    {
        LargeValueStore values(io, *metadata);
        PropertyCollectionRootStore properties(*pages, values, *metadata);
        BTreePageStore indexes(*pages, *metadata);
        NativeGraphWritePlannerOptions options;
        options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(127);
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(127), 989};
        NativeGraphWritePlanner planner(*metadata, *entities, *labels, properties, indexes, options);
        auto tx = planner.beginTransaction();
        for (size_t i = 0; i < 64U; ++i) {
            const auto id = tx.createNode({});
            PropertyCollectionRoot row{id, graph::Node::typeId,
                {{1, graph::PropertyValue(int64_t(id))}, {2, graph::PropertyValue("text")},
                 {3, graph::PropertyValue(std::vector<graph::PropertyValue>{graph::PropertyValue(int64_t{7})})}}};
            expected.push_back(row); tx.replaceNodeProperty(std::move(row));
        }
        for (size_t i = 0; i < 64U; ++i) {
            const auto id = tx.createEdge(1U, 2U, 1U);
            PropertyCollectionRoot row{id, graph::Edge::typeId, {{1, graph::PropertyValue(int64_t(id))}}};
            if (!i) {
                std::string large(8192U, 'x'); large[4096U] = '\0';
                row.entries.push_back({2, graph::PropertyValue(std::move(large))});
            }
            expected.push_back(row); tx.replaceEdgeProperty(std::move(row));
        }
        ASSERT_TRUE(tx.commit().committed());
    }
    const auto source = makeLegacyRowSource();
    const std::filesystem::path target(path.string() + ".fields");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989);
        const auto counts = builder->constructFields();
        EXPECT_EQ(counts.examined_groups, 2U); EXPECT_EQ(counts.converted_groups, 1U);
        EXPECT_EQ(counts.row_fallback_groups, 1U);
        EXPECT_THROW(static_cast<void>(builder->constructFields()), std::logic_error);
        EXPECT_EQ(builder->membershipCounts().nodes, 64U);
        EXPECT_EQ(builder->sourceStats().write_calls, 0U);
        EXPECT_NE(builder->buildingRoot().flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
        auto &store = ScanBuildingImageBuilderTestAccess::metadata(*builder);
        const auto targetIo = ScanBuildingImageBuilderTestAccess::io(*builder);
        SlottedPagePool readPages(targetIo, store, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        LargeValueStore values(targetIo, store, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        PropertyCollectionRootStore properties(readPages, values, store);
        auto pin = store.openSnapshot();
        auto outputMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U * 1024U * 1024U);
        graph::storage::detail::PropertyDecodeWorkspace output(outputMemory);
        for (const auto &row : expected) {
            const EntityPropertyKey key{row.owner_entity_type == graph::Node::typeId ?
                EntityPropertyOwnerKind::EPOK_NODE : EntityPropertyOwnerKind::EPOK_EDGE, row.owner_entity_id};
            EXPECT_EQ(properties.readEntityOptional(pin, key), std::optional(row));
        }
        // The preserved ROW remainder can occupy the physical tail. Moving
        // it must keep logical slots, values, membership and old-pin routes.
        std::optional<PageLocation> rowLocation;
        store.visitCatalogEntries(pin, [&](const CatalogEntry &pool) {
            if (rowLocation || pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL || pool.state != SegmentState::SGS_SEALED) return;
            const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(pin.superblock().data_region_offset, pool.physical_slot);
            for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
                std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES> bytes{};
                ASSERT_EQ(targetIo->readAt(offset + uint64_t(frame) * RSS_PAGE_SIZE, bytes.data(), bytes.size()), bytes.size());
                std::optional<PageOwnerHeader> owner;
                try { owner = PageOwnerHeaderCodec::decode(bytes); } catch (const FormatException &) { continue; }
                const auto route = store.findPageLocationOptional(pin, owner->page_id);
                if (owner->kind == PageOwnerKind::POK_PROPERTY && route && route->segment_id == pool.segment_id && route->page_index == frame) {
                    rowLocation = route; break;
                }
            }
        });
        ASSERT_TRUE(rowLocation);
        const auto before = pin.superblock();
        NativeHybridPropertyPoolRelocation move{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, rowLocation->segment_id, PhysicalSlotId(5), PhysicalSlotId(39), memory};
        struct CancelMove final : CancellationCheckpoint {
            void check() const override { throw std::runtime_error("cancel ROW pool move"); }
        } cancel;
        move.cancellation = &cancel; targetIo->resetStats();
        EXPECT_THROW(static_cast<void>(store.relocateHybridPropertyFieldPool(move,
            LogSequenceNumber(before.checkpoint_lsn.value() + 1U))), std::runtime_error);
        EXPECT_EQ(targetIo->stats().write_calls, 0U); EXPECT_EQ(store.openSnapshot().superblock(), before);
        move.cancellation = nullptr;
        ASSERT_NO_THROW(static_cast<void>(store.relocateHybridPropertyFieldPool(move,
            LogSequenceNumber(before.checkpoint_lsn.value() + 1U))));
        auto moved = store.openSnapshot();
        EXPECT_EQ(moved.superblock().hybrid_property_descriptor_root, before.hybrid_property_descriptor_root);
        EXPECT_NE(store.findPageLocation(moved, rowLocation->page_id).segment_id, rowLocation->segment_id);
        EXPECT_EQ(store.findPageLocation(pin, rowLocation->page_id), *rowLocation);
        for (const auto &row : expected) {
            const EntityPropertyKey key{row.owner_entity_type == graph::Node::typeId ?
                EntityPropertyOwnerKind::EPOK_NODE : EntityPropertyOwnerKind::EPOK_EDGE, row.owner_entity_id};
            EXPECT_EQ(properties.readEntityOptional(moved, key), std::optional(row));
            EXPECT_EQ(properties.readEntityOptional(pin, key), std::optional(row));
        }
        EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo, store, outputMemory).scan()));
    }
    EXPECT_EQ(memory->used(), 0U);
    std::ifstream input(path, std::ios::binary); std::vector<std::byte> unchanged(source.size());
    input.read(reinterpret_cast<char *>(unchanged.data()), static_cast<std::streamsize>(unchanged.size()));
    EXPECT_EQ(unchanged, source);
    std::error_code ignored; std::filesystem::remove(target, ignored);
}

TEST_F(BuildingScanMembershipTest, MoreThan64TypedFieldsKeepAllValuesInTheOriginalRowGroup) {
    LargeValueStore values(io, *metadata); PropertyCollectionRootStore properties(*pages, values, *metadata);
    BTreePageStore indexes(*pages, *metadata);
    NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(127);
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(127), 989};
    NativeGraphWritePlanner planner(*metadata, *entities, *labels, properties, indexes, options);
    std::vector<PropertyCollectionRoot> expected;
    { auto tx = planner.beginTransaction();
      for (size_t i = 0; i < 64U; ++i) {
          const auto id = tx.createNode({});
          if (i >= 48U) continue;
          PropertyCollectionRoot row{id, graph::Node::typeId, {}};
          for (uint32_t token = 1; token <= 65U; ++token) row.entries.push_back({token, graph::PropertyValue(int64_t(id + token))});
          expected.push_back(row); tx.replaceNodeProperty(std::move(row));
      }
      ASSERT_TRUE(tx.commit().committed()); }
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, memory,
        PhysicalSlotId(40), PhysicalSlotId(127), 989));
    int64_t columns = -1;
    graph::debug::PerfTrace::ScopedObserver observer(&columns,
        [](void *p, std::string_view key, int64_t value, bool) noexcept {
            if (key == "native_graph.scan_build.field_group_typed_columns") *static_cast<int64_t *>(p) = value;
        });
    const auto before = metadata->openSnapshot().superblock(); io->resetStats();
    const auto result = ScanBuildingImageBuilder::constructFieldsForBuilding(io, *metadata, memory,
        PhysicalSlotId(40), PhysicalSlotId(127), 989);
    EXPECT_EQ(columns, 65); EXPECT_EQ(result.examined_groups, 1U);
    EXPECT_EQ(result.converted_groups, 0U); EXPECT_EQ(result.row_fallback_groups, 1U);
    EXPECT_EQ(io->stats().write_calls, 0U); EXPECT_EQ(metadata->openSnapshot().superblock(), before);
    auto pin = metadata->openSnapshot();
    for (const auto &row : expected)
        EXPECT_EQ(properties.readEntityOptional(pin, {EntityPropertyOwnerKind::EPOK_NODE, row.owner_entity_id}), std::optional(row));
    EXPECT_EQ(descriptor(*metadata, pin).scan_live_node_count, 64U);
}

TEST_F(BuildingScanMembershipTest, OrdinaryRootCannotMintDeferredAdmissionAtEitherWriteBoundary) {
    initialize(false);
    LargeValueStore values(io, *metadata); PropertyCollectionRootStore properties(*pages, values, *metadata);
    BTreePageStore indexes(*pages, *metadata);
    NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(127);
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(127), 989};
    NativeGraphWritePlanner planner(*metadata, *entities, *labels, properties, indexes, options);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    auto tx = planner.beginTransaction(); const auto before = tx.baseSuperblock(); io->resetStats();
    EXPECT_THROW(tx.stageHybridPropertyConversion(EntityPropertyOwnerKind::EPOK_NODE, 1U, memory, nullptr, true,
        NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, true, {}, 1U, true), FormatException);
    EXPECT_EQ(io->stats().read_calls, 0U); EXPECT_EQ(io->stats().write_calls, 0U); tx.rollback();
    NativeGraphHybridPropertyRootMutation mutation{before.database_id, before.hybrid_property_descriptor_root,
        NativeHybridPropertyRootMode::NHPRM_REPLACE, {}, NativeGraphHybridPropertyConversion{
            EntityPropertyOwnerKind::EPOK_NODE, 1U, memory, nullptr, PhysicalSlotId(40), PhysicalSlotId(127), 989,
            true, NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID, {}, true, 1U, true}};
    io->resetStats();
    EXPECT_THROW(static_cast<void>(metadata->mutateNativeGraphPagesWithLargeValues(
        {}, {}, {}, {}, {}, {}, {}, LogSequenceNumber(before.checkpoint_lsn.value() + 1U),
        [](auto, auto, auto, auto, auto, auto) {}, [](auto &) {}, {}, {}, {}, {}, {}, {}, {},
        before.generation, {}, {}, {}, {}, {}, mutation)), FormatException);
    EXPECT_EQ(io->stats().write_calls, 0U); EXPECT_EQ(metadata->openSnapshot().superblock(), before);
}

TEST_F(BuildingScanMembershipTest, BuilderAuthenticatesAllRowSlotsBeforeFieldNormalization) {
    {
        PropertyCollectionRootStore properties(*pages, *metadata);
        BTreePageStore indexes(*pages, *metadata);
        NativeGraphWritePlannerOptions options;
        options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(127);
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(127), 989};
        NativeGraphWritePlanner planner(*metadata, *entities, *labels, properties, indexes, options);
        auto tx = planner.beginTransaction();
        const auto id = tx.createNode({});
        tx.replaceNodeProperty({id, graph::Node::typeId, {{1, graph::PropertyValue(int64_t{7})}}});
        ASSERT_TRUE(tx.commit().committed());
    }
    const EntityPropertyKey erased{EntityPropertyOwnerKind::EPOK_NODE, 1};
    const auto sourceRoot = metadata->openSnapshot().superblock();
    static_cast<void>(metadata->upsertEntityPropertyLocations({}, std::span(&erased, 1),
        LogSequenceNumber(sourceRoot.checkpoint_lsn.value() + 1U)));
    static_cast<void>(makeLegacyRowSource());
    const std::filesystem::path target(path.string() + ".scan");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989);
        auto &store = ScanBuildingImageBuilderTestAccess::metadata(*builder);
        const auto targetIo = ScanBuildingImageBuilderTestAccess::io(*builder);
        const auto corrupt = builder->buildingRoot();
        const auto writes = targetIo->stats().write_calls;
        EXPECT_THROW(static_cast<void>(builder->constructFields()), FormatException);
        EXPECT_EQ(builder->buildingRoot(), corrupt);
        EXPECT_EQ(targetIo->stats().write_calls, writes);
        EXPECT_EQ(store.activeSnapshotCount(), 0U);
        EXPECT_NE(corrupt.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
    }
    EXPECT_EQ(memory->used(), 0U);
    std::error_code ignored; std::filesystem::remove(target, ignored);
}

TEST_F(BuildingScanMembershipTest, SourceLengthChangeDiscardsThePrivateImageBeforeFieldWrites) {
    insert(1, 0, {});
    static_cast<void>(makeLegacyRowSource());
    const std::filesystem::path target(path.string() + ".scan");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989);
        { std::ofstream changed(path, std::ios::binary | std::ios::app); changed.put('x'); }
        EXPECT_THROW(static_cast<void>(builder->constructFields()), FormatException);
        EXPECT_FALSE(std::filesystem::exists(target));
        EXPECT_THROW(static_cast<void>(builder->buildingRoot()), std::logic_error);
        EXPECT_THROW(static_cast<void>(builder->targetFileBytes()), std::logic_error);
        EXPECT_THROW(static_cast<void>(builder->constructFields()), FormatException);
        EXPECT_EQ(builder->sourceStats().write_calls, 0U);
    }
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(BuildingScanMembershipTest, SourceChangeDuringCloneDeletesTheExclusivePrivateTarget) {
    insert(1, 0, {});
    static_cast<void>(makeLegacyRowSource());
    const std::filesystem::path target(path.string() + ".scan");
    struct ChangeSource final : CancellationCheckpoint {
        std::filesystem::path source, target;
        mutable bool changed = false;
        void check() const override {
            if (!changed && std::filesystem::exists(target) &&
                std::filesystem::file_size(target) >= 2U * RSS_SUPERBLOCK_BYTES) {
                std::ofstream stream(source, std::ios::binary | std::ios::app); stream.put('x');
                changed = true;
            }
        }
    } change;
    change.source = path; change.target = target;
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
        UINT64_C(64) * 1024U * 1024U, 989, &change)), FormatException);
    EXPECT_TRUE(change.changed);
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_EQ(memory->used(), 0U);
}

#ifndef _WIN32
TEST_F(BuildingScanMembershipTest, SameBytesAtAReplacedSourcePathDoNotAuthorizeTheOpenedImage) {
    insert(1, 0, {});
    const auto legacy = makeLegacyRowSource();
    const std::filesystem::path target(path.string() + ".scan"), moved(path.string() + ".moved");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989);
        std::filesystem::rename(path, moved);
        { std::ofstream replacement(path, std::ios::binary); replacement.write(
            reinterpret_cast<const char *>(legacy.data()), static_cast<std::streamsize>(legacy.size())); }
        EXPECT_THROW(static_cast<void>(builder->constructFields()), FormatException);
        EXPECT_FALSE(std::filesystem::exists(target));
    }
    EXPECT_EQ(memory->used(), 0U);
    std::error_code ignored; std::filesystem::remove(moved, ignored);
}
#endif

TEST_F(BuildingScanMembershipTest, MetadataMaintenanceRefusesOrdinaryStaleAndUnadmittedRequests) {
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    const auto requestFor = [&](const Superblock &root) {
        auto request = BuildingScanMembershipTestAccess::maintenance();
        request.expected_database_id = root.database_id; request.expected_generation = root.generation;
        request.expected_descriptor = root.hybrid_property_descriptor_root; request.memory = memory;
        return request;
    };
    initialize(false);
    auto root = metadata->openSnapshot().superblock(); auto request = requestFor(root);
    io->resetStats();
    EXPECT_THROW(static_cast<void>(metadata->reclaimOneBuildingScanMetadataPool(request, LogSequenceNumber(2))), FormatException);
    EXPECT_EQ(io->stats().write_calls, 0U); EXPECT_EQ(memory->used(), 0U);
    initialize(true); root = metadata->openSnapshot().superblock(); request = requestFor(root);
    request.expected_generation = Generation(root.generation.value() + 1U);
    io->resetStats();
    EXPECT_THROW(static_cast<void>(metadata->reclaimOneBuildingScanMetadataPool(request, LogSequenceNumber(2))), FormatException);
    EXPECT_EQ(io->stats().write_calls, 0U); EXPECT_EQ(memory->used(), 0U);
    request = requestFor(root);
    request.memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(256U);
    io->resetStats();
    EXPECT_THROW(static_cast<void>(metadata->reclaimOneBuildingScanMetadataPool(request, LogSequenceNumber(2))),
        graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(io->stats().read_calls, 0U); EXPECT_EQ(io->stats().write_calls, 0U);
    EXPECT_EQ(request.memory->used(), 0U);
    struct Cancel final : CancellationCheckpoint {
        void check() const override { throw std::runtime_error("cancel metadata maintenance"); }
    } cancel;
    request = requestFor(root); request.cancellation = &cancel;
    io->resetStats();
    EXPECT_THROW(static_cast<void>(metadata->reclaimOneBuildingScanMetadataPool(request, LogSequenceNumber(2))), std::runtime_error);
    EXPECT_EQ(io->stats().read_calls, 0U); EXPECT_EQ(io->stats().write_calls, 0U);
    EXPECT_EQ(memory->used(), 0U);

}

TEST_F(BuildingScanMembershipTest, MetadataMaintenanceReleasesAtMostOnePoolAndPreservesPinnedRoots) {
    MetadataSnapshotPin old;
    std::vector<MetadataSnapshotPin> history;
    ASSERT_NO_FATAL_FAILURE(seedPinnedNodeVersions(24U, old, history));
    const auto original = entities->readNode(old, 1);
    auto sourceMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, sourceMemory,
        PhysicalSlotId(70), PhysicalSlotId(127), 989));
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    const auto run = [&] {
        const auto root = metadata->openSnapshot().superblock();
        auto request = BuildingScanMembershipTestAccess::maintenance();
        request.expected_database_id = root.database_id; request.expected_generation = root.generation;
        request.expected_descriptor = root.hybrid_property_descriptor_root; request.memory = memory;
        auto result = metadata->reclaimOneBuildingScanMetadataPool(request, LogSequenceNumber(root.checkpoint_lsn.value() + 1U));
        EXPECT_LE(result.reclaimed_pool_slots, 1U);
        EXPECT_EQ(result.released_pool_slots.size(), result.reclaimed_pool_slots);
        EXPECT_EQ(result.superblock.hybrid_property_descriptor_root, root.hybrid_property_descriptor_root);
        EXPECT_NE(result.superblock.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
        EXPECT_EQ(memory->used(), result.released_pool_slots.capacity() * sizeof(PhysicalSlotId));
        return result;
    };
    static_cast<void>(run());
    EXPECT_EQ(entities->readNode(old, 1), original);
    old.release(); history.clear();
    uint64_t released = 0;
    for (size_t i = 0; i < 32U; ++i) {
        const auto before = metadata->openSnapshot().superblock();
        const auto result = run(); released += result.reclaimed_pool_slots;
        if (result.superblock == before) break;
    }
    EXPECT_GT(released, 0U);
    EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata, memory).scan()));
    EXPECT_EQ(memory->used(), 0U);
}


TEST_F(BuildingScanMembershipTest, MetadataMaintenanceAdmitsLegacyRouteWorkspaceBeforeCollection) {
    MetadataSnapshotPin old; std::vector<MetadataSnapshotPin> history;
    ASSERT_NO_FATAL_FAILURE(seedPinnedNodeVersions(24U, old, history));
    auto sourceMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, sourceMemory,
        PhysicalSlotId(70), PhysicalSlotId(127), 989));
    const auto before = metadata->openSnapshot().superblock();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(40U * 1024U);
    auto request = BuildingScanMembershipTestAccess::maintenance();
    request.expected_database_id = before.database_id; request.expected_generation = before.generation;
    request.expected_descriptor = before.hybrid_property_descriptor_root; request.memory = memory;
    EXPECT_THROW(static_cast<void>(metadata->reclaimOneBuildingScanMetadataPool(request,
        LogSequenceNumber(before.checkpoint_lsn.value() + 1U))), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(metadata->openSnapshot().superblock(), before); EXPECT_EQ(memory->used(), 0U);
    EXPECT_EQ(entities->readNode(old, 1).inline_label_count, 0U);
    old.release(); history.clear();
    auto verification = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *metadata, verification).scan()));
}

TEST_F(BuildingScanMembershipTest, MetadataMaintenanceABFaultsRecoverACompleteBuildingRoot) {
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,
            BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        initialize(true);
        MetadataSnapshotPin old; std::vector<MetadataSnapshotPin> history;
        ASSERT_NO_FATAL_FAILURE(seedPinnedNodeVersions(24U, old, history));
        auto sourceMemory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata, sourceMemory,
            PhysicalSlotId(70), PhysicalSlotId(127), 989));
        old.release(); history.clear();
        const auto before = metadata->openSnapshot().superblock();
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
        auto request = BuildingScanMembershipTestAccess::maintenance();
        request.expected_database_id = before.database_id; request.expected_generation = before.generation;
        request.expected_descriptor = before.hybrid_property_descriptor_root; request.memory = memory;
        MetadataPublishOptions options; options.bootstrap_failpoint = failpoint;
        EXPECT_THROW(static_cast<void>(metadata->reclaimOneBuildingScanMetadataPool(request,
            LogSequenceNumber(before.checkpoint_lsn.value() + 1U), options)), BootstrapPublishInterrupted);
        auto recovered = RelocatableMetadataStore::openReadOnly(io);
        const auto root = recovered->openSnapshot().superblock();
        EXPECT_EQ(root.database_id, before.database_id);
        EXPECT_TRUE(root.generation == before.generation || root.generation.value() == before.generation.value() + 1U);
        EXPECT_EQ(root.hybrid_property_descriptor_root, before.hybrid_property_descriptor_root);
        EXPECT_NE(root.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
        EXPECT_EQ(metadata->activeSnapshotCount(), 0U);
        EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *recovered, memory).scan()));
        EXPECT_EQ(memory->used(), 0U);
    }
}

TEST_F(BuildingScanMembershipTest, PrivateImageMovesTheOrdinaryAdjacencyPoolWithoutChangingMembershipAuthority) {
    {
        PropertyCollectionRootStore properties(*pages, *metadata);
        BTreePageStore indexes(*pages, *metadata);
        NativeGraphWritePlannerOptions options;
        options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(127);
        options.entity_pool_owner_id = 901;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(127), 989};
        NativeGraphWritePlanner planner(*metadata, *entities, *labels, properties, indexes, options);
        auto transaction = planner.beginTransaction();
        const auto first = transaction.createNode({}), second = transaction.createNode({});
        static_cast<void>(transaction.createEdge(first, second, 1U));
        ASSERT_TRUE(transaction.commit().committed());
    }
    static_cast<void>(makeLegacyRowSource());
    const std::filesystem::path target(path.string() + ".scan");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989);
        EXPECT_EQ(builder->constructFields().examined_groups, 0U);
        auto &store = ScanBuildingImageBuilderTestAccess::metadata(*builder);
        const auto targetIo = ScanBuildingImageBuilderTestAccess::io(*builder);
        auto old = store.openSnapshot(); const auto before = old.superblock();
        const auto root = store.findIndexRoot(old, RSS_GLOBAL_ADJACENCY_INDEX_ID);
        const auto location = store.findPageLocation(old, root.root_page_id);
        NativeHybridPropertyPoolRelocation request{before.database_id, before.generation,
            before.hybrid_property_descriptor_root, location.segment_id, PhysicalSlotId(5), PhysicalSlotId(39), memory};
        ASSERT_NO_THROW(static_cast<void>(store.relocateHybridPropertyIndexPool(request,
            LogSequenceNumber(before.checkpoint_lsn.value() + 1U))));
        auto current = store.openSnapshot();
        EXPECT_EQ(current.superblock().hybrid_property_descriptor_root, before.hybrid_property_descriptor_root);
        EXPECT_NE(store.findPageLocation(current, root.root_page_id).segment_id, location.segment_id);
        EXPECT_EQ(store.findPageLocation(old, root.root_page_id), location);
        EXPECT_NE(current.superblock().flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
        current.release(); old.release();
        auto verification = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
        EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo, store, verification).scan()));
        EXPECT_EQ(builder->sourceStats().write_calls, 0U);
    }
    EXPECT_EQ(memory->used(), 0U);
    std::error_code ignored; std::filesystem::remove(target, ignored);
}

TEST_F(BuildingScanMembershipTest, PrivateDataCompactionAndRetirementShrinkTheActualFileAndKeepBuilding) {
    insert(1, 0, {7});
    static_cast<void>(makeLegacyRowSource());
    const auto originalBytes = std::filesystem::file_size(path) + 16U * RSS_SEGMENT_SIZE;
    std::filesystem::resize_file(path, originalBytes); // Valid allocator-free complete trailing slots.
    const std::filesystem::path target(path.string() + ".scan");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    auto maintenance = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989);
        EXPECT_EQ(builder->constructFields().examined_groups, 0U);
        size_t moves = 0, releases = 0;
        bool stopped = false;
        for (size_t step = 0; step < 64U; ++step) {
            const auto release = builder->releaseRetiredPoolOnce(maintenance);
            ASSERT_FALSE(release.blocked);
            if (release.released) { ++releases; continue; }
            const auto before = builder->buildingRoot();
            const auto metadataRelease = builder->reclaimMetadataOnce(maintenance);
            if (metadataRelease.superblock != before) continue;
            if (builder->compactDataPoolOnce(maintenance)) { ++moves; continue; }
            stopped = true; break;
        }
        ASSERT_TRUE(stopped);
        EXPECT_GT(moves, 0U); EXPECT_EQ(releases, moves);
        const auto truncated = builder->truncateReleasedTailOnce(maintenance);
        EXPECT_EQ(truncated.status, RelocatableTailTruncationStatus::VTTS_TRUNCATED);
        EXPECT_EQ(builder->targetFileBytes(), truncated.new_file_bytes);
        EXPECT_LE(builder->targetFileBytes() * 10U, builder->sourceFileBytes() * 9U);
        EXPECT_EQ(std::filesystem::file_size(path), originalBytes);
        EXPECT_EQ(builder->sourceStats().write_calls, 0U);
        EXPECT_NE(builder->buildingRoot().flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
        auto targetIo = ScanBuildingImageBuilderTestAccess::io(*builder);
        auto reopened = RelocatableMetadataStore::openReadOnly(targetIo);
        EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo, *reopened, maintenance).scan()));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(maintenance->used(), 0U);
    std::error_code ignored; std::filesystem::remove(target, ignored);
}




TEST_F(BuildingScanMembershipTest, SparseMetadataCompactionEvacuatesOnePoolBelowItsSourceAndRetainsBuilding) {
    initialize(true, true);
    MetadataSnapshotPin oldest; std::vector<MetadataSnapshotPin> history;
    ASSERT_NO_FATAL_FAILURE(seedPinnedNodeVersions(24U, oldest, history));
    oldest.release(); history.clear(); static_cast<void>(makeLegacyRowSource());
    const std::filesystem::path target(path.string() + ".scan");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    auto maintenance = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989);
        static_cast<void>(builder->constructFields(nullptr, maintenance));
        size_t metadataMoves = 0;
        std::array<int64_t, 3> diagnostic{};
        graph::debug::PerfTrace::ScopedObserver observer(&diagnostic,
            [](void *owner, std::string_view key, int64_t value, bool duration) noexcept {
                if (duration) return;
                auto &values = *static_cast<std::array<int64_t, 3> *>(owner);
                if (key == "native_graph.scan_build.metadata_source_pool") values[0] = value;
                if (key == "native_graph.scan_build.metadata_source_frames") values[1] = value;
                if (key == "native_graph.scan_build.metadata_lower_free_frames") values[2] = value;
            });
        bool stopped = false;
        for (size_t step = 0; step < 128U; ++step) {
            const auto released = builder->releaseRetiredPoolOnce(maintenance);
            ASSERT_FALSE(released.blocked);
            if (released.released) continue;
            const auto before = builder->buildingRoot();
            if (builder->reclaimMetadataOnce(maintenance).superblock != before) continue;
            if (builder->compactDataPoolOnce(maintenance)) continue;
            auto &store = ScanBuildingImageBuilderTestAccess::metadata(*builder);
            auto targetIo = ScanBuildingImageBuilderTestAccess::io(*builder);
            auto old = store.openSnapshot();
            ASSERT_TRUE(old.superblock().usesPageDirectoryBootstrap());
            const auto oldDescriptor = old.superblock().hybrid_property_descriptor_root;
            MetadataPageFile oldPages(targetIo, old.superblock());
            const auto descriptorBefore = oldPages.readHybridPropertyDescriptor(oldDescriptor, old.superblock());
            const auto moved = builder->compactMetadataPoolOnce(maintenance);
            EXPECT_EQ(oldPages.readHybridPropertyDescriptor(oldDescriptor, old.superblock()), descriptorBefore);
            EntityPageStore entities(targetIo, store, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            EXPECT_EQ(entities.readNode(old, 1).inline_label_count, 0U);
            old.release();
            if (moved) {
                ++metadataMoves;
                EXPECT_LT(moved->page_directory_root.physical_slot.value(), uint64_t(diagnostic[0]));
                MetadataPageFile currentPages(targetIo, *moved);
                EXPECT_EQ(currentPages.readHybridPropertyDescriptor(moved->hybrid_property_descriptor_root, *moved).descriptor,
                    descriptorBefore.descriptor);
                EXPECT_NE(moved->flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
                EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo, store, maintenance).scan()));
                continue;
            }
            stopped = true; break;
        }
        SCOPED_TRACE(::testing::Message() << "pool=" << diagnostic[0] << " frames=" << diagnostic[1]
            << " lower_free=" << diagnostic[2]);
        EXPECT_TRUE(stopped); EXPECT_GT(metadataMoves, 0U);
        EXPECT_EQ(builder->membershipCounts().nodes, 24U);
        EXPECT_EQ(builder->sourceStats().write_calls, 0U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(maintenance->used(), 0U);
    std::error_code ignored; std::filesystem::remove(target, ignored);
}


TEST_F(BuildingScanMembershipTest, MetadataCompactionCancellationQuotaAndPhysicalSourceRefusalKeepTheImagePrivate) {
    insert(1, 0, {7}); static_cast<void>(makeLegacyRowSource());
    const std::filesystem::path target(path.string() + ".scan");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    auto maintenance = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989);
        static_cast<void>(builder->constructFields()); const auto before = builder->buildingRoot();
        ASSERT_FALSE(before.usesPageDirectoryBootstrap());
        auto targetIo = ScanBuildingImageBuilderTestAccess::io(*builder); targetIo->resetStats();
        const auto sourceReads = builder->sourceStats().read_calls;
        struct Cancel final : CancellationCheckpoint { void check() const override { throw std::runtime_error("cancel metadata move"); } } cancel;
        EXPECT_THROW(static_cast<void>(builder->compactMetadataPoolOnce(maintenance, &cancel)), std::runtime_error);
        auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(8U);
        EXPECT_THROW(static_cast<void>(builder->compactMetadataPoolOnce(tiny)), graph::query::execution::MaterializedByteLimitExceeded);
        EXPECT_EQ(targetIo->stats().read_calls, 0U); EXPECT_EQ(targetIo->stats().write_calls, 0U);
        EXPECT_EQ(builder->sourceStats().read_calls, sourceReads); EXPECT_EQ(tiny->used(), 0U);
        const auto bytes = builder->targetFileBytes();
        EXPECT_FALSE(builder->compactMetadataPoolOnce(maintenance));
        EXPECT_EQ(builder->buildingRoot(), before); EXPECT_EQ(builder->targetFileBytes(), bytes);
        EXPECT_NE(before.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
        EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo,
            ScanBuildingImageBuilderTestAccess::metadata(*builder), maintenance).scan()));
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(maintenance->used(), 0U);
    std::error_code ignored; std::filesystem::remove(target, ignored);
}

TEST_F(BuildingScanMembershipTest, SparseMetadataCompactionABFaultsPreserveOldAndRecoveredRootIntegrity) {
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,
            BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        initialize(true, true);
        MetadataSnapshotPin oldest; std::vector<MetadataSnapshotPin> history;
        ASSERT_NO_FATAL_FAILURE(seedPinnedNodeVersions(24U, oldest, history));
        oldest.release(); history.clear(); static_cast<void>(makeLegacyRowSource());
        const std::filesystem::path target(path.string() + ".scan");
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        auto maintenance = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
        {
            auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
                UINT64_C(64) * 1024U * 1024U, 989);
            static_cast<void>(builder->constructFields(nullptr, maintenance));
            bool interrupted = false;
            for (size_t step = 0; step < 128U; ++step) {
                const auto released = builder->releaseRetiredPoolOnce(maintenance);
                ASSERT_FALSE(released.blocked);
                if (released.released) continue;
                const auto before = builder->buildingRoot();
                if (builder->reclaimMetadataOnce(maintenance).superblock != before) continue;
                if (builder->compactDataPoolOnce(maintenance)) continue;
                auto &store = ScanBuildingImageBuilderTestAccess::metadata(*builder);
                auto old = store.openSnapshot(); const auto source = old.superblock();
                auto targetIo = ScanBuildingImageBuilderTestAccess::io(*builder);
                MetadataPageFile oldPages(targetIo, source);
                const auto oldDescriptor = oldPages.readHybridPropertyDescriptor(source.hybrid_property_descriptor_root, source);
                MetadataPublishOptions options; options.bootstrap_failpoint = failpoint;
                try { EXPECT_FALSE(builder->compactMetadataPoolOnce(maintenance, nullptr, options)); }
                catch (const BootstrapPublishInterrupted &) { interrupted = true; }
                EXPECT_EQ(oldPages.readHybridPropertyDescriptor(source.hybrid_property_descriptor_root, source), oldDescriptor);
                EntityPageStore entities(targetIo, store, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
                EXPECT_EQ(entities.readNode(old, 1).inline_label_count, 0U); old.release();
                auto reopened = RelocatableMetadataStore::openReadOnly(targetIo);
                const auto recovered = reopened->openSnapshot().superblock();
                if (failpoint == BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE) EXPECT_EQ(recovered, source);
                else EXPECT_EQ(recovered.generation.value(), source.generation.value() + 1U);
                EXPECT_NE(recovered.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
                EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo, *reopened, maintenance).scan()));
                break;
            }
            EXPECT_TRUE(interrupted); EXPECT_EQ(builder->sourceStats().write_calls, 0U);
        }
        EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(maintenance->used(), 0U);
        std::error_code ignored; std::filesystem::remove(target, ignored);
    }
}

TEST_F(BuildingScanMembershipTest, ConsecutiveMembershipsShareACowPageAndPreserveOldSlotRoutes) {
    insert(1, 0, {7}); insert(65, 1, {11});
    static_cast<void>(install(request(1)));
    auto old = metadata->openSnapshot(); const auto oldDescriptor = descriptor(*metadata, old);
    const auto original = read(*metadata, old, oldDescriptor, 1);
    static_cast<void>(install(request(2)));
    auto current = metadata->openSnapshot(); const auto desc = descriptor(*metadata, current);
    SlottedPagePool pages(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    BTreePageStore trees(pages, *metadata);
    ScanMembershipDirectoryMap directory(hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_SCAN),
        trees, *metadata, ScanMembershipDirectoryCodec::bind(desc, current));
    const auto first = HybridPropertyDirectoryCodec::decodeLocator(*directory.find(current, ScanMembershipDirectoryCodec::key(1)));
    const auto second = HybridPropertyDirectoryCodec::decodeLocator(*directory.find(current, ScanMembershipDirectoryCodec::key(2)));
    EXPECT_EQ(first.page_id, second.page_id); EXPECT_NE(first.slot_id, second.slot_id);
    EXPECT_EQ(read(*metadata, old, oldDescriptor, 1)->live, original->live);
    EXPECT_FALSE(read(*metadata, old, oldDescriptor, 2));
    EXPECT_EQ(read(*metadata, current, desc, 1)->live, original->live);
    EXPECT_EQ(read(*metadata, current, desc, 2)->select(std::array<uint64_t, 1>{11}), 1U);
    EXPECT_NO_THROW(static_cast<void>(verify(current)));
}

TEST_F(BuildingScanMembershipTest, CompletionRejectsUnconstructedAndUnsavedImages) {
    insert(1, 0, {7}); static_cast<void>(makeLegacyRowSource());
    const std::filesystem::path target(path.string() + ".scan");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    auto maintenance = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989);
        EXPECT_THROW(static_cast<void>(builder->complete(maintenance)), std::logic_error);
        static_cast<void>(builder->constructFields());
        const auto before = builder->buildingRoot();
        auto targetIo = ScanBuildingImageBuilderTestAccess::io(*builder); targetIo->resetStats();
        EXPECT_THROW(static_cast<void>(builder->complete(maintenance)), FormatException);
        EXPECT_EQ(targetIo->stats().write_calls, 0U);
        EXPECT_LT(targetIo->stats().read_calls, 32U); // Reject before traversing all graph pages.
        EXPECT_EQ(builder->buildingRoot(), before);
        EXPECT_NE(before.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING, 0U);
        EXPECT_EQ(builder->sourceStats().write_calls, 0U);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(maintenance->used(), 0U);
    std::error_code ignored; std::filesystem::remove(target, ignored);
}


TEST_F(BuildingScanMembershipTest, CompletionCancelsBeforeIoAndRejectsChecksummedMembershipCorruption) {
    insert(1, 0, {7}); static_cast<void>(makeLegacyRowSource());
    std::filesystem::resize_file(path, std::filesystem::file_size(path) + 32U * RSS_SEGMENT_SIZE);
    const std::filesystem::path target(path.string() + ".scan");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    auto maintenance = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989);
        static_cast<void>(builder->constructFields());
        for (size_t step = 0; step < 64U; ++step) {
            const auto released = builder->releaseRetiredPoolOnce(maintenance);
            ASSERT_FALSE(released.blocked);
            if (released.released) continue;
            const auto before = builder->buildingRoot();
            if (builder->reclaimMetadataOnce(maintenance).superblock != before) continue;
            if (builder->compactDataPoolOnce(maintenance)) continue;
            if (builder->compactMetadataPoolOnce(maintenance)) continue;
            static_cast<void>(builder->truncateReleasedTailOnce(maintenance)); break;
        }
        ASSERT_LE(builder->targetFileBytes() * 10U, builder->sourceFileBytes() * 9U);
        auto targetIo = ScanBuildingImageBuilderTestAccess::io(*builder);
        struct Cancel final : CancellationCheckpoint {
            void check() const override { throw std::runtime_error("cancel completion"); }
        } cancel;
        targetIo->resetStats();
        EXPECT_THROW(static_cast<void>(builder->complete(maintenance, &cancel)), std::runtime_error);
        EXPECT_EQ(targetIo->stats().read_calls, 0U); EXPECT_EQ(targetIo->stats().write_calls, 0U);
        auto &store = ScanBuildingImageBuilderTestAccess::metadata(*builder);
        auto pin = store.openSnapshot(); const auto before = pin.superblock();
        MetadataPageFile metaPages(targetIo, before);
        const auto desc = metaPages.readHybridPropertyDescriptor(before.hybrid_property_descriptor_root, before).descriptor;
        SlottedPagePool source(targetIo, store, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore trees(source, store);
        const auto binding = ScanMembershipDirectoryCodec::bind(desc, pin);
        ScanMembershipDirectoryMap directory(binding.root.index_id, trees, store, binding);
        const auto slot = HybridPropertyDirectoryCodec::decodeLocator(*directory.find(pin, ScanMembershipDirectoryCodec::key(1)));
        const auto location = store.findPageLocation(pin, slot.page_id);
        const auto pool = store.findCatalog(pin, location.segment_id);
        const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(before.data_region_offset, pool.physical_slot) +
            uint64_t(location.page_index) * RSS_PAGE_SIZE;
        std::array<std::byte, RSS_PAGE_SIZE> frame{};
        ASSERT_EQ(targetIo->readAt(offset, frame.data(), frame.size()), frame.size());
        auto image = HybridPropertyPageImage::open(std::span(frame).subspan(RSS_PAGE_OWNER_HEADER_BYTES),
            slot.page_id, location.generation, before.generation, memory);
        auto record = image.readScanMembership(slot, before.database_id, 1, desc.semantic_generation);
        record.labels[0].token = 11; // Legal wire data and CRC, false primary-label proof.
        image.replaceScanMembership(slot, record, desc.semantic_generation);
        const auto bytes = image.serialize();
        std::copy(bytes.begin(), bytes.end(), frame.begin() + RSS_PAGE_OWNER_HEADER_BYTES);
        targetIo->writeAt(offset, frame.data(), frame.size()); targetIo->sync(); pin.release();
        targetIo->resetStats();
        EXPECT_THROW(static_cast<void>(builder->complete(maintenance)), FormatException);
        EXPECT_EQ(targetIo->stats().write_calls, 0U); EXPECT_EQ(builder->buildingRoot(), before);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(maintenance->used(), 0U);
    std::error_code ignored; std::filesystem::remove(target, ignored);
}


TEST_F(BuildingScanMembershipTest, SourceChangeDuringPreparedCompletionDiscardsThePrivateImage) {
    insert(1, 0, {7}); static_cast<void>(makeLegacyRowSource());
    std::filesystem::resize_file(path, std::filesystem::file_size(path) + 32U * RSS_SEGMENT_SIZE);
    const std::filesystem::path target(path.string() + ".scan");
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    auto maintenance = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
            UINT64_C(64) * 1024U * 1024U, 989);
        static_cast<void>(builder->constructFields());
        for (size_t step = 0; step < 64U; ++step) {
            const auto released = builder->releaseRetiredPoolOnce(maintenance);
            ASSERT_FALSE(released.blocked);
            if (released.released) continue;
            const auto before = builder->buildingRoot();
            if (builder->reclaimMetadataOnce(maintenance).superblock != before) continue;
            if (builder->compactDataPoolOnce(maintenance)) continue;
            if (builder->compactMetadataPoolOnce(maintenance)) continue;
            static_cast<void>(builder->truncateReleasedTailOnce(maintenance)); break;
        }
        ASSERT_LE(builder->targetFileBytes() * 10U, builder->sourceFileBytes() * 9U);
        auto targetIo = ScanBuildingImageBuilderTestAccess::io(*builder); targetIo->resetStats();
        struct ChangeSource final : CancellationCheckpoint {
            std::filesystem::path source;
            bool prepared = false;
            mutable bool changed = false;
            void check() const override {
                if (!changed && prepared) {
                    std::filesystem::resize_file(source, std::filesystem::file_size(source) + RSS_SEGMENT_SIZE);
                    changed = true;
                }
            }
        } change;
        change.source = path;
        graph::debug::PerfTrace::ScopedObserver observer(&change,
            [](void *owner, std::string_view key, int64_t value, bool duration) noexcept {
                if (!duration && value && key == "native_graph.scan_build.prepared_completion")
                    static_cast<ChangeSource *>(owner)->prepared = true;
            });
        std::string refusal;
        try { static_cast<void>(builder->complete(maintenance, &change)); FAIL() << "completion unexpectedly published"; }
        catch (const FormatException &error) { refusal = error.what(); }
        SCOPED_TRACE(refusal);
        EXPECT_TRUE(change.changed); EXPECT_FALSE(std::filesystem::exists(target));
        EXPECT_THROW(static_cast<void>(builder->buildingRoot()), std::logic_error);
    }
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(maintenance->used(), 0U);
}

TEST_F(BuildingScanMembershipTest, CompletionPublishesAtomicallyAndReadyOutlivesItsSource) {
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_NONE,
            BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,
            BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        initialize(true); insert(1, 0, {7}); static_cast<void>(makeLegacyRowSource());
        std::filesystem::resize_file(path, std::filesystem::file_size(path) + 32U * RSS_SEGMENT_SIZE);
        const std::filesystem::path target(path.string() + ".scan");
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        auto maintenance = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
        bool ready = false;
        {
            auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, memory,
                UINT64_C(64) * 1024U * 1024U, 989);
            static_cast<void>(builder->constructFields());
            for (size_t step = 0; step < 64U; ++step) {
                const auto released = builder->releaseRetiredPoolOnce(maintenance);
                ASSERT_FALSE(released.blocked);
                if (released.released) continue;
                const auto before = builder->buildingRoot();
                if (builder->reclaimMetadataOnce(maintenance).superblock != before) continue;
                if (builder->compactDataPoolOnce(maintenance)) continue;
                break;
            }
            static_cast<void>(builder->truncateReleasedTailOnce(maintenance));
            const auto before = builder->buildingRoot();
            MetadataPublishOptions options; options.bootstrap_failpoint = failpoint;
            if (failpoint == BootstrapPublishFailpoint::BPF_NONE)
                EXPECT_NO_THROW(static_cast<void>(builder->complete(maintenance, nullptr, options)));
            else EXPECT_THROW(static_cast<void>(builder->complete(maintenance, nullptr, options)), BootstrapPublishInterrupted);
            const auto recovered = builder->buildingRoot();
            ready = !(recovered.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING);
            EXPECT_EQ(ready, failpoint != BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE);
            EXPECT_EQ(recovered.database_id, before.database_id);
            EXPECT_EQ(recovered.hybrid_property_descriptor_root, before.hybrid_property_descriptor_root);
            EXPECT_LE(builder->targetFileBytes() * 10U, builder->sourceFileBytes() * 9U);
            auto targetIo = ScanBuildingImageBuilderTestAccess::io(*builder);
            auto reopened = RelocatableMetadataStore::openReadOnly(targetIo);
            EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo, *reopened, maintenance).scan()));
            if (ready) {
                EXPECT_THROW(static_cast<void>(builder->complete(maintenance)), std::logic_error);
                // Once READY is durable, changing the original no longer revokes it.
                std::filesystem::resize_file(path, std::filesystem::file_size(path) + RSS_SEGMENT_SIZE);
            }
        }
        EXPECT_TRUE(std::filesystem::exists(target));
        EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(maintenance->used(), 0U);
        std::error_code ignored; std::filesystem::remove(target, ignored);
    }
}

#else
TEST(BuildingScanMembershipTest, DefaultFormatCannotInstallOrPublishScanMembership) {
    const auto path = std::filesystem::temp_directory_path() /
        ("zyx-building-refusal-" + boost::uuids::to_string(boost::uuids::random_generator()()));
    {
        auto file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file->is_open());
        auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        Superblock root; root.database_id = DatabaseId(71); root.generation = Generation(1);
        root.checkpoint_lsn = LogSequenceNumber(1);
        auto metadata = RelocatableMetadataStore::initializeNew(io, root);
        const auto before = metadata->openSnapshot().superblock();
        auto request = BuildingScanMembershipTestAccess::create();
        request.expected_database_id = before.database_id; request.expected_generation = before.generation;
        request.expected_descriptor = before.hybrid_property_descriptor_root; request.group_id = 1;
        request.first_slot = PhysicalSlotId(70); request.last_slot = PhysicalSlotId(127); request.field_pool_owner_id = 989;
        request.memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        io->resetStats();
        try {
            static_cast<void>(metadata->installBuildingScanMembershipGroup(request, LogSequenceNumber(2)));
            FAIL() << "default format admitted scan installation";
        } catch (const FormatException &error) { EXPECT_EQ(error.error(), FormatError::FERR_UNSUPPORTED_VERSION); }
        EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::populatePrimaryMembership(io, *metadata,
            request.memory, request.first_slot, request.last_slot, request.field_pool_owner_id)), FormatException);
        const std::filesystem::path refused(path.string() + ".scan");
        EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, refused,
            request.memory, UINT64_C(64) * 1024U * 1024U, 989)), FormatException);
        EXPECT_FALSE(std::filesystem::exists(refused));
        auto maintenance = BuildingScanMembershipTestAccess::maintenance();
        maintenance.memory = request.memory;
        EXPECT_THROW(static_cast<void>(metadata->reclaimOneBuildingScanMetadataPool(maintenance, LogSequenceNumber(2))), FormatException);
        EXPECT_THROW(static_cast<void>(ScanBuildingImageBuilder::constructFieldsForBuilding(io, *metadata,
            request.memory, request.first_slot, request.last_slot, request.field_pool_owner_id)), FormatException);
        EXPECT_EQ(io->stats().read_calls, 0U); EXPECT_EQ(io->stats().write_calls, 0U);
        EXPECT_EQ(metadata->openSnapshot().superblock(), before);
    }
    std::error_code ignored; std::filesystem::remove(path, ignored);
    EXPECT_FALSE(RSS_SCAN_PATH_FORMAT_ENABLED);
    EXPECT_THROW(static_cast<void>(ScanMembershipDirectoryCodec::bind({}, {})), FormatException);
}
#endif
