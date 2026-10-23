from pathlib import Path
p=Path('include/graph/storage/relocatable/property/HybridPropertyReadContext.hpp');s=p.read_text();a=s.index('    [[nodiscard]] FieldRoute visitField(');b=s.index('\nprivate:',a);method=s[a:b];brace=method.index(' {\n');signature=method[:brace];body=method[brace:];wrapper=signature+''' {
        return visitFieldImpl(owner, token, scalar, extension, boundaryAdmission, referenceAdmission,
            columnVisitor, encoded, largeValues, false);
    }
private:
''';impl=signature.replace('FieldRoute visitField(', 'FieldRoute visitFieldImpl(',1).replace('LargeValueStore *largeValues = nullptr) const', 'LargeValueStore *largeValues = nullptr, bool coldColumnPlan = false) const')+body;impl=impl.replace('            if (columnVisitor) throw FormatException(', '            if (columnVisitor && !coldColumnPlan) throw FormatException(',1);s=s[:a]+wrapper+impl+s[b:]
key='''            bool found = false;
            withColdOwner(owner, group, matrix, [&](auto &source) {''';# Only the remainder-field branch, not owner enumeration.
a=s.index('    FieldRoute visitRemainderField(');b=s.index('    [[nodiscard]] AuthenticatedColumn columnRecord',a);part=s[a:b];assert key in part;part=part.replace(key,'''            // Private hot planning grants no cold-field authority and must not
            // decode a non-hot source merely to decline this write route.
            if (columnVisitor) return FieldRoute::HFR_MISSING;
''' +key,1);s=s[:a]+part+s[b:];p.write_text(s)
p=Path('include/graph/storage/relocatable/property/HybridPropertyColumnDelta.hpp');s=p.read_text();a=s.index('    [[nodiscard]] static std::optional<HybridPropertyColumnDelta> planExistingColumn(');b=s.index('    // Missing memberships',a);v=s[a:b];v=v.replace('if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP)', 'if (group.usesColumnMembership())',1);v=v.replace('''            change(*state, owner, replacement, eraseOwner, nativeRecordLimit);
            result''','''            if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER && eraseOwner)
                throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "cold owner deletion requires its complete lifecycle");
            change(*state, owner, replacement, eraseOwner, nativeRecordLimit);
            if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER && state->action == Action::HCD_DEMOTE_FIELD)
                throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "cold ROW hot-field demotion requires complete repacking");
            result''',1);v=v.replace('context.visitField(owner, token,', 'context.visitFieldImpl(owner, token,',1).replace('std::ref(boundary), std::ref(reference), std::ref(column)));', 'std::ref(boundary), std::ref(reference), std::ref(column), {}, nullptr, true));',1);s=s[:a]+v+s[b:]
a=s.index('    [[nodiscard]] static std::optional<HybridPropertyColumnDelta> planAbsentColumnMember(');b=s.index('    [[nodiscard]] static HybridPropertyColumnDelta mergeExistingColumnChanges(',a);v=s[a:b];v=v.replace('''        require(context.visitField(owner, token, [](const auto &) {}, [](const auto &) {}, std::ref(boundary),
            std::ref(reference))''','''        const HybridPropertyReadContext::ColumnVisitor columnProbe = [](const auto &, const auto &, const auto &, const auto &) {};
        require(context.visitFieldImpl(owner, token, [](const auto &) {}, [](const auto &) {}, std::ref(boundary),
            std::ref(reference), columnProbe, {}, nullptr, true)''',1);v=v.replace('context.findGroup(owner, std::ref(boundary), &groupRef)', 'context.findGroup(owner, std::ref(boundary), &groupRef, true, true)',1);needle='''        require(group && context.isLive(owner, *group) != pendingOwner,
            "native insertion has inconsistent existing/pending owner liveness");''';assert needle in v;v=v.replace(needle,needle+'''
        if (group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER && pendingOwner)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "cold pending-owner insertion requires complete liveness coordination");''',1);v=v.replace('group->mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP', 'group->usesColumnMembership()');# Guard after constructing the final candidate, before it escapes.
key='''        change(*state, owner, &replacement, false, RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, pendingOwner);'''
if key not in v:
 print(v[v.index('        state->group ='):])
 raise SystemExit('locate insertion change before editing')
v=v.replace(key,key+'''
        if (group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER && state->action == Action::HCD_DEMOTE_FIELD)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "cold ROW hot insertion requires complete demotion repacking");''',1);s=s[:a]+v+s[b:]
key='''        check();
        return HybridPropertyColumnDelta(std::move(merged));''';assert key in s;s=s.replace(key,'''        if (state.group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
            (state.action == Action::HCD_DEMOTE_FIELD || state.effects.clear_live || state.effects.set_live))
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "merged cold hot changes require complete demotion/liveness coordination");
''' +key,1);p.write_text(s)
p=Path('include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp');s=p.read_text();a=s.index('        for (const auto *plan : changes) {');b=s.index('        std::sort(ordered.begin()',a);v=s[a:b];key='''            const auto effects = plan->effects();''';v=v.replace(key,key+'''
            if (group->source.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER)
                require(plan->action() != HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD &&
                    !effects.clear_live && !effects.set_live && !pendingMask(group->source),
                    "cold ROW private preparation only admits compatible hot changes");''',1);v=v.replace('group->source.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP', 'group->source.usesColumnMembership()');s=s[:a]+v+s[b:];s=s.replace('memberships < group.source.memberships && group.source.mode != HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP', 'memberships < group.source.memberships && !group.source.usesColumnMembership()',1);s=s.replace('if (change.sourceGroup().mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) continue;', 'if (change.sourceGroup().usesColumnMembership()) continue;',1);p.write_text(s)
