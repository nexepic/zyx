#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
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
 std::vector<std::byte> output;output.reserve(1400000U);
 tracking=true;
 for(int64_t i=0;i<100000;++i) graph::storage::CanonicalPropertyValueCodec::append(output,PropertyValue(i));
 tracking=false;
 if(output.size()!=1400000U)return 2;
 size_t offset=output.size()-14U;
 const auto last=graph::storage::CanonicalPropertyValueCodec::decodeNext(output,offset);
 if(last!=PropertyValue(int64_t{99999}) || offset!=output.size())return 3;
 std::printf("encoded=100000 output=%zu requests=%zu peak_temporary_request_bytes=%zu final=%zu\n",output.size(),calls,peak,live);
 return live!=0U;
}
