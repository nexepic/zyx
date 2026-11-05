#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <cstdint>
using namespace graph; using namespace graph::query::execution;
namespace { struct Header { void *base;size_t bytes;bool tracked;};bool tracking=false;size_t calls=0,live=0,peak=0;
void *allocate(size_t bytes,size_t alignment) {
 if(alignment<alignof(Header)) alignment=alignof(Header);
 if(bytes>SIZE_MAX-sizeof(Header)-alignment) throw std::bad_alloc();
 auto *base=std::malloc(bytes+sizeof(Header)+alignment);if(!base) throw std::bad_alloc();
 auto address=(reinterpret_cast<uintptr_t>(base)+sizeof(Header)+alignment-1U)&~(alignment-1U);
 auto *header=reinterpret_cast<Header *>(address)-1;*header={base,bytes,tracking};
 if(tracking){++calls;live+=bytes;peak=std::max(peak,live);}return reinterpret_cast<void *>(address);
}
void release(void *p) noexcept {if(!p)return;auto *h=static_cast<Header *>(p)-1;if(h->tracked)live-=h->bytes;std::free(h->base);}
}
void *operator new(size_t n){return allocate(n,alignof(std::max_align_t));}
void *operator new[](size_t n){return allocate(n,alignof(std::max_align_t));}
void *operator new(size_t n,std::align_val_t a){return allocate(n,static_cast<size_t>(a));}
void *operator new[](size_t n,std::align_val_t a){return allocate(n,static_cast<size_t>(a));}
void operator delete(void*p)noexcept{release(p);} void operator delete[](void*p)noexcept{release(p);}
void operator delete(void*p,size_t)noexcept{release(p);}void operator delete[](void*p,size_t)noexcept{release(p);}
void operator delete(void*p,std::align_val_t)noexcept{release(p);}void operator delete[](void*p,std::align_val_t)noexcept{release(p);}
void operator delete(void*p,size_t,std::align_val_t)noexcept{release(p);}void operator delete[](void*p,size_t,std::align_val_t)noexcept{release(p);}
int main(){
 uint64_t checksum=0;tracking=true;
 {std::vector<LogicalPlanReadRow> rows;rows.reserve(20000);
  for(int64_t i=0;i<20000;++i){LogicalPlanReadRow row;row.values.emplace("x",PropertyValue(i));rows.push_back(std::move(row));}
  for(const auto &r:rows)checksum+=std::get<int64_t>(std::get<PropertyValue>(r.values.at("x")).getVariant());
 }
 tracking=false;std::printf("rows=20000 row_size=%zu requests=%zu peak_request_bytes=%zu final=%zu checksum=%llu\n",sizeof(LogicalPlanReadRow),calls,peak,live,(unsigned long long)checksum);return live!=0U;
}
