#include <gtest/gtest.h>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldView.hpp"
using namespace graph::storage;
namespace {
ScanBlockMembership membership() {
    ScanBlockMembership value;
    value.database_id = DatabaseId(7); value.semantic_generation = Generation(8);
    value.group_id = 2; value.live = 7; value.label_count = 1; value.labels[0] = {9, 3};
    return value;
}
}
TEST(ScanMembershipPageTest, IsolatedPageAdmissionAndBorrowedReadRetainOwnerIdentity) {
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    {
        auto image = HybridPropertyPageImage::create(PageId(41), Generation(2), Generation(9), memory);
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
        const auto slot = image.appendScanMembership(membership());
        const auto bytes = image.serialize();
        const PageOwnerHeader owner{PageId(41), 51, PageOwnerKind::POK_HYBRID_PROPERTY, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(2), LogSequenceNumber(9)};
        const auto view = HybridPropertyFieldView::open(owner, bytes, Generation(9), LogSequenceNumber(9));
        EXPECT_EQ(view.readScanMembership(slot, DatabaseId(7), 2, Generation(9)).live, 7U);
        EXPECT_THROW((void)view.readScanMembership(slot, DatabaseId(8), 2, Generation(9)), FormatException);
        EXPECT_THROW((void)view.readScanMembership(slot, DatabaseId(7), 3, Generation(9)), FormatException);
        EXPECT_THROW((void)view.readScanMembership(slot, DatabaseId(7), 2, Generation(7)), FormatException);
        auto stale = slot; ++stale.generation;
        EXPECT_THROW((void)view.readScanMembership(stale, DatabaseId(7), 2, Generation(9)), FormatException);
#else
        EXPECT_THROW((void)image.appendScanMembership(membership()), FormatException);
        EXPECT_EQ(image.liveSlotCount(), 0U);
#endif
    }
    EXPECT_EQ(memory->used(), 0U);
}
#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
TEST(ScanMembershipPageTest, CopyOnWriteMembershipUpdatePreservesOriginalAndGenerationBounds) {
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536);
    {
        auto original = HybridPropertyPageImage::create(PageId(41), Generation(2), Generation(9), memory);
        const auto slot = original.appendScanMembership(membership());
        const auto before = original.serialize();
        auto updated = original.clone(Generation(10), memory);
        auto replacement = membership(); replacement.semantic_generation = Generation(10);
        replacement.labels[0].members = 6;
        updated.replaceScanMembership(slot, replacement, Generation(9));
        EXPECT_EQ(updated.readScanMembership(slot, DatabaseId(7), 2, Generation(10)).labels[0].members, 6U);
        EXPECT_EQ(original.readScanMembership(slot, DatabaseId(7), 2, Generation(9)).labels[0].members, 3U);
        EXPECT_EQ(original.serialize(), before);
        EXPECT_THROW((void)updated.readScanMembership(slot, DatabaseId(7), 2, Generation(9)), FormatException);
        replacement.semantic_generation = Generation(7);
        EXPECT_THROW(updated.replaceScanMembership(slot, replacement, Generation(10)), FormatException);
        replacement.semantic_generation = Generation(11);
        EXPECT_THROW(updated.replaceScanMembership(slot, replacement, Generation(10)), FormatException);
        auto reopened = HybridPropertyPageImage::open(updated.serialize(), PageId(41), Generation(2), Generation(10), memory);
        EXPECT_EQ(reopened.readScanMembership(slot, DatabaseId(7), 2, Generation(10)).labels[0].members, 6U);
    }
    EXPECT_EQ(memory->used(), 0U);
}
#endif
