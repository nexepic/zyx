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
