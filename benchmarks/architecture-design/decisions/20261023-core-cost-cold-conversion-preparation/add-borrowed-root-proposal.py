from pathlib import Path
p=Path('include/graph/storage/relocatable/property/HybridPropertyLargeFieldPreparation.hpp');s=p.read_text();idx=s.index('    [[nodiscard]] bool reused()');s=s[:idx]+'''    // Same owner-neutral carrier and same private-batch proposal as prepareRoot,
    // borrowing a filtered entry set rather than cloning the owning ROW tree.
    [[nodiscard]] static HybridPropertyLargeFieldPreparation prepareRootBorrowed(RelocatableMetadataStore &metadata,
        const MetadataSnapshotPin &snapshot, LargeValueStore &store, EntityPropertyKey owner,
        std::span<const BorrowedPropertyCollectionRootEntry> entries, LargeValueId proposedId,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation = nullptr) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native borrowed ROW preparation is disabled");
        hybrid_property_detail::require(owner.isValid() && !entries.empty(), "native borrowed ROW requires its logical owner and fields");
        return prepareEncoded(metadata, snapshot, store, owner, 0, proposedId, std::move(memory), {},
            LargeValueExtentClass::LVEC_32K, RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES, cancellation,
            [&](std::pmr::memory_resource &resource) {
                return EntityPropertyLargeValuePayloadCodec::encodeBorrowed(entries, resource,
                    [&] { if (cancellation) cancellation->check(); });
            });
    }
''' +s[idx:];p.write_text(s)
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyConversionPreparation.cpp');s=p.read_text();a=s.index('namespace graph::storage {');b=s.index('namespace {',a);access=s[a:b];access=access.replace('''    static auto prepareCold(MetadataWriteBatch &batch, const HybridPropertyGroupConversion &plan,
        const std::function<void()> &checkpoint = {}) {''','''    using WholeRootResolver = HybridPropertyConversionPreparation::WholeRootResolver;
    static auto prepareCold(MetadataWriteBatch &batch, const HybridPropertyGroupConversion &plan,
        const std::function<void()> &checkpoint = {}, const WholeRootResolver &wholeRoot = {}) {''',1).replace('batch, plan, checkpoint, {}, true, true));','batch, plan, checkpoint, {}, true, true, wholeRoot));',1)
header=Path('tests/include/storage/relocatable/property/HybridPropertyConversionPreparationTestAccess.hpp');header.write_text('#pragma once\n#include "graph/storage/relocatable/metadata/HybridPropertyConversionPreparation.hpp"\n'+access);s=s[:a]+'#include "storage/relocatable/property/HybridPropertyConversionPreparationTestAccess.hpp"\n'+s[b:];p.write_text(s)
p=Path('tests/src/storage/relocatable/property/test_HybridPropertyLargeFieldPreparation.cpp');s=p.read_text();s=s.replace('#include <gtest/gtest.h>','#include <gtest/gtest.h>\n#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"\n#include "storage/relocatable/property/HybridPropertyConversionPreparationTestAccess.hpp"\n#include "graph/storage/relocatable/property/HybridPropertyFieldView.hpp"',1)
s+='''
TEST_F(HybridPropertyLargeFieldPreparationTest, BorrowedWholeRootProposalAndColdBindingReuseActualManifestWithoutPublishing) {
    LargeValueStore store(io_, *metadata_);
    PropertyCollectionRootStore roots(*slotted_pages_, store, *metadata_);
    const EntityPropertyKey owner{EntityPropertyOwnerKind::EPOK_NODE, 1};
    PropertyCollectionRoot row{1, graph::Node::typeId, {{2, PropertyValue("user-000000")}, {4, PropertyValue(std::string(8193, 'x'))}}};
    auto memory = std::make_shared<MaterializedByteBudget>(1024 * 1024);
    const std::array<BorrowedPropertyCollectionRootEntry, 2> entries{{{2, &row.entries[0].value}, {4, &row.entries[1].value}}};
    if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED) {
        const auto pin = metadata_->openSnapshot();
        EXPECT_THROW(HybridPropertyLargeFieldPreparation::prepareRootBorrowed(*metadata_, pin, store, owner, entries,
            LargeValueId(pin.superblock().next_large_value_id), memory), FormatException);
        EXPECT_EQ(memory->used(), 0U); return;
    }
    NativeGraphWritePlannerOptions options; options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(80);
    options.entity_pool_owner_id = 901; options.logical_id_lease_block_size = 1;
    options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(80), 902};
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, roots, *btree_pages_, options);
    auto tx = planner.beginTransaction(); tx.reserveNodeIds(1); tx.createNodeWithReservedId(1, {}); tx.replaceNodeProperty(row);
    ASSERT_TRUE(tx.commit().committed());
    static_cast<void>(store.contentIndex());
    const auto pin = metadata_->openSnapshot(); const auto source = pin.superblock();
    ASSERT_EQ(roots.readEntity(pin, owner), std::optional(row));
    const auto size = std::filesystem::file_size(path_); const auto epoch = io_->dataMutationEpoch();
    {
        const auto proposal = HybridPropertyLargeFieldPreparation::prepareRootBorrowed(*metadata_, pin, store, owner, entries,
            LargeValueId(source.next_large_value_id), memory);
        EXPECT_TRUE(proposal.reused()); EXPECT_EQ(proposal.token(), 0U); EXPECT_EQ(proposal.owner(), owner);
        EXPECT_EQ(proposal.newDataPages(), 0U); EXPECT_EQ(proposal.newManifestPages(), 0U);
        const auto actual = store.manifest(pin, proposal.reference()); EXPECT_EQ(actual.reference(), proposal.reference());
        EXPECT_TRUE(std::ranges::equal(proposal.payload(), EntityPropertyLargeValuePayloadCodec::encode(row.entries)));
        class RejectPhysicalAllocation final : public MetadataPageAllocator {
            MetadataPageId allocateMetadataPageId() override { throw std::logic_error("private root allocated physical metadata"); }
            MetadataPageAllocation allocatePage(Generation, LogSequenceNumber, std::span<const PhysicalSlotId>) override {
                throw std::logic_error("private root allocated a physical page");
            }
            void abandonPage(PhysicalPageRef) noexcept override {}
        } allocator;
        MetadataWriteBatch batch(allocator, source, LogSequenceNumber(source.checkpoint_lsn.value() + 1));
        const std::array<PropertyCollectionRoot, 1> sourceRows{row};
        HybridPropertyGroupConversion plan(memory, sourceRows, owner.owner_kind, 1, batch.generation(), RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES);
        size_t resolverCalls = 0;
        const HybridPropertyConversionPreparationTestAccess::WholeRootResolver resolver = [&](EntityPropertyKey requested, auto fields) {
            EXPECT_EQ(requested, owner); EXPECT_EQ(fields.size(), 2U); ++resolverCalls;
            const auto bytes = EntityPropertyLargeValueReferenceCodec::encode({owner.entity_id, graph::Node::typeId, proposal.reference()});
            return std::pmr::vector<std::byte>(bytes.begin(), bytes.end(), memory.get());
        };
        const auto prepared = HybridPropertyConversionPreparationTestAccess::prepareCold(batch, plan, {}, resolver);
        const auto groupRef = HybridPropertyDirectoryCodec::decodeLocator(prepared->group().value);
        const auto image = [&](PageId id) -> const HybridPropertyPageImage & {
            for (const auto &page : prepared->pages()) if (page.pageId() == id) return page;
            throw std::logic_error("missing prepared root page");
        };
        const auto group = image(groupRef.page_id).readGroup(groupRef, owner.owner_kind, 1, batch.generation());
        const auto matrix = image(groupRef.page_id).readColumnMembership(group.column_membership, group, batch.generation());
        const auto ref = matrix.cold_rows.roots[0]; const auto bytes = image(ref.page_id).readRecord(ref);
        const auto block = HybridPropertyFieldView::decodeColdRows(bytes, ref, group, matrix, batch.generation(), batch.generation(), memory);
        const auto cold = block.row(0); ASSERT_TRUE(cold); EXPECT_TRUE(cold->large_value_reference); EXPECT_EQ(cold->field_count, 2U);
        const auto bound = EntityPropertyLargeValueReferenceCodec::decode(cold->root); EXPECT_EQ(bound.value, proposal.reference());
        EXPECT_EQ(store.manifest(pin, bound.value).reference(), bound.value); EXPECT_EQ(resolverCalls, 1U);
        EXPECT_TRUE(prepared->owners().empty()); EXPECT_TRUE(prepared->extensions().empty());
        EXPECT_EQ(batch.current(), source); EXPECT_EQ(batch.candidate().next_large_value_id, source.next_large_value_id);
    }
    EXPECT_EQ(metadata_->openSnapshot().superblock(), source); EXPECT_EQ(io_->dataMutationEpoch(), epoch);
    EXPECT_EQ(std::filesystem::file_size(path_), size); EXPECT_EQ(memory->used(), 0U);
}
''';p.write_text(s)
for profile in ['hybrid','default']:
 p=Path('/tmp/build-cold-preparation-'+profile+'-modules.py');s=p.read_text().replace("units=[", "units=['property/test_HybridPropertyLargeFieldPreparation', ");p.write_text(s)
