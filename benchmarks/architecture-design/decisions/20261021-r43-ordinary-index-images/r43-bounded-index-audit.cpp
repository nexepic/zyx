#include <fstream>
#include <iostream>
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
int main(int argc,char **argv) {
 using namespace graph::storage;
 try {
  if(argc!=2)return 2;
  auto memory=std::make_shared<graph::query::execution::MaterializedByteBudget>(8U*1024U*1024U);
  graph::utils::OwnedAllocationScope scope(memory);
  auto file=std::make_shared<std::fstream>(argv[1],std::ios::in|std::ios::binary);
  if(!file->is_open())return 3;
  auto io=std::make_shared<StorageIO>(file,INVALID_FILE_HANDLE,INVALID_FILE_HANDLE);
  auto metadata=RelocatableMetadataStore::openReadOnly(io);auto pin=metadata->openSnapshot();
  SlottedPagePool slots(io,*metadata,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
  SharedPagePoolAllocator pools(io,*metadata,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
  EntityPageStore entities(io,*metadata,SharedPagePoolOpenMode::SPPOM_READ_ONLY);
  BTreePageStore pages(slots,*metadata);SystemCatalog catalog(pages,*metadata);
  const auto counts=catalog.readGraphCardinalityStatistics(pin);
  if(!counts)throw std::runtime_error("missing graph counts");
  uint64_t routes=0,nodes=0,edges=0,typed=0,adjacency=0,members=0;
  metadata->visitPageLocationsUntil(pin,[&](const PageLocation &location){
   const auto page=pools.read(pin,location.page_id);
   if(page.location!=location || page.owner.page_id!=location.page_id)throw std::runtime_error("route/owner mismatch");
   ++routes;return true;
  });
  const auto definitions=catalog.readIndexDefinitions(pin,65536U);
  uint64_t memberId=0;
  for(const auto &definition:definitions)if(definition.kind==IndexDefinitionKind::IDK_LABEL && definition.entity_kind==IndexDefinitionEntityKind::IDEK_EDGE){
   if(memberId)throw std::runtime_error("audit fixture needs exactly one EDGE membership definition");memberId=definition.index_id;
  }
  if(!memberId)throw std::runtime_error("missing EDGE membership definition");
  BTreeIndex global(RSS_GLOBAL_ADJACENCY_INDEX_ID,pages,*metadata,BTreeLeafValueLayout::BLVL_EMPTY);
  BTreeIndex membership(memberId,pages,*metadata);
  metadata->visitNodePageLocations(pin,[&](const EntityPageLocation &location){static_cast<void>(entities.readNode(pin,location.entity_id));++nodes;});
  metadata->visitEdgePageLocations(pin,[&](const EntityPageLocation &location){
   const auto edge=entities.readEdge(pin,location.entity_id);++edges;
   static_cast<void>(entities.readNode(pin,edge.source_node_id));static_cast<void>(entities.readNode(pin,edge.target_node_id));
   if(!global.find(pin,AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT,edge.source_node_id,location.entity_id})) ||
      !global.find(pin,AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_IN,edge.target_node_id,location.entity_id})))
    throw std::runtime_error("missing primary Edge direction");
   if(edge.type_id){
    const auto found=membership.find(pin,IndexKeyCodec::encodeEntryKey(graph::PropertyValue(int64_t(edge.type_id)),location.entity_id));
    if(!found || *found!=location.entity_id)throw std::runtime_error("missing primary Edge type membership");++typed;
   }
  });
  auto directions=global.orderedSource(pin,20000000U)();
  while(auto entry=directions()){
   const auto key=AdjacencyKeyCodec::decode(entry->key);const auto edge=entities.readEdge(pin,key.edge_id);
   if(key.node_id!=(key.direction==GlobalAdjacencyDirection::GAD_OUT?edge.source_node_id:edge.target_node_id))
    throw std::runtime_error("orphan/wrong endpoint adjacency");++adjacency;
  }
  directions={};auto types=membership.orderedSource(pin,20000000U)();
  while(auto entry=types()){
   const auto id=IndexKeyCodec::decodeEntryEntityId(entry->key);const auto edge=entities.readEdge(pin,id);
   if(id!=entry->value_or_child || !edge.type_id || uint64_t(IndexKeyCodec::decodeIntegerEntryValue(entry->key))!=edge.type_id)
    throw std::runtime_error("orphan/wrong type membership");++members;
  }
  if(nodes!=counts->node_count || edges!=counts->edge_count || adjacency!=2*edges || members!=typed)
   throw std::runtime_error("independent graph/index totals mismatch");
  std::cout<<"{\"timing_benchmark\":false,\"physical_read_only\":true,\"workspace_bytes\":8388608,\"page_routes\":"<<routes
   <<",\"nodes\":"<<nodes<<",\"edges\":"<<edges<<",\"directions\":"<<adjacency<<",\"type_members\":"<<members<<"}\n";return 0;
 }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
