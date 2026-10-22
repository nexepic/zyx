#include <iostream>
#include <chrono>
#include <array>
#include <map>
#include "graph/core/Node.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/expressions/Expression.hpp"
using namespace graph; using namespace graph::storage; using namespace graph::query; using namespace graph::query::execution;
constexpr size_t N=100000;
struct Input {
 std::vector<std::vector<std::byte>> rows;
 struct Block { uint64_t first=0; std::array<uint64_t,64> words{}; size_t size=0; };
 std::vector<Block> blocks;
 Input() {
  const std::array<std::string_view,8> countries{"CN","US","DE","JP","SG","BR","IN","FR"};
  rows.reserve(N); blocks.resize((N+63)/64);
  for(size_t i=0;i<N;++i) {
   const auto text=countries[i%8]; PropertyCollectionRoot root{i+1,Node::typeId,{{1,PropertyValue(std::string(text))},
    {2,PropertyValue(int64_t(i%90))},{3,PropertyValue(double(i%1000))},{4,PropertyValue(std::string(32,'x'))}}};
   rows.push_back(EntityPropertyCollectionRootCodec::encode(root));
   auto &block=blocks[i/64]; block.first=(i/64)*64+1; ++block.size;
   block.words[i%64]=uint64_t(text.size())<<56;
   for(size_t j=0;j<text.size();++j) block.words[i%64]|=uint64_t(uint8_t(text[j]))<<(j*8);
  }
 }
};
struct Snapshot final: LogicalGraphReadSnapshot {
 const Input &input; bool compact;
 Snapshot(const Input&i,bool c):input(i),compact(c){}
 bool hasImmutableReadIdentity() const noexcept override{return true;}
 bool supportsNativeNodeRefScans()const override{return true;}
 bool supportsNodePropertyCellBatches()const override{return true;}
 void visitNodeRefs(size_t maximum,const std::function<bool(const EntityRef&)> &visitor) const override {
  for(size_t i=0;i<std::min(maximum,N);++i)if(!visitor({i+1,false,0,0,0}))break;
 }
 void visitNodeRefsWithLabelMatches(size_t maximum,const std::vector<std::string>&labels,const std::function<bool(const EntityRef&,bool)>&visitor)const override {
  if(!labels.empty())throw std::runtime_error("unexpected labels");
  visitNodeRefs(maximum,[&](const EntityRef&r){return visitor(r,true);});
 }
 void readNodePropertyCellsWithViewsSortedWithAdmission(std::span<const uint64_t> ids,std::span<const std::string_view> keys,
  const PropertyCellSink&,const PropertyScalarReadSink&,const PropertyTextReadSink &texts,const std::function<void(size_t)> &admission)const override {
  size_t validated=SIZE_MAX;
  for(size_t i=0;i<ids.size();++i) {
   const auto id=ids[i];if(!id||id>N||(i&&id<=ids[i-1]))throw std::runtime_error("bad owner");
   for(size_t k=0;k<keys.size();++k){if(keys[k]!="country")throw std::runtime_error("bad key");
    if(!compact){
     const std::array<uint32_t,1> tokens{1};
     EntityPropertyCollectionRootCodec::visitSelectedEncoded(input.rows[id-1],id,Node::typeId,tokens,[&](size_t,auto bytes){
      if(!bytes)throw std::runtime_error("missing");auto field=CanonicalPropertyValueCodec::tryBorrowPrimitive(*bytes);
      if(!field||field->type!=PropertyType::STRING)throw std::runtime_error("bad field");admission(bytes->size());texts(i,k,field->text);
     });
    }else {
     const size_t group=(id-1)/64;const auto &block=input.blocks[group];
     if(validated!=group){
      if(block.first!=group*64+1||!block.size||block.size>64)throw std::runtime_error("bad group");
      for(size_t j=0;j<block.size;++j){const auto w=block.words[j];const auto len=w>>56;
       if(len>7 || (len<7&&(w&0x00ffffffffffffffULL)>>(len*8)))throw std::runtime_error("noncanonical word");}
      validated=group;
     }
     if(id-block.first>=block.size)throw std::runtime_error("foreign ordinal");
     const auto word=block.words[id-block.first];const size_t len=word>>56;std::array<char,7> text{};
     for(size_t j=0;j<len;++j)text[j]=char(word>>(j*8));admission(10+len);texts(i,k,{text.data(),len});
    }
   }
  }
 }
 LogicalGraphNode readNode(uint64_t)const override{throw std::runtime_error("unexpected node materialization");}
 LogicalGraphEdge readEdge(uint64_t)const override{throw std::runtime_error("unexpected edge");}
 void visitNodes(size_t,const std::function<bool(const LogicalGraphNode&)>&)const override{throw std::runtime_error("unexpected scan");}
 void visitEdges(size_t,const std::function<bool(const LogicalGraphEdge&)>&)const override{}
 LogicalGraphNodePage scanNodesAfter(uint64_t,size_t,size_t)const override{return {};}
 LogicalGraphAdjacencyPage scanAdjacentEdges(uint64_t,LogicalGraphReadDirection,std::optional<LogicalGraphAdjacencyContinuation>,size_t)const override{return {};}
 void visitAdjacentEdges(uint64_t,LogicalGraphReadDirection,size_t,const std::function<bool(const LogicalGraphEdge&)>&)const override{}
};
struct Backend final:LogicalGraphReadBackend {
 const Input &input;bool compact;Backend(const Input&i,bool c):input(i),compact(c){}
 std::unique_ptr<LogicalGraphReadSnapshot>openLogicalGraphReadSnapshot()const override{return std::make_unique<Snapshot>(input,compact);}
};
int main(){try {
 Input input; LogicalPlanReadRuntimeBounds bounds;bounds.maximum_node_records=N+1;bounds.maximum_node_page_records=512;bounds.maximum_rows=N+1;bounds.maximum_metadata_pages_per_node_page=64;bounds.maximum_property_entries=16;bounds.maximum_distinct_rows=N+1;bounds.maximum_edges_per_node=1;bounds.maximum_edge_records=1;bounds.maximum_adjacency_page_records=1;
 for(bool compact:{false,true})for(bool grouped:{false,true}) {
  Backend backend(input,compact);LogicalPlanReadRuntime runtime(backend);
  auto scan=std::make_unique<logical::LogicalNodeScan>("n");
  if(!grouped)scan->setRangePredicates({{"country",PropertyValue("CN"),PropertyValue("CN"),true,true}});
  std::vector<std::shared_ptr<expressions::Expression>> groups;
  if(grouped)groups.push_back(std::make_shared<expressions::VariableReferenceExpression>("n","country"));
  logical::LogicalAggregate plan(std::move(scan),std::move(groups),{{"count",nullptr,"rows"}});
  for(int iteration=-1;iteration<3;++iteration){const auto start=std::chrono::steady_clock::now();
   const auto result=runtime.executeToQueryResult(plan,bounds);const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   if(result.rowCount()!=(grouped?8:1))throw std::runtime_error("wrong group cardinality");
   for(const auto &row:result.getRows())if(row.at("rows").asPrimitive()!=PropertyValue(int64_t(12500)))throw std::runtime_error("wrong count");
   if(iteration>=0)std::cout<<(compact?"compact":"row")<<"\t"<<(grouped?"group":"filter")<<"\t"<<iteration<<"\t"<<elapsed<<"\n";
  }
 }
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
