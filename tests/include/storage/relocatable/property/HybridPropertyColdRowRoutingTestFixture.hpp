#pragma once
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/property/HybridPropertyValueReader.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"
#include "storage/relocatable/property/HybridPropertyReadContextTestAccess.hpp"
#include "storage/relocatable/property/HybridPropertyDescriptorStoreTestAccess.hpp"
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
namespace graph::storage::test {
using graph::query::execution::MaterializedByteBudget;
class HybridPropertyColdRowRoutingTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
    static constexpr auto NODE = EntityPropertyOwnerKind::EPOK_NODE;
    EntityPropertyOwnerKind owner_kind_ = NODE;
    bool cold_only_ = false, split_cold_ = false, raw_cold_ = false;
    bool with_other_owner_kind_ = false;
    unsigned hot_owner_count_ = 48;
    unsigned live_owner_count_ = 64;
    Generation physical_generation_{99};
    bool orphan_cold_ = false, orphan_matrix_ = false, wrong_cold_binding_ = false, wrong_cold_count_ = false;
    bool malformed_cold_value_ = false, future_cold_reference_ = false;
    PageId field_page_id_{};
    SlottedSlotReference cold_reference_{};
    std::vector<std::byte> cold_encoded_bytes_;
    LargeValueRef value_reference_{};
    HybridPropertyDescriptor descriptor_;
    std::unique_ptr<LargeValueStore> values_;
    bool omit_cold_ = false;
    size_t last_cold_text_bytes_ = 0;
    bool competing_row_ = false, competing_selector_ = false, bad_cold_crc_ = false, hot_in_cold_ = false;
    void seed(bool large = false) {
        NativeGraphWritePlannerOptions options;
        options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
        options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
        NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
        const auto ownerType = owner_kind_ == NODE ? graph::Node::typeId : graph::Edge::typeId;
        auto tx = planner.beginTransaction();
        ASSERT_GE(live_owner_count_, 48U); ASSERT_LE(live_owner_count_, 64U);
        const auto nodeCount = owner_kind_ == NODE || with_other_owner_kind_ ? live_owner_count_ : 2U;
        tx.reserveNodeIds(nodeCount);
        for (uint64_t id = 1; id <= nodeCount; ++id) tx.createNodeWithReservedId(id, {});
        if (owner_kind_ != NODE || with_other_owner_kind_) {
            tx.reserveEdgeIds(live_owner_count_);
            for (uint64_t id = 1; id <= live_owner_count_; ++id) tx.createEdgeWithReservedId(id, 1, 2, 7);
        }
        if (competing_row_) {
            PropertyCollectionRoot row{1, ownerType, {{7, PropertyValue("competing")}}};
            if (owner_kind_ == NODE) tx.replaceNodeProperty(std::move(row)); else tx.replaceEdgeProperty(std::move(row));
        }
        ASSERT_TRUE(tx.commit().committed());
        std::vector<std::byte> first;
        if (large) {
            values_ = std::make_unique<LargeValueStore>(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            const auto payload = EntityPropertyLargeValuePayloadCodec::encode(std::vector<PropertyCollectionRootEntry>{
                {2, PropertyValue("user-000000")}, {4, PropertyValue(std::string(8193, 'x'))}});
            LargeValueWriteOptions placement;
            placement.first_data_slot = placement.first_manifest_slot = PhysicalSlotId(40);
            placement.last_data_slot = placement.last_manifest_slot = PhysicalSlotId(80);
            placement.media_type = RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE;
            auto ref = values_->write(payload, placement,
                LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1)).reference;
            value_reference_ = ref;
            if (future_cold_reference_) ref.generation = metadata_->openSnapshot().superblock().generation;
            first = EntityPropertyLargeValueReferenceCodec::encode({1, ownerType, ref});
        } else first = EntityPropertyCollectionRootCodec::encode({1, ownerType,
            {{2, PropertyValue("user-000000")}, {4, PropertyValue()}}});
        if (malformed_cold_value_ && !large) {
            size_t offset = 0; const std::array<uint32_t, 1> tokens{2};
            EntityPropertyCollectionRootCodec::visitSelectedEncoded(first, 1, ownerType, tokens,
                [&](size_t, auto bytes) { if (bytes) offset = size_t(bytes->data() - first.data()); });
            first[offset] = std::byte{0xff}; // canonical directory intact; selected value invalid
        }
        const auto last = EntityPropertyCollectionRootCodec::encode({live_owner_count_, ownerType,
            {{hot_in_cold_ ? 1U : 2U, PropertyValue(last_cold_text_bytes_ ? std::string(last_cold_text_bytes_, 'x') : std::string("user-000063"))}}});
        const auto current = metadata_->openSnapshot().superblock();
        const auto pageId = PageId((std::max)(UINT64_C(41), current.next_logical_page_id));
        field_page_id_ = pageId;
        auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
        auto image = HybridPropertyPageImage::create(pageId, physical_generation_, current.generation, memory);
        const auto live = live_owner_count_ == 64U ? UINT64_MAX : (UINT64_C(1) << live_owner_count_) - 1U;
        const auto lastColdBit = UINT64_C(1) << (live_owner_count_ - 1U);
        HybridPropertyGroupRecord group{owner_kind_, HybridPropertyGroupMode::HPGM_HYBRID,
            1, current.generation, live, uint32_t((cold_only_ ? 0U : hot_owner_count_) + (omit_cold_ ? 0U : 3U)), uint32_t(cold_only_ ? 0 : 1)};
        HybridPropertyColumnRecord column;
        column.identity = {owner_kind_, HybridPropertyScalarKind::HPSK_INT64, 1, 1, current.generation};
        ASSERT_GE(hot_owner_count_, 48U); ASSERT_LE(hot_owner_count_, live_owner_count_);
        column.live = live; column.presence = hot_owner_count_ == 64 ? UINT64_MAX : (UINT64_C(1) << hot_owner_count_) - 1; column.nulls = UINT64_C(1) << 2;
        for (unsigned slot = 0; slot < hot_owner_count_; ++slot) column.words[slot] = slot == 2 ? 0 : slot + 100;
        const auto hot = cold_only_ ? SlottedSlotReference{} : image.appendColumn(column, group, current.generation);
        const std::array<HybridColdRowInput, 2> rows{{{0, 2, large, first}, {uint8_t(live_owner_count_ - 1U), 1, false, last}}};
        if (future_cold_reference_) { ASSERT_TRUE(large); ASSERT_GT(current.generation.value(), 1U); }
        const auto blockGeneration = future_cold_reference_ ? Generation(current.generation.value() - 1) : current.generation;
        auto encoded = HybridPropertyColdRowBlock::encode(owner_kind_, 1, blockGeneration,
            split_cold_ ? std::span(rows).first(1) : std::span(rows), memory, !raw_cold_);
        const auto cold = omit_cold_ ? SlottedSlotReference{} : image.appendColdRows(encoded, owner_kind_, 1); cold_reference_ = cold;
        auto lastCold = cold;
        if (split_cold_ && !omit_cold_) {
            const auto bytes = HybridPropertyColdRowBlock::encode(owner_kind_, 1, blockGeneration,
                std::span(rows).last(1), memory, !raw_cold_);
            lastCold = image.appendColdRows(bytes, owner_kind_, 1);
        }
        if (orphan_cold_) static_cast<void>(image.appendColdRows(encoded, owner_kind_, 1));
        HybridPropertyColumnMembership matrix;
        if (!cold_only_) { matrix = HybridPropertyColumnMembership::fromColumns(group, std::span(&column, 1)); matrix.locators[0] = hot; }
        else { matrix.owner_kind = owner_kind_; matrix.group_id = 1; matrix.semantic_generation = current.generation; matrix.live = group.live; }
        matrix.row_remainder = true; matrix.cold_fields = omit_cold_ ? 0 : wrong_cold_count_ ? 4 : 3;
        if (wrong_cold_count_) ++group.memberships;
        if (!omit_cold_) {
            matrix.cold_rows.owners = UINT64_C(1) | lastColdBit;
            matrix.cold_rows.roots[0] = cold; matrix.cold_rows.roots[live_owner_count_ - 1U] = wrong_cold_binding_ ? hot : lastCold;
        }
        group.column_membership = image.appendColumnMembership(matrix, group, current.generation);
        group.mode = HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER;
        if (orphan_matrix_) static_cast<void>(image.appendColumnMembership(matrix, group, current.generation));
        const auto groupSlot = image.appendGroup(group);
        SlottedSlotReference otherGroupSlot;
        const auto otherKind = owner_kind_ == NODE ? EntityPropertyOwnerKind::EPOK_EDGE : NODE;
        if (with_other_owner_kind_) {
            auto otherGroup = group; otherGroup.mode = HybridPropertyGroupMode::HPGM_HYBRID;
            otherGroup.owner_kind = otherKind; otherGroup.memberships = hot_owner_count_ + 3U;
            otherGroup.columns = 1; otherGroup.column_membership = {};
            auto otherColumn = column; otherColumn.identity.owner_kind = otherKind;
            for (unsigned slot = 0; slot < hot_owner_count_; ++slot) otherColumn.words[slot] = slot == 2 ? 0 : slot + 200;
            const auto otherHot = image.appendColumn(otherColumn, otherGroup, current.generation);
            const auto otherType = otherKind == NODE ? graph::Node::typeId : graph::Edge::typeId;
            const auto otherFirst = EntityPropertyCollectionRootCodec::encode({1, otherType,
                {{2, PropertyValue("other-first")}, {4, PropertyValue()}}});
            const auto otherLast = EntityPropertyCollectionRootCodec::encode({live_owner_count_, otherType,
                {{2, PropertyValue("other-last")}}});
            const std::array<HybridColdRowInput, 2> otherRows{{{0, 2, false, otherFirst},
                {uint8_t(live_owner_count_ - 1U), 1, false, otherLast}}};
            const auto otherBytes = HybridPropertyColdRowBlock::encode(otherKind, 1, current.generation, otherRows, memory);
            const auto otherCold = image.appendColdRows(otherBytes, otherKind, 1);
            auto otherMatrix = HybridPropertyColumnMembership::fromColumns(otherGroup, std::span(&otherColumn, 1));
            otherMatrix.locators[0] = otherHot; otherMatrix.row_remainder = true; otherMatrix.cold_fields = 3;
            otherMatrix.cold_rows.owners = 1U | lastColdBit;
            otherMatrix.cold_rows.roots[0] = otherCold; otherMatrix.cold_rows.roots[live_owner_count_ - 1U] = otherCold;
            otherGroup.column_membership = image.appendColumnMembership(otherMatrix, otherGroup, current.generation);
            otherGroup.mode = HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER;
            otherGroupSlot = image.appendGroup(otherGroup);
        }
        SharedPagePoolAllocationRequest request;
        request.segment_id = SegmentId(31); request.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
        request.pool_owner_id = 902; request.generation = physical_generation_; request.page_id = pageId;
        request.page_owner_id = 902; request.page_owner_kind = PageOwnerKind::POK_PROPERTY;
        const auto neutral = image.serialize(); request.payload.assign(neutral.begin(), neutral.end());
        request.page_free_bytes = image.stats().contiguous_free_bytes;
        const auto lsn = LogSequenceNumber(current.checkpoint_lsn.value() + 1);
        SharedPagePoolAllocator pools(io_, *metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        const auto allocation = pools.allocateNewPoolPage(std::move(request), PhysicalSlotId(40), PhysicalSlotId(80), lsn);
        ASSERT_TRUE(allocation);
        PageOwnerHeader owner{pageId, 902, PageOwnerKind::POK_HYBRID_PROPERTY, 0,
            RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, physical_generation_, lsn};
        auto bound = std::move(image).bindPrivateOwner(owner, allocation->pool, lsn);
        if (bad_cold_crc_) { encoded.back() ^= std::byte{1}; bound.overwrite(cold, encoded); }
        cold_encoded_bytes_.assign(encoded.begin(), encoded.end());
        const auto payload = bound.serializeOwned(memory);
        std::array<std::byte, RSS_PAGE_SIZE> frame{};
        const auto header = PageOwnerHeaderCodec::encode(owner);
        std::copy(header.begin(), header.end(), frame.begin());
        std::copy(payload.begin(), payload.end(), frame.begin() + RSS_PAGE_OWNER_HEADER_BYTES);
        const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(current.data_region_offset, allocation->pool.physical_slot) +
            uint64_t(allocation->location.page_index) * RSS_PAGE_SIZE;
        io_->writeAt({offset, physical_generation_.value()}, frame.data(), frame.size());
        std::vector<IndexRootLocation> roots;
        const std::array kinds{owner_kind_, otherKind};
        for (const auto kind : std::span(kinds).first(with_other_owner_kind_ ? 2U : 1U)) {
        for (unsigned role : {0U, 3U}) {
            if (role == 0 && (!competing_selector_ || kind != owner_kind_)) continue;
            const auto ordinal = role + (kind == NODE ? 0U : 4U);
            const auto index = hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(ordinal));
            const auto treePage = PageId(pageId.value() + 60 + ordinal);
            const auto next = LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1);
            PageOwnerHeader treeOwner{treePage, index, PageOwnerKind::POK_INDEX, 0,
                RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, physical_generation_, next};
            BTreePageHeader treeHeader; treeHeader.index_id = index; treeHeader.page_id = treePage;
            treeHeader.generation = physical_generation_; treeHeader.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
            auto tree = BTreePage::create(treeOwner, treeHeader);
            std::vector<std::byte> entry;
            if (role == 3) {
                const auto bytes = HybridPropertyGroupDirectoryMap::encode(HybridPropertyDirectoryCodec::groupKey(1),
                    HybridPropertyDirectoryCodec::locatorValue(kind == owner_kind_ ? groupSlot : otherGroupSlot)); entry.assign(bytes.begin(), bytes.end());
            } else {
                const auto bytes = HybridPropertyOwnerMap::encode(HybridPropertyDirectoryCodec::ownerKey({owner_kind_, 64}, 1),
                    HybridPropertyDirectoryCodec::sourceValue(HybridPropertySource::HPS_EXTENSION)); entry.assign(bytes.begin(), bytes.end());
            }
            tree.insert({entry, 0});
            SlottedPagePoolAllocationRequest placement;
            placement.segment_id = SegmentId(71 + ordinal); placement.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL;
            placement.pool_owner_id = index; placement.generation = physical_generation_; placement.page_id = treePage;
            placement.page_owner_id = index; placement.page_owner_kind = PageOwnerKind::POK_INDEX;
            ASSERT_TRUE(btree_pages_->allocate(placement, tree, PhysicalSlotId(40), PhysicalSlotId(80), next));
            roots.push_back({index, treePage, physical_generation_}); descriptor_.roots[ordinal] = {treePage, physical_generation_};
        }
        }
        static_cast<void>(btree_pages_->publishRoots(roots, {},
            LogSequenceNumber(metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1)));
        static_cast<void>(HybridPropertyDescriptorStoreTestAccess::enableCompactReadFixture(*metadata_));
        const auto final = metadata_->openSnapshot().superblock();
        descriptor_.database_id = final.database_id; descriptor_.semantic_generation = final.generation;
    }
};
}
#endif
