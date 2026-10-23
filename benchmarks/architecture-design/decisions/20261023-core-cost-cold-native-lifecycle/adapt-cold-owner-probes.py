from pathlib import Path
r=Path('/workspace/zyx');p=r/'include/graph/storage/relocatable/property/HybridPropertyReadContext.hpp';s=p.read_text();old='''    // Fixed-size authenticated source for bounded inverse conversion.''';new='''    // Read-only group/Entity membership probe. A disengaged result follows
    // ordinary ROW routing; false names an absent pending slot in a real group.
    // This authenticates Entity/competing ROW authority without visiting fields
    // and grants no mutation or new-owner publication authority.
    [[nodiscard]] std::optional<bool> ownerLiveness(EntityPropertyKey owner,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        const auto group = findGroup(owner, boundaryAdmission, nullptr, true, true);
        if (!group) return {};
        return isLive(owner, *group);
    }
    // Fixed-size authenticated source for bounded inverse conversion.''';assert old in s;s=s.replace(old,new,1);p.write_text(s)
p=r/'src/storage/relocatable/property/PropertyCollectionRootStore.cpp';s=p.read_text();old='''        return bool(context.groupRecord(key, transientAdmission ? *transientAdmission : SelectedPropertyReadAdmission{}));''';new='''        return context.ownerLiveness(key, transientAdmission ? *transientAdmission : SelectedPropertyReadAdmission{}).has_value();''';assert old in s;s=s.replace(old,new,1)
old='''        const auto group = context.groupRecord(key, transientAdmission ? *transientAdmission : SelectedPropertyReadAdmission{});
        return group && (group->live & (UINT64_C(1) << hybridPropertySlotFor(key)));''';new='''        return context.ownerLiveness(key, transientAdmission ? *transientAdmission : SelectedPropertyReadAdmission{}).value_or(false);''';assert old in s;s=s.replace(old,new,1);p.write_text(s)
