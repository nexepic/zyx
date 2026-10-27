#include "../current-boundary/observer.hpp"
#include <array>
#include <sys/stat.h>
#include <zlib.h>
#include "graph/query/QueryGuard.hpp"
namespace call_cost {
struct Item { uint64_t total=0, inside=0, sampled=0, inside_sampled=0, ns=0, inside_ns=0; };
inline std::array<std::array<Item,3>,256> rows{};
inline void dump() noexcept {
 for(size_t i=0;i<rows.size();++i){bool any=false;for(auto x:rows[i])any|=x.total!=0;if(!any)continue;
  std::fprintf(stderr,"{\"call_ordinal\":%zu,\"items\":[",i);
  for(size_t j=0;j<3;++j){const auto x=rows[i][j];std::fprintf(stderr,"%s[%llu,%llu,%llu,%llu,%llu,%llu]",j?",":"",(unsigned long long)x.total,(unsigned long long)x.inside,(unsigned long long)x.sampled,(unsigned long long)x.inside_sampled,(unsigned long long)x.ns,(unsigned long long)x.inside_ns);}
  std::fprintf(stderr,"]}\n"); }
}
struct Scope {
 Item *item=nullptr; bool inside=false, selected=false; uint64_t start=0;
 explicit Scope(size_t id) noexcept {
  if(!boundary::enabled || cpu_sample::calls>=rows.size())return;
  static const bool once=[] {std::atexit(dump);return true;}();(void)once;
  item=&rows[cpu_sample::calls][id];inside=boundary::depth!=0;
  const auto n=item->total++;if(inside)++item->inside;
  selected=(n%32U)==(cpu_sample::calls%32U);
  if(selected){++item->sampled;if(inside)++item->inside_sampled;start=boundary::now();}
 }
 ~Scope(){if(!selected)return;const auto elapsed=boundary::now()-start;item->ns+=elapsed;if(inside)item->inside_ns+=elapsed;}
};
}
extern "C" int __real_fstat(int,struct stat *);
extern "C" int __wrap_fstat(int fd,struct stat *st){call_cost::Scope s(0);return __real_fstat(fd,st);}
extern "C" uLong __real_crc32(uLong,const Bytef *,uInt);
extern "C" uLong __wrap_crc32(uLong crc,const Bytef *bytes,uInt len){call_cost::Scope s(1);return __real_crc32(crc,bytes,len);}
extern "C" void real_guard(graph::query::QueryGuard *) asm("__real__ZN5graph5query10QueryGuard9checkSlowEv");
extern "C" void wrapped_guard(graph::query::QueryGuard *) asm("__wrap__ZN5graph5query10QueryGuard9checkSlowEv");
extern "C" void wrapped_guard(graph::query::QueryGuard *guard){call_cost::Scope s(2);real_guard(guard);}
