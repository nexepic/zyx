#include "BuildingScanMembershipFixture.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"

#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
TEST_F(BuildingScanMembershipTest, PrivateIndexPackingReusesAnOccupiedLowerPoolAcrossEpochsPinsAndABRecovery) {
    for (const auto failpoint : {BootstrapPublishFailpoint::BPF_NONE,
            BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE,
            BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE}) {
        initialize(true);
        BTreePageStore trees(*pages, *metadata);
        const auto page = [](PageId id, Generation generation, LogSequenceNumber lsn, bool root) {
            PageOwnerHeader owner{id, 17, PageOwnerKind::POK_INDEX, 0,
                RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, generation, lsn};
            BTreePageHeader header; header.index_id = 17; header.page_id = id; header.generation = generation;
            if (root) { header.kind = BTreeNodeKind::BPN_INTERNAL; header.level = 1; header.leftmost_child = PageId(101); }
            else if (id == PageId(101)) header.right_sibling = PageId(102);
            else header.left_sibling = PageId(101);
            auto result = BTreePage::create(owner, header);
            if (root) result.insert({{std::byte{2}}, 102});
            else if (id == PageId(101)) result.insert({{std::byte{1}}, 11});
            else result.insert({{std::byte{2}}, 22});
            return result;
        };
        SlottedPagePoolAllocationRequest left{SegmentId(71), SegmentKind::SGK_INDEX_PAGE_POOL,
            17, Generation(1), PageId(101), 17, PageOwnerKind::POK_INDEX};
        ASSERT_TRUE(trees.allocate(left, page(PageId(101), Generation(1), LogSequenceNumber(2), false),
            PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)));
        auto right = left; right.segment_id = SegmentId(72); right.generation = Generation(2); right.page_id = PageId(102);
        ASSERT_TRUE(trees.allocate(right, page(PageId(102), Generation(2), LogSequenceNumber(3), false),
            PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3)));
        const SlottedPagePoolAppendRequest rootPage{SegmentId(71), Generation(1), PageId(103), 17, PageOwnerKind::POK_INDEX};
        static_cast<void>(trees.append(rootPage, page(PageId(103), Generation(1), LogSequenceNumber(4), true), LogSequenceNumber(4)));
        const IndexRootLocation root{17, PageId(103), Generation(1)};
        static_cast<void>(trees.publishRoots(std::span(&root, 1), {}, LogSequenceNumber(5)));
        auto old = metadata->openSnapshot(); const auto before = old.superblock();
        const auto oldLocation = metadata->findPageLocation(old, PageId(102));
        ASSERT_TRUE(metadata->isPhysicalSlotAllocated(old, PhysicalSlotId(40)));
        const auto bytes = io->fileSize();
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        NativeHybridPropertyPoolRelocation move{before.database_id, before.generation, before.hybrid_property_descriptor_root,
            SegmentId(72), PhysicalSlotId(40), PhysicalSlotId(40), memory};
        auto noSpace = move; noSpace.first_slot = PhysicalSlotId(41); noSpace.last_slot = PhysicalSlotId(41);
        io->resetStats();
        EXPECT_EQ(metadata->relocateHybridPropertyIndexPool(noSpace, LogSequenceNumber(6)), before);
        EXPECT_EQ(io->stats().write_calls, 0U); EXPECT_EQ(metadata->openSnapshot().superblock(), before);
        MetadataPublishOptions options; options.bootstrap_failpoint = failpoint;
        if (failpoint == BootstrapPublishFailpoint::BPF_NONE)
            ASSERT_NO_THROW(static_cast<void>(metadata->relocateHybridPropertyIndexPool(move, LogSequenceNumber(6), options)));
        else EXPECT_THROW(static_cast<void>(metadata->relocateHybridPropertyIndexPool(move, LogSequenceNumber(6), options)), BootstrapPublishInterrupted);
        auto reopened = RelocatableMetadataStore::openReadOnly(io);
        SlottedPagePool recoveredPages(io, *reopened, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore recoveredTrees(recoveredPages, *reopened);
        BTreeIndex recovered(17, recoveredTrees, *reopened), original(17, trees, *metadata);
        auto current = reopened->openSnapshot();
        for (auto *index : {&recovered, &original}) {
            const auto &snapshot = index == &recovered ? current : old;
            EXPECT_EQ(index->find(snapshot, std::array{std::byte{1}}), std::optional<uint64_t>(11));
            EXPECT_EQ(index->find(snapshot, std::array{std::byte{2}}), std::optional<uint64_t>(22));
        }
        EXPECT_EQ(metadata->findPageLocation(old, PageId(102)), oldLocation);
        if (failpoint == BootstrapPublishFailpoint::BPF_NONE) {
            EXPECT_EQ(reopened->findPageLocation(current, PageId(102)).segment_id, SegmentId(71));
            EXPECT_EQ(reopened->findPageLocation(current, PageId(102)).generation, Generation(1));
            EXPECT_EQ(reopened->findCatalog(current, SegmentId(72)).state, SegmentState::SGS_RETIRED);
            EXPECT_EQ(io->fileSize(), bytes);
        }
        EXPECT_EQ(memory->used(), 0U);
    }
}
#endif

#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
TEST_F(BuildingScanMembershipTest, DataCompactionSkipsAnOrdinaryMixedOwnerIndexPoolWithoutChangingEitherTree) {
    BTreePageStore trees(*pages, *metadata);
    const auto leaf = [](PageId id, uint64_t index, LogSequenceNumber lsn) {
        const PageOwnerHeader owner{id, index, PageOwnerKind::POK_INDEX, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(1), lsn};
        BTreePageHeader header; header.index_id = index; header.page_id = id; header.generation = Generation(1);
        auto result = BTreePage::create(owner, header); result.insert({{std::byte{1}}, index}); return result;
    };
    const SlottedPagePoolAllocationRequest pool{SegmentId(71), SegmentKind::SGK_INDEX_PAGE_POOL,
        17, Generation(1), PageId(101), 17, PageOwnerKind::POK_INDEX};
    ASSERT_TRUE(trees.allocate(pool, leaf(PageId(101), 17, LogSequenceNumber(2)),
        PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2)));
    const SlottedPagePoolAppendRequest other{SegmentId(71), Generation(1), PageId(102), 18, PageOwnerKind::POK_INDEX};
    static_cast<void>(trees.append(other, leaf(PageId(102), 18, LogSequenceNumber(3)), LogSequenceNumber(3)));
    const std::array<IndexRootLocation, 2> roots{{{17, PageId(101), Generation(1)}, {18, PageId(102), Generation(1)}}};
    static_cast<void>(trees.publishRoots(roots, {}, LogSequenceNumber(4)));
    static_cast<void>(makeLegacyRowSource());
    const std::filesystem::path target(path.string() + ".mixed-index");
    auto source = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    auto work = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * 1024U * 1024U);
    {
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(path, target, source,
            UINT64_C(64) * 1024U * 1024U, 989);
        static_cast<void>(builder->constructFields(nullptr, work));
        auto &store = ScanBuildingImageBuilderTestAccess::metadata(*builder);
        auto targetIo = ScanBuildingImageBuilderTestAccess::io(*builder);
        const auto before = builder->buildingRoot(); targetIo->resetStats();
        EXPECT_FALSE(builder->compactDataPoolOnce(work));
        EXPECT_EQ(builder->buildingRoot(), before); EXPECT_EQ(targetIo->stats().write_calls, 0U);
        SlottedPagePool readPages(targetIo, store, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore readTrees(readPages, store); auto pin = store.openSnapshot();
        for (const auto id : {17U, 18U}) {
            BTreeIndex index(id, readTrees, store);
            EXPECT_EQ(index.find(pin, std::array{std::byte{1}}), std::optional<uint64_t>(id));
        }
        EXPECT_EQ(store.findPageLocation(pin, PageId(101)).segment_id, store.findPageLocation(pin, PageId(102)).segment_id);
        EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(targetIo, store, work).scan()));
    }
    EXPECT_EQ(source->used(), 0U); EXPECT_EQ(work->used(), 0U);
    std::error_code ignored; std::filesystem::remove(target, ignored);
}
#endif
