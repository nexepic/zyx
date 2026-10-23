from pathlib import Path
p=Path('/workspace/zyx/src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp');s=p.read_text();old='''                    *prior = HybridPropertyColumnDelta::mergeExistingColumnChanges(input, request.memory,
                        RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, request.cancellation, true);''';new='''                    *prior = HybridPropertyColumnDelta::mergeExistingColumnChanges(input, request.memory,
                        RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, request.cancellation, true,
                        !request.pending_owner_additions.empty());''';assert old in s;s=s.replace(old,new,1)
old='''    // Normal publication retains its original guard until complete cold inverse
    // installation/lifecycle acceptance. Private candidates reach only planning.
    if (!preparationSource) static_cast<void>(context.groupRecord({conversion.owner_kind,
        (conversion.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + 1U}));''';new='''    // The complete projection and source census below authenticate cold inverse
    // ownership. Generic groupRecord remains guarded for unadapted consumers;
    // a private candidate still unconditionally refuses before physical I/O.''';assert old in s;s=s.replace(old,new,1);p.write_text(s)
