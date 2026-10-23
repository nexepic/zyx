from pathlib import Path
root=Path('/workspace/zyx');p=root/'tests/src/storage/relocatable/property/test_HybridPropertyColdRowRouting.cpp';s=p.read_text();a=s.index('class HybridPropertyColdRowRoutingTest :');b=s.index('\n};',a)+len('\n};');body=s[a:b]
body=body.replace('protected:\n','''protected:
    static constexpr auto NODE = EntityPropertyOwnerKind::EPOK_NODE;
    EntityPropertyOwnerKind owner_kind_ = NODE;
    bool cold_only_ = false, split_cold_ = false, raw_cold_ = false;
    bool orphan_cold_ = false, orphan_matrix_ = false, wrong_cold_binding_ = false, wrong_cold_count_ = false;
    bool malformed_cold_value_ = false;
    PageId field_page_id_{};
    SlottedSlotReference cold_reference_{};
    LargeValueRef value_reference_{};
''',1)
needle='''        auto tx = planner.beginTransaction(); tx.reserveNodeIds(64);
        for (uint64_t id = 1; id <= 64; ++id) tx.createNodeWithReservedId(id, {});
        if (competing_row_) tx.replaceNodeProperty({1, graph::Node::typeId, {{7, PropertyValue("competing")}}});'''
assert needle in body
body=body.replace(needle,'''        const auto ownerType = owner_kind_ == NODE ? graph::Node::typeId : graph::Edge::typeId;
        auto tx = planner.beginTransaction();
        tx.reserveNodeIds(owner_kind_ == NODE ? 64 : 2);
        for (uint64_t id = 1; id <= (owner_kind_ == NODE ? 64U : 2U); ++id) tx.createNodeWithReservedId(id, {});
        if (owner_kind_ != NODE) {
            tx.reserveEdgeIds(64);
            for (uint64_t id = 1; id <= 64; ++id) tx.createEdgeWithReservedId(id, 1, 2, 7);
        }
        if (competing_row_) {
            PropertyCollectionRoot row{1, ownerType, {{7, PropertyValue("competing")}}};
            if (owner_kind_ == NODE) tx.replaceNodeProperty(std::move(row)); else tx.replaceEdgeProperty(std::move(row));
        }''',1)
# All subsequent canonical roots use the selected namespace.
first=body.index('        std::vector<std::byte> first;');body=body[:first]+body[first:].replace('graph::Node::typeId','ownerType')
body=body.replace('            first = EntityPropertyLargeValueReferenceCodec::encode({1, ownerType, ref});','            value_reference_ = ref;\n            first = EntityPropertyLargeValueReferenceCodec::encode({1, ownerType, ref});',1)
needle='''        const auto last = EntityPropertyCollectionRootCodec::encode''';body=body.replace(needle,'''        if (malformed_cold_value_ && !large) {
            size_t offset = 0; const std::array<uint32_t, 1> tokens{2};
            EntityPropertyCollectionRootCodec::visitSelectedEncoded(first, 1, ownerType, tokens,
                [&](size_t, auto bytes) { if (bytes) offset = size_t(bytes->data() - first.data()); });
            first[offset] = std::byte{0xff}; // canonical directory intact; selected value invalid
        }
'''+needle,1)
body=body.replace('        auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);','        field_page_id_ = pageId;\n        auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);',1)
body=body.replace('HybridPropertyGroupRecord group{NODE,','HybridPropertyGroupRecord group{owner_kind_,',1).replace('1, current.generation, UINT64_MAX, 51, 1};','1, current.generation, UINT64_MAX, uint32_t(cold_only_ ? 3 : 51), uint32_t(cold_only_ ? 0 : 1)};',1)
body=body.replace('column.identity = {NODE,','column.identity = {owner_kind_,',1)
body=body.replace('        const auto hot = image.appendColumn(column, group, current.generation);','        const auto hot = cold_only_ ? SlottedSlotReference{} : image.appendColumn(column, group, current.generation);',1)
a2=body.index('        auto encoded = HybridPropertyColdRowBlock::encode(');b2=body.index('        group.mode = HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER;',a2)
body=body[:a2]+'''        auto encoded = HybridPropertyColdRowBlock::encode(owner_kind_, 1, current.generation,
            split_cold_ ? std::span(rows).first(1) : std::span(rows), memory, !raw_cold_);
        const auto cold = image.appendColdRows(encoded, owner_kind_, 1); cold_reference_ = cold;
        auto lastCold = cold;
        if (split_cold_) {
            const auto bytes = HybridPropertyColdRowBlock::encode(owner_kind_, 1, current.generation,
                std::span(rows).last(1), memory, !raw_cold_);
            lastCold = image.appendColdRows(bytes, owner_kind_, 1);
        }
        if (orphan_cold_) static_cast<void>(image.appendColdRows(encoded, owner_kind_, 1));
        HybridPropertyColumnMembership matrix;
        if (!cold_only_) { matrix = HybridPropertyColumnMembership::fromColumns(group, std::span(&column, 1)); matrix.locators[0] = hot; }
        else { matrix.owner_kind = owner_kind_; matrix.group_id = 1; matrix.semantic_generation = current.generation; matrix.live = group.live; }
        matrix.row_remainder = true; matrix.cold_fields = wrong_cold_count_ ? 4 : 3;
        if (wrong_cold_count_) ++group.memberships;
        matrix.cold_rows.owners = UINT64_C(1) | (UINT64_C(1) << 63);
        matrix.cold_rows.roots[0] = cold; matrix.cold_rows.roots[63] = wrong_cold_binding_ ? hot : lastCold;
        group.column_membership = image.appendColumnMembership(matrix, group, current.generation);
'''+body[b2:]
body=body.replace('        const auto groupSlot = image.appendGroup(group);','        if (orphan_matrix_) static_cast<void>(image.appendColumnMembership(matrix, group, current.generation));\n        const auto groupSlot = image.appendGroup(group);',1)
# Candidate descriptor roots are namespace-relative; the physical tree uses the actual ordinal.
body=body.replace('            const auto index = hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(role));','            const auto ordinal = role + (owner_kind_ == NODE ? 0U : 4U);\n            const auto index = hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(ordinal));',1)
body=body.replace('HybridPropertyDirectoryCodec::ownerKey({NODE, 64}, 1)','HybridPropertyDirectoryCodec::ownerKey({owner_kind_, 64}, 1)',1).replace('descriptor_.roots[role] =','descriptor_.roots[ordinal] =',1)
inc='''#pragma once
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/property/HybridPropertyValueReader.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"
#include "storage/relocatable/property/HybridPropertyReadContextTestAccess.hpp"
#include "storage/relocatable/property/HybridPropertyDescriptorStoreTestAccess.hpp"
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
namespace graph::storage::test {
using graph::query::execution::MaterializedByteBudget;
'''
(root/'tests/include/storage/relocatable/property/HybridPropertyColdRowRoutingTestFixture.hpp').write_text(inc+body+'\n}\n#endif\n')
s=s[:a]+'using graph::storage::test::HybridPropertyColdRowRoutingTest;'+s[b:]
s=s.replace('#include <gtest/gtest.h>','#include <gtest/gtest.h>\n#include "storage/relocatable/property/HybridPropertyColdRowRoutingTestFixture.hpp"',1)
# Ownership is now integrated in the read-only auditor. Keep the mutation guard assertion.
needle='    EXPECT_THROW(HybridPropertyDirectoryIntegrity::verify(context, pin, memory), FormatException);'
assert s.count(needle)==1;s=s.replace(needle,'    EXPECT_EQ(HybridPropertyDirectoryIntegrity::verify(context, pin, memory).memberships, 51U);',1)
p.write_text(s)
