from pathlib import Path
p=Path('/workspace/zyx/src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp');s=p.read_text();old='''                    old[i] = roots.readEntityPropertyOptional(source, owner, *token);
                    if (old[i]) value = &old[i]->value;''';new='''                    auto selected = HybridPropertyValueReader::read(context, source, owner, *token, &sourceValues);
                    old[i] = selected.route == HybridPropertyReadContext::FieldRoute::HFR_ROW ?
                        roots.readEntityPropertyOptional(source, owner, *token) : std::move(selected.value);
                    if (old[i]) value = &old[i]->value;''';assert s.count(old)==1;s=s.replace(old,new);p.write_text(s)
