#pragma once
// Lab owner routing: logical slot identities are independent of sorted column row ordinals.
#include "TypedPropertyBlock.hpp"
#include <limits>
namespace zyx::benchmark {
class PropertyBlockOwnerSlots final {
public:
 struct Route { uint64_t block; uint32_t type; uint32_t slot; uint64_t generation; uint64_t owner;
  bool operator==(const Route &) const = default;
 };
 static constexpr size_t CAPACITY=64,HEADER=32,WIRE_BYTES=HEADER+CAPACITY*16;
 explicit PropertyBlockOwnerSlots(uint64_t block,uint32_t type):block_(block),type_(type) {
  require(block!=0,"invalid owner slot block");
 }
 Route insert(uint64_t owner) {
  require(owner!=0,"invalid owner slot identity");
  for(const auto &slot:slots_) require(slot.owner!=owner,"duplicate owner slot identity");
  for(size_t i=0;i<CAPACITY;++i) if(slots_[i].owner==0 && slots_[i].generation!=UINT64_MAX) {
   auto &slot=slots_[i];++slot.generation;slot.owner=owner;return {block_,type_,static_cast<uint32_t>(i),slot.generation,owner};
  }
  throw std::runtime_error("property owner slots exhausted");
 }
 Route route(uint64_t owner) const {
  require(owner!=0,"invalid property owner lookup");
  for(size_t i=0;i<CAPACITY;++i) if(slots_[i].owner==owner)
   return {block_,type_,static_cast<uint32_t>(i),slots_[i].generation,owner};
  throw std::runtime_error("property owner absent");
 }
 PropertyBlockOwnerSlots reconcile(std::span<const PropertyCollectionRoot> owners) const {
  require(owners.size()<=CAPACITY,"property owner capacity exceeded");
  std::set<uint64_t> wanted;uint64_t previous=0;
  for(const auto &owner:owners) {require(owner.owner_entity_type==type_ && owner.owner_entity_id>previous,"invalid property owner order");
   wanted.insert(owner.owner_entity_id);previous=owner.owner_entity_id;}
  auto result=*this;
  for(auto &slot:result.slots_) if(slot.owner && !wanted.contains(slot.owner))slot.owner=0;
  for(auto owner:wanted) {bool present=false;for(const auto &slot:result.slots_) if(slot.owner==owner)present=true;
   if(!present)result.insert(owner);}
  result.validateOwners(owners);return result;
 }
 uint64_t resolve(const Route &route) const {
  require(route.block==block_ && route.type==type_ && route.slot<CAPACITY && route.owner!=0,"invalid property owner route");
  const auto &slot=slots_[route.slot];
  require(slot.owner==route.owner && slot.generation==route.generation,"stale property owner route");
  return slot.owner;
 }
 void erase(const Route &route) {static_cast<void>(resolve(route));slots_[route.slot].owner=0;}
 std::vector<std::byte> encode() const {
  std::vector<std::byte> bytes(WIRE_BYTES);
  LittleEndianCodec::writeU32(bytes,0,0x5350545aU);LittleEndianCodec::writeU16(bytes,4,1);
  LittleEndianCodec::writeU16(bytes,6,CAPACITY);LittleEndianCodec::writeU64(bytes,8,block_);
  LittleEndianCodec::writeU32(bytes,16,type_);
  for(size_t i=0;i<CAPACITY;++i) {LittleEndianCodec::writeU64(bytes,HEADER+i*16,slots_[i].owner);
   LittleEndianCodec::writeU64(bytes,HEADER+i*16+8,slots_[i].generation);}
  LittleEndianCodec::writeU32(bytes,24,graph::utils::calculateCrc(bytes.data(),bytes.size()));return bytes;
 }
 static PropertyBlockOwnerSlots decode(std::span<const std::byte> bytes,uint64_t block,uint32_t type) {
  require(bytes.size()==WIRE_BYTES,"invalid property owner slot byte count");
  require(LittleEndianCodec::readU32(bytes,0)==0x5350545aU && LittleEndianCodec::readU16(bytes,4)==1 &&
   LittleEndianCodec::readU16(bytes,6)==CAPACITY && LittleEndianCodec::readU64(bytes,8)==block &&
   LittleEndianCodec::readU32(bytes,16)==type && LittleEndianCodec::readU32(bytes,20)==0 &&
   LittleEndianCodec::readU32(bytes,28)==0,"invalid property owner slot header");
  auto checked=std::vector<std::byte>(bytes.begin(),bytes.end());const auto crc=LittleEndianCodec::readU32(bytes,24);
  LittleEndianCodec::writeU32(checked,24,0);require(graph::utils::calculateCrc(checked.data(),checked.size())==crc,"invalid property owner slot checksum");
  PropertyBlockOwnerSlots result(block,type);std::set<uint64_t> owners;
  for(size_t i=0;i<CAPACITY;++i) {
   auto &slot=result.slots_[i];slot.owner=LittleEndianCodec::readU64(bytes,HEADER+i*16);
   slot.generation=LittleEndianCodec::readU64(bytes,HEADER+i*16+8);
   require(slot.owner==0 || (slot.generation!=0 && owners.insert(slot.owner).second),"invalid property owner slot mapping");
  }
  return result;
 }
 // Reject a route map that could bind a root to a different authoritative owner set.
 void validateOwners(std::span<const PropertyCollectionRoot> owners) const {
  std::set<uint64_t> active;for(const auto &slot:slots_) if(slot.owner)active.insert(slot.owner);
  require(active.size()==owners.size(),"property owner slot cardinality mismatch");
  uint64_t previous=0;for(const auto &owner:owners) {
   require(owner.owner_entity_type==type_ && owner.owner_entity_id>previous && active.contains(owner.owner_entity_id),"property owner slot root mismatch");
   previous=owner.owner_entity_id;
  }
 }
private:
 struct Slot {uint64_t owner=0,generation=0;};
 std::array<Slot,CAPACITY> slots_{};uint64_t block_;uint32_t type_;
 static void require(bool condition,const char *message) {if(!condition)throw std::runtime_error(message);}
};
}
