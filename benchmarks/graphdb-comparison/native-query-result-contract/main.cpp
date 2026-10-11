// Independent public-API result verification; not used in normal benchmark timing.
#define main comparisonRunnerMain
#include "apps/compare_benchmark/main.cpp"
#undef main

void emitScalar(const zyx::Value &value) {
 if (std::holds_alternative<std::monostate>(value)) std::cout<<"[\"null\",null]";
 else if (const auto *v=std::get_if<std::string>(&value)) std::cout<<"[\"string\",\""<<jsonEscape(*v)<<"\"]";
 else if (const auto *v=std::get_if<int64_t>(&value)) std::cout<<"[\"integer\","<<*v<<"]";
 else if (const auto *v=std::get_if<double>(&value)) std::cout<<"[\"double\","<<std::setprecision(17)<<*v<<"]";
 else if (const auto *v=std::get_if<bool>(&value)) std::cout<<"[\"boolean\","<<(*v?"true":"false")<<"]";
 else if (const auto *v=std::get_if<std::shared_ptr<zyx::Node>>(&value)) {
  if(!*v)throw std::runtime_error("null node pointer");
  std::cout<<"[\"node\",{\"internal_id\":"<<(*v)->id<<",\"label\":\""<<jsonEscape((*v)->label)<<"\",\"labels\":[";
  for(size_t i=0;i<(*v)->labels.size();++i){if(i)std::cout<<",";std::cout<<"\""<<jsonEscape((*v)->labels[i])<<"\"";}
  std::cout<<"],\"properties\":{";bool first=true;
  for(const auto &[key,entry]:(*v)->properties){if(!first)std::cout<<",";first=false;std::cout<<"\""<<jsonEscape(key)<<"\":";emitScalar(entry);}
  std::cout<<"}}]";
 } else if (const auto *v=std::get_if<std::shared_ptr<zyx::Edge>>(&value)) {
  if(!*v)throw std::runtime_error("null edge pointer");
  std::cout<<"[\"edge\",{\"internal_id\":"<<(*v)->id<<",\"source_id\":"<<(*v)->sourceId<<",\"target_id\":"<<(*v)->targetId<<",\"type\":\""<<jsonEscape((*v)->type)<<"\",\"properties\":{";bool first=true;
  for(const auto &[key,entry]:(*v)->properties){if(!first)std::cout<<",";first=false;std::cout<<"\""<<jsonEscape(key)<<"\":";emitScalar(entry);}
  std::cout<<"}}]";
 } else throw std::invalid_argument("Result contract probe accepts scalar,node and edge columns only");
}
int main(int argc,char **argv) {
 try {
  if(argc<4)throw std::invalid_argument("usage: result-contract prepare DATASET PRIVATE_DB | query PRIVATE_DB CYPHER");
  Options options;options.threads=1U;options.profile="scan";
  if(std::string_view(argv[1])=="prepare") {
   if(argc!=4)throw std::invalid_argument("prepare requires dataset and private database");
   options.dataset=argv[2];options.dbPath=argv[3];
   if(std::filesystem::exists(options.dbPath))throw std::invalid_argument("prepare requires a fresh private database");
   const auto loaded=loadDatabase(options,options.dbPath);
   std::cout<<"{\"prepared_rows\":"<<loaded.loadedRows<<"}\n";
  } else if((std::string_view(argv[1])=="query" || std::string_view(argv[1])=="query-counters")) {
   if(argc!=4)throw std::invalid_argument("query requires database and Cypher");
   if(!std::filesystem::exists(argv[2]))throw std::invalid_argument("query requires an existing prepared database");
   zyx::Database db(argv[2]);configureThreadPool(db,options);db.open();
   const bool counters=std::string_view(argv[1])=="query-counters";
   if(counters){graph::debug::PerfTrace::setEnabled(false);graph::debug::PerfTrace::setValueCollectionEnabled(true);graph::debug::PerfTrace::reset();}
   auto result=db.execute(argv[3]);if(!result.isSuccess())throw std::runtime_error(result.getError());
   std::cout<<"[";bool first=true;
   while(result.hasNext()) {result.next();if(!first)std::cout<<",";first=false;std::cout<<"[";
    for(int col=0;col<result.getColumnCount();++col){if(col)std::cout<<",";emitScalar(result.get(col));}
    std::cout<<"]";
   }
   std::cout<<"]\n";
   const auto emitCounters=[&](const char *phase) {
    if(!counters)return;
    for(const auto &[name,entry]:graph::debug::PerfTrace::snapshotAndReset())
     std::cerr<<"{\"phase\":\""<<phase<<"\",\"counter\":\""<<jsonEscape(name)<<"\",\"calls\":"<<entry.calls
              <<",\"value_total\":"<<entry.totalValue<<",\"value_calls\":"<<entry.valueCalls<<"}\n";
   };
   emitCounters("operation");db.close();emitCounters("close");
   if(counters)graph::debug::PerfTrace::setValueCollectionEnabled(false);
  } else throw std::invalid_argument("unknown probe mode");
  return 0;
 } catch(const std::exception &error){std::cerr<<error.what()<<"\n";return 1;}
}
