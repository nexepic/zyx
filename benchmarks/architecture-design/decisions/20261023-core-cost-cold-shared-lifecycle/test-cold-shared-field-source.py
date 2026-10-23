from pathlib import Path
r=Path('/workspace/zyx');p=r/'tests/include/storage/relocatable/property/HybridPropertyColdRowRoutingTestFixture.hpp';s=p.read_text();s=s.replace('bool cold_only_ = false, split_cold_ = false, raw_cold_ = false;','bool cold_only_ = false, split_cold_ = false, raw_cold_ = false;\n    bool with_other_owner_kind_ = false;',1)
s=s.replace('''        tx.reserveNodeIds(owner_kind_ == NODE ? live_owner_count_ : 2);
        for (uint64_t id = 1; id <= (owner_kind_ == NODE ? live_owner_count_ : 2U); ++id)''','''        const auto nodeCount = owner_kind_ == NODE || with_other_owner_kind_ ? live_owner_count_ : 2U;
        tx.reserveNodeIds(nodeCount);
        for (uint64_t id = 1; id <= nodeCount; ++id)''',1).replace('if (owner_kind_ != NODE) {\n            tx.reserveEdgeIds','if (owner_kind_ != NODE || with_other_owner_kind_) {\n            tx.reserveEdgeIds',1)
old='''        const auto groupSlot = image.appendGroup(group);
        SharedPagePoolAllocationRequest request;''';new='''        const auto groupSlot = image.appendGroup(group);
        SlottedSlotReference otherGroupSlot;
        const auto otherKind = owner_kind_ == NODE ? EntityPropertyOwnerKind::EPOK_EDGE : NODE;
        if (with_other_owner_kind_) {
            auto otherGroup = group; otherGroup.owner_kind = otherKind; otherGroup.memberships = hot_owner_count_ + 3U;
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
            otherGroupSlot = image.appendGroup(otherGroup);
        }
        SharedPagePoolAllocationRequest request;''';assert old in s;s=s.replace(old,new,1)
a=s.index('        for (unsigned role : {0U, 3U}) {');b=s.index('        static_cast<void>(btree_pages_->publishRoots',a);part=s[a:b]
part=part.replace('''        for (unsigned role : {0U, 3U}) {''','''        const std::array kinds{owner_kind_, otherKind};
        for (const auto kind : std::span(kinds).first(with_other_owner_kind_ ? 2U : 1U)) {
        for (unsigned role : {0U, 3U}) {''',1)
part=part.replace('if (role == 0 && !competing_selector_) continue;','if (role == 0 && (!competing_selector_ || kind != owner_kind_)) continue;',1)
part=part.replace('const auto ordinal = role + (owner_kind_ == NODE ? 0U : 4U);','const auto ordinal = role + (kind == NODE ? 0U : 4U);',1).replace('pageId.value() + 60 + role','pageId.value() + 60 + ordinal',1).replace('HybridPropertyDirectoryCodec::locatorValue(groupSlot)','HybridPropertyDirectoryCodec::locatorValue(kind == owner_kind_ ? groupSlot : otherGroupSlot)',1).replace('SegmentId(71 + role)','SegmentId(71 + ordinal)',1)
part+='        }\n';s=s[:a]+part+s[b:];p.write_text(s)
p=r/'tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp';s=p.read_text();s=s.replace('''schema.setGraphCardinalityStatistics({nodes ? live_owner_count_ : 2U, nodes ? 0U : live_owner_count_});
            if (!nodes)''','''schema.setGraphCardinalityStatistics({nodes || with_other_owner_kind_ ? live_owner_count_ : 2U,
                !nodes || with_other_owner_kind_ ? live_owner_count_ : 0U});
            if (!nodes || with_other_owner_kind_)''',1)
at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdNativeLifecycleTest, ActualInverseRetainsOtherNamespaceOnTheSameLogicalFieldFrame) {
    with_other_owner_kind_ = true; installDurableFixture(true); auto old = metadata_->openSnapshot(); const auto source = old.superblock();
    auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory);
        HybridPropertyReadContext before(io_, *slotted_pages_, *metadata_, old);
        const auto report = HybridPropertyDirectoryIntegrity::verify(before, old, memory, values_.get());
        EXPECT_EQ(report.groups, 2U); EXPECT_EQ(report.memberships, 102U);
        EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(before, old, field_page_id_, memory), 8U);
    }
    NativeGraphHybridPropertyRootMutation request{source.database_id, source.hybrid_property_descriptor_root,
        NativeHybridPropertyRootMode::NHPRM_REPLACE, {}, NativeGraphHybridPropertyConversion{NODE, 1, memory, nullptr,
            PhysicalSlotId(40), PhysicalSlotId(80), 902}};
    request.conversion->direction = NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW;
    request.conversion->publish_candidate = true; NativeGraphDerivedStateMutation derived;
    static_cast<void>(metadata_->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, derived,
        LogSequenceNumber(source.checkpoint_lsn.value() + 1), [](auto, auto, auto, auto, auto, auto) {},
        [](auto &) {}, {}, {}, {}, {}, {}, {}, {}, source.generation, {}, {}, {}, {}, {}, request));
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current), previous(io_, *slotted_pages_, *metadata_, old);
        const auto edgeKind = EntityPropertyOwnerKind::EPOK_EDGE;
        EXPECT_FALSE(properties_->isHybridPropertyOwner(current, {NODE, 1})); EXPECT_TRUE(properties_->isHybridPropertyOwner(current, {edgeKind, 1}));
        EXPECT_EQ(properties_->readEntityPropertyOptional(current, {NODE, 1}, 1)->value, graph::PropertyValue(int64_t(100)));
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.memberships, 51U); EXPECT_EQ(report.columns, 1U);
        EXPECT_EQ(HybridPropertyDirectoryIntegrity::verifyPageOwnership(context, current, field_page_id_, memory), 4U);
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {edgeKind, 1}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(200)));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {edgeKind, 1}, 2, values_.get()).value->value, graph::PropertyValue("other-first"));
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {edgeKind, 64}, 2, values_.get()).value->value, graph::PropertyValue("other-last"));
        EXPECT_EQ(HybridPropertyValueReader::read(previous, old, {NODE, 1}, 4, values_.get()).value->value, graph::PropertyValue(std::string(8193, 'x')));
        EXPECT_EQ(HybridPropertyValueReader::read(previous, old, {edgeKind, 1}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(200)));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''+s[at:];p.write_text(s)
