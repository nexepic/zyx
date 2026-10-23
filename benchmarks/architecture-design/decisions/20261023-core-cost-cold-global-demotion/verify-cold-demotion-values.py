from pathlib import Path
p=Path('/workspace/zyx/include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp');s=p.read_text();a=s.index('''                std::array<bool, 64> present{};''');b=s.index('''            matrix.semantic_generation = batch.generation();''',a)
s=s[:a]+'''                const auto verifyValue = [&](size_t ordinal, std::optional<std::span<const std::byte>> bytes) {
                    check(); const auto token = removedTokens[ordinal];
                    const auto found = std::find_if(demotions.begin(), demotions.end(), [&](const auto *plan) {
                        return plan->sourceColumn().identity.token == token;
                    });
                    require(found != demotions.end(), "cold demotion lost its exact column plan");
                    const auto &cell = (*found)->demotedCells()[row.owner_slot];
                    require(bool(bytes) == (cell.kind != HybridPropertyColumnDelta::CellKind::HCC_MISSING),
                        "cold demotion target contradicts an actual final column membership");
                    if (!bytes) return;
                    utils::OwnedHeapReservation scalarCredit(memory, 3U * utils::detail::standardContainerProxyBytes() +
                        utils::detail::standardAllocatorRequestBytes(8U, alignof(char)));
                    PropertyValue scalar; const PropertyValue *expected = &scalar;
                    switch (cell.kind) {
                        case HybridPropertyColumnDelta::CellKind::HCC_NULL: break;
                        case HybridPropertyColumnDelta::CellKind::HCC_INT64: scalar = PropertyValue(std::bit_cast<int64_t>(cell.bits)); break;
                        case HybridPropertyColumnDelta::CellKind::HCC_DOUBLE: scalar = PropertyValue(std::bit_cast<double>(cell.bits)); break;
                        case HybridPropertyColumnDelta::CellKind::HCC_BOOL: scalar = PropertyValue(bool(cell.bits)); break;
                        case HybridPropertyColumnDelta::CellKind::HCC_SHORT_TEXT7: scalar = PropertyValue(HybridPropertyShortText::decode(cell.bits)); break;
                        case HybridPropertyColumnDelta::CellKind::HCC_EXTENSION:
                            require(cell.extension, "cold demotion lost its borrowed replacement value"); expected = cell.extension; break;
                        default: require(false, "cold demotion has an invalid final cell");
                    }
                    std::pmr::vector<std::byte> encoded(memory.get());
                    encoded.reserve(CanonicalPropertyValueCodec::encodedSize(*expected));
                    CanonicalPropertyValueCodec::append(encoded, *expected);
                    require(std::ranges::equal(encoded, *bytes), "cold demotion target changes an authenticated final column value");
                };
                if (row.root.empty()) {
                    for (size_t i = 0; i < removedTokens.size(); ++i) verifyValue(i, {});
                } else if (!row.large_value_reference)
                    EntityPropertyCollectionRootCodec::visitSelectedEncoded(row.root, owner.entity_id, type, removedTokens, verifyValue);
                else {
                    utils::OwnedHeapReservation referenceCredit(memory, row.root.size() + 4U * utils::detail::standardContainerProxyBytes());
                    const auto reference = EntityPropertyLargeValueReferenceCodec::decode(row.root);
                    if (reference.value.value_id.value() >= batch.current().next_large_value_id) {
                        require(admitPreparedColdRoot(batch, row, owner, std::span(matrix.tokens).first(matrix.column_count),
                            newRoots, memory, cancellation), "cold demotion target has no original reserved carrier proposal");
                        const auto found = std::find_if(newRoots.begin(), newRoots.end(), [&](const auto &proposal) {
                            return proposal.reference() == reference.value;
                        });
                        require(found != newRoots.end(), "cold demotion lost its immutable carrier holder");
                        const auto directory = EntityPropertyLargeValuePayloadCodec::decodeDirectory(found->payload(), &admit, cancellation);
                        const auto data = found->payload().subspan(RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES);
                        for (size_t i = 0; i < removedTokens.size(); ++i) {
                            const auto entry = std::lower_bound(directory.begin(), directory.end(), removedTokens[i],
                                [](const auto &location, uint32_t token) { return location.key_token < token; });
                            if (entry == directory.end() || entry->key_token != removedTokens[i]) verifyValue(i, {});
                            else verifyValue(i, data.subspan(entry->offset + sizeof(uint32_t), entry->length - sizeof(uint32_t)));
                        }
                        boundary.set(0);
                    } else {
                        HybridPropertyColdRowInspection inspected(context.snapshot_, row, owner, batch.current().generation,
                            std::span(matrix.tokens).first(matrix.column_count), memory, values, cancellation);
                        inspected.visitSelected(removedTokens, verifyValue);
                    }
                }
            }
''' +s[b:];p.write_text(s)
