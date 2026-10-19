#include "../../../../../benchmarks/architecture-design/property-subblock/PropertySubblockFieldTransition.hpp"
#include <gtest/gtest.h>
#include "graph/core/PropertyTypes.hpp"

namespace {
using namespace zyx::benchmark::subblock;
Column source(unsigned occupancy=64,ScalarKind kind=ScalarKind::HPS_INT64) {
    Column out; out.identity={OwnerKind::HPK_EDGE,kind,1,9,5};
    out.live=UINT64_MAX; out.presence=occupancy==64 ? UINT64_MAX : (uint64_t{1}<<occupancy)-1;
    for(unsigned i=0;i<occupancy;++i) out.words[i]=kind==ScalarKind::HPS_BOOL ? i%2 : i+1;
    return out;
}
FieldTransition change(const Column &column,unsigned slot,FieldCell value,uint64_t newLive=UINT64_MAX) {
    return planFieldChange(column,slot,value,newLive,6,sizeof(FieldTransition),3968);
}
TEST(PropertySubblockFieldTransitionTest, NoopRetainsColumnAndSemanticGeneration) {
    for(auto kind:{ScalarKind::HPS_INT64,ScalarKind::HPS_DOUBLE,ScalarKind::HPS_BOOL}) {
        auto column=source(64,kind); const auto before=column;
        auto plan=change(column,7,cellAt(column,7));
        EXPECT_EQ(plan.action,FieldAction::HFT_NO_CHANGE);
        EXPECT_EQ(plan.column.identity.generation,5U); EXPECT_EQ(plan.column.words,column.words);
        EXPECT_EQ(column.words,before.words);
    }
}
TEST(PropertySubblockFieldTransitionTest, SetChangesExactlyOneWordAndAdvancesOnlyChangedColumn) {
    const auto column=source(); auto unrelated=source(); unrelated.identity.token=10;
    const auto otherBefore=unrelated; const auto plan=change(column,7,{CellKind::HFC_INT64,999});
    EXPECT_EQ(plan.action,FieldAction::HFT_REPLACE_COLUMN); EXPECT_EQ(plan.column.identity.generation,6U);
    for(unsigned i=0;i<64;++i) EXPECT_EQ(plan.column.words[i],i==7 ? 999U : i+1);
    EXPECT_EQ(column.words[7],8U); EXPECT_EQ(unrelated.words,otherBefore.words);
    EXPECT_EQ(unrelated.identity.generation,5U);
}
TEST(PropertySubblockFieldTransitionTest, NullRemainsPresentWhileRemoveDemotesAtFortySeven) {
    auto column=source(48);
    auto nullPlan=change(column,47,{CellKind::HFC_NULL,0});
    EXPECT_EQ(nullPlan.action,FieldAction::HFT_REPLACE_COLUMN);
    EXPECT_EQ(std::popcount(nullPlan.column.presence),48); EXPECT_EQ(nullPlan.column.words[47],0U);
    EXPECT_TRUE(nullPlan.column.nulls & (uint64_t{1}<<47));
    auto removed=change(column,47,{});
    EXPECT_EQ(removed.action,FieldAction::HFT_DEMOTE_FIELD);
    EXPECT_EQ(removed.extension_cells[47].kind,CellKind::HFC_MISSING);
    for(unsigned i=0;i<47;++i) EXPECT_EQ(removed.extension_cells[i],(FieldCell{CellKind::HFC_INT64,i+1}));
    for(unsigned i=48;i<64;++i) EXPECT_EQ(removed.extension_cells[i].kind,CellKind::HFC_MISSING);
}
TEST(PropertySubblockFieldTransitionTest, TypeChangeDemotesOneFieldWithoutNumericCoercionOrPayloadCopy) {
    const auto column=source();
    for(auto value:{FieldCell{CellKind::HFC_DOUBLE,UINT64_C(0x3ff0000000000000)},
        FieldCell{CellKind::HFC_BOOL,1},FieldCell{CellKind::HFC_EXTENSION,0}}) {
        auto plan=change(column,9,value); EXPECT_EQ(plan.action,FieldAction::HFT_DEMOTE_FIELD);
        EXPECT_THROW(plan.column.validate(),std::invalid_argument);
        EXPECT_EQ(plan.extension_cells[9],value);
        for(unsigned i=0;i<64;++i) if(i!=9) EXPECT_EQ(plan.extension_cells[i],cellAt(column,i));
    }
}
TEST(PropertySubblockFieldTransitionTest, RemovalAboveThresholdRetainsColumnAndZeroesPayload) {
    auto column=source(); auto plan=change(column,63,{});
    EXPECT_EQ(plan.action,FieldAction::HFT_REPLACE_COLUMN); EXPECT_EQ(plan.column.words[63],0U);
    EXPECT_FALSE(plan.column.presence & (uint64_t{1}<<63)); EXPECT_EQ(plan.column.live,UINT64_MAX);
    const auto oldNull=change(column,0,{CellKind::HFC_NULL,0});
    auto removed=planFieldChange(oldNull.column,0,{},UINT64_MAX,7,sizeof(FieldTransition),3968);
    EXPECT_EQ(removed.action,FieldAction::HFT_REPLACE_COLUMN);
    EXPECT_EQ(removed.column.words[0],0U); EXPECT_EQ(removed.column.nulls,0U);
}
TEST(PropertySubblockFieldTransitionTest, OwnerBirthDeathAreSingleSlotChangesAndPreserveOthers) {
    auto column=source(); column.live &= ~(uint64_t{1}<<63); column.presence=column.live; column.words[63]=0;
    auto born=change(column,63,{CellKind::HFC_INT64,128});
    EXPECT_EQ(born.action,FieldAction::HFT_REPLACE_COLUMN); EXPECT_EQ(born.column.live,UINT64_MAX);
    auto dead=change(source(),63,{},UINT64_MAX>>1);
    EXPECT_EQ(dead.action,FieldAction::HFT_REPLACE_COLUMN); EXPECT_EQ(dead.column.live,UINT64_MAX>>1);
    EXPECT_EQ(dead.column.presence,UINT64_MAX>>1);
    EXPECT_THROW(change(column,63,{CellKind::HFC_INT64,1},column.live),std::invalid_argument);
    EXPECT_THROW(change(column,63,{},column.live ^ 1),std::invalid_argument);
}
TEST(PropertySubblockFieldTransitionTest, NullAndMissingNoopsRemainDistinctAndNoPromotionIsProvided) {
    auto column=source(48); auto missing=change(column,63,{});
    EXPECT_EQ(missing.action,FieldAction::HFT_NO_CHANGE);
    auto insertedNull=change(column,63,{CellKind::HFC_NULL,0});
    EXPECT_EQ(insertedNull.action,FieldAction::HFT_REPLACE_COLUMN);
    auto nullNoop=change(insertedNull.column,63,{CellKind::HFC_NULL,0});
    EXPECT_EQ(nullNoop.action,FieldAction::HFT_NO_CHANGE);
    // No entry point accepts extension/ROW state and implicitly promotes it.
}
TEST(PropertySubblockFieldTransitionTest, BitsPreserveSignedZeroNaNPayloadAndBooleanWidth) {
    auto column=source(64,ScalarKind::HPS_DOUBLE); column.words[0]=UINT64_C(0x3ff0000000000000);
    auto negativeZero=change(column,0,{CellKind::HFC_DOUBLE,UINT64_C(0x8000000000000000)});
    EXPECT_EQ(negativeZero.action,FieldAction::HFT_REPLACE_COLUMN);
    auto nan=change(column,0,{CellKind::HFC_DOUBLE,UINT64_C(0x7ff8000000001234)});
    EXPECT_EQ(nan.column.words[0],UINT64_C(0x7ff8000000001234));
    auto same=planFieldChange(nan.column,0,{CellKind::HFC_DOUBLE,UINT64_C(0x7ff8000000001234)},
        UINT64_MAX,7,sizeof(FieldTransition),3968);
    EXPECT_EQ(same.action,FieldAction::HFT_REPLACE_COLUMN);
    auto zeroSource=column; zeroSource.words[0]=0;
    auto zero=change(zeroSource,0,{CellKind::HFC_DOUBLE,UINT64_C(0x8000000000000000)});
    EXPECT_EQ(zero.action,FieldAction::HFT_NO_CHANGE); EXPECT_EQ(zero.column.words[0],0U);
    EXPECT_THROW(change(source(64,ScalarKind::HPS_BOOL),0,{CellKind::HFC_BOOL,2}),std::invalid_argument);
}
TEST(PropertySubblockFieldTransitionTest, InvalidIdentityGenerationBoundsAndBudgetPublishNothing) {
    auto column=source(); const auto original=column;
    EXPECT_THROW(planFieldChange(column,0,{CellKind::HFC_INT64,999},UINT64_MAX,5,sizeof(FieldTransition),3968),std::invalid_argument);
    EXPECT_THROW(planFieldChange(column,0,{CellKind::HFC_INT64,999},UINT64_MAX,6,sizeof(FieldTransition)-1,3968),std::length_error);
    EXPECT_THROW(planFieldChange(column,0,{CellKind::HFC_INT64,999},UINT64_MAX,6,sizeof(FieldTransition),575),std::length_error);
    EXPECT_THROW(change(column,64,{}),std::invalid_argument);
    EXPECT_THROW(change(column,0,{CellKind(255),0}),std::invalid_argument);
    EXPECT_THROW(change(column,0,{CellKind::HFC_NULL,1}),std::invalid_argument);
    EXPECT_EQ(column.words,original.words); EXPECT_EQ(column.identity,original.identity);
}
}
namespace {
Descriptor descriptorForColumns() {
    Descriptor descriptor; descriptor.generation=12;
    for(size_t i=0;i<8;++i)descriptor.roots[i]={i+1,1}; return descriptor;
}
TEST(PropertySubblockFieldTransitionTest, UnchangedColumnMayPrecedeCurrentGroupAndDescriptorGeneration) {
    auto column=source(48); GroupHeader group{OwnerKind::HPK_EDGE,GroupMode::HPG_HYBRID,1,10,UINT64_MAX,48,1};
    const auto descriptor=descriptorForColumns();
    EXPECT_NO_THROW(validateColumnSnapshot(column,group,descriptor,1,15));
    auto wrong=column; wrong.identity.generation=11;
    EXPECT_THROW(validateColumnSnapshot(wrong,group,descriptor,1,15),std::invalid_argument);
    group.generation=13; EXPECT_THROW(validateColumnSnapshot(column,group,descriptor,1,15),std::invalid_argument);
}
TEST(PropertySubblockFieldTransitionTest, CurrentGroupLiveProtectsDeletedOwnersWithoutRewritingAbsentColumns) {
    auto column=source(48); GroupHeader group{OwnerKind::HPK_EDGE,GroupMode::HPG_HYBRID,1,10,(uint64_t{1}<<48)-1,48,1};
    const auto descriptor=descriptorForColumns();
    // Encoded live includes now-absent slots, but no value/selector is present.
    EXPECT_NE(column.live,group.live);
    EXPECT_NO_THROW(validateColumnSnapshot(column,group,descriptor,1,12));
    // A missing column slot can become live without touching this column.
    group.live |= uint64_t{1}<<63;
    EXPECT_NO_THROW(validateColumnSnapshot(column,group,descriptor,1,12));
    // A column value for a now-dead owner must be removed atomically.
    group.live &= ~uint64_t{1};
    EXPECT_THROW(validateColumnSnapshot(column,group,descriptor,1,12),std::invalid_argument);
}
TEST(PropertySubblockFieldTransitionTest, ColumnSnapshotRejectsIdentityRootsAndFutureDescriptor) {
    auto column=source(); auto descriptor=descriptorForColumns();
    GroupHeader group{OwnerKind::HPK_EDGE,GroupMode::HPG_HYBRID,1,10,UINT64_MAX,64,1};
    EXPECT_THROW(validateColumnSnapshot(column,group,descriptor,2,12),std::invalid_argument);
    EXPECT_THROW(validateColumnSnapshot(column,group,descriptor,1,11),std::invalid_argument);
    for(size_t root : {4,5,7}) {
        auto missing=descriptor; missing.roots[root]={};
        EXPECT_THROW(validateColumnSnapshot(column,group,missing,1,12),std::invalid_argument);
    }
    ++group.group; EXPECT_THROW(validateColumnSnapshot(column,group,descriptor,1,12),std::invalid_argument);
    group.group=1; group.kind=OwnerKind::HPK_NODE;
    EXPECT_THROW(validateColumnSnapshot(column,group,descriptor,1,12),std::invalid_argument);
}
}

namespace {
TEST(PropertySubblockFieldTransitionTest, NoopEqualityMatchesActualRowPropertyValueForAllPrimitivePairs) {
    const std::array cells{FieldCell{},FieldCell{CellKind::HFC_NULL,0},
        FieldCell{CellKind::HFC_INT64,0},FieldCell{CellKind::HFC_INT64,1},FieldCell{CellKind::HFC_INT64,UINT64_MAX},
        FieldCell{CellKind::HFC_DOUBLE,0},FieldCell{CellKind::HFC_DOUBLE,UINT64_C(0x8000000000000000)},
        FieldCell{CellKind::HFC_DOUBLE,UINT64_C(0x3ff0000000000000)},
        FieldCell{CellKind::HFC_DOUBLE,UINT64_C(0x7ff8000000001234)},
        FieldCell{CellKind::HFC_DOUBLE,UINT64_C(0x7ff0000000000000)},
        FieldCell{CellKind::HFC_BOOL,0},FieldCell{CellKind::HFC_BOOL,1}};
    const auto row=[](FieldCell cell)->std::optional<graph::PropertyValue> {
        switch(cell.kind) {
        case CellKind::HFC_MISSING:return {};
        case CellKind::HFC_NULL:return graph::PropertyValue(std::monostate{});
        case CellKind::HFC_INT64:return graph::PropertyValue(std::bit_cast<int64_t>(cell.bits));
        case CellKind::HFC_DOUBLE:return graph::PropertyValue(std::bit_cast<double>(cell.bits));
        case CellKind::HFC_BOOL:return graph::PropertyValue(cell.bits!=0);
        default:throw std::invalid_argument("opaque input has no scalar row conversion");
        }
    };
    for(const auto left:cells)for(const auto right:cells)
        EXPECT_EQ(sameCellValue(left,right),row(left)==row(right));
}
}
