#include "TypedPropertyBlock.hpp"
#include <chrono>
#include <iostream>
#include <string>
using zyx::benchmark::TypedPropertyBlock;
using graph::storage::EntityPropertyCollectionRootCodec;
using graph::storage::PropertyCollectionRoot;
using graph::PropertyValue;
namespace {
using Clock=std::chrono::steady_clock;
template<class F> auto measure(F &&f) {
 auto start=Clock::now(); auto result=f();
 auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
 return std::pair{std::move(result),elapsed};
}
void verify(std::span<const PropertyCollectionRoot> actual, std::span<const PropertyCollectionRoot> expected, std::span<const uint32_t> tokens) {
 if(actual.size()!=expected.size()) throw std::runtime_error("prototype changed row count");
 for(size_t i=0;i<actual.size();++i) {
  PropertyCollectionRoot want=expected[i]; std::erase_if(want.entries,[&](const auto &e){return !std::binary_search(tokens.begin(),tokens.end(),e.key_token);});
  if(actual[i]!=want) throw std::runtime_error("prototype property result mismatch");
 }
}
}
int main(int argc,char **argv) {
 try {
  if(argc!=5) throw std::runtime_error("usage: owners fields payload_bytes selected_fields");
  const size_t count=std::stoull(argv[1]),fields=std::stoull(argv[2]),payload=std::stoull(argv[3]),selected=std::stoull(argv[4]);
  if(!count || count>4096 || !fields || fields>64 || payload>8192 || !selected || selected>fields) throw std::runtime_error("fixture bound");
  std::vector<PropertyCollectionRoot> owners;
  for(size_t row=0;row<count;++row) {
   PropertyCollectionRoot root{row*7+11,1U,{}};
   for(uint32_t token=1;token<=fields;++token) {
    if((row+token)%17==0) continue;
    PropertyValue value;
    if((row+token)%19!=0) {
     if(token==1) value=PropertyValue(static_cast<int64_t>(row*31+token));
     else {
      std::string text(payload,'a'); for(size_t i=0;i<text.size();++i) text[i]=static_cast<char>('a'+(row*13+token*7+i*11)%26);
      value=PropertyValue(std::move(text));
     }
    }
    root.entries.push_back({token,std::move(value)});
   }
   owners.push_back(std::move(root));
  }
  std::vector<uint32_t> tokens;for(uint32_t i=1;i<=selected;++i) tokens.push_back(i);
  auto canonical=measure([&]{std::vector<std::vector<std::byte>> roots;for(const auto &owner:owners) roots.push_back(EntityPropertyCollectionRootCodec::encode(owner));return roots;});
  auto blocks=measure([&]{std::vector<TypedPropertyBlock::Image> images;for(size_t i=0;i<count;i+=64) images.push_back(TypedPropertyBlock::encode(i/64+1,std::span(owners).subspan(i,std::min<size_t>(64,count-i))));return images;});
  size_t canonicalBytes=0,blockBytes=0;
  for(const auto &root:canonical.first) canonicalBytes+=root.size();
  for(const auto &block:blocks.first) blockBytes+=TypedPropertyBlock::byteSize(block);
  std::cout<<"{\"event\":\"fixture\",\"canonical_bytes\":"<<canonicalBytes<<",\"block_bytes\":"<<blockBytes<<",\"canonical_encode_ms\":"<<canonical.second<<",\"block_encode_ms\":"<<blocks.second<<"}\n";
  const auto readCanonical=[&]{std::vector<PropertyCollectionRoot> result;for(size_t i=0;i<count;++i) {
   PropertyCollectionRoot root{owners[i].owner_entity_id,1U,{}};
   for(auto &entry:EntityPropertyCollectionRootCodec::decodeSelected(canonical.first[i],tokens)) if(entry) root.entries.push_back(std::move(*entry));
   result.push_back(std::move(root));}return result;};
  const auto readBlocks=[&]{std::vector<PropertyCollectionRoot> result;for(size_t i=0;i<blocks.first.size();++i) {
   auto roots=TypedPropertyBlock::decode(blocks.first[i],i+1,1U,tokens);
   result.insert(result.end(),std::make_move_iterator(roots.begin()),std::make_move_iterator(roots.end()));}return result;};
  for(size_t iteration=0;iteration<13;++iteration) {
   for(size_t order=0;order<2;++order) {
    bool block=(iteration+order)%2==0;
    auto measured=block?measure(readBlocks):measure(readCanonical);
    verify(measured.first,owners,tokens);
    if(iteration>=3) std::cout<<"{\"event\":\"read\",\"version\":\""<<(block?"block":"canonical")<<"\",\"milliseconds\":"<<measured.second<<",\"checked_owners\":"<<count<<"}\n";
   }
  }
  // First proof construction is timed separately; steady updates reuse only this frozen image.
  auto certified=measure([&]{std::vector<TypedPropertyBlock::VerifiedImage> proofs;
   for(size_t i=0;i<blocks.first.size();++i) proofs.push_back(TypedPropertyBlock::verify(blocks.first[i],i+1,1U));return proofs;});
  std::cout<<"{\"event\":\"proof_build\",\"milliseconds\":"<<certified.second<<"}\n";
  // Update one owner's complete collection. Existing images remain authoritative old versions.
  const size_t target=count/2,blockIndex=target/64;
  auto replacement=owners[target];
  auto changed=std::lower_bound(replacement.entries.begin(),replacement.entries.end(),uint32_t{1},[](const auto &entry,uint32_t token){return entry.key_token<token;});
  if(changed!=replacement.entries.end() && changed->key_token==1U) changed->value=PropertyValue(int64_t(-7001));
  else replacement.entries.insert(changed,{1U,PropertyValue(int64_t(-7001))});
  for(size_t iteration=0;iteration<13;++iteration) {
   auto scalar=measure([&]{return EntityPropertyCollectionRootCodec::encode(replacement);});
   auto block=measure([&]{return TypedPropertyBlock::replace(blocks.first[blockIndex],blockIndex+1,1U,replacement.owner_entity_id,replacement.entries);});
   auto verified=measure([&]{return TypedPropertyBlock::replaceVerified(certified.first[blockIndex],blockIndex+1,1U,replacement.owner_entity_id,replacement.entries);});
   if(EntityPropertyCollectionRootCodec::decode(scalar.first)!=replacement) throw std::runtime_error("canonical update mismatch");
   auto expected=std::vector<PropertyCollectionRoot>(owners.begin()+blockIndex*64,owners.begin()+std::min(count,(blockIndex+1)*64));
   expected[target%64]=replacement;
   const auto keys=TypedPropertyBlock::tokens(block.first);verify(TypedPropertyBlock::decode(block.first,blockIndex+1,1U,keys),expected,keys);
   const auto verifiedKeys=TypedPropertyBlock::tokens(verified.first.image());
   verify(TypedPropertyBlock::decode(verified.first.image(),blockIndex+1,1U,verifiedKeys),expected,verifiedKeys);
   const auto oldKeys=TypedPropertyBlock::tokens(certified.first[blockIndex].image());
   verify(TypedPropertyBlock::decode(certified.first[blockIndex].image(),blockIndex+1,1U,oldKeys),std::span(owners).subspan(blockIndex*64,std::min<size_t>(64,count-blockIndex*64)),oldKeys);
   verify(readCanonical(),owners,tokens); verify(readBlocks(),owners,tokens);
   if(iteration>=3) {
    std::cout<<"{\"event\":\"update\",\"version\":\"canonical\",\"milliseconds\":"<<scalar.second<<",\"encoded_bytes\":"<<scalar.first.size()<<"}\n";
    std::cout<<"{\"event\":\"update\",\"version\":\"block\",\"milliseconds\":"<<block.second<<",\"encoded_bytes\":"<<TypedPropertyBlock::rewrittenBytes(blocks.first[blockIndex],block.first)<<"}\n";
    std::cout<<"{\"event\":\"update\",\"version\":\"verified_block\",\"milliseconds\":"<<verified.second<<",\"encoded_bytes\":"<<TypedPropertyBlock::rewrittenBytes(certified.first[blockIndex].image(),verified.first.image())<<"}\n";
   }
  }
 } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
}
