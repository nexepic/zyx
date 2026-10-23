#define main comparisonRunnerMain
#include "apps/compare_benchmark/main.cpp"
#undef main
int main(int argc,char **argv){
 try {
  if(argc!=4)throw std::invalid_argument("import DATASET PRIVATE_DB EXPECTED_ROWS");
  Options options;options.dataset=argv[1];options.dbPath=argv[2];options.threads=1;options.loadBatchRows=1000;
  if(std::filesystem::exists(options.dbPath))throw std::invalid_argument("fresh import path required");
  graph::debug::PerfTrace::setEnabled(false);graph::debug::PerfTrace::setValueCollectionEnabled(false);
  const auto start=Clock::now();const auto loaded=loadDatabase(options,options.dbPath);
  const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
  if(loaded.loadedRows!=std::stoll(argv[3]))throw std::runtime_error("import row count");
  const auto fileBytes=std::filesystem::file_size(options.dbPath);
  zyx::Database db(options.dbPath.string());configureThreadPool(db,options);db.open();
  const auto nodes=scalarInt(db.execute("MATCH (n) RETURN count(n)"));
  const auto edges=scalarInt(db.execute("MATCH ()-[r]->() WHERE r.weight>=0 RETURN count(r)"));db.close();
  std::cout<<"{\"event\":\"sample\",\"case\":\"fresh_import\",\"latency_ms\":"<<std::setprecision(17)<<ms
   <<",\"loaded_rows\":"<<loaded.loadedRows<<",\"nodes\":"<<nodes<<",\"edges\":"<<edges<<",\"file_bytes\":"<<fileBytes<<"}\n";
  return 0;
 }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
