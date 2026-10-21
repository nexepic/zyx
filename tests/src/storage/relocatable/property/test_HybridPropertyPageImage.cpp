#include <gtest/gtest.h>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectory.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::MaterializedByteLimitExceeded;
using graph::query::execution::MaterializedByteReservation;
namespace {
HybridPropertyGroupRecord group() {
    return {EntityPropertyOwnerKind::EPOK_NODE, HybridPropertyGroupMode::HPGM_HYBRID, 1,
        Generation(8), UINT64_MAX, 97, 2};
}
HybridPropertyColumnRecord column(bool boolean = false) {
    HybridPropertyColumnRecord result;
    result.identity = {EntityPropertyOwnerKind::EPOK_NODE, boolean ? HybridPropertyScalarKind::HPSK_BOOL :
        HybridPropertyScalarKind::HPSK_INT64, 1, boolean ? 2U : 1U, Generation(7)};
    result.live = UINT64_MAX; result.presence = (UINT64_C(1) << 48) - 1;
    for (unsigned i = 0; i < 48; ++i) result.words[i] = boolean ? i % 2 : 100 + i;
    return result;
}
auto extension(std::pmr::memory_resource &memory, Generation generation = Generation(7), size_t length = 30) {
    return HybridPropertyExtensionView::encodeInline({EntityPropertyOwnerKind::EPOK_NODE, 1}, 3, generation,
        PropertyValue(std::string(length, 'x')), RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, memory);
}
TEST(HybridPropertyPageImageTest, SharedNativeImagePacksGroupsColumnsAndFieldsWithIndependentPhysicalGeneration) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    {
        auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
        EXPECT_GE(memory->used(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
        const auto g = group(); const auto scalar = column(); const auto boolean = column(true);
        const auto groupSlot = image.appendGroup(g); const auto scalarSlot = image.appendColumn(scalar, g, Generation(9));
        const auto boolSlot = image.appendColumn(boolean, g, Generation(9));
        const auto bytes = extension(*memory);
        const auto fieldSlot = image.appendExtension(bytes, {g.owner_kind, 1}, 3, g, Generation(9));
        auto secondGroup = g; secondGroup.group_id = 2; secondGroup.live = 1;
        secondGroup.memberships = 1; secondGroup.columns = 0;
        const auto secondGroupSlot = image.appendGroup(secondGroup);
        const auto secondBytes = HybridPropertyExtensionView::encodeInline({g.owner_kind, 65}, 3, Generation(7),
            PropertyValue("other group"), RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, *memory);
        const auto secondField = image.appendExtension(secondBytes, {g.owner_kind, 65}, 3, secondGroup, Generation(9));
        EXPECT_EQ(groupSlot.page_id, scalarSlot.page_id); EXPECT_EQ(scalarSlot.page_id, fieldSlot.page_id);
        EXPECT_EQ(secondField.page_id, groupSlot.page_id); EXPECT_EQ(secondGroupSlot.page_id, groupSlot.page_id);
        EXPECT_EQ(image.stats().live_slot_count, 6U); EXPECT_EQ(image.physicalPoolGeneration(), Generation(1));
        EXPECT_EQ(image.readGroup(groupSlot, g.owner_kind, 1, Generation(9)), g);
        EXPECT_EQ(image.readColumn(scalarSlot, scalar.identity, g, Generation(9)), scalar);
        EXPECT_EQ(image.readColumn(boolSlot, boolean.identity, g, Generation(9)), boolean);
        const auto owned = image.readRecord(fieldSlot);
        const auto view = HybridPropertyExtensionView::open(owned, {g.owner_kind, 1}, 3, g.semantic_generation, Generation(9), Generation(9));
        EXPECT_EQ(view.decodeInline([](const auto &) {}), PropertyValue(std::string(30, 'x')));
        const auto serialized = image.serialize();
        auto reopened = HybridPropertyPageImage::open(serialized, PageId(41), Generation(1), Generation(9), memory);
        EXPECT_EQ(reopened.readColumn(scalarSlot, scalar.identity, g, Generation(9)), scalar);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyPageImageTest, PrivateCopyUpdatesOneFieldWithoutChangingOriginalOrOtherSlots) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    {
        auto original = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
        const auto g = group(); const auto scalar = column(); const auto other = column(true);
        const auto groupSlot = original.appendGroup(g); const auto selected = original.appendColumn(scalar, g, Generation(9));
        const auto untouched = original.appendColumn(other, g, Generation(9)); const auto before = original.serialize();
        auto candidate = original.clone(Generation(10), memory);
        auto newGroup = g; newGroup.semantic_generation = Generation(10);
        auto changed = scalar; changed.identity.semantic_generation = Generation(10); changed.words[11] = 900;
        candidate.replaceColumn(selected, scalar.identity, changed, newGroup, Generation(10));
        candidate.replaceGroup(groupSlot, newGroup, Generation(10));
        EXPECT_EQ(original.serialize(), before);
        EXPECT_EQ(original.readColumn(selected, scalar.identity, g, Generation(9)), scalar);
        EXPECT_EQ(candidate.readColumn(selected, changed.identity, newGroup, Generation(10)), changed);
        EXPECT_EQ(candidate.readColumn(untouched, other.identity, newGroup, Generation(10)), other);
        EXPECT_EQ(candidate.physicalPoolGeneration(), Generation(1));
        const auto bytes = candidate.serialize();
        auto reopened = HybridPropertyPageImage::open(bytes, PageId(41), Generation(1), Generation(10), memory);
        EXPECT_EQ(reopened.readGroup(groupSlot, g.owner_kind, 1, Generation(10)), newGroup);
        EXPECT_EQ(reopened.readColumn(selected, changed.identity, newGroup, Generation(10)), changed);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyPageImageTest, ChangedIdentityDemotionAndFutureVisibilityRejectWithoutMutatingPrivateSource) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
    const auto g = group(); const auto scalar = column(); const auto slot = image.appendColumn(scalar, g, Generation(9));
    const auto before = image.serialize();
    for (unsigned bad = 0; bad < 4; ++bad) {
        auto changed = scalar;
        if (bad == 0) changed.identity.token++;
        if (bad == 1) changed.identity.scalar_kind = HybridPropertyScalarKind::HPSK_DOUBLE;
        if (bad == 2) { changed.presence &= ~UINT64_C(1); changed.words[0] = 0; }
        if (bad == 3) changed.identity.semantic_generation = Generation(10);
        EXPECT_THROW(image.replaceColumn(slot, scalar.identity, changed, g, Generation(9)), FormatException) << bad;
        EXPECT_EQ(image.serialize(), before);
    }
    auto wrongPrevious = scalar.identity; wrongPrevious.semantic_generation = Generation(6);
    EXPECT_THROW(image.replaceColumn(slot, wrongPrevious, scalar, g, Generation(9)), FormatException);
    auto future = g; future.semantic_generation = Generation(10);
    EXPECT_THROW(image.appendGroup(future), FormatException);
    EXPECT_THROW(image.clone(Generation(8), memory), FormatException);
    EXPECT_EQ(image.serialize(), before);
}
TEST(HybridPropertyPageImageTest, EraseReuseAndCompactionKeepOtherStableReferencesAndRefuseStaleGeneration) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    auto old = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
    const auto g = group(); const auto scalar = column(); const auto first = old.appendColumn(scalar, g, Generation(9));
    const auto bytes = extension(*memory); const auto retained = old.appendExtension(bytes, {g.owner_kind, 1}, 3, g, Generation(9));
    auto candidate = old.clone(Generation(10), memory); candidate.erase(first);
    EXPECT_THROW(candidate.readRecord(first), FormatException);
    const auto reused = candidate.appendColumn(scalar, g, Generation(9));
    EXPECT_EQ(reused.page_id, first.page_id); EXPECT_EQ(reused.slot_id, first.slot_id); EXPECT_GT(reused.generation, first.generation);
    candidate.compact(); EXPECT_EQ(candidate.readRecord(retained), old.readRecord(retained));
    EXPECT_EQ(old.readColumn(first, scalar.identity, g, Generation(9)), scalar);
    EXPECT_THROW(candidate.readColumn(first, scalar.identity, g, Generation(9)), FormatException);
}
TEST(HybridPropertyPageImageTest, ExtensionResizeChecksLiveOwnerIdentityAndKeepsSameSlot) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
    const auto g = group(); const auto bytes = extension(*memory);
    const auto slot = image.appendExtension(bytes, {g.owner_kind, 1}, 3, g, Generation(9));
    auto nextGroup = g; nextGroup.semantic_generation = Generation(9); const auto larger = extension(*memory, Generation(9), 1000);
    image.replaceExtension(slot, larger, {g.owner_kind, 1}, 3, nextGroup, Generation(9));
    EXPECT_EQ(image.readRecord(slot).size(), larger.size()); const auto before = image.serialize();
    auto dead = nextGroup; dead.live &= ~UINT64_C(1);
    EXPECT_THROW(image.replaceExtension(slot, larger, {g.owner_kind, 1}, 3, dead, Generation(9)), FormatException);
    EXPECT_THROW(image.replaceExtension(slot, larger, {g.owner_kind, 2}, 3, nextGroup, Generation(9)), FormatException);
    EXPECT_THROW(image.replaceExtension(slot, bytes, {g.owner_kind, 1}, 3, nextGroup, Generation(9)), FormatException);
    EXPECT_EQ(image.serialize(), before);
}
TEST(HybridPropertyPageImageTest, OwnedBufferQuotaRefusalRefundsAndRetryPreservesSource) {
    auto tiny = std::make_shared<MaterializedByteBudget>(4031);
    EXPECT_THROW(HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), tiny), MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U);
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    {
        auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
        const auto g = group(); const auto scalar = column(); const auto slot = image.appendColumn(scalar, g, Generation(9));
        const auto before = image.serialize(); const auto baseline = memory->used();
        {
            MaterializedByteReservation hold(memory.get()); hold.set(memory->available());
            EXPECT_THROW(image.readRecord(slot), MaterializedByteLimitExceeded);
            EXPECT_THROW(image.serialize(), MaterializedByteLimitExceeded);
        }
        EXPECT_EQ(memory->used(), baseline); EXPECT_EQ(image.serialize(), before);
        EXPECT_EQ(image.readColumn(slot, scalar.identity, g, Generation(9)), scalar);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyPageImageTest, PageIdentityAndChecksumsRejectAndEscapedCopiesRetainTheirAccount) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    std::optional<graph::utils::OwnedVector<std::byte>> escaped;
    std::weak_ptr<MaterializedByteBudget> weak = memory;
    {
        auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
        image.appendGroup(group()); const auto bytes = image.serialize(); const auto baseline = memory->used();
        EXPECT_THROW(HybridPropertyPageImage::open(bytes, PageId(42), Generation(1), Generation(9), memory), FormatException);
        EXPECT_THROW(HybridPropertyPageImage::open(bytes, PageId(41), Generation(2), Generation(9), memory), FormatException);
        auto damaged = bytes; damaged.back() ^= std::byte{1};
        const auto withCopy = memory->used();
        EXPECT_THROW(HybridPropertyPageImage::open(damaged, PageId(41), Generation(1), Generation(9), memory), FormatException);
        EXPECT_EQ(memory->used(), withCopy); EXPECT_GE(withCopy, baseline);
        escaped.emplace(image.serialize());
    }
    EXPECT_GE(memory->used(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
    memory.reset(); EXPECT_FALSE(weak.expired()); escaped.reset(); EXPECT_TRUE(weak.expired());
}
TEST(HybridPropertyPageImageTest, FullPageRefusesExtraRecordWithoutChangingOtherRecords) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
    auto g = group(); g.columns = 7; g.memberships = 7 * 48;
    const auto scalar = column(); std::vector<SlottedSlotReference> slots;
    for (unsigned i = 0; i < 6; ++i) {
        auto field = scalar; field.identity.token = i + 1;
        slots.push_back(image.appendColumn(field, g, Generation(9)));
    }
    const auto before = image.serialize();
    auto extra = scalar; extra.identity.token = 7;
    EXPECT_THROW(image.appendColumn(extra, g, Generation(9)), FormatException);
    EXPECT_EQ(image.serialize(), before);
    for (size_t i = 0; i < slots.size(); ++i) {
        auto expected = scalar; expected.identity.token = uint32_t(i + 1);
        EXPECT_EQ(image.readColumn(slots[i], expected.identity, g, Generation(9)), expected);
    }
}
TEST(HybridPropertyPageImageTest, SlottedOwnedCopiesUseDestinationAccountAndRefuseMissingOwnerWithoutChangingSource) {
    auto page = SlottedPage::create(PageId(41), Generation(1));
    const std::array<std::byte, 4> payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const auto slot = page.insert(payload); const auto before = page.serialize();
    EXPECT_THROW(page.readOwned(slot, {}), std::invalid_argument);
    EXPECT_THROW(page.serializeOwned({}), std::invalid_argument);
    auto tiny = std::make_shared<MaterializedByteBudget>(1);
    EXPECT_THROW(page.readOwned(slot, tiny), MaterializedByteLimitExceeded);
    EXPECT_THROW(page.serializeOwned(tiny), MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U); EXPECT_EQ(page.serialize(), before);
    auto target = std::make_shared<MaterializedByteBudget>(65536);
    {
        const auto copy = page.readOwned(slot, target);
        EXPECT_EQ(copy.size(), payload.size()); EXPECT_GE(target->used(), payload.size());
        EXPECT_TRUE(std::equal(copy.begin(), copy.end(), payload.begin(), payload.end()));
        const auto serialized = page.serializeOwned(target);
        EXPECT_EQ(std::span<const std::byte>(serialized).size(), before.size());
        EXPECT_TRUE(std::equal(serialized.begin(), serialized.end(), before.begin(), before.end()));
        EXPECT_GE(target->used(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + payload.size());
    }
    EXPECT_EQ(target->used(), 0U); EXPECT_EQ(page.serialize(), before);
}
TEST(HybridPropertyPageImageTest, FixedExtensionDirectoryResolvesActualSharedSlotWithoutPayloadBearingSeparators) {
    using Codec = HybridPropertyDirectoryCodec;
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
    const auto g = group(); const auto groupSlot = image.appendGroup(g); const auto bytes = extension(*memory);
    const auto fieldSlot = image.appendExtension(bytes, {g.owner_kind, 1}, 3, g, Generation(9));
    const auto key = Codec::ownerKey({g.owner_kind, 1}, 3);
    const auto encoded = HybridPropertyFieldDirectoryMap::encode(key, Codec::locatorValue(fieldSlot));
    EXPECT_EQ(encoded.size(), 28U); EXPECT_LT(encoded.size(), bytes.size());
    BTreePageEntry record{{encoded.begin(), encoded.end()}, 0};
    const auto directory = HybridPropertyFieldDirectoryMap::decode(record);
    const auto identity = Codec::decodeOwnerKey(directory.key, g.owner_kind);
    const auto selected = Codec::decodeLocator(directory.value); EXPECT_EQ(selected, fieldSlot);
    const auto body = image.readRecord(selected);
    const auto view = HybridPropertyExtensionView::open(body, identity.owner, identity.token,
        g.semantic_generation, Generation(9), Generation(9));
    EXPECT_EQ(view.decodeInline([](const auto &) {}), PropertyValue(std::string(30, 'x')));
    // The CRC-valid page and valid slot locator do not authorize the wrong
    // record role: a column reader must reject a group header at that slot.
    const auto wrong = Codec::decodeLocator(Codec::locatorValue(groupSlot));
    EXPECT_THROW(image.readColumn(wrong, column().identity, g, Generation(9)), FormatException);
    auto foreign = selected; foreign.page_id = PageId(42);
    EXPECT_THROW(image.readRecord(foreign), FormatException);
}
TEST(HybridPropertyPageImageTest, NativeDirectoryKeysPreserveOrderedPrefixesAndCanonicalLittleEndianSlotIdentity) {
    using Codec = HybridPropertyDirectoryCodec;
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_EDGE, UINT64_C(0x0102030405060708)};
    const auto encoded = Codec::ownerKey(owner, 0x090a0b0c);
    for (size_t i = 0; i < encoded.size(); ++i) EXPECT_EQ(encoded[i], std::byte(i + 1));
    EXPECT_EQ(Codec::decodeOwnerKey(encoded, owner.owner_kind), (Codec::OwnerField{owner, 0x090a0b0c}));
    EXPECT_LT(Codec::ownerKey({owner.owner_kind, 255}, UINT32_MAX), Codec::ownerKey({owner.owner_kind, 256}, 1));
    EXPECT_LT(Codec::columnKey(256, 255), Codec::columnKey(256, 256));
    EXPECT_EQ(Codec::decodeColumnKey(Codec::columnKey(37, 19)), (Codec::GroupField{37, 19}));
    EXPECT_EQ(Codec::decodeGroupKey(Codec::groupKey(RSS_HYBRID_MAX_GROUP_ID)), RSS_HYBRID_MAX_GROUP_ID);
    const SlottedSlotReference slot{PageId(UINT64_C(0x1122334455667788)), 0x1234, 0x56789abc};
    const auto locator = Codec::locatorValue(slot);
    EXPECT_EQ(locator[0], std::byte{0x88}); EXPECT_EQ(locator[7], std::byte{0x11});
    EXPECT_EQ(locator[8], std::byte{0x34}); EXPECT_EQ(locator[9], std::byte{0x12});
    EXPECT_EQ(locator[12], std::byte{0xbc}); EXPECT_EQ(Codec::decodeLocator(locator), slot);
    for (auto source : {HybridPropertySource::HPS_COLUMN, HybridPropertySource::HPS_EXTENSION})
        EXPECT_EQ(Codec::decodeSource(Codec::sourceValue(source)), source);
}
TEST(HybridPropertyPageImageTest, NativeDirectoriesRejectReservedBitsNoSlotAndOutOfDomainOwnership) {
    using Codec = HybridPropertyDirectoryCodec;
    const SlottedSlotReference slot{PageId(41), 0, 1}; const auto valid = Codec::locatorValue(slot);
    for (size_t offset : {size_t(10), size_t(11)}) {
        auto bad = valid; bad[offset] = std::byte{1}; EXPECT_THROW(Codec::decodeLocator(bad), FormatException);
    }
    for (auto bad : {SlottedSlotReference{PageId{}, 0, 1}, SlottedSlotReference{PageId(41), RSS_SLOTTED_PAGE_NO_SLOT, 1},
        SlottedSlotReference{PageId(41), 0, 0}}) EXPECT_THROW(Codec::locatorValue(bad), FormatException);
    auto badLocator = valid; LittleEndianCodec::writeU16(badLocator, 8, RSS_SLOTTED_PAGE_NO_SLOT);
    EXPECT_THROW(Codec::decodeLocator(badLocator), FormatException);
    auto source = Codec::sourceValue(HybridPropertySource::HPS_EXTENSION);
    for (size_t offset : {size_t(1), size_t(2), size_t(3)}) {
        auto bad = source; bad[offset] = std::byte{1}; EXPECT_THROW(Codec::decodeSource(bad), FormatException);
    }
    source[0] = std::byte{3}; EXPECT_THROW(Codec::decodeSource(source), FormatException);
    EXPECT_THROW(Codec::ownerKey({EntityPropertyOwnerKind::EPOK_NODE, UINT64_C(1) << 63}, 1), FormatException);
    EXPECT_THROW(Codec::ownerKey({EntityPropertyOwnerKind::EPOK_NODE, 1}, 0), FormatException);
    EXPECT_THROW(Codec::groupKey(RSS_HYBRID_MAX_GROUP_ID + 1), FormatException);
    EXPECT_THROW(Codec::columnKey(1, 0), FormatException);
    HybridPropertyOwnerMap::Key zero{}; EXPECT_THROW(Codec::decodeOwnerKey(zero, EntityPropertyOwnerKind::EPOK_NODE), FormatException);
}
TEST(HybridPropertyPageImageTest, CompactMembershipSharesAFrameAndSourceCopyRetainsItsOldOwnerBits) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    {
        auto source = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
        auto g = group(); const std::array columns{column(), column(true)};
        const auto matrix = HybridPropertyColumnMembership::fromColumns(g, columns);
        g.column_membership = source.appendColumnMembership(matrix, g, Generation(9));
        g.mode = HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP;
        const auto header = source.appendGroup(g); const auto before = source.serialize();
        auto candidate = source.clone(Generation(10), memory);
        auto nextGroup = g; nextGroup.semantic_generation = Generation(10); --nextGroup.memberships;
        auto changed = matrix; changed.semantic_generation = Generation(10); changed.owners[0][0] &= ~UINT64_C(1);
        // Source column1 had48 members, so removing one requires demotion and
        // cannot leave a47-member matrix entry under the existing contract.
        EXPECT_THROW(candidate.replaceColumnMembership(g.column_membership, changed, g, nextGroup, Generation(10)), FormatException);
        changed = matrix; changed.semantic_generation = Generation(10);
        nextGroup = g; nextGroup.semantic_generation = Generation(10);
        candidate.replaceColumnMembership(g.column_membership, changed, g, nextGroup, Generation(10));
        candidate.replaceGroup(header, nextGroup, Generation(10));
        EXPECT_EQ(source.serialize(), before);
        EXPECT_EQ(source.readColumnMembership(g.column_membership, g, Generation(9)), matrix);
        EXPECT_EQ(candidate.readColumnMembership(g.column_membership, nextGroup, Generation(10)), changed);
        auto wrong = g; ++wrong.column_membership.generation;
        EXPECT_THROW(source.readColumnMembership(g.column_membership, wrong, Generation(9)), FormatException);
        const auto encoded = nextGroup.encode();
        EXPECT_EQ(HybridPropertyGroupRecord::decode(encoded, g.owner_kind, g.group_id, Generation(10), Generation(10)), nextGroup);
    }
    EXPECT_EQ(memory->used(), 0U);
}

} // namespace
