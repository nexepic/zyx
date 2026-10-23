from pathlib import Path
p=Path('/workspace/zyx/include/graph/storage/relocatable/property/HybridPropertyColumnDelta.hpp');s=p.read_text();old='''        const CancellationCheckpoint *cancellation = nullptr, bool prepareColdDemotion = false) {''';new='''        const CancellationCheckpoint *cancellation = nullptr, bool prepareColdDemotion = false,
        bool prepareColdPending = false) {''';assert old in s;s=s.replace(old,new,1)
s=s.replace('''((state.action == Action::HCD_DEMOTE_FIELD && !prepareColdDemotion) || state.effects.clear_live || state.effects.set_live))''','''((state.action == Action::HCD_DEMOTE_FIELD && !prepareColdDemotion) || state.effects.clear_live ||
                (state.effects.set_live && !prepareColdPending)))''',1);p.write_text(s)
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp');s=p.read_text();old='''        auto merged = HybridPropertyColumnDelta::mergeExistingColumnChanges(inputs, memory,
            RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, nullptr, true);
        ASSERT_EQ(merged.action(), HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD);
        const std::array<const HybridPropertyColumnDelta *, 1> demotions{&merged};''';new='''        EXPECT_THROW((void)HybridPropertyColumnDelta::mergeExistingColumnChanges(inputs, memory,
            RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, nullptr, true), FormatException);
        auto merged = HybridPropertyColumnDelta::mergeExistingColumnChanges(inputs, memory,
            RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, nullptr, true, true);
        ASSERT_EQ(merged.action(), HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD);
        const std::array<const HybridPropertyColumnDelta *, 1> demotions{&merged};''';assert s.count(old)==1;s=s.replace(old,new,1);p.write_text(s)
