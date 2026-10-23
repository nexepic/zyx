#include <gtest/gtest.h>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/property/HybridPropertyPageImage.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldView.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldPageStore.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
using graph::query::execution::MaterializedByteLimitExceeded;
using graph::query::execution::MaterializedByteReservation;
namespace {
CatalogEntry pool() {
    return {SegmentId(3), PhysicalSlotId(40), SegmentKind::SGK_PROPERTY_PAGE_POOL, 902,
        Generation(3), SegmentState::SGS_SEALED, LogSequenceNumber(2)};
}
PageOwnerHeader owner() {
    return {PageId(41), 902, PageOwnerKind::POK_HYBRID_PROPERTY, 0,
        RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(3), LogSequenceNumber(9)};
}
HybridPropertyGroupRecord group() {
    return {EntityPropertyOwnerKind::EPOK_NODE, HybridPropertyGroupMode::HPGM_HYBRID,
        1, Generation(8), UINT64_MAX, 49, 1};
}
HybridPropertyColumnRecord column() {
    HybridPropertyColumnRecord result;
    result.identity = {EntityPropertyOwnerKind::EPOK_NODE, HybridPropertyScalarKind::HPSK_INT64, 1, 1, Generation(7)};
    result.live = UINT64_MAX; result.presence = (UINT64_C(1) << 48) - 1;
    for (unsigned i = 0; i < 48; ++i) result.words[i] = 100 + i;
    return result;
}
template<class Operation> void expectError(Operation operation, FormatError expected) {
    try { operation(); FAIL() << "expected typed format refusal"; }
    catch (const FormatException &error) { EXPECT_EQ(error.error(), expected) << error.what(); }
}
TEST(HybridPropertyPageBindingTest, OwnerCodecRecognizesSixteenOnlyInTheIsolatedFormat) {
    auto row = owner(); row.kind = PageOwnerKind::POK_PROPERTY;
    auto encoded = PageOwnerHeaderCodec::encode(row);
    LittleEndianCodec::writeU16(encoded, 24, uint16_t(PageOwnerKind::POK_HYBRID_PROPERTY));
    LittleEndianCodec::writeU32(encoded, 48, 0);
    LittleEndianCodec::writeU32(encoded, 48, graph::utils::calculateCrc(encoded.data(), encoded.size()));
    EXPECT_EQ(SlottedPage::supportsOwnerKind(PageOwnerKind::POK_HYBRID_PROPERTY), RSS_HYBRID_PROPERTY_FORMAT_ENABLED);
    if constexpr (RSS_HYBRID_PROPERTY_FORMAT_ENABLED) {
        const auto decoded = PageOwnerHeaderCodec::decode(encoded);
        EXPECT_EQ(decoded.kind, PageOwnerKind::POK_HYBRID_PROPERTY); EXPECT_EQ(decoded.page_id, PageId(41));
        EXPECT_EQ(PageOwnerHeaderCodec::encode(decoded), encoded);
    } else {
        EXPECT_THROW(PageOwnerHeaderCodec::encode(owner()), FormatException);
        EXPECT_THROW(PageOwnerHeaderCodec::decode(encoded), FormatException);
    }
}
TEST(HybridPropertyPageBindingTest, FeatureGuardAndOwnedEnumerationLeaveSourceUntouchedOnRefusal) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
    image.appendGroup(group()); const auto before = image.serialize();
    if constexpr (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED) {
        expectError([&] { std::move(image).bindPrivateOwner(owner(), pool(), LogSequenceNumber(9)); }, FormatError::FERR_UNSUPPORTED_VERSION);
        EXPECT_EQ(image.serialize(), before);
    }
    auto ordinary = SlottedPage::create(PageId(1), Generation(1)); const std::array value{std::byte{1}};
    const auto first = ordinary.insert(value); const auto second = ordinary.insert(value); ordinary.erase(first);
    auto tiny = std::make_shared<MaterializedByteBudget>(1);
    EXPECT_THROW(ordinary.liveSlotsOwned({}), std::invalid_argument);
    EXPECT_THROW(ordinary.liveSlotsOwned(tiny), MaterializedByteLimitExceeded); EXPECT_EQ(tiny->used(), 0U);
    {
        const auto refs = ordinary.liveSlotsOwned(memory); ASSERT_EQ(refs.size(), 1U); EXPECT_EQ(refs[0], second);
        EXPECT_EQ(ordinary.readOwned(second, memory).size(), 1U);
    }
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST(HybridPropertyPageBindingTest, MixedNodeAndEdgeFieldsBindAsOneTypedFrameAndRetainSemanticVisibility) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    {
        auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
        const auto g = group(); const auto c = column(); image.appendGroup(g);
        const auto columnSlot = image.appendColumn(c, g, Generation(9));
        const auto nodeBytes = HybridPropertyExtensionView::encodeInline({g.owner_kind, 1}, 2, Generation(7),
            PropertyValue("node"), 3952, *memory);
        image.appendExtension(nodeBytes, {g.owner_kind, 1}, 2, g, Generation(9));
        auto edgeGroup = g; edgeGroup.owner_kind = EntityPropertyOwnerKind::EPOK_EDGE;
        edgeGroup.live = 1; edgeGroup.memberships = 1; edgeGroup.columns = 0; image.appendGroup(edgeGroup);
        const auto edgeBytes = HybridPropertyExtensionView::encodeInline({edgeGroup.owner_kind, 1}, 2, Generation(7),
            PropertyValue("edge"), 3952, *memory);
        const auto edgeSlot = image.appendExtension(edgeBytes, {edgeGroup.owner_kind, 1}, 2, edgeGroup, Generation(9));
        auto bound = std::move(image).bindPrivateOwner(owner(), pool(), LogSequenceNumber(9));
        EXPECT_EQ(bound.ownerKind(), PageOwnerKind::POK_HYBRID_PROPERTY);
        EXPECT_EQ(bound.pageGeneration(), Generation(3)); EXPECT_EQ(bound.ownerCommittedLsn(), LogSequenceNumber(9));
        const auto bytes = bound.serializeOwned(memory); const auto view = SlottedPageReadView::open(owner(), bytes);
        EXPECT_EQ(HybridPropertyColumnRecord::decode(view.read(columnSlot), c.identity, g, Generation(9), Generation(9)), c);
        const auto edge = HybridPropertyExtensionView::open(view.read(edgeSlot), {edgeGroup.owner_kind, 1}, 2,
            Generation(8), Generation(9), Generation(9));
        EXPECT_EQ(edge.decodeInline([](const auto &) {}), PropertyValue("edge"));
        EXPECT_THROW(HybridPropertyExtensionView::open(view.read(edgeSlot), {g.owner_kind, 1}, 2,
            Generation(8), Generation(9), Generation(9)), FormatException);
        auto downgrade = owner(); downgrade.kind = PageOwnerKind::POK_PROPERTY;
        EXPECT_THROW(bound.rebindOwner(downgrade), FormatException);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(HybridPropertyPageBindingTest, BorrowedFieldReadsNeedNoAdditionalQuotaAndPreserveExactScalarBits) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
    auto g = group(); auto c = column();
    c.identity.scalar_kind = HybridPropertyScalarKind::HPSK_DOUBLE;
    c.words[0] = UINT64_C(0x8000000000000000); // negative zero
    c.words[1] = UINT64_C(0x7ff8000000000123); // exact NaN payload
    c.nulls = UINT64_C(1) << 2; c.words[2] = 0;
    const auto groupSlot = image.appendGroup(g);
    const auto columnSlot = image.appendColumn(c, g, Generation(9));
    const auto fieldBytes = HybridPropertyExtensionView::encodeInline({g.owner_kind, 1}, 2, Generation(7),
        PropertyValue("field"), 3952, *memory);
    const auto fieldSlot = image.appendExtension(fieldBytes, {g.owner_kind, 1}, 2, g, Generation(9));
    auto bound = std::move(image).bindPrivateOwner(owner(), pool(), LogSequenceNumber(9));
    const auto payload = bound.serializeOwned(memory);
    const auto baseline = memory->used();
    MaterializedByteReservation held(memory.get()); held.set(memory->available());
    const auto view = HybridPropertyFieldView::open(owner(), payload, Generation(9), LogSequenceNumber(9));
    EXPECT_EQ(view.readGroup(groupSlot, g.owner_kind, 1, Generation(9)), g);
    EXPECT_EQ(view.readColumn(columnSlot, g, c.identity.token, Generation(9)), c);
    EXPECT_THROW(view.readColumn(columnSlot, g, c.identity.token + 1, Generation(9)), FormatException);
    auto wrongGroup = g; wrongGroup.owner_kind = EntityPropertyOwnerKind::EPOK_EDGE;
    EXPECT_THROW(view.readColumn(columnSlot, wrongGroup, c.identity.token, Generation(9)), FormatException);

    const auto scalar = [&](uint64_t id) { return view.readScalar(columnSlot, c.identity, g, Generation(9), {g.owner_kind, id}); };
    EXPECT_EQ(scalar(1).word, UINT64_C(0x8000000000000000));
    EXPECT_EQ(scalar(2).word, UINT64_C(0x7ff8000000000123));
    EXPECT_TRUE(scalar(3).present); EXPECT_TRUE(scalar(3).is_null);
    EXPECT_FALSE(scalar(49).present);
    const auto extension = view.readExtension(fieldSlot, {g.owner_kind, 1}, 2, g, Generation(9));
    EXPECT_EQ(extension.token(), 2U);
    EXPECT_THROW(scalar(65), FormatException);
    auto dead = g; dead.live &= ~UINT64_C(1);
    EXPECT_THROW(view.readScalar(columnSlot, c.identity, dead, Generation(9), {g.owner_kind, 1}), FormatException);
    EXPECT_THROW(view.readExtension(fieldSlot, {g.owner_kind, 2}, 2, g, Generation(9)), FormatException);
    EXPECT_THROW(HybridPropertyFieldView::open(owner(), payload, Generation(9), LogSequenceNumber(8)), FormatException);
    auto retired = owner(); retired.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
    const auto oldView = HybridPropertyFieldView::open(retired, payload, Generation(9), LogSequenceNumber(9));
    EXPECT_EQ(oldView.readGroup(groupSlot, g.owner_kind, 1, Generation(9)), g);
    auto wrong = owner(); wrong.kind = PageOwnerKind::POK_PROPERTY;
    EXPECT_THROW(HybridPropertyFieldView::open(wrong, payload, Generation(9), LogSequenceNumber(9)), FormatException);
    held.set(0); EXPECT_EQ(memory->used(), baseline);
}
TEST(HybridPropertyPageBindingTest, BadPoolOwnerGenerationLsnAndRoleRejectBeforeChangingThePrivateImage) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
    image.appendGroup(group()); const auto before = image.serialize();
    for (unsigned bad = 0; bad < 10; ++bad) {
        auto target = owner(); auto destination = pool();
        if (bad == 0) target.page_id = PageId(42);
        if (bad == 1) target.owner_id++;
        if (bad == 2) target.generation = Generation(4);
        if (bad == 3) target.committed_lsn = LogSequenceNumber(10);
        if (bad == 4) target.kind = PageOwnerKind::POK_PROPERTY;
        if (bad == 5) target.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
        if (bad == 6) target.payload_bytes--;
        if (bad == 7) destination.state = SegmentState::SGS_RETIRED;
        if (bad == 8) destination.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
        if (bad == 9) destination.committed_lsn = LogSequenceNumber(10);
        EXPECT_THROW(std::move(image).bindPrivateOwner(target, destination, LogSequenceNumber(9)), FormatException) << bad;
        EXPECT_EQ(image.physicalPoolGeneration(), Generation(1)); EXPECT_EQ(image.serialize(), before);
    }
}
TEST(HybridPropertyPageBindingTest, ValidSlottedCrcDoesNotAuthorizeUnknownOrFutureRecordPayloads) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    for (unsigned bad = 0; bad < 3; ++bad) {
        auto raw = SlottedPage::create(PageId(41), Generation(1));
        if (bad == 0) { const std::array unknown{std::byte{1}}; raw.insert(unknown); }
        if (bad == 1) { auto g = group(); g.semantic_generation = Generation(10); raw.insert(g.encode()); }
        if (bad == 2) {
            auto c = column(); c.identity.semantic_generation = Generation(10);
            std::array<std::byte,576> bytes{}; c.encode(bytes); raw.insert(bytes);
        }
        const auto bytes = raw.serializeOwned(memory);
        auto image = HybridPropertyPageImage::open(bytes, PageId(41), Generation(1), Generation(9), memory);
        const auto before = image.serialize();
        EXPECT_THROW(std::move(image).bindPrivateOwner(owner(), pool(), LogSequenceNumber(9)), FormatException) << bad;
        EXPECT_EQ(image.serialize(), before);
    }
}
TEST(HybridPropertyPageBindingTest, BindingQuotaRefusalRefundsItsEnumerationAndPreservesRetry) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
    image.appendGroup(group()); const auto before = image.serialize(); const auto baseline = memory->used();
    {
        MaterializedByteReservation held(memory.get()); held.set(memory->available());
        EXPECT_THROW(std::move(image).bindPrivateOwner(owner(), pool(), LogSequenceNumber(9)), MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(memory->used(), baseline); EXPECT_EQ(image.serialize(), before);
    auto bound = std::move(image).bindPrivateOwner(owner(), pool(), LogSequenceNumber(9));
    EXPECT_EQ(bound.ownerKind(), PageOwnerKind::POK_HYBRID_PROPERTY);
}
TEST(HybridPropertyPageBindingTest, LargeReferenceBindingRequiresRetainedAdmissionAndRefundsBeforeRetry) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
    LargeValueRef reference; reference.value_id = LargeValueId(11); reference.generation = Generation(7);
    reference.logical_bytes = 8192; reference.content_hash[0] = std::byte{1};
    reference.kind = LargeValueKind::LVK_BYTES; reference.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
    const auto encodedReference = LargeValueRefCodec::encode(reference);
    MaterializedByteReservation media(memory.get()); const auto admit = [&](size_t n) { media.set(n); };
    const auto bytes = HybridPropertyExtensionView::encodeReference({EntityPropertyOwnerKind::EPOK_NODE, 1}, 2,
        Generation(7), encodedReference, 3952, *memory, admit);
    image.appendExtension(bytes, {EntityPropertyOwnerKind::EPOK_NODE, 1}, 2, group(), Generation(9), admit);
    media.set(0); const auto before = image.serialize(); const auto baseline = memory->used();
    EXPECT_THROW(std::move(image).bindPrivateOwner(owner(), pool(), LogSequenceNumber(9)), FormatException);
    EXPECT_EQ(memory->used(), baseline); EXPECT_EQ(image.serialize(), before);
    EXPECT_THROW(std::move(image).bindPrivateOwner(owner(), pool(), LogSequenceNumber(9),
        [](size_t) { throw MaterializedByteLimitExceeded(); }), MaterializedByteLimitExceeded);
    EXPECT_EQ(memory->used(), baseline); EXPECT_EQ(image.serialize(), before);
    auto bound = std::move(image).bindPrivateOwner(owner(), pool(), LogSequenceNumber(9), admit);
    EXPECT_GT(media.bytes(), 0U); media.set(0);
    EXPECT_EQ(bound.ownerKind(), PageOwnerKind::POK_HYBRID_PROPERTY);
}
#endif
class HybridPropertyPublicationGuardTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {};
TEST_F(HybridPropertyPublicationGuardTest, OrdinaryWritersRefuseHybridKindsBeforeReconciliationOrPagePreparation) {
    SharedPagePoolAllocator allocator(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION);
    const auto before = metadata_->openSnapshot().superblock(); const auto bytes = io_->fileSize();
    SharedPagePoolAllocationRequest allocation; allocation.page_owner_kind = PageOwnerKind::POK_HYBRID_PROPERTY;
    SharedPagePoolAppendRequest append; append.page_owner_kind = PageOwnerKind::POK_HYBRID_PROPERTY;
    SharedPagePoolReplacementRequest replace; replace.page_owner_kind = PageOwnerKind::POK_HYBRID_PROPERTY;
    SharedPagePoolEvacuationRequest evacuate; evacuate.page_owner_kind = PageOwnerKind::POK_HYBRID_PROPERTY;
    // Missing pool identities would fail differently if reconciliation ran.
    expectError([&] { allocator.allocateNewPoolPage(allocation, PhysicalSlotId(40), PhysicalSlotId(80), LogSequenceNumber(2)); }, FormatError::FERR_UNSUPPORTED_VERSION);
    expectError([&] { allocator.appendPage(append, LogSequenceNumber(2)); }, FormatError::FERR_UNSUPPORTED_VERSION);
    expectError([&] { allocator.replacePage(replace, LogSequenceNumber(2)); }, FormatError::FERR_UNSUPPORTED_VERSION);
    expectError([&] { allocator.evacuatePage(evacuate, LogSequenceNumber(2)); }, FormatError::FERR_UNSUPPORTED_VERSION);
    EXPECT_EQ(metadata_->openSnapshot().superblock(), before); EXPECT_EQ(io_->fileSize(), bytes);
}
} // namespace

// Physical-reader laboratory fixture only. Install a frame into a real native
// property pool by test IO; no descriptor or graph route is published here.
class HybridPropertyPhysicalReaderTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
    SharedPagePoolAllocation allocation_;
    SlottedSlotReference group_slot_;
    HybridPropertyGroupRecord group_;
    void seedRowPool() {
        PageOwnerHeader row{PageId(41), 902, PageOwnerKind::POK_PROPERTY, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, Generation(99), LogSequenceNumber(2)};
        auto page = SlottedPage::create(row);
        SharedPagePoolAllocationRequest request;
        request.segment_id = SegmentId(31); request.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
        request.pool_owner_id = row.owner_id; request.generation = row.generation;
        request.page_id = row.page_id; request.page_owner_id = row.owner_id; request.page_owner_kind = row.kind;
        request.payload = page.serialize(); request.page_free_bytes = page.stats().contiguous_free_bytes;
        SharedPagePoolAllocator allocator(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        auto result = allocator.allocateNewPoolPage(std::move(request), PhysicalSlotId(40), PhysicalSlotId(80), LogSequenceNumber(2));
        ASSERT_TRUE(result); allocation_ = *result;
    }
    void installFieldFrame() {
        auto memory = std::make_shared<MaterializedByteBudget>(65536);
        auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(2), memory);
        group_ = {EntityPropertyOwnerKind::EPOK_NODE, HybridPropertyGroupMode::HPGM_HYBRID,
            1, Generation(1), 1, 0, 0};
        group_slot_ = image.appendGroup(group_);
        auto header = allocation_.owner; header.kind = PageOwnerKind::POK_HYBRID_PROPERTY;
        auto page = std::move(image).bindPrivateOwner(header, allocation_.pool, LogSequenceNumber(2));
        writeFrame(header, page.serializeOwned(memory));
    }
    void writeFrame(const PageOwnerHeader &owner, std::span<const std::byte> payload) {
        std::array<std::byte, RSS_PAGE_SIZE> bytes{};
        const auto header = PageOwnerHeaderCodec::encode(owner);
        std::copy(header.begin(), header.end(), bytes.begin());
        std::copy(payload.begin(), payload.end(), bytes.begin() + RSS_PAGE_OWNER_HEADER_BYTES);
        const auto sb = metadata_->openSnapshot().superblock();
        const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(sb.data_region_offset, allocation_.pool.physical_slot) +
            uint64_t(allocation_.location.page_index) * RSS_PAGE_SIZE;
        io_->writeAt(BufferPoolPageKey{offset, allocation_.pool.generation.value()}, bytes.data(), bytes.size());
    }
};
TEST_F(HybridPropertyPhysicalReaderTest, FeatureAndRowBoundariesRemainExplicit) {
    seedRowPool();
    HybridPropertyFieldPageStore reader(io_, *metadata_);
    const auto pin = metadata_->openSnapshot();
    if constexpr (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED) {
        expectError([&] { reader.read(pin, PageId(41)); }, FormatError::FERR_UNSUPPORTED_VERSION);
    } else {
        EXPECT_THROW(reader.read(pin, PageId(41)), FormatException);
    }
    SharedPagePoolAllocator ordinary(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    EXPECT_EQ(ordinary.read(pin, PageId(41)).owner.kind, PageOwnerKind::POK_PROPERTY);
}
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
TEST_F(HybridPropertyPhysicalReaderTest, ActualDirectoryAndPoolRouteSupportsOwnedFallbackPinAndMove) {
    seedRowPool(); installFieldFrame();
    HybridPropertyFieldPageStore reader(io_, *metadata_);
    const auto pin = metadata_->openSnapshot();
    EXPECT_LT(pin.superblock().generation, Generation(99));
    {
        auto page = reader.read(pin, PageId(41)); EXPECT_FALSE(page.isPinned());
        auto moved = std::move(page);
        EXPECT_EQ(moved.view().readGroup(group_slot_, group_.owner_kind, 1, Generation(2)), group_);
    }
    io_->enableBufferPool(64U * RSS_PAGE_SIZE);
    {
        auto page = reader.read(pin, PageId(41)); EXPECT_TRUE(page.isPinned());
        auto moved = std::move(page);
        EXPECT_EQ(moved.view().readGroup(group_slot_, group_.owner_kind, 1, Generation(2)), group_);
        auto replacement = reader.read(pin, PageId(41)); replacement = std::move(moved);
        EXPECT_EQ(replacement.view().readGroup(group_slot_, group_.owner_kind, 1, Generation(2)), group_);
    }
    SharedPagePoolAllocator ordinary(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    EXPECT_THROW(ordinary.read(pin, PageId(41)), FormatException);
    EXPECT_THROW(ordinary.readPinned(pin, PageId(41)), FormatException);
    EXPECT_THROW(reader.read(pin, PageId{}), FormatException);
    EXPECT_THROW(reader.read(MetadataSnapshotPin{}, PageId(41)), FormatException);
    EXPECT_THROW(reader.read(pin, PageId(41), BufferPoolReadHint(255)), FormatException);
}
TEST_F(HybridPropertyPhysicalReaderTest, OwnedFallbackRetainsItsAllocationAndRefusalAllowsRetry) {
    seedRowPool(); installFieldFrame();
    HybridPropertyFieldPageStore reader(io_, *metadata_);
    const auto pin = metadata_->openSnapshot();
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    std::optional<HybridPropertyFieldPage> retained;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        {
            MaterializedByteReservation hold(memory.get()); hold.set(memory->available());
            EXPECT_THROW(reader.read(pin, PageId(41)), MaterializedByteLimitExceeded);
        }
        EXPECT_EQ(memory->used(), 0U);
        retained.emplace(reader.read(pin, PageId(41)));
        EXPECT_GE(memory->used(), RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
    }
    EXPECT_EQ(retained->view().readGroup(group_slot_, group_.owner_kind, 1, Generation(2)), group_);
    std::weak_ptr<MaterializedByteBudget> weak = memory;
    memory.reset(); EXPECT_FALSE(weak.expired());
    retained.reset(); EXPECT_TRUE(weak.expired());
}
TEST_F(HybridPropertyPhysicalReaderTest, ValidChecksumsDoNotAuthorizeForeignOwnerPageEpochOrFutureLsn) {
    seedRowPool(); installFieldFrame();
    HybridPropertyFieldPageStore reader(io_, *metadata_);
    const auto pin = metadata_->openSnapshot();
    SharedPagePoolAllocator ordinary(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
    // Keep the authenticated payload while replacing only checksummed owner
    // headers through test IO. A changed checksum is not an authority check.
    const auto sb = pin.superblock();
    const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(sb.data_region_offset, allocation_.pool.physical_slot) +
        uint64_t(allocation_.location.page_index) * RSS_PAGE_SIZE;
    std::array<std::byte, RSS_PAGE_SIZE> bytes{};
    ASSERT_EQ(io_->readAt(offset, bytes.data(), bytes.size()), bytes.size());
    const auto original = PageOwnerHeaderCodec::decode(std::span(bytes).first(RSS_PAGE_OWNER_HEADER_BYTES));
    for (const bool cached : {false, true}) {
        if (cached) io_->enableBufferPool(64U * RSS_PAGE_SIZE);
        for (unsigned bad = 0; bad < 4; ++bad) {
            auto header = original;
            if (bad == 0) header.owner_id++;
            if (bad == 1) header.page_id = PageId(42);
            if (bad == 2) header.generation = Generation(100);
            if (bad == 3) header.committed_lsn = LogSequenceNumber(3);
            writeFrame(header, std::span(bytes).subspan(RSS_PAGE_OWNER_HEADER_BYTES));
            EXPECT_THROW(reader.read(pin, PageId(41)), FormatException) << cached << "/" << bad;
        }
        writeFrame(original, std::span(bytes).subspan(RSS_PAGE_OWNER_HEADER_BYTES));
        EXPECT_EQ(reader.read(pin, PageId(41)).isPinned(), cached);
    }
}
#endif

TEST(HybridPropertyPageBindingTest, RowRemainderGroupUsesAuthenticatedSamePageMembershipInBorrowedView) {
    if constexpr (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED) return;
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
    HybridPropertyGroupRecord g{EntityPropertyOwnerKind::EPOK_NODE,
        HybridPropertyGroupMode::HPGM_HYBRID, 1, Generation(8), UINT64_MAX, 0, 0};
    HybridPropertyColumnMembership matrix;
    matrix.owner_kind = g.owner_kind; matrix.group_id = g.group_id;
    matrix.semantic_generation = g.semantic_generation; matrix.live = g.live; matrix.row_remainder = true;
    const auto membership = image.appendColumnMembership(matrix, g, Generation(9));
    g.mode = HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER; g.column_membership = membership;
    const auto groupSlot = image.appendGroup(g);
    auto bound = std::move(image).bindPrivateOwner(owner(), pool(), LogSequenceNumber(9));
    const auto payload = bound.serializeOwned(memory);
    MaterializedByteReservation held(memory.get()); held.set(memory->available());
    const auto view = HybridPropertyFieldView::open(owner(), payload, Generation(9), LogSequenceNumber(9));
    EXPECT_EQ(view.readGroup(groupSlot, g.owner_kind, 1, Generation(9)), g);
    EXPECT_EQ(view.readColumnMembership(membership, g, Generation(9)), matrix);
    auto wrong = g; wrong.column_membership.page_id = PageId(42);
    EXPECT_THROW(view.readColumnMembership(membership, wrong, Generation(9)), FormatException);
    wrong = g; wrong.mode = HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP;
    EXPECT_THROW(view.readColumnMembership(membership, wrong, Generation(9)), FormatException);
}

TEST(HybridPropertyPageBindingTest, ColdRowsValidateGroupOwnedMaskGlobalHotAuthorityAndAccountBeforeExposure) {
    if constexpr (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED) return;
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    auto image = HybridPropertyPageImage::create(PageId(41), Generation(1), Generation(9), memory);
    auto g = group(); auto c = column();
    const auto hot = image.appendColumn(c, g, Generation(9));
    const auto makeCold = [&](uint32_t token) {
        const auto root = EntityPropertyCollectionRootCodec::encode({49, graph::Node::typeId,
            {{token, PropertyValue("user-000048")}}});
        const HybridColdRowInput row{48, 1, false, root};
        return HybridPropertyColdRowBlock::encode(g.owner_kind, 1, Generation(8), std::span(&row, 1), memory, false);
    };
    const auto coldBytes = makeCold(2);
    const auto cold = image.appendColdRows(coldBytes, g.owner_kind, 1);
    const auto conflictingBytes = makeCold(1);
    const auto conflicting = image.appendColdRows(conflictingBytes, g.owner_kind, 1);
    auto matrix = HybridPropertyColumnMembership::fromColumns(g, std::span(&c, 1));
    matrix.row_remainder = true; matrix.locators[0] = hot;
    matrix.cold_fields = 1; matrix.cold_rows.owners = UINT64_C(1) << 48; matrix.cold_rows.roots[48] = cold;
    const auto membership = image.appendColumnMembership(matrix, g, Generation(9));
    g.mode = HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER; g.column_membership = membership;
    const auto groupSlot = image.appendGroup(g);
    auto bound = std::move(image).bindPrivateOwner(owner(), pool(), LogSequenceNumber(9));
    const auto payload = bound.serializeOwned(memory);
    const auto view = HybridPropertyFieldView::open(owner(), payload, Generation(9), LogSequenceNumber(9));
    EXPECT_EQ(view.readGroup(groupSlot, g.owner_kind, 1, Generation(9)), g);
    const auto baseline = memory->used();
    {
        const auto decoded = view.readColdRows(cold, g, matrix, Generation(9), memory);
        ASSERT_TRUE(decoded.row(48)); EXPECT_FALSE(decoded.row(47));
        EXPECT_EQ(EntityPropertyCollectionRootCodec::decodeSelected(decoded.row(48)->root, 2)->value,
            PropertyValue("user-000048"));
        EXPECT_GT(memory->used(), baseline);
    }
    EXPECT_EQ(memory->used(), baseline);
    auto wrong = matrix; wrong.cold_rows.roots[48] = conflicting;
    // Slot 48 has no hot membership. The token still belongs to the hot column
    // globally and must never fall back to its conflicting cold root.
    EXPECT_FALSE(wrong.contains({g.owner_kind, 49}, 1));
    EXPECT_THROW(view.readColdRows(conflicting, g, wrong, Generation(9), memory), FormatException);
    EXPECT_EQ(memory->used(), baseline);
    wrong = matrix; wrong.cold_rows.owners |= 1; wrong.cold_rows.roots[0] = cold; wrong.cold_fields = 2;
    auto larger = g; ++larger.memberships;
    EXPECT_THROW(view.readColdRows(cold, larger, wrong, Generation(9), memory), FormatException);
    auto stale = cold; ++stale.generation;
    EXPECT_THROW(view.readColdRows(stale, g, matrix, Generation(9), memory), FormatException);
    auto tiny = std::make_shared<MaterializedByteBudget>(1);
    EXPECT_THROW(view.readColdRows(cold, g, matrix, Generation(9), tiny), MaterializedByteLimitExceeded);
    EXPECT_EQ(tiny->used(), 0U); EXPECT_EQ(memory->used(), baseline);
    struct Stop final : CancellationCheckpoint {
        void check() const override { throw std::runtime_error("cancelled cold read"); }
    } cancelled;
    EXPECT_ANY_THROW(view.readColdRows(cold, g, matrix, Generation(9), memory, &cancelled));
    EXPECT_EQ(memory->used(), baseline);
}
