from pathlib import Path
r=Path('/workspace/zyx');p=r/'include/graph/storage/relocatable/property/HybridPropertyColumnDelta.hpp';s=p.read_text();old='''        const PropertyValue &replacement) {
        return planAbsentColumnMember(context, snapshot, owner, token, nextGeneration, std::move(memory), replacement, true);
    }''';new='''        const PropertyValue &replacement, bool prepareColdPending = false) {
        return planAbsentColumnMember(context, snapshot, owner, token, nextGeneration, std::move(memory), replacement, true, prepareColdPending);
    }''';assert old in s;s=s.replace(old,new,1)
s=s.replace('if (group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER && pendingOwner)','if (group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER && pendingOwner && !prepareColdDemotion)',1);p.write_text(s)
p=r/'include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp';s=p.read_text();s=s.replace('if (previous.mode != HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP || previous.live == replacement.live) return;','if (!previous.usesColumnMembership() || previous.live == replacement.live) return;',1)
old='std::span<const EntityPropertyKey> pendingOwners = {}, bool allowMultipleGroups = false)';new='std::span<const EntityPropertyKey> pendingOwners = {}, bool allowMultipleGroups = false, bool prepareColdPending = false)';assert old in s;s=s.replace(old,new,1)
old='''                    !effects.clear_live && !effects.set_live && !pendingMask(group->source),''';new='''                    !effects.clear_live && !(effects.set_live & ~pendingMask(group->source)) &&
                    (prepareColdPending || (!effects.set_live && !pendingMask(group->source))),''';assert old in s;s=s.replace(old,new,1)
# Locate this specific existing preparer, preserving the ordinary callers' guard.
a=s.index('[[nodiscard]] static HybridPropertyDemotionPreparation preparePendingOwners(');b=s.index('    // A complete private owner deletion.',a);part=s[a:b]
old='const LargeFieldResolver &largeField = {})';new='const LargeFieldResolver &largeField = {}, bool prepareColdPending = false)';assert old in part;part=part.replace(old,new,1)
old='context.findGroup(owner, std::ref(admit), &reference)';new='context.findGroup(owner, std::ref(admit), &reference, true, prepareColdPending)';assert old in part;part=part.replace(old,new,1)
old='memory, cancellation, largeField, false, owners, true);';new='memory, cancellation, largeField, false, owners, true, prepareColdPending);';assert old in part;part=part.replace(old,new,1)
s=s[:a]+part+s[b:];p.write_text(s)
p=r/'tests/include/storage/relocatable/property/HybridPropertyColdRowRoutingTestFixture.hpp';s=p.read_text();s=s.replace('unsigned hot_owner_count_ = 48;','unsigned hot_owner_count_ = 48;\n    unsigned live_owner_count_ = 64;',1)
s=s.replace('''        tx.reserveNodeIds(owner_kind_ == NODE ? 64 : 2);
        for (uint64_t id = 1; id <= (owner_kind_ == NODE ? 64U : 2U); ++id)''','''        ASSERT_GE(live_owner_count_, 48U); ASSERT_LE(live_owner_count_, 64U);
        tx.reserveNodeIds(owner_kind_ == NODE ? live_owner_count_ : 2);
        for (uint64_t id = 1; id <= (owner_kind_ == NODE ? live_owner_count_ : 2U); ++id)''',1)
s=s.replace('tx.reserveEdgeIds(64);\n            for (uint64_t id = 1; id <= 64; ++id)','tx.reserveEdgeIds(live_owner_count_);\n            for (uint64_t id = 1; id <= live_owner_count_; ++id)',1)
s=s.replace('EntityPropertyCollectionRootCodec::encode({64, ownerType,','EntityPropertyCollectionRootCodec::encode({live_owner_count_, ownerType,',1)
s=s.replace('''        HybridPropertyGroupRecord group{owner_kind_,''','''        const auto live = live_owner_count_ == 64U ? UINT64_MAX : (UINT64_C(1) << live_owner_count_) - 1U;
        const auto lastColdBit = UINT64_C(1) << (live_owner_count_ - 1U);
        HybridPropertyGroupRecord group{owner_kind_,''',1)
s=s.replace('1, current.generation, UINT64_MAX, uint32_t','1, current.generation, live, uint32_t',1).replace('ASSERT_LE(hot_owner_count_, 64U);','ASSERT_LE(hot_owner_count_, live_owner_count_);',1).replace('column.live = UINT64_MAX;','column.live = live;',1)
s=s.replace('{63, 1, false, last}','{uint8_t(live_owner_count_ - 1U), 1, false, last}',1)
s=s.replace('UINT64_C(1) | (UINT64_C(1) << 63)','UINT64_C(1) | lastColdBit',1).replace('matrix.cold_rows.roots[63] =','matrix.cold_rows.roots[live_owner_count_ - 1U] =',1);p.write_text(s)
