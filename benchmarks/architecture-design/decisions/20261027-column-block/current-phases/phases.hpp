#pragma once
#include "../current-boundary/observer.hpp"
#include <array>
namespace phases {
struct Row { std::array<uint64_t,8> ns{},counts{}; };
inline std::array<Row,256> rows{};
inline thread_local bool active=false;
inline thread_local unsigned current=0;
inline thread_local uint64_t start=0;
inline void add(uint64_t end) noexcept { rows[cpu_sample::calls].ns[current]+=end-start; }
struct Scope {
 bool enabled=boundary::enabled && active; unsigned previous=0;
 explicit Scope(unsigned id) noexcept {
  if(!enabled)return;const auto t=boundary::now();add(t);previous=current;current=id;start=t;
  ++rows[cpu_sample::calls].counts[id];
 }
 ~Scope(){if(!enabled)return;const auto t=boundary::now();add(t);current=previous;start=t;}
};
inline void dump() noexcept {
 for(size_t i=0;i<rows.size();++i) if(rows[i].counts[0]) {
  std::fprintf(stderr,"{\"phase_ordinal\":%zu,\"ns\":[",i);
  for(size_t j=0;j<8;++j)std::fprintf(stderr,"%s%llu",j?",":"",(unsigned long long)rows[i].ns[j]);
  std::fprintf(stderr,"],\"counts\":[");
  for(size_t j=0;j<8;++j)std::fprintf(stderr,"%s%llu",j?",":"",(unsigned long long)rows[i].counts[j]);
  std::fprintf(stderr,"]}\n");
 }
}
struct Root {
 bool enabled=false;
 Root() noexcept {
 if(!boundary::enabled || active)return;
 static unsigned last=0,sequence=0,offset=0;
 if(last!=cpu_sample::calls){last=cpu_sample::calls;offset=sequence++%32U;}
 if(boundary::calls%32U!=offset)return;
 enabled=true;active=true;current=0;start=boundary::now();++rows[cpu_sample::calls].counts[0];static const bool once=[] {std::atexit(dump);return true;}();(void)once;}
 ~Root(){if(!enabled)return;add(boundary::now());active=false;}
};
}
