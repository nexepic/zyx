from pathlib import Path
p=Path('/workspace/zyx/include/graph/storage/relocatable/property/HybridPropertyColumnDelta.hpp');s=p.read_text()
s=s.replace('''        size_t nativeRecordLimit = RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)''','''        size_t nativeRecordLimit = RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES,
        bool prepareColdDemotion = false) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)''',1)
s=s.replace('''if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER && state->action == Action::HCD_DEMOTE_FIELD)''','''if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
                state->action == Action::HCD_DEMOTE_FIELD && !prepareColdDemotion)''',1)
# Explicitly opt into PREPARATION only; this flag never grants owner-liveness or publication authority.
a=s.index('[[nodiscard]] static std::optional<HybridPropertyColumnDelta> planMissingColumnMember(');b=s.index('    // Private images only:',a);c=s[a:b].replace('''const PropertyValue &replacement) {''','''const PropertyValue &replacement, bool prepareColdDemotion = false) {''').replace('''std::move(memory), replacement, false);''','''std::move(memory), replacement, false, prepareColdDemotion);''');s=s[:a]+c+s[b:]
a=s.index('[[nodiscard]] static std::optional<HybridPropertyColumnDelta> planAbsentColumnMember(');b=s.index('\npublic:',a);c=s[a:b].replace('''const PropertyValue &replacement, bool pendingOwner) {''','''const PropertyValue &replacement, bool pendingOwner, bool prepareColdDemotion = false) {''').replace('''state->action == Action::HCD_DEMOTE_FIELD)''','''state->action == Action::HCD_DEMOTE_FIELD && !prepareColdDemotion)''');s=s[:a]+c+s[b:]
a=s.index('[[nodiscard]] static HybridPropertyColumnDelta mergeExistingColumnChanges(');b=s.index('    [[nodiscard]] bool isValid()',a);c=s[a:b].replace('''const CancellationCheckpoint *cancellation = nullptr) {''','''const CancellationCheckpoint *cancellation = nullptr, bool prepareColdDemotion = false) {''',1).replace('''(state.action == Action::HCD_DEMOTE_FIELD || state.effects.clear_live || state.effects.set_live)''','''((state.action == Action::HCD_DEMOTE_FIELD && !prepareColdDemotion) || state.effects.clear_live || state.effects.set_live)''');s=s[:a]+c+s[b:]
s=s.replace('''    [[nodiscard]] static std::optional<HybridPropertyColumnDelta> planExistingColumn(''','''    // prepareColdDemotion exposes only complete immutable cell planning for
    // the bounded whole-column coordinator. Default callers keep the original
    // guard; Entity liveness and physical publication remain forbidden.
    [[nodiscard]] static std::optional<HybridPropertyColumnDelta> planExistingColumn(''',1)
p.write_text(s)
