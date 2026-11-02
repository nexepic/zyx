#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/StorageIO.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>
using namespace graph;using namespace graph::storage;
int main(int argc,char**argv){
 if(argc!=2||std::filesystem::exists(argv[1]))return 2;
 try{
  std::filesystem::path p=argv[1];RelocatableStorageBackendOptions o;o.data_path=p;o.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;
  auto b=RelocatableStorageBackend::initializeNew(std::move(o));b->open();
  std::vector<LogicalGraphNodeCreateRequest> req(32,{{},{{"rank",PropertyValue(1)}}});auto ids=b->createLogicalNodes(req);
  if(ids.size()!=32)throw std::runtime_error("seed");
  auto count=[&](){auto f=std::make_shared<std::fstream>(p,std::ios::in|std::ios::binary);auto io=std::make_shared<StorageIO>(f,INVALID_FILE_HANDLE,INVALID_FILE_HANDLE);auto m=RelocatableMetadataStore::openReadOnly(io);auto pin=m->openSnapshot();size_t n=0;for(auto id:ids)n+=m->findEntityPropertyRevision(pin,{EntityPropertyOwnerKind::EPOK_NODE,id}).has_value();return n;};
  auto pin=b->openLogicalGraphReadSnapshot();std::vector<LogicalGraphNodeDeleteRequest> del;for(auto id:ids)del.push_back({id,false,0U});b->deleteLogicalNodes(del);
  size_t held=count();for(auto id:ids){auto n=pin->readNodeShaped(id,{.all_properties=true});if(n.properties.size()!=1||n.properties[0].value!=PropertyValue(1))throw std::runtime_error("old pin");}
  auto heldBytes=std::filesystem::file_size(p);if(held!=32)throw std::runtime_error("pinned tombstones lost");pin.reset();
  size_t remaining=held;int used=0;
  for(int i=0;i<8 && remaining;++i){auto trigger=b->createLogicalNodes({{{},{}}}).front();b->deleteLogicalNodes({{trigger,false,0U}});remaining=count();++used;std::cout<<"sweep="<<used<<" old_owner_records="<<remaining<<" file_bytes="<<std::filesystem::file_size(p)<<'\n';}
  auto fresh=b->openLogicalGraphReadSnapshot();for(auto id:ids){bool deleted=false;try{static_cast<void>(fresh->readNode(id));}catch(const FormatException&){deleted=true;}if(!deleted)throw std::runtime_error("deleted visible");}fresh.reset();static_cast<void>(b->verifyIntegrity());b->close();
  std::cout<<"pinned=32 held_records="<<held<<" held_file_bytes="<<heldBytes<<" after_records="<<remaining<<" sweeps="<<used<<" final_file_bytes="<<std::filesystem::file_size(p)<<'\n';return remaining?1:0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
