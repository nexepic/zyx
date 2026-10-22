// Shared observation/timing harness; compile against each frozen engine's own
// headers and archive. Engine source/libraries are never rebuilt by this probe.
#define main comparisonRunnerMain
#include "apps/compare_benchmark/main.cpp"
#undef main
#if R7_COHORT
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/maintenance/NativeStorageAllocationCensus.hpp"
#endif
using ProbeRows = std::vector<std::vector<zyx::Value>>;
static ProbeRows capture(zyx::Result result) {
 if(!result.isSuccess())throw std::runtime_error(result.getError());
 ProbeRows rows;
 while(result.hasNext()) {result.next();std::vector<zyx::Value> row;
  for(int col=0;col<result.getColumnCount();++col)row.push_back(result.get(col));
  rows.push_back(std::move(row));
 }
 return rows;
}
static void emitCell(const zyx::Value &value) {
 if(std::holds_alternative<std::monostate>(value))std::cout<<"[\"null\",null]";
 else if(const auto *v=std::get_if<int64_t>(&value))std::cout<<"[\"integer\","<<*v<<"]";
 else if(const auto *v=std::get_if<double>(&value))std::cout<<"[\"double\","<<std::setprecision(17)<<*v<<"]";
 else if(const auto *v=std::get_if<bool>(&value))std::cout<<"[\"boolean\","<<(*v?"true":"false")<<"]";
 else if(const auto *v=std::get_if<std::string>(&value))std::cout<<"[\"string\",\""<<jsonEscape(*v)<<"\"]";
 else if(const auto *v=std::get_if<std::shared_ptr<zyx::Node>>(&value)) {
  if(!*v)throw std::runtime_error("null owning Node");
  std::cout<<"[\"node\",{\"internal_id\":"<<(*v)->id<<",\"labels\":[";
  for(size_t i=0;i<(*v)->labels.size();++i){if(i)std::cout<<",";std::cout<<"\""<<jsonEscape((*v)->labels[i])<<"\"";}
  std::cout<<"],\"properties\":{";bool first=true;
  for(const auto &[key,cell]:(*v)->properties){if(!first)std::cout<<",";first=false;std::cout<<"\""<<jsonEscape(key)<<"\":";emitCell(cell);}
  std::cout<<"}}]";
 }else throw std::invalid_argument("probe permits scalar/owning Node result columns");
}
static void emitRows(const ProbeRows &rows) {
 std::cout<<"[";bool first=true;for(const auto &row:rows){if(!first)std::cout<<",";first=false;std::cout<<"[";
  for(size_t i=0;i<row.size();++i){if(i)std::cout<<",";emitCell(row[i]);}std::cout<<"]";
 }std::cout<<"]";
}
static std::string queryFor(std::string_view name) {
 if(name=="label_scan_filter")return "MATCH (u:User) WHERE u.country='CN' RETURN count(u)";
 if(name=="all_nodes_property_filter")return "MATCH (n) WHERE n.score>=900.0 RETURN count(n)";
 if(name=="label_multi_property_filter")return "MATCH (u:User) WHERE u.country='CN' AND u.age>=30 RETURN count(u)";
 if(name=="relationship_type_scan")return "MATCH ()-[r:FOLLOWS]->() RETURN count(r)";
 if(name=="relationship_property_filter")return "MATCH ()-[r:FOLLOWS]->() WHERE r.weight=1 RETURN count(r)";
 if(name=="one_hop_expand")return "MATCH (:User {id:'user-000001'})-[:FOLLOWS]->(v:User) RETURN count(v)";
 if(name=="two_hop_expand")return "MATCH (:User {id:'user-000001'})-[:FOLLOWS]->(:User)-[:FOLLOWS]->(v:User) RETURN count(v)";
 if(name=="aggregation_group_by")return "MATCH (u:User) RETURN count(DISTINCT u.country)";
 if(name=="aggregation_count_by_group")return "MATCH (u:User) RETURN u.country,count(*)";
 if(name=="topk_property_sort")return "MATCH (u:User) RETURN u.id ORDER BY u.score DESC LIMIT 100";
 if(name=="point_lookup_indexed")return "MATCH (u:User {id:'user-000001'}) RETURN count(u)";
 if(name=="point_lookup_missing")return "MATCH (u:User {id:'not-found'}) RETURN count(u)";
 if(name=="property_equality_indexed")return "MATCH (u:User) WHERE u.country='CN' RETURN count(u)";
 if(name=="property_range_indexed")return "MATCH (u:User) WHERE u.age>=30 AND u.age<40 RETURN count(u)";
 if(name=="point_node_fetch_by_id")return "MATCH (u:User {id:'user-000001'}) RETURN u.id,u.age,u.country,u.score";
 if(name=="point_edge_fetch_by_endpoints")return "MATCH (:User {id:'user-000001'})-[r:FOLLOWS]->(:User {id:'user-000006'}) RETURN r.weight";
 if(name=="batch_node_fetch_100")return "MATCH (u:User) RETURN u.id,u.age,u.country,u.score LIMIT 100";
 if(name=="one_hop_fetch_neighbor_ids")return "MATCH (:User {id:'user-000001'})-[:FOLLOWS]->(v:User) RETURN v.id";
 if(name=="one_hop_fetch_neighbor_records")return "MATCH (:User {id:'user-000001'})-[:FOLLOWS]->(v:User) RETURN v.id,v.age,v.country,v.score";
 if(name=="property_index_fetch_users_by_country")return "MATCH (u:User) WHERE u.country='CN' RETURN u.id,u.age,u.score LIMIT 100";
 if(name=="range_index_fetch_user_projection")return "MATCH (u:User) WHERE u.age>=30 AND u.age<40 RETURN u.id,u.age,u.country,u.score LIMIT 100";
 if(name=="relationship_property_fetch")return "MATCH ()-[r:FOLLOWS]->(v:User) WHERE r.weight=1 RETURN r.weight,v.id LIMIT 100";
 if(name=="graph_full_point")return "MATCH (u:User {id:'user-000001'}) RETURN u AS owner";
 if(name=="graph_full_neighbors")return "MATCH (:User {id:'user-000001'})-[:FOLLOWS]->(v:User) RETURN v AS owner ORDER BY v.id";
 if(name=="multi_key_group")return "MATCH (u:User) RETURN u.country AS country,u.age AS age,count(*) AS count ORDER BY country,age";
 if(name=="distinct_tuple")return "MATCH (u:User) RETURN count(DISTINCT [u.country,u.age]) AS count";
 if(name=="empty_count")return "MATCH (u:User) WHERE u.age<0 RETURN count(u) AS count";
 if(name=="missing_point")return "MATCH (u:User {id:'not-found'}) RETURN u";
 if(name=="large_selected")return "MATCH (o:Large) WHERE o.ordinal=1 RETURN o.ordinal AS value";
 if(name=="large_full")return "MATCH (o:Large) WHERE o.ordinal=1 RETURN o AS owner";
 if(name=="large_patch")return "MATCH (o:Large) WHERE o.ordinal=1 SET o.ordinal=-1 RETURN o.ordinal AS value";
 if(name=="skew_out")return "MATCH (:Hub {id:'h0'})-[:LINK]->(v:Hub) RETURN count(v) AS count";
 if(name=="skew_both")return "MATCH (:Hub {id:'h0'})-[:LINK]-(v:Hub) RETURN count(v) AS count";
 if(name=="skew_twohop")return "MATCH (:Hub {id:'h0'})-[:LINK]->(:Hub)-[:LINK]->(v:Hub) RETURN count(v) AS count";
 if(name=="edge_batch_100"||name=="edge_batch_1000"||name=="edge_batch_10000"||name=="steady_property")return {};
 if(name=="cohort_filter")return "MATCH (o:Owner) WHERE o.p1>256 RETURN count(o) AS count";
 if(name=="cohort_group")return "MATCH (o:Owner) RETURN o.p1 AS key,count(*) AS count ORDER BY key";
 if(name=="cohort_point")return "MATCH (o:Owner) WHERE o.p1=2 RETURN o.p64 AS value";
 if(name=="cohort_missing")return "MATCH (o:Owner) WHERE o.p1=0 RETURN o";
 if(name=="cohort_full_point")return "MATCH (o:Owner) WHERE o.p1=2 RETURN o AS owner";
 if(name=="cohort_full_scan")return "MATCH (o:Owner) RETURN o AS owner ORDER BY id(o)";
 if(name=="cohort_gather")return "UNWIND [2,0,2,513] AS probe OPTIONAL MATCH (o:Owner {p1:probe}) RETURN probe,o.p64 AS value";
 if(name=="property_set")return "MATCH (u:User {id:'user-000006'}) SET u.score=-1.0 RETURN u.score AS value";
 if(name=="property_noop")return "MATCH (u:User {id:'user-000006'}) SET u.score=u.score RETURN u.score AS value";
 if(name=="edge_create")return "MATCH (a:User {id:'user-000001'}),(b:User {id:'user-000002'}) CREATE (a)-[r:FOLLOWS {weight:-9999}]->(b) RETURN 1 AS value";
 if(name=="edge_create_delete")return {};
 if(name=="cohort_set")return "MATCH (o:Owner) WHERE o.p1=2 SET o.p64=777 RETURN o.p64 AS value";
 if(name=="cohort_noop")return "MATCH (o:Owner) WHERE o.p1=2 SET o.p64=o.p64 RETURN o.p64 AS value";
 if(name=="cohort_remove")return "MATCH (o:Owner) WHERE o.p1=2 REMOVE o.p64 RETURN o.p64 AS value";
 throw std::invalid_argument("unknown fixed probe case");
}
static void prepareRowFixtures(const std::filesystem::path &prefix) {
 const auto large=prefix.string()+"-large.zyx",skew=prefix.string()+"-skew.zyx";
 if(std::filesystem::exists(large)||std::filesystem::exists(skew))throw std::runtime_error("row fixtures require new paths");
 Options options;options.threads=1;
 {zyx::Database db(large);configureThreadPool(db,options);db.open();
  std::vector<zyx::PropertyColumn> columns{{"ordinal",{}},{"a",{}},{"b",{}},{"c",{}}};
  for(uint64_t owner=0;owner<16;++owner){columns[0].values.emplace_back(int64_t(owner));
   for(size_t field=1;field<=3;++field){uint64_t state=owner*1009+field*97+1;std::string text(8192,' ');
    for(char &ch:text){state^=state<<13;state^=state>>7;state^=state<<17;ch=char('!'+state%90);}columns[field].values.emplace_back(std::move(text));}}
  auto tx=db.beginTransaction();if(tx.createNodesColumnar("Large",16,columns).size()!=16)throw std::runtime_error("large fixture count");tx.commit();db.close();}
 {zyx::Database db(skew);configureThreadPool(db,options);db.open();
  std::vector<zyx::PropertyColumn> columns{{"id",{}}};for(int i=0;i<32;++i)columns[0].values.emplace_back("h"+std::to_string(i));
  auto tx=db.beginTransaction();auto ids=tx.createNodesColumnar("Hub",32,columns);tx.commit();
  if(ids.size()!=32)throw std::runtime_error("skew Node count");
  for(bool link:{true,false}){std::vector<int64_t> sources,targets;std::vector<zyx::PropertyColumn> props{{"weight",{}}};
   for(size_t i=0;i<512;++i)if((i%3==0)==link){const size_t source=i<256?0:(i*17)%32;const size_t target=i%17==0?source:(i*7+3)%32;
    sources.push_back(ids[source]);targets.push_back(ids[target]);props[0].values.emplace_back(int64_t(i%5));}
   auto edgeTx=db.beginTransaction();if(edgeTx.createEdgesColumnar(link?"LINK":"RARE",sources,targets,props).size()!=sources.size())throw std::runtime_error("skew Edge count");edgeTx.commit();}
  if(!db.execute("CREATE INDEX FOR (h:Hub) ON (h.id)").isSuccess())throw std::runtime_error("skew index");db.close();}
 std::cout<<"{\"timing_benchmark\":false,\"large_owners\":16,\"large_fields\":4,\"large_value_bytes\":8192,\"skew_nodes\":32,\"skew_edges\":512}\n";
}
#if R7_COHORT
static void prepareCohort(const std::filesystem::path &prefix) {
 using namespace graph::storage;
 const auto row=prefix.string()+"-row.zyx",native=prefix.string()+"-native.zyx";
 if(std::filesystem::exists(row)||std::filesystem::exists(native))throw std::runtime_error("cohort preparation needs fresh paths");
 RelocatableStorageBackendOptions options;options.data_path=row;options.open_mode=OpenMode::OPEN_CREATE_NEW_FILE;
 options.maximum_database_file_bytes=uint64_t(2)*1024*1024*1024;
 auto backend=RelocatableStorageBackend::initializeNew(options);backend->open();
 for(uint64_t group=0;group<8;++group){std::vector<LogicalGraphNodeCreateRequest> nodes;
  for(uint64_t slot=1;slot<=64;++slot){LogicalGraphNodeCreateRequest node;node.labels.push_back("Owner");
   for(uint64_t field=1;field<=64;++field)node.properties.push_back({"p"+std::to_string(field),graph::PropertyValue(int64_t(group*64+slot+field))});
   nodes.push_back(std::move(node));}
  if(backend->createLogicalNodes(nodes).size()!=64)throw std::runtime_error("cohort creation count");
 }
 backend->close();backend.reset();std::filesystem::copy_file(row,native);
 auto stream=std::make_shared<std::fstream>(native,std::ios::in|std::ios::out|std::ios::binary);
 auto io=std::make_shared<StorageIO>(stream,INVALID_FILE_HANDLE,INVALID_FILE_HANDLE);
 auto metadata=RelocatableMetadataStore::open(io);EntityPageStore entities(io,*metadata);SlottedPagePool pages(io,*metadata);
 BTreePageStore trees(pages,*metadata);EntityLabelSetStore labels(io,*metadata,pages);PropertyCollectionRootStore roots(pages,*metadata);
 NativeGraphWritePlannerOptions placement;placement.first_slot=PhysicalSlotId(1);placement.last_slot=PhysicalSlotId(16383);
 placement.entity_pool_owner_id=901;placement.property_roots={PhysicalSlotId(1),PhysicalSlotId(16383),903};
 NativeGraphWritePlanner writer(*metadata,entities,labels,roots,trees,placement);
 auto memory=std::make_shared<graph::query::execution::MaterializedByteBudget>(1024*1024);
 NativeHybridPropertyAllocationAdmission admission;admission.maximum_admitted_peak_bytes=32U*1024*1024;admission.maximum_admitted_file_bytes=uint64_t(2)*1024*1024*1024;
 auto old=metadata->openSnapshot();const auto before=NativeStorageAllocationCensus(io,*metadata,memory).inspect(old);
 auto tx=writer.beginTransaction();tx.stageHybridPropertyConversion(EntityPropertyOwnerKind::EPOK_NODE,1,memory,nullptr,true,
  NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID,true,admission,8);
 if(!tx.commit().committed())throw std::runtime_error("cohort transaction refused");
 auto now=metadata->openSnapshot();const auto after=NativeStorageAllocationCensus(io,*metadata,memory).inspect(now);
 if(!before||!after||!before->fullyAttributed()||!after->fullyAttributed()||!now.superblock().hybrid_property_descriptor_root.isValid())throw std::runtime_error("cohort gate not admitted");
 std::cout<<"{\"timing_benchmark\":false,\"row\":"<<before->steadyAllocatedBytes()<<",\"native\":"<<after->steadyAllocatedBytes()
  <<",\"peak\":"<<after->totalAllocatedBytes()<<",\"retired\":"<<after->retired_catalog_bytes<<",\"file\":"<<after->file_bytes<<"}\n";
}
#endif
int main(int argc,char **argv) {
 try {
  graph::debug::PerfTrace::setEnabled(false);graph::debug::PerfTrace::setValueCollectionEnabled(false);
#if R7_COHORT
  if(argc==3&&std::string_view(argv[1])=="cohort-prepare"){prepareCohort(argv[2]);return 0;}
#endif
  if(argc==3&&std::string_view(argv[1])=="row-fixtures-prepare"){prepareRowFixtures(argv[2]);return 0;}
  if(argc!=7)throw std::invalid_argument("probe read|write|check-read|check-write SOURCE PRIVATE CASE WARMUP ITERATIONS");
  const std::string mode=argv[1];std::string name=argv[4],query;
  std::vector<std::string> cases;std::stringstream caseNames(name);std::string part;
  while(std::getline(caseNames,part,',')){if(part.empty())throw std::invalid_argument("empty case");cases.push_back(part);}
  if(cases.empty())throw std::invalid_argument("empty cases");query=queryFor(cases.front());
  const bool writing=mode=="write"||mode=="check-write",timing=mode=="read"||mode=="write"||mode=="read-opened";
  if(!timing&&mode!="check-read"&&mode!="check-write")throw std::invalid_argument("unknown mode");
  if(writing&&cases.size()!=1)throw std::invalid_argument("writes require isolated case processes");
  const int warm=std::stoi(argv[5]),iterations=std::stoi(argv[6]);
  if(warm<0||iterations<1||iterations>100)throw std::invalid_argument("bounded positive sample count");
  const std::filesystem::path source=argv[2],target=argv[3];
  if(!std::filesystem::is_regular_file(source)||std::filesystem::exists(target))throw std::invalid_argument("fresh private path required");
  auto run=[&](zyx::Database &db,int iteration){
   const size_t batch=name=="edge_batch_100"?100:name=="edge_batch_1000"?1000:name=="edge_batch_10000"?10000:0;
   std::vector<int64_t> sources,targets;std::vector<zyx::PropertyColumn> columns;
   if(batch){const auto sourceId=scalarInt(db.execute("MATCH (u:User {id:'user-000001'}) RETURN id(u)"));
    const auto targetId=scalarInt(db.execute("MATCH (u:User {id:'user-000002'}) RETURN id(u)"));
    sources.assign(batch,sourceId);targets.assign(batch,targetId);columns.push_back({"weight",std::vector<zyx::Value>(batch,int64_t(1))});}
   graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setEnabled(iteration>=0);
   const auto start=timing?Clock::now():Clock::time_point{};
   ProbeRows rows;
   if(batch){auto tx=db.beginTransaction();const auto ids=tx.createEdgesColumnar("FOLLOWS",sources,targets,columns);tx.commit();
    if(ids.size()!=batch)throw std::runtime_error("columnar write count");rows.push_back({int64_t(ids.size())});}
   else if(name=="edge_create_delete"){
    auto created=capture(db.execute("MATCH (a:User {id:'user-000001'}),(b:User {id:'user-000002'}) CREATE (a)-[:FOLLOWS {weight:-9999}]->(b)"));
    rows=capture(db.execute("MATCH (:User {id:'user-000001'})-[r:FOLLOWS]->(:User {id:'user-000002'}) WHERE r.weight=-9999 DELETE r RETURN 1 AS value"));
   }else rows=capture(db.execute(query));
   const double ms=timing?std::chrono::duration<double,std::milli>(Clock::now()-start).count():0;
   graph::debug::PerfTrace::setEnabled(false);
   if(iteration>=0){auto stats=graph::debug::PerfTrace::snapshotAndReset();for(const auto &[key,e]:stats)
    std::cerr<<name<<"\t"<<key<<"\t"<<e.totalNs<<"\t"<<e.calls<<"\t"<<e.totalValue<<"\t"<<e.valueCalls<<"\n";}
   ProbeRows persisted;std::string verify;
   if(name=="edge_create"||name=="edge_create_delete")verify="MATCH (:User {id:'user-000001'})-[r:FOLLOWS]->(:User {id:'user-000002'}) WHERE r.weight=-9999 RETURN count(r)";
   else if(batch)verify="MATCH (:User {id:'user-000001'})-[r:FOLLOWS]->(:User {id:'user-000002'}) WHERE r.weight=1 RETURN count(r)";
   else if(name=="property_set"||name=="property_noop")verify="MATCH (u:User {id:'user-000006'}) RETURN u.score AS value";
   else if(name=="cohort_set"||name=="cohort_noop"||name=="cohort_remove")verify="MATCH (o:Owner) WHERE o.p1=2 RETURN o.p64 AS value";
   else if(name=="large_patch")verify="MATCH (o:Large) WHERE o.ordinal=-1 RETURN o.ordinal AS value";
   if(!verify.empty())persisted=capture(db.execute(verify));
   if(iteration>=0){std::cout<<"{\"timing_benchmark\":"<<(timing?"true":"false")<<",\"case\":\""<<name<<"\",\"iteration\":"<<iteration;
    if(timing)std::cout<<",\"latency_ms\":"<<std::setprecision(17)<<ms;
    std::cout<<",\"rows\":";emitRows(rows);if(!verify.empty()){std::cout<<",\"postcondition_rows\":";emitRows(persisted);}std::cout<<"}\n"<<std::flush;}
  };
  Options options;options.threads=1;
  if(mode=="read-opened") {
   std::filesystem::copy_file(source,target);
   for(const auto &selected:cases){name=selected;query=queryFor(name);
    for(int i=0;i<iterations;++i){zyx::Database db(target.string());configureThreadPool(db,options);
     const auto opened=Clock::now();db.open();const double openMs=std::chrono::duration<double,std::milli>(Clock::now()-opened).count();
     run(db,i);const auto closing=Clock::now();db.close();const double closeMs=std::chrono::duration<double,std::milli>(Clock::now()-closing).count();
     std::cout<<"{\"event\":\"handle_lifecycle\",\"case\":\""<<name<<"\",\"iteration\":"<<i<<",\"open_ms\":"<<openMs<<",\"close_ms\":"<<closeMs<<"}\n"<<std::flush;
    }
   }
   std::filesystem::remove(target);return 0;
  }
  if(name=="steady_property") {
   if(!writing)throw std::invalid_argument("steady requires write mode");
   std::filesystem::copy_file(source,target);zyx::Database db(target.string()),reader(target.string());
   configureThreadPool(db,options);configureThreadPool(reader,options);db.open();reader.open();
   auto pin=reader.beginReadOnlyTransaction();const auto oldRows=capture(pin.execute("MATCH (u:User {id:'user-000006'}) RETURN u.score AS value"));
   if(oldRows.size()!=1)throw std::runtime_error("steady anchor count");const auto oldScore=std::get<double>(oldRows[0][0]);
   const auto initial=std::filesystem::file_size(target);uintmax_t peak=initial,atDrain=initial;
   const int count=timing?40:4;const int releaseAt=count/2;
   for(int i=0;i<count;++i){const double expected=-1000.0-i;
    const std::string statement="MATCH (u:User {id:'user-000006'}) SET u.score="+std::to_string(expected)+" RETURN u.score AS value";
    const auto start=timing?Clock::now():Clock::time_point{};auto rows=capture(db.execute(statement));
    const double ms=timing?std::chrono::duration<double,std::milli>(Clock::now()-start).count():0;
    if(rows.size()!=1||std::get<double>(rows[0][0])!=expected)throw std::runtime_error("steady write result");
    if(i<releaseAt){const auto held=capture(pin.execute("MATCH (u:User {id:'user-000006'}) RETURN u.score AS value"));
     if(held.size()!=1||std::get<double>(held[0][0])!=oldScore)throw std::runtime_error("steady old snapshot changed");}
    if(i+1==releaseAt){pin.rollback();reader.close();atDrain=std::filesystem::file_size(target);}
    peak=std::max(peak,std::filesystem::file_size(target));
    if(i>=3||!timing){std::cout<<"{\"timing_benchmark\":"<<(timing?"true":"false")<<",\"case\":\"steady_property\",\"iteration\":"<<i;
     if(timing)std::cout<<",\"latency_ms\":"<<std::setprecision(17)<<ms;
     std::cout<<",\"old_pin_held\":"<<(i<releaseAt?"true":"false")<<",\"file_bytes\":"<<std::filesystem::file_size(target)<<",\"rows\":";emitRows(rows);std::cout<<"}\n"<<std::flush;}
   }
   db.close();const auto final=std::filesystem::file_size(target);
   std::cout<<"{\"event\":\"steady_space\",\"initial\":"<<initial<<",\"at_pin_drain\":"<<atDrain<<",\"peak\":"<<peak<<",\"final\":"<<final<<"}\n";
   std::filesystem::remove(target);return 0;
  }
  if(!writing){std::filesystem::copy_file(source,target);zyx::Database db(target.string());configureThreadPool(db,options);db.open();
   for(const auto &selected:cases){name=selected;query=queryFor(name);
    try {for(int i=0;i<warm+iterations;++i)run(db,i-warm);}
    catch(const std::exception &error){std::cout<<"{\"status\":\"failed\",\"case\":\""<<jsonEscape(name)<<"\",\"error\":\""<<jsonEscape(error.what())<<"\"}\n"<<std::flush;}
   }db.close();std::filesystem::remove(target);
  }else for(int i=0;i<warm+iterations;++i){
   std::filesystem::copy_file(source,target);{zyx::Database db(target.string());configureThreadPool(db,options);db.open();run(db,i-warm);db.close();}
   std::filesystem::remove(target);
  }
  return 0;
 }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
