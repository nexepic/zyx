#include <iostream>
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "CypherParserImpl.hpp"
int main(int argc,char **argv) {
 try {
  using namespace graph::storage;
  if(argc!=2)return 2;
  RelocatableStorageBackendOptions options; options.data_path=argv[1];
  options.open_mode=OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
  options.maximum_database_file_bytes=uint64_t(2)*1024*1024*1024;
  auto backend=std::make_shared<RelocatableStorageBackend>(options);backend->open();
  graph::query::QueryEngine engine(backend);graph::parser::cypher::CypherParserImpl parser;
  const std::vector<std::string> queries{
   "MATCH (a:User {id:'user-000001'})-[:FOLLOWS]->(b:User), (c:Tag {id:'tag-000001'}) RETURN count(*) AS count",
   "MATCH (a:User {id:'user-000001'})-[:FOLLOWS]->(b:User), (c:Tag {id:'tag-000001'}) RETURN id(b) AS target,id(c) AS tag ORDER BY target,tag",
   "MATCH (a:User {id:'user-000001'})-[:FOLLOWS]->(b:User)-[:FOLLOWS]->(d:User) RETURN id(d) AS target ORDER BY target",
   "MATCH (a:User {id:'user-000001'})-[:FOLLOWS]->(b:User) WITH id(b) AS target LIMIT 5 RETURN target",
   "UNWIND ['user-000003','not-found','user-000001','user-000001',NULL] AS probe OPTIONAL MATCH (u:User {id:probe}) RETURN probe,u.age AS age",
   "MATCH (a:User {id:'user-000001'})-[:FOLLOWS]->(b:User) WHERE b.absent_property IS NULL RETURN count(*) AS count"
  };
  std::cout<<"{\"timing_benchmark\":false,\"physical_read_only\":true,\"cases\":[";
  bool first=true;
  for(size_t i=0;i<queries.size();++i){
   const auto &query=queries[i];
   const auto raw=engine.executePlan(parser.parseToLogical(query),{});
   const auto optimized=engine.execute(query);
   if(raw.getColumns()!=optimized.getColumns() || raw.rowCount()!=optimized.rowCount())
    throw std::runtime_error("fullscale raw/optimized shape mismatch case "+std::to_string(i));
   for(size_t row=0;row<raw.rowCount();++row)for(const auto &column:raw.getColumns())
    if(raw.getRows()[row].at(column).asPrimitive()!=optimized.getRows()[row].at(column).asPrimitive())
     throw std::runtime_error("fullscale raw/optimized value mismatch case "+std::to_string(i));
   if(!first)std::cout<<",";first=false;
   std::cout<<"{\"case\":"<<i<<",\"passed\":true,\"rows\":"<<raw.rowCount()<<"}"<<std::flush;
  }
  std::cout<<"]}\n";backend->close();return 0;
 }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
