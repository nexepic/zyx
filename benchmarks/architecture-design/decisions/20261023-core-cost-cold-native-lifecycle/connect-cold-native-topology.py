from pathlib import Path
p=Path('/workspace/zyx/src/storage/relocatable/metadata/RelocatableMetadataStoreGraphMutation.cpp');s=p.read_text();old='''        std::array<EntityPropertyKey, 64> pendingNativeOwners{};''';new='''        // Only the sealed graph/field coordinator consumes this cold-aware
        // source view. Generic mutation group lookup stays guarded elsewhere.
        const auto topologyGroup = [&](EntityPropertyKey owner, const std::function<void(size_t)> &admission,
            SlottedSlotReference *reference = nullptr) {
            return topologyFields->findGroup(owner, admission, reference, true, true);
        };
        std::array<EntityPropertyKey, 64> pendingNativeOwners{};''';assert old in s;s=s.replace(old,new,1);assert s.count('topologyFields->groupRecord(')==5;s=s.replace('topologyFields->groupRecord(', 'topologyGroup(');p.write_text(s)
