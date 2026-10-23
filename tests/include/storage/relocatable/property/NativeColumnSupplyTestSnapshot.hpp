#pragma once
// Test-only adapter: compares existing native ROW and lab column carriers through
// the same production executor. It grants no production layout/routing policy.
#include "graph/core/Node.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/NativeColumnImageStore.hpp"

namespace native_column_supply_test {
using namespace graph;
using namespace graph::storage;
using zyx::benchmark::NativeColumnImageStore;
using zyx::benchmark::TypedPropertyBlock;
struct Source {
 const MetadataSnapshotPin &pin;
 PropertyCollectionRootStore &rows;
 NativeColumnImageStore &columns;
 LargeValueRef root;
 uint64_t block;
 std::vector<uint64_t> owners;
 bool compact;
 mutable size_t column_visits=0, row_batches=0, supplied_cells=0;
};
class Snapshot final : public LogicalGraphReadSnapshot {
 Source &source_;
 mutable PropertyCollectionRootScanCursor row_cursor_;
public:
 explicit Snapshot(Source &source):source_(source),row_cursor_(source.rows.openScanCursor(source.pin)){}
 bool hasImmutableReadIdentity() const noexcept override {return true;}
 bool supportsNativeNodeRefScans() const override {return true;}
 bool supportsNodePropertyCellBatches() const override {return true;}
 void visitNodeRefs(size_t maximum,const std::function<bool(const EntityRef&)> &visitor) const override {
  for(size_t i=0;i<std::min(maximum,source_.owners.size());++i)
   if(!visitor({source_.owners[i],false,0,0,0}))break;
 }
 void visitNodeRefsWithLabelMatches(size_t maximum,const std::vector<std::string> &labels,
     const std::function<bool(const EntityRef&,bool)> &visitor) const override {
  if(!labels.empty())throw std::logic_error("label authority is outside this supply experiment");
  visitNodeRefs(maximum,[&](const EntityRef &ref){return visitor(ref,true);});
 }
 void readNodePropertyCellsWithViewsSortedWithAdmission(std::span<const uint64_t> ids,
     std::span<const std::string_view> keys,const PropertyCellSink &cells,
     const PropertyScalarReadSink &scalars,const PropertyTextReadSink &texts,
     const std::function<void(size_t)> &admission) const override {
  PropertyColumnBatchReadSinks runs;
  runs.scalars=[&](auto rows,size_t column,PropertyType type,auto bits){
   for(size_t i=0;i<rows.size();++i)scalars(rows[i],column,type,bits[i]);
  };
  runs.texts=[&](auto rows,size_t column,auto values){
   for(size_t i=0;i<rows.size();++i)texts(rows[i],column,values[i]);
  };
  if(!tryReadNodePropertyColumnBatchesSortedWithAdmission(ids,keys,cells,runs,admission))
   throw std::logic_error("unsupported experiment field");
 }
 bool tryReadNodePropertyColumnBatchesSortedWithAdmission(std::span<const uint64_t> ids,
     std::span<const std::string_view> keys,const PropertyCellSink &cells,
     const PropertyColumnBatchReadSinks &runs,const std::function<void(size_t)> &admission) const override {
  if(keys.size()!=1 || keys[0]!="country")return false;
  if(!std::ranges::is_sorted(ids) || std::ranges::adjacent_find(ids)!=ids.end())
   throw std::logic_error("strictly ordered native owners required");
  if(source_.compact) {
   ++source_.column_visits;
   source_.columns.visitSelectedColumn(source_.pin,source_.root,source_.block,Node::typeId,1U,[&](auto borrowed){
    std::array<const TypedPropertyBlock::BorrowedCell *,64> selectedCells{};
    size_t selected=0;
    for(const auto &cell:borrowed) {
     if(selected==ids.size())break;
     if(cell.owner<ids[selected])continue;
     if(cell.owner!=ids[selected])throw std::logic_error("column owner map missing request");
     selectedCells[selected++]=&cell;
    }
    if(selected!=ids.size())throw std::logic_error("column owner map missing request");
    std::array<size_t,64> ordinals{};std::array<std::string_view,64> texts{};std::array<uint64_t,64> words{};
    size_t first=0;
    while(first<selected) {
     const auto &cell=*selectedCells[first];
     if(!cell.present || !cell.primitive) {
      if(!cell.present)cells(first,0,std::nullopt);
      else {
       if(admission)admission(cell.complex.size());
       size_t consumed=0;auto value=CanonicalPropertyValueCodec::decodeNext(cell.complex,consumed);
       cells(first,0,std::move(value));
      }
      ++first;continue;
     }
     const auto type=cell.primitive->type;
     size_t count=0;
     while(first+count<selected) {
      const auto &next=*selectedCells[first+count];
      if(!next.present || !next.primitive || next.primitive->type!=type)break;
      const auto &value=*next.primitive;
      if(admission)admission(CanonicalPropertyValueCodec::HEADER_BYTES+
          (type==PropertyType::STRING?value.text.size():type==PropertyType::NULL_TYPE?0U:type==PropertyType::BOOLEAN?1U:8U));
      ordinals[count]=first+count;texts[count]=value.text;words[count]=value.bits;++count;
     }
     if(type==PropertyType::STRING)runs.texts(std::span(ordinals).first(count),0,std::span(texts).first(count));
     else runs.scalars(std::span(ordinals).first(count),0,type,std::span(words).first(count));
     first+=count;
    }
    source_.supplied_cells+=selected;
   });
  } else {
   ++source_.row_batches;
   std::vector<EntityPropertyKey> owners;owners.reserve(ids.size());
   for(auto id:ids)owners.push_back({EntityPropertyOwnerKind::EPOK_NODE,id});
   const std::array<uint32_t,1> tokens{1U};
   const std::function<void(size_t,size_t,std::optional<PropertyCollectionRootEntry>)> owning=
       [&](size_t row,size_t col,auto entry){cells(row,col,entry?std::optional<PropertyValue>(std::move(entry->value)):std::nullopt);};
   const PropertyScalarReadSink scalar=[&](size_t row,size_t col,PropertyType type,uint64_t bits){
    const std::array ordinals{row};const std::array words{bits};runs.scalars(ordinals,col,type,words);
   };
   const PropertyTextReadSink text=[&](size_t row,size_t col,std::string_view value){
    const std::array ordinals{row};const std::array values{value};runs.texts(ordinals,col,values);
   };
   row_cursor_.readEntityPropertiesOptionalSorted(source_.pin,owners,tokens,{},true,&admission,&owning,&scalar,&text,&runs);
   source_.supplied_cells+=ids.size();
  }
  return true;
 }
 LogicalGraphNode readNode(uint64_t) const override {throw std::logic_error("unexpected owning node");}
 LogicalGraphEdge readEdge(uint64_t) const override {throw std::logic_error("unexpected edge");}
 void visitNodes(size_t,const std::function<bool(const LogicalGraphNode&)>&) const override {throw std::logic_error("unexpected owning scan");}
 void visitEdges(size_t,const std::function<bool(const LogicalGraphEdge&)>&) const override {}
 LogicalGraphNodePage scanNodesAfter(uint64_t,size_t,size_t) const override {return {};}
 LogicalGraphAdjacencyPage scanAdjacentEdges(uint64_t,LogicalGraphReadDirection,std::optional<LogicalGraphAdjacencyContinuation>,size_t) const override {return {};}
 void visitAdjacentEdges(uint64_t,LogicalGraphReadDirection,size_t,const std::function<bool(const LogicalGraphEdge&)>&) const override {}
};
class Backend final : public LogicalGraphReadBackend {
 Source &source_;
public:
 explicit Backend(Source &source):source_(source){}
 std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {return std::make_unique<Snapshot>(source_);}
};
}
