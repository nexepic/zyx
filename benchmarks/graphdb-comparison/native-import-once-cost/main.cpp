// Reuse the exact CSV parser, columnar import and index construction of the comparison runner.
#define main comparisonRunnerMain
#include "apps/compare_benchmark/main.cpp"
#undef main
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif
static uint64_t peakBytes(){
#if defined(__APPLE__) || defined(__linux__)
 rusage usage{};if(getrusage(RUSAGE_SELF,&usage)!=0)throw std::runtime_error("getrusage failed");
#if defined(__APPLE__)
 return static_cast<uint64_t>(usage.ru_maxrss);
#else
 return static_cast<uint64_t>(usage.ru_maxrss)*1024U;
#endif
#else
 return 0U; // External measurement is required on other platforms.
#endif
}
static uint64_t residentBytes(){
#if defined(__APPLE__)
 mach_task_basic_info_data_t info{};mach_msg_type_number_t count=MACH_TASK_BASIC_INFO_COUNT;
 if(task_info(mach_task_self(),MACH_TASK_BASIC_INFO,reinterpret_cast<task_info_t>(&info),&count)!=KERN_SUCCESS)throw std::runtime_error("task_info failed");
 return info.resident_size;
#else
 return 0U; // This live-resident diagnostic is implemented for macOS only.
#endif
}
int main(int argc,char **argv){
 if(argc!=4&&argc!=5)throw std::invalid_argument("usage: import-once DATASET PRIVATE_DB EXPECTED_ROWS [ROUNDS]");
 const size_t rounds=argc==5?std::stoull(argv[4]):1U;
 if(!rounds||rounds>4)throw std::invalid_argument("bounded import rounds required");
 Options options;options.dataset=argv[1];options.dbPath=argv[2];options.threads=1U;
 if(std::filesystem::exists(options.dbPath))throw std::invalid_argument("private fresh database required");
 for(size_t round=0;round<rounds;++round){
  const auto report=[round](const char *phase){std::cout<<"{\"event\":\"peak\",\"round\":"<<round<<",\"phase\":\""<<phase<<"\",\"bytes\":"<<peakBytes()<<",\"resident_bytes\":"<<residentBytes()<<"}\n"<<std::flush;};
  {
   ScopedArtifactCleanup cleanup{options.dbPath};
   const auto start=Clock::now();zyx::Database db(options.dbPath.string());configureThreadPool(db,options);db.open();report("opened");
   const LoadedGraph loaded=loadGraph(db,options.dataset);report("graph_committed");
   createBenchmarkIndexes(db,options);report("indexed");db.close();report("closed");
   if(loaded.loadedRows!=std::stoll(argv[3]))throw std::runtime_error("import row count mismatch");
   std::cout<<"{\"event\":\"sample\",\"round\":"<<round<<",\"milliseconds\":"<<std::chrono::duration<double,std::milli>(Clock::now()-start).count()<<",\"loaded_rows\":"<<loaded.loadedRows<<",\"file_bytes\":"<<std::filesystem::file_size(options.dbPath)<<"}\n";
  }
  report("released");
 }
}
