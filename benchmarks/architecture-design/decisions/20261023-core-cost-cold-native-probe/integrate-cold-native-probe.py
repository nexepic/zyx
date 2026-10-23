from pathlib import Path
p=Path('include/graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp');s=p.read_text();a=s.index('    struct NativeGraphHybridPropertyConversion {');b=s.index('    };',a);s=s[:b]+'''        // Same Native coordinator's explicit preparation-only cold layout.
        // Publication remains closed until the complete mode3 lifecycle passes.
        bool cold_row_remainder = false;
''' +s[b:];p.write_text(s)
p=Path('src/storage/relocatable/metadata/RelocatableMetadataStoreGraphMutation.cpp');s=p.read_text();idx=s.index('                const auto memory = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(conversion.memory);')
s=s[:idx]+'''                if (conversion.cold_row_remainder && conversion.publish_candidate)
                    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                        "native cold ROW publication requires complete mutation and reclamation lifecycle acceptance");
                if (conversion.cold_row_remainder && (!conversion.compact_column_memberships || conversion.group_count != 1U ||
                    conversion.direction != NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID))
                    throw FormatException(FormatError::FERR_INVALID_HEADER,
                        "native cold ROW preparation requires a single compact forward group");
''' +s[idx:]
a=s.index('                const auto resolveLarge = [&](EntityPropertyKey owner, uint32_t token, const PropertyValue &value) {');b=s.index('                    const size_t encodedBytes = RSS_LARGE_VALUE_REF_HEADER_BYTES',a)
old=s[a:b];d=old.index('                    const HybridPropertyLargeFieldPreparation *selected = &proposal;');retain=old[d:];retain=retain.replace('                    const HybridPropertyLargeFieldPreparation *selected = &proposal;','                    const HybridPropertyLargeFieldPreparation *selected = &proposal;',1)
# Existing dedup/ID ownership algorithm now serves both source shapes.
new='''                const auto retainProposal = [&](HybridPropertyLargeFieldPreparation proposal) -> const HybridPropertyLargeFieldPreparation & {
''' +retain+'''                    return *selected;
                };
                const auto resolveLarge = [&](EntityPropertyKey owner, uint32_t token, const PropertyValue &value) {
                    checkpoint();
                    const auto &proposal = retainProposal(HybridPropertyLargeFieldPreparation::prepare(*this, source, *conversionLargeValues,
                        owner, token, value, LargeValueId(batch.candidate().next_large_value_id), conversion.memory,
                        {}, LargeValueExtentClass::LVEC_32K, RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES, conversion.cancellation));
                    debug::PerfTrace::addValue("native_graph.hybrid.large_field_proposals_prepared", 1);
                    const auto *selected = &proposal;
'''
s=s[:a]+new+s[b:]
a=s.index('                const HybridPropertyConversionPreparation::LargeFieldResolver largeResolver = std::ref(resolveLarge);')
b=s.index('            }();',a)
s=s[:a]+'''                const auto resolveRoot = [&](EntityPropertyKey owner, std::span<const BorrowedPropertyCollectionRootEntry> entries) {
                    checkpoint();
                    const auto &proposal = retainProposal(HybridPropertyLargeFieldPreparation::prepareRootBorrowed(*this, source,
                        *conversionLargeValues, owner, entries, LargeValueId(batch.candidate().next_large_value_id),
                        conversion.memory, conversion.cancellation));
                    debug::PerfTrace::addValue("native_graph.hybrid.large_root_proposals_prepared", 1);
                    // The existing owner-keyed codec has two bounded standard
                    // buffers; charge their full supported media/proxy overlap.
                    constexpr size_t maximumReference = RSS_LARGE_VALUE_REF_HEADER_BYTES + RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES;
                    utils::OwnedHeapReservation credit(conversion.memory,
                        2U * utils::detail::standardContainerProxyBytes() +
                        utils::detail::standardAllocatorRequestBytes(maximumReference, alignof(std::byte)) +
                        utils::detail::standardAllocatorRequestBytes(RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES +
                            maximumReference, alignof(std::byte)));
                    const auto type = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
                    const auto encoded = EntityPropertyLargeValueReferenceCodec::encode({owner.entity_id, type, proposal.reference()});
                    return std::pmr::vector<std::byte>(encoded.begin(), encoded.end(), conversion.memory.get());
                };
                const HybridPropertyConversionPreparation::LargeFieldResolver largeResolver = std::ref(resolveLarge);
                const HybridPropertyConversionPreparation::WholeRootResolver rootResolver = std::ref(resolveRoot);
                return utils::shareOwned(std::unique_ptr<HybridPropertyConversionPreparation>(
                    new HybridPropertyConversionPreparation(batch, plan, checkpoint, largeResolver,
                        conversion.compact_column_memberships, conversion.cold_row_remainder, rootResolver)));
''' +s[b:];p.write_text(s)
