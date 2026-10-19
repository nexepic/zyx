#include <gtest/gtest.h>
#include "graph/storage/AdjacencyTopologyContract.hpp"

using namespace graph::storage;
namespace {
AdjacencyTopologyIdentity identity(){return {1,2,10,0,LogicalGraphReadDirection::LGRD_OUTGOING};}
TEST(AdjacencyTopologyContractTest, VerifiesParallelAndSelfLoopEdgesBeforeConsumersAndOwnsOutput) {
    std::array candidates{AdjacencyTopologyCandidate{1,20,3},AdjacencyTopologyCandidate{2,20,3},AdjacencyTopologyCandidate{3,10,0}};
    size_t lookups=0,checkpoints=0;
    auto batch=VerifiedAdjacencyTopologyBatch::verify(identity(),identity(),candidates,[&](uint64_t id){++lookups;return EntityRef{id,true,10,id==3?10U:20U,id==3?0U:3U};},[&]{++checkpoints;});
    EXPECT_EQ(lookups,3U); EXPECT_EQ(checkpoints,4U); candidates[0].neighbor_id=99;
    EXPECT_EQ(batch.identity(),identity());
    ASSERT_EQ(batch.entries().size(),3U); EXPECT_EQ(batch.entries()[0].target_node_id,20U);
    EXPECT_EQ(batch.entries()[1].entity_id,2U); EXPECT_EQ(batch.entries()[2].target_node_id,10U);
}
TEST(AdjacencyTopologyContractTest, RejectsWrongEndpointTypeOrOrphanEvenWhenConsumerWouldFilterIt) {
    const std::array candidates{AdjacencyTopologyCandidate{1,20,3}};
    for(auto master:{EntityRef{1,true,11,20,3},EntityRef{1,true,10,21,3},EntityRef{1,true,10,20,4},EntityRef{2,true,10,20,3},EntityRef{1,false,10,20,3},EntityRef{1,true,0,20,3}})
        EXPECT_THROW(VerifiedAdjacencyTopologyBatch::verify(identity(),identity(),candidates,[&](uint64_t){return master;},[]{}),std::runtime_error);
    EXPECT_THROW(VerifiedAdjacencyTopologyBatch::verify(identity(),identity(),candidates,[](uint64_t)->EntityRef{throw std::runtime_error("missing master");},[]{}),std::runtime_error);
}
TEST(AdjacencyTopologyContractTest, RejectsDuplicateOrderStaleIdentityAndContinuationBeforeMasterAccess) {
    size_t calls=0; auto lookup=[&](uint64_t id){++calls;return EntityRef{id,true,10,20,3};};
    std::array candidates{AdjacencyTopologyCandidate{1,20,3}};
    for(size_t field=0;field<5;++field){auto wrong=identity();if(field==0)++wrong.database_id;if(field==1)++wrong.snapshot_generation;if(field==2)++wrong.node_id;if(field==3)++wrong.after_edge_id;if(field==4)wrong.direction=LogicalGraphReadDirection::LGRD_INCOMING;
        EXPECT_THROW(VerifiedAdjacencyTopologyBatch::verify(wrong,identity(),candidates,lookup,[]{}),std::invalid_argument);}
    EXPECT_EQ(calls,0U);
    for(auto ids:{std::array<uint64_t,2>{1,1},std::array<uint64_t,2>{2,1}}){
        std::array bad{AdjacencyTopologyCandidate{ids[0],20,3},AdjacencyTopologyCandidate{ids[1],20,3}};
        EXPECT_THROW(VerifiedAdjacencyTopologyBatch::verify(identity(),identity(),bad,lookup,[]{}),std::runtime_error);}
    auto resumed=identity();resumed.after_edge_id=1;
    EXPECT_THROW(VerifiedAdjacencyTopologyBatch::verify(resumed,resumed,candidates,lookup,[]{}),std::runtime_error);
    resumed.expected_first_edge_id=2;
 const std::array skipped{AdjacencyTopologyCandidate{3,20,3}};
 EXPECT_THROW(VerifiedAdjacencyTopologyBatch::verify(resumed,resumed,skipped,lookup,[]{}),std::runtime_error);
 EXPECT_THROW(VerifiedAdjacencyTopologyBatch::verify(resumed,resumed,{},lookup,[]{}),std::runtime_error);
 const std::array exact{AdjacencyTopologyCandidate{2,20,3}};
 const auto resumedBatch=VerifiedAdjacencyTopologyBatch::verify(resumed,resumed,exact,lookup,[]{});
 EXPECT_EQ(resumedBatch.entries().size(),1U);
 std::array<AdjacencyTopologyCandidate,65> tooMany{};
    EXPECT_THROW(VerifiedAdjacencyTopologyBatch::verify(identity(),identity(),tooMany,lookup,[]{}),std::invalid_argument);
}
TEST(AdjacencyTopologyContractTest, IncomingAndCancellationPreserveDirectionAndNoPublishedPartialBatch) {
    auto incoming=identity();incoming.direction=LogicalGraphReadDirection::LGRD_INCOMING;
    const std::array candidates{AdjacencyTopologyCandidate{1,20,3}};
    auto verified=VerifiedAdjacencyTopologyBatch::verify(incoming,incoming,candidates,[](uint64_t id){return EntityRef{id,true,20,10,3};},[]{});
    EXPECT_EQ(verified.entries().front().source_node_id,20U);
    size_t calls=0;
    EXPECT_THROW(VerifiedAdjacencyTopologyBatch::verify(identity(),identity(),candidates,[&](uint64_t id){++calls;return EntityRef{id,true,10,20,3};},[]{throw std::runtime_error("cancel");}),std::runtime_error);
    EXPECT_EQ(calls,0U);
}
}

TEST(AdjacencyTopologyContractTest, LaterCorruptionOrFinalCancellationCannotPublishAnEarlierVerifiedPrefix) {
 std::array candidates{AdjacencyTopologyCandidate{1,20,3},AdjacencyTopologyCandidate{2,20,3}};
 std::optional<VerifiedAdjacencyTopologyBatch> published;
 size_t calls=0;
 EXPECT_THROW(published.emplace(VerifiedAdjacencyTopologyBatch::verify(identity(),identity(),candidates,
     [&](uint64_t id){++calls;return EntityRef{id,true,10,id==2 ? 99U:20U,3};},[]{})),std::runtime_error);
 EXPECT_EQ(calls,2U);EXPECT_FALSE(published);
 size_t checkpoints=0;
 EXPECT_THROW(published.emplace(VerifiedAdjacencyTopologyBatch::verify(identity(),identity(),candidates,
     [](uint64_t id){return EntityRef{id,true,10,20,3};},[&]{if(++checkpoints==3)throw std::runtime_error("cancel before sealing");})),std::runtime_error);
 EXPECT_FALSE(published);EXPECT_EQ(checkpoints,3U);
}

TEST(AdjacencyTopologyContractTest, EmptyTerminalAndFullBoundAreValidButUnknownIdentityFieldsReject) {
 auto lookup=[](uint64_t id){return EntityRef{id,true,10,20,0};};
 const auto empty=VerifiedAdjacencyTopologyBatch::verify(identity(),identity(),{},lookup,[]{});
 EXPECT_TRUE(empty.entries().empty());
 std::array<AdjacencyTopologyCandidate,64> full{};
 for(size_t i=0;i<full.size();++i)full[i]={i+1,20,0};
 const auto verified=VerifiedAdjacencyTopologyBatch::verify(identity(),identity(),full,lookup,[]{});
 ASSERT_EQ(verified.entries().size(),64U);EXPECT_EQ(verified.entries().back().entity_id,64U);
 for(size_t field=0;field<4;++field){auto bad=identity();if(field==0)bad.database_id=0;if(field==1)bad.snapshot_generation=0;if(field==2)bad.node_id=0;if(field==3)bad.direction=LogicalGraphReadDirection(255);
     EXPECT_THROW(VerifiedAdjacencyTopologyBatch::verify(bad,bad,{},lookup,[]{}),std::invalid_argument);}
}
