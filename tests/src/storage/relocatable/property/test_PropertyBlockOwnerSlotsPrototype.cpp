#include <gtest/gtest.h>
#include "benchmarks/graphdb-comparison/property-block-prototype/PropertyBlockOwnerSlots.hpp"
using zyx::benchmark::PropertyBlockOwnerSlots;
using graph::storage::LittleEndianCodec;
namespace {
void checksum(std::vector<std::byte> &bytes) {
 LittleEndianCodec::writeU32(bytes,24,0);
 LittleEndianCodec::writeU32(bytes,24,graph::utils::calculateCrc(bytes.data(),bytes.size()));
}
}
TEST(PropertyBlockOwnerSlotsPrototypeTest, DeletesAndReusesSlotWithoutChangingSiblingOrOldSnapshot) {
 PropertyBlockOwnerSlots map(11,1);auto first=map.insert(99),second=map.insert(7);const auto old=map;
 map.erase(first);EXPECT_THROW(static_cast<void>(map.resolve(first)),std::runtime_error);
 auto reused=map.insert(23);EXPECT_EQ(reused.slot,first.slot);EXPECT_EQ(reused.generation,first.generation+1);
 EXPECT_EQ(map.resolve(second),7);EXPECT_EQ(old.resolve(first),99);
 EXPECT_THROW(static_cast<void>(map.resolve(first)),std::runtime_error);
 auto restored=PropertyBlockOwnerSlots::decode(map.encode(),11,1);EXPECT_EQ(restored.resolve(reused),23);
 EXPECT_EQ(restored.resolve(second),7);
 const std::vector<graph::storage::PropertyCollectionRoot> roots{{7,1,{}},{23,1,{}}};restored.validateOwners(roots);
 EXPECT_THROW(restored.validateOwners(std::vector<graph::storage::PropertyCollectionRoot>{{7,1,{}}}),std::runtime_error);
 EXPECT_THROW(restored.validateOwners(std::vector<graph::storage::PropertyCollectionRoot>{{7,1,{}},{99,1,{}}}),std::runtime_error);
}
TEST(PropertyBlockOwnerSlotsPrototypeTest, RejectsForgedStaleForeignAndDuplicateRoutes) {
 PropertyBlockOwnerSlots map(11,1);const auto route=map.insert(7);
 EXPECT_THROW(static_cast<void>(map.insert(7)),std::runtime_error);
 EXPECT_THROW(static_cast<void>(map.insert(0)),std::runtime_error);
 for(int mode=0;mode<5;++mode) {auto bad=route;
  if(mode==0)bad.block++;if(mode==1)bad.type++;if(mode==2)bad.slot=64;if(mode==3)bad.generation++;if(mode==4)bad.owner=99;
  EXPECT_THROW(static_cast<void>(map.resolve(bad)),std::runtime_error);
  EXPECT_THROW(map.erase(bad),std::runtime_error);
 }
 EXPECT_EQ(map.resolve(route),7);
}
TEST(PropertyBlockOwnerSlotsPrototypeTest, BoundsCapacityAndDoesNotWrapExhaustedGeneration) {
 PropertyBlockOwnerSlots map(11,1);for(uint64_t i=1;i<=64;++i)map.insert(i);
 auto bytes=map.encode();LittleEndianCodec::writeU64(bytes,32,0);LittleEndianCodec::writeU64(bytes,40,UINT64_MAX);checksum(bytes);
 auto exhausted=PropertyBlockOwnerSlots::decode(bytes,11,1);
 EXPECT_THROW(static_cast<void>(exhausted.insert(65)),std::runtime_error);
 EXPECT_THROW(static_cast<void>(map.insert(65)),std::runtime_error);
 EXPECT_EQ(exhausted.encode(),bytes);
}
TEST(PropertyBlockOwnerSlotsPrototypeTest, RejectsCorruptionAndChecksumValidInvalidMappings) {
 PropertyBlockOwnerSlots map(11,1);map.insert(7);const auto good=map.encode();
 auto truncated=good;truncated.pop_back();EXPECT_THROW(static_cast<void>(PropertyBlockOwnerSlots::decode(truncated,11,1)),std::runtime_error);
 for(int mode=0;mode<6;++mode) {auto bad=good;
  if(mode==0)bad[32]^=std::byte{1};
  if(mode==1)LittleEndianCodec::writeU16(bad,4,2);
  if(mode==2)LittleEndianCodec::writeU32(bad,20,1);
  if(mode==3)LittleEndianCodec::writeU64(bad,40,0);
  if(mode==4){LittleEndianCodec::writeU64(bad,48,7);LittleEndianCodec::writeU64(bad,56,1);}
  if(mode==5)LittleEndianCodec::writeU64(bad,8,0);
  if(mode!=0)checksum(bad);
  EXPECT_THROW(static_cast<void>(PropertyBlockOwnerSlots::decode(bad,11,1)),std::runtime_error);
 }
}
